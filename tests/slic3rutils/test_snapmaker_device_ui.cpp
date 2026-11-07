#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#include <map>
#include <fstream>
#include <sstream>
#include <set>
#include <boost/filesystem.hpp>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <vector>

#include "slic3r/GUI/SnapmakerDeviceBridge.hpp"
#include "slic3r/Utils/CrealityPrintAgent.hpp"
#include "slic3r/Utils/MoonrakerPrinterAgent.hpp"
#include "slic3r/Utils/OrcaPrinterAgent.hpp"
#include "slic3r/Utils/QidiPrinterAgent.hpp"
#include "slic3r/Utils/SnapmakerPrinterAgent.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;
using nlohmann::json;

TEST_CASE("device_ui: only the Snapmaker agent declares the Snapmaker UI", "[SSWCP][device_ui]")
{
    CHECK(SnapmakerPrinterAgent("").device_ui() == DeviceUi::SnapmakerWeb);
    // Every other existing agent keeps the native Device tab.
    CHECK(MoonrakerPrinterAgent("").device_ui() == DeviceUi::Native);
    CHECK(QidiPrinterAgent("").device_ui() == DeviceUi::Native);
    CHECK(CrealityPrintAgent("").device_ui() == DeviceUi::Native);
    CHECK(OrcaPrinterAgent("").device_ui() == DeviceUi::Native);
    // The interface default, via an agent that does not override it.
    CHECK(DeviceUi::Native != DeviceUi::SnapmakerWeb);
}

TEST_CASE("page origin check accepts only the page server's own origin", "[SSWCP][device_ui]")
{
    const uint16_t p = 40123;
    CHECK(snapmaker_is_page_origin("http://127.0.0.1:40123/web/flutter_web/index.html?wps_token=x", p));
    CHECK(snapmaker_is_page_origin("http://127.0.0.1:40123", p));
    CHECK(snapmaker_is_page_origin("http://127.0.0.1:40123/", p));
    for (const char* bad : {"", "about:blank", "https://127.0.0.1:40123/", "http://127.0.0.1:40124/", "http://127.0.0.1:4012/",
                            "http://127.0.0.1:401230/", "http://127.0.0.1:40123@evil.example/", "http://127.0.0.1:40123.evil.example/",
                            "http://localhost:40123/", "http://evil.example/http://127.0.0.1:40123/", "file:///etc/passwd",
                            "HTTP://127.0.0.1:40123/", " http://127.0.0.1:40123/"}) {
        CAPTURE(bad);
        CHECK_FALSE(snapmaker_is_page_origin(bad, p));
    }
    CHECK_FALSE(snapmaker_is_page_origin("http://127.0.0.1:0/", 0));
}

TEST_CASE("user agent carries the SM-Slicer token first", "[SSWCP][device_ui]")
{
    const std::string ua = snapmaker_user_agent("2.5.0", true);
    CHECK(ua.rfind("SM-Slicer/v2.5.0 (dark) Mozilla/5.0 (", 0) == 0);
    CHECK(snapmaker_user_agent("2.5.0", false).find("(light)") != std::string::npos);
}

namespace {
struct BridgeHarness
{
    std::vector<std::string>          sent;
    std::vector<SnapmakerDeviceInfo>  devices;
    // Machine-state source: queries wait here until the test answers them (the real source is off-thread and late).
    struct PendingQuery { std::vector<std::string> objects; std::function<void(bool, json)> done; };
    std::vector<PendingQuery>         queries;
    bool                              have_source = true;
    std::vector<SnapmakerTab>         tabs;
    bool                              tab_ok = true;
    // Read-only resource source: path -> body (absent = failure). Every request is recorded.
    std::map<std::string, std::string> resources;
    std::vector<std::string>          fetched;
    // With hold_fetch set a fetch is not answered until the test calls release_fetches(); the real source is off-thread.
    bool                              hold_fetch = false;
    std::vector<std::function<void()>> held;
    // Camera monitor source: every request is recorded; it answers at once with camera_ok/camera_result, or waits in
    // camera_pending (the real source is off-thread).
    struct CameraCall { bool start; json params; };
    std::vector<CameraCall>           camera_calls;
    bool                              camera_ok = true;
    bool                              hold_camera = false;
    json                              camera_result = {{"state", "success"}, {"url", "/files/camera/monitor.jpg"}, {"pw", "ab"}, {"salt", "cd"}, {"iterations", 1204}};
    std::vector<std::function<void(bool, json)>> camera_pending;
    // Control source (#75): every forwarded RPC is recorded; it answers at once with control_ok, or waits in control_pending.
    struct ControlCall { std::string method; json params; };
    std::vector<ControlCall>          control_calls;
    bool                              control_ok = true;
    bool                              hold_control = false;
    std::vector<std::function<void(bool, json)>> control_pending;
    SnapmakerDeviceBridge             bridge;

    BridgeHarness()
        : bridge([this] {
              SnapmakerDeviceBridge::Options o;
              o.port       = 5000;
              o.http_host  = "http://127.0.0.1:5000";
              o.run_script = [this](const std::string& js) { sent.push_back(js); };
              o.devices    = [this] { return devices; };
              o.query_state = [this](const std::vector<std::string>& objs, std::function<void(bool, json)> done) {
                  if (!have_source) { done(false, json::object()); return; }
                  queries.push_back({objs, std::move(done)});
              };
              o.switch_tab = [this](SnapmakerTab t) { tabs.push_back(t); return tab_ok; };
              o.fetch_resource = [this](const std::string& path, std::size_t, std::function<void(bool, std::string)> done) {
                  fetched.push_back(path);
                  auto answer = [this, path, done = std::move(done)] {
                      auto it = resources.find(path);
                      if (it == resources.end()) done(false, std::string()); else done(true, it->second);
                  };
                  if (hold_fetch) held.push_back(std::move(answer)); else answer();
              };
              o.camera_monitor = [this](bool start, const json& params, std::function<void(bool, json)> done) {
                      camera_calls.push_back({start, params});
                      if (hold_camera) camera_pending.push_back(std::move(done)); else done(camera_ok, camera_ok ? camera_result : json::object());
                  };
              o.control = [this](const std::string& method, const json& params, std::function<void(bool, json)> done) {
                      control_calls.push_back({method, params});
                      if (hold_control) control_pending.push_back(std::move(done)); else done(control_ok, control_ok ? json{{"state", "success"}} : json::object());
                  };
              return o;
          }())
    {}
    void release_fetches()
    {
        auto pending = std::move(held);
        held.clear();
        for (auto& f : pending) f();
    }
    json reply(std::size_t n) const
    {
        const std::string prefix = "window.postMessage(JSON.stringify(", suffix = "), '*');";
        return json::parse(sent.at(n).substr(prefix.size(), sent.at(n).size() - prefix.size() - suffix.size()));
    }
    static std::string msg(const std::string& cmd, const std::string& event_id = "")
    {
        json p = {{"cmd", cmd}, {"params", json::object()}};
        if (!event_id.empty())
            p["event_id"] = event_id;
        return json{{"header", {{"id", 1}}}, {"payload", p}}.dump();
    }
    const std::string good = "http://127.0.0.1:5000/web/flutter_web/index.html";
};
} // namespace

TEST_CASE("bridge ignores messages from other origins and after shutdown", "[SSWCP][device_ui]")
{
    BridgeHarness h;
    CHECK_FALSE(h.bridge.on_script_message("https://evil.example/", BridgeHarness::msg("sw_GetSoftwareInfo")));
    CHECK_FALSE(h.bridge.on_script_message("", BridgeHarness::msg("sw_GetSoftwareInfo")));
    CHECK(h.sent.empty());
    CHECK(h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_GetSoftwareInfo")));
    REQUIRE(h.sent.size() == 1);
    CHECK(h.reply(0)["payload"]["data"]["http_host"] == "http://127.0.0.1:5000");

    h.bridge.shutdown();
    CHECK_FALSE(h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_GetSoftwareInfo")));
    CHECK(h.sent.size() == 1);
    CHECK(h.bridge.sswcp().subscription_count() == 0);
}

TEST_CASE("bridge serves the local devices and pushes changes to subscribers", "[SSWCP][device_ui]")
{
    BridgeHarness h;
    h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_GetLocalDevices"));
    CHECK(h.reply(0)["payload"]["data"] == json::array());

    h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_SubscribeLocalDevices", "dev-sub"));
    REQUIRE(h.sent.size() == 2); // the ack
    h.bridge.tick();
    CHECK(h.sent.size() == 2);   // unchanged list: no push

    h.devices.push_back({"192.168.1.5:7125", "", "U1", "192.168.1.5", "Snapmaker U1", true});
    h.bridge.tick();
    REQUIRE(h.sent.size() == 3);
    json push = h.reply(2);
    CHECK(push["header"] == json{{"event_id", "dev-sub"}});
    REQUIRE(push["payload"]["data"].size() == 1);
    CHECK(push["payload"]["data"][0]["name"] == "U1");
    CHECK(push["payload"]["data"][0]["online"] == true);

    h.bridge.tick(); // same list again
    CHECK(h.sent.size() == 3);
    h.devices[0].online = false;
    h.bridge.tick();
    CHECK(h.sent.size() == 4);

    h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_GetLocalDevices"));
    CHECK(h.reply(4)["payload"]["data"][0]["dev_id"] == "192.168.1.5:7125");
}

TEST_CASE("device record carries the keys the page needs, sn above all", "[SSWCP][device_ui]")
{
    SnapmakerDeviceInfo d;
    d.dev_id    = "3d-printer";
    d.name      = "U1";
    d.ip        = "http://3d-printer:7125/";
    d.model     = "Snapmaker U1";
    d.online    = true;
    d.connected = true;

    json r = snapmaker_device_to_json(d);
    // The page builds a card only from a non-empty `sn`; without a real serial it is the dev_id.
    REQUIRE(r.contains("sn"));
    CHECK(r["sn"].is_string());
    CHECK(r["sn"] == "3d-printer");
    CHECK(r["id"] == "3d-printer");
    CHECK(r["name"] == "U1");
    CHECK(r["device_name"] == "U1");
    CHECK(r["ip"] == "3d-printer"); // bare host: no scheme, port or path
    CHECK(r["port"] == 1884);
    CHECK(r["clientId"] == "orca-3d-printer");
    CHECK(r["link_mode"] == "lan");
    CHECK(r["machine_type"] == "Snapmaker U1");
    CHECK(r["connected"] == true);
    CHECK(r["online"] == true);

    d.sn = "SM2U1XYZ";
    CHECK(snapmaker_device_to_json(d)["sn"] == "SM2U1XYZ"); // a real serial wins
    CHECK(snapmaker_device_to_json(d)["id"] == "3d-printer");

    // Never an empty sn, even for a blank record with only an id.
    SnapmakerDeviceInfo bare;
    bare.dev_id = "x:1";
    CHECK(snapmaker_device_to_json(bare)["sn"] == "x:1");
    CHECK(snapmaker_device_to_json(bare)["name"] == "x:1");

    CHECK(snapmaker_bare_host("u1.local:7125") == "u1.local");
    CHECK(snapmaker_bare_host("https://u1.local/x") == "u1.local");
    CHECK(snapmaker_bare_host("192.168.1.5") == "192.168.1.5");
    CHECK(snapmaker_bare_host("[::1]:7125") == "[::1]");               // bracketed IPv6 with a port
    CHECK(snapmaker_bare_host("[::1]") == "[::1]");
    CHECK(snapmaker_bare_host("http://[fe80::1]:7125/x") == "[fe80::1]");
    CHECK(snapmaker_bare_host("fe80::1") == "fe80::1");                  // unbracketed IPv6: left whole, not cut at a colon
    CHECK(snapmaker_bare_host("http://user:pw@u1.local:7125/x") == "u1.local"); // credentials never reach the page
    CHECK(snapmaker_bare_host("user:pw@u1.local") == "u1.local");
}

TEST_CASE("sw_GetConnectedMachine returns the connected machine's record, {} when none", "[SSWCP][device_ui]")
{
    BridgeHarness h;
    h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_GetConnectedMachine"));
    REQUIRE(h.sent.size() == 1);
    CHECK(h.reply(0)["payload"]["code"] == 200);
    CHECK(h.reply(0)["payload"]["data"] == json::object());

    SnapmakerDeviceInfo a, b;
    a.dev_id = "a:1"; a.name = "A"; a.ip = "a"; // not connected
    b.dev_id = "3d-printer"; b.name = "U1"; b.ip = "3d-printer"; b.model = "Snapmaker U1"; b.online = true; b.connected = true;
    h.devices = {a, b};
    h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_GetConnectedMachine"));
    json rec = h.reply(1)["payload"]["data"];
    CHECK(rec["sn"] == "3d-printer");
    CHECK(rec["connected"] == true);
    // Same record as the list entry (the page was verified with both carrying the same object).
    h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_GetLocalDevices"));
    json list = h.reply(2)["payload"]["data"];
    REQUIRE(list.size() == 2);
    CHECK(list[1] == rec);
    for (const auto& e : list)
        CHECK_FALSE(e["sn"].get<std::string>().empty());

    // Foreign origin still refused.
    CHECK_FALSE(h.bridge.on_script_message("https://evil.example/", BridgeHarness::msg("sw_GetConnectedMachine")));
    // A command that is not on the allow-list is still a 501.
    h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_SendGCodes"));
    CHECK(h.reply(h.sent.size() - 1)["payload"]["code"] == 501);
}

TEST_CASE("local-device pushes use the same record shape", "[SSWCP][device_ui]")
{
    BridgeHarness h;
    h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_SubscribeLocalDevices", "sub"));
    SnapmakerDeviceInfo d;
    d.dev_id = "3d-printer"; d.name = "U1"; d.ip = "3d-printer"; d.online = true; d.connected = true;
    h.devices = {d};
    h.bridge.tick();
    REQUIRE(h.sent.size() == 2);
    json push = h.reply(1)["payload"]["data"];
    REQUIRE(push.size() == 1);
    CHECK(push[0] == snapmaker_device_to_json(d));
    CHECK(push[0]["sn"] == "3d-printer");
}

// ---- machine state, navigation, MQTT emulation, filament info, read-only guarantee ----

namespace {
json moonraker_result(double bed_temp)
{
    return {{"eventtime", 1234.5},
            {"status", {{"heater_bed", {{"temperature", bed_temp}, {"target", 60}}},
                        {"extruder", {{"temperature", 201}, {"target", 220}}},
                        {"print_stats", {{"state", "standby"}}}}}};
}
} // namespace

TEST_CASE("sw_GetMachineState replies the Moonraker result unwrapped, and later", "[SSWCP][device_ui]")
{
    BridgeHarness h;
    h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_GetMachineState"));
    REQUIRE(h.queries.size() == 1);
    CHECK(h.sent.empty());                       // delayed reply: nothing yet, instance still open
    CHECK(h.bridge.sswcp().instance_count() == 1);
    // No filter given: the default object set is asked for.
    CHECK(h.queries[0].objects == snapmaker_default_state_objects());

    h.queries[0].done(true, moonraker_result(59));
    REQUIRE(h.sent.size() == 1);
    json data = h.reply(0)["payload"]["data"];
    CHECK(h.reply(0)["payload"]["code"] == 200);
    CHECK(h.reply(0)["header"] == json{{"id", 1}});
    CHECK(data == moonraker_result(59));         // unchanged, NOT wrapped in "result"
    CHECK_FALSE(data.contains("result"));
    CHECK(data["status"]["heater_bed"]["temperature"] == 59);
    CHECK(h.bridge.sswcp().instance_count() == 0);
}

TEST_CASE("sw_GetMachineState asks for the objects the page names, sanitised", "[SSWCP][device_ui]")
{
    BridgeHarness h;
    json p = {{"objects", {{"extruder", nullptr}, {"fan_generic cavity_fan", nullptr}, {"bad/name", nullptr}, {"x&y=1", nullptr}}}};
    h.bridge.on_script_message(h.good, json{{"header", {{"id", 1}}}, {"payload", {{"cmd", "sw_GetMachineState"}, {"params", p}}}}.dump());
    REQUIRE(h.queries.size() == 1);
    CHECK(h.queries[0].objects == std::vector<std::string>{"extruder", "fan_generic cavity_fan"});
}

TEST_CASE("machine state fails soft: no source, failed query, late answer after shutdown", "[SSWCP][device_ui]")
{
    SECTION("no machine: the empty state, immediately") {
        BridgeHarness h;
        h.have_source = false;
        h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_GetMachineState"));
        REQUIRE(h.sent.size() == 1);
        CHECK(h.reply(0)["payload"]["code"] == 200);
        CHECK(h.reply(0)["payload"]["data"] == snapmaker_empty_machine_state());
        CHECK(h.reply(0)["payload"]["data"]["status"] == json::object());
    }
    SECTION("failed query") {
        BridgeHarness h;
        h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_GetMachineState"));
        h.queries.at(0).done(false, json::object());
        REQUIRE(h.sent.size() == 1);
        CHECK(h.reply(0)["payload"]["data"] == snapmaker_empty_machine_state());
    }
    SECTION("garbage result is treated as failure") {
        BridgeHarness h;
        h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_GetMachineState"));
        h.queries.at(0).done(true, json::array({1, 2}));
        REQUIRE(h.sent.size() == 1);
        CHECK(h.reply(0)["payload"]["data"] == snapmaker_empty_machine_state());
    }
    SECTION("answer after shutdown is dropped, not sent") {
        BridgeHarness h;
        h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_GetMachineState"));
        h.bridge.shutdown();
        h.queries.at(0).done(true, moonraker_result(1));
        CHECK(h.sent.empty());
    }
    SECTION("a second callback must not double-reply") {
        BridgeHarness h;
        h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_GetMachineState"));
        h.queries.at(0).done(true, moonraker_result(2));
        CHECK(h.sent.size() == 1);
        h.queries.at(0).done(true, moonraker_result(3));
        CHECK(h.sent.size() == 1);
    }
    SECTION("a query that never answers: the watchdog gives the waiter the empty state, a late answer is ignored") {
        BridgeHarness h;
        h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_GetMachineState"));
        REQUIRE(h.queries.size() == 1);
        for (int i = 0; i < 14; ++i) h.bridge.tick();
        CHECK(h.sent.empty());                                       // not yet
        h.bridge.tick();                                              // 15th tick: abandoned
        REQUIRE(h.sent.size() == 1);
        CHECK(h.reply(0)["payload"]["data"] == snapmaker_empty_machine_state());
        h.queries[0].done(true, moonraker_result(5));                // late
        CHECK(h.sent.size() == 1);
        // And polling is not wedged: a new request starts a new query.
        h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_GetMachineState"));
        CHECK(h.queries.size() == 2);
        h.queries[1].done(true, moonraker_result(6));
        CHECK(h.reply(1)["payload"]["data"] == moonraker_result(6));
    }
}

TEST_CASE("sw_SubscribeMachineState acks, pushes the first state, then only changes, throttled", "[SSWCP][device_ui]")
{
    BridgeHarness h;
    h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_SubscribeMachineState", "msub"));
    REQUIRE(h.sent.size() == 1);                                   // the ack
    CHECK(h.reply(0)["header"] == json{{"id", 1}});
    CHECK(h.reply(0)["payload"]["code"] == 200);
    CHECK(h.bridge.sswcp().subscriber_count("sw_SubscribeMachineState") == 1);

    REQUIRE(h.queries.size() == 1);                                // first state requested straight away
    h.queries[0].done(true, moonraker_result(59));
    REQUIRE(h.sent.size() == 2);
    CHECK(h.reply(1)["header"] == json{{"event_id", "msub"}});
    CHECK(h.reply(1)["payload"]["data"] == moonraker_result(59));

    // Throttle: ticks 1 and 2 do not query; tick 3 does.
    h.bridge.tick(); h.bridge.tick();
    CHECK(h.queries.size() == 1);
    h.bridge.tick();
    REQUIRE(h.queries.size() == 2);
    h.queries[1].done(true, moonraker_result(59));                 // same status, new eventtime
    json same = moonraker_result(59); same["eventtime"] = 9999;
    CHECK(h.sent.size() == 2);                                     // unchanged status: no push

    h.bridge.tick(); h.bridge.tick(); h.bridge.tick();
    REQUIRE(h.queries.size() == 3);
    h.queries[2].done(true, moonraker_result(60));                 // changed
    REQUIRE(h.sent.size() == 3);
    CHECK(h.reply(2)["payload"]["data"]["status"]["heater_bed"]["temperature"] == 60);

    // A failing poll pushes nothing and does not wedge the next poll.
    h.bridge.tick(); h.bridge.tick(); h.bridge.tick();
    h.queries[3].done(false, json::object());
    CHECK(h.sent.size() == 3);
    h.bridge.tick(); h.bridge.tick(); h.bridge.tick();
    CHECK(h.queries.size() == 5);

    // No subscriber, no polling.
    h.queries[4].done(true, moonraker_result(61));
    h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_UnSubscribeMachineState"));
    CHECK(h.bridge.sswcp().subscriber_count("sw_SubscribeMachineState") == 0);
    const auto n = h.queries.size();
    for (int i = 0; i < 6; ++i) h.bridge.tick();
    CHECK(h.queries.size() == n);
}

TEST_CASE("sw_SetSubscribeFilter narrows what the subscription polls", "[SSWCP][device_ui]")
{
    BridgeHarness h;
    json p = {{"objects", {{"extruder", nullptr}}}};
    h.bridge.on_script_message(h.good, json{{"header", {{"id", 1}}}, {"payload", {{"cmd", "sw_SetSubscribeFilter"}, {"params", p}}}}.dump());
    CHECK(h.reply(0)["payload"]["code"] == 200);
    h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_SubscribeMachineState", "s"));
    REQUIRE(h.queries.size() == 1);
    CHECK(h.queries[0].objects == std::vector<std::string>{"extruder"});
}

TEST_CASE("sw_SwitchTab maps the page's target names to host tabs", "[SSWCP][device_ui]")
{
    CHECK(snapmaker_tab_for_target("Monitor") == SnapmakerTab::Monitor);
    CHECK(snapmaker_tab_for_target("monitor") == SnapmakerTab::Monitor);
    CHECK(snapmaker_tab_for_target("Home") == SnapmakerTab::Home);
    CHECK(snapmaker_tab_for_target("3DEditor") == SnapmakerTab::Prepare);
    CHECK(snapmaker_tab_for_target("Preview") == SnapmakerTab::Preview);
    CHECK(snapmaker_tab_for_target("MultiDevice") == SnapmakerTab::MultiDevice);
    CHECK(snapmaker_tab_for_target("Project") == SnapmakerTab::Project);
    CHECK(snapmaker_tab_for_target("Calibration") == SnapmakerTab::Calibration);
    CHECK_FALSE(snapmaker_tab_for_target("Auxiliary").has_value());
    CHECK_FALSE(snapmaker_tab_for_target("DebugTool").has_value());
    CHECK_FALSE(snapmaker_tab_for_target("").has_value());

    BridgeHarness h;
    h.bridge.on_script_message(h.good, json{{"header", {{"id", 1}}}, {"payload", {{"cmd", "sw_SwitchTab"}, {"params", {{"target", "Monitor"}}}}}}.dump());
    REQUIRE(h.tabs.size() == 1);
    CHECK(h.tabs[0] == SnapmakerTab::Monitor);
    CHECK(h.reply(0)["payload"]["code"] == 200);

    for (const json& bad : {json::object(), json{{"target", 3}}, json{{"target", "Nowhere"}}, json("Monitor")}) {
        CAPTURE(bad.dump());
        BridgeHarness g;
        g.bridge.on_script_message(g.good, json{{"header", {{"id", 1}}}, {"payload", {{"cmd", "sw_SwitchTab"}, {"params", bad}}}}.dump());
        CHECK(g.tabs.empty());
        CHECK(g.reply(0)["payload"]["code"] == 400);
    }
    BridgeHarness f;
    f.tab_ok = false;
    f.bridge.on_script_message(f.good, json{{"header", {{"id", 1}}}, {"payload", {{"cmd", "sw_SwitchTab"}, {"params", {{"target", "Home"}}}}}}.dump());
    CHECK(f.reply(0)["payload"]["code"] == -1);
}

TEST_CASE("the page's MQTT surface is answered locally and does nothing", "[SSWCP][device_ui]")
{
    BridgeHarness h;
    h.bridge.on_script_message(h.good, json{{"header", {{"id", 1}}}, {"payload", {{"cmd", "sw_create_mqtt_client"},
        {"params", {{"server_address", "mqtts://3d-printer:1884"}, {"clientId", "c"}, {"ca", "SECRET-CA"}, {"key", "SECRET-KEY"}}}}}}.dump());
    CHECK(h.reply(0)["payload"]["code"] == 200);
    CHECK(h.reply(0)["payload"]["data"] == json{{"id", "emu-1"}, {"type", "mqtts"}});
    CHECK(h.sent[0].find("SECRET") == std::string::npos); // keys are never echoed

    for (const char* cmd : {"sw_mqtt_connect", "sw_mqtt_disconnect", "sw_mqtt_subscribe", "sw_mqtt_unsubscribe", "sw_mqtt_publish"}) {
        CAPTURE(cmd);
        h.sent.clear();
        h.bridge.on_script_message(h.good, BridgeHarness::msg(cmd));
        REQUIRE(h.sent.size() == 1);
        CHECK(h.reply(0)["payload"]["code"] == 200);
    }
    CHECK(h.queries.empty()); // nothing reached the machine
    CHECK(h.bridge.sswcp().subscription_count() == 0);
}

TEST_CASE("sw_UpdateMachineFilamentInfo stores the latest value per sn", "[SSWCP][device_ui]")
{
    BridgeHarness h;
    auto send = [&](const json& objects) {
        h.bridge.on_script_message(h.good, json{{"header", {{"id", 1}}}, {"payload", {{"cmd", "sw_UpdateMachineFilamentInfo"}, {"params", {{"objects", objects}}}}}}.dump());
    };
    send(json::array({json{{"key", "SN1"}, {"value", "{\"a\":1}"}}}));
    CHECK(h.reply(0)["payload"]["code"] == 200);
    CHECK(h.bridge.filament_info().at("SN1") == "{\"a\":1}");
    send(json::array({json{{"key", "SN1"}, {"value", "{\"a\":2}"}}, json{{"key", "SN2"}, {"value", "x"}}}));
    CHECK(h.bridge.filament_info().size() == 2);
    CHECK(h.bridge.filament_info().at("SN1") == "{\"a\":2}"); // latest wins
    // Junk entries are skipped, the reply is still 200, nothing else reaches the machine.
    send(json::array({1, json{{"key", 5}, {"value", "v"}}, json{{"key", ""}, {"value", "v"}}, json{{"key", "SN3"}}}));
    CHECK(h.bridge.filament_info().size() == 2);
    CHECK(h.sent.size() == 3);
    CHECK(h.queries.empty());
}

TEST_CASE("the read-only guarantee: no control action is registered, each gets a 501", "[SSWCP][device_ui]")
{
    BridgeHarness h;
    const std::vector<std::string> controls = {
        "sw_SendGCodes", "sw_MachinePrintStart",
        "sw_UploadFiletoMachine", "sw_UploadFile", "sw_UploadFileResult", "sw_DeleteCameraTimelapse", "sw_DeleteRecentFiles", "sw_DeleteDevices", "sw_RenameDevice",
        "sw_SetDeviceName", "sw_ControlPurifier",
        "sw_ControlExtruderTemp", "sw_StartLocalPrint", "sw_StartCloudPrint", "sw_BedMesh_AbortProbeMesh",
        "sw_SendCommand", "sw_Query", "sw_AddDevice", "sw_SwitchModel", "sw_UpdateDeviceInfo", "sw_mqtt_set_engine",
        "sw_SetFilamentMappingComplete", "sw_FinishFilamentMapping", "sw_DownloadMachineFile", "sw_DownLoadFile",
        "sw_PullCloudFile", "sw_ServerClientManagerSetUserinfo",
        "sw_Connect", "sw_Disconnect", "sw_ConnectOtherMachine", "sw_GetPincode", "sw_OpenBrowser", "sw_Exit",
        // opens files, windows, consoles or logs outside the app log, or starts discovery (pinned in snapmaker_bridge_commands.txt)
        "sw_OpenProject", "sw_OpenRecentFile", "sw_NewProject", "sw_LaunchConsole", "sw_OpenOrcaWebview", "sw_GetFileStream", "sw_FileView",
        "sw_GetFilesFromDir", "sw_OpenTimelapseFolder", "sw_DownLoadFileAndOpen", "sw_UploadCameraTimelapse", "sw_SetLogLevel", "sw_Log",
        "sw_SnapLog", "sw_StartMachineFind", "sw_UserLogout", "sw_OpenNetworkDialog"};
    for (const auto& cmd : controls) {
        CAPTURE(cmd);
        CHECK_FALSE(h.bridge.sswcp().is_registered(cmd));
        h.sent.clear();
        CHECK(h.bridge.on_script_message(h.good, BridgeHarness::msg(cmd)));
        REQUIRE(h.sent.size() == 1);
        CHECK(h.reply(0)["payload"]["code"] == 501);
    }
    CHECK(h.queries.empty());
    CHECK(h.tabs.empty());
}

TEST_CASE("exactly the documented command set is registered", "[SSWCP][device_ui]")
{
    BridgeHarness h;
    const std::set<std::string> expected = {
        // core (13)
        "sw_FileLog", "sw_SubUserUpdatePrivacy", "sw_SubscribePageStateChange", "sw_SubscribeForegroundChange", "sw_SubscribeRecentFiles",
        "sw_SubscribeLocalDevices", "sw_SubscribeCacheKey", "sw_SubscribeUserLoginState", "sw_GetUserLoginState", "sw_UserLogin",
        "sw_GetSoftwareInfo", "sw_UploadEvent", "sw_GetLocalDevices",
        // bridge
        "sw_GetConnectedMachine", "sw_SwitchTab", "sw_GetMachineState", "sw_SubscribeMachineState", "sw_SetSubscribeFilter",
        "sw_UnSubscribeMachineState", "sw_StopMachineStateSubscription", "sw_create_mqtt_client", "sw_mqtt_connect", "sw_mqtt_disconnect",
        "sw_mqtt_subscribe", "sw_mqtt_unsubscribe", "sw_mqtt_publish", "sw_UpdateMachineFilamentInfo", "sw_GetMachineSystemInfo",
        "sw_SystemGetDeviceInfo", "sw_FileGetStatus", "sw_exception_query", "sw_MachineFilesRoots", "sw_MachineFilesMetadata",
        "sw_GetCache", "sw_SetCache", "sw_RemoveCache", "sw_UnsubscribeCacheKeys", "sw_FilesThumbnailsBase64", "sw_MachineHeartbeat",
        // camera preview (start/stop only; the timelapse commands stay denied)
        "sw_CameraStartMonitor", "sw_CameraStopMonitor",
        // read-only file listing (#75)
        "sw_GetFileListPage",
        // machine control (#75, #79): LED, speed, fans, bed temperature, pause/resume/cancel; nozzle temperature, purifier, device name
        // and the bed-mesh abort stay denied
        "sw_ControlLed", "sw_ControlPrintSpeed", "sw_ControlMainFan", "sw_ControlGenericFan", "sw_ControlBedTemp", "sw_MachinePrintPause",
        "sw_MachinePrintResume", "sw_MachinePrintCancel"};
    for (const auto& c : expected)
        CHECK(h.bridge.sswcp().is_registered(c));
    CHECK(expected.size() == 50);
}

TEST_CASE("the {} read-only commands answer an empty object", "[SSWCP][device_ui]")
{
    BridgeHarness h;
    for (const char* cmd : {"sw_GetMachineSystemInfo", "sw_SystemGetDeviceInfo", "sw_FileGetStatus", "sw_exception_query",
                            "sw_MachineFilesMetadata", "sw_MachineHeartbeat", "sw_GetCache", "sw_SetCache", "sw_RemoveCache", "sw_UnsubscribeCacheKeys"}) {
        CAPTURE(cmd);
        h.sent.clear();
        h.bridge.on_script_message(h.good, BridgeHarness::msg(cmd));
        REQUIRE(h.sent.size() == 1);
        CHECK(h.reply(0)["payload"]["code"] == 200);
        CHECK(h.reply(0)["payload"]["data"] == json::object());
    }
}

// ---- 7f: real serial, system info, metadata, thumbnail ----

namespace {
const char* kSysInfoBody = R"({"result":{"system_info":{"product_info":{"machine_type":"Snapmaker U1","serial_number":"U1REAL123","device_name":"U1","firmware_version":"2.0.0"}}}})";
SnapmakerDeviceInfo connected_u1()
{
    SnapmakerDeviceInfo d;
    d.dev_id = "3d-printer"; d.name = "U1"; d.ip = "3d-printer"; d.model = "Snapmaker U1"; d.online = true; d.connected = true;
    return d;
}
} // namespace

TEST_CASE("serial and system_info helpers", "[SSWCP][device_ui]")
{
    CHECK(snapmaker_serial_from_system_info(json::parse(kSysInfoBody)) == "U1REAL123");
    CHECK(snapmaker_serial_from_system_info(json::parse(kSysInfoBody)["result"]) == "U1REAL123"); // bare result too
    CHECK(snapmaker_system_info_result(json::parse(kSysInfoBody)).contains("system_info"));
    for (const char* bad : {"{}", "[]", "5", R"({"result":{}})", R"({"result":{"system_info":{}}})", R"({"result":{"system_info":{"product_info":{"serial_number":5}}}})"}) {
        CAPTURE(bad);
        CHECK(snapmaker_serial_from_system_info(json::parse(bad)).empty());
    }
    CHECK(snapmaker_system_info_result(json::parse("{}")) == json::object());
}

TEST_CASE("filename and thumbnail-path validation and encoding", "[SSWCP][device_ui]")
{
    CHECK(snapmaker_valid_gcode_name("a.gcode"));
    CHECK(snapmaker_valid_gcode_name("dir/sub dir/a b.gcode"));
    for (const char* bad : {"", "/etc/passwd", "../x", "a/../b", "a//b", "a/./b", "a\\b"})
        CHECK_FALSE(snapmaker_valid_gcode_name(bad));
    CHECK_FALSE(snapmaker_valid_gcode_name(std::string("a\nb")));
    CHECK_FALSE(snapmaker_valid_gcode_name(std::string("a\0b", 3)));
    CHECK_FALSE(snapmaker_valid_gcode_name(std::string(513, 'a')));
    CHECK(snapmaker_valid_gcode_name(std::string(512, 'a')));

    CHECK(snapmaker_percent_encode("a b&c=d?e#f%g", false) == "a%20b%26c%3Dd%3Fe%23f%25g");
    CHECK(snapmaker_percent_encode("d/a b.png", true) == "d/a%20b.png");
    CHECK(snapmaker_percent_encode("d/a", false) == "d%2Fa");
    CHECK(snapmaker_percent_encode("\xc3\xa4", false) == "%C3%A4");

    CHECK(snapmaker_valid_thumbnail_path(".thumbs/x-300x300.png"));
    for (const char* bad : {"http://evil/x", "x?y=1", "x#y", "../x.png", "/abs.png", "c:/x.png", ""})
        CHECK_FALSE(snapmaker_valid_thumbnail_path(bad));
}

TEST_CASE("thumbnail reply shaping, base64 and the size cap", "[SSWCP][device_ui]")
{
    CHECK(snapmaker_base64("") == "");
    CHECK(snapmaker_base64("f") == "Zg==");
    CHECK(snapmaker_base64("fo") == "Zm8=");
    CHECK(snapmaker_base64("foo") == "Zm9v");
    CHECK(snapmaker_base64("foob") == "Zm9vYg==");
    CHECK(snapmaker_base64(std::string("\xff\xfe\x00", 3)) == "//4A");

    json ok = snapmaker_thumbnail_reply("a.gcode", 300, 300, "PNGDATA");
    CHECK(ok == json{{"width", 300}, {"height", 300}, {"size", 7}, {"data", snapmaker_base64("PNGDATA")}, {"state", "success"}, {"path", "a.gcode"}});
    CHECK(snapmaker_thumbnail_reply("a", 1, 1, std::string(SNAPMAKER_THUMBNAIL_MAX_BYTES, 'x'))["state"] == "success");
    json big = snapmaker_thumbnail_reply("a", 1, 1, std::string(SNAPMAKER_THUMBNAIL_MAX_BYTES + 1, 'x'));
    CHECK(big["state"] == "error");
    CHECK(big["data"] == "");
    CHECK(snapmaker_thumbnail_reply("a", 1, 1, "")["state"] == "error");

    json list = json::array({json{{"width", 96}, {"height", 96}, {"size", 100}, {"thumbnail_path", ".thumbs/a-96x96.png"}},
                             json{{"width", 300}, {"height", 300}, {"size", 900}, {"thumbnail_path", ".thumbs/a-300x300.png"}},
                             json{{"width", 999}, {"height", 999}, {"size", 99999999}, {"thumbnail_path", ".thumbs/huge.png"}},
                             json{{"width", 400}, {"height", 400}, {"size", 10}, {"thumbnail_path", "http://evil/x.png"}}});
    CHECK(snapmaker_pick_thumbnail(list)["thumbnail_path"] == ".thumbs/a-300x300.png"); // biggest valid one under the cap
    CHECK(snapmaker_pick_thumbnail(json::array()).empty());
    CHECK(snapmaker_pick_thumbnail(json::object()).empty());
}

TEST_CASE("the record's sn becomes the machine's serial and subscribers are told", "[SSWCP][device_ui]")
{
    BridgeHarness h;
    h.devices = {connected_u1()};
    h.resources["/machine/system_info"] = kSysInfoBody;

    h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_SubscribeLocalDevices", "ls"));
    h.bridge.tick();                                   // first sight of the connected machine: fetch + push the list
    REQUIRE(h.fetched.size() == 1);
    CHECK(h.fetched[0] == "/machine/system_info");
    // The list pushed first carried dev_id as sn; once the serial arrives (synchronously in the fake) the same tick
    // already sees the real one, and whatever was pushed last must agree with it.
    h.sent.clear();
    h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_GetLocalDevices"));
    CHECK(h.reply(0)["payload"]["data"][0]["sn"] == "U1REAL123");
    h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_GetConnectedMachine"));
    CHECK(h.reply(1)["payload"]["data"]["sn"] == "U1REAL123");
    h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_GetMachineSystemInfo"));
    json info = h.reply(2)["payload"]["data"];
    CHECK(info["system_info"]["product_info"]["serial_number"] == "U1REAL123"); // same serial the record carries
    CHECK(h.fetched.size() == 1);                      // cached: not fetched again

    for (int i = 0; i < 5; ++i) h.bridge.tick();
    CHECK(h.fetched.size() == 1);
}

// #77: the page keys a machine by the sn of the first record it gets and connects it under that key; a later record under
// another sn looks like a new, never-connected device ("Unconnected"). So nothing may name the connected machine by its
// dev_id while the serial read is still out.
TEST_CASE("the first device replies and pushes wait for the serial, so the sn never changes under the page", "[SSWCP][device_ui]")
{
    BridgeHarness h;
    h.devices    = {connected_u1()};
    h.hold_fetch = true;
    h.resources["/machine/system_info"] = kSysInfoBody;

    h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_SubscribeLocalDevices", "ls"));
    h.sent.clear();
    h.bridge.tick();                                   // starts the serial read, which stays in flight
    h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_GetConnectedMachine"));
    h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_GetLocalDevices"));
    REQUIRE(h.fetched.size() == 1);                    // one read, shared by both callers
    CHECK(h.sent.empty());                             // no reply, and no push, under the dev_id

    h.release_fetches();
    REQUIRE_FALSE(h.sent.empty());
    for (std::size_t n = 0; n < h.sent.size(); ++n) {  // everything the page is told names the machine by its serial
        json data = h.reply(n)["payload"]["data"];
        json rec  = data.is_array() ? data[0] : data;
        CHECK(rec["sn"] == "U1REAL123");
    }
    CHECK(h.fetched.size() == 1);
}

TEST_CASE("a failed serial read still answers the page, under the dev_id", "[SSWCP][device_ui]")
{
    BridgeHarness h;
    h.devices    = {connected_u1()};
    h.hold_fetch = true;                               // no resource registered: the read fails
    h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_GetConnectedMachine"));
    h.bridge.tick();
    CHECK(h.sent.empty());
    h.release_fetches();
    REQUIRE(h.sent.size() == 1);
    CHECK(h.reply(0)["payload"]["data"]["sn"] == "3d-printer");
    CHECK(h.reply(0)["payload"]["data"]["connected"] == true);
}

TEST_CASE("a late serial is pushed to local-device subscribers", "[SSWCP][device_ui]")
{
    BridgeHarness h;
    h.devices = {connected_u1()};
    // Hold the fetch: capture its completion instead of answering at once.
    std::function<void(bool, std::string)> pending;
    SnapmakerDeviceBridge::Options o;
    // (reuse the harness bridge but swap the source through resources: absent = failure first)
    h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_SubscribeLocalDevices", "ls"));
    h.bridge.tick();                                   // fetch fails (no resource): record keeps dev_id as sn
    h.sent.clear();
    h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_GetLocalDevices"));
    CHECK(h.reply(0)["payload"]["data"][0]["sn"] == "3d-printer");
    h.resources["/machine/system_info"] = kSysInfoBody;
    for (int i = 0; i < 12; ++i) h.bridge.tick();      // retry after the back-off
    // The retry got the serial and pushed the changed list to the subscriber.
    bool pushed = false;
    for (std::size_t n = 0; n < h.sent.size(); ++n) {
        json r = h.reply(n);
        if (r["header"] == json{{"event_id", "ls"}} && r["payload"]["data"][0]["sn"] == "U1REAL123") pushed = true;
    }
    CHECK(pushed);
}

TEST_CASE("sw_MachineFilesMetadata replies the flat result, fail-soft", "[SSWCP][device_ui]")
{
    BridgeHarness h;
    h.resources["/server/files/metadata?filename=dir%2Fmy%20file.gcode"] = R"({"result":{"size":123,"layer_count":103,"estimated_time":11793}})";
    json p = {{"filename", "dir/my file.gcode"}};
    h.bridge.on_script_message(h.good, json{{"header", {{"id", 1}}}, {"payload", {{"cmd", "sw_MachineFilesMetadata"}, {"params", p}}}}.dump());
    CHECK(h.reply(0)["payload"]["data"] == json{{"size", 123}, {"layer_count", 103}, {"estimated_time", 11793}}); // flat, not wrapped
    // Unknown file -> {} ; hostile names never reach a URL.
    for (const char* name : {"missing.gcode", "../../etc/passwd", "a?b=c", ""}) {
        CAPTURE(name);
        h.fetched.clear();
        h.bridge.on_script_message(h.good, json{{"header", {{"id", 1}}}, {"payload", {{"cmd", "sw_MachineFilesMetadata"}, {"params", {{"filename", name}}}}}}.dump());
        CHECK(h.reply(h.sent.size() - 1)["payload"]["data"] == json::object());
        if (std::string(name) == "missing.gcode") CHECK(h.fetched.size() == 1); else CHECK(h.fetched.empty() == (std::string(name) != "a?b=c"));
    }
}

TEST_CASE("sw_FilesThumbnailsBase64 fetches the list then the image, bounded and fail-soft", "[SSWCP][device_ui]")
{
    BridgeHarness h;
    h.resources["/server/files/thumbnails?filename=a%20b.gcode"] =
        R"({"result":[{"width":96,"height":96,"size":10,"thumbnail_path":".thumbs/a b-96x96.png"},{"width":300,"height":300,"size":7,"thumbnail_path":".thumbs/a b-300x300.png"}]})";
    h.resources["/server/files/gcodes/.thumbs/a%20b-300x300.png"] = "PNGDATA";
    auto ask = [&](const std::string& path) {
        h.bridge.on_script_message(h.good, json{{"header", {{"id", 1}}}, {"payload", {{"cmd", "sw_FilesThumbnailsBase64"}, {"params", {{"path", path}}}}}}.dump());
        return h.reply(h.sent.size() - 1)["payload"]["data"];
    };
    json r = ask("a b.gcode");
    CHECK(r == json{{"width", 300}, {"height", 300}, {"size", 7}, {"data", snapmaker_base64("PNGDATA")}, {"state", "success"}, {"path", "a b.gcode"}});
    CHECK(h.fetched.size() == 2);

    h.fetched.clear();
    CHECK(ask("nothing.gcode")["state"] == "error");           // no thumbnails for the file
    CHECK(h.fetched.size() == 1);
    h.fetched.clear();
    CHECK(ask("../../secret")["state"] == "error");            // never reaches a URL
    CHECK(h.fetched.empty());
    h.resources["/server/files/thumbnails?filename=evil.gcode"] = R"({"result":[{"width":300,"height":300,"size":7,"thumbnail_path":"http://evil.example/x.png"}]})";
    CHECK(ask("evil.gcode")["state"] == "error");              // a thumbnail_path that is not a relative path is ignored
    h.fetched.clear();
    h.resources["/server/files/thumbnails?filename=img.gcode"] = R"({"result":[{"width":300,"height":300,"size":7,"thumbnail_path":"t.png"}]})";
    h.resources["/server/files/gcodes/t.png"] = std::string(SNAPMAKER_THUMBNAIL_MAX_BYTES + 1, 'x');
    CHECK(ask("img.gcode")["state"] == "error");               // oversized image
}

// ---- review fixes: flood, queued subscribe, machine change, object allow-list, pause, userinfo, command gate ----

TEST_CASE("GetMachineState callers share ONE Moonraker query", "[SSWCP][device_ui]")
{
    BridgeHarness h;
    for (int i = 0; i < 20; ++i)
        h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_GetMachineState"));
    h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_SubscribeMachineState")); // no event_id: same one-shot path
    CHECK(h.queries.size() == 1);                      // coalesced
    CHECK(h.sent.empty());
    h.queries[0].done(true, moonraker_result(59));
    REQUIRE(h.sent.size() == 21);                      // every caller got the shared result
    for (std::size_t i = 0; i < h.sent.size(); ++i)
        CHECK(h.reply(i)["payload"]["data"] == moonraker_result(59));
    CHECK(h.bridge.sswcp().instance_count() == 0);
    // The next request starts a fresh query.
    h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_GetMachineState"));
    CHECK(h.queries.size() == 2);
}

// Real bug seen in the app (2026-11-06): the page asks for several object sets within a few hundred ms. A caller that
// arrived while a query was out was answered with the FIRST caller's objects, so the page got no print_stats /
// machine_state_manager / heater_bed, showed "idle" and a 0/0 bed during a print, and later pushes carried only deltas.
TEST_CASE("GetMachineState callers with different object sets each get THEIR objects", "[SSWCP][device_ui]")
{
    BridgeHarness h;
    auto ask = [&](std::vector<std::string> names) {
        json objs = json::object();
        for (const auto& n : names) objs[n] = nullptr;
        h.bridge.on_script_message(h.good, json{{"header", {{"id", 1}}}, {"payload", {{"cmd", "sw_GetMachineState"}, {"params", {{"objects", objs}}}}}}.dump());
    };
    ask({"extruder"});                                 // A: starts the query
    ask({"print_stats"});                              // B: not covered by A's objects
    ask({"extruder"});                                 // C: covered by A's, rides on it
    REQUIRE(h.queries.size() == 1);
    CHECK(h.queries[0].objects == std::vector<std::string>{"extruder"});
    h.queries[0].done(true, json{{"status", {{"extruder", {{"temperature", 1}}}}}});
    REQUIRE(h.sent.size() == 2);                       // A and C answered; B still waits for its own objects
    REQUIRE(h.queries.size() == 2);                    // B's objects were queried next, not dropped
    CHECK(h.queries[1].objects == std::vector<std::string>{"print_stats"});
    h.queries[1].done(true, json{{"status", {{"print_stats", {{"state", "printing"}}}}}});
    REQUIRE(h.sent.size() == 3);
    json b = h.reply(2)["payload"]["data"];
    CHECK(b["status"].contains("print_stats"));        // B got print_stats, not A's extruder
    CHECK(b["status"]["print_stats"]["state"] == "printing");
    CHECK(h.bridge.sswcp().instance_count() == 0);
}

TEST_CASE("a flood of GetMachineState hits the pending cap with a 429 and still one query", "[SSWCP][device_ui]")
{
    BridgeHarness h;
    for (std::size_t i = 0; i < SSWCP_MAX_PENDING + 50; ++i)
        h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_GetMachineState"));
    CHECK(h.queries.size() == 1);
    CHECK(h.bridge.sswcp().instance_count() == SSWCP_MAX_PENDING);
    std::size_t too_many = 0;
    for (std::size_t i = 0; i < h.sent.size(); ++i)
        if (h.reply(i)["payload"]["code"] == 429) ++too_many;
    CHECK(too_many == 50);
}

TEST_CASE("a subscriber added while a poll is in flight still gets an initial state", "[SSWCP][device_ui]")
{
    BridgeHarness h;
    h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_SubscribeMachineState", "first"));
    REQUIRE(h.queries.size() == 1);                    // the first subscriber's initial query, still out
    h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_SubscribeMachineState", "second"));
    CHECK(h.queries.size() == 1);                      // shares it
    h.queries[0].done(true, moonraker_result(59));
    std::set<std::string> got;
    for (std::size_t i = 0; i < h.sent.size(); ++i) {
        json r = h.reply(i);
        if (r["header"].contains("event_id") && r["payload"]["data"].contains("status")) got.insert(r["header"]["event_id"].get<std::string>());
    }
    CHECK(got == std::set<std::string>{"first", "second"});
}

TEST_CASE("selecting a different machine resets the cached state and drops replies from the old one", "[SSWCP][device_ui]")
{
    BridgeHarness h;
    SnapmakerDeviceInfo a = connected_u1();
    h.devices = {a};
    h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_SubscribeMachineState", "s"));
    h.bridge.tick();                                   // sees machine A
    REQUIRE_FALSE(h.queries.empty());
    h.queries.back().done(true, moonraker_result(59));
    const auto before = h.sent.size();

    // A request is out for machine A when the selection moves to B.
    h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_GetMachineState"));
    const auto out_idx = h.queries.size() - 1;
    SnapmakerDeviceInfo b = connected_u1();
    b.dev_id = "other:7125"; b.name = "B"; b.ip = "other";
    h.devices = {b};
    h.bridge.tick();
    // The waiting GetMachineState got the empty state; the late answer for machine A is dropped.
    json waiting = h.reply(h.sent.size() - 1);
    (void) before;
    bool got_empty = false;
    for (std::size_t i = before; i < h.sent.size(); ++i)
        if (h.reply(i)["payload"]["data"] == snapmaker_empty_machine_state()) got_empty = true;
    CHECK(got_empty);
    const auto sent_now = h.sent.size();
    h.queries[out_idx].done(true, moonraker_result(1));
    CHECK(h.sent.size() == sent_now);
    // B's first state is pushed even though A's status was identical.
    h.queries.back().done(true, moonraker_result(59));
    json last = h.reply(h.sent.size() - 1);
    CHECK(last["header"] == json{{"event_id", "s"}});
}

// Real bug seen in the app (2026-11-06): the page's first GetMachineState calls are out when the first tick() sees the
// connected machine for the first time. That was treated as "a different machine selected" and abandoned them, so they were
// answered code 200 with an EMPTY status; the page applies a reply as the whole state and showed "idle", 0% and bed 0/0
// during a print (full pushes did not repair it).
TEST_CASE("the first sight of the connected machine does not abandon the state queries already out", "[SSWCP][device_ui]")
{
    BridgeHarness h;
    h.devices = {connected_u1()};
    h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_GetMachineState")); // out before any tick
    REQUIRE(h.queries.size() >= 1);
    h.bridge.tick();                                   // first sight of the machine
    CHECK(h.sent.empty());                             // nobody was answered with an empty state
    h.queries[0].done(true, moonraker_result(59));
    REQUIRE_FALSE(h.sent.empty());
    CHECK(h.reply(0)["payload"]["data"] == moonraker_result(59));
}

TEST_CASE("a failed state query is answered with the last good state, never an empty one", "[SSWCP][device_ui]")
{
    BridgeHarness h;
    h.devices = {connected_u1()};
    h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_GetMachineState"));
    h.queries.back().done(true, moonraker_result(59));
    REQUIRE(h.sent.size() == 1);
    h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_GetMachineState"));
    h.queries.back().done(false, json::object());      // the printer did not answer this time
    REQUIRE(h.sent.size() == 2);
    CHECK(h.reply(1)["payload"]["code"] == 200);
    CHECK(h.reply(1)["payload"]["data"] == moonraker_result(59)); // the page keeps what it had
    // With nothing good yet the old fail-soft empty state remains.
    BridgeHarness fresh;
    fresh.bridge.on_script_message(fresh.good, BridgeHarness::msg("sw_GetMachineState"));
    fresh.queries.back().done(false, json::object());
    CHECK(fresh.reply(0)["payload"]["data"] == snapmaker_empty_machine_state());
}

TEST_CASE("only allow-listed Klipper objects are ever queried", "[SSWCP][device_ui]")
{
    for (const char* ok : {"extruder", "extruder1", "extruder3", "heater_bed", "print_stats", "fan", "fan_generic cavity_fan", "led cavity_led",
                           "filament_feed left", "machine_state_manager", "print_task_config", "temperature_sensor chamber"})
        CHECK(snapmaker_state_object_allowed(ok));
    for (const char* bad : {"configfile", "bed_mesh", "gcode_macro START_PRINT", "extruderX", "extruder 1", "fan_generic ", "led", "", "save_variables",
                            "heater_bed\n", "toolhead; rm", "printer", "firmware_retraction", "exclude_object", "output_pin x"})
        CHECK_FALSE(snapmaker_state_object_allowed(bad));
    CHECK_FALSE(snapmaker_state_object_allowed(std::string(65, 'a')));

    BridgeHarness h;
    json p = {{"objects", {{"extruder", nullptr}, {"configfile", nullptr}, {"gcode_macro X", nullptr}}}};
    h.bridge.on_script_message(h.good, json{{"header", {{"id", 1}}}, {"payload", {{"cmd", "sw_GetMachineState"}, {"params", p}}}}.dump());
    REQUIRE(h.queries.size() == 1);
    CHECK(h.queries[0].objects == std::vector<std::string>{"extruder"});     // refused names are not queried
    h.queries[0].done(true, moonraker_result(1));
    json st = h.reply(0)["payload"]["data"]["status"];
    CHECK(st["configfile"] == json::object());                              // ... but answered with empty entries
    CHECK(st["gcode_macro X"] == json::object());
    CHECK(st.contains("extruder"));

    // Everything refused: no query at all.
    BridgeHarness g;
    json q = {{"objects", {{"configfile", nullptr}}}};
    g.bridge.on_script_message(g.good, json{{"header", {{"id", 1}}}, {"payload", {{"cmd", "sw_GetMachineState"}, {"params", q}}}}.dump());
    CHECK(g.queries.empty());
    CHECK(g.reply(0)["payload"]["data"] == json{{"eventtime", 0}, {"status", {{"configfile", json::object()}}}});
}

TEST_CASE("a hidden page polls and pushes nothing, and resumes when shown", "[SSWCP][device_ui]")
{
    BridgeHarness h;
    h.devices = {connected_u1()};
    h.resources["/machine/system_info"] = kSysInfoBody;
    h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_SubscribeMachineState", "s"));
    h.queries.at(0).done(true, moonraker_result(59));
    h.bridge.set_active(false);
    const auto q = h.queries.size();
    const auto f = h.fetched.size();
    const auto n = h.sent.size();
    for (int i = 0; i < 30; ++i) h.bridge.tick();
    CHECK(h.queries.size() == q);                      // no Moonraker polling while hidden
    CHECK(h.fetched.size() == f);
    CHECK(h.sent.size() == n);
    h.bridge.set_active(true);
    CHECK(h.queries.size() == q + 1);                  // resumed: one state query for the subscribers
}

TEST_CASE("credentials in an address never reach the page", "[SSWCP][device_ui]")
{
    CHECK(strip_url_userinfo("http://u:p@h:7125/x") == "http://h:7125/x");
    CHECK(strip_url_userinfo("u:p@h") == "h");
    CHECK(strip_url_userinfo("http://h/a@b") == "http://h/a@b");   // an @ in the path is not userinfo
    CHECK(strip_url_userinfo("http://h:1") == "http://h:1");
    SnapmakerDeviceInfo d;
    d.dev_id = "h:7125"; d.name = "N"; d.ip = "http://user:secret@h:7125/";
    const std::string dumped = snapmaker_device_to_json(d).dump();
    CHECK(dumped.find("secret") == std::string::npos);
    CHECK(snapmaker_device_to_json(d)["ip"] == "h");
}

TEST_CASE("the live registry equals the REGISTERED lines of snapmaker_bridge_commands.txt", "[SSWCP][device_ui]")
{
    const boost::filesystem::path file = boost::filesystem::path(PROFILES_DIR).parent_path() / "web" / "snapmaker_bridge_commands.txt";
    std::ifstream in(file.string());
    REQUIRE(in.good());
    std::set<std::string> registered, denied;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string cls, name;
        if (!(ss >> cls >> name) || cls[0] == '#') continue;
        REQUIRE((cls == "REGISTERED" || cls == "DENIED"));
        (cls == "REGISTERED" ? registered : denied).insert(name);
    }
    CHECK(registered.size() == 50);
    BridgeHarness h;
    for (const auto& n : registered) { CAPTURE(n); CHECK(h.bridge.sswcp().is_registered(n)); }
    for (const auto& n : denied)     { CAPTURE(n); CHECK_FALSE(h.bridge.sswcp().is_registered(n)); }
    for (const auto& n : registered) CHECK(denied.count(n) == 0);
}

// ---- camera preview (#76): sw_CameraStartMonitor / sw_CameraStopMonitor -> Moonraker camera.start_monitor / stop_monitor ----

namespace {
std::string camera_msg(const std::string& cmd, const json& params)
{
    return json{{"header", {{"id", 1}}}, {"payload", {{"cmd", cmd}, {"params", params}}}}.dump();
}
const json kCameraParams = {{"domain", "lan"}, {"interval", 0}, {"expect_pw", true}, {"clientid", "orca-3d-printer"}};
} // namespace

TEST_CASE("camera params whitelist: only domain lan, interval 0..10, bool expect_pw, a plain clientid", "[SSWCP][device_ui][camera]")
{
    CHECK(camera_monitor_params_valid(kCameraParams));
    CHECK(camera_monitor_params_valid(json{{"domain", "lan"}, {"clientid", "orca-U1.x_2"}}));        // interval / expect_pw are optional
    CHECK(camera_monitor_params_valid(json{{"domain", "lan"}, {"interval", 10}, {"clientid", "a"}}));
    auto with = [](const char* key, json v) { json p = kCameraParams; p[key] = std::move(v); return p; };
    auto without = [](const char* key) { json p = kCameraParams; p.erase(key); return p; };
    CHECK_FALSE(camera_monitor_params_valid(json::array()));
    CHECK_FALSE(camera_monitor_params_valid(json()));
    CHECK_FALSE(camera_monitor_params_valid(with("domain", "wan")));
    CHECK_FALSE(camera_monitor_params_valid(with("domain", 1)));
    CHECK_FALSE(camera_monitor_params_valid(without("domain")));
    CHECK_FALSE(camera_monitor_params_valid(without("clientid")));
    CHECK_FALSE(camera_monitor_params_valid(with("interval", 11)));
    CHECK_FALSE(camera_monitor_params_valid(with("interval", -1)));
    CHECK_FALSE(camera_monitor_params_valid(with("interval", 1.5)));
    CHECK_FALSE(camera_monitor_params_valid(with("interval", "0")));
    CHECK_FALSE(camera_monitor_params_valid(with("expect_pw", 1)));
    CHECK_FALSE(camera_monitor_params_valid(with("extra", "x")));                                   // unknown key: refused, not forwarded
    for (const char* bad : {"", "a b", "a/b", "a\"b", "a;b", "a\nb", "é", "x:y"})
        CHECK_FALSE(camera_monitor_params_valid(with("clientid", bad)));
    CHECK_FALSE(camera_monitor_params_valid(with("clientid", std::string(65, 'a'))));
    CHECK(camera_monitor_params_valid(with("clientid", std::string(64, 'a'))));
    CHECK_FALSE(camera_monitor_params_valid(with("clientid", 5)));
}

TEST_CASE("camera bridge commands are registered, relay start/stop and unwrap the printer's result", "[SSWCP][device_ui][camera]")
{
    BridgeHarness h;
    REQUIRE(h.bridge.sswcp().is_registered("sw_CameraStartMonitor"));
    REQUIRE(h.bridge.sswcp().is_registered("sw_CameraStopMonitor"));
    h.bridge.on_script_message(h.good, camera_msg("sw_CameraStartMonitor", kCameraParams));
    REQUIRE(h.camera_calls.size() == 1);
    CHECK(h.camera_calls[0].start);
    CHECK(h.camera_calls[0].params == kCameraParams);
    REQUIRE(h.sent.size() == 1);
    CHECK(h.reply(0)["payload"]["code"] == 200);
    CHECK(h.reply(0)["payload"]["data"] == h.camera_result);                                       // the printer's result, unwrapped
    h.bridge.on_script_message(h.good, camera_msg("sw_CameraStopMonitor", {{"domain", "lan"}, {"clientid", "orca-3d-printer"}}));
    REQUIRE(h.camera_calls.size() == 2);
    CHECK_FALSE(h.camera_calls[1].start);
    REQUIRE(h.sent.size() == 2);
    CHECK(h.reply(1)["payload"]["code"] == 200);
}

TEST_CASE("a camera left running is stopped once when the page is hidden, shut down or destroyed (#76)", "[SSWCP][device_ui][camera]")
{
    const json stop_params = {{"domain", "lan"}, {"clientid", "orca-3d-printer"}};
    SECTION("hidden")
    {
        BridgeHarness h;
        h.bridge.on_script_message(h.good, camera_msg("sw_CameraStartMonitor", kCameraParams));
        REQUIRE(h.camera_calls.size() == 1);
        h.bridge.set_active(false);
        REQUIRE(h.camera_calls.size() == 2);
        CHECK_FALSE(h.camera_calls[1].start);
        CHECK(h.camera_calls[1].params == stop_params);
        h.bridge.set_active(true);
        h.bridge.set_active(false);
        h.bridge.shutdown();
        CHECK(h.camera_calls.size() == 2);                                   // already stopped: no second stop
    }
    SECTION("shutdown")
    {
        BridgeHarness h;
        h.bridge.on_script_message(h.good, camera_msg("sw_CameraStartMonitor", kCameraParams));
        h.bridge.shutdown();
        REQUIRE(h.camera_calls.size() == 2);
        CHECK_FALSE(h.camera_calls[1].start);
        CHECK(h.camera_calls[1].params == stop_params);
    }
    SECTION("the page stopped it itself: nothing more is sent")
    {
        BridgeHarness h;
        h.bridge.on_script_message(h.good, camera_msg("sw_CameraStartMonitor", kCameraParams));
        h.bridge.on_script_message(h.good, camera_msg("sw_CameraStopMonitor", stop_params));
        h.bridge.set_active(false);
        h.bridge.shutdown();
        CHECK(h.camera_calls.size() == 2);
    }
    SECTION("never started: nothing is sent")
    {
        BridgeHarness h;
        h.bridge.set_active(false);
        h.bridge.shutdown();
        CHECK(h.camera_calls.empty());
    }
}

TEST_CASE("camera bridge commands refuse bad params without asking the machine, and fail when it fails", "[SSWCP][device_ui][camera]")
{
    BridgeHarness h;
    h.bridge.on_script_message(h.good, camera_msg("sw_CameraStartMonitor", {{"domain", "wan"}, {"clientid", "x"}}));
    h.bridge.on_script_message(h.good, camera_msg("sw_CameraStartMonitor", {{"domain", "lan"}, {"clientid", "x y"}}));
    h.bridge.on_script_message(h.good, camera_msg("sw_CameraStopMonitor", json::array()));
    CHECK(h.camera_calls.empty());
    REQUIRE(h.sent.size() == 3);
    for (std::size_t i = 0; i < 3; ++i) CHECK(h.reply(i)["payload"]["code"] != 200);

    h.camera_ok = false; // the machine says no / times out
    h.bridge.on_script_message(h.good, camera_msg("sw_CameraStartMonitor", kCameraParams));
    REQUIRE(h.sent.size() == 4);
    CHECK(h.reply(3)["payload"]["code"] != 200);

    std::vector<std::string> sent; // no machine source at all
    SnapmakerDeviceBridge::Options o;
    o.port = 5000; o.http_host = "http://127.0.0.1:5000";
    o.run_script = [&sent](const std::string& js) { sent.push_back(js); };
    SnapmakerDeviceBridge none(std::move(o));
    none.on_script_message(h.good, camera_msg("sw_CameraStartMonitor", kCameraParams));
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].find("\"code\":-1") != std::string::npos);
}

TEST_CASE("a camera reply that arrives after shutdown is dropped; the timelapse commands stay denied", "[SSWCP][device_ui][camera]")
{
    BridgeHarness h;
    h.hold_camera = true;
    h.bridge.on_script_message(h.good, camera_msg("sw_CameraStartMonitor", kCameraParams));
    REQUIRE(h.camera_pending.size() == 1);
    CHECK(h.sent.empty());
    h.bridge.shutdown();
    h.camera_pending[0](true, h.camera_result);
    CHECK(h.sent.empty());
    for (const char* cmd : {"sw_DeleteCameraTimelapse", "sw_GetCameraTimelapseInstance", "sw_UploadCameraTimelapse"})
        CHECK_FALSE(h.bridge.sswcp().is_registered(cmd));
}

TEST_CASE("Moonraker camera RPC: only camera.start_monitor / camera.stop_monitor with whitelisted params are ever built", "[SSWCP][device_ui][camera]")
{
    const json start = json::parse(MoonrakerPrinterAgent::camera_monitor_request(true, kCameraParams, 7));
    CHECK(start == json{{"jsonrpc", "2.0"}, {"method", "camera.start_monitor"}, {"params", kCameraParams}, {"id", 7}});
    const json stop_params = {{"domain", "lan"}, {"clientid", "orca-3d-printer"}};
    const json stop = json::parse(MoonrakerPrinterAgent::camera_monitor_request(false, stop_params, 8));
    CHECK(stop == json{{"jsonrpc", "2.0"}, {"method", "camera.stop_monitor"}, {"params", stop_params}, {"id", 8}});
    // Refused: nothing is built, so nothing can be sent.
    CHECK(MoonrakerPrinterAgent::camera_monitor_request(true, json{{"domain", "wan"}, {"clientid", "x"}}, 1).empty());
    CHECK(MoonrakerPrinterAgent::camera_monitor_request(true, json{{"domain", "lan"}, {"clientid", "x"}, {"method", "printer.emergency_stop"}}, 1).empty());
    CHECK(MoonrakerPrinterAgent::camera_monitor_request(false, json::array(), 1).empty());

    // A refused request completes at once with ok=false (no machine is needed to refuse).
    int calls = 0; bool ok = true;
    MoonrakerPrinterAgent("").camera_monitor_async(true, json{{"domain", "lan"}, {"clientid", "bad id"}}, [&](bool o, json) { ++calls; ok = o; });
    CHECK(calls == 1);
    CHECK_FALSE(ok);
    calls = 0;
    MoonrakerPrinterAgent("").camera_monitor_async(true, kCameraParams, [&](bool o, json) { ++calls; ok = o; }); // valid, but no machine connected
    CHECK(calls == 1);
    CHECK_FALSE(ok);
}

TEST_CASE("Moonraker camera RPC reply parsing: matches the id, unwraps result, ignores notifications, rejects errors", "[SSWCP][device_ui][camera]")
{
    using R = MoonrakerPrinterAgent::CameraReply;
    json result;
    CHECK(MoonrakerPrinterAgent::parse_camera_monitor_reply(R"({"jsonrpc":"2.0","result":{"iterations":1204,"pw":"ab","salt":"cd","state":"success","url":"/files/camera/monitor.jpg"},"id":7})", 7, result) == R::Ok);
    CHECK(result["url"] == "/files/camera/monitor.jpg");
    CHECK(result["iterations"] == 1204);
    CHECK(MoonrakerPrinterAgent::parse_camera_monitor_reply(R"({"jsonrpc":"2.0","result":{"state":"success"},"id":8})", 8, result) == R::Ok);
    CHECK(result == json{{"state", "success"}});
    CHECK(MoonrakerPrinterAgent::parse_camera_monitor_reply(R"({"jsonrpc":"2.0","method":"notify_status_update","params":[{}]})", 7, result) == R::Ignore);
    CHECK(MoonrakerPrinterAgent::parse_camera_monitor_reply(R"({"jsonrpc":"2.0","result":{"state":"success"},"id":3})", 7, result) == R::Ignore); // another request's reply
    CHECK(MoonrakerPrinterAgent::parse_camera_monitor_reply("not json", 7, result) == R::Ignore);
    CHECK(MoonrakerPrinterAgent::parse_camera_monitor_reply(R"({"jsonrpc":"2.0","error":{"code":-32601,"message":"no"},"id":7})", 7, result) == R::Fail);
    CHECK(MoonrakerPrinterAgent::parse_camera_monitor_reply(R"({"jsonrpc":"2.0","result":"ok","id":7})", 7, result) == R::Fail);            // result must be an object
    CHECK(MoonrakerPrinterAgent::parse_camera_monitor_reply(R"({"jsonrpc":"2.0","result":{"state":"failed"},"id":7})", 7, result) == R::Fail); // state must be success
}

TEST_CASE("snapmaker_camera_origin: http://<bare host> of the machine, or empty", "[SSWCP][device_ui][camera]")
{
    CHECK(snapmaker_camera_origin("3d-printer") == "http://3d-printer");
    CHECK(snapmaker_camera_origin("192.168.1.5:7125") == "http://192.168.1.5");                  // the port is dropped: the snapshot is served on 80
    CHECK(snapmaker_camera_origin("https://u1.local/x") == "http://u1.local");
    CHECK(snapmaker_camera_origin("http://user:pw@u1.local:7125/x") == "http://u1.local");       // no credentials
    CHECK(snapmaker_camera_origin("[fe80::1]:7125") == "http://[fe80::1]");
    for (const char* bad : {"", "a b", "a;b", "evil.com; script-src *", "h'", "*", "a\r\nb"}) {
        CAPTURE(bad);
        CHECK(snapmaker_camera_origin(bad).empty());
    }
}

TEST_CASE("camera origin follows the selected machine: fallback to the dev id, and when the page must reload", "[SSWCP][device_ui][camera]")
{
    CHECK(snapmaker_camera_origin_for("192.168.1.5", "x") == "http://192.168.1.5");
    CHECK(snapmaker_camera_origin_for("", "3d-printer:7125") == "http://3d-printer");          // no ip: the dev id
    // The dev id fallback can be unusable: then there is no origin to serve (rejected, not passed through).
    CHECK(snapmaker_camera_origin_for("", "fe80::1").empty());                                  // unbracketed IPv6
    CHECK(snapmaker_camera_origin_for("", "my printer").empty());
    CHECK(snapmaker_camera_origin_for("", "evil.com; script-src *").empty());
    CHECK(snapmaker_camera_origin_for("", "").empty());
    // ... and the page server refuses what is not a plain origin even if one slipped through.
    // Reload decision: any change, an origin appearing or going away; no change (even both empty) means no reload.
    CHECK_FALSE(snapmaker_camera_origin_needs_reload("http://a", "http://a"));
    CHECK(snapmaker_camera_origin_needs_reload("http://a", "http://b"));
    CHECK(snapmaker_camera_origin_needs_reload("", "http://b"));                                // machine selected after the page started
    CHECK(snapmaker_camera_origin_needs_reload("http://a", ""));
    CHECK_FALSE(snapmaker_camera_origin_needs_reload("", ""));
}

TEST_CASE("camera RPC in-flight gate: one at a time", "[SSWCP][device_ui][camera]")
{
    MoonrakerPrinterAgent::InFlightGate g;
    CHECK(g.try_acquire());
    CHECK_FALSE(g.try_acquire());
    CHECK(g.busy_now());
    g.release();
    CHECK(g.try_acquire());
}

namespace {
// A one-connection websocket server on 127.0.0.1:<ephemeral>, standing in for the printer's /websocket.
struct LoopbackPrinter
{
    enum class Mode { Success, NotificationFirst, ErrorReply, Silent } mode;
    boost::asio::io_context                 ioc;
    boost::asio::ip::tcp::acceptor          acceptor{ioc, {boost::asio::ip::make_address("127.0.0.1"), 0}};
    std::thread                             thread;
    std::atomic<int>                        accepted{0};
    std::mutex                              m;
    std::string                             request;     // the first message the client sent
    std::string                             api_key;
    std::atomic<bool>                       release{false}; // Silent: keep the connection until set

    explicit LoopbackPrinter(Mode md) : mode(md)
    {
        thread = std::thread([this] {
            namespace ws = boost::beast::websocket;
            try {
                boost::asio::ip::tcp::socket sock{ioc};
                acceptor.accept(sock);
                ++accepted;
                ws::stream<boost::asio::ip::tcp::socket> conn{std::move(sock)};
                boost::beast::http::request<boost::beast::http::string_body> req;
                boost::beast::flat_buffer hb;
                boost::beast::http::read(conn.next_layer(), hb, req);
                { std::lock_guard<std::mutex> l(m); api_key = std::string(req["X-Api-Key"]); }
                conn.accept(req);
                boost::beast::flat_buffer buf;
                conn.read(buf);
                const std::string msg = boost::beast::buffers_to_string(buf.data());
                { std::lock_guard<std::mutex> l(m); request = msg; }
                const json r = json::parse(msg, nullptr, false);
                const int id = r.is_object() && r.contains("id") ? r["id"].get<int>() : -1;
                conn.text(true);
                if (mode == Mode::NotificationFirst)
                    conn.write(boost::asio::buffer(std::string(R"({"jsonrpc":"2.0","method":"notify_proc_stat_update","params":[{}]})")));
                if (mode == Mode::Success || mode == Mode::NotificationFirst)
                    conn.write(boost::asio::buffer(json{{"jsonrpc", "2.0"}, {"result", {{"state", "success"}, {"url", "/files/camera/monitor.jpg"}}}, {"id", id}}.dump()));
                else if (mode == Mode::ErrorReply)
                    conn.write(boost::asio::buffer(json{{"jsonrpc", "2.0"}, {"error", {{"code", -32601}, {"message", "nope"}}}, {"id", id}}.dump()));
                else
                    while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
                boost::beast::error_code ec;
                conn.read(buf, ec); // wait for the client's close
            } catch (...) {}
        });
    }
    ~LoopbackPrinter()
    {
        release = true;
        boost::beast::error_code ec;
        acceptor.close(ec); // unblocks a server nobody connected to
        if (thread.joinable()) thread.join();
    }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()); }
    std::string last_request() { std::lock_guard<std::mutex> l(m); return request; }
};

struct CameraResult
{
    std::promise<void> first;
    std::atomic<int>   calls{0};
    bool               ok = false;
    json               result;
    std::function<void(bool, json)> callback()
    {
        return [this](bool o, json r) { if (calls++ == 0) { ok = o; result = std::move(r); first.set_value(); } };
    }
    bool wait(std::chrono::milliseconds t) { return first.get_future().wait_for(t) == std::future_status::ready; }
};
const json kStartParams = {{"domain", "lan"}, {"interval", 0}, {"expect_pw", true}, {"clientid", "orca-3d-printer"}};
using Run = MoonrakerPrinterAgent;
} // namespace

TEST_CASE("camera RPC over a loopback websocket: success, notification first, error, silence, https, one call at a time", "[SSWCP][device_ui][camera][loopback]")
{
    using namespace std::chrono_literals;
    SECTION("success: sends exactly camera.start_monitor with the params and the api key, unwraps the result") {
        LoopbackPrinter p(LoopbackPrinter::Mode::Success);
        CameraResult r;
        Run::camera_monitor_run(p.url(), "KEY", true, kStartParams, 3000ms, std::make_shared<Run::InFlightGate>(), r.callback());
        REQUIRE(r.wait(5s));
        CHECK(r.ok);
        CHECK(r.result["url"] == "/files/camera/monitor.jpg");
        const json sent = json::parse(p.last_request());
        CHECK(sent["method"] == "camera.start_monitor");
        CHECK(sent["params"] == kStartParams);
        CHECK(sent["id"] == 1);
        { std::lock_guard<std::mutex> l(p.m); CHECK(p.api_key == "KEY"); }
        std::this_thread::sleep_for(200ms);
        CHECK(r.calls == 1);
    }
    SECTION("stop uses camera.stop_monitor") {
        LoopbackPrinter p(LoopbackPrinter::Mode::Success);
        CameraResult r;
        Run::camera_monitor_run(p.url(), "", false, json{{"domain", "lan"}, {"clientid", "orca-x"}}, 3000ms, std::make_shared<Run::InFlightGate>(), r.callback());
        REQUIRE(r.wait(5s));
        CHECK(r.ok);
        CHECK(json::parse(p.last_request())["method"] == "camera.stop_monitor");
    }
    SECTION("a notification before the reply is skipped") {
        LoopbackPrinter p(LoopbackPrinter::Mode::NotificationFirst);
        CameraResult r;
        Run::camera_monitor_run(p.url(), "", true, kStartParams, 3000ms, std::make_shared<Run::InFlightGate>(), r.callback());
        REQUIRE(r.wait(5s));
        CHECK(r.ok);
        CHECK(r.result["state"] == "success");
    }
    SECTION("an error reply fails") {
        LoopbackPrinter p(LoopbackPrinter::Mode::ErrorReply);
        CameraResult r;
        Run::camera_monitor_run(p.url(), "", true, kStartParams, 3000ms, std::make_shared<Run::InFlightGate>(), r.callback());
        REQUIRE(r.wait(5s));
        CHECK_FALSE(r.ok);
        CHECK(r.result == json::object());
    }
    SECTION("no reply: fails at the deadline, bounded, done once; a second call meanwhile is refused with no connection") {
        LoopbackPrinter p(LoopbackPrinter::Mode::Silent);
        auto gate = std::make_shared<Run::InFlightGate>();
        CameraResult r, second;
        const auto t0 = std::chrono::steady_clock::now();
        Run::camera_monitor_run(p.url(), "", true, kStartParams, 800ms, gate, r.callback());
        // The first call holds the gate: the second is answered at once, on this thread, without any new connection.
        Run::camera_monitor_run(p.url(), "", true, kStartParams, 800ms, gate, second.callback());
        CHECK(second.calls == 1);
        CHECK_FALSE(second.ok);
        REQUIRE(r.wait(5s));
        const auto took = std::chrono::steady_clock::now() - t0;
        CHECK_FALSE(r.ok);
        CHECK(took >= 700ms);
        CHECK(took < 3s);
        CHECK(p.accepted == 1);
        std::this_thread::sleep_for(100ms);
        CHECK_FALSE(gate->busy_now());   // released: the next call may go
        CHECK(r.calls == 1);
    }
    SECTION("an https machine, bad params and an empty url are refused without connecting") {
        LoopbackPrinter p(LoopbackPrinter::Mode::Success);
        CameraResult a, b, c;
        auto gate = std::make_shared<Run::InFlightGate>();
        std::string https = p.url(); https.replace(0, 4, "https");
        Run::camera_monitor_run(https, "", true, kStartParams, 2000ms, gate, a.callback());
        Run::camera_monitor_run(p.url(), "", true, json{{"domain", "wan"}, {"clientid", "x"}}, 2000ms, gate, b.callback());
        Run::camera_monitor_run("", "", true, kStartParams, 2000ms, gate, c.callback());
        for (CameraResult* x : {&a, &b, &c}) { CHECK(x->calls == 1); CHECK_FALSE(x->ok); }
        CHECK(p.accepted == 0);
        CHECK_FALSE(gate->busy_now());
    }
}

// ---- file tab (#75): sw_GetFileListPage / sw_MachineFilesRoots, read-only over GET /server/files/list and /roots ----

namespace {
json listpage_fixture()
{
    std::ifstream in((boost::filesystem::path(TEST_DATA_DIR) / "snapmaker_listpage.json").string());
    REQUIRE(in.good());
    return json::parse(in);
}
// The printer's HTTP list: {"result":[{path,modified,size,permissions}...]} in an arbitrary order, with extra fields the page must not see.
std::string http_list(std::vector<json> files)
{
    for (auto& f : files) f["extra"] = "must not leak";
    return json{{"result", files}}.dump();
}
json file(const std::string& path, double modified, long long size = 10, const std::string& perm = "rw")
{
    return {{"path", path}, {"modified", modified}, {"size", size}, {"permissions", perm}};
}
std::string list_msg(const json& params) { return camera_msg("sw_GetFileListPage", params); }
const char* kListPath = "/server/files/list?root=gcodes";
} // namespace

TEST_CASE("sw_GetFileListPage params: only root gcodes, page 0..10000, per page 1..500; bad params never fetch", "[SSWCP][device_ui][files]")
{
    BridgeHarness h;
    h.resources[kListPath] = http_list({file("a.gcode", 1)});
    const json ok = {{"root", "gcodes"}, {"page_number", 0}, {"files_per_page", 100}};
    auto with = [&](const char* k, json v) { json p = ok; p[k] = std::move(v); return p; };
    auto without = [&](const char* k) { json p = ok; p.erase(k); return p; };
    std::vector<json> bad = {json::array(), json(), with("root", "config"), with("root", "gcodes/"), with("root", "../gcodes"), with("root", 1), without("root"),
                             with("page_number", -1), with("page_number", 10001), with("page_number", 1.5), with("page_number", "0"), without("page_number"),
                             with("files_per_page", 0), with("files_per_page", 501), with("files_per_page", -3), with("files_per_page", 2.5), without("files_per_page"),
                             with("extra", 1)};
    for (const auto& p : bad) {
        CAPTURE(p.dump());
        h.sent.clear();
        h.bridge.on_script_message(h.good, list_msg(p));
        REQUIRE(h.sent.size() == 1);
        CHECK(h.reply(0)["payload"]["code"] == 400);
    }
    CHECK(h.fetched.empty());
    for (const json& p : {ok, with("page_number", 10000), with("files_per_page", 1), with("files_per_page", 500)}) {
        CAPTURE(p.dump());
        h.sent.clear();
        h.bridge.on_script_message(h.good, list_msg(p));
        REQUIRE(h.sent.size() == 1);
        CHECK(h.reply(0)["payload"]["code"] == 200);
    }
    CHECK(h.fetched.size() == 4);
    for (const auto& f : h.fetched) CHECK(f == kListPath);
}

TEST_CASE("sw_GetFileListPage sorts by modified descending (ties by path) and paginates host-side", "[SSWCP][device_ui][files]")
{
    BridgeHarness h;
    // 7 files in a scrambled order; b and c tie on modified.
    h.resources[kListPath] = http_list({file("old.gcode", 100), file("b.gcode", 500), file("newest.gcode", 900), file("c.gcode", 500),
                                        file("mid.gcode", 300), file("a.gcode", 500), file("oldest.gcode", 50)});
    const std::vector<std::string> order = {"newest.gcode", "a.gcode", "b.gcode", "c.gcode", "mid.gcode", "old.gcode", "oldest.gcode"};
    auto page = [&](int n, int per) {
        h.sent.clear();
        h.bridge.on_script_message(h.good, list_msg({{"root", "gcodes"}, {"page_number", n}, {"files_per_page", per}}));
        REQUIRE(h.sent.size() == 1);
        REQUIRE(h.reply(0)["payload"]["code"] == 200);
        return h.reply(0)["payload"]["data"];
    };
    auto paths = [](const json& d) { std::vector<std::string> v; for (const auto& f : d["files"]) v.push_back(f["path"]); return v; };
    json d = page(0, 3);
    CHECK(paths(d) == std::vector<std::string>(order.begin(), order.begin() + 3));
    CHECK(d["total"] == 7);
    CHECK(d["page_number"] == 0);
    CHECK(d["files_per_page"] == 3);
    CHECK(paths(page(1, 3)) == std::vector<std::string>(order.begin() + 3, order.begin() + 6));
    d = page(2, 3);                                           // the last, partial page
    CHECK(paths(d) == std::vector<std::string>{"oldest.gcode"});
    CHECK(d["total"] == 7);
    d = page(3, 3);                                           // beyond the end: empty, total still right
    CHECK(d["files"] == json::array());
    CHECK(d["total"] == 7);
    CHECK(paths(page(0, 7)) == order);                        // exact fit
    CHECK(paths(page(1, 7)).empty());
    CHECK(paths(page(0, 500)) == order);
    CHECK(paths(page(10000, 1)).empty());
    for (const auto& f : page(0, 7)["files"])                 // only the four fields
        CHECK(f.size() == 4);
}

TEST_CASE("sw_GetFileListPage replies the printer's own list_page shape (saved reply, first page)", "[SSWCP][device_ui][files]")
{
    const json fx = listpage_fixture();
    const json expected = fx["page0"];
    REQUIRE(expected["files"].size() == 3);
    BridgeHarness h;
    std::vector<json> files;
    for (const auto& f : expected["files"]) files.push_back(f);
    std::reverse(files.begin(), files.end());                 // the HTTP list is not sorted
    for (int i = 0; i < 133; ++i) files.push_back(file("older" + std::to_string(i) + ".gcode", 1000.0 + i)); // total 136, all older
    h.resources[kListPath] = http_list(files);
    h.bridge.on_script_message(h.good, list_msg({{"root", "gcodes"}, {"page_number", 0}, {"files_per_page", 3}}));
    REQUIRE(h.sent.size() == 1);
    CHECK(h.reply(0)["payload"]["code"] == 200);
    CHECK(h.reply(0)["payload"]["data"] == expected);         // same keys, same values, same order of files
    std::set<std::string> keys, want;
    const json got = h.reply(0)["payload"]["data"];
    for (auto it = got.begin(); it != got.end(); ++it) keys.insert(it.key());
    for (auto it = expected.begin(); it != expected.end(); ++it) want.insert(it.key());
    CHECK(keys == want);
}

TEST_CASE("sw_GetFileListPage fails soft: a failed or malformed or oversized list is never an empty success", "[SSWCP][device_ui][files]")
{
    const json p = {{"root", "gcodes"}, {"page_number", 0}, {"files_per_page", 100}};
    auto code_for = [&](const std::string* body) {
        BridgeHarness h;
        if (body) h.resources[kListPath] = *body;             // absent = the fetch fails
        h.bridge.on_script_message(h.good, list_msg(p));
        REQUIRE(h.sent.size() == 1);
        return h.reply(0)["payload"]["code"].get<int>();
    };
    CHECK(code_for(nullptr) != 200);
    for (const std::string& body : {std::string("not json"), std::string("{}"), std::string(R"({"result":{}})"), std::string(R"({"result":"x"})"), std::string("[]")})
        CHECK(code_for(&body) != 200);
    const std::string empty_list = R"({"result":[]})";        // a genuinely empty directory is a real success with total 0
    CHECK(code_for(&empty_list) == 200);
    std::string huge = R"({"result":[)" + std::string(2 * 1024 * 1024 + 1, ' ') + "]}";
    CHECK(code_for(&huge) != 200);                            // over the 2 MB cap, even if the source did not enforce it
    // No machine source at all.
    std::vector<std::string> sent;
    SnapmakerDeviceBridge::Options o;
    o.port = 5000; o.http_host = "http://127.0.0.1:5000";
    o.run_script = [&sent](const std::string& js) { sent.push_back(js); };
    SnapmakerDeviceBridge none(std::move(o));
    none.on_script_message("http://127.0.0.1:5000/web/flutter_web/index.html", list_msg(p));
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].find("\"code\":-1") != std::string::npos);
}

TEST_CASE("sw_GetFileListPage drops a reply that arrives after shutdown", "[SSWCP][device_ui][files]")
{
    BridgeHarness h;
    h.resources[kListPath] = http_list({file("a.gcode", 1)});
    h.hold_fetch = true;
    h.bridge.on_script_message(h.good, list_msg({{"root", "gcodes"}, {"page_number", 0}, {"files_per_page", 10}}));
    CHECK(h.sent.empty());
    h.bridge.shutdown();
    h.release_fetches();
    CHECK(h.sent.empty());
}

TEST_CASE("sw_GetFileListPage: requests arriving while a listing is in flight share one fetch, each answered with its own page (#78)", "[SSWCP][device_ui][files]")
{
    BridgeHarness h;
    h.resources[kListPath] = http_list({file("a.gcode", 3), file("b.gcode", 2), file("c.gcode", 1)});
    h.hold_fetch = true;
    h.bridge.on_script_message(h.good, list_msg({{"root", "gcodes"}, {"page_number", 0}, {"files_per_page", 1}}));
    h.bridge.on_script_message(h.good, list_msg({{"root", "gcodes"}, {"page_number", 1}, {"files_per_page", 1}}));
    h.bridge.on_script_message(h.good, list_msg({{"root", "gcodes"}, {"page_number", 0}, {"files_per_page", 10}}));
    CHECK(h.fetched.size() == 1);                              // the machine is asked once, not three times
    CHECK(h.sent.empty());
    h.release_fetches();
    REQUIRE(h.sent.size() == 3);
    CHECK(h.reply(0)["payload"]["data"]["files"].size() == 1);
    CHECK(h.reply(0)["payload"]["data"]["files"][0]["path"] == "a.gcode");
    CHECK(h.reply(1)["payload"]["data"]["files"][0]["path"] == "b.gcode");
    CHECK(h.reply(2)["payload"]["data"]["files"].size() == 3);
    h.hold_fetch = false;                                      // once answered, the next request fetches afresh
    h.bridge.on_script_message(h.good, list_msg({{"root", "gcodes"}, {"page_number", 0}, {"files_per_page", 1}}));
    CHECK(h.fetched.size() == 2);
    CHECK(h.sent.size() == 4);
}

TEST_CASE("sw_MachineFilesRoots replies the roots list from GET /server/files/roots, fail-soft", "[SSWCP][device_ui][files]")
{
    const json fx = listpage_fixture();
    {
        BridgeHarness h;
        h.resources["/server/files/roots"] = json{{"result", fx["roots"]}}.dump();
        h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_MachineFilesRoots"));
        REQUIRE(h.sent.size() == 1);
        CHECK(h.reply(0)["payload"]["code"] == 200);
        CHECK(h.reply(0)["payload"]["data"] == fx["roots"]);
        REQUIRE(h.fetched.size() == 1);
        CHECK(h.fetched[0] == "/server/files/roots");
    }
    BridgeHarness bad;                                        // fetch fails
    bad.bridge.on_script_message(bad.good, BridgeHarness::msg("sw_MachineFilesRoots"));
    REQUIRE(bad.sent.size() == 1);
    CHECK(bad.reply(0)["payload"]["code"] != 200);
    BridgeHarness junk;
    junk.resources["/server/files/roots"] = R"({"result":{"a":1}})";
    junk.bridge.on_script_message(junk.good, BridgeHarness::msg("sw_MachineFilesRoots"));
    REQUIRE(junk.sent.size() == 1);
    CHECK(junk.reply(0)["payload"]["code"] != 200);
}

TEST_CASE("deleting, downloading and uploading machine files stay denied", "[SSWCP][device_ui][files]")
{
    BridgeHarness h;
    for (const char* cmd : {"sw_DeleteMachineFile", "sw_DownloadMachineFile", "sw_DownLoadFile", "sw_UploadFiletoMachine", "sw_MachinePrintStart",
                            "sw_StartLocalPrint"})
        CHECK_FALSE(h.bridge.sswcp().is_registered(cmd));
}

// ---- machine control (#75): Snapmaker's control handlers forklifted into a table; only the approved ones are reachable ----

namespace {
std::string control_msg(const std::string& cmd, const json& params) { return camera_msg(cmd, params); }
} // namespace

TEST_CASE("sw_ControlLed forwards the exact printer.control.led RPC for on and off", "[SSWCP][device_ui][control]")
{
    BridgeHarness h;
    REQUIRE(h.bridge.sswcp().is_registered("sw_ControlLed"));
    for (int white : {1, 0}) {
        h.bridge.on_script_message(h.good, control_msg("sw_ControlLed", {{"name", "cavity_led"}, {"white", white}}));
        REQUIRE(h.control_calls.size() == (white ? 1u : 2u));
        CHECK(h.control_calls.back().method == "printer.control.led");
        CHECK(h.control_calls.back().params == json{{"name", "cavity_led"}, {"white", white}});
        CHECK(h.reply(h.sent.size() - 1)["payload"]["code"] == 200);
    }
    // keys the page adds are not forwarded
    h.bridge.on_script_message(h.good, control_msg("sw_ControlLed", {{"name", "cavity_led"}, {"white", 1}, {"gcode", "M112"}}));
    CHECK(h.control_calls.back().params == json{{"name", "cavity_led"}, {"white", 1}});
}

// #82: the page's light switch reads 'led cavity_led' -> color_data; the default query never asked for it, so the switch stayed ON.
TEST_CASE("the default machine-state query asks for led cavity_led and the reply carries its color_data (#82)", "[SSWCP][device_ui][control]")
{
    BridgeHarness h;
    h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_GetMachineState"));
    REQUIRE(h.queries.size() == 1);
    CHECK(std::find(h.queries[0].objects.begin(), h.queries[0].objects.end(), "led cavity_led") != h.queries[0].objects.end());
    json state = moonraker_result(59);
    state["status"]["led cavity_led"] = {{"color_data", json::array({json::array({0, 0, 0, 0})})}};
    h.queries[0].done(true, state);
    REQUIRE(h.sent.size() == 1);
    CHECK(h.reply(0)["payload"]["data"]["status"]["led cavity_led"]["color_data"] == json::array({json::array({0, 0, 0, 0})}));
}

TEST_CASE("a successful sw_ControlLed re-queries the state and pushes it to subscribers at once (#82)", "[SSWCP][device_ui][control]")
{
    BridgeHarness h;
    h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_SubscribeMachineState", "msub"));
    REQUIRE(h.queries.size() == 1);
    json on = moonraker_result(59);
    on["status"]["led cavity_led"] = {{"color_data", json::array({json::array({0.0, 0.0, 0.0, 1.0})})}};
    h.queries[0].done(true, on);
    const auto pushed = h.sent.size();

    h.bridge.on_script_message(h.good, control_msg("sw_ControlLed", {{"name", "cavity_led"}, {"white", 0}}));
    REQUIRE(h.queries.size() == 2);                       // no tick: refreshed promptly
    json off = on;
    off["status"]["led cavity_led"] = {{"color_data", json::array({json::array({0, 0, 0, 0})})}};
    h.queries[1].done(true, off);
    REQUIRE(h.sent.size() == pushed + 2);                 // the control reply and the pushed state
    const json last = h.reply(h.sent.size() - 1);
    CHECK(last["header"] == json{{"event_id", "msub"}});
    CHECK(last["payload"]["data"]["status"]["led cavity_led"]["color_data"] == json::array({json::array({0, 0, 0, 0})}));
}

TEST_CASE("a failed sw_ControlLed does not refresh the state (#82)", "[SSWCP][device_ui][control]")
{
    BridgeHarness h;
    h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_SubscribeMachineState", "msub"));
    h.queries[0].done(true, moonraker_result(59));
    h.control_ok = false;
    h.bridge.on_script_message(h.good, control_msg("sw_ControlLed", {{"name", "cavity_led"}, {"white", 0}}));
    CHECK(h.queries.size() == 1);
}

TEST_CASE("sw_ControlLed reproduces Snapmaker's messages and refuses bad values without asking the machine", "[SSWCP][device_ui][control]")
{
    BridgeHarness h;
    const auto fail_msg = [&](const json& params) {
        h.sent.clear();
        h.bridge.on_script_message(h.good, control_msg("sw_ControlLed", params));
        REQUIRE(h.sent.size() == 1);
        CHECK(h.reply(0)["payload"]["code"] == -1);
        return h.reply(0)["payload"]["message"].get<std::string>();
    };
    CHECK(fail_msg(json::object()) == "param [name] required!");
    CHECK(fail_msg({{"white", 1}}) == "param [name] required!");
    CHECK(fail_msg({{"name", "cavity_led"}}) == "param [white] required!");
    CHECK(fail_msg({{"name", 5}, {"white", 1}}) == "failure");             // get<std::string>() throws in Snapmaker
    CHECK(fail_msg({{"name", "cavity_led"}, {"white", "1"}}) == "failure");
    CHECK(fail_msg({{"name", "cavity_led"}, {"white", 1.5}}) == "failure");
    CHECK(fail_msg({{"name", "chamber_led"}, {"white", 1}}) != "failure");   // wrong LED name: rejected
    CHECK(fail_msg({{"name", "cavity_led "}, {"white", 1}}) != "failure");
    CHECK(fail_msg({{"name", "cavity_led"}, {"white", 2}}) != "failure");    // white is 0 or 1
    CHECK(fail_msg({{"name", "cavity_led"}, {"white", -1}}) != "failure");
    CHECK(fail_msg({{"name", "cavity_led"}, {"white", 255}}) != "failure");
    CHECK(h.control_calls.empty());

    h.control_ok = false; // the machine says no / times out
    h.sent.clear();
    h.bridge.on_script_message(h.good, control_msg("sw_ControlLed", {{"name", "cavity_led"}, {"white", 1}}));
    REQUIRE(h.sent.size() == 1);
    CHECK(h.reply(0)["payload"]["code"] != 200);

    std::vector<std::string> sent; // no machine source at all
    SnapmakerDeviceBridge::Options o;
    o.port = 5000; o.http_host = "http://127.0.0.1:5000";
    o.run_script = [&sent](const std::string& js) { sent.push_back(js); };
    SnapmakerDeviceBridge none(std::move(o));
    none.on_script_message(h.good, control_msg("sw_ControlLed", {{"name", "cavity_led"}, {"white", 1}}));
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].find("\"code\":-1") != std::string::npos);
}

TEST_CASE("a control reply that arrives after shutdown is dropped", "[SSWCP][device_ui][control]")
{
    BridgeHarness h;
    h.hold_control = true;
    h.bridge.on_script_message(h.good, control_msg("sw_ControlLed", {{"name", "cavity_led"}, {"white", 1}}));
    REQUIRE(h.control_pending.size() == 1);
    CHECK(h.sent.empty());
    h.bridge.shutdown();
    h.control_pending[0](true, json{{"state", "success"}});
    CHECK(h.sent.empty());
}

TEST_CASE("the late control reply is safe after the bridge is destroyed (m_alive)", "[SSWCP][device_ui][control]")
{
    std::function<void(bool, json)> late;
    std::vector<std::string>        sent;
    {
        SnapmakerDeviceBridge::Options o;
        o.port = 5000; o.http_host = "http://127.0.0.1:5000";
        o.run_script = [&sent](const std::string& js) { sent.push_back(js); };
        o.control = [&late](const std::string&, const json&, std::function<void(bool, json)> done) { late = std::move(done); };
        SnapmakerDeviceBridge bridge(std::move(o));
        bridge.on_script_message("http://127.0.0.1:5000/web/flutter_web/index.html", control_msg("sw_ControlLed", {{"name", "cavity_led"}, {"white", 0}}));
        REQUIRE(late);
    }
    late(true, json{{"state", "success"}}); // must neither crash nor reach the dead bridge's callbacks
    CHECK(sent.empty());
}

TEST_CASE("the forklifted control table is complete, uses allowlisted methods and exposes only the approved controls", "[SSWCP][device_ui][control]")
{
    BridgeHarness h;
    const std::map<std::string, std::string> expected = {
        {"sw_ControlLed", "printer.control.led"}, {"sw_ControlPrintSpeed", "printer.control.print_speed"},
        {"sw_ControlPurifier", "printer.control.purifier"}, {"sw_ControlMainFan", "printer.control.main_fan"},
        {"sw_ControlGenericFan", "printer.control.generic_fan"}, {"sw_ControlBedTemp", "printer.control.bed_temp"},
        {"sw_ControlExtruderTemp", "printer.control.extruder_temp"}, {"sw_SetDeviceName", "machine.set_device_name"},
        {"sw_BedMesh_AbortProbeMesh", "printer.bed_mesh.abort_probe_mesh"}, {"sw_MachinePrintPause", "printer.print.pause"},
        {"sw_MachinePrintResume", "printer.print.resume"}, {"sw_MachinePrintCancel", "printer.print.cancel"}};
    std::map<std::string, std::string> actual;
    for (const auto& c : snapmaker_control_table()) {
        actual[c.sw_name] = c.rpc_method;
        CHECK(control_method_allowed(c.rpc_method));
    }
    CHECK(actual == expected);
    CHECK(snapmaker_registered_control_commands() ==
          std::vector<std::string>{"sw_ControlLed", "sw_ControlPrintSpeed", "sw_ControlMainFan", "sw_ControlGenericFan", "sw_ControlBedTemp",
                                   "sw_MachinePrintPause", "sw_MachinePrintResume", "sw_MachinePrintCancel"});
    // The unreachable entries still answer "unsupported command" (501) and never reach the machine, even with valid params.
    const std::map<std::string, json> valid = {
        {"sw_ControlPurifier", {{"fan_speed", 50}}},
        {"sw_ControlExtruderTemp", {{"temp", 200}, {"index", 0}}}, {"sw_SetDeviceName", {{"name", "U1"}}},
        {"sw_BedMesh_AbortProbeMesh", json::object()}};
    for (const auto& [cmd, params] : valid) {
        CAPTURE(cmd);
        CHECK_FALSE(h.bridge.sswcp().is_registered(cmd));
        h.sent.clear();
        h.bridge.on_script_message(h.good, control_msg(cmd, params));
        REQUIRE(h.sent.size() == 1);
        CHECK(h.reply(0)["payload"]["code"] == 501);
        CHECK(h.reply(0)["payload"]["message"] == "unsupported command");
    }
    CHECK(h.control_calls.empty());
}

TEST_CASE("snapmaker_control_prepare: Snapmaker's required-param messages and strict ranges for every table entry", "[SSWCP][device_ui][control]")
{
    std::string method, err;
    json        out;
    const auto prep = [&](const std::string& cmd, const json& p) { out = json(); return snapmaker_control_prepare(cmd, p, method, out, err); };
    CHECK_FALSE(prep("sw_NoSuchThing", json::object()));
    CHECK(err == "unsupported command");
    CHECK_FALSE(prep("sw_ControlLed", json::array()));

    CHECK_FALSE(prep("sw_ControlPrintSpeed", json::object()));
    CHECK(err == "param [percentage] required!");
    CHECK(prep("sw_ControlPrintSpeed", {{"percentage", 100}}));
    CHECK(method == "printer.control.print_speed");
    CHECK(out == json{{"percentage", 100}});
    CHECK_FALSE(prep("sw_ControlPrintSpeed", {{"percentage", 0}}));
    CHECK_FALSE(prep("sw_ControlPrintSpeed", {{"percentage", 100000}}));

    CHECK_FALSE(prep("sw_ControlMainFan", json::object()));
    CHECK(err == "param [speed] required!");
    CHECK(prep("sw_ControlMainFan", {{"speed", 0}}));
    CHECK_FALSE(prep("sw_ControlMainFan", {{"speed", 101}}));
    CHECK_FALSE(prep("sw_ControlMainFan", {{"speed", -1}}));

    CHECK_FALSE(prep("sw_ControlGenericFan", {{"speed", 5}}));
    CHECK(err == "param [fan_id] required!");            // sic: Snapmaker checks "name" and reports fan_id
    CHECK_FALSE(prep("sw_ControlGenericFan", {{"name", "fan1"}}));
    CHECK(err == "param [speed] required!");
    CHECK(prep("sw_ControlGenericFan", {{"name", "cavity_fan"}, {"speed", 50}}));
    CHECK(out == json{{"name", "cavity_fan"}, {"speed", 50}});
    CHECK_FALSE(prep("sw_ControlGenericFan", {{"name", "fan1"}, {"speed", 50}})); // the name is pinned like the LED's
    CHECK_FALSE(prep("sw_ControlGenericFan", {{"name", "fan 1; M112"}, {"speed", 50}}));
    CHECK_FALSE(prep("sw_ControlGenericFan", {{"name", ""}, {"speed", 50}}));

    CHECK_FALSE(prep("sw_ControlBedTemp", json::object()));
    CHECK(err == "param [temp] required!");
    CHECK(prep("sw_ControlBedTemp", {{"temp", 60}}));
    CHECK_FALSE(prep("sw_ControlBedTemp", {{"temp", 101}}));
    CHECK_FALSE(prep("sw_ControlBedTemp", {{"temp", -5}}));

    CHECK_FALSE(prep("sw_ControlExtruderTemp", {{"index", 0}}));
    CHECK(err == "param [temp] required!");
    CHECK(prep("sw_ControlExtruderTemp", {{"temp", 200}}));
    CHECK(out == json{{"temp", 200}});                    // index / map only forwarded when given
    CHECK(prep("sw_ControlExtruderTemp", {{"temp", 200}, {"index", 3}, {"map", 1}}));
    CHECK(out == json{{"temp", 200}, {"index", 3}, {"map", 1}});
    CHECK_FALSE(prep("sw_ControlExtruderTemp", {{"temp", 301}}));
    CHECK_FALSE(prep("sw_ControlExtruderTemp", {{"temp", 200}, {"index", 4}}));

    CHECK_FALSE(prep("sw_ControlPurifier", json::object()));
    CHECK(prep("sw_ControlPurifier", {{"fan_speed", 50}, {"delay_time", 10}, {"work_time", 60}, {"evil", 1}}));
    CHECK(out == json{{"fan_speed", 50}, {"delay_time", 10}, {"work_time", 60}});
    CHECK_FALSE(prep("sw_ControlPurifier", {{"fan_speed", 500}}));

    CHECK_FALSE(prep("sw_SetDeviceName", json::object()));
    CHECK(err == "param [name] required!");
    CHECK(prep("sw_SetDeviceName", {{"name", "My U1"}}));
    CHECK(method == "machine.set_device_name");
    CHECK_FALSE(prep("sw_SetDeviceName", {{"name", ""}}));
    CHECK_FALSE(prep("sw_SetDeviceName", {{"name", std::string("a\nb")}}));
    CHECK_FALSE(prep("sw_SetDeviceName", {{"name", std::string(65, 'a')}}));

    for (const char* cmd : {"sw_BedMesh_AbortProbeMesh", "sw_MachinePrintPause", "sw_MachinePrintResume", "sw_MachinePrintCancel"}) {
        CAPTURE(cmd);
        CHECK(prep(cmd, {{"x", 1}}));
        CHECK(out == json::object());                       // no params are forwarded
    }
}

TEST_CASE("control ranges: min-1, min, max, max+1 and non-integers for every ranged param", "[SSWCP][device_ui][control]")
{
    struct Range { const char* cmd; const char* key; int lo, hi; json extra; };
    const Range ranges[] = {
        {"sw_ControlLed", "white", 0, 1, {{"name", "cavity_led"}}},
        {"sw_ControlPrintSpeed", "percentage", 50, 200, json::object()},
        {"sw_ControlMainFan", "speed", 0, 100, json::object()},
        {"sw_ControlGenericFan", "speed", 0, 100, {{"name", "cavity_fan"}}},
        {"sw_ControlBedTemp", "temp", 0, 100, json::object()},
        {"sw_ControlExtruderTemp", "temp", 0, 300, json::object()},
        {"sw_ControlExtruderTemp", "index", 0, 3, {{"temp", 200}}},
    };
    std::string method, err;
    json        out;
    for (const auto& r : ranges) {
        CAPTURE(r.cmd, r.key);
        const auto with = [&](const json& v) { json p = r.extra; p[r.key] = v; out = json(); return snapmaker_control_prepare(r.cmd, p, method, out, err); };
        CHECK_FALSE(with(r.lo - 1));
        CHECK(with(r.lo));
        CHECK(out[r.key] == r.lo);
        CHECK(with(r.hi));
        CHECK(out[r.key] == r.hi);
        CHECK_FALSE(with(r.hi + 1));
        CHECK_FALSE(with(r.lo + 0.5));
        CHECK_FALSE(with(static_cast<double>(r.lo)));  // 50.0 is not a JSON integer
        CHECK_FALSE(with(true));
        CHECK_FALSE(with(false));
        CHECK_FALSE(with("50"));
        CHECK_FALSE(with(nullptr));
    }
    CHECK_FALSE(snapmaker_control_prepare("sw_ControlGenericFan", {{"name", "cavity_fan "}, {"speed", 1}}, method, out, err));
    CHECK_FALSE(snapmaker_control_prepare("sw_ControlGenericFan", {{"name", "Cavity_Fan"}, {"speed", 1}}, method, out, err));
}

TEST_CASE("control_async request builder: allowlisted methods only, and the reply parser", "[SSWCP][device_ui][control]")
{
    using R = MoonrakerPrinterAgent::CameraReply;
    const json led = {{"name", "cavity_led"}, {"white", 1}};
    const json req = json::parse(MoonrakerPrinterAgent::control_request("printer.control.led", led, 4));
    CHECK(req["jsonrpc"] == "2.0");
    CHECK(req["method"] == "printer.control.led");
    CHECK(req["params"] == led);
    CHECK(req["id"] == 4);
    CHECK(MoonrakerPrinterAgent::control_request("printer.emergency_stop", json::object(), 1).empty());
    CHECK(MoonrakerPrinterAgent::control_request("camera.start_monitor", json::object(), 1).empty());
    CHECK(MoonrakerPrinterAgent::control_request("printer.gcode.script", {{"script", "M112"}}, 1).empty());
    CHECK(MoonrakerPrinterAgent::control_request("printer.control.led", json::array(), 1).empty());

    int calls = 0; bool ok = true;
    MoonrakerPrinterAgent("").control_async("printer.emergency_stop", json::object(), [&](bool o, json) { ++calls; ok = o; });
    CHECK(calls == 1);
    CHECK_FALSE(ok);
    MoonrakerPrinterAgent("").control_async("printer.control.led", led, [&](bool o, json) { ++calls; ok = o; }); // valid, no machine connected
    CHECK(calls == 2);
    CHECK_FALSE(ok);

    json result;
    CHECK(MoonrakerPrinterAgent::parse_control_reply(R"({"jsonrpc":"2.0","result":{"state":"success"},"id":7})", 7, result) == R::Ok);
    CHECK(result["state"] == "success");
    CHECK(MoonrakerPrinterAgent::parse_control_reply(R"({"jsonrpc":"2.0","result":"ok","id":7})", 7, result) == R::Ok);
    CHECK(result == json{{"result", "ok"}});
    CHECK(MoonrakerPrinterAgent::parse_control_reply(R"({"jsonrpc":"2.0","method":"notify_status_update","params":[{}]})", 7, result) == R::Ignore);
    CHECK(MoonrakerPrinterAgent::parse_control_reply(R"({"jsonrpc":"2.0","result":{},"id":3})", 7, result) == R::Ignore);
    CHECK(MoonrakerPrinterAgent::parse_control_reply(R"({"jsonrpc":"2.0","error":{"code":-1,"message":"no"},"id":7})", 7, result) == R::Fail);
    CHECK(MoonrakerPrinterAgent::parse_control_reply(R"({"jsonrpc":"2.0","id":7})", 7, result) == R::Fail);
    // a state other than "success" is the printer saying no, even without an error member
    CHECK(MoonrakerPrinterAgent::parse_control_reply(R"({"jsonrpc":"2.0","result":{"state":"fail"},"id":7})", 7, result) == R::Fail);
    CHECK(MoonrakerPrinterAgent::parse_control_reply(R"({"jsonrpc":"2.0","result":{"state":"error"},"id":7})", 7, result) == R::Fail);
    CHECK(MoonrakerPrinterAgent::parse_control_reply(R"({"jsonrpc":"2.0","result":{"state":true},"id":7})", 7, result) == R::Fail);
    CHECK(MoonrakerPrinterAgent::parse_control_reply(R"({"jsonrpc":"2.0","result":null,"id":7})", 7, result) == R::Fail);
    CHECK(MoonrakerPrinterAgent::parse_control_reply(R"({"jsonrpc":"2.0","result":{"state":"success"},"error":{"code":1},"id":7})", 7, result) == R::Fail);
    CHECK(MoonrakerPrinterAgent::parse_control_reply(R"({"jsonrpc":"2.0","result":{},"id":7})", 7, result) == R::Ok); // no state member: the shape of a plain ack
}

TEST_CASE("print.* control has its own gate and a 20 s timeout; the other controls keep 5 s and a gate shared with each other only", "[SSWCP][device_ui][control]")
{
    using namespace std::chrono_literals;
    for (const char* m : {"printer.print.pause", "printer.print.resume", "printer.print.cancel"}) {
        CAPTURE(m);
        CHECK(control_method_is_print(m));
        CHECK(MoonrakerPrinterAgent::control_timeout_for(m) == 20000ms);
    }
    for (const char* m : {"printer.control.led", "printer.control.print_speed", "printer.control.main_fan", "printer.control.generic_fan",
                          "printer.control.bed_temp", "printer.control.extruder_temp", "printer.bed_mesh.abort_probe_mesh"}) {
        CAPTURE(m);
        CHECK_FALSE(control_method_is_print(m));
        CHECK(MoonrakerPrinterAgent::control_timeout_for(m) == 5000ms);
    }
    MoonrakerPrinterAgent agent("");
    const auto print = agent.control_gate_for("printer.print.cancel");
    const auto other = agent.control_gate_for("printer.control.led");
    REQUIRE(print);
    REQUIRE(other);
    CHECK(print == agent.control_gate_for("printer.print.pause"));
    CHECK(print == agent.control_gate_for("printer.print.resume"));
    CHECK(other == agent.control_gate_for("printer.control.main_fan"));
    CHECK(print != other);
    CHECK(print != agent.camera_gate());
    CHECK(other != agent.camera_gate());
    // a cancel in flight neither blocks the LED nor the camera, and a second cancel is refused at once
    REQUIRE(print->try_acquire());
    CHECK(other->try_acquire());
    CHECK(agent.camera_gate()->try_acquire());
    CHECK_FALSE(print->try_acquire());
    int calls = 0; bool ok = true;
    MoonrakerPrinterAgent::control_run("ws://127.0.0.1:1", "", "printer.print.cancel", json::object(), 20000ms, print, [&](bool o, json) { ++calls; ok = o; });
    CHECK(calls == 1);
    CHECK_FALSE(ok);
}

TEST_CASE("control RPC over a loopback websocket sends exactly the allowlisted method", "[SSWCP][device_ui][control][loopback]")
{
    using namespace std::chrono_literals;
    const json led = {{"name", "cavity_led"}, {"white", 0}};
    SECTION("success") {
        LoopbackPrinter p(LoopbackPrinter::Mode::Success);
        CameraResult r;
        Run::control_run(p.url(), "KEY", "printer.control.led", led, 3000ms, std::make_shared<Run::InFlightGate>(), r.callback());
        REQUIRE(r.wait(5s));
        CHECK(r.ok);
        const json sent = json::parse(p.last_request());
        CHECK(sent["method"] == "printer.control.led");
        CHECK(sent["params"] == led);
        { std::lock_guard<std::mutex> l(p.m); CHECK(p.api_key == "KEY"); }
    }
    SECTION("an error reply fails") {
        LoopbackPrinter p(LoopbackPrinter::Mode::ErrorReply);
        CameraResult r;
        Run::control_run(p.url(), "", "printer.control.led", led, 3000ms, std::make_shared<Run::InFlightGate>(), r.callback());
        REQUIRE(r.wait(5s));
        CHECK_FALSE(r.ok);
    }
    SECTION("a method off the allowlist never opens a connection") {
        LoopbackPrinter p(LoopbackPrinter::Mode::Success);
        CameraResult r;
        Run::control_run(p.url(), "", "printer.emergency_stop", json::object(), 3000ms, std::make_shared<Run::InFlightGate>(), r.callback());
        REQUIRE(r.wait(5s));
        CHECK_FALSE(r.ok);
        CHECK(p.accepted == 0);
    }
}


// ---- print-state guard and registration of the approved controls (#79) ----

namespace {
// Answers the newest state query of the harness with a print_stats state (or a failure when `state` is null).
void answer_state_query(BridgeHarness& h, const json& state)
{
    REQUIRE_FALSE(h.queries.empty());
    auto q = std::move(h.queries.back());
    h.queries.pop_back();
    CHECK(q.objects == std::vector<std::string>{"print_stats"});
    if (state.is_null()) q.done(false, json::object());
    else q.done(true, json{{"status", {{"print_stats", {{"state", state}}}}}});
}
const std::map<std::string, std::string> kPrintCmds = {{"sw_MachinePrintPause", "printer.print.pause"},
                                                       {"sw_MachinePrintResume", "printer.print.resume"},
                                                       {"sw_MachinePrintCancel", "printer.print.cancel"}};
} // namespace

TEST_CASE("snapmaker_print_state_allows: cancel in printing/paused, pause only printing, resume only paused", "[SSWCP][device_ui][control]")
{
    for (const char* st : {"standby", "ready", "complete", "cancelled", "error", "", "Printing", "printing "}) {
        CAPTURE(st);
        for (const char* m : {"printer.print.cancel", "printer.print.pause", "printer.print.resume"})
            CHECK_FALSE(snapmaker_print_state_allows(m, st));
    }
    CHECK(snapmaker_print_state_allows("printer.print.cancel", "printing"));
    CHECK(snapmaker_print_state_allows("printer.print.cancel", "paused"));
    CHECK_FALSE(snapmaker_print_state_allows("printer.print.cancel", "standby"));
    CHECK_FALSE(snapmaker_print_state_allows("printer.print.cancel", "complete"));
    CHECK_FALSE(snapmaker_print_state_allows("printer.print.cancel", "cancelled"));
    CHECK_FALSE(snapmaker_print_state_allows("printer.print.cancel", "error"));
    CHECK_FALSE(snapmaker_print_state_allows("printer.print.cancel", ""));
    CHECK(snapmaker_print_state_allows("printer.print.pause", "printing"));
    CHECK_FALSE(snapmaker_print_state_allows("printer.print.pause", "paused"));
    CHECK_FALSE(snapmaker_print_state_allows("printer.print.pause", "standby"));
    CHECK(snapmaker_print_state_allows("printer.print.resume", "paused"));
    CHECK_FALSE(snapmaker_print_state_allows("printer.print.resume", "printing"));
    CHECK_FALSE(snapmaker_print_state_allows("printer.print.resume", "standby"));
    CHECK_FALSE(snapmaker_print_state_allows("printer.control.led", "printing")); // not a print method: never allowed through this
    CHECK_FALSE(snapmaker_print_state_allows("printer.bed_mesh.abort_probe_mesh", "printing"));
}

TEST_CASE("pause/resume/cancel through the bridge: sent only in the matching print state", "[SSWCP][device_ui][control][guard]")
{
    struct Row { const char* cmd; const char* state; bool sent; };
    const Row rows[] = {
        {"sw_MachinePrintCancel", "printing", true},  {"sw_MachinePrintCancel", "paused", true},   {"sw_MachinePrintCancel", "standby", false},
        {"sw_MachinePrintCancel", "complete", false}, {"sw_MachinePrintCancel", "cancelled", false}, {"sw_MachinePrintCancel", "error", false},
        {"sw_MachinePrintPause", "printing", true},   {"sw_MachinePrintPause", "paused", false},   {"sw_MachinePrintPause", "standby", false},
        {"sw_MachinePrintResume", "paused", true},    {"sw_MachinePrintResume", "printing", false}, {"sw_MachinePrintResume", "standby", false},
    };
    for (const auto& r : rows) {
        CAPTURE(r.cmd, r.state);
        BridgeHarness h;
        REQUIRE(h.bridge.sswcp().is_registered(r.cmd));
        h.bridge.on_script_message(h.good, control_msg(r.cmd, json::object()));
        CHECK(h.control_calls.empty());                       // nothing is sent before the state is known
        answer_state_query(h, r.state);
        if (r.sent) {
            REQUIRE(h.control_calls.size() == 1);
            CHECK(h.control_calls[0].method == kPrintCmds.at(r.cmd));
            CHECK(h.control_calls[0].params == json::object());
            REQUIRE(h.sent.size() == 1);
            CHECK(h.reply(0)["payload"]["code"] == 200);
        } else {
            CHECK(h.control_calls.empty());
            REQUIRE(h.sent.size() == 1);
            CHECK(h.reply(0)["payload"]["code"] != 0);
        }
    }
}

TEST_CASE("pause/resume/cancel fail closed when the state is unknown", "[SSWCP][device_ui][control][guard]")
{
    for (const auto& [cmd, method] : kPrintCmds) {
        CAPTURE(cmd);
        SECTION("the query fails") {
            BridgeHarness h;
            h.bridge.on_script_message(h.good, control_msg(cmd, json::object()));
            answer_state_query(h, json());
            CHECK(h.control_calls.empty());
            REQUIRE(h.sent.size() == 1);
            CHECK(h.reply(0)["payload"]["code"] != 0);
        }
        SECTION("no state source at all") {
            BridgeHarness h;
            h.have_source = false;
            h.bridge.on_script_message(h.good, control_msg(cmd, json::object()));
            CHECK(h.control_calls.empty());
            REQUIRE(h.sent.size() == 1);
            CHECK(h.reply(0)["payload"]["code"] != 0);
        }
        SECTION("the reply carries no print_stats state, or a non-string one") {
            for (const json& result : {json::object(), json{{"status", json::object()}}, json{{"status", {{"print_stats", json::object()}}}},
                                       json{{"status", {{"print_stats", {{"state", 3}}}}}}, json{{"status", {{"print_stats", {{"state", nullptr}}}}}},
                                       json("printing")}) {
                BridgeHarness h;
                h.bridge.on_script_message(h.good, control_msg(cmd, json::object()));
                REQUIRE(h.queries.size() == 1);
                h.queries[0].done(true, result);
                CHECK(h.control_calls.empty());
                REQUIRE(h.sent.size() == 1);
                CHECK(h.reply(0)["payload"]["code"] != 0);
            }
        }
    }
}

TEST_CASE("a second pause/resume/cancel while one is in flight is refused with no second RPC", "[SSWCP][device_ui][control][guard]")
{
    SECTION("while the state query is pending") {
        BridgeHarness h;
        h.bridge.on_script_message(h.good, control_msg("sw_MachinePrintCancel", json::object()));
        h.bridge.on_script_message(h.good, control_msg("sw_MachinePrintCancel", json::object()));
        REQUIRE(h.sent.size() == 1);                           // the second is answered at once
        CHECK(h.reply(0)["payload"]["code"] != 0);
        CHECK(h.reply(0)["payload"]["message"] == "failure");
        REQUIRE(h.queries.size() == 1);                        // and did not start another query
        answer_state_query(h, "printing");
        CHECK(h.control_calls.size() == 1);
    }
    SECTION("while the RPC is pending, for any print command; then a fresh request needs a fresh state query") {
        BridgeHarness h;
        h.hold_control = true;
        h.bridge.on_script_message(h.good, control_msg("sw_MachinePrintCancel", json::object()));
        answer_state_query(h, "printing");
        REQUIRE(h.control_calls.size() == 1);
        for (const char* other : {"sw_MachinePrintCancel", "sw_MachinePrintPause", "sw_MachinePrintResume"}) {
            h.bridge.on_script_message(h.good, control_msg(other, json::object()));
        }
        CHECK(h.control_calls.size() == 1);
        CHECK(h.queries.empty());
        CHECK(h.sent.size() == 3);
        h.control_pending[0](false, json::object());           // the first one times out
        REQUIRE(h.sent.size() == 4);
        CHECK(h.reply(3)["payload"]["code"] != 0);
        h.bridge.on_script_message(h.good, control_msg("sw_MachinePrintCancel", json::object()));
        CHECK(h.control_calls.size() == 1);                    // not sent until the state is known again
        REQUIRE(h.queries.size() == 1);
        answer_state_query(h, "standby");                      // the first cancel did run
        CHECK(h.control_calls.size() == 1);
    }
    SECTION("the busy flag is cleared by a failed query") {
        BridgeHarness h;
        h.bridge.on_script_message(h.good, control_msg("sw_MachinePrintPause", json::object()));
        answer_state_query(h, json());
        h.bridge.on_script_message(h.good, control_msg("sw_MachinePrintPause", json::object()));
        answer_state_query(h, "printing");
        CHECK(h.control_calls.size() == 1);
    }
}

TEST_CASE("print control is a separate gate from the camera and from the LED and other controls at the bridge", "[SSWCP][device_ui][control][guard]")
{
    BridgeHarness h;
    h.hold_control = true;
    h.bridge.on_script_message(h.good, control_msg("sw_MachinePrintPause", json::object()));
    answer_state_query(h, "printing");
    REQUIRE(h.control_calls.size() == 1);
    // while the pause is in flight: LED, speed, fan, bed temperature and the camera all go through
    h.bridge.on_script_message(h.good, control_msg("sw_ControlLed", {{"name", "cavity_led"}, {"white", 1}}));
    h.bridge.on_script_message(h.good, control_msg("sw_ControlPrintSpeed", {{"percentage", 100}}));
    h.bridge.on_script_message(h.good, control_msg("sw_ControlMainFan", {{"speed", 10}}));
    h.bridge.on_script_message(h.good, control_msg("sw_ControlBedTemp", {{"temp", 60}}));
    CHECK(h.control_calls.size() == 5);
    CHECK(h.control_calls[1].method == "printer.control.led");
    CHECK(h.control_calls[2].method == "printer.control.print_speed");
    CHECK(h.control_calls[3].method == "printer.control.main_fan");
    CHECK(h.control_calls[4].method == "printer.control.bed_temp");
    CHECK(h.queries.empty());                                  // none of those asked for the print state
    h.bridge.on_script_message(h.good, control_msg("sw_CameraStartMonitor", {{"domain", "lan"}, {"clientid", "abc"}}));
    CHECK(h.camera_calls.size() == 1);
}

TEST_CASE("the newly registered controls forward the exact RPC with no state query", "[SSWCP][device_ui][control]")
{
    BridgeHarness h;
    h.bridge.on_script_message(h.good, control_msg("sw_ControlPrintSpeed", {{"percentage", 50}}));
    h.bridge.on_script_message(h.good, control_msg("sw_ControlMainFan", {{"speed", 0}}));
    h.bridge.on_script_message(h.good, control_msg("sw_ControlGenericFan", {{"name", "cavity_fan"}, {"speed", 100}}));
    h.bridge.on_script_message(h.good, control_msg("sw_ControlBedTemp", {{"temp", 100}, {"gcode", "M112"}}));
    REQUIRE(h.control_calls.size() == 4);
    CHECK(h.control_calls[0].method == "printer.control.print_speed");
    CHECK(h.control_calls[0].params == json{{"percentage", 50}});
    CHECK(h.control_calls[1].params == json{{"speed", 0}});
    CHECK(h.control_calls[2].method == "printer.control.generic_fan");
    CHECK(h.control_calls[2].params == json{{"name", "cavity_fan"}, {"speed", 100}});
    CHECK(h.control_calls[3].params == json{{"temp", 100}});
    CHECK(h.queries.empty());
    // still denied
    for (const char* cmd : {"sw_ControlExtruderTemp", "sw_ControlPurifier", "sw_SetDeviceName", "sw_BedMesh_AbortProbeMesh"}) {
        h.sent.clear();
        h.bridge.on_script_message(h.good, control_msg(cmd, json::object()));
        REQUIRE(h.sent.size() == 1);
        CHECK(h.reply(0)["payload"]["code"] == 501);
    }
    CHECK(h.control_calls.size() == 4);
}
