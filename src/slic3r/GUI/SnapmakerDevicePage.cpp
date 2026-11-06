#include "SnapmakerDevicePage.hpp"

#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>

#include <wx/sizer.h>
#include <wx/stattext.h>

#include "I18N.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "DeviceManager.hpp"
#include "DeviceCore/DevManager.h"
#include "Widgets/WebView.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/Utils/IPrinterAgent.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r_version.h"

namespace Slic3r { namespace GUI {

SnapmakerDevicePage::SnapmakerDevicePage(wxWindow* parent) : wxPanel(parent), m_timer(this)
{
    SetSizer(new wxBoxSizer(wxVERTICAL));
    Bind(wxEVT_TIMER, &SnapmakerDevicePage::on_timer, this);
}

SnapmakerDevicePage::~SnapmakerDevicePage()
{
    teardown();
}

// Hidden (another tab selected) or removed from the book: nothing may keep polling the machine. The loopback server stays
// up while the page lives (it is idle, loopback-only and avoids a reload on every tab switch); reset() ends it.
bool SnapmakerDevicePage::Show(bool show)
{
    const bool changed = wxPanel::Show(show);
    if (show && !m_started) {
        start();
    } else if (m_bridge) {
        m_bridge->set_active(show);
        if (show) m_timer.Start(1000); else m_timer.Stop();
    }
    return changed;
}

// The page leaves the Device tab (the active agent changed, or agents were switched off): drop everything, including the
// webview and its subscriptions, so showing it again starts from a clean page rather than one holding a dead bridge.
void SnapmakerDevicePage::reset()
{
    teardown();
    if (m_browser_window) {
        GetSizer()->Detach(m_browser_window);
        m_browser_window->Destroy();
        m_browser_window = nullptr;
    }
    GetSizer()->Clear(true);
    m_started = false;
}

bool SnapmakerDevicePage::start()
{
    m_started = true; // one attempt per page; a failure shows a message instead of retrying every Show()

    m_server = std::make_unique<WebPageServer>(boost::filesystem::path(resources_dir()) / "web" / "flutter_web");
    // The camera snapshot is loaded by the page straight from the selected machine, so its origin (and only that one) is added to
    // the policy for this page load. It is fixed here: selecting another machine later needs the page reloaded.
    if (DeviceManager* dev = wxGetApp().getDeviceManager())
        if (const MachineObject* selected = dev->get_selected_machine()) {
            const std::string address = selected->get_dev_ip().empty() ? selected->get_dev_id() : selected->get_dev_ip();
            const std::string origin  = snapmaker_camera_origin(address);
            if (!origin.empty() && m_server->set_extra_origin(origin))
                BOOST_LOG_TRIVIAL(info) << "SnapmakerDevicePage: camera origin " << origin;
        }
    if (!m_server->start()) {
        BOOST_LOG_TRIVIAL(error) << "SnapmakerDevicePage: the page server did not start";
        m_server.reset();
        GetSizer()->Add(new wxStaticText(this, wxID_ANY, _L("The Snapmaker device page could not be started.")), 0, wxALL, 20);
        Layout();
        return false;
    }

    SnapmakerDeviceBridge::Options opt;
    opt.port      = m_server->port();
    opt.http_host = "http://127.0.0.1:" + std::to_string(m_server->port());
    // Replies are queued rather than run inside the script-message handler: on WKWebView RunScript can yield
    // to the event loop, which would re-enter this handler. CallAfter keeps them in order and drops them if
    // the page is destroyed first.
    opt.run_script = [this](const std::string& js) { CallAfter([this, js]() {
        if (m_browser && m_bridge && !m_bridge->is_shut_down())
            WebView::RunScript(m_browser, wxString::FromUTF8(js));
    }); };
    opt.devices = [this]() { return current_devices(); };
    // Machine state: the live agent's read-only object query runs on Http's thread; the result is handed to the UI thread
    // through the app (not this page, which may be gone by then). The bridge ignores results after its shutdown.
    opt.query_state = [](const std::vector<std::string>& objects, std::function<void(bool, nlohmann::json)> done) {
        auto* net   = wxGetApp().getAgent();
        auto  agent = net ? net->get_printer_agent() : nullptr;
        if (!agent) {
            done(false, nlohmann::json::object());
            return;
        }
        agent->query_machine_objects_async(objects, [done](bool ok, nlohmann::json result) {
            wxGetApp().CallAfter([done, ok, result = std::move(result)]() mutable { done(ok, std::move(result)); });
        });
    };
    opt.fetch_resource = [](const std::string& path, std::size_t max_bytes, std::function<void(bool, std::string)> done) {
        auto* net   = wxGetApp().getAgent();
        auto  agent = net ? net->get_printer_agent() : nullptr;
        if (!agent) {
            done(false, std::string());
            return;
        }
        agent->read_machine_resource_async(path, max_bytes, [done](bool ok, std::string body) {
            wxGetApp().CallAfter([done, ok, body = std::move(body)]() mutable { done(ok, std::move(body)); });
        });
    };
    opt.camera_monitor = [](bool start, const nlohmann::json& params, std::function<void(bool, nlohmann::json)> done) {
        auto* net   = wxGetApp().getAgent();
        auto  agent = net ? net->get_printer_agent() : nullptr;
        if (!agent) {
            done(false, nlohmann::json::object());
            return;
        }
        agent->camera_monitor_async(start, params, [done](bool ok, nlohmann::json result) {
            wxGetApp().CallAfter([done, ok, result = std::move(result)]() mutable { done(ok, std::move(result)); });
        });
    };
    opt.switch_tab = [](SnapmakerTab tab) {
        MainFrame* frame = wxGetApp().mainframe;
        if (!frame)
            return false;
        const char* id = TAB_ID_HOME;
        switch (tab) {
        case SnapmakerTab::Home: id = TAB_ID_HOME; break;
        case SnapmakerTab::Prepare: id = TAB_ID_PREPARE; break;
        case SnapmakerTab::Preview: id = TAB_ID_PREVIEW; break;
        case SnapmakerTab::Monitor: id = TAB_ID_MONITOR; break; // this page's own tab: the Device view
        case SnapmakerTab::MultiDevice: id = TAB_ID_MULTI_DEVICE; break;
        case SnapmakerTab::Project: id = TAB_ID_PROJECT; break;
        case SnapmakerTab::Calibration: id = TAB_ID_CALIBRATION; break;
        }
        wxGetApp().CallAfter([frame, id]() { frame->request_select_tab(id); });
        return true;
    };
    m_bridge    = std::make_unique<SnapmakerDeviceBridge>(std::move(opt));

    m_browser = WebView::CreateWebView(this, "");
    m_browser_window = m_browser;
    if (!m_browser) {
        BOOST_LOG_TRIVIAL(error) << "SnapmakerDevicePage: could not create the webview";
        teardown();
        return false;
    }
    // Page compatibility: the bundle (and Snapmaker's cloud) read the SM-Slicer/v<version> token, so the user
    // agent replaces the Bambu one CreateWebView sets.
    m_browser->SetUserAgent(wxString::FromUTF8(snapmaker_user_agent(SLIC3R_VERSION, wxGetApp().dark_mode())));
    m_browser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &SnapmakerDevicePage::on_script_message, this);
    m_browser->Bind(wxEVT_WEBVIEW_NAVIGATING, &SnapmakerDevicePage::on_navigating, this);
    m_browser->Bind(wxEVT_WEBVIEW_ERROR, [](wxWebViewEvent& e) {
        BOOST_LOG_TRIVIAL(warning) << "SnapmakerDevicePage: webview error " << e.GetInt() << ": " << e.GetString().ToStdString(wxMBConvUTF8())
                                   << " url=" << e.GetURL().ToStdString(wxMBConvUTF8()).substr(0, 200);
    });
    m_browser->Bind(wxEVT_WEBVIEW_LOADED, [](wxWebViewEvent& e) {
        BOOST_LOG_TRIVIAL(debug) << "SnapmakerDevicePage: loaded " << e.GetURL().ToStdString(wxMBConvUTF8()).substr(0, 120);
    });
    m_browser->Bind(wxEVT_WEBVIEW_NEWWINDOW, &SnapmakerDevicePage::on_new_window, this);
    GetSizer()->Add(m_browser, 1, wxEXPAND);
    Layout();

    m_timer.Start(1000);
    // path=2 is the bundle's Device route (Home is path=1); the Home card's Control button asks for it with sw_SwitchTab.
    m_browser->LoadURL(wxString::FromUTF8(m_server->start_url("path=2")));
    return true;
}

void SnapmakerDevicePage::teardown()
{
    m_timer.Stop();
    if (m_bridge)
        m_bridge->shutdown();
    m_bridge.reset();
    m_browser = nullptr; // wx destroys the child window with this panel
    if (m_server) {
        m_server->stop();
        m_server.reset();
    }
}

void SnapmakerDevicePage::on_script_message(wxWebViewEvent& evt)
{
    if (!m_bridge || !m_browser)
        return;
    BOOST_LOG_TRIVIAL(trace) << "SnapmakerDevicePage: script message " << evt.GetString().ToStdString(wxMBConvUTF8()).substr(0, 900);
    // Top-level document origin only. wx reports the main frame's URL (WKWebView's and Edge's GetCurrentURL), not the URL of
    // the frame that posted the message, so a message from a sub-frame is judged by the page that contains it. That is
    // acceptable only because the page cannot navigate away (on_navigating vetoes foreign origins) and the bundle is
    // vendored with a checked hash; sub-frames inside it are not otherwise restricted.
    // The page server's Content-Security-Policy now carries frame-src 'none' (resources/web/flutter_web.csp), so the bundle cannot host a
    // sub-frame at all; the origin check is still top-level only and is not what separates frames.
    wxString url = evt.GetURL();
    if (url.empty())
        url = m_browser->GetCurrentURL();
    m_bridge->on_script_message(url.ToStdString(wxMBConvUTF8()), evt.GetString().ToStdString(wxMBConvUTF8()));
}

void SnapmakerDevicePage::on_navigating(wxWebViewEvent& evt)
{
    // The page may only ever show its own origin; an external link cannot become a window that talks SSWCP.
    const std::string url = evt.GetURL().ToStdString(wxMBConvUTF8());
    BOOST_LOG_TRIVIAL(debug) << "SnapmakerDevicePage: navigating to " << url.substr(0, url.find('?'));
    if (url.empty() || url == "about:blank")
        return;
    if (!m_server || !snapmaker_is_page_origin(url, m_server->port())) {
        BOOST_LOG_TRIVIAL(warning) << "SnapmakerDevicePage: blocked navigation away from the page origin";
        evt.Veto();
    }
}

void SnapmakerDevicePage::on_new_window(wxWebViewEvent& evt)
{
    BOOST_LOG_TRIVIAL(warning) << "SnapmakerDevicePage: blocked a new-window request";
    evt.Veto();
}

void SnapmakerDevicePage::on_timer(wxTimerEvent&)
{
    if (m_bridge)
        m_bridge->tick();
}

std::vector<SnapmakerDeviceInfo> SnapmakerDevicePage::current_devices() const
{
    std::vector<SnapmakerDeviceInfo> out;
    DeviceManager* dev = wxGetApp().getDeviceManager();
    if (!dev)
        return out;
    // Scoped to the live agent, so a Bambu or Moonraker machine from an earlier session does not show up here.
    // A failed connect keeps the machine selected (the agent reports it separately), so the page can show the reason
    // next to the machine the failure belongs to.
    const MachineObject* selected = dev->get_selected_machine();
    std::string          connect_error;
    std::string          preset_model;
    if (wxGetApp().preset_bundle)
        preset_model = wxGetApp().preset_bundle->printers.get_edited_preset().config.opt_string("printer_model");
    if (auto* net = wxGetApp().getAgent())
        if (auto pa = net->get_printer_agent())
            connect_error = pa->last_connect_error();
    for (const auto& kv : dev->get_my_machine_list(dev->get_current_printer_agent_id())) {
        const MachineObject* m = kv.second;
        if (!m)
            continue;
        const bool is_selected = m == selected;
        // The model comes from the printer preset (printer_model, e.g. "Snapmaker U1"), not from a printer check: the
        // preset is what chose this agent. Other machines of the agent have no preset of their own, so they carry
        // the machine's own type string.
        SnapmakerDeviceInfo d;
        d.dev_id    = m->get_dev_id();
        d.name      = m->get_dev_name();
        d.ip        = m->get_dev_ip();
        d.model     = (is_selected && !preset_model.empty()) ? preset_model : m->printer_type;
        d.online    = const_cast<MachineObject*>(m)->is_online();
        d.connected = is_selected;
        d.error     = is_selected ? connect_error : std::string();
        out.push_back(std::move(d));
    }
    return out;
}

}} // namespace Slic3r::GUI
