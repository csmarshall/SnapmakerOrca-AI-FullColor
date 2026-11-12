#ifndef __MOONRAKER_PRINTER_AGENT_HPP__
#define __MOONRAKER_PRINTER_AGENT_HPP__

#include "IPrinterAgent.hpp"
#include "bambu_networking.hpp"
#include "libslic3r/Spoolman/SpoolmanTransport.hpp"

#include <functional>
#include <chrono>
#include <cstdint>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>
#include <vector>

namespace Slic3r { class ICloudServiceAgent; }
#include "SpoolmanJoin.hpp"

namespace Slic3r {

class MoonrakerPrinterAgent : public IPrinterAgent
{
public:
    explicit MoonrakerPrinterAgent(std::string log_dir);
    ~MoonrakerPrinterAgent() override;

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    // Moonraker family (Qidi, Creality, Snapmaker inherit): the user types the host, the API key is optional.
    bool        needs_print_host() const override { return true; }
    std::string last_connect_error() const override;
    void        read_machine_resource_async(const std::string& path_and_query, size_t max_bytes,
                                            std::function<void(bool ok, std::string body)> done) override;
    // True for the read-only paths read_machine_resource_async() will fetch (exposed for tests).
    static bool is_readable_resource_path(const std::string& path_and_query);
    void        query_machine_objects_async(const std::vector<std::string>& object_names,
                                            std::function<void(bool ok, nlohmann::json result)> done) override;
    // Camera preview: a one-request websocket RPC (camera.start_monitor / camera.stop_monitor) on its own short-lived thread, ~5 s
    // in all. Plain ws:// only (a https:// machine fails). Params go through camera_monitor_params_valid() first.
    void        camera_monitor_async(bool start, const nlohmann::json& params,
                                     std::function<void(bool ok, nlohmann::json result)> done) override;
    // The JSON-RPC request text for the two camera methods and nothing else; "" if `params` fail the whitelist (exposed for tests).
    static std::string camera_monitor_request(bool start, const nlohmann::json& params, int id);
    // Classifies one websocket message against request `id`: Ok (result object with state "success" copied to `result`), Fail
    // (an error reply, or an unusable result), Ignore (a notification, another request's reply, not JSON).
    enum class CameraReply { Ignore, Ok, Fail };
    // At most one camera RPC per agent is in flight: a call made while one is pending is refused at once (done(false)), with no
    // thread and no socket. The accounting is this small gate so it can be tested without a network.
    class InFlightGate
    {
    public:
        bool try_acquire() { bool idle = false; return busy.compare_exchange_strong(idle, true); }
        void release() { busy.store(false); }
        bool busy_now() const { return busy.load(); }
    private:
        std::atomic<bool> busy{false};
    };
    // The whole RPC against `base_url` (what camera_monitor_async runs with the connected machine's url). `total` bounds
    // resolve + connect + handshake + reply. Exposed for the loopback tests.
    static void camera_monitor_run(const std::string& base_url, const std::string& api_key, bool start, const nlohmann::json& params,
                                   std::chrono::milliseconds total, std::shared_ptr<InFlightGate> gate,
                                   std::function<void(bool ok, nlohmann::json result)> done);
    static CameraReply parse_camera_monitor_reply(const std::string& message, int id, nlohmann::json& result);

    // Machine control (#75): one-request websocket RPC for a method on the kControlRpcMethods allowlist (IPrinterAgent.hpp); same
    // thread/timeout/gate model as camera_monitor_async, with its own gate so a camera stream never blocks the LED (print.* has a third gate, below).
    void        control_async(const std::string& method, const nlohmann::json& params,
                              std::function<void(bool ok, nlohmann::json result)> done) override;
    // pause / resume / cancel run on their own gate with kPrintControlTimeout (a cancel can outlast 5 s; the socket closing early does not
    // stop the cancel, so the bridge re-checks the print state before any retry); every other control keeps kControlTimeout and the shared gate.
    static constexpr std::chrono::milliseconds kControlTimeout{5000};
    static constexpr std::chrono::milliseconds kPrintControlTimeout{20000};
    static std::chrono::milliseconds control_timeout_for(const std::string& method)
    {
        return control_method_is_print(method) ? kPrintControlTimeout : kControlTimeout;
    }
    std::shared_ptr<InFlightGate> control_gate_for(const std::string& method) const { return control_method_is_print(method) ? m_print_gate : m_control_gate; }
    std::shared_ptr<InFlightGate> camera_gate() const { return m_camera_gate; }
    // The JSON-RPC request text; "" if `method` is off the allowlist or `params` is not an object (exposed for tests).
    static std::string control_request(const std::string& method, const nlohmann::json& params, int id);
    // Ok = a non-error result for `id`, Fail = an error reply or no result, Ignore = notification / another id / not JSON.
    static CameraReply parse_control_reply(const std::string& message, int id, nlohmann::json& result);
    static void control_run(const std::string& base_url, const std::string& api_key, const std::string& method, const nlohmann::json& params,
                            std::chrono::milliseconds total, std::shared_ptr<InFlightGate> gate,
                            std::function<void(bool ok, nlohmann::json result)> done);

    // Cloud Agent Dependency
    void set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud) override;

    // Communication
    int send_message(std::string dev_id, std::string json_str, int qos, int flag) override;
    int connect_printer(const PrinterConnectionParams& params) override;
    int disconnect_printer() override;
    int send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) override;

    // Discovery
    bool start_discovery(bool start, bool sending) override;

    // Binding
    int bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect) override;
    int set_server_callback(OnServerErrFn fn) override;

    // Machine Selection
    std::string get_user_selected_machine() override;
    int set_user_selected_machine(std::string dev_id) override;

    // Print Job Operations
    // Whether print_uploaded_file first sends the per-print preferences (flow calibration, time-lapse, bed leveling) as
    // SET_PRINT_PREFERENCE lines, as Snapmaker Orca's send page does. Only the Snapmaker agent does.
    virtual bool supports_print_preferences() const { return false; }

    int start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override;
    int start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override;

    // Callbacks
    int set_on_ssdp_msg_fn(OnMsgArrivedFn fn) override;
    int set_on_printer_connected_fn(OnPrinterConnectedFn fn) override;
    int set_on_subscribe_failure_fn(GetSubscribeFailureFn fn) override;
    int set_on_message_fn(OnMessageFn fn) override;
    int set_on_user_message_fn(OnMessageFn fn) override;
    int set_on_local_connect_fn(OnLocalConnectedFn fn) override;
    int set_on_local_message_fn(OnMessageFn fn) override;
    int set_queue_on_main_fn(QueueOnMainFn fn) override;

    // Pull-mode agent (on-demand filament sync)
    FilamentSyncMode get_filament_sync_mode() const override { return FilamentSyncMode::pull; }
    bool fetch_filament_info(std::string dev_id, FilamentSyncMode sync_mode = FilamentSyncMode::pull) override;

    // Read-only Spoolman catalog through Moonraker's /server/spoolman/proxy (#95 S7). Spoolman timeouts: 3 s connect, 6 s total.
    // The proxy is a POST to Moonraker whose payload says request_method "GET": Spoolman is only read and Moonraker's state is unchanged.
    static constexpr int    kSpoolmanConnectTimeoutSeconds = 3;
    static constexpr int    kSpoolmanMaxTimeoutSeconds     = 6;
    // Http's default response size limit is 1 GiB; the Spoolman requests set this instead so curl aborts an oversized transfer
    // (reported as "response too large"). check_body_limits() is the second layer.
    static constexpr size_t kSpoolmanMaxResponseBytes      = Spoolman::kMaxBodyBytes + 64 * 1024;
    // Whether Moonraker is connected to Spoolman; nullopt when it cannot be told (treat as not connected).
    std::optional<bool>    spoolman_status() const;
    // Catalog fetch, cached for the session per printer. With `preference_on` false (the app preference spoolman_preset_sync) no request
    // of any kind is made. The caller reads the preference. It is synchronous and blocks for up to ~4 requests x 6 s, so call it off the
    // GUI thread, and do not destroy the agent while it runs. The printer address and key are read ONCE under connect_mutex and used for
    // the cache key and every request, so a connect/disconnect during the fetch cannot mix two printers.
    Spoolman::CatalogFetch fetch_spoolman_catalog(bool preference_on);
    // The last good catalog of the current printer, even after invalidate or a failed refresh (show it as stale); nullopt if none.
    std::optional<Spoolman::CatalogFetch> last_good_spoolman_catalog() const;
    void                   invalidate_spoolman_cache();

    // #105: whether this agent may ask the printer to re-read its spool RFID tags (the Spoolman button only). Off by default; the
    // Snapmaker agent turns it on.
    virtual bool supports_rfid_reread() const { return false; }
    enum class RfidReread { unsupported, busy, state_unknown, send_failed, done, unconfirmed };
    // Sends the approved re-read script (Spoolman::rfid_reread_script) through Spoolman::send_rfid_reread, which re-checks
    // print_stats.state immediately before sending, then polls filament_detect (read-only) until a slot UID changed or `timeout` passed.
    // `unconfirmed` = sent but no change was seen in time. Blocking: call it off the GUI thread.
    RfidReread reread_spool_tags(std::chrono::milliseconds timeout = std::chrono::seconds(10),
                                 std::chrono::milliseconds interval = std::chrono::seconds(1));

    // JSON helper methods
    static std::string safe_json_string(const nlohmann::json& obj, const char* key);
    static int safe_json_int(const nlohmann::json& obj, const char* key);
    static std::string safe_array_string(const nlohmann::json& arr, int idx);
    static int safe_array_int(const nlohmann::json& arr, int idx);

protected:
    struct MoonrakerDeviceInfo
    {
        std::string dev_id;
        std::string dev_ip;
        std::string api_key;
        std::string base_url;
        std::string model_id;
        std::string model_name;
        std::string dev_name;
        std::string version;
        std::string klippy_state;
        bool        use_ssl = false;
    } device_info;

    // Copied when the stream starts, so changes apply to the next start_status_stream(). A connection's retries read
    // reconnect_delay directly.
    struct StatusStreamTimings
    {
        std::chrono::milliseconds handshake{std::chrono::seconds(10)};      // Opening and closing handshakes
        std::chrono::milliseconds close{std::chrono::seconds(1)};           // Wait for the close frame's answer before dropping the socket
        std::chrono::milliseconds idle{std::chrono::seconds(30)};           // Silence before giving up, with a ping every half of it
        std::chrono::milliseconds heartbeat{std::chrono::seconds(10)};      // Silence before the cached status is sent again
        std::chrono::milliseconds reconnect_delay{std::chrono::seconds(1)}; // First wait, doubling per consecutive failure up to 32x
        std::chrono::milliseconds storage_check{std::chrono::seconds(30)};  // How often the free space of the gcodes root is re-read
    } ws_timings;

    // Tray data for AMS payload building
    struct AmsTrayData {
        int         slot_index = 0;      // 0-based slot index
        bool        has_filament = false;
        std::string tray_type;           // Material type (e.g., "PLA", "ASA")
        std::string tray_color;          // Raw color (#RRGGBB, 0xRRGGBB, or RRGGBBAA)
        std::vector<std::string> tray_colors; // Optional: every color of a multi-color spool, in order (same formats).
                                              // Used when it holds two or more; otherwise tray_color is the only one.
        bool        colors_gradient = false;  // With tray_colors: blend them as a gradient instead of drawing segments
        std::string tray_info_idx;       // Filament ID (optional)
        int         bed_temp = 0;        // Optional
        int         nozzle_temp = 0;     // Optional
        SpoolmanSpoolInfo spoolman;      // Optional, read-only Spoolman spool joined by RFID UID (valid() when matched)
    };

    // Build ams JSON and call parser
    void build_ams_payload(int ams_count, int max_lane_index, const std::vector<AmsTrayData>& trays);
    // Name shown for a filament unit, or empty for the AMS Lite name
    virtual std::string ams_unit_name(int /*ams_id*/) const { return {}; }

    // Methods that derived classes may need to override or access
    virtual bool init_device_info(const std::string& dev_id, const std::string& dev_ip, const std::string& username, const std::string& password, bool use_ssl, const std::string& port);
    // An override must give up its request once `cancelled` returns true, or the destructor waits out its timeout
    virtual bool fetch_device_info(const std::string&           base_url,
                                   const std::string&           api_key,
                                   MoonrakerDeviceInfo&         info,
                                   std::string&                 error,
                                   const std::function<bool()>& cancelled) const;
    // Starts the print of a file already uploaded to the gcodes root
    virtual bool print_uploaded_file(const PrintParams& params, const std::string& filename);
    // Whether the printer prints a .gcode.3mf bundle as is (its use_3mf option)
    virtual bool accepts_3mf() const;
    void start_status_stream(const std::string& dev_id, const std::string& base_url, const std::string& api_key);
    void stop_status_stream();

    // State access for derived classes
    mutable std::recursive_mutex       state_mutex;

    // Helpers
    bool        is_numeric(const std::string& value);
    std::string normalize_base_url(bool use_ssl, const std::string& host, const std::string& port);
    std::string sanitize_filename(const std::string& filename);
    std::string join_url(const std::string& base_url, const std::string& path) const;
    bool send_gcode_sync(const std::string& dev_id, const std::string& gcode) const;
    // One Moonraker HTTP request for the Spoolman code. `post_body` empty means GET. Returns the HTTP status, 0 on connection failure or
    // timeout. Virtual so tests can record requests without a socket.
    // The printer address and key are passed in (a snapshot), never read from device_info here. Returns Spoolman::kStatusTooLarge when
    // the size limit aborted the transfer.
    virtual int spoolman_moonraker_request(const std::string& base_url, const std::string& api_key, const std::string& path,
                                           const std::string& post_body, std::string& response) const;
    // Posts the request to /printer/print/start
    bool send_jsonrpc_command(const std::string& base_url, const std::string& api_key,
                              const nlohmann::json& request, std::string& response) const;

    // Trim whitespace and convert to uppercase
    static std::string trim_and_upper(const std::string& input);

    // Map filament type to OrcaFilamentLibrary preset ID for AMS sync compatibility
    static std::string map_filament_type_to_generic_id(const std::string& filament_type);

private:
    struct SpoolmanEndpoint { std::string base_url, api_key, key; };
    SpoolmanEndpoint spoolman_endpoint() const; // snapshot under connect_mutex
    std::optional<bool> spoolman_status(const SpoolmanEndpoint& ep) const;
    int handle_request(const std::string& dev_id, const std::string& json_str);
    int send_version_info(const std::string& dev_id);
    int send_access_code(const std::string& dev_id);

    // GETs a Moonraker endpoint and parses its JSON answer, giving up once `cancelled` returns true
    bool get_json(const std::string&           url,
                  const std::string&           api_key,
                  const std::function<bool()>& cancelled,
                  nlohmann::json&              json,
                  std::string&                 error) const;
    bool fetch_object_list(const std::string& base_url, const std::string& api_key, std::set<std::string>& objects, std::string& error) const;
    bool fetch_has_free_storage(const std::string& base_url, const std::string& api_key, bool& has_free, std::string& error) const;
    bool query_printer_status(const std::string&           base_url,
                              const std::string&           api_key,
                              nlohmann::json&              status,
                              std::string&                 error,
                              const std::function<bool()>& cancelled) const;
    bool gcode_command_known(const std::string& base_url, const std::string& api_key, const std::string& command, bool& missing) const;
    void send_gcode_async(const std::string& dev_id, const std::string& gcode,
                          std::function<void(bool)> on_result = {}) const;

    void announce_printhost_device();
    void dispatch_local_connect(int state, const std::string& dev_id, const std::string& msg);
    void dispatch_printer_connected(const std::string& dev_id);
    void dispatch_message(const std::string& dev_id, const std::string& payload);
    // Joins a stream stopped from its own callback, so a new stream can start without waiting on it under connect_mutex
    void join_stopped_status_stream();
    void run_status_stream(std::string dev_id, std::string base_url, std::string api_key, StatusStreamTimings timings);
    void handle_ws_message(const std::string& dev_id, const std::string& payload);
    void send_heartbeat_if_due(const std::string& dev_id, std::chrono::milliseconds interval);
    void dispatch_status(const std::string& dev_id);
    // Re-reads whether the gcodes root has free space, and returns whether the answer changed
    bool refresh_storage(const std::string& base_url, const std::string& api_key);
    void update_status_cache(const nlohmann::json& updates);
    nlohmann::json build_print_payload_locked() const;

    // Print control helpers
    int pause_print(const std::string& dev_id);
    int resume_print(const std::string& dev_id);
    int cancel_print(const std::string& dev_id);

    // File upload
    bool upload_gcode(const std::string& local_path, const std::string& filename,
                      const std::string& base_url, const std::string& api_key,
                      OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn);

    // Connection thread management
    void perform_connection_async(const std::string& dev_id,
                                   const std::string& base_url,
                                   const std::string& api_key,
                                   uint64_t generation);

    // System-specific filament fetch methods
    bool fetch_hh_filament_info(std::vector<AmsTrayData>& trays, int& max_lane_index);
    bool fetch_moonraker_filament_data(std::vector<AmsTrayData>& trays, int& max_lane_index);
    bool fetch_creality_cfs_data(std::vector<AmsTrayData>& trays, int& max_lane_index);

    static std::string normalize_color_value(const std::string& color);

    std::string                        ssdp_announced_host;
    std::string                        ssdp_announced_id;
    std::shared_ptr<ICloudServiceAgent> m_cloud_agent;
    std::string                        selected_machine;

    OnMsgArrivedFn       on_ssdp_msg_fn;
    OnPrinterConnectedFn on_printer_connected_fn;
    GetSubscribeFailureFn on_subscribe_failure_fn;
    OnMessageFn          on_message_fn;
    OnMessageFn          on_user_message_fn;
    OnLocalConnectedFn   on_local_connect_fn;
    OnMessageFn          on_local_message_fn;
    QueueOnMainFn        queue_on_main_fn;
    OnServerErrFn        on_server_err_fn;

    mutable std::recursive_mutex payload_mutex;
    nlohmann::json     status_cache;

    Spoolman::CatalogCache spoolman_cache;
    std::atomic<int>       next_jsonrpc_id{1};
    std::set<std::string>  available_objects;  // Track for feature detection
    std::atomic<bool>      has_storage{false}; // The gcodes root has free space

    std::atomic<bool>   ws_stop{false};
    std::atomic<bool>   ws_reconnect_requested{false};  // Flag to trigger reconnection
    std::atomic<uint64_t> ws_last_emit_ms{0};
    std::thread         ws_thread;
    std::mutex          ws_abort_mutex; // Guards ws_thread and ws_abort
    std::function<void()> ws_abort; // Interrupts whatever the stream thread is waiting on, from any thread

    // Throttling configuration for WebSocket updates
    // Critical changes (state transitions) dispatch immediately; telemetry is throttled
    static constexpr uint64_t STATUS_UPDATE_INTERVAL_MS = 1000;  // 1 update/sec for telemetry
    std::atomic<uint64_t> ws_last_dispatch_ms{0};
    std::string last_print_state;  // Track state for immediate dispatch on change

    // Connection thread management
    std::atomic<uint64_t>  connect_generation{0};
    std::thread            connect_thread;
    std::shared_ptr<InFlightGate> m_camera_gate = std::make_shared<InFlightGate>();
    std::shared_ptr<InFlightGate> m_control_gate = std::make_shared<InFlightGate>();
    std::shared_ptr<InFlightGate> m_print_gate = std::make_shared<InFlightGate>();
    mutable std::recursive_mutex connect_mutex; // mutable: spoolman_endpoint() snapshots under it
    mutable std::mutex     connect_error_mutex;
    std::string            connect_error; // see last_connect_error()
    void                   set_connect_error(std::string error);
};

} // namespace Slic3r

#endif
