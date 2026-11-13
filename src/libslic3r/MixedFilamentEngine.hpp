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
//    in-range inputs the results are identical to Snapmaker's;
//  - gradient weights: a token of more than 5 digits invalidates the whole list, each weight is clamped to
//    10000, more than 256 weights is invalid (MixedFilament.hpp); weights up to 10000 match Snapmaker exactly;
//  - a manual pattern longer than kMaxManualPatternChars (256) is ignored, so ordered_perimeter_extruders
//    cannot be driven quadratic by a huge pattern;
//  - the painted-region cadence divides in float, as Snapmaker does (a double division flips .5 ties).
//
// Slot-backed view: this build keeps mixed filaments as ordinary tail slots (filament_is_mixed). The engine
// is built over the rows of those slots with from_slots(); slot id == num_physical + 1 + k, the same numbering
// as Snapmaker's virtual ids for the enabled subset, so the arithmetic is unchanged. Native 2.5 slots (empty
// filament_mixed_engine_row marker) are not engine rows (MixedFilamentEngine::is_engine_row) and are resolved by
// ToolOrdering::resolve_mixed_filaments exactly as before.

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "MixedFilament.hpp"

namespace Slic3r {

class ConfigBase;

// Largest total slot count the paint encoding can address (Extruder32, TriangleSelector.hpp).
constexpr size_t kMixedEngineMaxSlots = 32;

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

    // Slot-backed adapter. CONTRACT: `slot_rows` holds exactly one row per mixed tail slot of the config, in slot
    // order, INCLUDING native 2.5 mixed slots (give those a default-constructed row with the right components):
    // dropping any shifts every later virtual id, because the engine numbers rows, not slots. `total_slots` is
    // the filament count of the config (physical + mixed); num_physical = total_slots - slot_rows.size(), also
    // available afterwards as num_physical() and in the report. Disabled/deleted flags on a slot row are ignored
    // with a warning (a slot that exists is a slot: dropping rows is the importer's job). Invalid when more rows
    // than slots, no physical slot is left, or a row names a component outside 1..num_physical. More than
    // kMixedEngineMaxSlots total slots is a warning, not an error (Extruder32 cap, painting above it is lossy).
    // A manual pattern longer than kMaxManualPatternChars is ignored, with a warning.
    // The view has no ratios of its own: rows keep ratio_a = ratio_b = 1 (strict A,B alternation) until
    // apply_gradient_settings() is called, which derives them from mix_b_percent.
    static MixedFilamentEngine from_slots(const std::vector<MixedFilament> &slot_rows, size_t total_slots,
                                          MixedEngineSlotsReport *report = nullptr);

    // Physical filament count when built by from_slots(); 0 (unknown) otherwise. The resolve calls still take
    // num_physical explicitly, as in Snapmaker.
    size_t num_physical() const { return m_num_physical; }

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
    // Slice 4c routing: true when the row at `row_index` (slot order) came from a non-empty
    // filament_mixed_engine_row marker. The marker is the whole rule (no field heuristic): a plain Snapmaker ratio
    // row is an engine row too. Set by mixed_engine_from_config; false for every row of an engine built any other way.
    bool is_engine_row(size_t row_index) const { return row_index < m_engine_rows.size() && m_engine_rows[row_index]; }
    void set_engine_rows(std::vector<bool> engine_rows) { m_engine_rows = std::move(engine_rows); }
    size_t enabled_count() const;
    size_t total_filaments(size_t num_physical) const { return num_physical + enabled_count(); }

private:
    std::vector<MixedFilament> m_mixed;
    int                        m_gradient_mode      = 0;
    float                      m_height_lower_bound = 0.04f;
    float                      m_height_upper_bound = 0.16f;
    bool                       m_advanced_dithering = false;
    size_t                     m_num_physical       = 0;
    std::vector<bool>          m_engine_rows;
};

// ---- Slice 4b: config -> engine -----------------------------------------------------------------------

// The 11 global mixed_* keys after range validation (PrintConfig.cpp bounds, plus a finite upper cap so a
// hostile project file cannot feed absurd values to the arithmetic). Out-of-range values are clamped, NaN/inf
// fall back to the key's default; each correction adds a warning.
struct MixedEngineGlobals
{
    float color_layer_height_a   = 0.f;
    float color_layer_height_b   = 0.f;
    bool  gradient_mode          = false;
    float height_lower_bound     = 0.04f;
    float height_upper_bound     = 0.16f;
    bool  advanced_dithering     = false;
    float pointillism_pixel_size = 0.f;
    float pointillism_line_gap   = 0.f;
    bool  component_bias_enabled = false;
    float surface_indentation    = 0.f;
    bool  region_collapse        = true;
};
MixedEngineGlobals mixed_engine_globals_from_config(const ConfigBase &config, std::vector<std::string> *warnings = nullptr);

// Slice 4c: Snapmaker's resolve_mixed_with_layer_heights (S:GCode/ToolOrdering.cpp:32-70) over the engine.
// `filament_id` is the 1-based slot id; a non-mixed id is returned unchanged. A non-custom row with
// mixed_color_layer_height_a/b set and no active gradient is resolved by the A/B height cadence of the globals,
// everything else by engine.resolve(). Snapmaker's "current_object != nullptr" gate of the gradient test is always
// true on its ToolOrdering call sites and is therefore folded in. layer_index is the OBJECT-LOCAL layer index.
unsigned int mixed_engine_resolve_layer(const MixedFilamentEngine &engine, const MixedEngineGlobals &globals, unsigned int filament_id,
                                        size_t num_physical, int layer_index, float layer_print_z, float layer_height,
                                        float base_layer_height);

// Builds the engine from the per-slot filament_mixed_engine_row strings (Snapmaker's single-row token format,
// parsed by the bounded parser) and the global keys. Works on DynamicPrintConfig and PrintConfig alike.
// num_physical = filament count - mixed slot count (never the vector size). The engine has one row per mixed
// slot in slot order; native 2.5 slots (empty marker) get a placeholder row so ids stay aligned, and are not
// engine rows. Returns an EMPTY engine (report.valid == false) when every marker is empty (native project, no
// warning), and also, with a warning, when anything is inconsistent: a malformed row, a marker on a physical
// slot, mixed slots that are not a contiguous tail, more markers than slots, or from_slots() rejecting the rows.
// Never throws on bad input.
MixedFilamentEngine mixed_engine_from_config(const ConfigBase &config, MixedEngineSlotsReport *report = nullptr);

// Fixes engine-row strings after a PHYSICAL filament is deleted, following Snapmaker's remove_physical_filament
// (check order and adjust rules): a manual pattern naming the deleted id (tokens "1"/"2" are the symbolic A/B), or,
// without a pattern, a gradient id list or a pair naming it, makes Snapmaker DROP the row. We keep the slot (dropping
// it would renumber every later slot and paint id): its marker is cleared so it becomes a native 2.5 mix, and if
// `components` is given its string becomes the surviving ids (shifted down; padded with a 0 sentinel to two entries),
// with a user-facing line pushed to `warnings`. Every other row keeps its meaning: with a pattern the pair is left
// alone and the gradient ids lose the deleted one; without a pattern the pair and gradient ids shift down.
// Rows that are empty, on non-mixed slots, or unparseable are left alone.
void remap_mixed_engine_rows_on_delete(const std::vector<unsigned char> &is_mixed, std::vector<std::string> &rows,
                                       unsigned int del_1based, std::vector<std::string> *components = nullptr,
                                       std::vector<std::string> *warnings = nullptr);

// Clears the engine marker of one slot (the slot becomes a native 2.5 mix). Called when the user edits the slot's
// mix in the UI or a published native mix is applied onto it: the old Snapmaker row no longer describes the slot.
// Returns true if a non-empty marker was cleared. Out-of-range slots and a missing key are no-ops.
bool clear_mixed_engine_marker(ConfigBase &config, size_t slot);

// Slots (0-based) whose engine marker is in a state that disables the engine: empty when every marker is empty
// (native project) or the engine builds cleanly; otherwise every slot carrying a non-empty marker, since
// a structural problem switches the engine off as a whole. why (optional) receives the first warning.
std::vector<size_t> mixed_engine_marker_problem_slots(const ConfigBase &config, std::string *why = nullptr);

// Moves the last (total - insert_pos) entries of every per-slot filament array to sit at insert_pos, so a physical
// filament appended at index total is placed before the mixed tail. The one list of per-slot project keys to
// rotate, including filament_mixed_engine_row, so the engine marker stays on its slot. No-op for arrays <= total.
void rotate_new_filament_before_mixed(ConfigBase &config, size_t insert_pos, size_t total);

} // namespace Slic3r

#endif
