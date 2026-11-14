#ifndef __I_PRINTER_AGENT_HPP__
#define __I_PRINTER_AGENT_HPP__

#include "bambu_networking.hpp"
// why: these extend the BAMBU_NETWORK_* return space rather than opening a new one - the value
// flows through the same int domain callers already compare against BAMBU_NETWORK_SUCCESS.
// They live here and not in bambu_networking.hpp because that file is a vendor header replaced
// wholesale by header-sync commits (see c09252ce11), which would silently clobber them.
// -70xx is free: the vendor occupies -1..-25 and -10xx through -60xx.
#define ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED -7010 // no translation exists for this command
#define ORCA_NETWORK_ERR_CAP_NOT_AVAILABLE -7020 // a translation exists; this printer lacks the capability
#include <string>
#include <memory>
#include <vector>
#include <functional>
#include <cmath>
#include <nlohmann/json.hpp>
#include <boost/format.hpp>

namespace Slic3r {

class ICloudServiceAgent;

/**
 * AgentInfo - Metadata structure for printer agent information.
 *
 * Contains identification and descriptive information about a printer agent
 * implementation, used for discovery and selection purposes.
 */
struct AgentInfo {
    std::string id;         ///< Unique identifier for the agent, e.g. "orca", "bbl"
    std::string name;       ///< Human-readable agent name, e.g. "Orca", "Bambu Lab"
    std::string version;    ///< Agent version string, e.g. "1.0.0"
    std::string description; ///< Brief description of the agent's capabilities, e.g. "Orca printer agent"
};

struct PrinterConnectionParams
{
    std::string dev_id;
    std::string host; // host address, usually the IP address without the http/https protocol
    std::string port; // optional
    std::string username;
    std::string password;
    bool use_ssl = false; // indicates if http or https
    std::string ca_file;
};

/**
 * FilamentSyncMode - Modes for filament data synchronization.
 *
 * Defines how filament information is obtained from the printer:
 * - Subscription: Real-time push updates (e.g., MQTT subscriptions)
 * - Pull: On-demand fetch via REST API (blocking call)
 * - None: Filament sync unavailable
 */
enum class FilamentSyncMode {
    none = 0,     ///< Filament synchronization not supported
    subscription, ///< Real-time push updates via subscription (e.g., MQTT)
    pull          ///< On-demand fetch via REST API (blocking call)
};

enum class CameraStreamMode {
    none = 0,
    http,  // LAN or Cloud
    https, // LAN or Cloud over TLS
    rtsp,  // LAN only
    webrtc, // Cloud only
    http_snapshot // HTTP endpoint returning one image per request
};

// Which Device-tab experience an agent's printers use. Declared by the agent (and so by the printer
// preset that names it), never inferred from a printer model.
enum class DeviceUi {
    Native,        // upstream's Device tab (MonitorPanel)
    SnapmakerWeb,  // Snapmaker's Flutter Home/Device page, hosted in a webview and bridged by SSWCP
};

/**
 * IPrinterAgent - Interface for printer operations.
 *
 * This interface encapsulates all printer-related functionality:
 * - Direct printer communication (LAN and cloud relay)
 * - Certificate management
 * - Device discovery (SSDP)
 * - Printer binding/unbinding
 * - Print job operations
 *
 * Implementations:
 * - OrcaPrinterAgent: Stub implementation (printer ops not yet supported)
 * - PrinterAgentPluginCapability: Python printer-agent plugin capability that
 *   implements IPrinterAgent directly and is handed out as the live agent
 * - BBLPrinterAgent: Wrapper around Bambu Lab's proprietary DLL
 *
 * Token Access:
 * Printer agents receive an ICloudServiceAgent instance via set_cloud_agent() to
 * access tokens for cloud-relay operations.
 */

class IPrinterAgent {
public:
    virtual ~IPrinterAgent() = default;

    // Cloud Agent Dependency
    // ========================================================================
    /**
     * Set the cloud agent used for token access.
     * Must be called before any cloud-relay operations.
     */
    virtual void set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud) = 0;

    // ========================================================================
    // Communication
    // ========================================================================
    /**
     * Publish a JSON command to a printer through cloud relay.
     */
    virtual int send_message(std::string dev_id, std::string json_str, int qos, int flag) = 0;

    // why: gcode is firmware dialect, not a waist concept - commands whose body is Bambu-dialect
    // gcode live on the agent that speaks it; the default is an honest refusal that MachineObject's
    // publish funnel turns into a dialog.
    virtual int command_ams_refresh_rfid(std::string, int, int, int, bool)
    { return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED; }
    virtual int command_ams_calibrate(std::string, int, int, bool)
    { return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED; }
    virtual int command_ams_select_tray(std::string, std::string, int, bool)
    { return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED; }
    virtual int command_start_camera(std::string)
    { return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED; }

private:
    int publish_command_json(std::string dev_id, const nlohmann::json& j, bool lan_mode)
    {
        return lan_mode ? send_message_to_printer(dev_id, j.dump(), 0, 0)
                         : send_message(dev_id, j.dump(), 0, 0);
    }

public:
    virtual int command_xyz_abs(std::string dev_id, int sequence_id, bool lan_mode)
    {
        nlohmann::json j;
        j["print"]["command"]     = "gcode_line";
        j["print"]["param"]       = "G90 \n";
        j["print"]["sequence_id"] = std::to_string(sequence_id);
        return publish_command_json(dev_id, j, lan_mode);
    }
    virtual int command_auto_leveling(std::string dev_id, int sequence_id, bool lan_mode)
    {
        nlohmann::json j;
        j["print"]["command"]     = "gcode_line";
        j["print"]["param"]       = "G29 \n";
        j["print"]["sequence_id"] = std::to_string(sequence_id);
        return publish_command_json(dev_id, j, lan_mode);
    }
    virtual int command_go_home(std::string dev_id, bool is_printing, bool supports_mqtt_homing, int sequence_id, bool lan_mode)
    {
        nlohmann::json j;
        j["print"]["sequence_id"] = std::to_string(sequence_id);
        if (supports_mqtt_homing) {
            j["print"]["command"] = "back_to_center";
        } else {
            j["print"]["command"] = "gcode_line";
            j["print"]["param"]   = is_printing ? "G28 X\n" : "G28 \n";
        }
        return publish_command_json(dev_id, j, lan_mode);
    }
    virtual int command_set_bed(std::string dev_id, int temp, bool supports_mqtt_bed_ctrl, int sequence_id, bool lan_mode)
    {
        nlohmann::json j;
        j["print"]["sequence_id"] = std::to_string(sequence_id);
        if (supports_mqtt_bed_ctrl) {
            j["print"]["command"] = "set_bed_temp";
            j["print"]["temp"]    = temp;
        } else {
            j["print"]["command"] = "gcode_line";
            j["print"]["param"]   = (boost::format("M140 S%1%\n") % temp).str();
        }
        return publish_command_json(dev_id, j, lan_mode);
    }
    virtual int command_set_nozzle(std::string dev_id, int temp, int sequence_id, bool lan_mode)
    {
        nlohmann::json j;
        j["print"]["command"]     = "gcode_line";
        j["print"]["param"]       = (boost::format("M104 S%1%\n") % temp).str();
        j["print"]["sequence_id"] = std::to_string(sequence_id);
        return publish_command_json(dev_id, j, lan_mode);
    }

    virtual int command_axis_control(std::string dev_id, std::string axis, double unit, double input_val, int speed,
                                      bool is_core_xy, bool supports_mqtt_axis_control, int sequence_id, bool lan_mode)
    {
        (void) supports_mqtt_axis_control;

        double value = input_val;
        if (!is_core_xy && (axis == "Y" || axis == "Z"))
            value = -input_val;

        std::string value_str = (boost::format("%.1f") % (value * unit)).str();
        std::string gcode;
        if (axis == "X" || axis == "Y" || axis == "Z") {
            gcode = (boost::format("G91 \nG1 %1%%2% F%3%\nG90 \n") % axis % value_str % speed).str();
        } else if (axis == "E") {
            gcode = (boost::format("M83 \nG0 %1%%2% F%3%\n") % axis % value_str % speed).str();
        } else {
            return -1;
        }

        nlohmann::json j;
        j["print"]["command"]     = "gcode_line";
        j["print"]["param"]       = gcode;
        j["print"]["sequence_id"] = std::to_string(sequence_id);
        return publish_command_json(dev_id, j, lan_mode);
    }

    /**
     * Default LAN account username for this agent's protocol, if it has a fixed one.
     * Returns an empty string if the agent has no fixed default (e.g. caller must supply one).
     */
    virtual std::string default_lan_username() const { return {}; }

    /**
     * Establish a direct LAN connection to a printer.
     */
    virtual int connect_printer(const PrinterConnectionParams& params) = 0;

    /**
     * Tear down the active LAN printer connection.
     */
    virtual int disconnect_printer() = 0;

    /**
     * Send a JSON command to a LAN printer (bypassing cloud).
     */
    virtual int send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) = 0;

    // ========================================================================
    // Certificates
    // ========================================================================
    /**
     * Validate current user certificates for the printer.
     */
    virtual int check_cert() { return BAMBU_NETWORK_SUCCESS; }

    /**
     * Install or refresh device certificate for LAN TLS.
     */
    virtual void install_device_cert(std::string dev_id, bool lan_only)
    {
        (void) dev_id;
        (void) lan_only;
    }

    // ========================================================================
    // Discovery
    // ========================================================================
    /**
     * Start or stop SSDP discovery.
     */
    virtual bool start_discovery(bool start, bool sending) = 0;

    // ========================================================================
    // Binding
    // ========================================================================
    /**
     * Ping the binding endpoint to check printer readiness.
     */
    virtual int ping_bind(std::string ping_code)
    {
        (void) ping_code;
        return BAMBU_NETWORK_SUCCESS;
    }

    /**
     * Perform binding detection/handshake on a LAN printer.
     */
    virtual int bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect) = 0;

    /**
     * Execute the multi-stage printer binding workflow.
     */
    virtual int bind(std::string dev_ip, std::string dev_id, std::string dev_model, std::string sec_link,
                     std::string timezone, bool improved, OnUpdateStatusFn update_fn)
    {
        (void) dev_ip;
        (void) dev_id;
        (void) dev_model;
        (void) sec_link;
        (void) timezone;
        (void) improved;
        (void) update_fn;
        return BAMBU_NETWORK_SUCCESS;
    }

    /**
     * Remove the association between account and printer.
     */
    virtual int unbind(std::string dev_id)
    {
        (void) dev_id;
        return BAMBU_NETWORK_SUCCESS;
    }

    /**
     * Request a one-time bind ticket from the server.
     */
    virtual int request_bind_ticket(std::string* ticket)
    {
        if (ticket)
            *ticket = {};
        return BAMBU_NETWORK_SUCCESS;
    }

    /**
     * Fetch the cloud snapshot image captured at a print failure.
     * Returns 0 if the request was dispatched; the image body arrives via callback(body, http_status).
     */
    virtual int get_hms_snapshot(std::string dev_id, std::string file_name,
                                 std::function<void(std::string, int)> callback)
    {
        (void) dev_id;
        (void) file_name;
        (void) callback;
        return -1;
    }

    /**
     * Register callback for fatal HTTP errors.
     */
    virtual int set_server_callback(OnServerErrFn fn) = 0;

    // ========================================================================
    // Machine Selection
    // ========================================================================
    /**
     * Return the currently selected printer ID.
     */
    virtual std::string get_user_selected_machine() = 0;

    /**
     * Update the selected cloud machine preference.
     */
    virtual int set_user_selected_machine(std::string dev_id) = 0;

    // ========================================================================
    // Subscriptions
    // ========================================================================
    /**
     * Subscribe to a logical module (for example app- or tunnel-scoped streams).
     */
    virtual int start_subscribe(std::string module) { (void) module; return BAMBU_NETWORK_SUCCESS; }

    /**
     * Stop listening to a formerly subscribed module.
     */
    virtual int stop_subscribe(std::string module) { (void) module; return BAMBU_NETWORK_SUCCESS; }

    /**
     * Subscribe to push streams for specific device identifiers.
     */
    virtual int add_subscribe(std::vector<std::string> dev_list) { (void) dev_list; return BAMBU_NETWORK_SUCCESS; }

    /**
     * Remove device-level subscriptions.
     */
    virtual int del_subscribe(std::vector<std::string> dev_list) { (void) dev_list; return BAMBU_NETWORK_SUCCESS; }

    // ========================================================================
    // Print Job Operations
    // ========================================================================
    /**
     * Start a fully managed cloud print.
     */
    virtual int start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) = 0;

    /**
     * Start a local print with cloud record.
     */
    virtual int start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) = 0;

    /**
     * Upload gcode to printer's SD card without starting.
     */
    virtual int start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) = 0;

    /**
     * Start a LAN-only print.
     */
    virtual int start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) = 0;

    /**
     * Start a print from printer's SD card.
     */
    virtual int start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) = 0;

    // ========================================================================
    // Callback Registration
    // ========================================================================
    /**
     * Register SSDP discovery callback.
     */
    virtual int set_on_ssdp_msg_fn(OnMsgArrivedFn fn) = 0;

    /**
     * Register printer MQTT connection callback.
     */
    virtual int set_on_printer_connected_fn(OnPrinterConnectedFn fn) = 0;

    /**
     * Register subscription failure callback.
     */
    virtual int set_on_subscribe_failure_fn(GetSubscribeFailureFn fn) = 0;

    /**
     * Register cloud device message callback.
     */
    virtual int set_on_message_fn(OnMessageFn fn) = 0;

    /**
     * Register user-scoped message callback.
     */
    virtual int set_on_user_message_fn(OnMessageFn fn) = 0;

    /**
     * Register LAN connection status callback.
     */
    virtual int set_on_local_connect_fn(OnLocalConnectedFn fn) = 0;

    /**
     * Register LAN message callback.
     */
    virtual int set_on_local_message_fn(OnMessageFn fn) = 0;

    /**
     * Provide main thread queue callback.
     */
    virtual int set_queue_on_main_fn(QueueOnMainFn fn) = 0;

    /**
     * Get agent information.
     */
    virtual AgentInfo get_agent_info() = 0;

    // ========================================================================
    // Filament Operations
    // ========================================================================
    /**
     * Get the filament synchronization mode for this agent.
     * 
     * @return FilamentSyncMode indicating how filament data is obtained:
     *         - subscription: Real-time push updates via MQTT (no fetch needed)
     *         - pull: On-demand fetch via REST API (call fetch_filament_info())
     *         - none: Filament synchronization not supported
     */
    virtual FilamentSyncMode get_filament_sync_mode() const { return FilamentSyncMode::none; }

    /**
     * Get the camera stream mode for this agent. This value can be deterministic and derived at
     * runtime if the printer supports multiple camera stream modes. E.g. LAN => HTTP/HTTPS/RTSP, Cloud => WebRTC.
     *
     * @return CameraStreamMode indicating how the camera stream is obtained or used:
     */
    virtual CameraStreamMode get_camera_stream_mode() const { return CameraStreamMode::none; }

    /**
     * Which UI the Device tab shows while this agent is the live one. Native unless an agent says otherwise.
     */
    virtual DeviceUi device_ui() const { return DeviceUi::Native; }

    /**
     * True when this agent reaches its printers by a host (and optional port) that the user types in, held in the printer
     * preset's print_host / printhost_port / printhost_apikey (the Moonraker family). Such an agent
     *  - needs the physical-printer (Connection) dialog, since nothing else can supply the host;
     *  - identifies a machine by MachineObject::dev_id_from_address(host, port), so one host is one machine;
     *  - treats the API key as optional: a blank key is sent as no key, never as a made-up access code.
     * Bambu-style agents (access code mandatory, serial-number ids) and agents that discover their own devices return false.
     */
    virtual bool needs_print_host() const { return false; }

    /**
     * True when this agent's printers are sent to through the page-driven pre-print dialog (the bundled Snapmaker pre-print page in a
     * non-modal window, docs/records/snapmaker-u1-send-flow-design.md section 4) rather than the plain print-host send dialog. A capability,
     * never a model-name check (ADR 0003). Only the Snapmaker agent says yes: generic Moonraker, OctoPrint-style and Bambu agents do not.
     */
    virtual bool supports_preprint_page() const { return false; }

    /**
     * The reason the last connect_printer() attempt failed, empty while connecting, once connected, or when nothing
     * failed. Host and port are not secrets and may appear; keys never do. Thread-safe.
     */
    virtual std::string last_connect_error() const { return {}; }

    /**
     * Read-only: fetch the selected machine's Klipper objects (the Moonraker printer.objects.query "result" object,
     * {eventtime, status:{...}}) without blocking the caller. `done` is called exactly once, from ANY thread, with
     * ok=false and an empty object when there is no connected machine, the request fails or times out. Agents that have
     * no such notion keep the default (reports failure at once).
     */
    /**
     * Read-only: GET one allow-listed resource of the selected machine ("/machine/system_info", "/server/files/metadata?...",
     * "/server/files/thumbnails?...", "/server/files/gcodes/<relative path>") and hand back the raw body, at most `max_bytes`.
     * `done` is called exactly once, from ANY thread; ok=false (empty body) for a refused path, no machine, an HTTP error, a
     * timeout or an oversized body. Agents without the notion keep the default (fails at once).
     */
    virtual void read_machine_resource_async(const std::string& path_and_query, size_t max_bytes,
                                             std::function<void(bool ok, std::string body)> done)
    {
        (void) path_and_query; (void) max_bytes;
        if (done) done(false, std::string());
    }

    /**
     * Camera preview: ask the selected machine to start (`start`) or stop streaming its monitor snapshot, by the machine's own
     * Moonraker JSON-RPC methods camera.start_monitor / camera.stop_monitor (and nothing else; `params` must pass
     * camera_monitor_params_valid()). `done(ok, result)` gets the printer's "result" object, exactly once, from ANY thread;
     * ok=false (empty object) for refused params, no machine, an error reply or a timeout. Agents without the notion keep the
     * default (fails at once).
     */
    virtual void camera_monitor_async(bool start, const nlohmann::json& params,
                                      std::function<void(bool ok, nlohmann::json result)> done)
    {
        (void) start; (void) params;
        if (done) done(false, nlohmann::json::object());
    }

    /**
     * Timelapse listing (#90, read-only): Snapmaker's JSON-RPC method camera.get_timelapse_instance (and nothing else; `params` must pass
     * timelapse_list_params_valid()). `done(ok, result)` gets the printer's "result" object, exactly once, from ANY thread; ok=false
     * (empty object) for refused params, no machine, an error reply or a timeout. Agents without the notion keep the default (fails at once).
     */
    virtual void timelapse_list_async(const nlohmann::json& params, std::function<void(bool ok, nlohmann::json result)> done)
    {
        (void) params;
        if (done) done(false, nlohmann::json::object());
    }

    /**
     * Machine control (#75): call one of the forklifted Snapmaker control JSON-RPC methods (kControlRpcMethods; anything else is
     * refused). `params` must be an object. `done(ok, result)` exactly once, from ANY thread; ok=false for a refused method, no
     * machine, an error reply or a timeout. Agents without the notion keep the default (fails at once).
     */
    virtual void control_async(const std::string& method, const nlohmann::json& params,
                               std::function<void(bool ok, nlohmann::json result)> done)
    {
        (void) method; (void) params;
        if (done) done(false, nlohmann::json::object());
    }

    virtual void query_machine_objects_async(const std::vector<std::string>& object_names,
                                             std::function<void(bool ok, nlohmann::json result)> done)
    {
        (void) object_names;
        if (done) done(false, nlohmann::json::object());
    }

    /**
     * Refresh filament info from the printer synchronously.
     * Should only be called when get_filament_sync_mode() returns FilamentSyncMode::pull.
     * Populates the MachineObject's DevFilaSystem with fetched filament data.
     */
    virtual bool fetch_filament_info(std::string dev_id, FilamentSyncMode sync_mode = FilamentSyncMode::pull) { return false; }

    /**
     * Translate one filament id across the printer boundary.
     *
     * Orca content-addresses every system filament; a printer, its AMS and its vendor cloud
     * know only that vendor's own catalog ids. An agent whose printers already speak Orca's
     * ids leaves them alone, and so does an id with no mapping.
     */
    virtual std::string to_orca_filament_id(const std::string& printer_filament_id) const { return printer_filament_id; }
    virtual std::string from_orca_filament_id(const std::string& orca_filament_id) const { return orca_filament_id; }

    /**
     * Get the current camera stream URL for this agent's active machine.
     * Only meaningful when get_camera_stream_mode() returns an HTTP, HTTPS, or RTSP mode.
     */
    virtual std::string get_camera_url() const { return {}; }
};

// ---- Pure policy, kept free of wx so it can be unit-tested --------------------------------------------------------

// Bambu's LAN protocol requires an access code and the old UI filled in this placeholder when the user left it blank.
// It is only ever meaningful to Bambu-style agents.
inline constexpr const char* kBambuLanPlaceholderAccessCode = "88888888";

// A URL or address with any "user:password@" removed from its authority ("http://u:p@h:7125/x" -> "http://h:7125/x",
// "u:p@h" -> "h"). Used before an address is logged, shown to the page or put in an error message.
inline std::string strip_url_userinfo(const std::string& url)
{
    const std::size_t scheme_end = url.find("://");
    const std::size_t auth_begin = scheme_end == std::string::npos ? 0 : scheme_end + 3;
    std::size_t       auth_end   = url.find_first_of("/?#", auth_begin);
    if (auth_end == std::string::npos) auth_end = url.size();
    const std::size_t at = url.rfind('@', auth_end == 0 ? 0 : auth_end - 1);
    if (at == std::string::npos || at < auth_begin || at >= auth_end)
        return url;
    return url.substr(0, auth_begin) + url.substr(at + 1);
}

// Locale-independent character classes (std::isalnum depends on the C locale).
inline bool ascii_alnum(unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// The compile-time allowlist for control_async(): the printer JSON-RPC methods behind Snapmaker's sw_Control* / pause / resume / cancel /
// set-name / bed-mesh-abort handlers (Moonraker_Mqtt::async_control_* in Snapmaker/OrcaSlicer src/slic3r/Utils/MoonRaker.cpp).
inline constexpr const char* kControlRpcMethods[] = {
    "printer.control.led",          "printer.control.print_speed", "printer.control.purifier", "printer.control.main_fan",
    "printer.control.generic_fan",  "printer.control.bed_temp",    "printer.control.extruder_temp",
    "printer.print.pause",          "printer.print.resume",        "printer.print.cancel",
    "printer.bed_mesh.abort_probe_mesh", "machine.set_device_name",
    "printer.gcode.script"}; // printer.gcode.script only ever carries nozzle-temperature scripts (snapmaker_nozzle_script_*_valid, below)
// pause / resume / cancel: they get their own in-flight gate and a longer timeout than the other controls (#79)
inline bool control_method_is_print(const std::string& method)
{
    return method == "printer.print.pause" || method == "printer.print.resume" || method == "printer.print.cancel";
}
inline bool control_method_allowed(const std::string& method)
{
    for (const char* m : kControlRpcMethods)
        if (method == m) return true;
    return false;
}

// Nozzle temperature through the gcode route the Device page uses (#79): `SET_HEATER_TEMPERATURE HEATER=<h> TARGET=<n>` with h one of
// extruder, extruder1..extruder3 (the page names the heater "extruder" for index 0, else "extruder"+index) and n an integer 0..300.
// Nothing else is a valid script. Trailing whitespace/newlines are ignored; an embedded newline, extra token, other case or command is not.
inline bool snapmaker_nozzle_script_line_valid(const std::string& line)
{
    std::size_t end = line.size();
    while (end > 0 && (line[end - 1] == ' ' || line[end - 1] == '\t' || line[end - 1] == '\n' || line[end - 1] == '\r')) --end;
    const std::string s = line.substr(0, end);
    static const std::string prefix = "SET_HEATER_TEMPERATURE HEATER=";
    if (s.compare(0, prefix.size(), prefix) != 0) return false;
    const std::size_t sp = s.find(' ', prefix.size());
    if (sp == std::string::npos) return false;
    const std::string heater = s.substr(prefix.size(), sp - prefix.size());
    if (heater != "extruder" && heater != "extruder1" && heater != "extruder2" && heater != "extruder3") return false;
    static const std::string target = " TARGET=";
    if (s.compare(sp, target.size(), target) != 0) return false;
    const std::string n = s.substr(sp + target.size());
    if (n.empty() || n.size() > 3 || (n.size() > 1 && n[0] == '0')) return false;
    for (char c : n)
        if (c < '0' || c > '9') return false;
    return std::stoi(n) <= 300;
}
inline constexpr std::size_t kNozzleScriptMax = 4;
// The page's `script` param: a string (one script) or a non-empty array of at most 4 strings; every string is one valid script (an
// embedded newline fails the digit check, so two commands in one string are refused).
inline bool snapmaker_nozzle_script_valid(const nlohmann::json& script)
{
    if (script.is_string()) return snapmaker_nozzle_script_line_valid(script.get_ref<const std::string&>());
    if (!script.is_array() || script.empty() || script.size() > kNozzleScriptMax) return false;
    for (const auto& e : script)
        if (!e.is_string() || !snapmaker_nozzle_script_line_valid(e.get_ref<const std::string&>())) return false;
    return true;
}
// The agent's check of the `script` string it is about to send: the scripts the bridge joined with '\n', each valid, at most 4.
inline bool snapmaker_nozzle_script_joined_valid(const std::string& script)
{
    std::size_t begin = 0, count = 0;
    while (true) {
        const std::size_t nl = script.find('\n', begin);
        const std::string line = script.substr(begin, nl == std::string::npos ? std::string::npos : nl - begin);
        if (++count > kNozzleScriptMax || !snapmaker_nozzle_script_line_valid(line)) return false;
        if (nl == std::string::npos) return true;
        begin = nl + 1;
    }
}

// The whitelist for timelapse_list_async() and the page bridge (#90): an object with page_index (integer 0..10000), page_rows (integer
// 1..50) and optionally thumbnail_direct (only true: covers inline). Snapmaker passes the page's params through unchanged
// (Moonraker_Mqtt::async_get_timelapse_instance, MoonRaker.cpp); anything else here is refused.
inline bool timelapse_list_params_valid(const nlohmann::json& p)
{
    if (!p.is_object()) return false;
    bool has_index = false, has_rows = false;
    for (auto it = p.begin(); it != p.end(); ++it) {
        const std::string& k = it.key();
        const nlohmann::json& v = it.value();
        if (k == "page_index") {
            if (!v.is_number_integer() || v.get<long long>() < 0 || v.get<long long>() > 10000) return false;
            has_index = true;
        } else if (k == "page_rows") {
            if (!v.is_number_integer() || v.get<long long>() < 1 || v.get<long long>() > 50) return false;
            has_rows = true;
        } else if (k == "thumbnail_direct") {
            if (!v.is_boolean() || !v.get<bool>()) return false;
        } else
            return false;
    }
    return has_index && has_rows;
}

// The whitelist for camera_monitor_async() and the page bridge: an object with exactly the keys domain ("lan"), clientid
// (^[A-Za-z0-9_.-]{1,64}$) and optionally interval (integer 0..10) and expect_pw (bool). Anything else is refused.
inline bool camera_monitor_params_valid(const nlohmann::json& p)
{
    if (!p.is_object()) return false;
    bool has_domain = false, has_client = false;
    for (auto it = p.begin(); it != p.end(); ++it) {
        const std::string& k = it.key();
        const nlohmann::json& v = it.value();
        if (k == "domain") {
            if (!v.is_string() || v.get<std::string>() != "lan") return false;
            has_domain = true;
        } else if (k == "clientid") {
            if (!v.is_string()) return false;
            const std::string& id = v.get_ref<const std::string&>();
            if (id.empty() || id.size() > 64) return false;
            for (unsigned char c : id)
                if (!(ascii_alnum(c) || c == '_' || c == '.' || c == '-')) return false;
            has_client = true;
        } else if (k == "interval") {
            if (!v.is_number_integer() || v.get<long long>() < 0 || v.get<long long>() > 10) return false;
        } else if (k == "expect_pw") {
            if (!v.is_boolean()) return false;
        } else {
            return false;
        }
    }
    return has_domain && has_client;
}

// The sidebar's Connection (physical printer) button. BBL printers keep their own flow (hidden); with printer agents
// off it is the classic always-visible button; with agents on it is shown only for an agent that needs a typed-in host.
inline bool connection_button_visible(bool use_bbl_network, bool use_printer_agents, bool agent_needs_print_host)
{
    if (use_bbl_network)
        return false;
    return use_printer_agents ? agent_needs_print_host : true;
}

// The access code / API key to hand to the machine. A host-based agent gets exactly what the user entered (blank means no
// key); every other agent keeps the Bambu placeholder for a blank code.
inline std::string effective_access_code(const std::string& entered, bool agent_needs_print_host)
{
    if (!entered.empty() || agent_needs_print_host)
        return entered;
    return kBambuLanPlaceholderAccessCode;
}

} // namespace Slic3r

#endif // __I_PRINTER_AGENT_HPP__
