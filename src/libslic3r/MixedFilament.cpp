#include "MixedFilament.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <locale>
#include <sstream>
#include <unordered_set>

#include "LocalesUtils.hpp"
#include "MixedFilamentLegacy.hpp"

namespace Slic3r {

namespace {

int clamp_int(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

float clamp_surface_offset(float v) { return std::clamp(v, -2.f, 2.f); }

std::vector<int> parse_gradient_weight_tokens(const std::string &weights)
{
    std::vector<int> out;
    std::string token;
    for (const char c : weights) {
        if (c >= '0' && c <= '9') {
            token.push_back(c);
            continue;
        }
        if (!token.empty()) {
            out.emplace_back(std::max(0, std::atoi(token.c_str())));
            token.clear();
        }
    }
    if (!token.empty())
        out.emplace_back(std::max(0, std::atoi(token.c_str())));
    return out;
}

std::vector<int> normalize_weight_vector_to_percent(const std::vector<int> &weights)
{
    std::vector<int> out(weights.size(), 0);
    if (weights.empty())
        return out;
    int sum = 0;
    for (const int w : weights)
        sum += std::max(0, w);
    if (sum <= 0)
        return out;

    std::vector<double> remainders(weights.size(), 0.);
    int assigned = 0;
    for (size_t i = 0; i < weights.size(); ++i) {
        const double exact = 100.0 * double(std::max(0, weights[i])) / double(sum);
        out[i] = int(std::floor(exact));
        remainders[i] = exact - double(out[i]);
        assigned += out[i];
    }
    int missing = std::max(0, 100 - assigned);
    while (missing > 0) {
        size_t best_idx = 0;
        double best_rem = -1.0;
        for (size_t i = 0; i < remainders.size(); ++i) {
            if (weights[i] <= 0)
                continue;
            if (remainders[i] > best_rem) {
                best_rem = remainders[i];
                best_idx = i;
            }
        }
        ++out[best_idx];
        remainders[best_idx] = 0.0;
        --missing;
    }
    return out;
}

// Snapmaker normalize_distribution_mode_without_pointillism + disable_pointillism_mode.
int normalize_distribution_mode_without_pointillism(int distribution_mode, const std::string &gradient_component_ids)
{
    const int clamped_mode = clamp_int(distribution_mode, int(MixedFilament::LayerCycle), int(MixedFilament::Simple));
    if (clamped_mode != int(MixedFilament::SameLayerPointillisme))
        return clamped_mode;
    const size_t gradient_count = decode_gradient_component_ids(gradient_component_ids, 0).size();
    return gradient_count >= 3 ? int(MixedFilament::LayerCycle) : int(MixedFilament::Simple);
}

void disable_pointillism_mode(MixedFilament &mf)
{
    mf.pointillism_all_filaments = false;
    mf.distribution_mode = normalize_distribution_mode_without_pointillism(mf.distribution_mode, mf.gradient_component_ids);
}

} // namespace

bool MixedFilament::operator==(const MixedFilament &rhs) const
{
    constexpr float k_surface_offset_epsilon = 1e-6f;
    constexpr float k_gradient_epsilon       = 1e-4f;
    return component_a == rhs.component_a &&
           component_b == rhs.component_b &&
           stable_id   == rhs.stable_id   &&
           ratio_a     == rhs.ratio_a     &&
           ratio_b     == rhs.ratio_b     &&
           mix_b_percent == rhs.mix_b_percent &&
           manual_pattern == rhs.manual_pattern &&
           gradient_component_ids == rhs.gradient_component_ids &&
           gradient_component_weights == rhs.gradient_component_weights &&
           pointillism_all_filaments == rhs.pointillism_all_filaments &&
           distribution_mode == rhs.distribution_mode &&
           local_z_max_sublayers == rhs.local_z_max_sublayers &&
           gradient_enabled == rhs.gradient_enabled &&
           std::abs(gradient_start - rhs.gradient_start) <= k_gradient_epsilon &&
           std::abs(gradient_end   - rhs.gradient_end)   <= k_gradient_epsilon &&
           std::abs(component_a_surface_offset - rhs.component_a_surface_offset) <= k_surface_offset_epsilon &&
           std::abs(component_b_surface_offset - rhs.component_b_surface_offset) <= k_surface_offset_epsilon &&
           enabled      == rhs.enabled &&
           deleted      == rhs.deleted &&
           custom       == rhs.custom &&
           origin_auto  == rhs.origin_auto &&
           ui_mode      == rhs.ui_mode;
}

std::string encode_gradient_component_ids(const std::vector<unsigned int> &ids)
{
    bool extended = false;
    for (unsigned int id : ids)
        if (id > 9) { extended = true; break; }

    // Single extended ID: use leading '/' to disambiguate from legacy format
    if (extended && ids.size() == 1)
        return "/" + std::to_string(ids[0]);

    std::string out;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i > 0 && extended) out.push_back('/');
        if (extended)
            out.append(std::to_string(ids[i]));
        else
            out.push_back(char('0' + ids[i]));
    }
    return out;
}

std::vector<unsigned int> decode_gradient_component_ids(const std::string &components, size_t num_physical)
{
    std::vector<unsigned int> ids;
    if (components.empty())
        return ids;

    const bool validate = (num_physical > 0);
    std::unordered_set<unsigned int> seen;
    ids.reserve(components.size());

    if (components.find('/') != std::string::npos) {
        std::string token;
        auto flush = [&]() {
            if (token.empty())
                return;
            const unsigned int id = unsigned(std::strtoul(token.c_str(), nullptr, 10));
            if (id >= 1 && (!validate || id <= num_physical) && seen.insert(id).second)
                ids.emplace_back(id);
            token.clear();
        };
        for (const char c : components) {
            if (c == '/')
                flush();
            else
                token.push_back(c);
        }
        flush();
    } else {
        bool seen_legacy[10] = { false };
        for (const char c : components) {
            if (c < '1' || c > '9')
                continue;
            const unsigned int id = unsigned(c - '0');
            if (id == 0 || (validate && id > num_physical) || seen_legacy[id])
                continue;
            seen_legacy[id] = true;
            ids.emplace_back(id);
        }
    }
    return ids;
}

std::string normalize_gradient_component_ids(const std::string &components)
{
    return encode_gradient_component_ids(decode_gradient_component_ids(components, kMaxMixedPhysicalFilaments));
}

std::string normalize_gradient_component_weights(const std::string &weights, size_t expected_components)
{
    if (expected_components == 0)
        return std::string();
    std::vector<int> parsed = parse_gradient_weight_tokens(weights);
    if (parsed.size() != expected_components)
        return std::string();
    std::vector<int> normalized = normalize_weight_vector_to_percent(parsed);
    int sum = 0;
    for (const int v : normalized)
        sum += v;
    if (sum <= 0)
        return std::string();

    std::ostringstream ss;
    for (size_t i = 0; i < normalized.size(); ++i) {
        if (i > 0)
            ss << '/';
        ss << normalized[i];
    }
    return ss.str();
}

std::string normalize_manual_pattern(const std::string &pattern)
{
    if (pattern.empty())
        return {};
    std::string normalized;
    bool        group_has_content = false;
    for (size_t i = 0; i < pattern.size(); ++i) {
        const char c = pattern[i];
        if (c >= '1' && c <= '9') {
            normalized.push_back(c);
            group_has_content = true;
        } else if (c == ',') {
            if (!group_has_content)
                return {};
            normalized.push_back(',');
            group_has_content = false;
        } else if (c == '[') {
            size_t j = i + 1;
            while (j < pattern.size() && pattern[j] >= '0' && pattern[j] <= '9')
                ++j;
            if (j == i + 1 || j >= pattern.size() || pattern[j] != ']')
                return {};
            const std::string num = pattern.substr(i + 1, j - i - 1);
            if (num.size() > 2 || (num.size() > 1 && num[0] == '0') || num == "0")
                return {};
            if (num.size() == 1) {
                normalized.push_back(num[0]);
            } else {
                normalized.push_back('[');
                normalized.append(num);
                normalized.push_back(']');
            }
            group_has_content = true;
            i = j;
        } else {
            return {};
        }
    }
    return group_has_content ? normalized : std::string();
}

std::string format_surface_offset_token(float value)
{
    std::string out = float_to_string_decimal_point(clamp_surface_offset(value), 4);
    while (!out.empty() && out.back() == '0')
        out.pop_back();
    if (!out.empty() && out.back() == '.')
        out.pop_back();
    if (out == "-0")
        out = "0";
    return out.empty() ? std::string("0") : out;
}

std::string serialize_mixed_filaments(const std::vector<MixedFilament> &rows)
{
    std::ostringstream ss;
    ss.imbue(std::locale::classic());
    uint64_t next_stable_id = 1;
    bool     first          = true;
    for (MixedFilament mf : rows) {
        if (!first)
            ss << ';';
        first = false;
        disable_pointillism_mode(mf);
        if (mf.stable_id == 0)
            mf.stable_id = next_stable_id++;
        else if (mf.stable_id >= next_stable_id)
            next_stable_id = mf.stable_id + 1;
        const std::string normalized_ids = normalize_gradient_component_ids(mf.gradient_component_ids);
        const std::string normalized_weights = normalize_gradient_component_weights(mf.gradient_component_weights, decode_gradient_component_ids(normalized_ids, 0).size());
        ss << mf.component_a << ','
           << mf.component_b << ','
           << (mf.enabled ? 1 : 0) << ','
           << (mf.custom ? 1 : 0) << ','
           << clamp_int(mf.mix_b_percent, 0, 100) << ','
           << (mf.pointillism_all_filaments ? 1 : 0) << ','
           << 'g' << normalized_ids << ','
           << 'w' << normalized_weights << ','
           << 'm' << clamp_int(mf.distribution_mode, int(MixedFilament::LayerCycle), int(MixedFilament::Simple)) << ','
           << 'z' << std::max(0, mf.local_z_max_sublayers) << ','
           << "xa" << format_surface_offset_token(mf.component_a_surface_offset) << ','
           << "xb" << format_surface_offset_token(mf.component_b_surface_offset) << ','
           << 'd' << (mf.deleted ? 1 : 0) << ','
           << 'o' << (mf.origin_auto ? 1 : 0) << ','
           << 'u' << mf.stable_id;
        if (mf.ui_mode >= 0)
            ss << ",cm" << mf.ui_mode;
        if (mf.gradient_enabled)
            ss << ",r1/" << float_to_string_decimal_point(mf.gradient_start, 4) << '/'
               << float_to_string_decimal_point(mf.gradient_end, 4);
        const std::string normalized_pattern = normalize_manual_pattern(mf.manual_pattern);
        if (!normalized_pattern.empty())
            ss << ',' << normalized_pattern;
    }
    return ss.str();
}

MixedFilament mixed_filament_from_legacy_row(const LegacyMixedRow &r)
{
    MixedFilament mf;
    mf.component_a                = r.a;
    mf.component_b                = r.b;
    mf.stable_id                  = r.stable_id;
    mf.mix_b_percent              = r.mix_b_percent;
    mf.manual_pattern             = r.manual_pattern;
    mf.gradient_component_ids     = r.gradient_component_ids;
    mf.gradient_component_weights = r.gradient_component_weights;
    mf.pointillism_all_filaments  = false; // Snapmaker forces this off at load
    mf.distribution_mode          = r.distribution_mode;
    mf.local_z_max_sublayers      = r.local_z_max_sublayers;
    mf.gradient_enabled           = r.gradient_enabled;
    mf.gradient_start             = r.gradient_start;
    mf.gradient_end               = r.gradient_end;
    mf.component_a_surface_offset = r.surface_offset_a;
    mf.component_b_surface_offset = r.surface_offset_b;
    mf.enabled                    = r.enabled;
    mf.deleted                    = r.deleted;
    mf.custom                     = r.custom;
    mf.origin_auto                = r.origin_auto;
    mf.ui_mode                    = r.ui_mode;
    return mf;
}

std::vector<MixedFilament> parse_mixed_filaments(const std::string &serialized, size_t num_physical)
{
    const LegacyMixedParse     parsed = parse_legacy_mixed_definitions(serialized, num_physical);
    std::vector<MixedFilament> out;
    out.reserve(parsed.rows.size());
    for (const LegacyMixedRow &r : parsed.rows)
        out.emplace_back(mixed_filament_from_legacy_row(r));
    return out;
}

} // namespace Slic3r
