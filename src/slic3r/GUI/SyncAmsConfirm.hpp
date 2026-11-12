#pragma once

// Pure decision logic for the confirm shown by Sidebar::sync_ams_list() when SyncAmsInfoDialog is
// skipped (empty project). Mirrors Snapmaker Orca's SyncAmsDialog; kept free of wx so it is unit-testable.

#include <string>

namespace Slic3r { namespace GUI {

enum class SyncAmsConfirmKind {
    FirstTime, // nothing synced yet for this device: single "Yes" + Cancel
    Repeat     // already synced: "Sync" (changes only) / "Resync" (all) + Cancel
};

// First time == no stored ams_filament_ids for the device.
inline SyncAmsConfirmKind sync_ams_confirm_kind(const std::string &stored_ams_filament_ids)
{
    return stored_ams_filament_ids.empty() ? SyncAmsConfirmKind::FirstTime : SyncAmsConfirmKind::Repeat;
}

// Result codes match wx ids (wxID_OK=5100, wxID_CANCEL=5101, wxID_YES=5103) but are passed in so this header needs no wx.
// Returns true when the user cancelled.
inline bool sync_ams_confirm_cancelled(int result, int id_cancel) { return result == id_cancel; }

// Yes / Resync (wxID_YES) force filament_changed on every slot; Sync (wxID_OK) only the changed ones.
inline bool sync_ams_confirm_force_all(int result, int id_yes) { return result == id_yes; }

}} // namespace Slic3r::GUI
