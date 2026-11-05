// SSWCP: the message bridge between Snapmaker's Flutter Home/Device page and the slicer.
//
// Derived from Snapmaker/OrcaSlicer src/slic3r/GUI/SSWCP.{hpp,cpp} (AGPL-3.0, same licence as this
// project); only the dispatcher core is ported, restructured so it has no wxWebView and no wxGetApp():
//
//  * replies leave through an injected sink (Services::sink) that the webview adapter (ADR 0005 step 1d)
//    binds to RunScript, so the whole dispatcher is unit-testable;
//  * the command registry is an explicit allow-list: a command that is not registered never runs and
//    gets a structured 501 reply;
//  * there is no worker thread. Instance timeouts are swept by expire_due(), which the adapter calls
//    from the UI thread (a wxTimer); nothing here spawns threads or touches the network.
//
// Wire protocol (verified against the vendored bundle and Snapmaker's SSWCP.cpp):
//   page -> C++ : JSON string {header, payload:{cmd, params, event_id}}
//   C++ -> page : window.postMessage(JSON.stringify({header, payload:{code, message, data}}), '*');
//   code 200 = success. A request with an event_id is a long-lived subscription: the first reply
//   echoes the request header, later pushes carry header {event_id}.
#ifndef slic3r_GUI_SSWCP_hpp_
#define slic3r_GUI_SSWCP_hpp_

#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "nlohmann/json.hpp"

namespace Slic3r { namespace GUI {

class SSWCP_Instance;
struct SSWCPCore;

// Reply codes. 200 is success; -1 and -2 are the values Snapmaker's handlers use for generic failure and
// timeout (kept so the page's error handling is unchanged); the rest are ours.
namespace SSWCPCode {
constexpr int Ok            = 200;
constexpr int BadRequest    = 400;
constexpr int TooMany       = 429;
constexpr int Unsupported   = 501;
constexpr int Failure       = -1;
constexpr int Timeout       = -2;
} // namespace SSWCPCode

// Hard limits on what the page may send. The page is a bundle we do not control and a webview can be
// navigated, so everything it sends is treated as hostile input.
constexpr std::size_t SSWCP_MAX_MESSAGE_BYTES = 1024 * 1024; // 1 MiB
constexpr int         SSWCP_MAX_JSON_DEPTH    = 32;
constexpr std::size_t SSWCP_MAX_EVENT_ID_LEN  = 256;
constexpr std::size_t SSWCP_MAX_SUBSCRIPTIONS = 256;
constexpr std::size_t SSWCP_MAX_LOG_CHARS     = 4096;

// What the dispatcher did with one incoming message (the page never sees this; tests and logs do).
enum class SSWCPOutcome {
    Dispatched,    // a registered command ran
    Unsupported,   // well-formed but the cmd is not on the allow-list: 501 sent
    BadRequest,    // header present but the payload/cmd/event_id/params were invalid: 400 sent
    Dropped,       // no usable header to echo (not JSON, no header, no payload...): nothing sent
    TooLarge,      // over SSWCP_MAX_MESSAGE_BYTES: nothing sent
    TooDeep,       // JSON nested deeper than SSWCP_MAX_JSON_DEPTH: nothing sent
};

// Everything the core needs from the outside world.
struct SSWCPServices
{
    // Receives a complete JavaScript statement ("window.postMessage(...);"). Called on the thread that
    // called handle_web_message / push / expire_due; never with an internal lock held.
    std::function<void(const std::string& js)> sink;
    // Returns the JSON array for sw_GetLocalDevices and for pushes to sw_SubscribeLocalDevices. Step 1d
    // binds the DeviceManager here. Default (null): empty array.
    std::function<nlohmann::json()> local_devices;
    // Tripwire for "no outbound traffic": a handler that would ever contact the outside world must call
    // it first. No handler in this core does (sw_UploadEvent is accepted and dropped), so tests assert
    // it stays at zero.
    std::function<void(const std::string& what)> outbound_observer;
    // Optional extras for sw_GetSoftwareInfo. Empty name/version fall back to the build's values.
    std::string software_name;
    std::string software_version;
    std::string http_host;  // Snapmaker returns the page server's URL here; omitted when empty
    // Injectable clock for timeout tests.
    std::function<std::chrono::steady_clock::time_point()> now;
};

using SSWCPHandler = std::function<void(SSWCP_Instance&)>;

// One in-flight request. Replies are built from res_data/status/msg and leave through the sink.
class SSWCP_Instance : public std::enable_shared_from_this<SSWCP_Instance>
{
public:
    SSWCP_Instance(std::weak_ptr<SSWCPCore> core, std::string cmd, nlohmann::json header, nlohmann::json params,
                   std::string event_id, SSWCPHandler handler, bool subscription);

    // Runs the handler. For a request with an event_id, first sends the acknowledgement (request header,
    // 200) and switches the header to {event_id}, as Snapmaker's process() does.
    void process();
    // Sends the current header/status/msg/data to the page.
    void send_to_js();
    // Removes the instance from the list (unless it is a live subscription, which stays until dropped).
    void finish_job();
    void handle_general_fail(int code = SSWCPCode::Failure, const std::string& msg = "failure");
    // Called by the dispatcher when the default timeout passes without finish_job().
    void on_timeout();

    // Convenience for handlers: reply code 200 with this data and finish (or, for a subscription, stay).
    void reply_ok(nlohmann::json data = nlohmann::json::object());
    // Registers this instance as a subscription for its event_id (replaces an earlier one with the same id).
    // Returns false and replies 429 when the subscription cap is reached.
    bool subscribe();

    const std::string&    cmd() const { return m_cmd; }
    const std::string&    event_id() const { return m_event_id; }
    const nlohmann::json& params() const { return m_params; }
    bool                  is_subscription_command() const { return m_subscription; }
    const SSWCPServices&  services() const;

    nlohmann::json m_header;
    nlohmann::json m_res_data = nlohmann::json::object();
    int            m_status   = SSWCPCode::Ok;
    std::string    m_msg      = "OK";

private:
    std::weak_ptr<SSWCPCore> m_core;
    std::string              m_cmd;
    nlohmann::json           m_params;
    std::string              m_event_id;
    SSWCPHandler             m_handler;
    bool                     m_subscription;
    bool                     m_subscribed = false;
    bool                     m_done       = false;
};

// The dispatcher: one per webview. Not tied to wx.
class SSWCP
{
public:
    // Default lifetime of a one-shot instance that never finishes. (Snapmaker's constant is 80 s although
    // its comment says 8 s; the value is kept.)
    static constexpr std::chrono::milliseconds DEFAULT_INSTANCE_TIMEOUT{80000};

    explicit SSWCP(SSWCPServices services);
    ~SSWCP();
    SSWCP(const SSWCP&)            = delete;
    SSWCP& operator=(const SSWCP&) = delete;

    // Entry point for one raw string from the page. Never throws; replies (if any) go to the sink.
    SSWCPOutcome handle_web_message(const std::string& message);

    // Adds a command to the allow-list (later port steps register theirs here; the startup set is built in).
    // Not reachable from the page. Replaces an existing registration of the same name.
    void register_command(const std::string& cmd, SSWCPHandler handler, bool subscription);
    // True if `cmd` is on the allow-list.
    bool is_registered(const std::string& cmd) const;

    // Pushes {code, message, data} to every live subscription of `cmd` using that subscription's event_id.
    // Returns the number of subscribers reached. This is how step 1d forwards device-list changes etc.
    std::size_t push(const std::string& cmd, const nlohmann::json& data, int code = SSWCPCode::Ok,
                     const std::string& message = "OK");

    // Drops every subscription (the adapter calls this when the webview goes away).
    void drop_all_subscriptions();

    // Times out one-shot instances older than DEFAULT_INSTANCE_TIMEOUT; infinite entries never expire.
    // Returns the number expired. Call from the UI thread periodically.
    std::size_t expire_due();

    // Test/inspection helpers.
    std::size_t instance_count() const;
    std::size_t subscription_count() const;

private:
    std::shared_ptr<SSWCPCore> m_core;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_SSWCP_hpp_
