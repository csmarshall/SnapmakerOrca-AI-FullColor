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

// One machine as the bridge knows it. snapmaker_devices_to_json() turns it into the record the Flutter page
// builds a device card from (docs/snapmaker-page-contract.md section 3; the page rejects a record without `sn`).
struct SnapmakerDeviceInfo
{
    std::string dev_id;        // the DeviceManager id (host[:port] for a host-based agent)
    std::string sn;            // the printer's real serial if known; empty falls back to dev_id (see below)
    std::string name;
    std::string ip;            // as stored: may carry a scheme or port; the record gets the bare host
    std::string model;         // the printer preset's printer_model, e.g. "Snapmaker U1"
    bool        online    = false;
    bool        connected = false; // the machine the host currently has selected/connected

    std::string error;   // why the last connect attempt failed (IPrinterAgent::last_connect_error()); empty when fine
};

// The page's per-device record. Keys (all read by the page's DeviceModel; `sn` is the one it requires):
//   sn            non-empty. The machine's serial when SnapmakerDeviceInfo::sn has one, else dev_id: nothing in
//                 the agent or DeviceManager exposes the printer's real serial yet, and the page only needs a
//                 stable non-empty key. Replace by filling SnapmakerDeviceInfo::sn when the agent learns it.
//   id, dev_id    dev_id
//   name, device_name   the machine name (dev_id when blank)
//   ip            bare host (scheme, path and port removed)
//   port          1884, the value of the record that rendered a card in the harness; nothing here listens there
//   clientId      "orca-<sn>"
//   link_mode     "lan"
//   machine_type  model, "" when the preset names none
//   connected, online, error
nlohmann::json snapmaker_devices_to_json(const std::vector<SnapmakerDeviceInfo>& devices);
nlohmann::json snapmaker_device_to_json(const SnapmakerDeviceInfo& device);
// The host part of an address as typed or stored: "http://u1.local:7125/x" -> "u1.local".
std::string snapmaker_bare_host(const std::string& address);

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
