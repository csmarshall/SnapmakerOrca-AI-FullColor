#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/PreprintCapabilityStore.hpp"
#include "slic3r/GUI/WebPageServer.hpp"

#include <boost/asio.hpp>
#include <boost/filesystem.hpp>

#include <atomic>
#include <chrono>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

using Slic3r::GUI::PreprintCapabilityStore;
using Slic3r::GUI::WebPageServer;
namespace asio = boost::asio;
namespace fs   = boost::filesystem;

namespace {

fs::path real_csp() { return fs::path(PROFILES_DIR).parent_path() / "web" / "flutter_web.csp"; }
fs::path bundle_root() { return fs::path(PROFILES_DIR).parent_path() / "web" / "flutter_web"; }

struct Reply
{
    std::string raw;
    int         status = 0;
    std::string head() const { return raw.substr(0, raw.find("\r\n\r\n")); }
    std::string body() const { const auto p = raw.find("\r\n\r\n"); return p == std::string::npos ? std::string() : raw.substr(p + 4); }
    std::string header(std::string name) const // case-insensitive, "" if absent
    {
        auto low = [](std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; };
        const std::string h = head(), lh = low(h), n = "\r\n" + low(name) + ":";
        const auto p = lh.find(n);
        if (p == std::string::npos) return {};
        const auto e = h.find("\r\n", p + 2);
        std::string v = h.substr(p + n.size(), e == std::string::npos ? std::string::npos : e - p - n.size());
        const auto f = v.find_first_not_of(' ');
        return f == std::string::npos ? std::string() : v.substr(f);
    }
};

Reply raw_request(uint16_t port, const std::string& bytes)
{
    asio::io_context      io;
    asio::ip::tcp::socket sock(io);
    sock.connect({asio::ip::make_address("127.0.0.1"), port});
    asio::write(sock, asio::buffer(bytes));
    Reply                     r;
    boost::system::error_code ec;
    char                      buf[65536];
    for (;;) {
        const size_t n = sock.read_some(asio::buffer(buf), ec);
        r.raw.append(buf, n);
        if (ec) break;
    }
    if (r.raw.compare(0, 9, "HTTP/1.1 ") == 0) r.status = std::atoi(r.raw.c_str() + 9);
    return r;
}

struct Opts
{
    std::string method = "GET";
    bool        cookie = true;
    bool        query_token = false;
    std::string extra; // extra header lines, each ending in \r\n
};

Reply get(const WebPageServer& s, const std::string& target, const Opts& o = {})
{
    std::string t = target;
    if (o.query_token) t += (t.find('?') == std::string::npos ? "?" : "&") + std::string(WebPageServer::token_param) + "=" + s.token();
    std::string b = o.method + " " + t + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(s.port()) + "\r\n";
    if (o.cookie) b += "Cookie: " + s.cookie_name() + "=" + s.token() + "\r\n";
    b += o.extra + "Connection: close\r\n\r\n";
    return raw_request(s.port(), b);
}

struct TempDir
{
    fs::path path = fs::temp_directory_path() / fs::unique_path("cap-test-%%%%-%%%%-%%%%");
    TempDir() { fs::create_directories(path); }
    ~TempDir() { boost::system::error_code ec; fs::remove_all(path, ec); }
};

std::string payload(std::size_t n)
{
    std::string s(n, '\0');
    for (std::size_t i = 0; i < n; ++i) s[i] = static_cast<char>((i * 131 + 7) & 0xff); // includes NUL and high bytes
    return s;
}

void write_file(const fs::path& p, const std::string& c) { std::ofstream(p.string(), std::ios::binary) << c; }
std::string read_file(const fs::path& p)
{
    std::ifstream in(p.string(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// A store with a manually advanced clock.
struct Fixture
{
    TempDir                                   tmp;
    std::shared_ptr<std::atomic<long long>>   now_ms = std::make_shared<std::atomic<long long>>(0);
    std::shared_ptr<PreprintCapabilityStore>  store;
    WebPageServer                             server{bundle_root(), real_csp()};
    fs::path                                  source = tmp.path / "plate.gcode";
    std::string                               bytes  = payload(300 * 1000 + 17);
    PreprintCapabilityStore::Registration     reg;

    Fixture()
    {
        auto n = now_ms;
        store  = std::make_shared<PreprintCapabilityStore>([n] { return std::chrono::steady_clock::time_point(std::chrono::milliseconds(n->load())); });
        write_file(source, bytes);
        REQUIRE(server.set_capability_store(store));
        REQUIRE(server.start());
        reg = store->register_file(source, "plate_1.gcode");
        REQUIRE(reg.ok);
    }
    void advance(std::chrono::milliseconds d) { now_ms->fetch_add(d.count()); }
};

} // namespace

TEST_CASE("capability serves exactly the snapshot bytes, with the hardened headers", "[PreprintCapability]")
{
    Fixture f;
    REQUIRE(f.reg.size == f.bytes.size());
    REQUIRE(f.reg.sha256_hex.size() == 64);
    const Reply r = get(f.server, f.reg.url_path);
    REQUIRE(r.status == 200);
    CHECK(r.body() == f.bytes);
    CHECK(r.header("Content-Type") == "application/octet-stream");
    CHECK(r.header("Cache-Control") == "no-store");
    CHECK(r.header("X-Content-Type-Options") == "nosniff");
    CHECK(r.header("Content-Length") == std::to_string(f.bytes.size()));
    CHECK(r.header("Access-Control-Allow-Origin").empty());
}

TEST_CASE("capability sha256 matches a known vector", "[PreprintCapability]")
{
    TempDir tmp;
    write_file(tmp.path / "abc", "abc");
    PreprintCapabilityStore store;
    const auto reg = store.register_file(tmp.path / "abc", "abc.gcode");
    REQUIRE(reg.ok);
    CHECK(reg.sha256_hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("capability is consumed by one full transfer: second GET is 410", "[PreprintCapability]")
{
    Fixture f;
    REQUIRE(get(f.server, f.reg.url_path).status == 200);
    CHECK(get(f.server, f.reg.url_path).status == 410);
    CHECK_FALSE(f.store->has_live_entries());
}

TEST_CASE("an aborted transfer does not consume the capability", "[PreprintCapability]")
{
    Fixture f;
    const auto a = f.store->begin_serve(f.reg.url_path);
    REQUIRE(a.status == PreprintCapabilityStore::Status::Ok);
    CHECK(f.store->begin_serve(f.reg.url_path).status == PreprintCapabilityStore::Status::Gone); // in flight
    f.store->finish_serve(f.reg.url_path, false);
    CHECK(get(f.server, f.reg.url_path).status == 200);
}

TEST_CASE("capability needs the session cookie and the session token", "[PreprintCapability]")
{
    Fixture f;
    Opts no_cookie; no_cookie.cookie = false;
    CHECK(get(f.server, f.reg.url_path, no_cookie).status == 403);
    Opts query_only; query_only.cookie = false; query_only.query_token = true;
    CHECK(get(f.server, f.reg.url_path, query_only).status == 403); // the query token does not authorise a capability
    Opts wrong; wrong.cookie = false;
    wrong.extra = "Cookie: " + f.server.cookie_name() + "=" + std::string(64, '0') + "\r\n";
    CHECK(get(f.server, f.reg.url_path, wrong).status == 403);
    CHECK(get(f.server, f.reg.url_path).status == 200); // still intact after the refusals
}

TEST_CASE("capability lookup is exact: other, traversal and encoded paths are 404", "[PreprintCapability]")
{
    Fixture f;
    const std::string token_dir = f.reg.url_path.substr(0, f.reg.url_path.rfind('/'));
    const std::string good_token = token_dir.substr(5);
    std::string flipped = good_token; flipped[0] = flipped[0] == 'a' ? 'b' : 'a';
    for (const std::string& t : std::vector<std::string>{
             std::string("/cap/"), std::string("/cap"), std::string("/cap/") + std::string(64, '0') + "/plate_1.gcode",
             "/cap/" + flipped + "/plate_1.gcode", token_dir, token_dir + "/other.gcode", token_dir + "/plate_1.gcode/",
             token_dir + "/../plate_1.gcode", "/cap/../" + good_token + "/plate_1.gcode", token_dir + "/./plate_1.gcode",
             token_dir + "/%2e%2e/plate_1.gcode", token_dir + "/plate%5F1.gcode", token_dir + "/plate_1%2Egcode",
             "/cap/%2e%2e/%2e%2e/etc/passwd", "/cap//" + good_token + "/plate_1.gcode", "/CAP/" + good_token + "/plate_1.gcode",
             token_dir + "%2Fplate_1.gcode", token_dir + "\\plate_1.gcode" }) {
        INFO(t);
        CHECK(get(f.server, t).status == 404);
    }
    // Through the bundle route the existing traversal layer refuses it (403): either way nothing is served.
    const Reply via_bundle = get(f.server, "/web/flutter_web/../" + f.reg.url_path.substr(1));
    CHECK((via_bundle.status == 403 || via_bundle.status == 404));
    CHECK(via_bundle.raw.find(f.bytes.substr(1000, 64)) == std::string::npos);
    CHECK(get(f.server, f.reg.url_path).status == 200); // none of that consumed it
}

TEST_CASE("capability is GET only and refuses Range", "[PreprintCapability]")
{
    Fixture f;
    for (const char* m : {"POST", "PUT", "DELETE", "HEAD", "PATCH", "OPTIONS"}) {
        Opts o; o.method = m;
        INFO(m);
        CHECK(get(f.server, f.reg.url_path, o).status == 405);
    }
    Opts range; range.extra = "Range: bytes=0-9\r\n";
    const Reply r = get(f.server, f.reg.url_path, range);
    CHECK(r.status == 416);
    CHECK(r.body().find(f.bytes.substr(0, 16)) == std::string::npos);
    CHECK(get(f.server, f.reg.url_path).status == 200); // refused requests did not consume it
}

TEST_CASE("capability: revoke makes the route 410 and deletes the snapshot", "[PreprintCapability]")
{
    Fixture f;
    const fs::path snap = f.store->snapshot_path(f.reg.url_path);
    REQUIRE(fs::exists(snap));
    f.store->revoke(f.reg.url_path);
    CHECK_FALSE(fs::exists(snap));
    CHECK(get(f.server, f.reg.url_path).status == 410);
}

TEST_CASE("capability: TTL expiry makes the route 410 and deletes the snapshot", "[PreprintCapability]")
{
    Fixture f;
    const fs::path snap = f.store->snapshot_path(f.reg.url_path);
    f.advance(PreprintCapabilityStore::capability_ttl - std::chrono::milliseconds(1));
    REQUIRE(fs::exists(snap));
    f.advance(std::chrono::milliseconds(1));
    CHECK(get(f.server, f.reg.url_path).status == 410);
    CHECK_FALSE(fs::exists(snap));
}

TEST_CASE("capability: TTL constant is 10 minutes", "[PreprintCapability]")
{
    CHECK(PreprintCapabilityStore::capability_ttl == std::chrono::minutes(10));
}

TEST_CASE("capability: destruction revokes and removes the snapshots", "[PreprintCapability]")
{
    TempDir tmp;
    write_file(tmp.path / "a", "hello");
    fs::path snap;
    {
        PreprintCapabilityStore store;
        const auto reg = store.register_file(tmp.path / "a", "a.gcode");
        REQUIRE(reg.ok);
        snap = store.snapshot_path(reg.url_path);
        REQUIRE(fs::exists(snap));
    }
    CHECK_FALSE(fs::exists(snap));
    CHECK_FALSE(fs::exists(snap.parent_path()));
}

TEST_CASE("capability: mutating or deleting the source does not change the served bytes", "[PreprintCapability]")
{
    Fixture f;
    write_file(f.source, "something else entirely, a background re-slice");
    CHECK(get(f.server, f.reg.url_path).body() == f.bytes);

    Fixture g;
    fs::remove(g.source);
    CHECK(get(g.server, g.reg.url_path).body() == g.bytes);
}

TEST_CASE("capability: a corrupted snapshot (same size or different size) gives 500 and serves nothing", "[PreprintCapability]")
{
    SECTION("same size, flipped byte")
    {
        Fixture f;
        std::string c = read_file(f.store->snapshot_path(f.reg.url_path));
        c[c.size() / 2] ^= 0x01;
        write_file(f.store->snapshot_path(f.reg.url_path), c);
        const Reply r = get(f.server, f.reg.url_path);
        CHECK(r.status == 500);
        CHECK(r.header("Content-Type").find("octet-stream") == std::string::npos);
        CHECK(r.raw.find(f.bytes.substr(1000, 64)) == std::string::npos);
        CHECK(get(f.server, f.reg.url_path).status == 500); // stays refused
    }
    SECTION("truncated")
    {
        Fixture f;
        write_file(f.store->snapshot_path(f.reg.url_path), f.bytes.substr(0, 100));
        CHECK(get(f.server, f.reg.url_path).status == 500);
    }
    SECTION("snapshot deleted underneath")
    {
        Fixture f;
        fs::remove(f.store->snapshot_path(f.reg.url_path));
        CHECK(get(f.server, f.reg.url_path).status == 500);
    }
}

TEST_CASE("capability tokens are 256-bit and unique", "[PreprintCapability]")
{
    TempDir tmp;
    write_file(tmp.path / "a", "x");
    PreprintCapabilityStore store;
    std::set<std::string>   tokens;
    for (int i = 0; i < 200; ++i) {
        const auto reg = store.register_file(tmp.path / "a", "a.gcode");
        REQUIRE(reg.ok);
        REQUIRE(reg.url_path.compare(0, 5, "/cap/") == 0);
        const std::string token = reg.url_path.substr(5, reg.url_path.find('/', 5) - 5);
        CHECK(token.size() == 64);
        CHECK(token.find_first_not_of("0123456789abcdef") == std::string::npos);
        tokens.insert(token);
    }
    CHECK(tokens.size() == 200);
    // Not a randomness test: only that the hex is not degenerate (every nibble value shows up across 200 tokens).
    std::set<char> seen;
    for (const auto& t : tokens) seen.insert(t.begin(), t.end());
    CHECK(seen.size() == 16);
}

TEST_CASE("capability: snapshot is private (dir 0700, file 0600) and a separate copy", "[PreprintCapability]")
{
    Fixture f;
    const fs::path snap = f.store->snapshot_path(f.reg.url_path);
    CHECK(snap != f.source);
#ifndef _WIN32
    struct stat st{};
    REQUIRE(::stat(snap.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);
    REQUIRE(::stat(snap.parent_path().c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0700);
#endif
}

TEST_CASE("capability: registration refuses bad names, empty, missing and oversize input", "[PreprintCapability]")
{
    TempDir tmp;
    write_file(tmp.path / "a", "x");
    write_file(tmp.path / "empty", "");
    PreprintCapabilityStore store;
    for (const char* bad : {"", ".", "..", "a/b", "a\\b", "a%2fb", "a?b", "a#b", "a b", "a\n.gcode"})
        CHECK_FALSE(store.register_file(tmp.path / "a", bad).ok);
    CHECK_FALSE(store.register_file(tmp.path / "a", std::string(PreprintCapabilityStore::max_display_name + 1, 'a')).ok);
    CHECK(store.register_file(tmp.path / "a", std::string(PreprintCapabilityStore::max_display_name, 'a')).ok);
    CHECK_FALSE(store.register_file(tmp.path / "missing", "m.gcode").ok);
    CHECK_FALSE(store.register_file(tmp.path / "empty", "e.gcode").ok);
    CHECK_FALSE(store.register_file(tmp.path, "dir.gcode").ok);
    CHECK(PreprintCapabilityStore::max_snapshot_bytes > 0);
}

TEST_CASE("without a registered capability store WebPageServer still has no file or POST endpoint", "[PreprintCapability]")
{
    WebPageServer s(bundle_root(), real_csp()); // no set_capability_store()
    REQUIRE(s.start());
    CHECK(get(s, "/cap/" + std::string(64, 'a') + "/plate.gcode").status == 404);
    for (const char* m : {"POST", "PUT", "DELETE", "PATCH"}) {
        Opts o; o.method = m;
        INFO(m);
        CHECK(get(s, "/web/flutter_web/index.html", o).status == 405);
        CHECK(get(s, "/cap/" + std::string(64, 'a') + "/plate.gcode", o).status == 405);
    }
    CHECK(get(s, "/upload").status == 404);

    // A store with no entry registered adds nothing either: everything under /cap/ is 404, and non-GET stays refused.
    WebPageServer s2(bundle_root(), real_csp());
    REQUIRE(s2.set_capability_store(std::make_shared<PreprintCapabilityStore>()));
    REQUIRE(s2.start());
    CHECK(get(s2, "/cap/" + std::string(64, 'a') + "/plate.gcode").status == 404);
    Opts post; post.method = "POST";
    CHECK(get(s2, "/web/flutter_web/index.html", post).status == 405);
    CHECK_FALSE(s2.set_capability_store(nullptr)); // not changeable while running
}
