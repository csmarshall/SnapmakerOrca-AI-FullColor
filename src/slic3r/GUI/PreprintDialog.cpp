#include "PreprintDialog.hpp"

#include <algorithm>
#include <atomic>
#include <thread>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <wx/dialog.h>
#include <wx/sizer.h>
#include <wx/timer.h>
#include <wx/webview.h>

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "DeviceCore/DevManager.h"
#include "I18N.hpp"
#include "DeviceManager.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "NotificationManager.hpp"
#include "WebPageServer.hpp"
#include "Widgets/WebView.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/ProjectTask.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r_version.h"
#include "slic3r/Utils/MoonrakerPreprintPrinter.hpp"
#include "slic3r/Utils/MoonrakerPrinterAgent.hpp"

namespace Slic3r { namespace GUI {

namespace {
// The bundle's pre-print routes. INFERRED from docs/snapmaker-page-contract.md (path=2 is the Device route, 4 print, 5 send); not driven yet:
// S0 / the live GUI run settles them. One place to change.
constexpr const char* kPreprintRoutePrint = "path=4";
constexpr const char* kPreprintRouteSend  = "path=5";
constexpr int         kDialogWidth        = 714;
constexpr int         kDialogHeight       = 750;

// Posts to the UI thread, and does nothing once the app is gone (a detached helper thread can outlive wxTheApp at teardown).
void post_ui(std::function<void()> fn)
{
    if (wxTheApp) wxGetApp().CallAfter(std::move(fn));
}

// Runs `work` on a detached thread, then `on_ui` on the UI thread. An exception escaping `work` is logged, never std::terminate.
void run_guarded(std::function<void()> work, std::function<void()> on_ui)
{
    std::thread([work = std::move(work), on_ui = std::move(on_ui)]() mutable {
        try {
            work();
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "PreprintDialog: background work threw: " << e.what();
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << "PreprintDialog: background work threw";
        }
        post_ui(std::move(on_ui));
    }).detach();
}

// A NON-modal notification (the plater's notification manager); never a modal dialog in this path.
void preprint_notify(const std::string& msg, bool error)
{
    post_ui([msg, error] {
        Plater* plater = wxGetApp().plater();
        NotificationManager* nm = plater ? plater->get_notification_manager() : nullptr;
        if (!nm) return;
        if (error) nm->push_plater_error_notification(msg);
        else       nm->push_plater_warning_notification(msg);
    });
}

} // namespace

// The non-modal window. Owns the page server, the webview and (after attach_bridge) the pre-print bridge.
class PreprintDialog : public wxDialog
{
public:
    PreprintDialog(wxWindow* parent, std::shared_ptr<PreprintCapabilityStore> store, std::shared_ptr<SnapmakerPreprintContext> ctx,
                   std::shared_ptr<SnapmakerPreprintSession> session)
        : wxDialog(parent, wxID_ANY, wxString(), wxDefaultPosition, wxSize(kDialogWidth, kDialogHeight), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
        , m_store(std::move(store)), m_ctx(std::move(ctx)), m_session(std::move(session)), m_timer(this)
    {
        SetSizer(new wxBoxSizer(wxVERTICAL));
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { if (m_bridge) m_bridge->tick(); });
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) {
            if (m_on_user_closed) m_on_user_closed(); // the controller closes everything and calls destroy() on the view
            else Destroy();
        });
    }
    ~PreprintDialog() override
    {
        detach(); // from here on no callback reaches the controller (the bridge's shutdown below fires preprint_done)
        m_timer.Stop();
        if (m_bridge) m_bridge->shutdown();
        m_bridge.reset();
        m_browser = nullptr;
        if (m_server) m_server->stop();
    }

    // After this no callback of this window reaches the controller. The functions themselves are never reassigned or cleared: one of them
    // may be executing right now (a close event that ends in the controller calling destroy()).
    void detach() { if (m_detached) m_detached->store(true); }

    // Starts the loopback server and creates the webview on about:blank. `origin` is the connected printer's plain http origin: it joins the
    // policy's connect-src (the page uploads there). False when the origin is refused, the server does not start or the webview is missing.
    bool init(const std::string& origin)
    {
        m_server = std::make_unique<WebPageServer>(boost::filesystem::path(resources_dir()) / "web" / "flutter_web");
        m_server->set_capability_store(m_store);
        if (origin.empty() || !m_server->set_extra_origin(origin)) {
            BOOST_LOG_TRIVIAL(error) << "PreprintDialog: the printer origin is not usable for the page's policy: " << origin;
            return false;
        }
        if (!m_server->start()) {
            BOOST_LOG_TRIVIAL(error) << "PreprintDialog: the page server did not start";
            return false;
        }
        m_browser = WebView::CreateWebView(this, "about:blank");
        if (!m_browser) return false;
        m_browser->SetUserAgent(wxString::FromUTF8(snapmaker_user_agent(SLIC3R_VERSION, wxGetApp().dark_mode())));
        m_browser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, [this](wxWebViewEvent& evt) {
            if (!m_bridge || !m_browser) return;
            if (m_on_message) m_on_message(); // the page is alive (first message of the page-load watchdog)
            wxString url = evt.GetURL();
            if (url.empty()) url = m_browser->GetCurrentURL();
            m_bridge->on_script_message(url.ToStdString(wxMBConvUTF8()), evt.GetString().ToStdString(wxMBConvUTF8()));
        });
        m_browser->Bind(wxEVT_WEBVIEW_NAVIGATING, [this](wxWebViewEvent& evt) {
            const std::string url = evt.GetURL().ToStdString(wxMBConvUTF8());
            if (url.empty() || url == "about:blank") return;
            if (!m_server || !snapmaker_is_page_origin(url, m_server->port())) {
                BOOST_LOG_TRIVIAL(warning) << "PreprintDialog: blocked navigation away from the page origin";
                evt.Veto();
            }
        });
        m_browser->Bind(wxEVT_WEBVIEW_NEWWINDOW, [](wxWebViewEvent& evt) { evt.Veto(); });
        m_browser->Bind(wxEVT_WEBVIEW_ERROR, [this](wxWebViewEvent& e) {
            const std::string what = e.GetString().ToStdString(wxMBConvUTF8());
            BOOST_LOG_TRIVIAL(warning) << "PreprintDialog: webview error " << e.GetInt() << ": " << what;
            if (m_on_error) m_on_error(what);
        });
        GetSizer()->Add(m_browser, 1, wxEXPAND);
        Layout();
        return true;
    }

    bool handler_added() const { return m_browser && WebView::IsScriptMessageHandlerAdded(m_browser); }

    void attach_bridge(std::uint64_t epoch)
    {
        SnapmakerDeviceBridge::Options opt;
        opt.port             = m_server->port();
        opt.http_host        = "http://127.0.0.1:" + std::to_string(m_server->port());
        opt.preprint         = m_ctx;
        opt.preprint_session = m_session;
        opt.preprint_epoch   = epoch;
        // Replies are queued, not run inside the script-message handler (RunScript can yield on WKWebView), as in the Device tab.
        opt.run_script = [this](const std::string& js) {
            CallAfter([this, js] {
                if (m_browser && m_bridge && !m_bridge->is_shut_down()) WebView::RunScript(m_browser, wxString::FromUTF8(js));
            });
        };
        // The session's blocking printer calls run on a worker thread; the continuation returns to the UI thread through the app.
        opt.preprint_run  = [](std::function<void()> work, std::function<void()> on_ui) { run_guarded(std::move(work), std::move(on_ui)); };
        opt.preprint_post = [](std::function<void()> fn) { post_ui(std::move(fn)); };
        opt.preprint_done = m_on_done;
        m_bridge = std::make_unique<SnapmakerDeviceBridge>(std::move(opt));
        m_timer.Start(1000);
    }
    void load_page(bool send_only)
    {
        if (m_browser && m_server) m_browser->LoadURL(wxString::FromUTF8(m_server->start_url(send_only ? kPreprintRouteSend : kPreprintRoutePrint)));
    }

    // Set once by the factory before the window is shown, never reassigned. Each checks the shared `m_detached` token first.
    std::function<void(bool)>               m_on_done;
    std::function<void()>                   m_on_user_closed, m_on_message;
    std::function<void(const std::string&)> m_on_error;
    std::shared_ptr<std::atomic<bool>>      m_detached = std::make_shared<std::atomic<bool>>(false);

private:
    std::shared_ptr<PreprintCapabilityStore>  m_store;
    std::shared_ptr<SnapmakerPreprintContext> m_ctx;
    std::shared_ptr<SnapmakerPreprintSession> m_session;
    std::unique_ptr<WebPageServer>            m_server;
    std::unique_ptr<SnapmakerDeviceBridge>    m_bridge;
    wxWebView*                                m_browser{nullptr};
    wxTimer                                   m_timer;
};

namespace {

class WxPreprintView final : public PreprintView
{
public:
    explicit WxPreprintView(PreprintDialog* d) : m_dlg(d) {}
    bool script_handler_added() override { return m_dlg && m_dlg->handler_added(); }
    void show() override { if (m_dlg) m_dlg->Show(); } // Show(), never a modal loop
    void raise() override { if (m_dlg) { m_dlg->Show(); m_dlg->Raise(); } }
    void set_title(const std::string& t) override { if (m_dlg) m_dlg->SetTitle(_L("Print") + ": " + wxString::FromUTF8(t)); }
    void attach_bridge(std::uint64_t epoch) override { if (m_dlg) m_dlg->attach_bridge(epoch); }
    void load_page(bool send_only) override { if (m_dlg) m_dlg->load_page(send_only); }
    void destroy() override
    {
        if (!m_dlg) return;
        PreprintDialog* d = m_dlg;
        m_dlg = nullptr;
        d->detach();
        d->Hide();
        d->Destroy(); // deferred deletion: safe from inside the dialog's own close event
    }

private:
    wxWeakRef<PreprintDialog> m_dlg;
};

class WxScheduler final : public PreprintScheduler
{
public:
    std::chrono::steady_clock::time_point now() override { return std::chrono::steady_clock::now(); }
    void call_after(std::function<void()> fn) override { post_ui(std::move(fn)); }
    void call_later(std::chrono::milliseconds delay, std::function<void()> fn) override
    {
        // One-shot timer owned by itself: wxTimer::Notify runs on the UI thread and the object deletes itself afterwards.
        struct OneShot : wxTimer
        {
            std::function<void()> fn;
            void Notify() override
            {
                auto f = std::move(fn);
                post_ui([self = this] { delete self; });
                if (f) f();
            }
        };
        auto* t = new OneShot;
        t->fn = std::move(fn);
        t->StartOnce(int(delay.count()));
    }
    void run_background(std::function<void()> work, std::function<void()> on_ui) override
    {
        run_guarded(std::move(work), std::move(on_ui));
    }
};

} // namespace

void notify_preprint(const std::string& message, bool error) { preprint_notify(message, error); }

std::shared_ptr<PreprintDialogController> create_preprint_controller(wxWindow* parent, std::shared_ptr<MoonrakerPrinterAgent> agent)
{
    if (!agent) return nullptr;
    auto store   = std::make_shared<PreprintCapabilityStore>();
    auto ctx     = std::make_shared<SnapmakerPreprintContext>();
    auto printer = std::make_shared<MoonrakerPreprintPrinter>(std::move(agent));
    auto session = std::make_shared<SnapmakerPreprintSession>(ctx, printer);
    auto holder  = std::make_shared<std::weak_ptr<PreprintDialogController>>();

    PreprintDialogController::Deps d;
    d.scheduler    = std::make_shared<WxScheduler>();
    d.store        = store;
    d.context      = ctx;
    d.session      = session;
    d.bind_printer = [printer] { return printer->bind_current(); };
    d.make_view    = [parent, store, ctx, session, holder, printer](std::uint64_t open_id) -> std::unique_ptr<PreprintView> {
        // One printer, three sources: the CSP origin comes from the AGENT's connected device (the one the pin is taken from); the selected
        // machine must name the same host, otherwise the send is refused. No fallback, no print_host.
        const std::string origin = printer->connected_origin();
        DeviceManager*       dev      = wxGetApp().getDeviceManager();
        const MachineObject* selected = dev ? dev->get_selected_machine() : nullptr;
        const std::string    selected_origin = selected ? snapmaker_camera_origin_for(selected->get_dev_ip(), selected->get_dev_id()) : std::string();
        auto lower = [](std::string v) { for (char& c : v) c = char(std::tolower((unsigned char) c)); return v; };
        if (origin.empty()) {
            preprint_notify("The connected printer's address is not a plain host, so the print window cannot be opened for it.", true);
            return nullptr;
        }
        if (selected_origin.empty() || lower(selected_origin) != lower(origin)) {
            preprint_notify("The selected printer and the connected printer differ (" + selected_origin + " vs " + origin + "). Reconnect and try again.", true);
            return nullptr;
        }
        auto* dlg = new PreprintDialog(parent, store, ctx, session);
        if (!dlg->init(origin)) {
            dlg->Destroy();
            return nullptr;
        }
        auto detached = dlg->m_detached;
        auto alive    = [holder, detached]() -> std::shared_ptr<PreprintDialogController> {
            return detached->load() ? nullptr : holder->lock();
        };
        dlg->m_on_done        = [alive, open_id](bool started) { if (auto c = alive()) c->on_page_done(open_id, started); };
        dlg->m_on_user_closed = [alive, open_id] { if (auto c = alive()) c->on_user_closed(open_id); };
        dlg->m_on_message     = [alive, open_id] { if (auto c = alive()) c->on_page_message(open_id); };
        dlg->m_on_error       = [alive, open_id](const std::string& what) { if (auto c = alive()) c->on_page_error(open_id, what); };
        return std::make_unique<WxPreprintView>(dlg);
    };
    d.report_error = [](const std::string& msg) { preprint_notify(msg, true); };
    d.notify_info  = [](const std::string& msg) { preprint_notify(msg, false); };
    d.switch_to_device_tab = [] {
        if (MainFrame* frame = wxGetApp().mainframe) frame->request_select_tab(TAB_ID_MONITOR);
    };
    auto c  = PreprintDialogController::create(std::move(d));
    *holder = c;
    return c;
}

bool collect_preprint_request(PartPlate& plate, const PresetBundle& bundle, const std::string& name, bool start, bool switch_tab,
                              PreprintRequest& out, std::string& error)
{
    out = PreprintRequest{};
    // Only a finished, valid and printable slice is offered: never a stale or still-running one (the snapshot must be what was sliced).
    if (!plate.is_slice_result_valid() || !plate.is_slice_result_ready_for_print()) {
        error = "Slice the plate first.";
        return false;
    }
    const std::string gcode = plate.get_tmp_gcode_path();
    if (gcode.empty() || !boost::filesystem::exists(gcode)) {
        error = "Slice the plate first.";
        return false;
    }
    out.gcode_path           = gcode;
    out.requested_name       = name;
    out.start_requested      = start;
    out.switch_to_device_tab = switch_tab;

    const DynamicPrintConfig cfg = bundle.full_config();
    const auto strings = [&](const char* key) { const auto* o = cfg.option<ConfigOptionStrings>(key); return o ? o->values : std::vector<std::string>{}; };
    const auto floats  = [&](const char* key) { const auto* o = cfg.option<ConfigOptionFloats>(key); return o ? o->values : std::vector<double>{}; };
    SnapmakerPreprintPlate& p = out.plate;
    p.filament_colour   = strings("filament_colour");
    p.filament_type     = strings("filament_type");
    p.filament_density  = floats("filament_density");
    p.filament_diameter = floats("filament_diameter");
    p.nozzle_diameter   = floats("nozzle_diameter");
    p.machine_model     = cfg.opt_string("printer_model");
    out.extruder_count  = int(p.nozzle_diameter.size());
    if (GCodeProcessorResult* result = plate.get_slice_result())
        p.estimated_time_s = result->print_statistics.modes[size_t(PrintEstimatedStatistics::ETimeMode::Normal)].time;

    // The filaments the plate really uses (0-based ids) and their used length, from the slice (never from the live project).
    for (const FilamentInfo& f : plate.get_slice_filaments_info()) {
        if (f.id < 0) continue;
        out.plate_filaments.push_back(f.id);
        if (size_t(f.id) < p.filament_diameter.size()) {
            const double d = p.filament_diameter[f.id];
            p.volume_mm3[f.id] = double(f.used_m) * 1000.0 * 3.14159265358979323846 * (d * 0.5) * (d * 0.5);
        }
    }
    if (out.plate_filaments.empty()) {
        error = "The plate reports no used filaments.";
        return false;
    }
    // Snapmaker's own pre-print flow sends the plain sliced G-code without running post_process scripts (Plater.cpp send_gcode_legacy U1 branch
    // takes plate->get_tmp_gcode_path() directly); we do the same, and say so when scripts are configured (see out.notice).
    if (const auto* pp = cfg.option<ConfigOptionStrings>("post_process"); pp && std::any_of(pp->values.begin(), pp->values.end(), [](const std::string& v) { return !v.empty(); }))
        out.notice = "Post-processing scripts are configured but are not run for this send (the plain sliced G-code is sent, as in Snapmaker Orca).";
    // Thumbnails are not supplied yet (the page tolerates their absence); the filament-to-extruder map is left to the page.
    return true;
}

}} // namespace Slic3r::GUI
