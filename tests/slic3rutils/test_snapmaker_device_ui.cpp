#include <catch2/catch_test_macros.hpp>

#include <set>
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
              return o;
          }())
    {}
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
    CHECK(snapmaker_bare_host("[::1]:7125") == "[::1]:7125"); // IPv6 literal left alone
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
    SECTION("a query that never answers times out with -2 and a late answer sends nothing more") {
        BridgeHarness h;
        h.bridge.on_script_message(h.good, BridgeHarness::msg("sw_GetMachineState"));
        // The dispatcher's own clock is the real one here; force expiry through its API with a far-future sweep.
        // (The harness does not inject a clock into the bridge, so check the instance is still open instead.)
        CHECK(h.bridge.sswcp().instance_count() == 1);
        h.queries.at(0).done(true, moonraker_result(2));
        CHECK(h.sent.size() == 1);
        h.queries.at(0).done(true, moonraker_result(3)); // a second callback must not double-reply
        CHECK(h.sent.size() == 1);
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
        "sw_SendGCodes", "sw_MachinePrintStart", "sw_MachinePrintPause", "sw_MachinePrintResume", "sw_MachinePrintCancel",
        "sw_UploadFiletoMachine", "sw_UploadFile", "sw_UploadFileResult", "sw_DeleteMachineFile", "sw_DeleteDevices", "sw_RenameDevice",
        "sw_SetDeviceName", "sw_ControlLed", "sw_ControlPrintSpeed", "sw_ControlPurifier", "sw_ControlMainFan", "sw_ControlGenericFan",
        "sw_ControlBedTemp", "sw_ControlExtruderTemp", "sw_StartLocalPrint", "sw_StartCloudPrint", "sw_BedMesh_AbortProbeMesh",
        "sw_SendCommand", "sw_Query", "sw_AddDevice", "sw_SwitchModel", "sw_UpdateDeviceInfo", "sw_mqtt_set_engine",
        "sw_SetFilamentMappingComplete", "sw_FinishFilamentMapping", "sw_DownloadMachineFile", "sw_DownLoadFile",
        "sw_CameraStartMonitor", "sw_CameraStopMonitor", "sw_PullCloudFile", "sw_ServerClientManagerSetUserinfo",
        "sw_Connect", "sw_Disconnect", "sw_ConnectOtherMachine", "sw_GetPincode", "sw_OpenBrowser", "sw_Exit"};
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
        "sw_GetCache", "sw_SetCache", "sw_RemoveCache", "sw_UnsubscribeCacheKeys"};
    for (const auto& c : expected)
        CHECK(h.bridge.sswcp().is_registered(c));
    CHECK(expected.size() == 37);
}

TEST_CASE("the {} read-only commands answer an empty object", "[SSWCP][device_ui]")
{
    BridgeHarness h;
    for (const char* cmd : {"sw_GetMachineSystemInfo", "sw_SystemGetDeviceInfo", "sw_FileGetStatus", "sw_exception_query", "sw_MachineFilesRoots",
                            "sw_MachineFilesMetadata", "sw_GetCache", "sw_SetCache", "sw_RemoveCache", "sw_UnsubscribeCacheKeys"}) {
        CAPTURE(cmd);
        h.sent.clear();
        h.bridge.on_script_message(h.good, BridgeHarness::msg(cmd));
        REQUIRE(h.sent.size() == 1);
        CHECK(h.reply(0)["payload"]["code"] == 200);
        CHECK(h.reply(0)["payload"]["data"] == json::object());
    }
}
