// Loopback-socket tests for the direct Spoolman transport (#107): a real Http request to a listener on 127.0.0.1 (ephemeral port).
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>

#include "libslic3r/Spoolman/SpoolmanTransport.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/SpoolmanDirect.hpp"

using namespace Slic3r;
using namespace Slic3r::Spoolman;
namespace net = boost::asio;
using tcp     = net::ip::tcp;

namespace {

struct Reply
{
    int         status = 200;
    std::string location;
    std::string body;
};

class LoopbackServer
{
public:
    // handler(target, port) -> reply. Every raw request (head plus whatever followed) is recorded.
    explicit LoopbackServer(std::function<Reply(const std::string &, int)> handler)
        : m_handler(std::move(handler)), m_acceptor(m_ioc, tcp::endpoint(net::ip::make_address("127.0.0.1"), 0))
    {
        m_thread = std::thread([this] { loop(); });
    }
    ~LoopbackServer()
    {
        m_stopping = true;
        boost::system::error_code ec;
        tcp::socket               wake{m_ioc};
        wake.connect(m_acceptor.local_endpoint(), ec);
        m_thread.join();
    }
    int         port() const { return m_acceptor.local_endpoint().port(); }
    std::string base() const { return "http://127.0.0.1:" + std::to_string(port()); }
    std::vector<std::string> requests()
    {
        std::lock_guard<std::mutex> l(m_mutex);
        return m_requests;
    }

private:
    void loop()
    {
        while (!m_stopping) {
            tcp::socket               s{m_ioc};
            boost::system::error_code ec;
            m_acceptor.accept(s, ec);
            if (ec || m_stopping)
                continue;
            net::streambuf buf;
            net::read_until(s, buf, "\r\n\r\n", ec);
            if (ec)
                continue;
            std::string raw(net::buffers_begin(buf.data()), net::buffers_end(buf.data()));
            { std::lock_guard<std::mutex> l(m_mutex); m_requests.push_back(raw); }
            const size_t a = raw.find(' '), b = raw.find(' ', a + 1);
            const Reply  r = m_handler(raw.substr(a + 1, b - a - 1), port());
            std::string  out = "HTTP/1.1 " + std::to_string(r.status) + " X\r\n";
            if (!r.location.empty() || r.status / 100 == 3)
                out += "Location: " + r.location + "\r\n";
            out += "Content-Length: " + std::to_string(r.body.size()) + "\r\nConnection: close\r\n\r\n" + r.body;
            net::write(s, net::buffer(out), ec);
            s.shutdown(tcp::socket::shutdown_both, ec);
        }
    }

    std::function<Reply(const std::string &, int)> m_handler;
    net::io_context                                m_ioc;
    tcp::acceptor                                  m_acceptor;
    std::thread                                    m_thread;
    std::atomic<bool>                              m_stopping{false};
    std::mutex                                     m_mutex;
    std::vector<std::string>                       m_requests;
};

std::string lower(std::string s)
{
    for (char &c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct ExtraHeadersScope
{
    std::map<std::string, std::string> saved = Http::get_extra_headers();
    ExtraHeadersScope() { Http::set_extra_headers({{"X-BBL-OS-Type", "macos"}, {"X-BBL-Client-Name", "OrcaSlicer"}, {"X-Test-Secret", "s3cret"}}); }
    ~ExtraHeadersScope() { Http::set_extra_headers(saved); }
};

} // namespace

TEST_CASE("Spoolman direct sends no global extra header, no body and no credentials", "[Spoolman][SpoolmanDirect]")
{
    ExtraHeadersScope extra;
    LoopbackServer    server([](const std::string &, int) { return Reply{200, "", "[]"}; });

    std::string location, body;
    CHECK(direct_http_fetch(server.base() + "/api/v1/vendor", location, body) == 200);
    CHECK(body == "[]");
    const auto reqs = server.requests();
    REQUIRE(reqs.size() == 1);
    const std::string raw = lower(reqs[0]);
    INFO(reqs[0]);
    CHECK(boost::starts_with(reqs[0], "GET /api/v1/vendor HTTP/1.1\r\n"));
    for (const char *h : {"x-bbl", "x-test-secret", "authorization", "cookie", "content-length", "content-type", "transfer-encoding"})
        CHECK(raw.find(h) == std::string::npos);
    CHECK(boost::ends_with(reqs[0], "\r\n\r\n")); // nothing after the head: no body
}

TEST_CASE("Spoolman direct catalog goes out as three GETs with the hop-0 query", "[Spoolman][SpoolmanDirect]")
{
    ExtraHeadersScope extra;
    LoopbackServer    server([](const std::string &, int) { return Reply{200, "", "[]"}; });
    fetch_catalog(make_direct_http_get(server.base(), &direct_http_fetch));
    const auto reqs = server.requests();
    REQUIRE(reqs.size() == 3);
    CHECK(boost::starts_with(reqs[0], "GET /api/v1/vendor HTTP/1.1\r\n"));
    CHECK(boost::starts_with(reqs[1], "GET /api/v1/filament HTTP/1.1\r\n"));
    CHECK(boost::starts_with(reqs[2], "GET /api/v1/spool?allow_archived=false HTTP/1.1\r\n"));
    for (const auto &r : reqs)
        CHECK(lower(r).find("x-bbl") == std::string::npos);
}

// Characterisation of the bug this replaced: Http::remove_header only appends a blank "Name:" entry; libcurl still sends the valued one.
TEST_CASE("Http::remove_header does not stop an extra header (why SpoolmanDirect uses headers_clear)", "[.][SpoolmanDirect]")
{
    ExtraHeadersScope extra;
    LoopbackServer    server([](const std::string &, int) { return Reply{200, "", "[]"}; });
    auto              http = Http::get(server.base() + "/x");
    http.remove_header("X-BBL-OS-Type").perform_sync();
    INFO(server.requests().at(0));
    CHECK(lower(server.requests().at(0)).find("x-bbl-os-type: macos") != std::string::npos);
}

TEST_CASE("Spoolman direct reports a 3xx status and its Location", "[Spoolman][SpoolmanDirect]")
{
    for (int status : {301, 302, 307, 308}) {
        LoopbackServer server([status](const std::string &, int) { return Reply{status, "/api/v1/filament", "moved"}; });
        std::string    location, body;
        INFO(status);
        CHECK(direct_http_fetch(server.base() + "/api/v1/vendor", location, body) == status);
        CHECK(location == "/api/v1/filament");
        CHECK(server.requests().size() == 1); // curl did not follow it
    }
}

TEST_CASE("Spoolman direct redirects over a real socket", "[Spoolman][SpoolmanDirect]")
{
    const auto run = [](const std::string &target_for_vendor, int &status, std::vector<std::string> &reqs, int code = 302, const char *loop_path = nullptr) {
        LoopbackServer server([&](const std::string &t, int port) {
            if (t == "/api/v1/vendor") {
                std::string loc = target_for_vendor;
                for (size_t p; (p = loc.find("{PORT}")) != std::string::npos;)
                    loc.replace(p, 6, std::to_string(port));
                return Reply{code, loc, ""};
            }
            if (loop_path && t == loop_path)
                return Reply{code, loop_path, ""};
            return Reply{200, "", "[1]"};
        });
        std::string body;
        status = make_direct_http_get(server.base(), &direct_http_fetch)("/v1/vendor", "", body);
        reqs   = server.requests();
        if (status == 200)
            CHECK(body == "[1]");
    };

    int                      status = 0;
    std::vector<std::string> reqs;
    for (int code : {301, 302, 307, 308}) {
        INFO(code);
        run("/api/v1/filament", status, reqs, code);
        CHECK(status == 200);
        CHECK(reqs.size() == 2);
        run("http://127.0.0.1:{PORT}/api/v1/filament", status, reqs, code);
        CHECK(status == 200);
        CHECK(reqs.size() == 2);
    }
    SECTION("refused targets: one request, the 3xx comes back")
    {
        for (const char *loc : {"http://other.example/api/v1/vendor", "http://127.0.0.1:1/api/v1/vendor", "http://localhost:{PORT}/api/v1/vendor",
                                "api/v1/filament", "", "//127.0.0.1:{PORT}/api/v1/filament",
                                "/printer/gcode/script?script=M112", "/printer/emergency_stop", "/api/v1/spool/1", "/api/v1/spool", "/api/v1/vendor?x=1",
                                "/api/v1/export/spools", "/api/v1/filament/../../printer/info"}) {
            INFO(loc);
            run(loc, status, reqs);
            CHECK(status == 302);
            CHECK(reqs.size() == 1);
        }
    }
    SECTION("a loop stops after the original and 2 hops")
    {
        run("/api/v1/filament", status, reqs, 302, "/api/v1/filament");
        CHECK(status == 302);
        CHECK(reqs.size() == 3);
    }
}
