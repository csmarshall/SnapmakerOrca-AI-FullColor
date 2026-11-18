#ifndef slic3r_GUI_SpoolmanSummaryDialog_hpp_
#define slic3r_GUI_SpoolmanSummaryDialog_hpp_

// "Spoolman Sync Summary" dialog (#95, S8 part 2). Renders the pure model of libslic3r/Spoolman/SpoolmanSummary.hpp through summary_lines():
// the layout decisions live there and are unit-tested; this file only turns lines into widgets. All Spoolman text is shown with
// SetLabelText (plain text: no markup, no '&' mnemonics). Non-modal; it never blocks the sync, and it must be created on the GUI thread.
// NOT verified: how it looks (never launched).

#include "GUI_Utils.hpp"
#include "libslic3r/Spoolman/SpoolmanSummary.hpp"

#include <functional>
#include <vector>

#include <wx/timer.h>

class wxScrolledWindow;
class wxStaticText;
class wxWindow;

namespace Slic3r { namespace GUI {

class SpoolmanSummaryDialog : public DPIDialog
{
public:
    // `rerun_as_sync` (#131), when set, adds a "Run again as Sync (changes only)" button: it closes this dialog and calls the function (no new fetch).
    SpoolmanSummaryDialog(wxWindow *parent, const Spoolman::SummaryModel &model, std::function<void()> rerun_as_sync = {});

    // Shows the dialog (non-modal, self-destroying) when `preference_on` and the model has content. Returns whether it was shown.
    // GUI thread only.
    static bool show_if_needed(wxWindow *parent, const Spoolman::SummaryModel &model, bool preference_on, std::function<void()> rerun_as_sync = {});

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    void set_details_visible(bool visible);
    // #140 countdown: on_tick runs each second; stop_countdown(reason) cancels it and hides the label and the "Keep open" button.
    void on_tick();
    void stop_countdown(const char *reason);
    void log_event(const std::string &event);

    wxScrolledWindow          *m_body = nullptr;
    std::vector<wxWindow *>    m_detail_widgets; // table rows / ignored lines hidden by the Details button
    bool                       m_details_visible = true;
    wxTimer                    m_timer;                  // 1 s ticks while the countdown runs; stopped before the dialog is destroyed
    wxStaticText              *m_countdown_label = nullptr;
    wxWindow                  *m_keep_open       = nullptr;
    int                        m_remaining       = 0;    // seconds left; 0 with no timer running = no auto-close
    bool                       m_closing         = false;
};

}} // namespace Slic3r::GUI

#endif
