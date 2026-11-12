#ifndef slic3r_GUI_SpoolmanSummaryDialog_hpp_
#define slic3r_GUI_SpoolmanSummaryDialog_hpp_

// "Spoolman Sync Summary" dialog (#95, S8 part 2). Renders the pure model of libslic3r/Spoolman/SpoolmanSummary.hpp through summary_lines():
// the layout decisions live there and are unit-tested; this file only turns lines into widgets. All Spoolman text is shown with
// SetLabelText (plain text: no markup, no '&' mnemonics). Non-modal; it never blocks the sync, and it must be created on the GUI thread.
// NOT verified: how it looks (never launched).

#include "GUI_Utils.hpp"
#include "libslic3r/Spoolman/SpoolmanSummary.hpp"

#include <vector>

class wxScrolledWindow;
class wxWindow;

namespace Slic3r { namespace GUI {

class SpoolmanSummaryDialog : public DPIDialog
{
public:
    SpoolmanSummaryDialog(wxWindow *parent, const Spoolman::SummaryModel &model);

    // Shows the dialog (non-modal, self-destroying) when `preference_on` and the model has content. Returns whether it was shown.
    // GUI thread only.
    static bool show_if_needed(wxWindow *parent, const Spoolman::SummaryModel &model, bool preference_on);

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    void set_details_visible(bool visible);

    wxScrolledWindow          *m_body = nullptr;
    std::vector<wxWindow *>    m_detail_widgets; // table rows / ignored lines hidden by the Details button
    bool                       m_details_visible = true;
};

}} // namespace Slic3r::GUI

#endif
