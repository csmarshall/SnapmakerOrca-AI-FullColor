#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>
#include <nlohmann/json.hpp>

#include "slic3r/GUI/SnapmakerDeviceBridge.hpp"
#include "slic3r/GUI/SnapmakerPreprintSession.hpp"
#include "slic3r/Utils/MoonrakerPreprintPrinter.hpp"
#include "slic3r/Utils/MoonrakerPrinterAgent.hpp"

// S4c: the real SnapmakerPreprintPrinter adapter against a LOOPBACK fake Moonraker (127.0.0.1, no printer, no network). What this shows: which
// requests the adapter sends, in which order, with which bytes; that every read is fresh; that every failure reads as unknown / refused; that
// the pin never comes from print_host. What it cannot show: how real Moonraker/Klipper/Snapmaker firmware answers (the fake's JSON shapes are
// our reading of the Moonraker docs and of the agent's existing parsers), that the printer really starts the file, or that
// machine.system_info carries product_info.serial_number on every firmware. Only S7 (live, idle printer, Charles's OK) can show that.

using namespace Slic3r;
using namespace Slic3r::GUI;
using nlohmann::json;
using Known = SnapmakerPreprintPrinter::Known;
namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
using tcp       = net::ip::tcp;

namespace {

struct Req
{
    std::string method, target, body;
    std::string str() const { return method + " " + target + (body.empty() ? "" : " " + body); }
};
struct Resp
{
    int         status = 200;
    std::string body   = "{}";
    bool        hang   = false; // never answers until the server is released
};

// A one-shot-per-connection HTTP server on 127.0.0.1. The handler decides each answer; every request is recorded in order.
class FakeMoonraker
{
public:
    std::function<Resp(const Req&)> handler = [](const Req&) { return Resp{404, "{}"}; };

    FakeMoonraker() { m_thread = std::thread([this] { loop(); }); }
    ~FakeMoonraker()
    {
        {
            std::lock_guard<std::mutex> l(m_m);
            m_released = true;
            m_cv.notify_all();
        }
        m_stopping = true;
        net::io_context   ioc;
        tcp::socket       wake{ioc};
        beast::error_code ec;
        wake.connect(m_acceptor.local_endpoint(), ec);
        m_thread.join();
        for (auto& t : m_conns) t.join();
    }
    std::string port() const { return std::to_string(m_acceptor.local_endpoint().port()); }
    std::string base_url() const { return "http://127.0.0.1:" + port(); }
    std::vector<Req> requests()
    {
        std::lock_guard<std::mutex> l(m_m);
        return m_reqs;
    }
    std::vector<std::string> lines()
    {
        std::vector<std::string> o;
        for (const auto& r : requests()) o.push_back(r.str());
        return o;
    }
    void clear()
    {
        std::lock_guard<std::mutex> l(m_m);
        m_reqs.clear();
    }

private:
    void loop()
    {
        for (;;) {
            auto              ioc = std::make_unique<net::io_context>();
            tcp::socket       s{*ioc};
            beast::error_code ec;
            m_acceptor.accept(s, ec);
            if (ec || m_stopping) return;
            m_conns.emplace_back([this, ioc = std::move(ioc), s = std::move(s)]() mutable { serve(std::move(s)); });
        }
    }
    void serve(tcp::socket s)
    {
        beast::flat_buffer               buf;
        http::request<http::string_body> rq;
        beast::error_code                ec;
        http::read(s, buf, rq, ec);
        if (ec) return;
        Req r{std::string(rq.method_string()), std::string(rq.target()), rq.body()};
        {
            std::lock_guard<std::mutex> l(m_m);
            m_reqs.push_back(r);
        }
        const Resp a = handler(r);
        if (a.hang) {
            std::unique_lock<std::mutex> l(m_m);
            m_cv.wait(l, [this] { return m_released; });
            return; // hang up without an answer
        }
        http::response<http::string_body> resp{static_cast<http::status>(a.status), rq.version()};
        resp.body() = a.body;
        resp.keep_alive(false);
        resp.prepare_payload();
        http::write(s, resp, ec);
        s.shutdown(tcp::socket::shutdown_both, ec);
    }

    net::io_context          m_ioc;
    tcp::acceptor            m_acceptor{m_ioc, {net::ip::make_address("127.0.0.1"), 0}};
    std::atomic<bool>        m_stopping{false};
    std::thread              m_thread;
    std::vector<std::thread> m_conns;
    std::mutex               m_m;
    std::condition_variable  m_cv;
    bool                     m_released = false;
    std::vector<Req>         m_reqs;
};

// The agent aimed at a fake. NOT through connect_printer or the app's printer preset (no app here).
class TestAgent : public MoonrakerPrinterAgent
{
public:
    TestAgent() : MoonrakerPrinterAgent("") {}
    void aim(const std::string& base_url, const std::string& dev_id = "U1-dev")
    {
        device_info.base_url = base_url;
        device_info.dev_id   = dev_id;
        device_info.dev_ip   = "preset-host.invalid"; // must never reach the pin
    }
};

json sysinfo(const std::string& serial) { return {{"result", {{"system_info", {{"product_info", {{"serial_number", serial}}}}}}}}; }
json stats(const std::string& state) { return {{"result", {{"status", {{"print_stats", {{"state", state}}}}}}}}; }
json meta(const std::string& file, json size) { return {{"result", {{"filename", file}, {"size", size}}}}; }
json help(std::vector<std::string> cmds)
{
    json r = json::object();
    for (auto& c : cmds) r[c] = "help";
    return {{"result", r}};
}

struct Rig
{
    FakeMoonraker                          fake;
    std::shared_ptr<TestAgent>             agent = std::make_shared<TestAgent>();
    std::shared_ptr<MoonrakerPreprintPrinter> sp = std::make_shared<MoonrakerPreprintPrinter>(agent);
    MoonrakerPreprintPrinter&              printer = *sp;
    std::string                            serial = "SN-U1-001", state = "standby", metadata_file = "plate_1.gcode";
    json                                   size   = 1234;
    std::vector<std::string>               commands = {"SET_PRINT_EXTRUDER_MAP", "SET_PRINT_USED_EXTRUDERS", "SET_PRINT_PREFERENCES"};
    int                                    script_status = 200;
    Rig()
    {
        agent->aim(fake.base_url());
        fake.handler = [this](const Req& r) -> Resp {
            if (r.target == "/machine/system_info") return {200, sysinfo(serial).dump()};
            if (r.target == "/printer/objects/query?print_stats") return {200, stats(state).dump()};
            if (r.target.rfind("/server/files/metadata?filename=", 0) == 0) return {200, meta(metadata_file, size).dump()};
            if (r.target == "/printer/gcode/help") return {200, help(commands).dump()};
            if (r.target == "/printer/gcode/script") return {script_status, "{\"result\":\"ok\"}"};
            return {404, "{}"};
        };
        printer.bind_current(); // the dialog pins the connected device at Open
        fake.clear();
    }
};
} // namespace

TEST_CASE("preprint adapter: connected_pin is the agent's device and a live serial, never print_host", "[preprint_adapter]")
{
    Rig r;
    const SnapmakerPreprintPin p = r.printer.connected_pin();
    CHECK(p.host == "127.0.0.1"); // from the connected base URL, not device_info.dev_ip ("preset-host.invalid")
    CHECK(p.port == r.fake.port());
    CHECK(p.serial == "SN-U1-001");
    CHECK(r.fake.lines() == std::vector<std::string>{"GET /machine/system_info"});

    SECTION("the serial is read live each call: a swapped printer behind the same address changes the pin")
    {
        r.serial = "SN-OTHER";
        CHECK(r.printer.connected_pin().serial == "SN-OTHER");
        CHECK(r.printer.connected_pin() != p);
        CHECK(r.fake.requests().size() == 3);
    }
    SECTION("an unreadable serial leaves it empty (the gate then refuses): HTTP error, malformed JSON, missing fields, wrong type")
    {
        for (int mode = 0; mode < 5; ++mode) {
            r.fake.handler = [mode](const Req&) -> Resp {
                switch (mode) {
                case 0: return {500, "{}"};
                case 1: return {200, "not json"};
                case 2: return {200, "{}"};
                case 3: return {200, R"({"result":{"system_info":{"product_info":{}}}})"};
                default: return {200, R"({"result":{"system_info":{"product_info":{"serial_number":7}}}})"};
                }
            };
            const auto q = r.printer.connected_pin();
            INFO("mode " << mode);
            CHECK(q.serial.empty());
            CHECK(q != p);
        }
    }
    SECTION("nothing connected: no base URL, no request at all, an all-empty pin")
    {
        r.agent->aim("");
        r.fake.clear();
        const auto q = r.printer.connected_pin();
        CHECK(q.host.empty());
        CHECK(q.port.empty());
        CHECK(q.serial.empty());
        CHECK(r.fake.requests().empty());
    }
    SECTION("an unreachable printer (connection refused) leaves the serial empty")
    {
        r.agent->aim("http://127.0.0.1:1");
        const auto q = r.printer.connected_pin();
        CHECK(q.host == "127.0.0.1");
        CHECK(q.serial.empty());
    }
}

TEST_CASE("preprint adapter: endpoint_of canonicalises the connected URL the same way every call", "[preprint_adapter]")
{
    using P = MoonrakerPreprintPrinter;
    CHECK(P::endpoint_of("http://U1.Local:7125").host == "u1.local");
    CHECK(P::endpoint_of("http://U1.Local:7125").port == "7125");
    CHECK(P::endpoint_of("http://u1.local").port == "80");
    CHECK(P::endpoint_of("https://u1.local").port == "443");
    CHECK(P::endpoint_of("http://[fe80::1]:7125").host == "[fe80::1]");
    CHECK(P::endpoint_of("http://[fe80::1]:7125").port == "7125");
    CHECK(P::endpoint_of("http://[fe80::1]").port == "80");
    CHECK(P::endpoint_of("http://u1.local:7125/path").port == "7125");
    for (const char* bad : {"", "u1.local", "ftp://u1.local", "http://", "http://:7125", "http://u1.local:", "http://u1.local:7x", "http://user@u1.local"})
        CHECK(P::endpoint_of(bad).host.empty());
}

TEST_CASE("preprint adapter: print_state is a fresh read-only GET and failures are unknown", "[preprint_adapter]")
{
    Rig r;
    CHECK(r.printer.print_state() == std::optional<std::string>("standby"));
    r.state = "printing"; // changes between two calls are seen: nothing is cached
    CHECK(r.printer.print_state() == std::optional<std::string>("printing"));
    r.state = "paused";
    CHECK(r.printer.print_state() == std::optional<std::string>("paused"));
    CHECK(r.fake.lines() == std::vector<std::string>(3, "GET /printer/objects/query?print_stats"));

    for (int mode = 0; mode < 7; ++mode) {
        r.fake.handler = [mode](const Req&) -> Resp {
            switch (mode) {
            case 0: return {500, "{}"};
            case 1: return {404, "{}"};
            case 2: return {200, "garbage{"};
            case 3: return {200, "{}"};
            case 4: return {200, R"({"result":{"status":{}}})"};
            case 5: return {200, R"({"result":{"status":{"print_stats":{"state":5}}}})"};
            default: return {200, R"({"result":{"status":{"print_stats":{"state":""}}}})"};
            }
        };
        INFO("mode " << mode);
        CHECK_FALSE(r.printer.print_state().has_value());
    }
    r.agent->aim("http://127.0.0.1:1"); r.printer.bind_current();
    CHECK_FALSE(r.printer.print_state().has_value());
    r.agent->aim(""); r.printer.bind_current();
    CHECK_FALSE(r.printer.print_state().has_value());
}

TEST_CASE("preprint adapter: print_state gives up on a printer that never answers (timeout reads unknown)", "[preprint_adapter][slow]")
{
    Rig r;
    r.fake.handler = [](const Req&) { return Resp{200, "{}", true}; };
    const auto t0 = std::chrono::steady_clock::now();
    CHECK_FALSE(r.printer.print_state().has_value());
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(30)); // the agent's own limit is 10 s total
}

TEST_CASE("preprint adapter: remote_file_size reads server.files.metadata for exactly the pinned name", "[preprint_adapter]")
{
    Rig r;
    CHECK(r.printer.remote_file_size("plate_1.gcode") == std::optional<std::uint64_t>(1234));
    CHECK(r.fake.lines() == std::vector<std::string>{"GET /server/files/metadata?filename=plate_1.gcode"});

    r.size = 99; // fresh each call
    CHECK(r.printer.remote_file_size("plate_1.gcode") == std::optional<std::uint64_t>(99));

    SECTION("the name is URL-encoded")
    {
        r.metadata_file = "my plate-x.gcode";
        CHECK(r.printer.remote_file_size("my plate-x.gcode") == std::optional<std::uint64_t>(99));
        CHECK(r.fake.requests().back().target == "/server/files/metadata?filename=my%20plate-x.gcode");
    }
    SECTION("failure and odd answers are unknown")
    {
        for (int mode = 0; mode < 8; ++mode) {
            r.fake.handler = [mode](const Req&) -> Resp {
                switch (mode) {
                case 0: return {404, R"({"error":{"message":"Metadata not available"}})"};
                case 1: return {500, "{}"};
                case 2: return {200, "nope"};
                case 3: return {200, "{}"};
                case 4: return {200, R"({"result":{"filename":"plate_1.gcode"}})"};                 // no size
                case 5: return {200, R"({"result":{"filename":"plate_1.gcode","size":"1234"}})"};    // string size
                case 6: return {200, R"({"result":{"filename":"plate_1.gcode","size":-5}})"};        // negative
                default: return {200, R"({"result":{"filename":"other.gcode","size":1234}})"};       // an answer for another file
                }
            };
            INFO("mode " << mode);
            CHECK_FALSE(r.printer.remote_file_size("plate_1.gcode").has_value());
        }
    }
    SECTION("a name that is not a plain gcode name is never asked about")
    {
        r.fake.clear();
        for (const char* bad : {"", "a\"b.gcode", "a\\b.gcode", "../x.gcode", "/abs.gcode", "a\nb.gcode", "a#b.gcode", "a;b.gcode", "a=b.gcode", "x.cfg", "dir/"})
            CHECK_FALSE(r.printer.remote_file_size(bad).has_value());
        CHECK(r.fake.requests().empty());
    }
}

TEST_CASE("preprint adapter: command_known probe matrix", "[preprint_adapter]")
{
    Rig r;
    CHECK(r.printer.command_known("SET_PRINT_EXTRUDER_MAP") == Known::Yes);
    CHECK(r.printer.command_known("SET_PRINT_PREFERENCE") == Known::No); // registered list lacks it
    CHECK(r.fake.lines() == std::vector<std::string>(2, "GET /printer/gcode/help"));
    r.commands = {"SET_PRINT_PREFERENCE"}; // fresh: the list is read every time
    CHECK(r.printer.command_known("SET_PRINT_PREFERENCE") == Known::Yes);

    for (int mode = 0; mode < 6; ++mode) {
        r.fake.handler = [mode](const Req&) -> Resp {
            switch (mode) {
            case 0: return {404, "{}"}; // the route is absent: unknown, not "No"
            case 1: return {500, "{}"};
            case 2: return {200, "[["};
            case 3: return {200, "{}"};
            case 4: return {200, R"({"result":[]})"};
            default: return {200, R"({"result":"x"})"};
            }
        };
        INFO("mode " << mode);
        CHECK(r.printer.command_known("SET_PRINT_EXTRUDER_MAP") == Known::Unreadable);
    }
    r.agent->aim("http://127.0.0.1:1"); r.printer.bind_current();
    CHECK(r.printer.command_known("SET_PRINT_EXTRUDER_MAP") == Known::Unreadable);
    CHECK(r.printer.command_known("") == Known::Unreadable);
}

TEST_CASE("preprint adapter: send_gcode posts the script verbatim and succeeds only on a clean reply", "[preprint_adapter]")
{
    Rig r;
    const std::string script = "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=0 MAP_EXTRUDER=2\nSET_PRINT_USED_EXTRUDERS EXTRUDERS=2\n";
    CHECK(r.printer.send_gcode(script));
    REQUIRE(r.fake.requests().size() == 2); // the pin re-read (F1), then the write
    CHECK(r.fake.requests()[0].target == "/machine/system_info");
    CHECK(r.fake.requests()[1].method == "POST");
    CHECK(r.fake.requests()[1].target == "/printer/gcode/script");
    CHECK(json::parse(r.fake.requests()[1].body) == json{{"script", script}});

    r.script_status = 400;
    CHECK_FALSE(r.printer.send_gcode(script));
    r.script_status = 500;
    CHECK_FALSE(r.printer.send_gcode(script));
    CHECK_FALSE(r.printer.send_gcode(""));
    r.agent->aim("http://127.0.0.1:1"); r.printer.bind_current();
    CHECK_FALSE(r.printer.send_gcode(script));
    r.agent->aim(""); r.printer.bind_current();
    CHECK_FALSE(r.printer.send_gcode(script));
}

TEST_CASE("preprint adapter: start_print sends exactly the bare SDCARD_PRINT_FILE line and nothing else", "[preprint_adapter]")
{
    Rig r;
    CHECK(r.printer.start_print("plate_1.gcode"));
    REQUIRE(r.fake.requests().size() == 2); // the pin re-read (F1) and the start; no help probe, no map, no preferences, no metadata
    CHECK(r.fake.requests()[0].target == "/machine/system_info");
    CHECK(r.fake.requests()[1].method == "POST");
    CHECK(r.fake.requests()[1].target == "/printer/gcode/script");
    CHECK(json::parse(r.fake.requests()[1].body) == json{{"script", "SDCARD_PRINT_FILE FILENAME=\"plate_1.gcode\""}});
    CHECK(MoonrakerPreprintPrinter::start_line("My plate v2.gcode") == "SDCARD_PRINT_FILE FILENAME=\"My plate v2.gcode\"");

    r.fake.clear();
    r.script_status = 500;
    CHECK_FALSE(r.printer.start_print("plate_1.gcode"));
    r.script_status = 200;

    SECTION("names that could break out of FILENAME=\"...\" are refused without a request")
    {
        r.fake.clear();
        const std::string nul("a\0b.gcode", 9);
        for (const std::string& bad : {std::string(), std::string("a\"b.gcode"), std::string("a\\b.gcode"), std::string("a\nb.gcode"),
                                       std::string("a\rb.gcode"), std::string("a\tb.gcode"), nul, std::string("../a.gcode"),
                                       std::string("a/../b.gcode"), std::string("a'b.gcode"), std::string("a#b.gcode"), std::string("a;b.gcode"),
                                       std::string("a=b.gcode"), std::string("x.cfg"), std::string("dir/"), std::string("a//b.gcode"), std::string("noext"), std::string("caf\xc3\xa9.gcode"), std::string("/etc/a.gcode"), std::string(513, 'a')}) {
            CHECK(MoonrakerPreprintPrinter::start_line(bad).empty());
            CHECK_FALSE(r.printer.start_print(bad));
        }
        CHECK(r.fake.requests().empty());
    }
}

TEST_CASE("preprint adapter: end to end through SnapmakerPreprintSession and the pre-print bridge profile, happy path", "[preprint_adapter][SSWCP]")
{
    Rig r;
    const std::string file = "plate_1.gcode";
    r.metadata_file        = file;
    r.size                 = 1234;
    const std::vector<int> mapping = {2, 0};
    const std::vector<std::pair<std::string, int>> prefs = {{"bed_level", 1}, {"flow_calibrate", 0}};
    const std::string script = build_snapmaker_plural_preference_script(mapping, prefs);

    auto ctx     = std::make_shared<SnapmakerPreprintContext>();
    auto session = std::make_shared<SnapmakerPreprintSession>(ctx, r.sp);
    SnapmakerPreprintPlate plate;
    plate.filament_colour       = {"#FF0000", "#00FF00"};
    plate.filament_type         = {"PLA", "PETG"};
    plate.nozzle_diameter       = {0.4, 0.4, 0.4, 0.4};
    plate.filament_extruder_map = {{0, 2}, {1, 0}};
    plate.machine_model         = "Snapmaker U1";
    plate.display_name          = file;
    std::string err;
    REQUIRE(ctx->set(plate, {}, err));

    SnapmakerPreprintSession::Open o;
    o.gate.printer            = r.printer.connected_pin(); // what the dialog pins at Open
    o.gate.remote_filename    = file;
    o.gate.plate_filaments    = {0, 1};
    o.gate.extruder_count     = 4;
    o.gate.snapshot_size      = 1234;
    o.gate.configure_required = true;
    o.capability_path         = "/web/flutter_web/cap/TOKEN/plate_1.gcode";
    o.sha256_hex              = std::string(64, 'a');
    const std::uint64_t epoch = session->open(o, err);
    REQUIRE(epoch != 0);
    CHECK(o.gate.printer.serial == "SN-U1-001");

    std::vector<std::string> sent;
    std::vector<bool>        done;
    SnapmakerDeviceBridge::Options bo;
    bo.port             = 5000;
    bo.http_host        = "http://127.0.0.1:5000";
    bo.preprint         = ctx;
    bo.preprint_session = session;
    bo.preprint_epoch   = epoch;
    bo.run_script       = [&](const std::string& js) { sent.push_back(js); };
    bo.preprint_done    = [&](bool started) { done.push_back(started); };
    bo.preprint_run     = [](std::function<void()> work, std::function<void()> on_ui) { work(); on_ui(); };
    SnapmakerDeviceBridge bridge(std::move(bo));
    const std::string page = "http://127.0.0.1:5000/web/flutter_web/index.html";
    auto call = [&](const std::string& cmd, const json& params) {
        const std::size_t before = sent.size();
        bridge.on_script_message(page, json{{"header", {{"id", 1}}}, {"payload", {{"cmd", cmd}, {"params", params}}}}.dump());
        REQUIRE(sent.size() == before + 1);
        const std::string prefix = "window.postMessage(JSON.stringify(", suffix = "), '*');";
        return json::parse(sent.back().substr(prefix.size(), sent.back().size() - prefix.size() - suffix.size()))["payload"];
    };

    r.fake.clear();
    CHECK(call("sw_GetActiveFile", json::object())["code"] == 200);
    CHECK(r.fake.requests().empty()); // reading the context touches no printer

    r.fake.clear();
    CHECK(call("sw_SendGCodes", {{"script", script}})["code"] == 200);
    const std::vector<std::string> sent_requests = r.fake.lines();
    REQUIRE(sent_requests.size() == 7);
    CHECK(sent_requests[0] == "GET /machine/system_info");
    CHECK(sent_requests[1] == "GET /printer/objects/query?print_stats");
    CHECK(sent_requests[2] == "GET /printer/gcode/help");
    CHECK(sent_requests[3] == "GET /printer/gcode/help");
    CHECK(sent_requests[4] == "GET /printer/gcode/help");
    CHECK(sent_requests[5] == "GET /machine/system_info"); // F1: the pin is re-read right before the write
    CHECK(sent_requests[6] == "POST /printer/gcode/script " + json{{"script", script}}.dump());

    r.fake.clear();
    CHECK(call("sw_MachinePrintStart", {{"filename", file}})["code"] == 200);
    CHECK(r.fake.lines() == std::vector<std::string>{
                                "GET /machine/system_info",
                                "GET /printer/objects/query?print_stats",
                                "GET /server/files/metadata?filename=plate_1.gcode",
                                "GET /machine/system_info", // F1: re-read right before the write
                                "POST /printer/gcode/script " + json{{"script", "SDCARD_PRINT_FILE FILENAME=\"plate_1.gcode\""}}.dump()});

    CHECK(call("sw_SetFilamentMappingComplete", {{"status", "success"}, {"message", "ok"}})["code"] == 200);
    CHECK(done == std::vector<bool>{true});
}

TEST_CASE("preprint adapter: end to end, a printing printer or a swapped serial refuses before any write", "[preprint_adapter][SSWCP]")
{
    Rig r;
    auto ctx     = std::make_shared<SnapmakerPreprintContext>();
    auto session = std::make_shared<SnapmakerPreprintSession>(ctx, r.sp);
    const std::string file = "plate_1.gcode";
    SnapmakerPreprintSession::Open o;
    o.gate.printer         = r.printer.connected_pin();
    o.gate.remote_filename = file;
    o.gate.plate_filaments = {0};
    o.gate.extruder_count  = 4;
    o.gate.snapshot_size   = 1234;
    o.capability_path      = "/web/flutter_web/cap/T/plate_1.gcode";
    o.sha256_hex           = std::string(64, 'b');
    std::string err;
    const std::uint64_t epoch = session->open(o, err);
    REQUIRE(epoch != 0);
    auto posts = [&] {
        int n = 0;
        for (auto& q : r.fake.requests()) n += q.method == "POST";
        return n;
    };
    r.fake.clear();
    r.state = "printing";
    CHECK_FALSE(session->start(epoch, {{"filename", file}}).ok);
    CHECK(posts() == 0);
    r.state  = "standby";
    r.serial = "SN-SOMEONE-ELSE"; // another printer answers on the same address
    CHECK_FALSE(session->start(epoch, {{"filename", file}}).ok);
    CHECK(posts() == 0);
    r.serial = "SN-U1-001";
    r.size   = 5; // the printer's copy is another size
    CHECK_FALSE(session->start(epoch, {{"filename", file}}).ok);
    CHECK(posts() == 0);
}

TEST_CASE("preprint adapter: F1 the write goes only to the pinned device; a switch of the connected device refuses it", "[preprint_adapter]")
{
    Rig a; // the pinned printer
    Rig b; // another printer the agent could be switched to
    auto posts = [](Rig& r) { int n = 0; for (auto& q : r.fake.requests()) n += q.method == "POST"; return n; };
    b.serial = "SN-B";
    const std::string script = "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=0 MAP_EXTRUDER=1\nSET_PRINT_USED_EXTRUDERS EXTRUDERS=1\n";

    SECTION("switched before the call: refused, nothing lands on either printer")
    {
        a.agent->aim(b.fake.base_url());
        CHECK_FALSE(a.printer.send_gcode(script));
        CHECK_FALSE(a.printer.start_print("plate_1.gcode"));
        CHECK(posts(a) == 0);
        CHECK(posts(b) == 0);
        CHECK(a.printer.connected_pin().serial == "SN-B"); // the gate sees the new device and refuses too
    }
    SECTION("same address, another serial: refused")
    {
        a.serial = "SN-SWAPPED";
        CHECK_FALSE(a.printer.send_gcode(script));
        CHECK_FALSE(a.printer.start_print("plate_1.gcode"));
        CHECK(posts(a) == 0);
    }
    SECTION("switched DURING the call (after the identity check answered): the write still goes to the pinned device, never the new one")
    {
        auto inner = a.fake.handler;
        a.fake.handler = [&](const Req& r) -> Resp {
            Resp x = inner(r);
            if (r.target == "/machine/system_info") a.agent->aim(b.fake.base_url()); // the agent moves on right after the check
            return x;
        };
        CHECK(a.printer.start_print("plate_1.gcode"));
        CHECK(posts(a) == 1);
        CHECK(posts(b) == 0);
        CHECK(json::parse(a.fake.requests().back().body)["script"] == "SDCARD_PRINT_FILE FILENAME=\"plate_1.gcode\"");
    }
    SECTION("reads also stay on the pinned device after a switch")
    {
        a.agent->aim(b.fake.base_url());
        b.state = "printing";
        CHECK(a.printer.print_state() == std::optional<std::string>("standby"));
        CHECK(b.fake.requests().empty());
    }
    SECTION("unbound (never pinned, or the pin could not be read) fails closed")
    {
        Rig c;
        c.agent->aim("http://127.0.0.1:1");
        c.printer.bind_current();
        CHECK_FALSE(c.printer.send_gcode(script));
        CHECK_FALSE(c.printer.start_print("plate_1.gcode"));
        CHECK_FALSE(c.printer.print_state().has_value());
        CHECK(c.printer.command_known("SET_PRINT_EXTRUDER_MAP") == Known::Unreadable);
        CHECK_FALSE(c.printer.remote_file_size("plate_1.gcode").has_value());
        CHECK(posts(c) == 0);
    }
}

TEST_CASE("preprint adapter: F6 a closed, stale or malformed request causes no printer traffic", "[preprint_adapter][SSWCP]")
{
    Rig r;
    auto ctx     = std::make_shared<SnapmakerPreprintContext>();
    auto session = std::make_shared<SnapmakerPreprintSession>(ctx, r.sp);
    SnapmakerPreprintSession::Open o;
    o.gate.printer         = r.printer.bind_current();
    o.gate.remote_filename = "plate_1.gcode";
    o.gate.plate_filaments = {0, 1};
    o.gate.extruder_count  = 4;
    o.gate.snapshot_size   = 1234;
    o.capability_path      = "/web/flutter_web/cap/T/plate_1.gcode";
    o.sha256_hex           = std::string(64, 'b');
    std::string err;
    const std::uint64_t epoch = session->open(o, err);
    REQUIRE(epoch != 0);
    const std::string good = build_snapmaker_plural_preference_script({2, 0}, {});
    r.fake.clear();
    CHECK_FALSE(session->start(epoch, {{"filename", "other.gcode"}}).ok);          // not the pinned file
    CHECK_FALSE(session->configure(epoch, {{"script", "G28"}}).ok);                // not an accepted script
    CHECK_FALSE(session->configure(epoch + 1, {{"script", good}}).ok);             // stale epoch
    CHECK_FALSE(session->start(epoch + 1, {{"filename", "plate_1.gcode"}}).ok);
    session->finish();
    CHECK_FALSE(session->configure(epoch, {{"script", good}}).ok);                 // closed
    CHECK_FALSE(session->start(epoch, {{"filename", "plate_1.gcode"}}).ok);
    CHECK(r.fake.requests().empty());
}

TEST_CASE("preprint session: F5 remote file names are an allowlist with a .gcode suffix", "[preprint_adapter][SSWCP]")
{
    Rig r;
    for (const std::string& bad : {std::string("a#b.gcode"), std::string("a;b.gcode"), std::string("a=b.gcode"), std::string("x.cfg"), std::string("dir/"),
                                   std::string("a\"b.gcode"), std::string("a\\b.gcode"), std::string("../a.gcode"), std::string("a b"), std::string("plate.gcode.cfg"),
                                   std::string(".gcode"), std::string("/a.gcode")}) {
        auto ctx     = std::make_shared<SnapmakerPreprintContext>();
        auto session = std::make_shared<SnapmakerPreprintSession>(ctx, r.sp);
        SnapmakerPreprintSession::Open o;
        o.gate.printer         = r.printer.connected_pin();
        o.gate.remote_filename = bad;
        o.gate.plate_filaments = {0};
        o.gate.extruder_count  = 4;
        o.gate.snapshot_size   = 1;
        o.capability_path      = "/web/flutter_web/cap/T/x.gcode";
        o.sha256_hex           = std::string(64, 'b');
        std::string err;
        INFO("name " << bad);
        CHECK(session->open(o, err) == 0);
    }
    CHECK(snapmaker_preprint_filename_ok("sub dir/My_plate-1.v2.gcode"));
}
