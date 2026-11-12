#ifndef slic3r_SpoolmanSummary_hpp_
#define slic3r_SpoolmanSummary_hpp_

// Pure data model of the "Spoolman Sync Summary" dialog (S8, part 1). Turns the sync results (UpsertReport, SpoolmanTierReport, tray info,
// catalog state, replaced project copies) into cards, notices, header data and a toast that the later GUI step renders 1:1. No wx, no GUI
// headers, no PresetBundle, no network, no disk, no printer-specific code.
//
// Behaviour (each line is covered by tests/libslic3r/test_spoolman_summary.cpp):
//  - Slots: a tray is identified by its RAW filament_ams_list key (the key S6 reports; extruder + ams_id*4 + slot_id, 254/255+extruder for
//    the external spool) and shown by `display_slot` (a 1-based number or a label such as "Ext"). Joins with the S6 tier entries use the
//    raw key; cards are ordered by display slot (numbers first, numerically, then labels). A card with on_tray=false is a filament (or
//    tier entry) that matches no tray: it is never dropped.
//  - One card per tray with a Spoolman filament id or a tier entry. Filaments on no tray get a card only when there is something to tell.
//    A skipped filament is ALWAYS a card (skipped, skip_reason, the kept linked_presets).
//  - Rows: created (parent -> Spoolman) or updated (old -> new), grouped by source class (native, extra, ofd, other) keeping the order the
//    upsert gave (the mapping's own order: SpoolmanPresets sorts its changes that way); values and source strings are copied as given.
//  - Results are kept per preset NAME, not per Spoolman id: two results of one id keep their own preset/rule. If the S6 tier chose a preset
//    that is not the one the upsert refreshed, the card shows the tier's preset and a mismatch flag, `changes_preset` names the preset the
//    rows belong to.
//  - Created: card.created, parent_name and a `note` ("Created from X; the preset value column is X's value" / "Created, no values differ
//    from X").
//  - Rejected values go to `ignored` (key, value, reason, warning). The value is capped to kMaxValueBytes at a UTF-8 boundary with "…",
//    control characters shown as \xNN, original_length keeps the real size. Warning-only entries and tier mismatches are notes: kept
//    on their cards, counted in warning_count (only when the card is visible) and shown in the toast as "M notes", but they do NOT open
//    the dialog by themselves.
//  - Tier entries sharing a raw key all stay visible: the first (sorted) is the slot's, the others become cards flagged "Duplicate tier
//    entry".
//  - show_dialog: true for any created preset, updated preset with changes, non-warning rejection, skipped, ambiguous (upsert or tier),
//    superseded, orphaned, duplicate, needs_reselect, tier entry without a tray, stale catalog or replaced project copy. Otherwise false and
//    toast_text = "Spoolman: N filament(s) in sync, nothing changed" (", M notes" instead of "nothing changed" when M > 0). N counts distinct
//    ids. With catalog_size == 0 there is NO "in sync" toast (nothing to be in sync with).
//  - Catalog state: unreachable / bad_response, with has_previous_sync and an optional age; the notice text says which is true, whether
//    there are earlier values, and always the consequence (slots are matched by the printer's RFID type).
//  - Notices are ordered stale, project copy, info. Text is plain English from constants in SpoolmanSummary.cpp (translation hooks come
//    with the GUI step).
//
// VERIFIED: by the tests. ASSUMED / NOT shown: how the strings read in the real dialog (not built), translation, real sync data beyond
// the hand-built reports.

#include "SpoolmanPresets.hpp"
#include "SpoolmanTierReport.hpp"

#include <optional>
#include <string>
#include <vector>

namespace Slic3r { namespace Spoolman {

struct SummaryRow
{
    std::string key;
    std::string preset_value;
    std::string spoolman_value;
    std::string source;
};

struct SummaryIgnored
{
    std::string key;
    std::string value;           // capped (kMaxValueBytes), control characters shown as \xNN; empty when the report carries no value
    size_t      original_length = 0; // bytes of the value as received; > value.size() style truncation is flagged by `truncated`
    bool        truncated = false;
    std::string reason;
    bool        warning = false;
};

// Longest rejected value shown on a card, in bytes (cut at a UTF-8 code-point boundary, "…" appended). The report itself keeps it whole.
inline constexpr size_t kMaxValueBytes = 120;

struct SlotCard
{
    int                         raw_key = -1;   // filament_ams_list key (joins with SpoolmanTierReport::Entry::slot); -1 = none
    std::string                 display_slot;   // "1", "2", ... or a label such as "Ext"; empty when the card is on no tray
    bool                        on_tray = false;
    int                         spoolman_id = 0;
    std::string                 colour_hex;
    std::string                 spool_name;     // never blank
    std::string                 remaining_weight_text; // "612 g left", "weight unknown" or empty when no tray info
    std::optional<std::string>  preset_name;
    std::string                 rule_text;
    bool                        skipped = false;
    std::string                 skip_reason;
    std::vector<std::string>    linked_presets; // skipped: existing linked presets kept untouched
    bool                        created = false;
    std::string                 parent_name;
    std::string                 note;           // created-preset explanation, empty otherwise
    std::string                 changes_preset; // the preset the rows belong to
    std::vector<SummaryRow>     changes;
    std::vector<SummaryIgnored> ignored;
    std::vector<std::string>    flags;          // human text: ambiguous, superseded, duplicate, needs re-select, orphaned, mismatch
};

enum class NoticeKind { stale_spoolman, project_copy_replaced, info };

struct SummaryNotice
{
    NoticeKind              kind = NoticeKind::info;
    std::string             text;
    std::vector<SummaryRow> rows; // project_copy_replaced: the changed values (preset value = the project's old one)
};

struct SummaryModel
{
    std::vector<SlotCard>      slots;
    std::vector<SummaryNotice> notices;
    bool                       show_dialog = false;
    std::string                toast_text;
    // header / footer of the dialog
    size_t                     catalog_size = 0;
    std::string                fetched_text;   // "fetched just now", "last good fetch 2 days ago", ...
    std::string                read_only_note;
    size_t                     warning_count = 0; // notes visible on cards: warning-only rejections and tier mismatches
};

struct SummaryTray
{
    int                    key = -1;           // raw filament_ams_list key
    // "1".. or "Ext". The GUI step must pass DISTINCT labels: an empty one becomes "Slot <key>", and trays sharing a label get " 1", " 2"
    // suffixes in raw-key order here, as a last resort.
    std::string            display_slot;
    std::string            colour_hex;
    std::string            spool_name;
    std::optional<double>  remaining_weight_g;
    int                    spoolman_filament_id = 0; // 0 = not a Spoolman tray
};

// An older copy of a preset embedded in the project was replaced by the library preset (decision D2). Rows: project value -> library value.
struct ProjectCopyReplaced
{
    std::string             preset_name;
    std::vector<SummaryRow> changes;
};

enum class CatalogState { fresh, unreachable, bad_response };

struct SyncOutcome
{
    UpsertReport                     report;
    SpoolmanTierReport               tiers;
    std::vector<SummaryTray>         trays;
    CatalogState                     catalog_state = CatalogState::fresh;
    bool                             has_previous_sync = false;   // stale only: values from an earlier good fetch exist
    std::optional<long long>         last_good_age_seconds;       // stale only: age of that fetch
    std::optional<long long>         fetched_age_seconds;         // fresh: age of the fetch; 0..5 = "just now"; nullopt or negative = unknown time
    size_t                           catalog_size = 0;            // filaments in the catalog the sync used
    std::vector<ProjectCopyReplaced> project_copies;
};

SummaryModel build_summary(const SyncOutcome &outcome);

// Flat, GUI-neutral lines of the dialog (S8 part 2). The wx dialog renders these 1:1 as PLAIN text (SetLabelText, no markup), so the
// layout logic is unit-testable without a window. Every string is sanitized: control characters (newline, tab, ESC, ...) become a space,
// so an untrusted Spoolman name cannot add lines or fake a row.
struct SummaryLine
{
    enum class Kind {
        notice,       // cells: {text}
        notice_head,  // cells: Setting | Preset value | Spoolman value | Source (before a notice's rows)
        notice_row,   // cells: 4
        slot_header,  // cells: {slot label, spool name, remaining weight text, colour hex}
        slot_preset,  // cells: {text}: chosen preset and the matching rule
        slot_note,    // cells: {text}: created note, skip reason, kept presets
        slot_flag,    // cells: {text}
        table_head,   // cells: Setting | Preset value | Spoolman value | Source
        table_row,    // cells: 4
        ignored,      // cells: {"Ignored key = value (reason)"}
        footer        // cells: {text}: fetched_text, read-only note
    };
    Kind                     kind = Kind::notice;
    std::vector<std::string> cells;
};

// Replaces every control character (< 0x20, 0x7F) with a space; other bytes (UTF-8) are kept.
std::string sanitize_for_display(const std::string &s);

std::vector<SummaryLine> summary_lines(const SummaryModel &model);

}} // namespace Slic3r::Spoolman

#endif
