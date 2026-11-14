#include "MixedFilamentEngine.hpp"

#include "Config.hpp"
#include "FilamentMixer.hpp"
#include "MixedFilamentLegacy.hpp"
#include "PrintConfig.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <sstream>

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

// ---- Slice 4b ---------------------------------------------------------------------------------------------

namespace {

float sanitize_global(float v, float lo, float hi, float def, const char *key, std::vector<std::string> *warnings)
{
    float out = v;
    if (!std::isfinite(v))
        out = def;
    else
        out = std::min(std::max(v, lo), hi);
    if (out != v && warnings != nullptr)
        warnings->push_back(std::string("Mixed filament setting ") + key + " was out of range and was corrected.");
    return out;
}

// Largest value accepted for the mm-valued globals; far above any real nozzle or layer height.
constexpr float k_max_global_mm = 100.f;

std::string pattern_token(unsigned int n)
{
    return n >= 10 ? "[" + std::to_string(n) + "]" : std::string(1, char('0' + n));
}

} // namespace

MixedEngineGlobals mixed_engine_globals_from_config(const ConfigBase &config, std::vector<std::string> *warnings)
{
    MixedEngineGlobals g;
    auto f = [&](const char *key, float def, float lo, float hi) {
        const auto *o = config.option<ConfigOptionFloat>(key);
        return o == nullptr ? def : sanitize_global(float(o->value), lo, hi, def, key, warnings);
    };
    auto b = [&](const char *key, bool def) {
        const auto *o = config.option<ConfigOptionBool>(key);
        return o == nullptr ? def : o->value;
    };
    g.color_layer_height_a   = f("mixed_color_layer_height_a", 0.f, 0.f, k_max_global_mm);
    g.color_layer_height_b   = f("mixed_color_layer_height_b", 0.f, 0.f, k_max_global_mm);
    g.gradient_mode          = b("mixed_filament_gradient_mode", false);
    g.height_lower_bound     = f("mixed_filament_height_lower_bound", 0.04f, 0.01f, k_max_global_mm);
    g.height_upper_bound     = f("mixed_filament_height_upper_bound", 0.16f, 0.01f, k_max_global_mm);
    g.advanced_dithering     = b("mixed_filament_advanced_dithering", false);
    g.pointillism_pixel_size = f("mixed_filament_pointillism_pixel_size", 0.f, 0.f, k_max_global_mm);
    g.pointillism_line_gap   = f("mixed_filament_pointillism_line_gap", 0.f, 0.f, k_max_global_mm);
    g.component_bias_enabled = b("mixed_filament_component_bias_enabled", false);
    g.surface_indentation    = f("mixed_filament_surface_indentation", 0.f, -2.f, 2.f);
    g.region_collapse        = b("mixed_filament_region_collapse", true);
    return g;
}

MixedFilamentEngine mixed_engine_from_config(const ConfigBase &config, MixedEngineSlotsReport *report)
{
    MixedEngineSlotsReport local;
    MixedEngineSlotsReport &rep = report ? *report : local;
    rep = MixedEngineSlotsReport();

    const auto *rows_opt = config.option<ConfigOptionStrings>("filament_mixed_engine_row");
    if (rows_opt == nullptr || std::all_of(rows_opt->values.begin(), rows_opt->values.end(), [](const std::string &s) { return s.empty(); }))
        return MixedFilamentEngine(); // native project: no engine rows

    auto fail = [&](const std::string &why) {
        rep = MixedEngineSlotsReport();
        rep.warnings.push_back("Mixed filament engine: " + why + "; engine disabled.");
        return MixedFilamentEngine();
    };

    const auto *colours  = config.option<ConfigOptionStrings>("filament_colour");
    const auto *is_mixed = config.option<ConfigOptionBools>("filament_is_mixed");
    const auto *comps    = config.option<ConfigOptionStrings>("filament_mixed_components");
    if (colours == nullptr || is_mixed == nullptr)
        return fail("filament_colour or filament_is_mixed is missing");
    const size_t total = colours->values.size();
    if (rows_opt->values.size() > total || is_mixed->values.size() > total)
        return fail("per-slot mixed settings have more entries than there are filament slots");

    // Mixed slots must be a contiguous tail, so that slot id == num_physical + 1 + k.
    size_t num_mixed = 0;
    for (size_t i = 0; i < is_mixed->values.size(); ++i)
        if (is_mixed->values[i])
            ++num_mixed;
    if (num_mixed == 0 || num_mixed >= total)
        return fail("no mixed slots, or no physical slot is left");
    const size_t num_physical = total - num_mixed;
    for (size_t i = 0; i < total; ++i) {
        const bool mixed = i < is_mixed->values.size() && is_mixed->values[i];
        if (mixed != (i >= num_physical))
            return fail("mixed slots are not the last slots");
    }
    for (size_t i = 0; i < rows_opt->values.size() && i < num_physical; ++i)
        if (!rows_opt->values[i].empty())
            return fail("physical slot " + std::to_string(i + 1) + " carries an engine row");

    std::vector<MixedFilament> slot_rows;
    std::vector<std::string>   row_warnings;
    std::vector<bool>          demoted(num_mixed, false);
    slot_rows.reserve(num_mixed);
    for (size_t i = num_physical; i < total; ++i) {
        const std::string text = i < rows_opt->values.size() ? rows_opt->values[i] : std::string();
        MixedFilament     mf;
        if (text.empty()) {
            // Native slot: placeholder row, never resolved by the engine (is_engine_row is false for it: empty marker).
            std::vector<unsigned int> c;
            if (comps != nullptr && i < comps->values.size())
                c = parse_mixed_components(comps->values[i]);
            mf.component_a = (c.size() >= 2 && c[0] >= 1 && c[0] <= num_physical) ? c[0] : 1;
            mf.component_b = (c.size() >= 2 && c[1] >= 1 && c[1] <= num_physical) ? c[1] : 1;
        } else {
            // Per-row failure: one unusable row must not switch the other rows off. The slot falls back to a native
            // 2.5 mix (placeholder row, marker ignored) and the user is told. Only structural problems (checked
            // above and in from_slots) disable the whole engine, because they break the slot-id arithmetic.
            LegacyMixedRow r;
            const bool parsed = text.size() <= 4096 && text.find(';') == std::string::npos && parse_legacy_mixed_row(text, r);
            mf = parsed ? mixed_filament_from_legacy_row(r) : MixedFilament();
            if (!parsed || mf.component_a < 1 || mf.component_a > num_physical || mf.component_b < 1 || mf.component_b > num_physical) {
                row_warnings.push_back("Mixed filament engine: the engine row of slot " + std::to_string(i + 1) +
                                       (parsed ? " names a physical filament that does not exist" : " is malformed") +
                                       "; the slot is used as a native mix instead.");
                std::vector<unsigned int> c;
                if (comps != nullptr && i < comps->values.size())
                    c = parse_mixed_components(comps->values[i]);
                mf = MixedFilament();
                mf.component_a = (c.size() >= 2 && c[0] >= 1 && c[0] <= num_physical) ? c[0] : 1;
                mf.component_b = (c.size() >= 2 && c[1] >= 1 && c[1] <= num_physical) ? c[1] : 1;
                demoted[i - num_physical] = true;
            }
        }
        slot_rows.push_back(std::move(mf));
    }

    MixedEngineSlotsReport inner;
    MixedFilamentEngine engine = MixedFilamentEngine::from_slots(slot_rows, total, &inner);
    rep = inner;
    if (!inner.valid)
        return MixedFilamentEngine();
    rep.warnings.insert(rep.warnings.end(), row_warnings.begin(), row_warnings.end());
    std::vector<bool> engine_rows(num_mixed, false);
    for (size_t k = 0; k < num_mixed; ++k)
        engine_rows[k] = !demoted[k] && num_physical + k < rows_opt->values.size() && !rows_opt->values[num_physical + k].empty();
    engine.set_engine_rows(std::move(engine_rows));

    const MixedEngineGlobals g = mixed_engine_globals_from_config(config, &rep.warnings);
    engine.apply_gradient_settings(g.gradient_mode ? 1 : 0, g.height_lower_bound, g.height_upper_bound, g.advanced_dithering);
    return engine;
}

unsigned int mixed_engine_resolve_layer(const MixedFilamentEngine &engine, const MixedEngineGlobals &globals, unsigned int filament_id,
                                        size_t num_physical, int layer_index, float layer_print_z, float layer_height,
                                        float base_layer_height)
{
    if (!engine.is_mixed(filament_id, num_physical))
        return filament_id;
    const MixedFilament *row = engine.mixed_filament_from_id(filament_id, num_physical);
    const float layer_height_a = globals.color_layer_height_a, layer_height_b = globals.color_layer_height_b;

    const bool gradient_active = row != nullptr && row->gradient_enabled && row->component_a != row->component_b && layer_index > 0;
    if (row != nullptr && !row->custom && !gradient_active && (layer_height_a > 0.f || layer_height_b > 0.f)) {
        const float safe_base = std::max<float>(0.01f, base_layer_height);
        const int ratio_a = std::max(1, int(std::lround((layer_height_a > 0.f ? layer_height_a : safe_base) / safe_base)));
        const int ratio_b = std::max(1, int(std::lround((layer_height_b > 0.f ? layer_height_b : safe_base) / safe_base)));
        const int cycle   = ratio_a + ratio_b;
        const int pos     = ((layer_index % cycle) + cycle) % cycle;
        return pos < ratio_a ? row->component_a : row->component_b;
    }
    return engine.resolve(filament_id, num_physical, layer_index, layer_print_z, layer_height, false);
}

bool clear_mixed_engine_marker(ConfigBase &config, size_t slot)
{
    auto *rows = config.option<ConfigOptionStrings>("filament_mixed_engine_row");
    if (rows == nullptr || slot >= rows->values.size() || rows->values[slot].empty())
        return false;
    rows->values[slot].clear();
    return true;
}

std::vector<size_t> mixed_engine_marker_problem_slots(const ConfigBase &config, std::string *why)
{
    std::vector<size_t> out;
    const auto *rows = config.option<ConfigOptionStrings>("filament_mixed_engine_row");
    if (rows == nullptr)
        return out;
    MixedEngineSlotsReport rep;
    const MixedFilamentEngine e = mixed_engine_from_config(config, &rep);
    if (rep.valid || rep.warnings.empty())
        return out;
    for (size_t i = 0; i < rows->values.size(); ++i)
        if (!rows->values[i].empty())
            out.push_back(i);
    if (why != nullptr)
        *why = rep.warnings.front();
    return out;
}

void rotate_new_filament_before_mixed(ConfigBase &config, size_t insert_pos, size_t total)
{
    if (insert_pos >= total)
        return;
    auto rot = [&](auto *opt) {
        if (opt != nullptr && opt->values.size() > total)
            std::rotate(opt->values.begin() + insert_pos, opt->values.begin() + total, opt->values.end());
    };
    for (const char *k : { "filament_colour", "filament_multi_colour", "filament_colour_type", "filament_mixed_components",
                           "filament_mixed_sublayer_ratios", "filament_mixed_gradient_range", "filament_mixed_gradient_curve",
                           "filament_mixed_engine_row" })
        rot(config.option<ConfigOptionStrings>(k));
    for (const char *k : { "filament_map", "filament_nozzle_map", "filament_volume_map" })
        rot(config.option<ConfigOptionInts>(k));
    for (const char *k : { "filament_is_mixed", "filament_mixed_gradient", "filament_mixed_gradient_per_part" })
        rot(config.option<ConfigOptionBools>(k));
}

void remap_mixed_engine_rows_on_delete(const std::vector<unsigned char> &is_mixed, std::vector<std::string> &rows,
                                       unsigned int del_1based, std::vector<std::string> *components,
                                       std::vector<std::string> *warnings)
{
    constexpr size_t k_max_ids = 99; // the largest id a '[NN]' pattern token can carry
    for (size_t i = 0; i < rows.size() && i < is_mixed.size(); ++i) {
        if (!is_mixed[i] || rows[i].empty())
            continue;
        LegacyMixedRow r;
        if (rows[i].size() > 4096 || rows[i].find(';') != std::string::npos || !parse_legacy_mixed_row(rows[i], r))
            continue;
        MixedFilament mf = mixed_filament_from_legacy_row(r);
        auto down = [del_1based](unsigned int id) { return id > del_1based ? id - 1 : id; };

        // Snapmaker's MixedFilamentManager::remove_physical_filament, check order and adjust rules as in
        // MixedFilament.cpp: (1) manual pattern, (2) gradient ids (only without a pattern), (3) pair (only without
        // a pattern). Tokens "1"/"2" of a pattern are the symbolic A/B, not literal ids.
        const std::string norm = normalize_manual_pattern(mf.manual_pattern);
        std::vector<std::string> pattern_groups;
        std::vector<unsigned int> used; // physical ids the row resolves to, in order, before the delete
        if (!norm.empty()) {
            pattern_groups = MixedFilamentEngine::split_pattern_groups(norm);
            for (const std::string &g : pattern_groups)
                for (const std::string &t : tokenize_pattern_group(g)) {
                    const unsigned int id = MixedFilamentEngine::physical_filament_from_token(t, mf, k_max_ids);
                    if (id != 0)
                        used.push_back(id);
                }
        }
        const std::vector<unsigned int> grad_ids = decode_gradient_component_ids(mf.gradient_component_ids, 0);
        bool drop = std::find(used.begin(), used.end(), del_1based) != used.end();
        if (norm.empty()) {
            used = grad_ids.empty() ? std::vector<unsigned int>{ mf.component_a, mf.component_b } : grad_ids;
            drop = std::find(grad_ids.begin(), grad_ids.end(), del_1based) != grad_ids.end() ||
                   mf.component_a == del_1based || mf.component_b == del_1based;
        }

        if (drop) {
            // Snapmaker drops the row. Dropping a slot would renumber every later slot and paint id, so the slot is kept
            // and becomes a native 2.5 mix of the surviving components instead (a single survivor leaves it native
            // with that one component and a 0 sentinel, which is what remap_mixed_components_on_delete uses).
            std::vector<unsigned int> survivors;
            for (unsigned int id : used)
                if (id != del_1based && id != 0 && std::find(survivors.begin(), survivors.end(), down(id)) == survivors.end())
                    survivors.push_back(down(id));
            if (components != nullptr && i < components->size()) {
                std::string c;
                for (unsigned int id : survivors)
                    c += (c.empty() ? "" : ",") + std::to_string(id);
                if (survivors.size() < 2)
                    c += (c.empty() ? "0" : ",0");
                (*components)[i] = c;
            }
            if (warnings != nullptr)
                warnings->push_back("Mixed slot " + std::to_string(i + 1) + " used the deleted filament " + std::to_string(del_1based) +
                                    "; Snapmaker's recipe for it is gone and the slot is now a native mix of " +
                                    (survivors.size() >= 2 ? "its remaining filaments." : "one remaining filament (pick a second one)."));
            rows[i].clear();
            continue;
        }

        if (!norm.empty()) {
            // Pattern tokens: Snapmaker's rules (symbolic 1/2 = A/B, every other id is literal and shifts).
            std::string adjusted;
            for (size_t gi = 0; gi < pattern_groups.size(); ++gi) {
                if (gi > 0)
                    adjusted += ',';
                for (const std::string &t : tokenize_pattern_group(pattern_groups[gi])) {
                    int id = 0;
                    if (parse_bounded_mixed_token(t, id) && unsigned(id) > del_1based)
                        adjusted += pattern_token(unsigned(id) - 1);
                    else
                        adjusted += t.size() > 1 ? "[" + t + "]" : t;
                }
            }
            mf.manual_pattern = adjusted;
            // DEPARTURE FROM SNAPMAKER (docs/u1-differences-from-snapmaker.md): Snapmaker leaves the pair of a pattern row
            // unshifted, so a pair other than exactly (1,2) names the wrong filament after a delete. The pair is still the
            // row's display/ratio colours, so it is shifted unless it is exactly (1,2). A pair member equal to the deleted
            // id that the pattern does not need (the pattern needing it dropped the row above) has no successor: it
            // becomes the lowest filament id that differs from the other member (the pair must stay two distinct ids) and
            // the user is warned.
            if (!(mf.component_a == 1 && mf.component_b == 2)) {
                const bool a_gone = mf.component_a == del_1based, b_gone = mf.component_b == del_1based;
                unsigned int a = a_gone ? 0 : down(mf.component_a), b = b_gone ? 0 : down(mf.component_b);
                if (a_gone || b_gone) {
                    const unsigned int other = a_gone ? b : a;
                    const unsigned int sub   = other == 1 ? 2 : 1;
                    (a_gone ? a : b) = sub;
                    if (a_gone && b_gone)
                        b = sub == 1 ? 2 : 1;
                    if (warnings != nullptr)
                        warnings->push_back("Mixed slot " + std::to_string(i + 1) + " had the deleted filament " + std::to_string(del_1based) +
                                            " in its filament pair; the pattern does not use it, so the pair now shows filament " +
                                            std::to_string(sub) + " instead.");
                }
                mf.component_a = a;
                mf.component_b = b;
            }
        } else {
            mf.component_a = down(mf.component_a);
            mf.component_b = down(mf.component_b);
        }
        // Gradient ids: with a pattern the deleted id is erased (the pattern is what resolves), then all shift down.
        if (!mf.gradient_component_ids.empty()) {
            std::vector<unsigned int> ids = grad_ids;
            if (!norm.empty()) {
                // The matching weight goes with the id so ids and weights stay the same length (serialisation would
                // otherwise discard the whole weight list on a count mismatch).
                std::vector<std::string> wt;
                {
                    std::stringstream ss(mf.gradient_component_weights);
                    std::string tok;
                    while (std::getline(ss, tok, '/'))
                        wt.push_back(tok);
                }
                const auto it = std::find(ids.begin(), ids.end(), del_1based);
                if (it != ids.end()) {
                    const size_t idx = size_t(it - ids.begin());
                    if (wt.size() == ids.size()) {
                        wt.erase(wt.begin() + idx);
                        std::string w;
                        for (size_t k = 0; k < wt.size(); ++k)
                            w += (k ? "/" : "") + wt[k];
                        mf.gradient_component_weights = w;
                    }
                    ids.erase(it);
                }
            }
            for (unsigned int &id : ids)
                id = down(id);
            mf.gradient_component_ids = encode_gradient_component_ids(ids);
        }
        // serialize_mixed_filaments gives a row without a stable id the id 1; done per row that would put the same id on
        // every such row. Give it the same unique id the legacy conversion would (1000000 + slot), keep any real id.
        if (mf.stable_id == 0)
            mf.stable_id = 1000000 + i;
        rows[i] = serialize_mixed_filaments({ mf });
    }
}

} // namespace Slic3r
