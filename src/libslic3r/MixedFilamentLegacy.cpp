#include "MixedFilamentLegacy.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <set>
#include <sstream>
#include <unordered_set>

#include <boost/log/trivial.hpp>
#include <fast_float/fast_float.h>

#include "Config.hpp"
#include "FilamentMixer.hpp"
#include "LocalesUtils.hpp"
#include "PrintConfig.hpp"
#include "Preset.hpp"

namespace Slic3r {

namespace {

constexpr float k_min_gradient_difference = 0.05f; // Snapmaker MixedFilament::k_min_gradient_difference

std::string trim_copy(const std::string &s)
{
    size_t lo = 0, hi = s.size();
    while (lo < hi && std::isspace(static_cast<unsigned char>(s[lo])))
        ++lo;
    while (hi > lo && std::isspace(static_cast<unsigned char>(s[hi - 1])))
        --hi;
    return s.substr(lo, hi - lo);
}

bool parse_int_token(const std::string &tok, int &out)
{
    const std::string t = trim_copy(tok);
    if (t.empty())
        return false;
    try {
        size_t consumed = 0;
        const int v = std::stoi(t, &consumed);
        if (consumed != t.size())
            return false;
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_uint64_token(const std::string &tok, uint64_t &out)
{
    const std::string t = trim_copy(tok);
    if (t.empty())
        return false;
    try {
        size_t consumed = 0;
        const unsigned long long v = std::stoull(t, &consumed);
        if (consumed != t.size())
            return false;
        out = uint64_t(v);
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_float_token(const std::string &tok, float &out)
{
    const std::string t = trim_copy(tok);
    if (t.empty())
        return false;
    const char *first = t.data();
    const char *last  = first + t.size();
    if (*first == '+') {
        if (++first == last || *first == '-' || *first == '+')
            return false;
    }
    float      value  = 0.f;
    const auto result = fast_float::from_chars(first, last, value);
    if (result.ec != std::errc() || result.ptr != last || !std::isfinite(value))
        return false;
    out = value;
    return true;
}

float clamp_surface_offset(float v) { return std::clamp(v, -2.f, 2.f); }

// Snapmaker decode_gradient_component_ids with no validation cap: "/"-separated decimals, or
// concatenated single digits in the legacy form. Duplicates are dropped.
std::vector<unsigned int> decode_gradient_ids(const std::string &components)
{
    std::vector<unsigned int> ids;
    if (components.empty())
        return ids;
    std::unordered_set<unsigned int> seen;
    if (components.find('/') != std::string::npos) {
        std::string token;
        auto flush = [&]() {
            if (token.empty())
                return;
            const unsigned int id = unsigned(std::strtoul(token.c_str(), nullptr, 10));
            if (id >= 1 && seen.insert(id).second)
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
            if (seen_legacy[id])
                continue;
            seen_legacy[id] = true;
            ids.emplace_back(id);
        }
    }
    return ids;
}

// Snapmaker normalize_manual_pattern: digits 1-9, "[NN]" for 10..99, "," group separator; empty if invalid.
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

std::vector<std::string> split_groups(const std::string &pattern)
{
    std::vector<std::string> groups;
    std::string              current;
    for (const char c : pattern) {
        if (c == ',') {
            if (!current.empty())
                groups.emplace_back(std::move(current));
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty())
        groups.emplace_back(std::move(current));
    return groups;
}

std::vector<std::string> tokenize_group(const std::string &group)
{
    std::vector<std::string> tokens;
    for (size_t i = 0; i < group.size(); ++i) {
        const char c = group[i];
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

int mix_percent_from_pattern(const std::string &pattern)
{
    const std::vector<std::string> groups = split_groups(pattern);
    if (groups.empty())
        return 50;
    double blend_b = 0.0;
    for (const std::string &group : groups) {
        const std::vector<std::string> tokens = tokenize_group(group);
        if (tokens.empty())
            continue;
        const int count_b = int(std::count(tokens.begin(), tokens.end(), "2"));
        blend_b += double(count_b) / double(tokens.size());
    }
    return std::clamp(int(std::lround(100.0 * blend_b / double(groups.size()))), 0, 100);
}

bool references_exceed_physical(const std::string &gradient_ids, const std::string &manual_pattern, size_t n)
{
    for (unsigned int id : decode_gradient_ids(gradient_ids))
        if (id > n)
            return true;
    for (const std::string &group : split_groups(manual_pattern))
        for (const std::string &token : tokenize_group(group)) {
            if (token == "1" || token == "2")
                continue;
            char *              end = nullptr;
            const unsigned long val = std::strtoul(token.c_str(), &end, 10);
            if (*end == '\0' && val > n)
                return true;
        }
    return false;
}

uint64_t pair_key(unsigned a, unsigned b)
{
    return (uint64_t(std::min(a, b)) << 32) | uint64_t(std::max(a, b));
}

} // namespace

bool parse_legacy_mixed_row(const std::string &row, LegacyMixedRow &out)
{
    std::vector<std::string> tokens;
    {
        std::stringstream ss(row);
        std::string       token;
        while (std::getline(ss, token, ','))
            tokens.emplace_back(trim_copy(token));
    }
    if (tokens.size() < 4)
        return false;

    int values[5] = { 0, 0, 1, 1, 50 };
    if (tokens.size() == 4) {
        // Legacy: a,b,enabled,mix
        if (!parse_int_token(tokens[0], values[0]) || !parse_int_token(tokens[1], values[1]) ||
            !parse_int_token(tokens[2], values[2]) || !parse_int_token(tokens[3], values[4]))
            return false;
    } else {
        for (size_t i = 0; i < 5; ++i)
            if (!parse_int_token(tokens[i], values[i]))
                return false;
    }
    if (values[0] <= 0 || values[1] <= 0)
        return false;

    LegacyMixedRow r;
    r.a             = unsigned(values[0]);
    r.b             = unsigned(values[1]);
    r.enabled       = values[2] != 0;
    r.custom        = (tokens.size() == 4) ? true : (values[3] != 0);
    r.origin_auto   = !r.custom;
    r.mix_b_percent = std::clamp(values[4], 0, 100);

    size_t      token_idx = 5;
    std::string manual_pattern;
    if (tokens.size() >= 6) {
        const std::string &legacy = tokens[5];
        if (legacy == "0" || legacy == "1") {
            r.pointillism = (legacy == "1");
            token_idx     = 6;
        } else if (legacy.empty() || legacy[0] == 'g' || legacy[0] == 'G' || legacy[0] == 'm' || legacy[0] == 'M' ||
                   legacy[0] == 'r' || legacy[0] == 'R') {
            token_idx = 5;
        } else {
            manual_pattern = legacy;
            token_idx      = 6;
        }
    }

    std::vector<std::string> pattern_tokens;
    if (!manual_pattern.empty())
        pattern_tokens.push_back(manual_pattern);
    for (size_t i = token_idx; i < tokens.size(); ++i) {
        const std::string &tok = tokens[i];
        if (tok.empty())
            continue;
        const char k = tok[0];
        if (k == 'g' || k == 'G') {
            r.gradient_component_ids = tok.substr(1);
            continue;
        }
        if (k == 'w' || k == 'W') {
            r.gradient_component_weights = tok.substr(1);
            continue;
        }
        if (k == 'm' || k == 'M') {
            int parsed = r.distribution_mode;
            if (parse_int_token(tok.substr(1), parsed))
                r.distribution_mode = std::clamp(parsed, 0, 2);
            continue;
        }
        if (k == 'z' || k == 'Z') {
            int parsed = r.local_z_max_sublayers;
            if (parse_int_token(tok.substr(1), parsed))
                r.local_z_max_sublayers = std::max(0, parsed);
            continue;
        }
        if ((k == 'x' || k == 'X') && tok.size() >= 3) {
            const char component = char(std::tolower(static_cast<unsigned char>(tok[1])));
            if (component == 'a' || component == 'b') {
                float parsed = component == 'a' ? r.surface_offset_a : r.surface_offset_b;
                if (parse_float_token(tok.substr(2), parsed))
                    (component == 'a' ? r.surface_offset_a : r.surface_offset_b) = clamp_surface_offset(parsed);
                continue;
            }
        }
        if (k == 'd' || k == 'D') {
            int parsed = r.deleted ? 1 : 0;
            if (parse_int_token(tok.substr(1), parsed))
                r.deleted = parsed != 0;
            continue;
        }
        if (k == 'o' || k == 'O') {
            int parsed = r.origin_auto ? 1 : 0;
            if (parse_int_token(tok.substr(1), parsed))
                r.origin_auto = parsed != 0;
            continue;
        }
        if (k == 'u' || k == 'U') {
            uint64_t parsed = r.stable_id;
            if (parse_uint64_token(tok.substr(1), parsed))
                r.stable_id = parsed;
            continue;
        }
        if ((k == 'c' || k == 'C') && tok.size() >= 3 && (tok[1] == 'm' || tok[1] == 'M')) {
            int v = r.ui_mode;
            if (parse_int_token(tok.substr(2), v))
                r.ui_mode = std::clamp(v, -1, 3);
            continue;
        }
        if (k == 'r' || k == 'R') {
            const std::string body = tok.substr(1);
            const size_t      s1   = body.find('/');
            const size_t      s2   = (s1 == std::string::npos) ? std::string::npos : body.find('/', s1 + 1);
            if (s1 != std::string::npos && s2 != std::string::npos) {
                int   flag  = r.gradient_enabled ? 1 : 0;
                float start = r.gradient_start, end = r.gradient_end;
                if (parse_int_token(body.substr(0, s1), flag) && parse_float_token(body.substr(s1 + 1, s2 - s1 - 1), start) &&
                    parse_float_token(body.substr(s2 + 1), end)) {
                    r.gradient_enabled = flag != 0;
                    if (start > 0.f && start < 1.f)
                        r.gradient_start = start;
                    if (end > 0.f && end < 1.f)
                        r.gradient_end = end;
                }
            }
            continue;
        }
        pattern_tokens.push_back(tok);
    }
    if (!pattern_tokens.empty()) {
        std::string joined;
        for (size_t i = 0; i < pattern_tokens.size(); ++i) {
            if (i != 0)
                joined += ',';
            joined += pattern_tokens[i];
        }
        manual_pattern = joined;
    }
    r.manual_pattern = manual_pattern;

    // Snapmaker forces pointillism off at load; remember that it was asked for so it can be reported.
    if (r.distribution_mode == 1) {
        r.pointillism = true;
        r.distribution_mode = decode_gradient_ids(r.gradient_component_ids).size() >= 3 ? 0 : 2;
    }

    if (r.gradient_enabled) {
        r.gradient_start = std::clamp(r.gradient_start, 0.01f, 0.99f);
        r.gradient_end   = std::clamp(r.gradient_end, 0.01f, 0.99f);
        if (std::abs(r.gradient_start - r.gradient_end) < k_min_gradient_difference)
            r.gradient_enabled = false;
    }
    out = std::move(r);
    return true;
}

LegacyMixedParse parse_legacy_mixed_definitions(const std::string &serialized, size_t n)
{
    LegacyMixedParse result;
    if (serialized.empty() || n < 2 || n > kLegacyMixedMaxFilaments)
        return result;

    // Parse and validate every row first; auto rows are only honoured when the string shows the
    // auto-generate preference was on (see header).
    std::vector<LegacyMixedRow> accepted;
    std::set<uint64_t>          consumed_auto_pairs;
    {
        std::stringstream all(serialized);
        std::string       row;
        while (std::getline(all, row, ';')) {
            if (row.empty())
                continue;
            if (result.parsed >= kLegacyMixedMaxRows) {
                ++result.skipped;
                continue;
            }
            ++result.parsed;
            LegacyMixedRow r;
            if (!parse_legacy_mixed_row(row, r)) {
                ++result.skipped;
                if (result.skipped <= 10) BOOST_LOG_TRIVIAL(warning) << "Legacy mixed filament row malformed, skipped: " << row;
                continue;
            }
            if (r.a > n || r.b > n || r.a == r.b) {
                ++result.skipped;
                if (result.skipped <= 10) BOOST_LOG_TRIVIAL(warning) << "Legacy mixed filament row rejected (components out of range or equal): " << row;
                continue;
            }
            const std::string norm_pattern = normalize_manual_pattern(r.manual_pattern);
            if (references_exceed_physical(r.gradient_component_ids, norm_pattern, n)) {
                ++result.skipped;
                if (result.skipped <= 10) BOOST_LOG_TRIVIAL(warning) << "Legacy mixed filament row rejected (gradient/pattern ids exceed physical count): " << row;
                continue;
            }
            r.manual_pattern = norm_pattern;
            if (!norm_pattern.empty())
                r.mix_b_percent = mix_percent_from_pattern(norm_pattern);
            if (!r.custom) {
                if (!consumed_auto_pairs.insert(pair_key(r.a, r.b)).second) {
                    ++result.skipped;
                    continue;
                }
                const unsigned lo = std::min(r.a, r.b), hi = std::max(r.a, r.b);
                r.a = lo;
                r.b = hi;
                r.origin_auto = true;
                result.auto_generated = true;
            }
            if (r.deleted)
                r.enabled = false;
            accepted.emplace_back(std::move(r));
        }
    }

    for (LegacyMixedRow &r : accepted) {
        if (!r.custom && !result.auto_generated)
            continue; // unreachable: auto_generated is set by any auto row
        result.rows.emplace_back(std::move(r));
    }

    if (result.auto_generated) {
        // Snapmaker appends any C(N,2) pair the string lacks, enabled, at the end.
        for (unsigned i = 1; i <= n; ++i)
            for (unsigned j = i + 1; j <= n; ++j) {
                if (consumed_auto_pairs.count(pair_key(i, j)))
                    continue;
                LegacyMixedRow r;
                r.a = i;
                r.b = j;
                r.custom = false;
                r.origin_auto = true;
                result.rows.emplace_back(std::move(r));
            }
    }
    return result;
}

namespace {

// Rebuilds a vector option so entry i is a copy of the old entry src[i]; the old values are cloned first.
void rebuild_vector(ConfigOptionVectorBase *opt, const std::vector<size_t> &src)
{
    std::unique_ptr<ConfigOption> old(opt->clone());
    opt->resize(src.size(), old.get());
    for (size_t i = 0; i < src.size(); ++i)
        opt->set_at(old.get(), i, src[i]);
}

} // namespace

LegacyMixedConversion convert_legacy_mixed_filaments(DynamicPrintConfig &config, const std::string &serialized)
{
    LegacyMixedConversion out;
    if (serialized.empty())
        return out;

    auto *colours = config.option<ConfigOptionStrings>("filament_colour");
    if (colours == nullptr || colours->values.size() < 2)
        return out;
    const size_t n = colours->values.size();
    if (n > kLegacyMixedMaxFilaments) {
        out.warnings.push_back("This project has " + std::to_string(n) + " filaments; Snapmaker mixed filament definitions are only converted up to " +
                               std::to_string(kLegacyMixedMaxFilaments) + ", so they were ignored.");
        return out;
    }

    if (const auto *is_mixed = config.option<ConfigOptionBools>("filament_is_mixed");
        is_mixed != nullptr && std::any_of(is_mixed->values.begin(), is_mixed->values.end(), [](unsigned char v) { return v != 0; })) {
        out.warnings.push_back("This project carries both Snapmaker mixed filament definitions and mixed filament slots; the slots were kept and the Snapmaker definitions ignored.");
        return out;
    }

    const LegacyMixedParse parsed = parse_legacy_mixed_definitions(serialized, n);
    out.rows_skipped              = parsed.skipped;
    if (parsed.skipped > 0)
        out.warnings.push_back(std::to_string(parsed.skipped) + " Snapmaker mixed filament row(s) were malformed or referenced missing filaments and were skipped.");

    // Slots to append, in Snapmaker virtual-id order: enabled, non-deleted rows only.
    std::vector<const LegacyMixedRow *> slots;
    for (const LegacyMixedRow &r : parsed.rows)
        if (r.enabled && !r.deleted)
            slots.push_back(&r);
    if (slots.empty())
        return out;
    if (n + slots.size() > kLegacyMixedMaxFilaments) {
        out.warnings.push_back("The Snapmaker project defines " + std::to_string(slots.size()) + " mixed filaments but only " +
                               std::to_string(kLegacyMixedMaxFilaments - n) + " more slots fit (limit " + std::to_string(kLegacyMixedMaxFilaments) +
                               "); the rest were dropped, so objects using them fall back to filament 1.");
        slots.resize(kLegacyMixedMaxFilaments - n);
        if (slots.empty())
            return out;
    }

    // Per-slot text for the new slots.
    struct NewSlot { std::string colour, components, ratios, range; bool gradient; };
    std::vector<NewSlot> news;
    for (size_t k = 0; k < slots.size(); ++k) {
        const LegacyMixedRow &r    = *slots[k];
        NewSlot               s;
        s.components = format_mixed_components({ r.a, r.b });
        // Snapmaker resolves an auto row without a pattern at a forced 1:1 cadence (apply_gradient_settings),
        // whatever mix_b_percent the string carries.
        int mix = (r.custom || !r.manual_pattern.empty()) ? r.mix_b_percent : 50;
        if (mix < 1 || mix > 99) {
            // The 2.5 ratio validator needs both ratios above zero.
            mix = std::clamp(mix, 1, 99);
            out.warnings.push_back("Snapmaker mixed filament " + std::to_string(n + k + 1) + " (filaments " + std::to_string(r.a) + "+" + std::to_string(r.b) +
                                   ") is a pure single-filament mix; it was stored as a " + std::to_string(100 - mix) + "/" + std::to_string(mix) + " blend.");
        }
        s.ratios     = format_mixed_ratios({ 100 - mix, mix });
        s.gradient   = r.gradient_enabled;
        if (r.gradient_enabled)
            s.range = float_to_string_decimal_point(r.gradient_start, 4) + "," + float_to_string_decimal_point(r.gradient_end, 4);
        s.colour = blend_color(colours->values[r.a - 1], colours->values[r.b - 1], float(mix) / 100.f);
        if (s.colour.empty())
            s.colour = colours->values[r.a - 1];
        news.push_back(std::move(s));

        std::vector<std::string> dropped;
        if (!r.manual_pattern.empty())
            dropped.push_back("manual pattern");
        if (r.distribution_mode != 2 && decode_gradient_ids(r.gradient_component_ids).size() >= 3) // Snapmaker ignores the ids in simple mode
            dropped.push_back("3+ colour gradient");
        if (std::abs(r.surface_offset_a) > 1e-6f || std::abs(r.surface_offset_b) > 1e-6f)
            dropped.push_back("surface offsets");
        if (r.local_z_max_sublayers > 0)
            dropped.push_back("Local-Z sublayer cap");
        if (r.pointillism)
            dropped.push_back("pointillism");
        if (!dropped.empty()) {
            std::string list;
            for (size_t i = 0; i < dropped.size(); ++i)
                list += (i ? ", " : "") + dropped[i];
            out.warnings.push_back("Snapmaker mixed filament " + std::to_string(n + k + 1) + " (filaments " + std::to_string(r.a) + "+" +
                                   std::to_string(r.b) + ") uses " + list + ", which this build cannot represent; it was converted to a " +
                                   std::to_string(100 - mix) + "/" + std::to_string(mix) + " blend of the two filaments.");
        }
    }

    // Extend every per-filament vector. Layouts: one value per filament; one value per filament
    // variant (filament_self_index maps variants to filaments); [print, filaments..., printer] groups.
    const size_t total = n + slots.size();
    const auto  *self_idx = config.option<ConfigOptionInts>("filament_self_index");
    std::vector<int> self_index_values;
    if (self_idx != nullptr)
        self_index_values = self_idx->values;
    const size_t variant_count = self_index_values.size();
    // Variant layout only when filament_self_index and filament_extruder_variant agree in length and every
    // value names an existing filament; anything else is treated as one value per filament.
    const auto *variant_names = config.option<ConfigOptionStrings>("filament_extruder_variant");
    bool        has_variants  = variant_count > n && variant_names != nullptr && variant_names->size() == variant_count;
    if (has_variants)
        for (int v : self_index_values)
            if (v < 1 || size_t(v) > n) {
                has_variants = false;
                break;
            }

    static const std::set<std::string> project_keys = {
        "filament_colour", "filament_multi_colour", "filament_colour_type", "filament_map", "filament_nozzle_map",
        "filament_volume_map", "filament_settings_id", "filament_ids", "filament_is_mixed", "filament_mixed_components",
        "filament_mixed_sublayer_ratios", "filament_mixed_gradient", "filament_mixed_gradient_range",
        "filament_mixed_gradient_curve", "filament_mixed_gradient_per_part", "compatible_process_expression_group" };
    std::set<std::string> keys(project_keys);
    for (const std::string &k : Preset::filament_options())
        keys.insert(k);

    // Per-slot source filament (0-based) for each new slot: the first component.
    std::vector<size_t> src_slot;
    for (const LegacyMixedRow *r : slots)
        src_slot.push_back(r->a - 1);

    // Check every vector first, so a malformed project is left exactly as loaded rather than half extended.
    auto is_variant_key_of = [](const std::string &key) {
        return filament_options_with_variant.count(key) != 0 || key == "filament_extruder_variant" || key == "filament_self_index";
    };
    std::string inconsistent;
    if (has_variants)
        for (size_t s : src_slot)
            if (std::find(self_index_values.begin(), self_index_values.end(), int(s + 1)) == self_index_values.end())
                inconsistent = "filament_self_index";
    for (const std::string &key : keys) {
        if (key == "compatible_printers" || key == "compatible_prints" || !inconsistent.empty())
            continue;
        const auto *opt = dynamic_cast<const ConfigOptionVectorBase *>(config.option(key));
        if (opt == nullptr)
            continue;
        const size_t sz = opt->size();
        if (sz <= 1 || sz == n || (has_variants && is_variant_key_of(key) && sz == variant_count))
            continue;
        inconsistent = key;
    }
    if (!inconsistent.empty()) {
        out.warnings.push_back("The Snapmaker mixed filaments could not be converted: the per-filament setting \"" + inconsistent +
                               "\" does not match the filament count. The project was loaded without them.");
        return out;
    }

    for (const std::string &key : keys) {
        if (key == "compatible_printers" || key == "compatible_prints")
            continue;
        auto *opt = dynamic_cast<ConfigOptionVectorBase *>(config.option(key));
        if (opt == nullptr)
            continue;
        const bool is_variant_key = filament_options_with_variant.count(key) != 0 || key == "filament_extruder_variant" || key == "filament_self_index";
        if (has_variants && is_variant_key && opt->size() == variant_count) {
            std::vector<size_t> src;
            for (size_t i = 0; i < variant_count; ++i)
                src.push_back(i);
            for (size_t s : src_slot)
                for (size_t i = 0; i < variant_count; ++i)
                    if (size_t(self_index_values[i]) == s + 1)
                        src.push_back(i);
            rebuild_vector(opt, src);
        } else if (opt->size() == n) {
            std::vector<size_t> src;
            for (size_t i = 0; i < n; ++i)
                src.push_back(i);
            for (size_t s : src_slot)
                src.push_back(s);
            rebuild_vector(opt, src);
        }
    }
    if (has_variants) {
        // The new slots' variants belong to the new filament numbers.
        // Rebuilt by push_back from the validated copy: no write depends on the option's current size.
        if (auto *si = config.option<ConfigOptionInts>("filament_self_index")) {
            si->values = self_index_values;
            for (size_t k = 0; k < slots.size(); ++k)
                for (size_t i = 0; i < variant_count; ++i)
                    if (size_t(self_index_values[i]) == src_slot[k] + 1)
                        si->values.push_back(int(n + k + 1));
        }
    }
    // [print, filament 1..N, printer] groups: the new slots go before the printer entry.
    for (const char *key : { "inherits_group", "compatible_machine_expression_group", "different_settings_to_system" }) {
        auto *opt = dynamic_cast<ConfigOptionVectorBase *>(config.option(key));
        if (opt == nullptr || opt->size() != n + 2)
            continue;
        std::vector<size_t> src;
        for (size_t i = 0; i <= n; ++i)
            src.push_back(i);
        for (size_t s : src_slot)
            src.push_back(s + 1);
        src.push_back(n + 1);
        rebuild_vector(opt, src);
    }

    // Mixed metadata: the arrays may be absent or short on a Snapmaker file; size them, then fill the new slots.
    resize_mixed_filament_metadata(config, std::min(n, total), total);
    auto *is_mixed   = config.option<ConfigOptionBools>("filament_is_mixed", true);
    auto *comps      = config.option<ConfigOptionStrings>("filament_mixed_components", true);
    auto *ratios     = config.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios", true);
    auto *gradient   = config.option<ConfigOptionBools>("filament_mixed_gradient", true);
    auto *range      = config.option<ConfigOptionStrings>("filament_mixed_gradient_range", true);
    auto *colour     = config.option<ConfigOptionStrings>("filament_colour", true);
    auto *multi      = config.option<ConfigOptionStrings>("filament_multi_colour");
    auto *colour_typ = config.option<ConfigOptionStrings>("filament_colour_type");
    for (size_t k = 0; k < news.size(); ++k) {
        const size_t i = n + k;
        is_mixed->values[i]  = 1;
        comps->values[i]     = news[k].components;
        ratios->values[i]    = news[k].ratios;
        gradient->values[i]  = news[k].gradient ? 1 : 0;
        range->values[i]     = news[k].range;
        colour->values[i]    = news[k].colour;
        if (multi != nullptr && i < multi->values.size())
            multi->values[i] = news[k].colour;
        if (colour_typ != nullptr && i < colour_typ->values.size())
            colour_typ->values[i] = "1";
    }
    out.slots_added = slots.size();
    return out;
}

} // namespace Slic3r
