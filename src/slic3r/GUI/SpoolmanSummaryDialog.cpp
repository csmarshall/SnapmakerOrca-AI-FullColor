#include "SpoolmanSummaryDialog.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"

#include <wx/colour.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/statline.h>

namespace Slic3r { namespace GUI {

using Spoolman::SummaryLine;

namespace {
// Static text with the model's string, shown literally.
wxStaticText *plain_text(wxWindow *parent, const std::string &s, int wrap_width = -1)
{
    auto *t = new wxStaticText(parent, wxID_ANY, wxEmptyString);
    t->SetLabelText(wxString::FromUTF8(s.c_str()));
    if (wrap_width > 0)
        t->Wrap(wrap_width);
    return t;
}
} // namespace

SpoolmanSummaryDialog::SpoolmanSummaryDialog(wxWindow *parent, const Spoolman::SummaryModel &model)
    : DPIDialog(parent, wxID_ANY, _L("Spoolman Sync Summary"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    SetFont(wxGetApp().normal_font());
    const bool dark = wxGetApp().dark_mode();
    SetBackgroundColour(dark ? wxColour(0x2D, 0x2D, 0x31) : *wxWHITE);

    auto *main_sizer = new wxBoxSizer(wxVERTICAL);
    m_body           = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(640), FromDIP(420)), wxVSCROLL);
    m_body->SetScrollRate(0, FromDIP(10));
    m_body->SetBackgroundColour(GetBackgroundColour());
    auto *body_sizer = new wxBoxSizer(wxVERTICAL);
    const int wrap   = FromDIP(600);

    wxFlexGridSizer *grid = nullptr; // current table
    auto end_table        = [&]() { grid = nullptr; };
    auto add_detail       = [&](wxWindow *w) { m_detail_widgets.push_back(w); };
    auto table_cells      = [&](const SummaryLine &l, bool head) {
        if (!grid) {
            grid = new wxFlexGridSizer(4, FromDIP(4), FromDIP(14));
            grid->AddGrowableCol(3);
            body_sizer->Add(grid, 0, wxEXPAND | wxLEFT | wxBOTTOM, FromDIP(12));
        }
        for (const std::string &c : l.cells) {
            wxStaticText *t = plain_text(m_body, c);
            if (head)
                t->SetFont(t->GetFont().Bold());
            else
                add_detail(t);
            grid->Add(t, 0, wxALIGN_CENTER_VERTICAL);
        }
    };

    for (const SummaryLine &l : Spoolman::summary_lines(model)) {
        using K = SummaryLine::Kind;
        if (l.kind != K::table_head && l.kind != K::table_row && l.kind != K::notice_head && l.kind != K::notice_row)
            end_table();
        switch (l.kind) {
        case K::notice:
        case K::footer:
            body_sizer->Add(plain_text(m_body, l.cells[0], wrap), 0, wxEXPAND | wxALL, FromDIP(6));
            break;
        case K::slot_header: {
            body_sizer->Add(new wxStaticLine(m_body), 0, wxEXPAND | wxTOP, FromDIP(8));
            auto *row = new wxBoxSizer(wxHORIZONTAL);
            // colour swatch: a small coloured panel (invalid colour string -> light grey)
            auto *sw = new wxPanel(m_body, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(16), FromDIP(16)));
            wxColour c(wxString::FromUTF8(l.cells[3].c_str()));
            sw->SetBackgroundColour(c.IsOk() ? c : wxColour(0xCC, 0xCC, 0xCC));
            row->Add(sw, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
            wxStaticText *name = plain_text(m_body, l.cells[0] + ": " + l.cells[1]);
            name->SetFont(name->GetFont().Bold());
            row->Add(name, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
            row->Add(plain_text(m_body, l.cells[2]), 0, wxALIGN_CENTER_VERTICAL);
            body_sizer->Add(row, 0, wxEXPAND | wxALL, FromDIP(6));
            break;
        }
        case K::slot_preset:
        case K::slot_note:
        case K::slot_flag:
            body_sizer->Add(plain_text(m_body, l.cells[0], wrap), 0, wxEXPAND | wxLEFT | wxBOTTOM, FromDIP(12));
            break;
        case K::table_head:
        case K::notice_head:
            table_cells(l, true);
            break;
        case K::table_row:
        case K::notice_row:
            table_cells(l, false);
            break;
        case K::catalog_note: {
            wxStaticText *t = plain_text(m_body, l.cells[0], wrap);
            add_detail(t);
            body_sizer->Add(t, 0, wxEXPAND | wxALL, FromDIP(6));
            break;
        }
        case K::ignored: {
            wxStaticText *t = plain_text(m_body, l.cells[0], wrap);
            add_detail(t);
            body_sizer->Add(t, 0, wxEXPAND | wxLEFT | wxBOTTOM, FromDIP(12));
            break;
        }
        }
    }
    m_body->SetSizer(body_sizer);
    main_sizer->Add(m_body, 1, wxEXPAND | wxALL, FromDIP(8));

    auto *buttons = new wxBoxSizer(wxHORIZONTAL);
    auto *details = new Button(this, _L("Details"));
    details->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    auto *close = new Button(this, _L("Close"));
    close->SetStyle(ButtonStyle::Confirm, ButtonType::Window);
    buttons->AddStretchSpacer();
    buttons->Add(details, 0, wxRIGHT, FromDIP(8));
    buttons->Add(close, 0);
    main_sizer->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(10));
    SetSizerAndFit(main_sizer);

    details->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { set_details_visible(!m_details_visible); });
    close->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { Close(); });
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent &) { Destroy(); }); // non-modal: owns itself
    wxGetApp().UpdateDlgDarkUI(this);
    CenterOnParent();
}

void SpoolmanSummaryDialog::set_details_visible(bool visible)
{
    m_details_visible = visible;
    for (wxWindow *w : m_detail_widgets)
        w->Show(visible);
    m_body->Layout();
    m_body->FitInside();
}

void SpoolmanSummaryDialog::on_dpi_changed(const wxRect &) { Fit(); Refresh(); }

bool SpoolmanSummaryDialog::show_if_needed(wxWindow *parent, const Spoolman::SummaryModel &model, bool preference_on)
{
    if (!preference_on || !model.show_dialog)
        return false;
    (new SpoolmanSummaryDialog(parent, model))->Show();
    return true;
}

}} // namespace Slic3r::GUI
