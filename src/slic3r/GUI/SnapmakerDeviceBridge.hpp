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
#include <map>
#include <memory>
#include <optional>
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
// "http://<bare host>" of a machine address, the one extra origin the page may load the camera snapshot from; "" if the host is
// not a plain DNS name, IPv4 address or bracketed IPv6 literal (no credentials, port, path or anything CSP-significant).
std::string snapmaker_camera_origin(const std::string& address);

// True only for http://127.0.0.1:<port>/... (the WebPageServer's own origin). Anything else - another host,
// another port, https, userinfo tricks like http://127.0.0.1:1@evil/, localhost, empty - is false.
bool snapmaker_is_page_origin(const std::string& url, uint16_t port);

// "SM-Slicer/v<version> (dark|light) Mozilla/5.0 (<platform>) AppleWebKit/605.1.15 (KHTML, like Gecko)", the
// form Snapmaker's WebView.cpp sets; the bundle (and Snapmaker's cloud) key off the SM-Slicer token.
std::string snapmaker_user_agent(const std::string& version, bool dark);

// The host tabs the page can ask for with sw_SwitchTab {target}. Names are Snapmaker's (case-insensitive); the page
// adapter maps each to the tab of the same meaning. "Auxiliary" and "DebugTool" have no counterpart here.
enum class SnapmakerTab { Home, Prepare, Preview, Monitor, MultiDevice, Project, Calibration };
std::optional<SnapmakerTab> snapmaker_tab_for_target(const std::string& target);

// What an empty machine state looks like: Moonraker's printer.objects.query result with nothing in it.
nlohmann::json snapmaker_empty_machine_state();

// The Klipper objects asked for when the page names none (the set the 2.3.40 Device page reads, contract doc 7.3).
const std::vector<std::string>& snapmaker_default_state_objects();
// Which Klipper objects the page may have queried: the 2.3.40 Device page's set (contract doc 7.3) plus the extruder,
// fan, LED, heater and sensor families. configfile, bed_mesh, gcode macros and the rest are refused.
bool snapmaker_state_object_allowed(const std::string& name);
// The sw_GetMachineState reply: the result unchanged (or the empty state), plus an empty entry per refused object name.
nlohmann::json snapmaker_state_reply(bool ok, nlohmann::json result, const std::vector<std::string>& rejected);

// ---- pure helpers for the read-only machine resources (unit-tested) ----

// machine.system_info result -> product_info.serial_number ("" when absent or not a string). Accepts the bare result
// ({system_info:{...}}) or a Moonraker HTTP body ({result:{system_info:{...}}}).
std::string snapmaker_serial_from_system_info(const nlohmann::json& body);
// The machine.system_info `result` object the page expects ({system_info:{...}}), or {} when `body` has none.
nlohmann::json snapmaker_system_info_result(const nlohmann::json& body);
// A gcode file name the page may ask about: 1..512 bytes, no control characters, no backslash, not absolute, no ".."
// segment. Used before a name goes into a URL.
bool snapmaker_valid_gcode_name(const std::string& name);
// Percent-encodes everything but unreserved characters (and '/' when keep_slash), for a query value or URL path.
std::string snapmaker_percent_encode(const std::string& s, bool keep_slash);
// A thumbnail_path from /server/files/thumbnails: relative, valid name, no '?', '#', ':' (so it cannot become another URL).
bool snapmaker_valid_thumbnail_path(const std::string& path);
std::string snapmaker_base64(const std::string& bytes);
constexpr std::size_t SNAPMAKER_THUMBNAIL_MAX_BYTES = 2 * 1024 * 1024;
// The thumbnails_base64-shaped reply {width,height,size,data,state,path}; {state:"error",...} when the image is empty or over the cap.
nlohmann::json snapmaker_thumbnail_reply(const std::string& path, int width, int height, const std::string& image);
nlohmann::json snapmaker_thumbnail_error(const std::string& path);
// Of a /server/files/thumbnails result list, the entry with the most pixels whose reported size fits the cap ({} if none).
nlohmann::json snapmaker_pick_thumbnail(const nlohmann::json& list);

// The bridge adds the device-oriented commands to the SSWCP core's 13. All READ-ONLY. Bridge-level emulation (ADR 0005,
// decision 2 amendment): the page believes it talks to a machine over MQTT; here it is answered from the Moonraker
// connection the app already has, and the sw_mqtt_* commands are accepted and ignored. Anything that could change printer
// state (G-code, print control, uploads, deletes, renames, firmware, fan/heater/LED control) is deliberately NOT registered
// and gets the core's 501; test_snapmaker_device_ui.cpp pins that list.
class SnapmakerDeviceBridge
{
public:
    struct Options
    {
        std::function<void(const std::string& js)>           run_script; // bound to WebView::RunScript
        std::function<std::vector<SnapmakerDeviceInfo>()>    devices;    // the current agent's machines
        uint16_t                                             port = 0;   // the page server's port
        std::string                                          http_host;  // for sw_GetSoftwareInfo
        // Read-only fetch of Klipper objects from the selected machine, without blocking. `done(ok, result)` MUST be
        // called on the UI thread (the adapter hops threads), exactly once. Null = no machine source: state replies are
        // the empty state.
        std::function<void(const std::vector<std::string>& objects, std::function<void(bool ok, nlohmann::json result)> done)> query_state;
        // Read-only GET of one allow-listed machine resource (IPrinterAgent::read_machine_resource_async); `done(ok, body)`
        // on the UI thread, exactly once. Null = no machine source.
        std::function<void(const std::string& path_and_query, std::size_t max_bytes, std::function<void(bool ok, std::string body)> done)> fetch_resource;
        // Camera preview: IPrinterAgent::camera_monitor_async (start or stop the machine's monitor stream) with the page's
        // whitelisted params; `done(ok, result)` on the UI thread, exactly once. Null = no machine source (the commands fail).
        std::function<void(bool start, const nlohmann::json& params, std::function<void(bool ok, nlohmann::json result)> done)> camera_monitor;
        // Makes the host show a tab (sw_SwitchTab). Returns false if it cannot.
        std::function<bool(SnapmakerTab)>                    switch_tab;
    };

    explicit SnapmakerDeviceBridge(Options options);
    SnapmakerDeviceBridge(const SnapmakerDeviceBridge&)            = delete;
    SnapmakerDeviceBridge& operator=(const SnapmakerDeviceBridge&) = delete;
    ~SnapmakerDeviceBridge() { *m_alive = false; }

    // Returns false (and does nothing) after shutdown() or when `page_url` is not the page's own origin.
    bool on_script_message(const std::string& page_url, const std::string& message);

    // Called about once a second: expires stale instances and pushes the device list if it changed.
    void tick();
    void refresh_devices();

    // Drops all subscriptions and ignores everything afterwards.
    void shutdown();

    // The page is shown (true) or hidden (false). While hidden, tick() polls nothing and pushes nothing; showing it again
    // refreshes the device list and queues a state push for the subscribers.
    void set_active(bool active);

    // Latest sw_UpdateMachineFilamentInfo value per sn (the page's JSON string, untouched). Stored only; nothing consumes it yet.
    const std::map<std::string, std::string>& filament_info() const { return m_filament_info; }

    SSWCP&       sswcp() { return m_sswcp; }
    bool         is_shut_down() const { return m_closed; }

private:
    Options        m_options;
    std::map<std::string, std::string>     m_serials;      // dev_id -> serial_number (declared before m_sswcp: its services read it)
    std::map<std::string, nlohmann::json>  m_system_info;  // dev_id -> machine.system_info result
    SSWCP          m_sswcp;
    // Throttled machine-state pushes: every kStatePushEveryTicks ticks (tick() is about 1 s) while someone subscribes.
    static constexpr int kStatePushEveryTicks = 3;
    void register_commands();
    // The objects a request names, split by the allow-list. `requested` is false when it named none.
    struct Selection { std::vector<std::string> allowed, rejected; bool requested = false; };
    Selection select_objects(const nlohmann::json& params) const;
    void run_query(const std::vector<std::string>& objects, std::function<void(bool, nlohmann::json)> done);
    void request_state(const std::vector<std::string>& objects, std::function<void(bool, nlohmann::json)> cb);
    void abandon_state_query();
    void push_state(bool force);
    std::string connected_dev_id() const;

    // dev_id -> machine.system_info result / serial, filled by read-only fetches; a record's sn is the serial once known.
    void fetch_system_info(const std::string& dev_id, std::function<void()> then);
    std::vector<SnapmakerDeviceInfo> current_devices() const;
    int                                 m_serial_retry_at = 0;
    std::map<std::string, std::vector<std::function<void()>>> m_sysinfo_waiters; // dev_id -> callbacks of a fetch in flight

    nlohmann::json                      m_last_devices = nlohmann::json::array();
    bool                                m_closed       = false;
    std::shared_ptr<bool>               m_alive        = std::make_shared<bool>(true); // delayed replies check it
    std::vector<std::string>            m_state_objects; // the subscription's / SetSubscribeFilter's object names
    nlohmann::json                      m_last_state_status; // last pushed `status`
    bool                                m_state_in_flight = false;
    std::vector<std::function<void(bool, nlohmann::json)>> m_state_waiters; // everyone waiting on the query in flight
    std::uint64_t                       m_state_gen = 0;           // a callback of an older generation is a late one: ignored
    int                                 m_state_started_tick = 0;
    static constexpr int                kStateWatchdogTicks  = 15; // a lost callback cannot wedge polling for longer
    bool                                m_push_waiting = false;    // a push is queued on the query in flight
    bool                                m_force_push   = false;    // push the next result even if unchanged (new subscriber)
    std::string                         m_state_machine;           // the machine the cached state belongs to
    bool                                m_active = true;           // false while the page is hidden: no polling, no pushes
    int                                 m_ticks           = 0;
    std::map<std::string, std::string>  m_filament_info;
};

}} // namespace Slic3r::GUI

#endif
