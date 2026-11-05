#include <catch2/catch_test_macros.hpp>

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
    SnapmakerDeviceBridge             bridge;

    BridgeHarness()
        : bridge([this] {
              SnapmakerDeviceBridge::Options o;
              o.port       = 5000;
              o.http_host  = "http://127.0.0.1:5000";
              o.run_script = [this](const std::string& js) { sent.push_back(js); };
              o.devices    = [this] { return devices; };
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

    h.devices.push_back({"192.168.1.5:7125", "U1", "192.168.1.5", "Snapmaker U1", true});
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
