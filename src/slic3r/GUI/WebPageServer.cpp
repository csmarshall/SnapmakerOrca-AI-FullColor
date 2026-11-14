#include "WebPageServer.hpp"
#include "PreprintCapabilityStore.hpp"
#include <cstring>
#include <regex>
#include <sstream>
#include <fstream>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace asio  = boost::asio;
namespace beast = boost::beast;
namespace http  = boost::beast::http;
namespace fs    = boost::filesystem;

namespace Slic3r { namespace GUI {

namespace {

constexpr std::size_t k_header_limit    = 16 * 1024;
constexpr std::size_t k_max_connections = 64;
// Re-armed per phase. A client that has not sent a complete request header in 5 s is dropped, so idle
// unauthenticated sockets cannot hold the connection cap for long; a file body gets longer.
constexpr auto        k_header_timeout   = std::chrono::seconds(5);
constexpr auto        k_status_timeout   = std::chrono::seconds(10);
constexpr auto        k_file_timeout     = std::chrono::seconds(120);
constexpr auto        k_accept_retry     = std::chrono::milliseconds(100);

std::string make_token()
{
    std::random_device rd;
    static const char  hex[] = "0123456789abcdef";
    std::string        out;
    for (int i = 0; i < 32; ++i) { // 32 bytes = 256 bits
        const unsigned b = static_cast<unsigned>(rd()) & 0xffu;
        out += hex[b >> 4];
        out += hex[b & 15];
    }
    return out;
}

bool constant_time_equal(const std::string& a, const std::string& b)
{
    unsigned diff = a.size() == b.size() ? 0 : 1;
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i)
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decodes %XX once. Returns false on a malformed escape.
bool percent_decode(const std::string& in, std::string& out)
{
    out.clear();
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%') {
            if (i + 2 >= in.size()) return false;
            const int h = hex_val(in[i + 1]), l = hex_val(in[i + 2]);
            if (h < 0 || l < 0) return false;
            out += static_cast<char>(h * 16 + l);
            i += 2;
        } else {
            out += in[i];
        }
    }
    return true;
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

const char* mime_for(const fs::path& p)
{
    const std::string ext = lower(p.extension().string());
    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".js" || ext == ".mjs")   return "text/javascript; charset=utf-8";
    if (ext == ".wasm")                  return "application/wasm";
    if (ext == ".json" || ext == ".map") return "application/json; charset=utf-8";
    if (ext == ".css")                   return "text/css; charset=utf-8";
    if (ext == ".png")                   return "image/png";
    if (ext == ".webp")                  return "image/webp";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".svg")                   return "image/svg+xml";
    if (ext == ".ico")                   return "image/x-icon";
    if (ext == ".ttf")                   return "font/ttf";
    if (ext == ".otf")                   return "font/otf";
    if (ext == ".woff")                  return "font/woff";
    if (ext == ".woff2")                 return "font/woff2";
    if (ext == ".txt")                   return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

// Extracts the value of `key` from a raw query string; returns "" if absent.
std::string query_value(const std::string& query, const std::string& key)
{
    std::size_t pos = 0;
    while (pos <= query.size()) {
        std::size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        const std::string pair = query.substr(pos, amp - pos);
        const std::size_t eq   = pair.find('=');
        if (eq != std::string::npos && pair.compare(0, eq, key) == 0)
            return pair.substr(eq + 1);
        pos = amp + 1;
    }
    return {};
}

// True if ANY cookie called `name` equals the token (a planted duplicate earlier in the header must not win).
bool cookie_matches(const std::string& header, const std::string& name, const std::string& token)
{
    std::size_t pos = 0;
    while (pos < header.size()) {
        std::size_t semi = header.find(';', pos);
        if (semi == std::string::npos) semi = header.size();
        std::string item = header.substr(pos, semi - pos);
        const std::size_t first = item.find_first_not_of(" \t");
        if (first != std::string::npos) {
            item = item.substr(first);
            const std::size_t eq = item.find('=');
            if (eq != std::string::npos && item.compare(0, eq, name) == 0 && constant_time_equal(item.substr(eq + 1), token))
                return true;
        }
        pos = semi + 1;
    }
    return false;
}

// The query string without the token parameter ("" if nothing else is left).
std::string query_without(const std::string& query, const std::string& key)
{
    std::string out;
    std::size_t pos = 0;
    while (pos <= query.size()) {
        std::size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        const std::string pair = query.substr(pos, amp - pos);
        const std::size_t eq   = pair.find('=');
        const bool        drop = pair.compare(0, eq == std::string::npos ? pair.size() : eq, key) == 0;
        if (!pair.empty() && !drop) out += (out.empty() ? "" : "&") + pair;
        pos = amp + 1;
    }
    return out;
}

} // namespace

namespace web_page_server_detail {

bool segments_to_relative(const std::string& rest, fs::path& rel)
{
    rel.clear();
    std::size_t pos = 0;
    while (pos < rest.size()) {
        std::size_t slash = rest.find('/', pos);
        if (slash == std::string::npos) slash = rest.size();
        const std::string raw_seg = rest.substr(pos, slash - pos);
        std::string       seg;
        if (raw_seg.empty() || !percent_decode(raw_seg, seg) || seg.empty() || seg == "." || seg == ".." ||
            seg.find('/') != std::string::npos || seg.find('\\') != std::string::npos ||
            seg.find('\0') != std::string::npos || seg.find(':') != std::string::npos)
            return false;
        rel /= seg;
        pos = slash + 1;
    }
    if (rest.empty() || rest.back() == '/') rel /= "index.html"; // directory-style URL -> its index page only
    return true;
}

int resolve_within(const fs::path& canonical_root, const fs::path& rel, fs::path& resolved)
{
    boost::system::error_code ec;
    resolved = fs::canonical(canonical_root / rel, ec);
    if (ec) return 404;
    const std::string root_str = canonical_root.generic_string();
    const std::string res_str  = resolved.generic_string();
    if (res_str.size() <= root_str.size() || res_str.compare(0, root_str.size(), root_str) != 0 || res_str[root_str.size()] != '/')
        return 403;
    if (!fs::is_regular_file(resolved, ec)) return 404; // directories: no listing
    return 200;
}

} // namespace web_page_server_detail

struct WebPageServer::Impl : std::enable_shared_from_this<WebPageServer::Impl>
{
    fs::path                root;           // as given
    fs::path                csp_file;
    std::string             csp;            // the policy sent on every response; set by start()
    std::string             extra_origin;   // validated by set_extra_origin(); added to csp by start()
    fs::path                canonical_root; // set by start()
    std::shared_ptr<PreprintCapabilityStore> caps; // opt-in; set before start()
    std::string             token = make_token();
    std::string             expected_host;
    std::string             expected_origin;
    std::string             cookie_name;
    uint16_t                port = 0;
    asio::io_context        io{1}; // one thread: every handler is serialised, so stop() may touch connections
    asio::ip::tcp::acceptor acceptor{io};
    asio::steady_timer      accept_retry{io};
    std::vector<std::thread> threads;
    std::atomic<bool>       running{false};
    std::atomic<std::size_t> connections{0};
    class Connection;
    std::set<Connection*>   open_connections; // touched on the io thread only

    using Response = http::response<http::file_body>;

    // The result of routing one request: a file to send, a redirect, or a bare status.
    struct Outcome
    {
        http::status status = http::status::forbidden;
        fs::path     file;
        std::string  location; // for 302
        bool         set_cookie = false;
        bool         capability = false; // file is a capability snapshot: send as octet-stream, then finish_serve(cap_key)
        std::string  cap_key;
        std::uint64_t cap_size = 0;
    };

    Outcome route(http::verb method, const std::string& target, const std::string& host, const std::string& origin, bool has_origin,
                  const std::string& cookie_header, bool has_fetch_site, const std::string& fetch_site, bool has_range = false) const
    {
        Outcome out;
        // DNS rebinding and cross-site requests: before anything else.
        if (host != expected_host) return out;
        if (has_origin && origin != expected_origin) return out;
        // Browsers say where a request came from; cross-site and same-site (another port on this host) are refused.
        if (has_fetch_site && fetch_site != "same-origin" && fetch_site != "none") return out;

        const std::size_t qpos      = target.find('?');
        const std::string raw_path  = target.substr(0, qpos);
        const std::string raw_query = qpos == std::string::npos ? std::string() : target.substr(qpos + 1);

        const std::string q_token = query_value(raw_query, token_param);
        const bool token_from_query  = !q_token.empty() && constant_time_equal(q_token, token);
        const bool token_from_cookie = cookie_matches(cookie_header, cookie_name, token);
        if (!token_from_query && !token_from_cookie) return out; // 403

        if (caps && raw_path.compare(0, std::strlen(PreprintCapabilityStore::path_prefix), PreprintCapabilityStore::path_prefix) == 0) {
            if (!token_from_cookie) return out;   // the cookie is required here; the query token only bootstraps the bundle page
            if (method != http::verb::get) { out.status = http::status::method_not_allowed; return out; } // no HEAD either
            if (has_range) { out.status = http::status::range_not_satisfiable; return out; }
            const auto serve = caps->begin_serve(raw_path); // exact match on the whole path
            switch (serve.status) {
            case PreprintCapabilityStore::Status::Ok:
                out.status = http::status::ok; out.file = serve.snapshot; out.capability = true; out.cap_key = raw_path; out.cap_size = serve.size;
                break;
            case PreprintCapabilityStore::Status::Gone:    out.status = http::status::gone; break;
            case PreprintCapabilityStore::Status::Corrupt: out.status = http::status::internal_server_error; break;
            default:                                       out.status = http::status::not_found; break;
            }
            return out;
        }

        if (method != http::verb::get && method != http::verb::head) {
            out.status = http::status::method_not_allowed;
            return out;
        }

        // Only origin-form targets ("/...") are routed; absolute-form ("http://host/...") fails the prefix test.
        const std::string prefix = url_prefix;
        if (raw_path.compare(0, prefix.size(), prefix) != 0) {
            out.status = http::status::not_found;
            return out;
        }
        fs::path rel;
        if (!web_page_server_detail::segments_to_relative(raw_path.substr(prefix.size()), rel)) {
            out.status = http::status::forbidden;
            return out;
        }
        fs::path resolved;
        switch (web_page_server_detail::resolve_within(canonical_root, rel, resolved)) {
        case 200: break;
        case 403: out.status = http::status::forbidden; return out;
        default:  out.status = http::status::not_found; return out;
        }
        out.file = resolved;
        if (token_from_query) {
            // Hand the token over to the cookie and take it out of the URL (document.location, $current_url, history).
            const std::string rest_q = query_without(raw_query, token_param);
            out.status     = http::status::found;
            out.location   = raw_path + (rest_q.empty() ? "" : "?" + rest_q);
            out.set_cookie = true;
            return out;
        }
        out.status = http::status::ok;
        return out;
    }

    class Connection : public std::enable_shared_from_this<Connection>
    {
    public:
        Connection(std::shared_ptr<Impl> impl, asio::ip::tcp::socket sock) : impl(std::move(impl)), stream(std::move(sock))
        {
            this->impl->open_connections.insert(this);
        }
        ~Connection()
        {
            impl->open_connections.erase(this);
            impl->connections--;
        }
        void close() { beast::error_code ec; stream.close(); }

        void run()
        {
            stream.expires_after(k_header_timeout);
            parser.emplace();
            parser->header_limit(k_header_limit);
            parser->body_limit(0);
            http::async_read(stream, buffer, *parser, [self = shared_from_this()](beast::error_code ec, std::size_t) {
                try {
                    if (ec) {
                        if (ec != http::error::end_of_stream && ec != asio::error::operation_aborted && ec != beast::error::timeout)
                            self->send_status(http::status::bad_request);
                        return;
                    }
                    self->handle();
                } catch (const std::exception& e) {
                    BOOST_LOG_TRIVIAL(error) << "WebPageServer: handler failed: " << e.what();
                    self->send_status_noexcept(http::status::internal_server_error);
                } catch (...) {
                    BOOST_LOG_TRIVIAL(error) << "WebPageServer: handler failed with an unknown exception";
                    self->send_status_noexcept(http::status::internal_server_error);
                }
            });
        }

    private:
        std::shared_ptr<Impl>                         impl;
        beast::tcp_stream                             stream;
        beast::flat_buffer                            buffer;
        boost::optional<http::request_parser<http::empty_body>> parser;
        std::shared_ptr<void>                         keep;

        template<class Msg> void common_headers(Msg& res)
        {
            res.set(http::field::server, "orca-web-page-server");
            res.set(http::field::cache_control, "no-store");
            res.set("Referrer-Policy", "no-referrer");
            res.set("X-Content-Type-Options", "nosniff");
            res.set("Content-Security-Policy", impl->csp); // every response shape, not only HTML
            res.keep_alive(false);
            // No Access-Control-* header is ever added.
        }

        void finish_write(std::shared_ptr<void> msg, std::chrono::seconds timeout)
        {
            keep = std::move(msg);
            stream.expires_after(timeout);
        }
        void shutdown()
        {
            beast::error_code e;
            stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, e);
        }

        void send_status_noexcept(http::status st)
        {
            try { send_status(st); } catch (...) { close(); }
        }

        void send_status(http::status st, const std::string& location = {}, const std::string& set_cookie = {})
        {
            auto res = std::make_shared<http::response<http::string_body>>(st, 11);
            common_headers(*res);
            res->set(http::field::content_type, "text/plain; charset=utf-8");
            if (st == http::status::method_not_allowed) res->set(http::field::allow, "GET, HEAD");
            if (!location.empty()) res->set(http::field::location, location);
            if (!set_cookie.empty()) res->insert(http::field::set_cookie, set_cookie);
            res->body() = std::string(http::obsolete_reason(st)) + "\n";
            res->prepare_payload();
            finish_write(res, k_status_timeout);
            http::async_write(stream, *res, [self = shared_from_this()](beast::error_code, std::size_t) { self->shutdown(); });
        }

        void handle()
        {
            const auto&       req    = parser->get();
            const std::string target = std::string(req.target());
            const auto        host   = std::string(req[http::field::host]);
            const bool        has_origin = req.find(http::field::origin) != req.end();
            const auto        origin = std::string(req[http::field::origin]);
            const bool        has_fetch_site = req.find("Sec-Fetch-Site") != req.end();
            const auto        fetch_site = std::string(req["Sec-Fetch-Site"]);
            // Every Cookie header counts, joined the way a single header would be.
            std::string cookie;
            const auto  range = req.equal_range(http::field::cookie);
            for (auto it = range.first; it != range.second; ++it) cookie += std::string(it->value()) + "; ";

            const bool    has_range = req.find(http::field::range) != req.end();
            const Outcome o = impl->route(req.method(), target, host, origin, has_origin, cookie, has_fetch_site, fetch_site, has_range);
            // Diagnostics for "the page does not load": what the webview asked for and what we answered. The query (token) is never logged.
            BOOST_LOG_TRIVIAL(trace) << "WebPageServer: " << std::string(req.method_string()) << " " << target.substr(0, target.find('?'))
                                    << " host=" << host << " origin=" << (has_origin ? origin : std::string("-"))
                                    << " fetch-site=" << (has_fetch_site ? fetch_site : std::string("-"))
                                    << " cookie=" << (cookie.empty() ? "no" : "yes") << " -> " << static_cast<int>(o.status);
            if (o.status == http::status::found) {
                send_status(o.status, o.location, o.set_cookie ? set_cookie_header() : std::string());
                return;
            }
            if (o.status != http::status::ok) {
                send_status(o.status);
                return;
            }
            if (o.capability) {
                send_capability(o);
                return;
            }

            beast::error_code ec;
            http::file_body::value_type body;
            // NOTE: file_body opens by narrow path. On Windows that is the ANSI code page, so an install
            // path with characters outside it fails (404 here). Not fixed: needs a wide-path File type that
            // cannot be tested on this host.
            body.open(o.file.string().c_str(), beast::file_mode::scan, ec);
            if (ec) {
                send_status(http::status::not_found);
                return;
            }
            const auto size = body.size();
            if (req.method() == http::verb::head) {
                auto res = std::make_shared<http::response<http::empty_body>>(http::status::ok, 11);
                common_headers(*res);
                res->set(http::field::content_type, mime_for(o.file));
                res->content_length(size);
                finish_write(res, k_status_timeout);
                http::async_write(stream, *res, [self = shared_from_this()](beast::error_code, std::size_t) { self->shutdown(); });
                return;
            }
            auto res = std::make_shared<Response>(http::status::ok, 11);
            common_headers(*res);
            res->set(http::field::content_type, mime_for(o.file));
            res->body() = std::move(body);
            res->prepare_payload();
            finish_write(res, k_file_timeout);
            http::async_write(stream, *res, [self = shared_from_this()](beast::error_code, std::size_t) { self->shutdown(); });
        }

        // One-shot snapshot download. The capability is consumed only when the whole body was written without error.
        void send_capability(const Outcome& o)
        {
            beast::error_code ec;
            http::file_body::value_type body;
            body.open(o.file.string().c_str(), beast::file_mode::scan, ec);
            if (ec || body.size() != o.cap_size) {
                body.close();
                impl->caps->finish_serve(o.cap_key, false);
                send_status(http::status::internal_server_error);
                return;
            }
            auto res = std::make_shared<Response>(http::status::ok, 11);
            common_headers(*res); // no-store, nosniff, no CORS
            res->set(http::field::content_type, "application/octet-stream");
            res->body() = std::move(body);
            res->prepare_payload(); // fixed Content-Length = snapshot size
            finish_write(res, k_file_timeout);
            http::async_write(stream, *res, [self = shared_from_this(), key = o.cap_key](beast::error_code wec, std::size_t) {
                self->impl->caps->finish_serve(key, !wec);
                self->shutdown();
            });
        }

        std::string set_cookie_header() const
        {
            return impl->cookie_name + "=" + impl->token + "; Path=" + url_prefix + "; HttpOnly; SameSite=Strict";
        }
    };

    void do_accept()
    {
        acceptor.async_accept([self = shared_from_this()](beast::error_code ec, asio::ip::tcp::socket sock) {
            if (ec == asio::error::operation_aborted || !self->running) return; // closed by stop()
            if (ec) {
                // Transient (EMFILE, ECONNABORTED, ...): keep serving, after a short pause so a persistent error cannot spin.
                BOOST_LOG_TRIVIAL(warning) << "WebPageServer: accept failed: " << ec.message();
                self->accept_retry.expires_after(k_accept_retry);
                self->accept_retry.async_wait([self](beast::error_code wec) {
                    if (!wec && self->running) self->do_accept();
                });
                return;
            }
            try {
                if (self->connections.load() >= k_max_connections) {
                    beast::error_code e;
                    sock.close(e);
                } else {
                    self->connections++;
                    std::make_shared<Connection>(self, std::move(sock))->run();
                }
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(error) << "WebPageServer: connection setup failed: " << e.what();
            }
            self->do_accept();
        });
    }

    // Runs the io loop; an exception escaping a handler is logged and the loop resumes, never terminating the app.
    void run_io()
    {
        for (;;) {
            try {
                io.run();
                return;
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(error) << "WebPageServer: io loop exception: " << e.what();
            } catch (...) {
                BOOST_LOG_TRIVIAL(error) << "WebPageServer: io loop unknown exception";
            }
        }
    }
};

namespace web_page_server_detail {

std::string parse_csp_file(const std::string& text)
{
    std::string out;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#') continue;
        out += (out.empty() ? "" : " ") + line.substr(first);
    }
    if (out.empty() || out.size() > 8192) return {};
    for (unsigned char c : out)
        if (c < 0x20 || c == 0x7f) return {};
    // 'unsafe-hashes' and 'wasm-unsafe-eval' are allowed (hashed handlers; WebAssembly only); 'unsafe-inline' and 'unsafe-eval' are not.
    if (out.find("'unsafe-inline'") != std::string::npos || out.find("'unsafe-eval'") != std::string::npos) return {};
    return out;
}

std::string csp_with_extra_origin(const std::string& policy, const std::string& origin)
{
    static const std::regex ok(R"(^https?://([A-Za-z0-9.-]+|\[[0-9A-Fa-f:]+\])(:[0-9]{1,5})?$)");
    if (policy.empty() || !std::regex_match(origin, ok)) return {};
    // Split into directives on ';' and append to img-src and connect-src only (the page fetches a JPEG; it never plays media).
    std::vector<std::string> dirs;
    std::size_t pos = 0;
    while (pos < policy.size()) {
        std::size_t semi = policy.find(';', pos);
        if (semi == std::string::npos) semi = policy.size();
        std::string d = policy.substr(pos, semi - pos);
        const auto f = d.find_first_not_of(' ');
        if (f != std::string::npos) dirs.push_back(d.substr(f));
        pos = semi + 1;
    }
    for (auto& d : dirs) {
        for (const char* name : {"connect-src", "img-src"})
            if (d.compare(0, std::strlen(name), name) == 0 && (d.size() == std::strlen(name) || d[std::strlen(name)] == ' '))
                d += " " + origin;
    }
    std::string out;
    for (const auto& d : dirs) out += (out.empty() ? "" : "; ") + d;
    return out;
}

} // namespace web_page_server_detail

WebPageServer::WebPageServer(fs::path root, fs::path csp_file) : m_impl(std::make_shared<Impl>())
{
    m_impl->csp_file = csp_file.empty() ? root.parent_path() / "flutter_web.csp" : std::move(csp_file);
    m_impl->root     = std::move(root);
}

bool WebPageServer::set_extra_origin(const std::string& origin)
{
    m_impl->extra_origin.clear();
    if (origin.empty()) return true;
    static const std::regex ok(R"(^http://([A-Za-z0-9.-]+|\[[0-9A-Fa-f:]+\])$)");
    if (!std::regex_match(origin, ok)) return false;
    m_impl->extra_origin = origin;
    return true;
}

bool WebPageServer::set_capability_store(std::shared_ptr<PreprintCapabilityStore> store)
{
    if (m_impl->running) return false;
    m_impl->caps = std::move(store);
    return true;
}

const std::string& WebPageServer::content_security_policy() const { return m_impl->csp; }

WebPageServer::~WebPageServer() { stop(); }

bool WebPageServer::start()
{
    if (m_impl->running) return true;
    boost::system::error_code ec;
    m_impl->canonical_root = fs::canonical(m_impl->root, ec);
    if (ec || !fs::is_directory(m_impl->canonical_root)) {
        BOOST_LOG_TRIVIAL(error) << "WebPageServer: root is not a directory: " << m_impl->root.string();
        return false;
    }
    // Fail closed: no policy, no server.
    {
        std::ifstream in(m_impl->csp_file.string(), std::ios::binary);
        std::ostringstream ss;
        if (in) ss << in.rdbuf();
        m_impl->csp = web_page_server_detail::parse_csp_file(ss.str());
        if (m_impl->csp.empty()) {
            BOOST_LOG_TRIVIAL(error) << "WebPageServer: refusing to start without a valid Content-Security-Policy file: " << m_impl->csp_file.string();
            return false;
        }
        if (!m_impl->extra_origin.empty()) {
            const std::string widened = web_page_server_detail::csp_with_extra_origin(m_impl->csp, m_impl->extra_origin);
            if (widened.empty())
                BOOST_LOG_TRIVIAL(warning) << "WebPageServer: extra origin refused, serving the static policy";
            else
                m_impl->csp = widened;
        }
    }
    try {
        m_impl->io.restart(); // a stopped server can be started again
        m_impl->token = make_token(); // a new session, a new token
        asio::ip::tcp::endpoint ep(asio::ip::make_address_v4("127.0.0.1"), 0); // loopback only
        m_impl->acceptor.open(ep.protocol());
        m_impl->acceptor.bind(ep);
        m_impl->acceptor.listen();
        m_impl->port            = m_impl->acceptor.local_endpoint().port();
        m_impl->expected_host   = "127.0.0.1:" + std::to_string(m_impl->port);
        m_impl->expected_origin = "http://" + m_impl->expected_host;
        m_impl->cookie_name     = "orca_wps_" + std::to_string(m_impl->port);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "WebPageServer: cannot bind loopback: " << e.what();
        boost::system::error_code ignored;
        m_impl->acceptor.close(ignored);
        return false;
    }
    m_impl->running = true;
    m_impl->do_accept();
    m_impl->threads.emplace_back([impl = m_impl] { impl->run_io(); });
    return true;
}

void WebPageServer::stop()
{
    if (!m_impl->running.exchange(false)) return;
    // Close the listener and every open connection on the io thread; run() then returns on its own
    // because nothing is pending, which also releases the handlers' shared_ptr<Impl> references.
    asio::post(m_impl->io, [impl = m_impl] {
        boost::system::error_code ec;
        impl->acceptor.close(ec);
        impl->accept_retry.cancel();
        for (auto* c : std::set<Impl::Connection*>(impl->open_connections)) c->close();
    });
    for (auto& t : m_impl->threads) t.join();
    m_impl->threads.clear();
}

bool WebPageServer::is_running() const { return m_impl->running; }
uint16_t WebPageServer::port() const { return m_impl->port; }
std::string WebPageServer::cookie_name() const { return m_impl->cookie_name; }
const std::string& WebPageServer::token() const { return m_impl->token; }

std::string WebPageServer::start_url(const std::string& extra_query) const
{
    return "http://127.0.0.1:" + std::to_string(m_impl->port) + url_prefix + "index.html?" + token_param + "=" + m_impl->token +
           (extra_query.empty() ? "" : "&" + extra_query);
}

}} // namespace Slic3r::GUI
