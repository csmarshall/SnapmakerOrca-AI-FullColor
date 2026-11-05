#include "WebPageServer.hpp"

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
constexpr auto        k_io_timeout      = std::chrono::seconds(30);

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

std::string cookie_value(const std::string& header, const std::string& name)
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
            if (eq != std::string::npos && item.compare(0, eq, name) == 0)
                return item.substr(eq + 1);
        }
        pos = semi + 1;
    }
    return {};
}

} // namespace

struct WebPageServer::Impl : std::enable_shared_from_this<WebPageServer::Impl>
{
    fs::path                root;           // as given
    fs::path                canonical_root; // set by start()
    std::string             token = make_token();
    std::string             expected_host;
    std::string             expected_origin;
    uint16_t                port = 0;
    asio::io_context        io{1}; // one thread: every handler is serialised, so stop() may touch connections
    asio::ip::tcp::acceptor acceptor{io};
    std::vector<std::thread> threads;
    std::atomic<bool>       running{false};
    std::atomic<std::size_t> connections{0};

    class Connection;
    std::set<Connection*> open_connections; // touched on the io thread only
    using Response = http::response<http::file_body>;

    // The result of routing one request: either a file to send or a bare status.
    struct Outcome
    {
        http::status status = http::status::forbidden;
        fs::path     file;
        bool         set_cookie = false;
        bool         allow_get_head = false;
    };

    Outcome route(http::verb method, const std::string& target, const std::string& host, const std::string& origin,
                  const std::string& cookie_header, bool has_origin) const
    {
        Outcome out;
        // DNS rebinding and cross-site requests: before anything else.
        if (host != expected_host) return out;
        if (has_origin && origin != expected_origin) return out;

        // Split target into path and query.
        const std::size_t qpos       = target.find('?');
        const std::string raw_path   = target.substr(0, qpos);
        const std::string raw_query  = qpos == std::string::npos ? std::string() : target.substr(qpos + 1);

        const std::string q_token = query_value(raw_query, token_param);
        const std::string c_token = cookie_value(cookie_header, cookie_name);
        const bool token_from_query  = !q_token.empty() && constant_time_equal(q_token, token);
        const bool token_from_cookie = !c_token.empty() && constant_time_equal(c_token, token);
        if (!token_from_query && !token_from_cookie) return out; // 403

        if (method != http::verb::get && method != http::verb::head) {
            out.status = http::status::method_not_allowed;
            return out;
        }
        // Only origin-form targets ("/...") are routed; absolute-form ("http://host/...") fails the prefix test.
        // Path: decode per segment, so an encoded "/" or "\" or "." can never form a separator or dot segment.
        const std::string prefix = url_prefix;
        if (raw_path.compare(0, prefix.size(), prefix) != 0) {
            out.status = http::status::not_found;
            return out;
        }
        const std::string rest = raw_path.substr(prefix.size());
        fs::path          rel;
        std::size_t       pos = 0;
        while (pos < rest.size()) {
            std::size_t slash = rest.find('/', pos);
            if (slash == std::string::npos) slash = rest.size();
            const std::string raw_seg = rest.substr(pos, slash - pos);
            std::string       seg;
            if (raw_seg.empty() || !percent_decode(raw_seg, seg) || seg.empty() || seg == "." || seg == ".." ||
                seg.find('/') != std::string::npos || seg.find('\\') != std::string::npos ||
                seg.find('\0') != std::string::npos || seg.find(':') != std::string::npos) {
                out.status = http::status::forbidden;
                return out;
            }
            rel /= seg;
            pos = slash + 1;
        }
        if (rest.empty() || rest.back() == '/') rel /= "index.html"; // directory-style URL -> its index page only

        // Symlinks: resolve and require the result to stay inside the canonical root.
        boost::system::error_code ec;
        const fs::path            resolved = fs::canonical(canonical_root / rel, ec);
        if (ec) {
            out.status = http::status::not_found;
            return out;
        }
        const std::string root_str = canonical_root.generic_string();
        const std::string res_str  = resolved.generic_string();
        if (res_str.size() <= root_str.size() || res_str.compare(0, root_str.size(), root_str) != 0 || res_str[root_str.size()] != '/') {
            out.status = http::status::forbidden;
            return out;
        }
        if (!fs::is_regular_file(resolved, ec)) { // directories: no listing
            out.status = http::status::not_found;
            return out;
        }
        out.status     = http::status::ok;
        out.file       = resolved;
        out.set_cookie = token_from_query;
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
        void close() { stream.close(); }

        void run()
        {
            stream.expires_after(k_io_timeout);
            parser.emplace();
            parser->header_limit(k_header_limit);
            parser->body_limit(0);
            http::async_read(stream, buffer, *parser, [self = shared_from_this()](beast::error_code ec, std::size_t) {
                if (ec) {
                    if (ec != http::error::end_of_stream) self->send_status(http::status::bad_request);
                    return;
                }
                self->handle();
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
            res.keep_alive(false);
            // No Access-Control-* header is ever added.
        }

        void send_status(http::status st)
        {
            auto res = std::make_shared<http::response<http::string_body>>(st, 11);
            common_headers(*res);
            res->set(http::field::content_type, "text/plain; charset=utf-8");
            if (st == http::status::method_not_allowed) res->set(http::field::allow, "GET, HEAD");
            res->body() = std::string(http::obsolete_reason(st)) + "\n";
            res->prepare_payload();
            keep = res;
            http::async_write(stream, *res, [self = shared_from_this()](beast::error_code, std::size_t) {
                beast::error_code ec;
                self->stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            });
        }

        void handle()
        {
            const auto&       req    = parser->get();
            const std::string target = std::string(req.target());
            const auto        host   = std::string(req[http::field::host]);
            const bool        has_origin = req.find(http::field::origin) != req.end();
            const auto        origin = std::string(req[http::field::origin]);
            const auto        cookie = std::string(req[http::field::cookie]);

            const Outcome o = impl->route(req.method(), target, host, origin, cookie, has_origin);
            if (o.status != http::status::ok) {
                send_status(o.status);
                return;
            }

            beast::error_code ec;
            http::file_body::value_type body;
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
                if (o.set_cookie) res->insert(http::field::set_cookie, set_cookie_header());
                res->content_length(size);
                keep = res;
                http::async_write(stream, *res, [self = shared_from_this()](beast::error_code, std::size_t) {
                    beast::error_code e;
                    self->stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, e);
                });
                return;
            }
            auto res = std::make_shared<Response>(http::status::ok, 11);
            common_headers(*res);
            res->set(http::field::content_type, mime_for(o.file));
            if (o.set_cookie) res->insert(http::field::set_cookie, set_cookie_header());
            res->body() = std::move(body);
            res->prepare_payload();
            keep = res;
            http::async_write(stream, *res, [self = shared_from_this()](beast::error_code, std::size_t) {
                beast::error_code e;
                self->stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, e);
            });
        }

        std::string set_cookie_header() const
        {
            return std::string(cookie_name) + "=" + impl->token + "; Path=" + url_prefix + "; HttpOnly; SameSite=Strict";
        }
    };

    void do_accept()
    {
        acceptor.async_accept([self = shared_from_this()](beast::error_code ec, asio::ip::tcp::socket sock) {
            if (ec) return; // closed by stop()
            if (self->connections.load() >= k_max_connections) {
                beast::error_code e;
                sock.close(e);
            } else {
                self->connections++;
                std::make_shared<Connection>(self, std::move(sock))->run();
            }
            self->do_accept();
        });
    }
};

WebPageServer::WebPageServer(fs::path root) : m_impl(std::make_shared<Impl>())
{
    m_impl->root = std::move(root);
}

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
    try {
        asio::ip::tcp::endpoint ep(asio::ip::make_address_v4("127.0.0.1"), 0); // loopback only
        m_impl->acceptor.open(ep.protocol());
        m_impl->acceptor.bind(ep);
        m_impl->acceptor.listen();
        m_impl->port            = m_impl->acceptor.local_endpoint().port();
        m_impl->expected_host   = "127.0.0.1:" + std::to_string(m_impl->port);
        m_impl->expected_origin = "http://" + m_impl->expected_host;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "WebPageServer: cannot bind loopback: " << e.what();
        return false;
    }
    m_impl->running = true;
    m_impl->do_accept();
    m_impl->threads.emplace_back([impl = m_impl] { impl->io.run(); });
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
        for (auto* c : std::set<Impl::Connection*>(impl->open_connections)) c->close();
    });
    for (auto& t : m_impl->threads) t.join();
    m_impl->threads.clear();
}

bool WebPageServer::is_running() const { return m_impl->running; }
uint16_t WebPageServer::port() const { return m_impl->port; }
const std::string& WebPageServer::token() const { return m_impl->token; }

std::string WebPageServer::start_url() const
{
    return "http://127.0.0.1:" + std::to_string(m_impl->port) + url_prefix + "index.html?" + token_param + "=" + m_impl->token;
}

}} // namespace Slic3r::GUI
