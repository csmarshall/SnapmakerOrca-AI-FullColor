#ifndef slic3r_SpoolmanPresets_hpp_
#define slic3r_SpoolmanPresets_hpp_

// Catalog upsert: one USER filament preset per Spoolman filament (per parent), created or refreshed idempotently inside a PresetCollection.
// No network, no GUI, no wx, no printer-specific code, never reads an app preference (the caller gates the call), never touches disk
// (the caller saves; a new preset's `file` is set to the collection's path for its name so Preset::save() works).
//
// Behaviour (each line is covered by tests/libslic3r/test_spoolman_presets.cpp unless marked ASSUMED):
//  - Parent: choose_parent (SpoolmanParent) over the collection's SYSTEM presets that are compatible with the target printer
//    (build_parent_candidates). No parent => the filament is `skipped` with the reason and nothing is created.
//  - Name: "<Vendor> <Spoolman name> (Spoolman) @<parent's suffix>" (kSpoolmanTag; the vendor is not repeated when the name starts with it;
//    characters that are illegal in file names become '-'; no '@' in the parent's name => no suffix). A name held by anything that is not
//    THE linked preset of this filament (a user's own preset, a system preset, a preset linked to another id) gets " #2", " #3" ... before
//    the suffix; it is never overwritten.
//  - The new preset inherits the parent (full copy of the parent's config, inherits = parent name, parent's filament_id and compatibility,
//    setting_id cleared like Save-as), then map_filament's overrides (SpoolmanMapping) are applied, then the link (SpoolmanLink) with the
//    per-key sources. Nothing else changes. A refresh touches ONLY the override keys, the keys of the old link's sources that are no longer
//    provided (reset to the parent's current value) and the link; other edits on the preset are left alone.
//  - Matching an existing preset to a filament: a USER preset (never system, default, external, project-embedded or bundle) whose link
//    parses and has the filament's id and that inherits the chosen parent (a same-named preset under another parent is NOT matched: it is
//    `superseded`; a Spoolman rename keeps
//    the preset name and link, so the name need not match). Unlinked presets and presets with an unparsable link are never touched.
//    ALL name comparisons are ASCII case-insensitive (two names that differ only in case would be one file on macOS/Windows).
//  - Several matches (Save-as and clone_presets copy the link): if exactly one is named like the generated name it is refreshed and the
//    others are `duplicates`, untouched; otherwise the filament is `ambiguous` (candidates listed) and NOTHING is modified or created. A
//    single match is refreshed whatever its name (a lone copy whose original was deleted cannot be told from a renamed original).
//  - A linked preset of the same filament under another parent is reported in `superseded` (kept, untouched).
//  - Generated names are capped so "<name>.json" stays under 200 bytes (the Spoolman-name part is truncated at a UTF-8 boundary, the tag and
//    suffix stay), leading/trailing dots and spaces, control characters and 0x7F are removed, an empty name becomes "Filament <id>".
//  - filament_settings_id is set to the preset's own name on create and rewritten on every refresh; when it is the only difference the
//    write happens but the preset is reported `unchanged`.
//  - If an override cannot be applied while the old link lists its key, the key stays in the link (fail closed) and is reported rejected.
//  - The edited copy of a refreshed preset is not refreshed: a refreshed preset that is the selected/edited one is listed in needs_reselect.
//  - String options (e.g. filament_vendor) are set directly, never through ConfigOptionStrings::deserialize (it splits on ';' and unquotes).
//  - Names are cased/ASCII-folded only (non-ASCII case pairs are not unified); a Spoolman rename stalls while two candidates match; the
//    generated-name match prefers a byte-exact name, then a case-insensitive one.
//  - Vector options: the single Spoolman value is written to EVERY element (S2 mapping gives one single-element value per key).
//  - Printer limits (PrinterLimits in the context, supplied by the caller): a nozzle / bed temperature override above the limit is NOT
//    applied, is reported in `rejected` ("exceeds printer limit") and the rest of the preset still applies. A refresh then resets that key to
//    the parent's value. Limits apply per GROUP: nozzle_temperature + nozzle_temperature_initial_layer both pass or both are rejected, and
//    for each plate its normal + initial-layer key likewise; non-numeric text is rejected. A parent value that already exceeds a limit is
//    kept and reported as a warning in `rejected`. v2.5.0 has NO printer-level maximum-temperature option (checked: Preset::printer_options and print_config_def), so
//    the context struct is the only source; default = no check.
//  - Lifecycle: a linked user preset whose filament id is not in the catalog is `orphaned` (kept, never refreshed, reported on every run).
//    An EMPTY catalog never orphans anything (the caller must not pass a failed fetch at all). Presets are never deleted. A preset the user
//    deleted simply returns as `created`.
//  - If the link would exceed kMaxLinkBytes the filament is `skipped` ("link too large") and nothing changes.
//  - The caller must refresh any edited/selected copy of a changed preset (PresetCollection keeps a separate edited preset); not done here.
//
// VERIFIED: by the tests. ASSUMED, not verified: that candidate parents need not be is_visible (an uninstalled system preset can be a parent);
// that sanitising file-name characters is enough for every file system; real-profile behaviour beyond the in-memory presets built from the
// candidates TSV (no real bundle loaded); that overriding every element of a multi-element vector option with the single value is right.

#include "SpoolmanCatalog.hpp"
#include "SpoolmanMapping.hpp"
#include "SpoolmanParent.hpp"

#include <optional>
#include <string>
#include <vector>

namespace Slic3r {
class Preset;
class PresetCollection;
namespace Spoolman {

// The literal tag in a generated preset name.
inline constexpr const char *kSpoolmanTag = "(Spoolman)";

struct PrinterLimits
{
    std::optional<double> max_nozzle_temp; // degrees C; nullopt = no check
    std::optional<double> max_bed_temp;
};

struct UpsertContext
{
    // The target printer preset (for compatibility filtering of parents). nullptr = every system preset is a candidate.
    const Preset *printer = nullptr;
    // ParentOptions::preferred_token, e.g. "@U1 0.4 nozzle".
    std::string   preferred_token;
    PrinterLimits limits;
};

struct ValueChange
{
    std::string key;
    std::string old_value;
    std::string new_value;
    std::string source; // "native:..." | "extra:..." | "ofd:..." | "parent" (reset to the parent's value)
};

struct PresetResult
{
    int                      spoolman_id = 0;
    std::string              preset_name;
    std::string              parent_name;
    std::string              parent_rule; // which choose_parent rule fired
    std::vector<ValueChange> changes;     // created: parent's value -> Spoolman's; updated: old -> new
};

struct SkippedFilament
{
    int         spoolman_id = 0;
    std::string filament_name;
    std::string reason;
    // Existing linked user presets of this filament (any parent), kept untouched: they are neither refreshed nor orphaned while skipped.
    std::vector<std::string> linked_presets;
};

// More than one preset matches the filament and none is named exactly like the generated name: nothing is modified and nothing created.
struct AmbiguousFilament
{
    int                      spoolman_id = 0;
    std::string              filament_name;
    std::vector<std::string> candidates;
};

// A linked preset of this filament under a DIFFERENT parent than the one chosen now (material or printer changed): kept untouched.
struct SupersededPreset
{
    int         spoolman_id = 0;
    std::string preset_name;
    std::string old_parent;
    std::string new_parent;
};

struct OrphanedPreset
{
    int         spoolman_id = 0;
    std::string preset_name;
};

struct DuplicatePreset
{
    int         spoolman_id = 0;
    std::string preset_name;      // untouched
    std::string refreshed_preset; // the preset that was refreshed instead ("" when the filament was skipped)
};

struct RejectedValue
{
    int         spoolman_id = 0;
    std::string preset_name;
    std::string key_or_field;
    std::string reason;
    bool        warning = false;
};

struct UpsertReport
{
    std::vector<PresetResult>    created;
    std::vector<PresetResult>    updated;
    std::vector<PresetResult>    unchanged;
    std::vector<SkippedFilament> skipped;
    std::vector<OrphanedPreset>  orphaned;
    std::vector<DuplicatePreset> duplicates;
    std::vector<RejectedValue>   rejected;
    std::vector<AmbiguousFilament> ambiguous;
    std::vector<SupersededPreset>  superseded;
    // Refreshed presets that are the SELECTED (or edited) preset of the collection: the edited copy is NOT refreshed here, the caller must
    // re-select them. Refreshing any other preset leaves the selection and the edited preset untouched.
    std::vector<std::string>       needs_reselect;
};

// System presets of `filaments` compatible with `printer` (nullptr = all), as parent candidates. Default presets are never candidates.
std::vector<ParentCandidate> build_parent_candidates(const PresetCollection &filaments, const Preset *printer);

// Creates or refreshes the Spoolman presets in `filaments`. `catalog` empty => nothing is created or orphaned.
UpsertReport upsert_catalog(PresetCollection &filaments, const std::vector<Filament> &catalog, const UpsertContext &context);

}} // namespace Slic3r::Spoolman

#endif
