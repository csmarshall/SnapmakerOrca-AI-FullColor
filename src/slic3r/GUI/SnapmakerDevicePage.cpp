#include "SnapmakerDevicePage.hpp"

#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>

#include <wx/sizer.h>
#include <wx/stattext.h>

#include "I18N.hpp"
#include "GUI_App.hpp"
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

bool SnapmakerDevicePage::Show(bool show)
{
    const bool changed = wxPanel::Show(show);
    if (show && !m_started)
        start();
    return changed;
}

bool SnapmakerDevicePage::start()
{
    m_started = true; // one attempt per page; a failure shows a message instead of retrying every Show()

    m_server = std::make_unique<WebPageServer>(boost::filesystem::path(resources_dir()) / "web" / "flutter_web");
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
    m_bridge    = std::make_unique<SnapmakerDeviceBridge>(std::move(opt));

    m_browser = WebView::CreateWebView(this, "");
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
    m_browser->Bind(wxEVT_WEBVIEW_NEWWINDOW, &SnapmakerDevicePage::on_new_window, this);
    GetSizer()->Add(m_browser, 1, wxEXPAND);
    Layout();

    m_timer.Start(1000);
    m_browser->LoadURL(wxString::FromUTF8(m_server->start_url()));
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
    // The frame that sent the message when wx reports it, else the page currently loaded.
    wxString url = evt.GetURL();
    if (url.empty())
        url = m_browser->GetCurrentURL();
    m_bridge->on_script_message(url.ToStdString(wxMBConvUTF8()), evt.GetString().ToStdString(wxMBConvUTF8()));
}

void SnapmakerDevicePage::on_navigating(wxWebViewEvent& evt)
{
    // The page may only ever show its own origin; an external link cannot become a window that talks SSWCP.
    const std::string url = evt.GetURL().ToStdString(wxMBConvUTF8());
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
