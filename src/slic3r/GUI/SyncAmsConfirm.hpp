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

// #131: the Spoolman button is an explicit "refresh everything from Spoolman", so it never shows the Sync / Resync question: it
// behaves as Resync (all slots forced). The Spoolman summary's "run again as Sync" button re-runs it with changes_only_rerun = true
// (only the changed slots). The default sync button (from_spoolman == false) keeps the confirm.
struct SyncAmsConfirmPlan
{
    bool show_confirm; // false: do not ask, use `force_all`
    bool force_all;    // only meaningful when show_confirm is false
};

inline SyncAmsConfirmPlan sync_ams_confirm_plan(bool from_spoolman, bool changes_only_rerun)
{
    if (!from_spoolman)
        return {true, false};
    return {false, !changes_only_rerun};
}

// #131: the remembered answer to "Re-read spool tags on the printer first?", stored in app_config key spoolman_reread_answer.
// "yes" / "no" are remembered answers; anything else (empty, unknown) means ask.
enum class RereadAnswer { Ask, Yes, No };

inline RereadAnswer spoolman_reread_stored_answer(const std::string &stored)
{
    return stored == "yes" ? RereadAnswer::Yes : stored == "no" ? RereadAnswer::No : RereadAnswer::Ask;
}

// Stored value to write after the user answered: only when "don't ask again" was ticked; otherwise "" (keep asking).
inline std::string spoolman_reread_value_to_store(bool answered_yes, bool dont_ask_again)
{
    return dont_ask_again ? (answered_yes ? "yes" : "no") : "";
}

}} // namespace Slic3r::GUI
