#ifndef slic3r_GUI_PreprintDialog_hpp_
#define slic3r_GUI_PreprintDialog_hpp_

// Send-flow slice S5, the wx glue: a NON-MODAL dialog around a webview that shows the bundled Snapmaker pre-print page, plus the app-side
// factory that wires the pure PreprintDialogController (PreprintDialogController.hpp) to it. All logic is in the controller; this file only
// owns wx objects and forwards events. Compile-checked only: the real webview behaviour (handler install, the page's fetch and cookie flow,
// the #84 hang) can only be shown by a live GUI run.

#include <memory>
#include <string>

#include <wx/window.h>

#include "PreprintDialogController.hpp"

namespace Slic3r {
class PartPlate;
class PresetBundle;
class MoonrakerPrinterAgent;
}

namespace Slic3r { namespace GUI {

// Builds a controller for one send: its own capability store, context, session and Moonraker adapter bound to `agent`, and a view factory
// producing the non-modal dialog under `parent`. Null when `agent` is null.
std::shared_ptr<PreprintDialogController> create_preprint_controller(wxWindow* parent, std::shared_ptr<MoonrakerPrinterAgent> agent);

// Fills a request from the plate (G-code path, filaments, colours, nozzle data, print time); false + error when the plate has no usable data.
// `name`/`start`/`switch_tab` come from PrintHostSendDialog.
bool collect_preprint_request(PartPlate& plate, const PresetBundle& bundle, const std::string& name, bool start, bool switch_tab,
                              PreprintRequest& out, std::string& error);

}} // namespace Slic3r::GUI

#endif
