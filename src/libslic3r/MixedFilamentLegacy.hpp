#ifndef slic3r_MixedFilamentLegacy_hpp_
#define slic3r_MixedFilamentLegacy_hpp_

// Import-only reader for the Snapmaker Orca "mixed_filament_definitions" project string.
//
// Snapmaker keeps mixed filaments as rows of one config string and numbers them as "virtual
// filaments" after the physical ones; this build keeps them as ordinary filament slots flagged by
// filament_is_mixed. This file parses the Snapmaker rows (forklifted from Snapmaker's
// MixedFilamentManager::parse_row_definition / load_custom_entries, without the manager or any
// wx/GUI dependency) and appends one 2.5 mixed slot per enabled row, in row order, so the ids line
// up and object/volume/paint filament ids need no remap. See
// docs/records/fullspectrum-data-model-slice1.md in the plan repo.
//
// One-way: nothing here writes the Snapmaker string back.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {

class DynamicPrintConfig;

// One parsed row of the Snapmaker string. Field meanings follow Snapmaker's struct MixedFilament.
// Limits: the 3MF is untrusted input. Total slots (physical + converted) never exceed this.
constexpr size_t kLegacyMixedMaxFilaments = 32;   // = the paint cap (EnforcerBlockerType::ExtruderMax); physical count above this: nothing is converted
constexpr size_t kLegacyMixedMaxRows      = 1024; // row strings read from the definitions; the rest are skipped

struct LegacyMixedRow
{
    unsigned int a = 0;                 // 1-based physical filament
    unsigned int b = 0;
    uint64_t     stable_id = 0;
    bool         enabled = true;
    bool         custom = true;         // false = auto-generated pair row
    bool         origin_auto = false;
    int          mix_b_percent = 50;    // share of component b, 0..100
    bool         pointillism = false;   // legacy flag token, or distribution mode 1, as read
    std::string  gradient_component_ids;
    std::string  gradient_component_weights;
    std::string  manual_pattern;        // normalised; empty when absent or invalid
    int          distribution_mode = 2; // 0 layer cycle, 1 same-layer pointillism, 2 simple
    int          local_z_max_sublayers = 0;
    float        surface_offset_a = 0.f;
    float        surface_offset_b = 0.f;
    bool         deleted = false;
    bool         gradient_enabled = false;
    float        gradient_start = 0.8f; // ratio range of component a
    float        gradient_end = 0.2f;
    int          ui_mode = -1;
};

// Parses one row exactly as Snapmaker's loader does (token order, legacy 4-token form, the
// g/w/m/z/xa/xb/d/o/u/cm/r1 tokens, a trailing manual pattern). Returns false for a malformed row.
// Range checks against the physical count are NOT done here (see parse_legacy_mixed_definitions).
bool parse_legacy_mixed_row(const std::string &row, LegacyMixedRow &out);

struct LegacyMixedParse
{
    // The rows in Snapmaker's virtual-filament order, disabled and deleted ones included: the
    // virtual id of a row is num_physical + 1 + (the number of earlier rows that are enabled and
    // not deleted).
    std::vector<LegacyMixedRow> rows;
    size_t                      parsed = 0;   // non-empty row strings seen
    size_t                      skipped = 0;  // malformed, out-of-range, duplicate or unmatchable rows
    // True when the string holds auto-generated pair rows, i.e. it was written with Snapmaker's
    // "auto generate" preference on; the C(N,2) pairs the string lacks are then appended, as the loader does.
    bool                        auto_generated = false;
};

// Mirrors Snapmaker's load_custom_entries for the given physical filament count. The auto-generate
// preference lives in Snapmaker's app config, not in the file; it is inferred from the string
// (auto rows present means on), because with it off Snapmaker's loader drops every auto row.
LegacyMixedParse parse_legacy_mixed_definitions(const std::string &serialized, size_t num_physical);

struct LegacyMixedConversion
{
    size_t                   slots_added = 0;
    size_t                   rows_skipped = 0;
    std::vector<std::string> warnings;       // for the user; rows converted lossily, and anything left unconverted
};

// Appends one mixed slot per enabled, non-deleted row of `serialized` to the per-filament vectors of
// a loaded project config. Does nothing when the string is empty, when fewer than two physical slots
// exist, or when the config already holds 2.5 mixed slots (it wins; that is reported as a warning).
LegacyMixedConversion convert_legacy_mixed_filaments(DynamicPrintConfig &config, const std::string &serialized);

} // namespace Slic3r

#endif
