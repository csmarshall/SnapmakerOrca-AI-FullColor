#pragma once

// Send-flow slice S5 (docs/records/snapmaker-u1-send-flow-design.md section 4): the lifecycle of the non-modal pre-print dialog, wx-free.
//
// Everything with a window, a timer or a thread is injected (PreprintView, PreprintScheduler, the Deps callbacks), so the lifecycle is
// unit-tested with fakes. What the fakes cannot show: that a real WKWebView installs its "wx" script handler after about:blank, that the
// page then loads and calls the bridge, and that nothing hangs (#84). That needs a live GUI run (S0) and a live send (S7).
//
// Lifecycle of open():
//   1. re-entry guard: a live dialog is only raised;
//   2. the file name is sanitised (snapmaker_preprint_sanitise_filename), the plate G-code is SNAPSHOTTED into the capability store (S3);
//   3. the view is created on about:blank and Show()n (never modal) and a poll waits, through scheduler callbacks and no nested event loop,
//      for the script handler (view->script_handler_added()). kPreprintHandlerWatchdog bounds that wait: log + error + close;
//   4. then the printer is bound (Deps::bind_printer, BLOCKING HTTP, run in the background), the mapping context is filled from the plate,
//      the session is opened with the pinned identity and the snapshot, the bridge is attached to the view and only then the page is loaded;
//   5. completion (page done, window closed by the user, plate changed) closes everything: capability revoked, session finished, the view
//      DESTROYED, and only after that, in a later scheduler turn, the Device-tab switch.
// All members run on the UI thread.

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>

#include "PreprintCapabilityStore.hpp"
#include "SnapmakerDeviceBridge.hpp"
#include "SnapmakerPreprintSession.hpp"
#include "slic3r/Utils/SnapmakerPreprintFilename.hpp"

namespace Slic3r { namespace GUI {

constexpr std::chrono::milliseconds kPreprintHandlerWatchdog{5000}; // handler not installed this long after Show(): give up (#84 guard)
constexpr std::chrono::milliseconds kPreprintPollInterval{50};

// The dialog plus its webview. In the app: PreprintDialog (a non-modal wxDialog). In tests: a fake that records calls.
class PreprintView
{
public:
    virtual ~PreprintView() = default;
    virtual bool script_handler_added() = 0;            // the webview's "wx" script message handler is installed
    virtual void show() = 0;                            // non-modal Show()
    virtual void raise() = 0;
    virtual void set_title(const std::string& title) = 0;
    virtual void attach_bridge(std::uint64_t epoch) = 0; // builds this dialog's SnapmakerDeviceBridge (pre-print profile) for the epoch
    virtual void load_page(bool send_only) = 0;          // the pre-print page (send_only: the upload-only variant)
    virtual void destroy() = 0;                          // destroys the window; the view never calls the controller from here
};

class PreprintScheduler
{
public:
    virtual ~PreprintScheduler() = default;
    virtual std::chrono::steady_clock::time_point now() = 0;
    virtual void call_after(std::function<void()> fn) = 0;                                  // wx CallAfter: a later event-loop turn
    virtual void call_later(std::chrono::milliseconds delay, std::function<void()> fn) = 0; // a one-shot timer
    // `work` off the UI thread, then `on_ui` exactly once on the UI thread.
    virtual void run_background(std::function<void()> work, std::function<void()> on_ui) = 0;
};

struct PreprintRequest
{
    boost::filesystem::path gcode_path;           // the plate's sliced G-code (plate->get_tmp_gcode_path())
    std::string             requested_name;       // from PrintHostSendDialog
    bool                    start_requested = true; // PrintHostSendDialog post action == StartPrint; false = upload only
    bool                    switch_to_device_tab = false;
    SnapmakerPreprintPlate  plate;                // display_name is replaced by the sanitised remote name
    std::vector<int>        plate_filaments;      // 0-based filament indices the plate uses
    int                     extruder_count = 0;
    std::map<std::string, std::string> metadata;
    bool                    configure_required = true; // a validated map/preference script must precede the start
};

class PreprintDialogController : public std::enable_shared_from_this<PreprintDialogController>
{
public:
    struct Deps
    {
        std::shared_ptr<PreprintScheduler>        scheduler;
        std::shared_ptr<PreprintCapabilityStore>  store;
        std::shared_ptr<SnapmakerPreprintContext> context;
        std::shared_ptr<SnapmakerPreprintSession> session;
        std::function<SnapmakerPreprintPin()>     bind_printer; // MoonrakerPreprintPrinter::bind_current(); BLOCKING, run in the background
        std::function<std::unique_ptr<PreprintView>()> make_view;
        std::function<void(const std::string&)>  report_error; // a message for the user (UI thread)
        std::function<void()>                     switch_to_device_tab; // called after the view was destroyed, in a later scheduler turn
    };
    enum class Result { Opened, AlreadyOpen, Refused };
    enum class State { Idle, WaitingForHandler, Binding, Live, Closed };

    static std::shared_ptr<PreprintDialogController> create(Deps deps) { return std::shared_ptr<PreprintDialogController>(new PreprintDialogController(std::move(deps))); }

    Result open(PreprintRequest req)
    {
        if (is_live()) {
            if (m_view) m_view->raise();
            return Result::AlreadyOpen;
        }
        reset_for_open();
        const SnapmakerPreprintName name = snapmaker_preprint_sanitise_filename(req.requested_name);
        m_remote_filename = name.name;
        m_name_changed    = name.changed;
        if (name.changed)
            BOOST_LOG_TRIVIAL(info) << "PreprintDialogController: file name \"" << req.requested_name << "\" sent as \"" << name.name << "\"";
        // Snapshot FIRST: what the user confirmed in the send dialog is what is served, whatever a re-slice does while the page is open.
        const auto reg = m_d.store->register_file(req.gcode_path, snapmaker_preprint_capability_display_name(name.name));
        if (!reg.ok) {
            fail("The plate G-code could not be prepared: " + reg.error);
            return Result::Refused;
        }
        m_cap_path = reg.url_path;
        m_size = reg.size;
        m_sha  = reg.sha256_hex;
        m_req  = std::move(req);
        m_req.plate.display_name = m_remote_filename;
        m_view = m_d.make_view();
        if (!m_view) {
            fail("The pre-print window could not be created.");
            return Result::Refused;
        }
        m_state = State::WaitingForHandler;
        m_view->set_title(m_remote_filename);
        m_view->show(); // non-modal: nothing here runs a nested event loop
        m_wait_started = m_d.scheduler->now();
        schedule_poll();
        return Result::Opened;
    }

    // The plate was re-sliced or changed: what the page holds is stale. "plate changed, reopen".
    void invalidate(const std::string& why = "The plate changed while the print window was open. Reopen Print plate.")
    {
        if (!is_live()) return;
        BOOST_LOG_TRIVIAL(info) << "PreprintDialogController: invalidated: " << why;
        if (m_d.report_error) m_d.report_error(why);
        close(/*switch_tab=*/false);
    }
    // The user closed the window.
    void on_user_closed() { if (is_live()) close(false); }
    // The bridge's preprint_done (already deferred by the bridge to a later turn). `started` = the print was started on the printer.
    void on_page_done(bool started)
    {
        if (!is_live()) return;
        close(m_req.switch_to_device_tab && (started || !m_req.start_requested));
    }

    bool        is_live() const { return m_state == State::WaitingForHandler || m_state == State::Binding || m_state == State::Live; }
    State       state() const { return m_state; }
    // The sanitised name that is pinned, shown in the window title and sent; name_changed() = it differs from what the user typed.
    const std::string& remote_filename() const { return m_remote_filename; }
    bool        name_changed() const { return m_name_changed; }
    const std::string& capability_path() const { return m_cap_path; }

private:
    explicit PreprintDialogController(Deps d) : m_d(std::move(d)) {}

    void reset_for_open()
    {
        m_state = State::Idle;
        m_epoch = 0;
        m_cap_path.clear();
        m_sha.clear();
        m_size = 0;
        m_view.reset();
    }
    void schedule_poll()
    {
        std::weak_ptr<PreprintDialogController> weak = shared_from_this();
        const auto generation = m_generation;
        m_d.scheduler->call_later(kPreprintPollInterval, [weak, generation] {
            if (auto self = weak.lock()) self->poll(generation);
        });
    }
    void poll(std::uint64_t generation)
    {
        if (generation != m_generation || m_state != State::WaitingForHandler || !m_view) return;
        if (m_view->script_handler_added()) {
            begin_open();
            return;
        }
        if (m_d.scheduler->now() - m_wait_started >= kPreprintHandlerWatchdog) {
            BOOST_LOG_TRIVIAL(error) << "PreprintDialogController: the webview's script handler was not installed within "
                                     << kPreprintHandlerWatchdog.count() << " ms; closing the pre-print window";
            fail("The print window did not start (its web view did not become ready). Please try again.");
            return;
        }
        schedule_poll();
    }
    // The handler is installed: bind the printer (blocking HTTP, so off the UI thread), then open the session on the UI thread.
    void begin_open()
    {
        m_state = State::Binding;
        auto pin = std::make_shared<SnapmakerPreprintPin>();
        std::weak_ptr<PreprintDialogController> weak = shared_from_this();
        const auto generation = m_generation;
        auto       bind = m_d.bind_printer;
        m_d.scheduler->run_background(
            [pin, bind] {
                try {
                    if (bind) *pin = bind();
                } catch (...) {
                    *pin = SnapmakerPreprintPin{};
                }
            },
            [weak, generation, pin] {
                if (auto self = weak.lock()) self->finish_open(generation, *pin);
            });
    }
    void finish_open(std::uint64_t generation, const SnapmakerPreprintPin& pin)
    {
        if (generation != m_generation || m_state != State::Binding || !m_view) return; // closed meanwhile
        std::string err;
        if (!m_d.context->set(m_req.plate, m_req.metadata, err)) {
            fail("The plate data could not be prepared: " + err);
            return;
        }
        SnapmakerPreprintSession::Open o;
        o.gate.printer            = pin;
        o.gate.remote_filename    = m_remote_filename;
        o.gate.plate_filaments    = m_req.plate_filaments;
        o.gate.extruder_count     = m_req.extruder_count;
        o.gate.snapshot_size      = m_size;
        o.gate.configure_required = m_req.configure_required;
        o.gate.allow_start        = m_req.start_requested; // upload-only: no start may ever be allowed
        o.capability_path         = m_cap_path;
        o.sha256_hex              = m_sha;
        m_epoch = m_d.session->open(std::move(o), err);
        if (m_epoch == 0) {
            fail("The printer could not be prepared for this print: " + err);
            return;
        }
        m_state = State::Live;
        m_view->attach_bridge(m_epoch);
        m_view->load_page(!m_req.start_requested);
    }

    void fail(const std::string& message)
    {
        BOOST_LOG_TRIVIAL(error) << "PreprintDialogController: " << message;
        if (m_d.report_error) m_d.report_error(message);
        if (is_live()) close(false);
        else {
            teardown_resources();
            m_state = State::Closed;
        }
    }
    void teardown_resources()
    {
        ++m_generation; // late poll / bind callbacks of this open are ignored
        if (m_d.store && !m_cap_path.empty()) m_d.store->revoke(m_cap_path);
        if (m_d.session && m_epoch) m_d.session->finish(m_epoch);
        if (m_d.context) m_d.context->clear();
    }
    void close(bool switch_tab)
    {
        if (!is_live()) return;
        m_state = State::Closed;
        teardown_resources();
        if (m_view) {
            m_view->destroy(); // the window is gone before anything else is switched to
            m_view.reset();
        }
        if (switch_tab && m_d.switch_to_device_tab) {
            // A later turn, never from inside this call: the Device tab builds a webview, which must not happen under the dialog (#84).
            m_d.scheduler->call_after([fn = m_d.switch_to_device_tab] { fn(); });
        }
    }

    Deps                          m_d;
    State                         m_state = State::Idle;
    std::uint64_t                 m_generation = 1;
    std::uint64_t                 m_epoch = 0;
    PreprintRequest               m_req;
    std::unique_ptr<PreprintView> m_view;
    std::string                   m_remote_filename, m_cap_path, m_sha;
    std::uint64_t                 m_size = 0;
    bool                          m_name_changed = false;
    std::chrono::steady_clock::time_point m_wait_started;
};

}} // namespace Slic3r::GUI
