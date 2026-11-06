#ifndef slic3r_GUI_SnapmakerDevicePage_hpp_
#define slic3r_GUI_SnapmakerDevicePage_hpp_

#include <memory>
#include <vector>

#include <wx/panel.h>
#include <wx/timer.h>
#include <wx/webview.h>

#include "SnapmakerDeviceBridge.hpp"
#include "WebPageServer.hpp"

namespace Slic3r { namespace GUI {

// The content of the Device tab while the live printer agent declares DeviceUi::SnapmakerWeb (ADR 0005):
// Snapmaker's Flutter Home/Device bundle in a webview, served by WebPageServer and bridged to the slicer by an
// SSWCP dispatcher. Everything with logic lives in SnapmakerDeviceBridge; this class only owns the wx objects
// and forwards events, and is compile-checked only.
//
// Lazy: the page server and the webview are created the first time the page is shown, not when the tab is
// built, so a user who never opens the tab pays nothing and no port is bound.
class SnapmakerDevicePage : public wxPanel
{
public:
    explicit SnapmakerDevicePage(wxWindow* parent);
    ~SnapmakerDevicePage() override;

    bool Show(bool show = true) override;
    // Tears the page down completely (see the .cpp); the next Show() builds it again.
    void reset();

private:
    bool start();
    void teardown();
    void on_script_message(wxWebViewEvent& evt);
    void on_navigating(wxWebViewEvent& evt);
    void on_new_window(wxWebViewEvent& evt);
    void on_timer(wxTimerEvent& evt);
    std::vector<SnapmakerDeviceInfo> current_devices() const;

    std::unique_ptr<WebPageServer>        m_server;
    std::string                           m_camera_origin;   // the origin the page's policy was started with (empty = none)
    std::string                           selected_camera_origin() const;
    std::unique_ptr<SnapmakerDeviceBridge> m_bridge;
    wxWebView*                            m_browser{nullptr};        // null once torn down
    wxWebView*                            m_browser_window{nullptr}; // the window itself, until reset() destroys it
    wxTimer                               m_timer;
    bool                                  m_started{false};
};

}} // namespace Slic3r::GUI

#endif
