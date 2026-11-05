#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "slic3r/GUI/WebPageServer.hpp"

#include <boost/asio.hpp>
#include <boost/filesystem.hpp>

#include <fstream>
#include <thread>
#include <chrono>
#ifndef _WIN32
#include <sys/resource.h>
#include <unistd.h>
#endif
#include <string>
#include <vector>
#include <algorithm>

using Slic3r::GUI::WebPageServer;
namespace asio = boost::asio;
namespace fs   = boost::filesystem;

namespace {

fs::path bundle_root() { return fs::path(PROFILES_DIR).parent_path() / "web" / "flutter_web"; }

struct Reply
{
    std::string raw;
    int         status = 0;
    std::string head() const { return raw.substr(0, raw.find("\r\n\r\n")); }
    // Case-insensitive header presence check on the header block only.
    bool has_header(std::string name) const
    {
        std::string h = head(), n = name;
        for (auto& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (auto& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return h.find("\r\n" + n + ":") != std::string::npos;
    }
    std::string header(std::string name) const
    {
        const std::string h = head();
        std::string lh = h, n = "\r\n" + name + ":";
        for (auto& c : lh) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (auto& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const auto p = lh.find(n);
        if (p == std::string::npos) return {};
        const auto e = h.find("\r\n", p + 2);
        std::string v = h.substr(p + n.size(), e == std::string::npos ? std::string::npos : e - p - n.size());
        const auto f = v.find_first_not_of(' ');
        return f == std::string::npos ? std::string() : v.substr(f);
    }
};

// Sends the bytes exactly as given (no client-side path normalisation) and reads until the server closes.
Reply raw_request(const std::string& address, uint16_t port, const std::string& bytes)
{
    asio::io_context        io;
    asio::ip::tcp::socket   sock(io);
    sock.connect({asio::ip::make_address(address), port});
    asio::write(sock, asio::buffer(bytes));
    Reply                    r;
    boost::system::error_code ec;
    char                     buf[65536];
    for (;;) {
        const size_t n = sock.read_some(asio::buffer(buf), ec);
        r.raw.append(buf, n);
        if (ec) break;
    }
    if (r.raw.compare(0, 9, "HTTP/1.1 ") == 0) r.status = std::atoi(r.raw.c_str() + 9);
    return r;
}

// A request builder. By default it authenticates with the session cookie (as the webview does after the
// redirect); anon() sends no credentials and query_token() sends the token only in the URL.
struct Req
{
    std::string m_method = "GET";
    std::string m_target;
    std::string m_host;   // empty => correct host
    std::string m_origin; // empty => none
    std::string m_cookie; // empty => the valid session cookie
    std::string m_fetch_site;
    bool        m_no_host = false;
    bool        m_anon    = false;
    bool        m_query   = false;
    bool        m_raw_cookie = false; // m_cookie is the whole header, empty included

    explicit Req(std::string target) : m_target(std::move(target)) {}
    Req method(std::string v) && { m_method = std::move(v); return std::move(*this); }
    Req host(std::string v) && { m_host = std::move(v); return std::move(*this); }
    Req origin(std::string v) && { m_origin = std::move(v); return std::move(*this); }
    Req cookie(std::string v) && { m_cookie = std::move(v); m_raw_cookie = true; return std::move(*this); }
    Req fetch_site(std::string v) && { m_fetch_site = std::move(v); return std::move(*this); }
    Req no_host() && { m_no_host = true; return std::move(*this); }
    Req anon() && { m_anon = true; return std::move(*this); }
    Req query_token() && { m_query = true; m_anon = true; return std::move(*this); }
};

Reply send(const WebPageServer& s, Req r)
{
    if (r.m_host.empty()) r.m_host = "127.0.0.1:" + std::to_string(s.port());
    std::string target = r.m_target;
    if (r.m_query) target += (target.find('?') == std::string::npos ? "?" : "&") + std::string(WebPageServer::token_param) + "=" + s.token();
    std::string b = r.m_method + " " + target + " HTTP/1.1\r\n";
    if (!r.m_no_host) b += "Host: " + r.m_host + "\r\n";
    if (!r.m_origin.empty()) b += "Origin: " + r.m_origin + "\r\n";
    if (!r.m_fetch_site.empty()) b += "Sec-Fetch-Site: " + r.m_fetch_site + "\r\n";
    if (r.m_raw_cookie) b += "Cookie: " + r.m_cookie + "\r\n";
    else if (!r.m_anon) b += "Cookie: " + s.cookie_name() + "=" + s.token() + "\r\n";
    if (r.m_method == "POST") b += "Content-Length: 0\r\n";
    b += "Connection: close\r\n\r\n";
    return raw_request("127.0.0.1", s.port(), b);
}

std::string subst(std::string t, uint16_t port)
{
    for (auto&& [key, val] : {std::pair<const char*, std::string>{"{P}", std::to_string(port)}, {"{Q}", std::to_string(port + 1)}}) {
        for (size_t p; (p = t.find(key)) != std::string::npos;) t.replace(p, 3, val);
    }
    return t;
}

struct TempDir
{
    fs::path path = fs::temp_directory_path() / fs::unique_path("wps-test-%%%%-%%%%-%%%%");
    TempDir() { fs::create_directories(path); }
    ~TempDir() { boost::system::error_code ec; fs::remove_all(path, ec); }
};

void write_file(const fs::path& p, const std::string& content)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p.string(), std::ios::binary) << content;
}

} // namespace


namespace {
const std::string P = "/web/flutter_web/";
}

TEST_CASE("WebPageServer answers the token-bearing navigation with a redirect that drops the token", "[WebPageServer]")
{
    WebPageServer s(bundle_root());
    REQUIRE(s.start());
    CHECK(s.start_url() == "http://127.0.0.1:" + std::to_string(s.port()) + P + "index.html?wps_token=" + s.token());
    CHECK(s.token().size() == 64);

    const Reply r1 = send(s, Req(P + "index.html").query_token());
    CHECK(r1.status == 302);
    CHECK(r1.header("location") == P + "index.html");
    CHECK(r1.header("location").find("wps_token") == std::string::npos);
    CHECK(r1.raw.find("<html") == std::string::npos); // no content on the token-bearing URL
    const std::string sc = r1.header("set-cookie");
    CHECK(sc.find(s.cookie_name() + "=" + s.token()) == 0);
    CHECK(sc.find("HttpOnly") != std::string::npos);
    CHECK(sc.find("SameSite=Strict") != std::string::npos);
    CHECK(s.cookie_name() == "orca_wps_" + std::to_string(s.port()));

    // Follow the redirect the way a browser does: the cookie from the 302, the Location from the 302.
    const Reply r2 = send(s, Req(r1.header("location")).cookie(sc.substr(0, sc.find(';'))));
    CHECK(r2.status == 200);
    CHECK(r2.header("content-type").find("text/html") == 0);
    CHECK(r2.raw.find("<base href=\"/web/flutter_web/\">") != std::string::npos);
    CHECK_FALSE(r2.has_header("set-cookie"));
    CHECK(r2.raw.find(s.token()) == std::string::npos);
}

TEST_CASE("WebPageServer keeps other query parameters when it strips the token", "[WebPageServer]")
{
    WebPageServer s(bundle_root());
    REQUIRE(s.start());
    const Reply r = send(s, Req(P + "index.html?a=1&wps_token=" + s.token() + "&b=2").anon());
    CHECK(r.status == 302);
    CHECK(r.header("location") == P + "index.html?a=1&b=2");
    // A redirect is not issued for a path that does not exist, and no cookie is set for it.
    const Reply nf = send(s, Req(P + "missing.js").query_token());
    CHECK(nf.status == 404);
    CHECK_FALSE(nf.has_header("set-cookie"));
}

TEST_CASE("WebPageServer directory URL maps to index.html", "[WebPageServer]")
{
    WebPageServer s(bundle_root());
    REQUIRE(s.start());
    CHECK(send(s, Req(P)).status == 200);
    CHECK(send(s, Req(P + "version.json")).status == 200);
}

TEST_CASE("WebPageServer refuses requests without the right token", "[WebPageServer]")
{
    WebPageServer s(bundle_root());
    REQUIRE(s.start());
    const std::string ck = s.cookie_name();
    CHECK(send(s, Req(P + "index.html").anon()).status == 403);
    CHECK(send(s, Req(P + "index.html?wps_token=").anon()).status == 403);
    CHECK(send(s, Req(P + "index.html?wps_token=" + std::string(64, 'a')).anon()).status == 403);
    CHECK(send(s, Req(P + "index.html?wps_token=" + s.token().substr(0, 63)).anon()).status == 403);
    CHECK(send(s, Req(P + "version.json").cookie(ck + "=" + std::string(64, '0'))).status == 403);
    CHECK(send(s, Req(P + "version.json").cookie(ck + "=" + s.token().substr(0, 63))).status == 403);
    // The old unscoped cookie name is not accepted.
    CHECK(send(s, Req(P + "version.json").cookie("orca_wps=" + s.token())).status == 403);
    // The same refusal for a file that does not exist: no oracle for what is on disk.
    CHECK(send(s, Req(P + "nope.js").anon()).status == 403);
}

TEST_CASE("WebPageServer cookie is per port and a planted duplicate cannot shadow it", "[WebPageServer]")
{
    WebPageServer a(bundle_root()), b(bundle_root());
    REQUIRE(a.start());
    REQUIRE(b.start());
    CHECK(a.cookie_name() != b.cookie_name());
    const std::string good = a.cookie_name() + "=" + a.token();
    const std::string bad  = a.cookie_name() + "=" + std::string(64, 'f');
    // Planted junk before, after, and in a second Cookie header: any matching cookie of the name is enough.
    CHECK(send(a, Req(P + "version.json").cookie(bad + "; " + good)).status == 200);
    CHECK(send(a, Req(P + "version.json").cookie(good + "; " + bad)).status == 200);
    CHECK(send(a, Req(P + "version.json").cookie("x=1;  " + bad + ";" + good + "; y=2")).status == 200);
    // ... but junk alone is refused, and so is the other server's cookie (its name and token differ).
    CHECK(send(a, Req(P + "version.json").cookie(bad)).status == 403);
    CHECK(send(a, Req(P + "version.json").cookie(b.cookie_name() + "=" + b.token())).status == 403);
    CHECK(send(a, Req(P + "version.json").cookie(b.cookie_name() + "=" + a.token())).status == 403);
    // Raw second Cookie header line.
    const std::string two = "GET " + P + "version.json HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(a.port()) + "\r\nCookie: " + bad +
                            "\r\nCookie: " + good + "\r\nConnection: close\r\n\r\n";
    CHECK(raw_request("127.0.0.1", a.port(), two).status == 200);
}

TEST_CASE("WebPageServer honours Sec-Fetch-Site", "[WebPageServer]")
{
    WebPageServer s(bundle_root());
    REQUIRE(s.start());
    CHECK(send(s, Req(P + "version.json")).status == 200); // absent
    CHECK(send(s, Req(P + "version.json").fetch_site("same-origin")).status == 200);
    CHECK(send(s, Req(P + "version.json").fetch_site("none")).status == 200);
    for (const char* v : {"cross-site", "same-site", "bogus", "Same-Origin-ish"}) {
        CAPTURE(v);
        CHECK(send(s, Req(P + "version.json").fetch_site(v)).status == 403);
    }
    // It applies to the token-bearing first navigation as well.
    CHECK(send(s, Req(P + "index.html").query_token().fetch_site("cross-site")).status == 403);
    CHECK(send(s, Req(P + "index.html").query_token().fetch_site("none")).status == 302);
}

TEST_CASE("WebPageServer refuses a Host header that is not its own loopback origin", "[WebPageServer]")
{
    WebPageServer s(bundle_root());
    REQUIRE(s.start());
    // {P} = this server's port, {Q} = port + 1
    const std::string tmpl = GENERATE(as<std::string>{}, "evil.example:{P}", "evil.example", "localhost:{P}", "127.0.0.1", "127.0.0.1:{Q}",
                                      "127.0.0.1.evil.example:{P}", "[::1]:{P}");
    const std::string host = subst(tmpl, s.port());
    CAPTURE(host);
    CHECK(send(s, Req(P + "index.html").host(host)).status == 403);
    CHECK(send(s, Req(P + "index.html").no_host()).status == 403);
}

TEST_CASE("WebPageServer refuses a foreign Origin and accepts its own", "[WebPageServer]")
{
    WebPageServer s(bundle_root());
    REQUIRE(s.start());
    const std::string own = "http://127.0.0.1:" + std::to_string(s.port());
    const std::string tmpl = GENERATE(as<std::string>{}, "http://evil.example", "https://evil.example", "null", "http://localhost:{P}",
                                      "http://127.0.0.1:{Q}", "https://127.0.0.1:{P}", "http://127.0.0.1:{P}.evil.example");
    const std::string origin = subst(tmpl, s.port());
    CAPTURE(origin);
    CHECK(send(s, Req(P + "index.html").origin(origin)).status == 403);
    CHECK(send(s, Req(P + "index.html").origin(own)).status == 200);
}

TEST_CASE("Path segment rules (layer 1) reject traversal on their own", "[WebPageServer][traversal]")
{
    using Slic3r::GUI::web_page_server_detail::segments_to_relative;
    fs::path rel;
    const std::string bad = GENERATE(as<std::string>{}, "../secret.txt", "%2e%2e/secret.txt", "%2E%2E/secret.txt", "..%2fsecret.txt",
        "..%2Fsecret.txt", "%2e%2e%2fsecret.txt", "..%5csecret.txt", "..\\secret.txt", "./index.html", "%2e/index.html", "/index.html",
        "index.html%00.png", "sub/../../secret.txt", "%zz", "%", "%4", "C:/Windows/win.ini", "a//b");
    CAPTURE(bad);
    CHECK_FALSE(segments_to_relative(bad, rel));
    // %252e%252e decodes once to the literal "%2e%2e", which is a harmless file name, not a dot segment.
    REQUIRE(segments_to_relative("%252e%252e/secret.txt", rel));
    CHECK(rel == fs::path("%2e%2e") / "secret.txt");
    REQUIRE(segments_to_relative("a/b%20c.txt", rel));
    CHECK(rel == fs::path("a") / "b c.txt");
    REQUIRE(segments_to_relative("", rel));
    CHECK(rel == fs::path("index.html"));
    REQUIRE(segments_to_relative("sub/", rel));
    CHECK(rel == fs::path("sub") / "index.html");
}

TEST_CASE("Canonical containment (layer 2) refuses escapes on its own", "[WebPageServer][traversal]")
{
    using Slic3r::GUI::web_page_server_detail::resolve_within;
    TempDir tmp;
    write_file(tmp.path / "root" / "index.html", "<html>ok</html>");
    write_file(tmp.path / "root" / "sub" / "a.txt", "a");
    write_file(tmp.path / "secret.txt", "TOP-SECRET");
    write_file(tmp.path / "root-evil" / "x.txt", "sibling sharing the root's name as a prefix");
    const fs::path root = fs::canonical(tmp.path / "root");
    fs::path       out;
    // Handed ".." directly, bypassing layer 1.
    CHECK(resolve_within(root, fs::path("..") / "secret.txt", out) == 403);
    CHECK(resolve_within(root, fs::path("sub") / ".." / ".." / "secret.txt", out) == 403);
    CHECK(resolve_within(root, fs::path("..") / "root-evil" / "x.txt", out) == 403); // prefix-sharing sibling
    CHECK(resolve_within(root, fs::path("/etc/passwd"), out) != 200);
    CHECK(resolve_within(root, "index.html", out) == 200);
    CHECK(resolve_within(root, "missing.txt", out) == 404);
    CHECK(resolve_within(root, "sub", out) == 404); // a directory is not a file
#ifndef _WIN32
    fs::create_symlink(tmp.path / "secret.txt", tmp.path / "root" / "link.txt");
    fs::create_directory_symlink(tmp.path / "root-evil", tmp.path / "root" / "dirlink");
    fs::create_symlink(tmp.path / "root" / "index.html", tmp.path / "root" / "okay-link.txt");
    CHECK(resolve_within(root, "link.txt", out) == 403);
    CHECK(resolve_within(root, fs::path("dirlink") / "x.txt", out) == 403);
    CHECK(resolve_within(root, "okay-link.txt", out) == 200);
#endif
}

TEST_CASE("WebPageServer rejects path traversal end to end", "[WebPageServer]")
{
    TempDir tmp;
    write_file(tmp.path / "root" / "index.html", "<html>ok</html>");
    write_file(tmp.path / "secret.txt", "TOP-SECRET");
    WebPageServer s(tmp.path / "root");
    REQUIRE(s.start());
    const std::string target = GENERATE(as<std::string>{},
        "/web/flutter_web/../secret.txt", "/web/flutter_web/%2e%2e/secret.txt", "/web/flutter_web/%2E%2E/secret.txt",
        "/web/flutter_web/..%2fsecret.txt", "/web/flutter_web/..%2Fsecret.txt", "/web/flutter_web/%2e%2e%2fsecret.txt",
        "/web/flutter_web/..%5csecret.txt", "/web/flutter_web/..\\secret.txt", "/web/flutter_web/./index.html",
        "/web/flutter_web/%2e/index.html", "/web/flutter_web//index.html", "/web/flutter_web/index.html%00.png",
        "/web/flutter_web/sub/../../secret.txt", "/web/flutter_web/%252e%252e/secret.txt", "/web/flutter_web/%zz",
        "/web/flutter_web/%", "/web/flutter_web/C:/Windows/win.ini", "/secret.txt", "/", "/web/", "/web/flutter_webx/index.html",
        "//web/flutter_web/index.html", "http://127.0.0.1/web/flutter_web/index.html", "/etc/passwd", "*", "web/flutter_web/index.html");
    CAPTURE(target);
    const Reply r = send(s, Req(target));
    CHECK((r.status == 403 || r.status == 404 || r.status == 400));
    CHECK(r.raw.find("TOP-SECRET") == std::string::npos);
    CHECK(send(s, Req(P + "index.html")).status == 200); // control
}

TEST_CASE("WebPageServer does not follow a symlink out of the root", "[WebPageServer]")
{
#ifdef _WIN32
    SKIP("symlinks need privileges on Windows");
#else
    TempDir tmp;
    write_file(tmp.path / "root" / "index.html", "<html>ok</html>");
    write_file(tmp.path / "root" / "inside.txt", "inside");
    write_file(tmp.path / "outside" / "secret.txt", "TOP-SECRET");
    fs::create_symlink(tmp.path / "outside" / "secret.txt", tmp.path / "root" / "link.txt");
    fs::create_directory_symlink(tmp.path / "outside", tmp.path / "root" / "dirlink");
    fs::create_symlink(tmp.path / "root" / "inside.txt", tmp.path / "root" / "okay-link.txt");
    WebPageServer s(tmp.path / "root");
    REQUIRE(s.start());
    for (const char* p : {"link.txt", "dirlink/secret.txt"}) {
        CAPTURE(p);
        const Reply r = send(s, Req(P + p));
        CHECK(r.status == 403);
        CHECK(r.raw.find("TOP-SECRET") == std::string::npos);
    }
    CHECK(send(s, Req(P + "okay-link.txt")).status == 200);
#endif
}

TEST_CASE("WebPageServer lists no directories", "[WebPageServer]")
{
    TempDir tmp;
    write_file(tmp.path / "root" / "index.html", "<html>ok</html>");
    write_file(tmp.path / "root" / "sub" / "a.txt", "a");
    WebPageServer s(tmp.path / "root");
    REQUIRE(s.start());
    for (const char* p : {"sub", "sub/"}) {
        CAPTURE(p);
        const Reply r = send(s, Req(P + p));
        CHECK(r.status == 404);
        CHECK(r.raw.find("a.txt") == std::string::npos);
    }
}

TEST_CASE("WebPageServer answers only GET and HEAD", "[WebPageServer]")
{
    WebPageServer s(bundle_root());
    REQUIRE(s.start());
    for (const char* m : {"POST", "PUT", "DELETE", "PATCH", "OPTIONS", "TRACE", "CONNECT"}) {
        CAPTURE(m);
        const Reply r = send(s, Req(P + "index.html").method(m));
        CHECK(r.status == 405);
        CHECK(r.header("allow") == "GET, HEAD");
    }
    const Reply head = send(s, Req(P + "index.html").method("HEAD"));
    CHECK(head.status == 200);
    CHECK(std::stoul(head.header("content-length")) > 0);
    CHECK(head.raw.substr(head.raw.find("\r\n\r\n") + 4).empty());
}

TEST_CASE("WebPageServer sends no CORS headers and always nosniff", "[WebPageServer]")
{
    WebPageServer s(bundle_root());
    REQUIRE(s.start());
    const std::string own = "http://127.0.0.1:" + std::to_string(s.port());
    std::vector<Req> reqs;
    reqs.push_back(Req(P + "index.html"));
    reqs.push_back(Req(P + "index.html").origin(own));
    reqs.push_back(Req(P + "index.html").origin("http://evil.example"));
    reqs.push_back(Req(P + "index.html").anon());
    reqs.push_back(Req(P + "index.html").query_token());
    reqs.push_back(Req(P + "index.html").method("OPTIONS").origin("http://evil.example"));
    reqs.push_back(Req(P + "missing"));
    for (const Req& q : reqs) {
        const Reply r = send(s, q);
        CAPTURE(r.head());
        CHECK_FALSE(r.has_header("access-control-allow-origin"));
        CHECK_FALSE(r.has_header("access-control-allow-credentials"));
        CHECK_FALSE(r.has_header("access-control-allow-methods"));
        CHECK(r.header("x-content-type-options") == "nosniff");
        CHECK(r.header("referrer-policy") == "no-referrer");
    }
}

TEST_CASE("WebPageServer serves the bundle files with the right MIME types", "[WebPageServer]")
{
    WebPageServer s(bundle_root());
    REQUIRE(s.start());
    std::string main_js;
    for (auto& e : fs::directory_iterator(bundle_root()))
        if (e.path().filename().string().rfind("main.", 0) == 0 && e.path().extension() == ".js") main_js = e.path().filename().string();
    REQUIRE(!main_js.empty());

    struct Case { std::string path, mime; };
    const Case cases[] = {
        {main_js, "text/javascript"},
        {"canvaskit/canvaskit.wasm", "application/wasm"},
        {"version.json", "application/json"},
        {"manifest.json", "application/json"},
        {"favicon.png", "image/png"},
        {"index.html", "text/html"},
        {"assets/assets/svgs/iconGcode%2520copy.svg", "image/svg+xml"}, // on-disk name contains a literal "%20"
    };
    for (const Case& c : cases) {
        CAPTURE(c.path);
        const Reply r = send(s, Req(P + c.path));
        CHECK(r.status == 200);
        CHECK(r.header("content-type").find(c.mime) == 0);
        CHECK(r.header("x-content-type-options") == "nosniff");
    }
    const Reply wasm = send(s, Req(P + "canvaskit/canvaskit.wasm"));
    CHECK(wasm.raw.compare(wasm.raw.find("\r\n\r\n") + 4, 4, std::string("\0asm", 4)) == 0);
    CHECK(wasm.raw.size() > 1000000);
}

TEST_CASE("WebPageServer maps css, ttf, otf and unknown extensions", "[WebPageServer]")
{
    TempDir tmp;
    write_file(tmp.path / "r" / "a.css", "a{}");
    write_file(tmp.path / "r" / "a.ttf", "t");
    write_file(tmp.path / "r" / "a.OTF", "o");
    write_file(tmp.path / "r" / "NOTICES", "n");
    WebPageServer s(tmp.path / "r");
    REQUIRE(s.start());
    CHECK(send(s, Req(P + "a.css")).header("content-type").find("text/css") == 0);
    CHECK(send(s, Req(P + "a.ttf")).header("content-type") == "font/ttf");
    CHECK(send(s, Req(P + "a.OTF")).header("content-type") == "font/otf");
    CHECK(send(s, Req(P + "NOTICES")).header("content-type") == "application/octet-stream");
}

TEST_CASE("WebPageServer is bound to loopback only", "[WebPageServer]")
{
    WebPageServer s(bundle_root());
    REQUIRE(s.start());

    // Find this machine's outward-facing address without sending a packet (UDP connect only picks a route).
    asio::io_context       io;
    asio::ip::udp::socket  probe(io);
    boost::system::error_code ec;
    probe.connect({asio::ip::make_address("192.0.2.1"), 9}, ec);
    std::string lan;
    if (!ec) lan = probe.local_endpoint(ec).address().to_string();
    if (ec || lan.empty() || lan == "127.0.0.1" || lan == "0.0.0.0")
        SKIP("no non-loopback IPv4 address to probe");

    asio::ip::tcp::socket sock(io);
    sock.connect({asio::ip::make_address(lan), s.port()}, ec);
    CHECK(ec);

    asio::ip::tcp::socket lo(io);
    lo.connect({asio::ip::make_address("127.0.0.1"), s.port()}, ec);
    CHECK_FALSE(ec);
}

TEST_CASE("WebPageServer stops, refuses a missing root, and can be started again", "[WebPageServer]")
{
    {
        WebPageServer bad(fs::path("/nonexistent/wps/root"));
        CHECK_FALSE(bad.start());
    }
    WebPageServer s(bundle_root());
    REQUIRE(s.start());
    const uint16_t port1  = s.port();
    const std::string tok1 = s.token();
    CHECK(send(s, Req(P + "version.json")).status == 200);
    s.stop();
    CHECK_FALSE(s.is_running());
    {
        asio::io_context       io;
        asio::ip::tcp::socket  sock(io);
        boost::system::error_code ec;
        sock.connect({asio::ip::make_address("127.0.0.1"), port1}, ec);
        CHECK(ec);
    }
    // Restart: works, serves, and is a new session (new token; the old cookie value is useless).
    REQUIRE(s.start());
    CHECK(s.is_running());
    CHECK(s.token() != tok1);
    CHECK(send(s, Req(P + "version.json")).status == 200);
    CHECK(send(s, Req(P + "version.json").cookie(s.cookie_name() + "=" + tok1)).status == 403);
    // Stop/start a few more times, and a double stop/start is harmless.
    for (int i = 0; i < 3; ++i) {
        s.stop();
        s.stop();
        REQUIRE(s.start());
        REQUIRE(s.start());
        CHECK(send(s, Req(P + "index.html")).status == 200);
    }
}

TEST_CASE("Two WebPageServers use different ports and tokens", "[WebPageServer]")
{
    WebPageServer a(bundle_root()), b(bundle_root());
    REQUIRE(a.start());
    REQUIRE(b.start());
    CHECK(a.port() != b.port());
    CHECK(a.token() != b.token());
    CHECK(send(b, Req(P + "index.html?wps_token=" + a.token()).anon()).status == 403);
}

TEST_CASE("WebPageServer drops a client that never finishes its request header", "[WebPageServer][slow]")
{
    WebPageServer s(bundle_root());
    REQUIRE(s.start());
    asio::io_context       io;
    asio::ip::tcp::socket  idle(io);
    idle.connect({asio::ip::make_address("127.0.0.1"), s.port()});
    asio::write(idle, asio::buffer(std::string("GET /web/flutter_web/index.html HTTP/1.1\r\nHost: x")));
    const auto t0 = std::chrono::steady_clock::now();
    boost::system::error_code ec;
    char                      buf[64];
    idle.read_some(asio::buffer(buf), ec); // blocks until the server hangs up
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ec);
    CHECK(secs >= 4);
    CHECK(secs <= 8); // the header timeout is 5 s, well under the 120 s file timeout
    // Meanwhile the server is still serving.
    CHECK(send(s, Req(P + "version.json")).status == 200);
}

#ifndef _WIN32
TEST_CASE("WebPageServer keeps accepting after a transient accept error (EMFILE)", "[WebPageServer][emfile]")
{
    WebPageServer s(bundle_root());
    REQUIRE(s.start());
    CHECK(send(s, Req(P + "version.json")).status == 200);

    // Restores the descriptor limit and releases every hogged descriptor even if a check below throws.
    struct FdHog
    {
        rlimit           old{};
        std::vector<int> fds;
        FdHog() { REQUIRE(getrlimit(RLIMIT_NOFILE, &old) == 0); }
        void hog()
        {
            rlimit lim = old;
            lim.rlim_cur = std::min<rlim_t>(old.rlim_cur, 256);
            REQUIRE(setrlimit(RLIMIT_NOFILE, &lim) == 0);
            for (;;) {
                int fd = ::dup(0);
                if (fd < 0) break;
                fds.push_back(fd);
            }
        }
        ~FdHog()
        {
            for (int fd : fds) ::close(fd);
            setrlimit(RLIMIT_NOFILE, &old);
        }
    };
    {
        FdHog                 hog;
        asio::io_context      io;
        asio::ip::tcp::socket c(io);
        c.open(asio::ip::tcp::v4()); // created before the descriptors run out
        hog.hog();                   // every descriptor used: the server's next accept() fails with EMFILE
        boost::system::error_code ec;
        c.connect({asio::ip::make_address("127.0.0.1"), s.port()}, ec); // the kernel completes the handshake
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400)); // longer than the 100 ms retry pause
    CHECK(send(s, Req(P + "version.json")).status == 200);
}
#endif
