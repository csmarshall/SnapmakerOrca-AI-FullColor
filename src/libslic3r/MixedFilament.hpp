#pragma once
#ifndef slic3r_MixedFilament_hpp_
#define slic3r_MixedFilament_hpp_

// Data model and row serialization for Snapmaker's "mixed_filament_definitions" project string.
//
// Slice 2 of the mixed-engine port (docs/records/mixed-engine-port-slices.md in the plan repo): the
// MixedFilament row struct and its (de)serialization, forklifted from Snapmaker Orca's
// src/libslic3r/MixedFilament.{hpp,cpp} (struct MixedFilament, MixedFilamentManager::serialize_custom_entries,
// encode/decode/normalize_gradient_component_ids, normalize_gradient_component_weights,
// normalize_manual_pattern, format_surface_offset_token). Field names, token order and defaults are
// copied verbatim (ADR 0004). wx-free; no manager, no resolution, no slicing hooks: nothing in the
// engine reads this yet. Forklifted code stays under Snapmaker's licence.
//
// Differences from Snapmaker, all deliberate:
//  - free functions instead of MixedFilamentManager statics (there is no manager here);
//  - no process-global auto-generate flag: nothing in this file needs it;
//  - serialization is const: stable ids of 0 are assigned on a copy, in row order, the way a fresh
//    Snapmaker manager would (first free id is 1, later rows continue above the highest id seen).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {

struct LegacyMixedRow;

struct MixedFilament
{
    enum DistributionMode : uint8_t {
        LayerCycle = 0,
        SameLayerPointillisme = 1,
        Simple = 2
    };

    // 1-based physical filament IDs that are combined.
    unsigned int component_a = 1;
    unsigned int component_b = 2;

    // Persistent row identity used to keep painted virtual-tool assignments stable.
    uint64_t stable_id = 0;

    // Layer-alternation ratio (runtime only, not persisted).
    int ratio_a = 1;
    int ratio_b = 1;

    // Blend percentage of component B in [0..100].
    int mix_b_percent = 50;

    // Optional manual pattern. Legacy: each '1'-'9' char is a token; '[NN]' for ids 10..99; comma
    // separates per-perimeter groups.
    std::string manual_pattern;

    // Optional explicit gradient component list ("123" for ids <= 9, else "1/12/3") and weights ("50/25/25").
    std::string gradient_component_ids;
    std::string gradient_component_weights;

    // Legacy compatibility flag from earlier prototype serialization (forced off on write).
    bool pointillism_all_filaments = false;

    int distribution_mode = int(Simple);

    // Optional Local-Z cap for this mixed row. 0 disables the cap.
    int local_z_max_sublayers = 0;

    static constexpr float k_default_gradient_dominant = 0.8f;
    static constexpr float k_default_gradient_minority = 0.2f;
    static constexpr float k_min_gradient_difference   = 0.05f;

    bool  gradient_enabled = false;
    float gradient_start = k_default_gradient_dominant;
    float gradient_end   = k_default_gradient_minority;

    // XY surface offsets in mm when the row resolves to component A or B for a whole layer.
    float component_a_surface_offset = 0.f;
    float component_b_surface_offset = 0.f;

    bool enabled = true;
    bool deleted = false;
    bool custom = false;
    bool origin_auto = false;

    // UI mode that created this row (-1=unknown/legacy, 0=RATIO, 1=CYCLE, 2=MATCH, 3=GRADIENT).
    int ui_mode = -1;

    // Computed display colour as "#RRGGBB"; not persisted.
    std::string display_color;

    bool operator==(const MixedFilament &rhs) const;
    bool operator!=(const MixedFilament &rhs) const { return !(*this == rhs); }
};

// Encode 1-based filament ids to a compact string: concatenated digits when all ids <= 9, else
// '/'-separated decimals ("1/12/3"); a single id > 9 gets a leading '/' ("/12").
std::string encode_gradient_component_ids(const std::vector<unsigned int> &ids);
// Decode either format. When num_physical > 0 each id must be <= num_physical. Duplicates dropped.
std::vector<unsigned int> decode_gradient_component_ids(const std::string &components, size_t num_physical = 0);
// Decode (ids up to kMaxMixedPhysicalFilaments) and re-encode to canonical form.
std::string normalize_gradient_component_ids(const std::string &components);
// Weights to integer percentages summing to 100, "a/b/c"; empty when the count does not match.
std::string normalize_gradient_component_weights(const std::string &weights, size_t expected_components);
// Longest manual pattern (normalised form) the engine accepts. Snapmaker has no limit; a pattern from an
// untrusted project file can be hundreds of KB and ordered_perimeter_extruders is quadratic in its groups.
// Longer patterns are rejected (normalize_manual_pattern returns empty) and ignored by the engine.
constexpr size_t kMaxManualPatternChars = 256;
// Canonical manual pattern (digits 1-9, "[NN]" for 10..99, "," group separator); empty if invalid or longer
// than kMaxManualPatternChars (deliberate deviation from Snapmaker).
std::string normalize_manual_pattern(const std::string &pattern);
// Surface offset as Snapmaker writes it: clamped to +-2, 4 decimals, trailing zeros trimmed, "-0" -> "0".
std::string format_surface_offset_token(float value);

constexpr size_t kMaxMixedPhysicalFilaments = 64;

// Digits-only token (1 to 5 digits) to [0, 99999]; false for anything else. The one range-checked integer
// parser for tokens read from untrusted project files (no atoi/strtoul on file data).
bool parse_bounded_mixed_token(const std::string &token, int &out);
// Snapmaker decode_gradient_component_weights: the normalised percentages (sum 100), empty when the count
// does not match expected_components or the weights are unusable.
// Deliberate deviations from Snapmaker (strtoul/atoi there): a weight token of more than 5 digits makes the
// whole list invalid (empty) instead of wrapping or saturating; each weight is clamped to 10000 before
// normalisation; more than 256 weights is invalid. Weights of 10000 or less behave exactly as in Snapmaker.
std::vector<int> decode_gradient_component_weights(const std::string &weights, size_t expected_components); // Snapmaker MixedFilamentManager::kMaxPhysicalFilaments

// The row string Snapmaker writes into mixed_filament_definitions: rows joined by ';'. Mirrors
// serialize_custom_entries (pointillism forced off, ids/weights/pattern normalised, stable ids filled).
std::string serialize_mixed_filaments(const std::vector<MixedFilament> &rows);

// Converts a row of the slice-1 parser into the data model. Display colour and ratios are not
// persisted and stay at their defaults.
MixedFilament mixed_filament_from_legacy_row(const LegacyMixedRow &row);

// Parses a mixed_filament_definitions string with the slice-1 parser (one parser, not two) and returns
// the rows in Snapmaker's virtual-filament order, disabled and deleted ones included. Disabled rows are
// kept here so a round trip is lossless; dropping them is the importer's job (convert_legacy_mixed_filaments).
std::vector<MixedFilament> parse_mixed_filaments(const std::string &serialized, size_t num_physical);

} // namespace Slic3r

#endif
