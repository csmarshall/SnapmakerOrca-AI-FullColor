// The wx-free half of the Snapmaker Device page (ADR 0005 step 1d): the SSWCP dispatcher for one webview, the
// origin check for messages from it, the local-device list it serves, and the user agent it must present.
// SnapmakerDevicePage (wx) owns one of these and only forwards events to it, so everything here is unit-tested.
//
// Threading: every member must be called on the UI thread (script messages, the 1 s timer and the DeviceManager
// all live there). The reply sink therefore runs on the UI thread too, which is what RunScript requires.
#ifndef slic3r_GUI_SnapmakerDeviceBridge_hpp_
#define slic3r_GUI_SnapmakerDeviceBridge_hpp_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "SSWCP.hpp"

namespace Slic3r { namespace GUI {

// The minimal shape served for sw_GetLocalDevices / pushed to sw_SubscribeLocalDevices. Snapmaker's page
// expects a richer record (its own machine objects); this is the smallest useful subset and is unverified
// against the page until the GUI is run.
struct SnapmakerDeviceInfo
{
    std::string dev_id;
    std::string name;
    std::string ip;
    std::string model;
    bool        online = false;
};

nlohmann::json snapmaker_devices_to_json(const std::vector<SnapmakerDeviceInfo>& devices);

// True only for http://127.0.0.1:<port>/... (the WebPageServer's own origin). Anything else - another host,
// another port, https, userinfo tricks like http://127.0.0.1:1@evil/, localhost, empty - is false.
bool snapmaker_is_page_origin(const std::string& url, uint16_t port);

// "SM-Slicer/v<version> (dark|light) Mozilla/5.0 (<platform>) AppleWebKit/605.1.15 (KHTML, like Gecko)", the
// form Snapmaker's WebView.cpp sets; the bundle (and Snapmaker's cloud) key off the SM-Slicer token.
std::string snapmaker_user_agent(const std::string& version, bool dark);

class SnapmakerDeviceBridge
{
public:
    struct Options
    {
        std::function<void(const std::string& js)>           run_script; // bound to WebView::RunScript
        std::function<std::vector<SnapmakerDeviceInfo>()>    devices;    // the current agent's machines
        uint16_t                                             port = 0;   // the page server's port
        std::string                                          http_host;  // for sw_GetSoftwareInfo
    };

    explicit SnapmakerDeviceBridge(Options options);
    SnapmakerDeviceBridge(const SnapmakerDeviceBridge&)            = delete;
    SnapmakerDeviceBridge& operator=(const SnapmakerDeviceBridge&) = delete;

    // Returns false (and does nothing) after shutdown() or when `page_url` is not the page's own origin.
    bool on_script_message(const std::string& page_url, const std::string& message);

    // Called about once a second: expires stale instances and pushes the device list if it changed.
    void tick();
    void refresh_devices();

    // Drops all subscriptions and ignores everything afterwards.
    void shutdown();

    SSWCP&       sswcp() { return m_sswcp; }
    bool         is_shut_down() const { return m_closed; }

private:
    Options        m_options;
    SSWCP          m_sswcp;
    nlohmann::json m_last_devices = nlohmann::json::array();
    bool           m_closed       = false;
};

}} // namespace Slic3r::GUI

#endif
