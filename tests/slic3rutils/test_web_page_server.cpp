#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "slic3r/GUI/WebPageServer.hpp"

#include <boost/asio.hpp>
#include <boost/filesystem.hpp>

#include <fstream>
#include <string>

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

struct Req
{
    std::string method = "GET";
    std::string target;
    std::string host;   // empty => correct host
    std::string origin; // empty => none
    std::string cookie;
    bool        no_host = false;
};

Reply send(const WebPageServer& s, Req r)
{
    if (r.host.empty()) r.host = "127.0.0.1:" + std::to_string(s.port());
    std::string b = r.method + " " + r.target + " HTTP/1.1\r\n";
    if (!r.no_host) b += "Host: " + r.host + "\r\n";
    if (!r.origin.empty()) b += "Origin: " + r.origin + "\r\n";
    if (!r.cookie.empty()) b += "Cookie: " + r.cookie + "\r\n";
    if (r.method == "POST") b += "Content-Length: 0\r\n";
    b += "Connection: close\r\n\r\n";
    return raw_request("127.0.0.1", s.port(), b);
}

std::string tok(const WebPageServer& s) { return std::string("?") + WebPageServer::token_param + "=" + s.token(); }

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

TEST_CASE("WebPageServer serves index.html with the session token", "[WebPageServer]")
{
    WebPageServer s(bundle_root());
    REQUIRE(s.start());
    const Reply r = send(s, {.target = "/web/flutter_web/index.html" + tok(s)});
    CHECK(r.status == 200);
    CHECK(r.header("content-type").find("text/html") == 0);
    CHECK(r.raw.find("<base href=\"/web/flutter_web/\">") != std::string::npos);
    CHECK(r.header("set-cookie").find(std::string(WebPageServer::cookie_name) + "=" + s.token()) == 0);
    CHECK(r.header("set-cookie").find("HttpOnly") != std::string::npos);
    CHECK(r.header("set-cookie").find("SameSite=Strict") != std::string::npos);
    CHECK(s.start_url() == "http://127.0.0.1:" + std::to_string(s.port()) + "/web/flutter_web/index.html" + tok(s));
    CHECK(s.token().size() == 64);
}

TEST_CASE("WebPageServer accepts the cookie after the first navigation and the directory URL maps to index.html", "[WebPageServer]")
{
    WebPageServer s(bundle_root());
    REQUIRE(s.start());
    const std::string cookie = std::string(WebPageServer::cookie_name) + "=" + s.token();
    CHECK(send(s, {.target = "/web/flutter_web/version.json", .cookie = cookie}).status == 200);
    CHECK(send(s, {.target = "/web/flutter_web/", .cookie = cookie}).status == 200);
    // A cookie with the wrong value is no better than none.
    CHECK(send(s, {.target = "/web/flutter_web/version.json", .cookie = std::string(WebPageServer::cookie_name) + "=" + std::string(64, '0')}).status == 403);
}

TEST_CASE("WebPageServer refuses requests without the right token", "[WebPageServer]")
{
    WebPageServer s(bundle_root());
    REQUIRE(s.start());
    CHECK(send(s, {.target = "/web/flutter_web/index.html"}).status == 403);
    CHECK(send(s, {.target = "/web/flutter_web/index.html?wps_token="}).status == 403);
    CHECK(send(s, {.target = "/web/flutter_web/index.html?wps_token=" + std::string(64, 'a')}).status == 403);
    // A prefix of the real token must not match.
    CHECK(send(s, {.target = "/web/flutter_web/index.html?wps_token=" + s.token().substr(0, 63)}).status == 403);
    // The same refusal for a file that does not exist: no oracle for what is on disk.
    CHECK(send(s, {.target = "/web/flutter_web/nope.js"}).status == 403);
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
    CHECK(send(s, {.target = "/web/flutter_web/index.html" + tok(s), .host = host}).status == 403);
    CHECK(send(s, {.target = "/web/flutter_web/index.html" + tok(s), .no_host = true}).status == 403);
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
    CHECK(send(s, {.target = "/web/flutter_web/index.html" + tok(s), .origin = origin}).status == 403);
    CHECK(send(s, {.target = "/web/flutter_web/index.html" + tok(s), .origin = own}).status == 200);
}

TEST_CASE("WebPageServer rejects path traversal", "[WebPageServer]")
{
    TempDir tmp;
    write_file(tmp.path / "root" / "index.html", "<html>ok</html>");
    write_file(tmp.path / "secret.txt", "TOP-SECRET");
    WebPageServer s(tmp.path / "root");
    REQUIRE(s.start());
    const std::string t = "?" + std::string(WebPageServer::token_param) + "=" + s.token();
    const std::string target = GENERATE(as<std::string>{},
        "/web/flutter_web/../secret.txt", "/web/flutter_web/%2e%2e/secret.txt", "/web/flutter_web/%2E%2E/secret.txt",
        "/web/flutter_web/..%2fsecret.txt", "/web/flutter_web/..%2Fsecret.txt", "/web/flutter_web/%2e%2e%2fsecret.txt",
        "/web/flutter_web/..%5csecret.txt", "/web/flutter_web/..\\secret.txt", "/web/flutter_web/./index.html",
        "/web/flutter_web/%2e/index.html", "/web/flutter_web//index.html", "/web/flutter_web/index.html%00.png",
        "/web/flutter_web/sub/../../secret.txt", "/web/flutter_web/%252e%252e/secret.txt", "/web/flutter_web/%zz",
        "/web/flutter_web/%", "/web/flutter_web/C:/Windows/win.ini", "/secret.txt", "/", "/web/", "/web/flutter_webx/index.html",
        "//web/flutter_web/index.html", "http://127.0.0.1/web/flutter_web/index.html", "/etc/passwd", "*", "web/flutter_web/index.html");
    CAPTURE(target);
    const Reply r = send(s, {.target = target + t});
    CHECK((r.status == 403 || r.status == 404 || r.status == 400));
    CHECK(r.raw.find("TOP-SECRET") == std::string::npos);
    // Control: the same root does serve a legitimate file, so the refusals above are not a dead server.
    CHECK(send(s, {.target = "/web/flutter_web/index.html" + t}).status == 200);
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
    const std::string t = tok(s);
    for (const char* p : {"/web/flutter_web/link.txt", "/web/flutter_web/dirlink/secret.txt"}) {
        CAPTURE(p);
        const Reply r = send(s, {.target = p + t});
        CHECK(r.status == 403);
        CHECK(r.raw.find("TOP-SECRET") == std::string::npos);
    }
    // A symlink that stays inside the root is fine.
    CHECK(send(s, {.target = "/web/flutter_web/okay-link.txt" + t}).status == 200);
#endif
}

TEST_CASE("WebPageServer lists no directories", "[WebPageServer]")
{
    TempDir tmp;
    write_file(tmp.path / "root" / "index.html", "<html>ok</html>");
    write_file(tmp.path / "root" / "sub" / "a.txt", "a");
    WebPageServer s(tmp.path / "root");
    REQUIRE(s.start());
    for (const char* p : {"/web/flutter_web/sub", "/web/flutter_web/sub/"}) {
        CAPTURE(p);
        const Reply r = send(s, {.target = p + tok(s)});
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
        const Reply r = send(s, {.method = m, .target = "/web/flutter_web/index.html" + tok(s)});
        CHECK(r.status == 405);
        CHECK(r.header("allow") == "GET, HEAD");
    }
    const Reply head = send(s, {.method = "HEAD", .target = "/web/flutter_web/index.html" + tok(s)});
    CHECK(head.status == 200);
    CHECK(std::stoul(head.header("content-length")) > 0);
    CHECK(head.raw.substr(head.raw.find("\r\n\r\n") + 4).empty());
}

TEST_CASE("WebPageServer sends no CORS headers", "[WebPageServer]")
{
    WebPageServer s(bundle_root());
    REQUIRE(s.start());
    const std::string own = "http://127.0.0.1:" + std::to_string(s.port());
    const Req reqs[] = {
        {.target = "/web/flutter_web/index.html" + tok(s)},
        {.target = "/web/flutter_web/index.html" + tok(s), .origin = own},
        {.target = "/web/flutter_web/index.html" + tok(s), .origin = "http://evil.example"},
        {.target = "/web/flutter_web/index.html"},
        {.method = "OPTIONS", .target = "/web/flutter_web/index.html" + tok(s), .origin = "http://evil.example"},
        {.target = "/web/flutter_web/missing" + tok(s)},
    };
    for (const Req& q : reqs) {
        const Reply r = send(s, q);
        CAPTURE(r.head());
        CHECK_FALSE(r.has_header("access-control-allow-origin"));
        CHECK_FALSE(r.has_header("access-control-allow-credentials"));
        CHECK_FALSE(r.has_header("access-control-allow-methods"));
    }
}

TEST_CASE("WebPageServer serves the bundle files with the right MIME types", "[WebPageServer]")
{
    WebPageServer s(bundle_root());
    REQUIRE(s.start());
    // Resolve the hashed main.*.js name from the real directory instead of hard-coding it.
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
        const Reply r = send(s, {.target = "/web/flutter_web/" + c.path + tok(s)});
        CHECK(r.status == 200);
        CHECK(r.header("content-type").find(c.mime) == 0);
        CHECK(r.header("x-content-type-options") == "nosniff");
    }
    // wasm magic number: the whole file really is streamed, not an empty body.
    const Reply wasm = send(s, {.target = "/web/flutter_web/canvaskit/canvaskit.wasm" + tok(s)});
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
    CHECK(send(s, {.target = "/web/flutter_web/a.css" + tok(s)}).header("content-type").find("text/css") == 0);
    CHECK(send(s, {.target = "/web/flutter_web/a.ttf" + tok(s)}).header("content-type") == "font/ttf");
    CHECK(send(s, {.target = "/web/flutter_web/a.OTF" + tok(s)}).header("content-type") == "font/otf");
    CHECK(send(s, {.target = "/web/flutter_web/NOTICES" + tok(s)}).header("content-type") == "application/octet-stream");
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
    CHECK(ec); // refused: nothing listens on the LAN address

    // And the listener really is on 127.0.0.1, which is reachable.
    asio::ip::tcp::socket lo(io);
    lo.connect({asio::ip::make_address("127.0.0.1"), s.port()}, ec);
    CHECK_FALSE(ec);
}

TEST_CASE("WebPageServer stops cleanly and refuses a missing root", "[WebPageServer]")
{
    {
        WebPageServer bad(fs::path("/nonexistent/wps/root"));
        CHECK_FALSE(bad.start());
    }
    WebPageServer s(bundle_root());
    REQUIRE(s.start());
    const uint16_t port = s.port();
    s.stop();
    CHECK_FALSE(s.is_running());
    asio::io_context       io;
    asio::ip::tcp::socket  sock(io);
    boost::system::error_code ec;
    sock.connect({asio::ip::make_address("127.0.0.1"), port}, ec);
    CHECK(ec);
}

TEST_CASE("Two WebPageServers use different ports and tokens", "[WebPageServer]")
{
    WebPageServer a(bundle_root()), b(bundle_root());
    REQUIRE(a.start());
    REQUIRE(b.start());
    CHECK(a.port() != b.port());
    CHECK(a.token() != b.token());
    // One server's token is no use on the other (the Host check fails first, the token second).
    CHECK(send(b, {.target = "/web/flutter_web/index.html?wps_token=" + a.token()}).status == 403);
}
