#pragma once
#ifndef slic3r_MixedFilamentEngine_hpp_
#define slic3r_MixedFilamentEngine_hpp_

// Resolution half of Snapmaker's MixedFilamentManager (slice 3 of the mixed-engine port, see
// docs/records/mixed-engine-port-slices.md in the plan repo): virtual filament id <-> row mapping, per-layer
// resolution of a virtual id to a physical extruder (manual pattern, weighted gradient, height-weighted
// cadence, ratio cycle, advanced dithering), painted-region id, surface offset, perimeter order, and the
// ratio arithmetic. Forklifted from Snapmaker Orca's src/libslic3r/MixedFilament.cpp (resolve,
// resolve_perimeter, effective_painted_region_filament_id, component_surface_offset,
// ordered_perimeter_extruders, mixed_index_from_filament_id, expand_virtual_extruder_ids, apply_gradient_settings,
// compute_gradient_ratios/heights, normalize_ratio_pair, build_weighted_gradient_sequence, the pattern token
// helpers). Ids, constants and arithmetic are copied verbatim (ADR 0004); forklifted code stays under
// Snapmaker's licence. wx-free, no Print/PrintObject, no slicing hooks, no process-global state.
//
// Deliberate differences from Snapmaker:
//  - the unused `const PrintObject *current_object` parameter is dropped;
//  - no static auto-generate flag (auto_generate_enabled is passed as an argument by whoever builds rows);
//  - every integer read from a pattern/weight token goes through parse_bounded_mixed_token (no strtoul/atoi);
//  - cadence arithmetic is done in 64 bit and gradient height bounds are clamped to a sane finite range, so a
//    hostile or NaN value from a project file cannot overflow or reach lround() undefined behaviour. For
//    in-range inputs the results are identical to Snapmaker's.
//
// Slot-backed view: this build keeps mixed filaments as ordinary tail slots (filament_is_mixed). The engine
// is built over the rows of those slots with from_slots(); slot id == num_physical + 1 + k, the same numbering
// as Snapmaker's virtual ids for the enabled subset, so the arithmetic is unchanged. Native 2.5 slots (no
// engine fields) are not engine rows (is_engine_row) and never reach this code.

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "MixedFilament.hpp"

namespace Slic3r {

// Largest total slot count the paint encoding can address (Extruder32, TriangleSelector.hpp).
constexpr size_t kMixedEngineMaxSlots = 32;

// True when the row carries any field the native 2.5 mixed model cannot express. Rows for which this is
// false are resolved by ToolOrdering::resolve_mixed_filaments exactly as before.
bool is_engine_row(const MixedFilament &row);

// Weighted, evenly interleaved extruder sequence for a gradient row (cycle capped at 48).
std::vector<unsigned int> build_weighted_gradient_sequence(const std::vector<unsigned int> &ids,
                                                           const std::vector<int>          &weights);

struct MixedEngineSlotsReport
{
    bool                     valid = false;   // false: the engine is empty and must not be used
    size_t                   num_physical = 0;
    size_t                   num_mixed = 0;
    std::vector<std::string> warnings;        // for the user
};

class MixedFilamentEngine
{
public:
    MixedFilamentEngine() = default;
    // Engine over rows as Snapmaker holds them (disabled / deleted rows allowed and skipped by the numbering).
    explicit MixedFilamentEngine(std::vector<MixedFilament> rows) : m_mixed(std::move(rows)) {}

    // Slot-backed adapter. `slot_rows` holds one row per mixed tail slot, in slot order; `total_slots` is the
    // filament count of the config (physical + mixed). Disabled/deleted flags on a slot row are ignored with a
    // warning (a slot that exists is a slot: dropping rows is the importer's job). Invalid when more rows than
    // slots, no physical slot is left, or a row names a component outside 1..num_physical. More than
    // kMixedEngineMaxSlots total slots is a warning, not an error (Extruder32 cap, painting above it is lossy).
    static MixedFilamentEngine from_slots(const std::vector<MixedFilament> &slot_rows, size_t total_slots,
                                          MixedEngineSlotsReport *report = nullptr);

    // ---- Ratio arithmetic ----------------------------------------------------------------------------
    // gradient_mode: 0 layer-cycle weighted, 1 height weighted. Recomputes ratio_a/ratio_b of custom rows.
    void apply_gradient_settings(int gradient_mode, float lower_bound, float upper_bound, bool advanced_dithering = false);

    static int         safe_mod(int x, int m);
    static void        normalize_ratio_pair(int &a, int &b);
    static float       canonical_signed_bias_value(float component_a_surface_offset, float component_b_surface_offset);

    // ---- Pattern helpers -----------------------------------------------------------------------------
    static int                      mix_percent_from_manual_pattern(const std::string &pattern);
    static std::vector<std::string> split_pattern_group_to_tokens(const std::string &group, size_t num_physical);
    static unsigned int             physical_filament_from_token(const std::string &token, const MixedFilament &mf, size_t num_physical);
    static std::vector<std::string> split_pattern_groups(const std::string &pattern);

    // ---- Id mapping ------------------------------------------------------------------------------------
    bool is_mixed(unsigned int filament_id, size_t num_physical) const { return mixed_index_from_filament_id(filament_id, num_physical) >= 0; }
    // Virtual ids enumerate enabled, non-deleted rows only. -1 for a physical id or an unknown id.
    int  mixed_index_from_filament_id(unsigned int filament_id, size_t num_physical) const;
    const MixedFilament *mixed_filament_from_id(unsigned int filament_id, size_t num_physical) const;
    // Inverse of the above: 1-based virtual id of row `row_index`, 0 when that row is disabled/deleted or out of range.
    unsigned int filament_id_from_mixed_index(size_t row_index, size_t num_physical) const;
    void expand_virtual_extruder_ids(std::vector<int> &ids, size_t num_physical) const;

    // ---- Resolution --------------------------------------------------------------------------------------
    unsigned int resolve(unsigned int filament_id, size_t num_physical, int layer_index, float layer_print_z = 0.f,
                         float layer_height = 0.f, bool force_height_weighted = false) const;
    unsigned int resolve_perimeter(unsigned int filament_id, size_t num_physical, int layer_index, int perimeter_index,
                                   float layer_print_z = 0.f, float layer_height = 0.f, bool force_height_weighted = false) const;
    unsigned int effective_painted_region_filament_id(unsigned int filament_id, size_t num_physical, int layer_index,
                                                      float layer_print_z = 0.f, float layer_height = 0.f,
                                                      float layer_height_a = 0.f, float layer_height_b = 0.f,
                                                      float base_layer_height = 0.2f) const;
    float component_surface_offset(unsigned int filament_id, size_t num_physical, int layer_index, float layer_print_z = 0.f,
                                   float layer_height = 0.f, bool force_height_weighted = false) const;
    std::vector<unsigned int> ordered_perimeter_extruders(unsigned int filament_id, size_t num_physical, int layer_index,
                                                          float layer_print_z = 0.f, float layer_height = 0.f,
                                                          bool force_height_weighted = false) const;

    // ---- Accessors ---------------------------------------------------------------------------------------
    const std::vector<MixedFilament> &mixed_filaments() const { return m_mixed; }
    std::vector<MixedFilament>       &mixed_filaments()       { return m_mixed; }
    size_t enabled_count() const;
    size_t total_filaments(size_t num_physical) const { return num_physical + enabled_count(); }

private:
    std::vector<MixedFilament> m_mixed;
    int                        m_gradient_mode      = 0;
    float                      m_height_lower_bound = 0.04f;
    float                      m_height_upper_bound = 0.16f;
    bool                       m_advanced_dithering = false;
};

} // namespace Slic3r

#endif
