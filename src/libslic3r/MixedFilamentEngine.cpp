#include "MixedFilamentEngine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>

namespace Slic3r {

namespace {

constexpr double k_epsilon = 1e-4; // libslic3r EPSILON, as used by Snapmaker's component_surface_offset

int clamp_int(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

float clamp_surface_offset(float v) { return std::clamp(v, -2.f, 2.f); }

// Sanity range for the height bounds fed to the height-weighted cadence (mm). Snapmaker applies none; the
// values come from project settings, and a huge or NaN bound must not reach lround().
constexpr float k_max_height_bound_mm = 100.f;

int safe_ratio_from_height(float h, float unit)
{
    if (unit <= 1e-6f)
        return 1;
    return std::max(0, int(std::lround(h / unit)));
}

void compute_gradient_heights(const MixedFilament &mf, float lower_bound, float upper_bound, float &h_a, float &h_b)
{
    const int   mix_b = clamp_int(mf.mix_b_percent, 0, 100);
    const float pct_b = float(mix_b) / 100.f;
    const float pct_a = 1.f - pct_b;
    const float lo    = std::max(0.01f, lower_bound);
    const float hi    = std::max(lo, upper_bound);

    h_a = lo + pct_a * (hi - lo);
    h_b = lo + pct_b * (hi - lo);
}

void compute_gradient_ratios(MixedFilament &mf, int gradient_mode, float lower_bound, float upper_bound)
{
    if (gradient_mode == 1) {
        float h_a = 0.f;
        float h_b = 0.f;
        compute_gradient_heights(mf, lower_bound, upper_bound, h_a, h_b);
        const float unit = std::max(0.01f, std::min(h_a, h_b));
        mf.ratio_a = std::max(1, safe_ratio_from_height(h_a, unit));
        mf.ratio_b = std::max(1, safe_ratio_from_height(h_b, unit));
    } else {
        const int mix_b = clamp_int(mf.mix_b_percent, 0, 100);
        if (mix_b <= 0) {
            mf.ratio_a = 1;
            mf.ratio_b = 0;
        } else if (mix_b >= 100) {
            mf.ratio_a = 0;
            mf.ratio_b = 1;
        } else {
            const int pct_b = mix_b;
            const int pct_a = 100 - pct_b;
            const bool b_is_major = pct_b >= pct_a;
            const int major_pct = b_is_major ? pct_b : pct_a;
            const int minor_pct = b_is_major ? pct_a : pct_b;
            const int major_layers = std::max(1, int(std::lround(double(major_pct) / double(std::max(1, minor_pct)))));
            mf.ratio_a = b_is_major ? 1 : major_layers;
            mf.ratio_b = b_is_major ? major_layers : 1;
        }
    }

    MixedFilamentEngine::normalize_ratio_pair(mf.ratio_a, mf.ratio_b);
}

int dithering_phase_step(int cycle)
{
    if (cycle <= 1)
        return 0;
    int step = cycle / 2 + 1;
    while (std::gcd(step, cycle) != 1)
        ++step;
    return step % cycle;
}

// Snapmaker use_component_b_advanced_dither. Products are 64 bit so a large cycle cannot overflow int.
bool use_component_b_advanced_dither(int layer_index, int ratio_a, int ratio_b)
{
    constexpr int k_max_ratio = 1 << 20;
    ratio_a = std::min(k_max_ratio, std::max(0, ratio_a));
    ratio_b = std::min(k_max_ratio, std::max(0, ratio_b));

    const int cycle = ratio_a + ratio_b;
    if (cycle <= 0 || ratio_b <= 0)
        return false;
    if (ratio_a <= 0)
        return true;

    const int pos = MixedFilamentEngine::safe_mod(layer_index, cycle);
    const int cycle_idx = (layer_index - pos) / cycle;

    const int phase = MixedFilamentEngine::safe_mod(int((int64_t(cycle_idx) * dithering_phase_step(cycle)) % cycle), cycle);
    const int p = MixedFilamentEngine::safe_mod(pos + phase, cycle);

    const int64_t b_before = (int64_t(p) * ratio_b) / cycle;
    const int64_t b_after  = (int64_t(p + 1) * ratio_b) / cycle;
    return b_after > b_before;
}

std::vector<std::string> split_manual_pattern_groups(const std::string &pattern)
{
    std::vector<std::string> groups;
    if (pattern.empty())
        return groups;

    std::string current;
    for (const char c : pattern) {
        if (c == ',') {
            if (!current.empty()) {
                groups.emplace_back(std::move(current));
                current.clear();
            }
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty())
        groups.emplace_back(std::move(current));
    return groups;
}

std::string flatten_manual_pattern_groups(const std::string &pattern)
{
    std::string flattened;
    flattened.reserve(pattern.size());
    for (const char c : pattern)
        if (c != ',')
            flattened.push_back(c);
    return flattened;
}

// Single-digit tokens and '[NN]' tokens; anything else is skipped (Snapmaker tokenize_pattern_group).
std::vector<std::string> tokenize_pattern_group(const std::string &group)
{
    std::vector<std::string> tokens;
    if (group.empty())
        return tokens;

    for (size_t i = 0; i < group.size(); ++i) {
        char c = group[i];
        if (c >= '1' && c <= '9') {
            tokens.emplace_back(1, c);
        } else if (c == '[') {
            size_t j = i + 1;
            while (j < group.size() && group[j] >= '0' && group[j] <= '9')
                ++j;
            if (j > i + 1 && j < group.size() && group[j] == ']') {
                tokens.emplace_back(group.substr(i + 1, j - i - 1));
                i = j;
            }
        }
    }
    return tokens;
}

// A manual pattern longer than kMaxManualPatternChars is ignored (the row resolves as if it had none).
const std::string &pattern_of(const MixedFilament &mf)
{
    static const std::string k_none;
    return mf.manual_pattern.size() > kMaxManualPatternChars ? k_none : mf.manual_pattern;
}

} // namespace

bool is_engine_row(const MixedFilament &row)
{
    return !pattern_of(row).empty() || !row.gradient_component_ids.empty() || !row.gradient_component_weights.empty() ||
           row.distribution_mode != int(MixedFilament::Simple) || row.local_z_max_sublayers != 0 || row.gradient_enabled ||
           row.component_a_surface_offset != 0.f || row.component_b_surface_offset != 0.f || row.ui_mode != -1;
}

std::vector<unsigned int> build_weighted_gradient_sequence(const std::vector<unsigned int> &ids,
                                                           const std::vector<int>          &weights)
{
    if (ids.empty())
        return {};

    std::vector<unsigned int> filtered_ids;
    std::vector<int>          counts;
    filtered_ids.reserve(ids.size());
    counts.reserve(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        const int w = (i < weights.size()) ? std::max(0, weights[i]) : 0;
        if (w <= 0)
            continue;
        filtered_ids.emplace_back(ids[i]);
        counts.emplace_back(w);
    }
    if (filtered_ids.empty()) {
        filtered_ids = ids;
        counts.assign(ids.size(), 1);
    }

    int g = 0;
    for (const int c : counts)
        g = std::gcd(g, std::max(1, c));
    if (g > 1) {
        for (int &c : counts)
            c = std::max(1, c / g);
    }

    int cycle = std::accumulate(counts.begin(), counts.end(), 0);
    constexpr int k_max_cycle = 48;
    if (cycle > k_max_cycle) {
        const double scale = double(k_max_cycle) / double(cycle);
        for (int &c : counts)
            c = std::max(1, int(std::round(double(c) * scale)));
        cycle = std::accumulate(counts.begin(), counts.end(), 0);
        while (cycle > k_max_cycle) {
            auto it = std::max_element(counts.begin(), counts.end());
            if (it == counts.end() || *it <= 1)
                break;
            --(*it);
            --cycle;
        }
    }
    if (cycle <= 0)
        return {};

    std::vector<unsigned int> sequence;
    sequence.reserve(size_t(cycle));
    std::vector<int> emitted(counts.size(), 0);
    for (int pos = 0; pos < cycle; ++pos) {
        size_t best_idx = 0;
        double best_score = -1e9;
        for (size_t i = 0; i < counts.size(); ++i) {
            const double target = double((pos + 1) * counts[i]) / double(cycle);
            const double score = target - double(emitted[i]);
            if (score > best_score) {
                best_score = score;
                best_idx = i;
            }
        }
        ++emitted[best_idx];
        sequence.emplace_back(filtered_ids[best_idx]);
    }
    return sequence;
}

MixedFilamentEngine MixedFilamentEngine::from_slots(const std::vector<MixedFilament> &slot_rows, size_t total_slots,
                                                    MixedEngineSlotsReport *report)
{
    MixedEngineSlotsReport local;
    MixedEngineSlotsReport &rep = report ? *report : local;
    rep = MixedEngineSlotsReport();

    if (slot_rows.size() >= total_slots) {
        rep.warnings.emplace_back("Mixed filament engine: more mixed slots than filament slots, or no physical slot left; engine disabled.");
        return MixedFilamentEngine();
    }
    const size_t num_physical = total_slots - slot_rows.size();

    std::vector<MixedFilament> rows = slot_rows;
    size_t forced_enabled = 0;
    size_t ignored_patterns = 0;
    for (MixedFilament &mf : rows) {
        if (mf.manual_pattern.size() > kMaxManualPatternChars)
            ++ignored_patterns;
        if (mf.component_a < 1 || mf.component_a > num_physical || mf.component_b < 1 || mf.component_b > num_physical) {
            rep.warnings.emplace_back("Mixed filament engine: a mixed slot names a physical filament that does not exist; engine disabled.");
            return MixedFilamentEngine();
        }
        if (!mf.enabled || mf.deleted) {
            mf.enabled = true;
            mf.deleted = false;
            ++forced_enabled;
        }
    }
    if (forced_enabled > 0)
        rep.warnings.emplace_back(std::to_string(forced_enabled) + " mixed slot row(s) were marked disabled or deleted; a slot that exists is always enabled.");
    if (ignored_patterns > 0)
        rep.warnings.emplace_back(std::to_string(ignored_patterns) + " mixed slot row(s) have a manual pattern longer than " +
                                  std::to_string(kMaxManualPatternChars) + " characters; it is ignored.");
    if (total_slots > kMixedEngineMaxSlots)
        rep.warnings.emplace_back("Mixed filament engine: " + std::to_string(total_slots) + " filament slots exceed the " +
                                  std::to_string(kMixedEngineMaxSlots) + " that painting can address; ids above that cannot be painted.");

    rep.valid        = true;
    rep.num_physical = num_physical;
    rep.num_mixed    = rows.size();
    MixedFilamentEngine engine(std::move(rows));
    engine.m_num_physical = num_physical;
    return engine;
}

void MixedFilamentEngine::normalize_ratio_pair(int &a, int &b)
{
    a = std::max(0, a);
    b = std::max(0, b);
    if (a == 0 && b == 0) {
        a = 1;
        return;
    }
    if (a > 0 && b > 0) {
        const int g = std::gcd(a, b);
        if (g > 1) {
            a /= g;
            b /= g;
        }
    }
}

int MixedFilamentEngine::safe_mod(int x, int m)
{
    if (m <= 0)
        return 0;
    int r = x % m;
    return (r < 0) ? (r + m) : r;
}

float MixedFilamentEngine::canonical_signed_bias_value(float component_a_surface_offset, float component_b_surface_offset)
{
    const float offset_a = clamp_surface_offset(component_a_surface_offset);
    const float offset_b = clamp_surface_offset(component_b_surface_offset);

    if (std::abs(offset_b) > k_epsilon)
        return offset_b;
    if (std::abs(offset_a) > k_epsilon)
        return (offset_a >= 0.f) ? -std::abs(offset_a) : std::abs(offset_a);
    return 0.f;
}

void MixedFilamentEngine::apply_gradient_settings(int gradient_mode, float lower_bound, float upper_bound, bool advanced_dithering)
{
    if (!std::isfinite(lower_bound)) lower_bound = 0.04f;
    if (!std::isfinite(upper_bound)) upper_bound = 0.16f;
    lower_bound = std::min(lower_bound, k_max_height_bound_mm);
    upper_bound = std::min(upper_bound, k_max_height_bound_mm);

    m_gradient_mode      = (gradient_mode != 0) ? 1 : 0;
    m_height_lower_bound = std::max(0.01f, lower_bound);
    m_height_upper_bound = std::max(m_height_lower_bound, upper_bound);
    m_advanced_dithering = advanced_dithering;

    for (MixedFilament &mf : m_mixed) {
        mf.pointillism_all_filaments = false; // Snapmaker disable_pointillism_mode
        {
            const int clamped = clamp_int(mf.distribution_mode, int(MixedFilament::LayerCycle), int(MixedFilament::Simple));
            if (clamped != int(MixedFilament::SameLayerPointillisme))
                mf.distribution_mode = clamped;
            else
                mf.distribution_mode = decode_gradient_component_ids(mf.gradient_component_ids, 0).size() >= 3 ?
                                           int(MixedFilament::LayerCycle) : int(MixedFilament::Simple);
        }
        if (!mf.custom) {
            mf.ratio_a = 1;
            mf.ratio_b = 1;
            continue;
        }
        compute_gradient_ratios(mf, m_gradient_mode, m_height_lower_bound, m_height_upper_bound);
    }
}

std::vector<std::string> MixedFilamentEngine::split_pattern_group_to_tokens(const std::string &group, size_t /*num_physical*/)
{
    return tokenize_pattern_group(group);
}

unsigned int MixedFilamentEngine::physical_filament_from_token(const std::string &token, const MixedFilament &mf, size_t num_physical)
{
    // Cycle-mode invariant: component_a==1, component_b==2 always, so "1"/"2" are symbolic.
    if (token == "1")
        return (mf.component_a >= 1 && mf.component_a <= num_physical) ? mf.component_a : 0;
    if (token == "2")
        return (mf.component_b >= 1 && mf.component_b <= num_physical) ? mf.component_b : 0;

    int id = 0;
    if (parse_bounded_mixed_token(token, id) && id >= 1 && size_t(id) <= num_physical)
        return unsigned(id);
    return 0;
}

std::vector<std::string> MixedFilamentEngine::split_pattern_groups(const std::string &pattern)
{
    std::vector<std::string> groups;
    std::string current;
    for (char c : pattern) {
        if (c == ',') {
            if (!current.empty()) {
                groups.emplace_back(std::move(current));
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty())
        groups.emplace_back(std::move(current));
    return groups;
}

int MixedFilamentEngine::mix_percent_from_manual_pattern(const std::string &pattern)
{
    const std::vector<std::string> groups = split_manual_pattern_groups(normalize_manual_pattern(pattern));
    if (groups.empty())
        return 50;

    double blend_b = 0.0;
    for (const std::string &group : groups) {
        if (group.empty())
            continue;
        const std::vector<std::string> tokens = tokenize_pattern_group(group);
        if (tokens.empty())
            continue;
        const int count_b = int(std::count(tokens.begin(), tokens.end(), "2"));
        blend_b += double(count_b) / double(tokens.size());
    }
    return clamp_int(int(std::lround(100.0 * blend_b / double(groups.size()))), 0, 100);
}

int MixedFilamentEngine::mixed_index_from_filament_id(unsigned int filament_id, size_t num_physical) const
{
    if (filament_id <= num_physical)
        return -1;

    const size_t enabled_virtual_idx = size_t(filament_id - num_physical - 1);
    size_t enabled_seen = 0;
    for (size_t i = 0; i < m_mixed.size(); ++i) {
        if (!m_mixed[i].enabled || m_mixed[i].deleted)
            continue;
        if (enabled_seen == enabled_virtual_idx)
            return int(i);
        ++enabled_seen;
    }
    return -1;
}

const MixedFilament *MixedFilamentEngine::mixed_filament_from_id(unsigned int filament_id, size_t num_physical) const
{
    const int idx = mixed_index_from_filament_id(filament_id, num_physical);
    return idx >= 0 ? &m_mixed[size_t(idx)] : nullptr;
}

unsigned int MixedFilamentEngine::filament_id_from_mixed_index(size_t row_index, size_t num_physical) const
{
    if (row_index >= m_mixed.size() || !m_mixed[row_index].enabled || m_mixed[row_index].deleted)
        return 0;
    size_t enabled_before = 0;
    for (size_t i = 0; i < row_index; ++i)
        if (m_mixed[i].enabled && !m_mixed[i].deleted)
            ++enabled_before;
    return unsigned(num_physical + 1 + enabled_before);
}

void MixedFilamentEngine::expand_virtual_extruder_ids(std::vector<int> &ids, size_t num_physical) const
{
    if (num_physical == 0)
        return;
    std::vector<int> expanded;
    expanded.reserve(ids.size() * 2);
    for (int id : ids) {
        if (id > static_cast<int>(num_physical)) {
            const MixedFilament *mf = mixed_filament_from_id(static_cast<unsigned int>(id), num_physical);
            if (mf != nullptr && mf->enabled) {
                expanded.push_back(static_cast<int>(mf->component_a));
                expanded.push_back(static_cast<int>(mf->component_b));
                auto gradient_ids = decode_gradient_component_ids(mf->gradient_component_ids, num_physical);
                for (unsigned int gid : gradient_ids)
                    expanded.push_back(static_cast<int>(gid));
            } else {
                expanded.push_back(id);
            }
        } else {
            expanded.push_back(id);
        }
    }
    ids = std::move(expanded);
}

unsigned int MixedFilamentEngine::resolve(unsigned int filament_id, size_t num_physical, int layer_index, float layer_print_z,
                                          float layer_height, bool force_height_weighted) const
{
    const int mixed_idx = mixed_index_from_filament_id(filament_id, num_physical);
    if (mixed_idx < 0)
        return filament_id;

    const MixedFilament &mf = m_mixed[size_t(mixed_idx)];

    // Manual pattern takes precedence when provided.
    if (!pattern_of(mf).empty()) {
        const std::string flattened_pattern = flatten_manual_pattern_groups(pattern_of(mf));
        if (!flattened_pattern.empty()) {
            const std::vector<std::string> tokens = split_pattern_group_to_tokens(flattened_pattern, num_physical);
            if (!tokens.empty()) {
                const int pos = safe_mod(layer_index, int(tokens.size()));
                const unsigned int resolved = physical_filament_from_token(tokens[size_t(pos)], mf, num_physical);
                if (resolved >= 1 && resolved <= num_physical)
                    return resolved;
            }
        }
        return mf.component_a;
    }

    const bool use_simple_mode = mf.distribution_mode == int(MixedFilament::Simple);
    const std::vector<unsigned int> gradient_ids = decode_gradient_component_ids(mf.gradient_component_ids, num_physical);
    if (!use_simple_mode && gradient_ids.size() >= 3) {
        const std::vector<int> gradient_weights = decode_gradient_component_weights(mf.gradient_component_weights, gradient_ids.size());
        const std::vector<unsigned int> gradient_sequence = build_weighted_gradient_sequence(
            gradient_ids, gradient_weights.empty() ? std::vector<int>(gradient_ids.size(), 1) : gradient_weights);
        if (!gradient_sequence.empty()) {
            const size_t pos = size_t(safe_mod(layer_index, int(gradient_sequence.size())));
            return gradient_sequence[pos];
        }
    }

    // Height-weighted cadence: forced by the local-Z planner, or the gradient height mode for custom rows.
    const bool use_height_weighted = force_height_weighted || (m_gradient_mode == 1 && mf.custom);
    if (use_height_weighted) {
        float h_a = 0.f;
        float h_b = 0.f;
        compute_gradient_heights(mf, m_height_lower_bound, m_height_upper_bound, h_a, h_b);
        const float cycle_h = std::max(0.01f, h_a + h_b);
        const float z_anchor = (layer_height > 1e-6f)
            ? std::max(0.f, layer_print_z - 0.5f * layer_height)
            : std::max(0.f, layer_print_z);
        float phase = std::fmod(z_anchor, cycle_h);
        if (phase < 0.f)
            phase += cycle_h;
        return (phase < h_a) ? mf.component_a : mf.component_b;
    }

    const int64_t cycle = int64_t(mf.ratio_a) + int64_t(mf.ratio_b);
    if (cycle <= 0)
        return mf.component_a;

    if (m_gradient_mode == 0 && m_advanced_dithering && mf.custom)
        return use_component_b_advanced_dither(layer_index, mf.ratio_a, mf.ratio_b) ? mf.component_b : mf.component_a;

    const int64_t pos = ((int64_t(layer_index) % cycle) + cycle) % cycle; // safe modulo for negatives
    return (pos < mf.ratio_a) ? mf.component_a : mf.component_b;
}

unsigned int MixedFilamentEngine::resolve_perimeter(unsigned int filament_id, size_t num_physical, int layer_index, int perimeter_index,
                                                    float layer_print_z, float layer_height, bool force_height_weighted) const
{
    const int mixed_idx = mixed_index_from_filament_id(filament_id, num_physical);
    if (mixed_idx < 0)
        return filament_id;

    const MixedFilament &mf = m_mixed[size_t(mixed_idx)];
    if (!pattern_of(mf).empty()) {
        const std::vector<std::string> pattern_groups = split_manual_pattern_groups(pattern_of(mf));
        if (!pattern_groups.empty()) {
            const size_t group_idx = size_t(std::max(0, perimeter_index));
            const std::string &group = pattern_groups[std::min(group_idx, pattern_groups.size() - 1)];
            if (!group.empty()) {
                const std::vector<std::string> tokens = split_pattern_group_to_tokens(group, num_physical);
                if (!tokens.empty()) {
                    const int pos = safe_mod(layer_index, int(tokens.size()));
                    const unsigned int resolved = physical_filament_from_token(tokens[size_t(pos)], mf, num_physical);
                    if (resolved >= 1 && resolved <= num_physical)
                        return resolved;
                }
            }
        }
    }

    return resolve(filament_id, num_physical, layer_index, layer_print_z, layer_height, force_height_weighted);
}

unsigned int MixedFilamentEngine::effective_painted_region_filament_id(unsigned int filament_id, size_t num_physical, int layer_index,
                                                                       float layer_print_z, float layer_height, float layer_height_a,
                                                                       float layer_height_b, float base_layer_height) const
{
    const int mixed_idx = mixed_index_from_filament_id(filament_id, num_physical);
    if (mixed_idx < 0)
        return filament_id;

    const MixedFilament &mf = m_mixed[size_t(mixed_idx)];
    if (mf.distribution_mode == int(MixedFilament::SameLayerPointillisme))
        return filament_id;

    const std::string normalized_pattern = normalize_manual_pattern(pattern_of(mf));
    if (normalized_pattern.find(',') != std::string::npos)
        return filament_id;

    const bool is_custom_mixed = mf.custom;
    if (!is_custom_mixed && (layer_height_a > 0.f || layer_height_b > 0.f)) {
        const float safe_base = std::max<float>(0.01f, base_layer_height);
        // Snapmaker: max(1, lround(h / safe_base)), a float division (it matters at .5 ties). The quotient is capped
        // so a hostile height cannot reach lround UB.
        auto ratio_of = [safe_base](float h) {
            float q = (h > 0.f ? h : safe_base) / safe_base;
            if (!(q <= 1e6f))
                q = 1e6f;
            return std::max(1, int(std::lround(q)));
        };
        const int ratio_a = ratio_of(layer_height_a);
        const int ratio_b = ratio_of(layer_height_b);
        const int cycle   = ratio_a + ratio_b;

        if (cycle > 0) {
            const int pos = ((layer_index % cycle) + cycle) % cycle;
            return pos < ratio_a ? mf.component_a : mf.component_b;
        }
    }

    return resolve(filament_id, num_physical, layer_index, layer_print_z, layer_height);
}

float MixedFilamentEngine::component_surface_offset(unsigned int filament_id, size_t num_physical, int layer_index, float layer_print_z,
                                                    float layer_height, bool force_height_weighted) const
{
    const MixedFilament *mixed_row = mixed_filament_from_id(filament_id, num_physical);
    if (mixed_row == nullptr)
        return 0.f;

    if (mixed_row->distribution_mode == int(MixedFilament::SameLayerPointillisme))
        return 0.f;

    const std::string normalized_pattern = normalize_manual_pattern(pattern_of(*mixed_row));
    if (normalized_pattern.find(',') != std::string::npos)
        return 0.f;

    const unsigned int resolved = resolve(filament_id, num_physical, layer_index, layer_print_z, layer_height, force_height_weighted);
    const float signed_bias = canonical_signed_bias_value(mixed_row->component_a_surface_offset, mixed_row->component_b_surface_offset);
    if (signed_bias > k_epsilon && resolved == mixed_row->component_b)
        return signed_bias;
    if (signed_bias < -k_epsilon && resolved == mixed_row->component_a)
        return -signed_bias;
    return 0.f;
}

std::vector<unsigned int> MixedFilamentEngine::ordered_perimeter_extruders(unsigned int filament_id, size_t num_physical, int layer_index,
                                                                           float layer_print_z, float layer_height,
                                                                           bool force_height_weighted) const
{
    std::vector<unsigned int> ordered;

    const int mixed_idx = mixed_index_from_filament_id(filament_id, num_physical);
    if (mixed_idx < 0) {
        ordered.emplace_back(filament_id);
        return ordered;
    }

    const MixedFilament &mf = m_mixed[size_t(mixed_idx)];
    if (!pattern_of(mf).empty()) {
        const std::vector<std::string> pattern_groups = split_manual_pattern_groups(pattern_of(mf));
        if (!pattern_groups.empty()) {
            ordered.reserve(pattern_groups.size());
            for (size_t group_idx = 0; group_idx < pattern_groups.size(); ++group_idx) {
                const unsigned int resolved = resolve_perimeter(filament_id, num_physical, layer_index, int(group_idx),
                                                                layer_print_z, layer_height, force_height_weighted);
                if (resolved < 1 || resolved > num_physical)
                    continue;
                if (std::find(ordered.begin(), ordered.end(), resolved) == ordered.end())
                    ordered.emplace_back(resolved);
            }
            if (!ordered.empty())
                return ordered;
        }
    }

    ordered.emplace_back(resolve(filament_id, num_physical, layer_index, layer_print_z, layer_height, force_height_weighted));
    return ordered;
}

size_t MixedFilamentEngine::enabled_count() const
{
    size_t n = 0;
    for (const MixedFilament &mf : m_mixed)
        if (mf.enabled && !mf.deleted)
            ++n;
    return n;
}

} // namespace Slic3r
