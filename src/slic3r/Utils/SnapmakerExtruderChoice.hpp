#pragma once

// Pure resolver and validator for the filament-to-extruder choice of a legacy U1 send (#120 slice 1). No GUI, no network.
// Index convention: filament and extruder indices are 0-based, as in build_snapmaker_extruder_map_script and
// PrintParams::ams_mapping (one int per project filament, -1 = not used). That the printer reads the same base is INFERRED from the page.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

#include <nlohmann/json.hpp>

#include "SnapmakerPrintPreference.hpp"

namespace Slic3r {

// A filament the plate uses. spoolman_id < 0 = no link.
struct SnapmakerUsedFilament
{
    int         index = 0; // project filament index, 0-based
    std::string type;      // e.g. "PLA"
    std::string colour;    // "#RRGGBB" or "#RRGGBBAA"
    int         spoolman_id = -1;
};

// What the printer reports for one tool (RFID / Spoolman join data, already available as plain values). An empty type and colour = nothing known.
struct SnapmakerSlotInfo
{
    std::string type;
    std::string colour;
    int         spoolman_id = -1;
};

inline std::string snapmaker_upper(std::string s)
{
    for (char& c : s)
        c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Squared Euclidean distance in RGB (alpha ignored). Returns -1 for text that is not #RRGGBB or #RRGGBBAA.
inline long snapmaker_colour_distance(const std::string& a, const std::string& b)
{
    auto parse = [](const std::string& s, int rgb[3]) {
        if ((s.size() != 7 && s.size() != 9) || s[0] != '#')
            return false;
        for (size_t i = 1; i < s.size(); ++i)
            if (!std::isxdigit(static_cast<unsigned char>(s[i])))
                return false;
        for (int c = 0; c < 3; ++c)
            rgb[c] = int(std::stoi(s.substr(1 + 2 * size_t(c), 2), nullptr, 16));
        return true;
    };
    int x[3], y[3];
    if (!parse(a, x) || !parse(b, y))
        return -1;
    long d = 0;
    for (int c = 0; c < 3; ++c)
        d += long(x[c] - y[c]) * long(x[c] - y[c]);
    return d;
}

struct SnapmakerExtruderSuggestion
{
    std::vector<int> map;             // per project filament (size = highest used index + 1), -1 = not used
    bool             oversubscribed = false; // more used filaments than extruders: some extruder is shared
};

// Suggested pre-fill: for every (filament, slot) pair the cost is ordered (spool link differs, type differs, colour distance, not the identity
// slot, slot number); pairs are taken cheapest first, each filament and each slot once. Filaments left over (more used than slots) take their
// cheapest slot regardless and oversubscribed is set. Deterministic: ties fall to the lower filament index, then the lower slot.
// Unknown colour/type count as the worst match, so with no printer data the result is the identity.
inline SnapmakerExtruderSuggestion suggest_snapmaker_extruder_map(const std::vector<SnapmakerUsedFilament>& used, const std::vector<SnapmakerSlotInfo>& slots)
{
    SnapmakerExtruderSuggestion out;
    if (used.empty() || slots.empty())
        return out;
    int max_index = -1;
    for (const auto& f : used)
        max_index = std::max(max_index, f.index);
    if (max_index < 0)
        return out;
    out.map.assign(size_t(max_index) + 1, -1);

    using Cost = std::tuple<int, int, long, int, int, int, int>; // link, type, colour, identity, filament index, slot, filament position
    struct Pair { Cost cost; size_t f; int s; };
    std::vector<Pair> pairs;
    for (size_t f = 0; f < used.size(); ++f) {
        if (used[f].index < 0)
            continue;
        for (size_t s = 0; s < slots.size(); ++s) {
            const auto& u  = used[f];
            const auto& sl = slots[s];
            const bool  link_match = u.spoolman_id >= 0 && u.spoolman_id == sl.spoolman_id;
            const bool  type_match = !u.type.empty() && snapmaker_upper(u.type) == snapmaker_upper(sl.type);
            const long  d          = snapmaker_colour_distance(u.colour, sl.colour);
            pairs.push_back({Cost{link_match ? 0 : 1, type_match ? 0 : 1, d < 0 ? 3 * 255 * 255 + 1 : d, u.index == int(s) ? 0 : 1, u.index, int(s), int(f)}, f, int(s)});
        }
    }
    std::stable_sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.cost < b.cost; });
    std::vector<bool> slot_taken(slots.size(), false);
    std::vector<bool> done(used.size(), false);
    for (const auto& p : pairs) {
        if (done[p.f] || slot_taken[size_t(p.s)])
            continue;
        done[p.f]                       = true;
        slot_taken[size_t(p.s)]         = true;
        out.map[size_t(used[p.f].index)] = p.s;
    }
    for (const auto& p : pairs) { // leftovers share their cheapest slot
        if (done[p.f])
            continue;
        done[p.f]                        = true;
        out.map[size_t(used[p.f].index)] = p.s;
        out.oversubscribed               = true;
    }
    return out;
}

enum class SnapmakerMapRefusal { None, Unchosen, InvalidIndex, Unmapped, OutOfRange, Duplicate };

struct SnapmakerMapValidation
{
    SnapmakerMapRefusal refusal        = SnapmakerMapRefusal::None;
    std::string         message;               // user-facing, set when refused
    bool                oversubscribed = false; // OK, but more used filaments than extruders, so some extruder is shared
    bool                ok() const { return refusal == SnapmakerMapRefusal::None; }
};

// map[f] = extruder for project filament f (-1 or missing = not chosen). Entries of unused filaments are ignored. An empty map, or one that
// chooses nothing, is "unchosen": the start is refused and nothing is guessed. Two used filaments on one extruder are refused unless there are
// more used filaments than extruders, and then every extruder must still be used as far as it can be (sharing only where forced).
inline SnapmakerMapValidation validate_snapmaker_extruder_map(const std::vector<int>& map, const std::vector<SnapmakerUsedFilament>& used,
                                                              const std::vector<SnapmakerSlotInfo>& slots)
{
    SnapmakerMapValidation v;
    auto refuse = [&v](SnapmakerMapRefusal r, std::string m) {
        v.refusal = r;
        v.message = std::move(m);
        return v;
    };
    const int extruders = std::min<int>(int(slots.size()), snapmaker_max_extruders);
    if (used.empty())
        return v;
    bool any_chosen = false;
    for (int e : map)
        any_chosen = any_chosen || e >= 0;
    if (!any_chosen)
        return refuse(SnapmakerMapRefusal::Unchosen, "No filament-to-extruder assignment was chosen. Choose an extruder for each filament and confirm before printing.");
    std::vector<int> count(size_t(std::max(extruders, 0)), 0);
    for (const auto& f : used) {
        if (f.index < 0 || f.index >= snapmaker_max_filaments)
            return refuse(SnapmakerMapRefusal::InvalidIndex, "Filament " + std::to_string(f.index + 1) + " is not a valid filament number.");
        const int e = size_t(f.index) < map.size() ? map[size_t(f.index)] : -1;
        if (e < 0)
            return refuse(SnapmakerMapRefusal::Unmapped, "Please select an extruder for filament " + std::to_string(f.index + 1) + ".");
        if (e >= extruders)
            return refuse(SnapmakerMapRefusal::OutOfRange, "Filament " + std::to_string(f.index + 1) + " is assigned to extruder " + std::to_string(e + 1) + ", which this printer does not have.");
        ++count[size_t(e)];
    }
    const int used_count = int(used.size());
    int       distinct   = 0;
    for (int c : count)
        distinct += c > 0 ? 1 : 0;
    if (distinct < std::min(used_count, extruders))
        return refuse(SnapmakerMapRefusal::Duplicate, "Two filaments are assigned to the same extruder. Give each filament its own extruder.");
    v.oversubscribed = used_count > extruders;
    return v;
}

struct SnapmakerAmsMappingResult
{
    std::string json;  // PrintParams::ams_mapping: "[2,0,3]"; empty when refused
    std::string error; // user-facing, set when refused
    bool        ok() const { return error.empty(); }
};

// Converts a confirmed, valid map into PrintParams::ams_mapping. Unused filaments become -1. A map that does not validate gives no JSON
// (so an unchosen map can never reach the agent as an identity or empty mapping).
inline SnapmakerAmsMappingResult snapmaker_map_to_ams_mapping(const std::vector<int>& map, const std::vector<SnapmakerUsedFilament>& used,
                                                              const std::vector<SnapmakerSlotInfo>& slots)
{
    SnapmakerAmsMappingResult r;
    const SnapmakerMapValidation v = validate_snapmaker_extruder_map(map, used, slots);
    if (!v.ok() || used.empty()) {
        r.error = v.ok() ? "No filament is used by this plate." : v.message;
        return r;
    }
    int max_index = 0;
    for (const auto& f : used)
        max_index = std::max(max_index, f.index);
    std::vector<int> out(size_t(max_index) + 1, -1);
    for (const auto& f : used)
        out[size_t(f.index)] = map[size_t(f.index)];
    r.json = nlohmann::json(out).dump();
    return r;
}

} // namespace Slic3r
