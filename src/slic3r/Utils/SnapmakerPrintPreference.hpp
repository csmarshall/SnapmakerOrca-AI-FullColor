#pragma once

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r {

// The per-print preferences Snapmaker Orca's send page applies by sending gcode lines to the printer before the print starts
// (resources/web/flutter_web/main.b347974c91352c2f.js: setPrePrintConfiguration ~39855, line builder ~148187, preference map ~40793).
// Nothing in the plate's G-code changes. Only these three fixed lines with a 0 or 1 value are ever produced or accepted.
//
// One "SET_PRINT_PREFERENCE KEY=<key> VALUE=<v>" line per key, joined with "\n" and no trailing newline, in the page's map order:
// bed_level, flow_calibrate, time_lapse_camera.
inline std::string build_snapmaker_print_preference_script(bool flow_calibrate, bool time_lapse, bool bed_level)
{
    auto line = [](const char* key, bool on) { return std::string("SET_PRINT_PREFERENCE KEY=") + key + " VALUE=" + (on ? "1" : "0"); };
    return line("bed_level", bed_level) + "\n" + line("flow_calibrate", flow_calibrate) + "\n" + line("time_lapse_camera", time_lapse);
}

// True only for a script that build_snapmaker_print_preference_script could have produced.
inline bool is_snapmaker_print_preference_script(const std::string& script)
{
    for (int bits = 0; bits < 8; ++bits)
        if (script == build_snapmaker_print_preference_script(bits & 1, bits & 2, bits & 4))
            return true;
    return false;
}

// Filament-to-extruder map lines Snapmaker Orca's send page emits BEFORE the preference lines
// (resources/web/flutter_web/main.b347974c91352c2f.js: setPrePrintConfiguration ~39855 builds them, the map comes from Gv.Xa ~150065
// and is stringified by aI9 ~152710, the unmapped guard is in us() ~151709).
// Our reading of that page:
//   * map key   = CONFIG_EXTRUDER = the project filament index, 0-based, ascending (Dart sorts the file's filament keys);
//   * map value = MAP_EXTRUDER    = the printer extruder the user picked, 0-based (the page stores nozzle.index - 1, nozzle.index is 1-based);
//   * only filaments with a used amount > 0 are in the map at all (Gk.gCn);
//   * SET_PRINT_USED_EXTRUDERS EXTRUDERS=<list> is the DISTINCT map values, comma separated with no spaces, in map order (Dart Set from the values);
//   * the page refuses to send with an empty map or a -1 value ("Please select filament type"), so no line is ever sent for an unmapped filament.
// Output: one "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=<k> MAP_EXTRUDER=<v>" line per used filament, then the USED_EXTRUDERS line, joined with
// "\n", no trailing newline. The preference lines follow as a separate script.
constexpr int snapmaker_max_filaments = 32;
constexpr int snapmaker_max_extruders = 4; // the U1

struct SnapmakerExtruderMapResult
{
    std::string script; // empty when no map lines are to be sent
    std::string error;  // non-empty when the mapping must not be sent: the print must be refused
    bool        ok() const { return error.empty(); }
};

// mapping[f] = extruder for project filament f, or -1 when unmapped. used[f] says whether filament f is used by the plate; an empty
// `used` means every filament in `mapping` is used. A used filament that is unmapped is an error, as Snapmaker's page refuses to send.
// An empty `mapping` is not an error and yields no lines (the legacy send path has no mapping UI and nothing is invented), unless
// `used_filament_count` is above 1: several used filaments with no mapping at all are refused.
inline SnapmakerExtruderMapResult build_snapmaker_extruder_map_script(const std::vector<int>& mapping, const std::vector<bool>& used = {},
                                                                      int used_filament_count = 0)
{
    SnapmakerExtruderMapResult r;
    if (mapping.empty()) {
        if (used_filament_count > 1)
            r.error = "No filament-to-extruder mapping was given for a plate that uses " + std::to_string(used_filament_count) + " filaments";
        return r;
    }
    if (mapping.size() > size_t(snapmaker_max_filaments)) {
        r.error = "Too many filaments in the extruder mapping: " + std::to_string(mapping.size());
        return r;
    }
    if (!used.empty() && used.size() != mapping.size()) {
        r.error = "The used-filament list does not match the extruder mapping";
        return r;
    }
    std::vector<std::pair<int, int>> pairs;
    std::vector<int>                 extruders; // distinct, first-seen order
    for (size_t f = 0; f < mapping.size(); ++f) {
        const bool is_used = used.empty() || used[f];
        const int  e       = mapping[f];
        if (e < -1 || e >= snapmaker_max_extruders) {
            r.error = "Filament " + std::to_string(f + 1) + " is mapped to an invalid extruder: " + std::to_string(e);
            return r;
        }
        if (!is_used)
            continue; // an unused filament is not in the page's map; whatever it holds is ignored
        if (e < 0) {
            r.error = "Filament " + std::to_string(f + 1) + " is not mapped to an extruder";
            return r;
        }
        pairs.emplace_back(int(f), e);
        bool seen = false;
        for (int x : extruders)
            seen = seen || x == e;
        if (!seen)
            extruders.push_back(e);
    }
    if (pairs.empty())
        return r;
    for (const auto& kv : pairs)
        r.script += "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=" + std::to_string(kv.first) + " MAP_EXTRUDER=" + std::to_string(kv.second) + "\n";
    r.script += "SET_PRINT_USED_EXTRUDERS EXTRUDERS=";
    for (size_t i = 0; i < extruders.size(); ++i)
        r.script += (i ? "," : "") + std::to_string(extruders[i]);
    return r;
}

// Parses PrintParams::ams_mapping, a JSON array with one integer per filament (the layout QidiPrinterAgent reads). An empty string or "[]"
// gives an empty mapping and true. Anything that is not an array of integers is rejected (false).
inline bool parse_snapmaker_ams_mapping(const std::string& ams_mapping, std::vector<int>& mapping)
{
    mapping.clear();
    if (ams_mapping.empty())
        return true;
    const auto j = nlohmann::json::parse(ams_mapping, nullptr, false);
    if (!j.is_array() || j.size() > size_t(snapmaker_max_filaments))
        return false;
    for (const auto& v : j) {
        if (!v.is_number_integer())
            return false;
        const long long x = v.get<long long>();
        if (x < -1 || x >= snapmaker_max_extruders)
            return false;
        mapping.push_back(int(x));
    }
    return true;
}

// True only for a script that build_snapmaker_extruder_map_script could have produced (every filament in it used).
inline bool is_snapmaker_extruder_map_script(const std::string& script)
{
    std::vector<int> mapping;
    size_t           pos = 0;
    for (size_t nl; (nl = script.find('\n', pos)) != std::string::npos; pos = nl + 1) {
        const std::string line = script.substr(pos, nl - pos);
        int               k = 0, v = 0, n = 0;
        if (std::sscanf(line.c_str(), "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=%d MAP_EXTRUDER=%d%n", &k, &v, &n) != 2 || n != int(line.size()) ||
            k < 0 || k >= snapmaker_max_filaments)
            return false;
        if (mapping.size() <= size_t(k))
            mapping.resize(size_t(k) + 1, -1);
        if (mapping[k] != -1)
            return false;
        mapping[k] = v;
    }
    if (mapping.empty())
        return false;
    std::vector<bool> used(mapping.size());
    for (size_t i = 0; i < mapping.size(); ++i)
        used[i] = mapping[i] >= 0;
    const SnapmakerExtruderMapResult r = build_snapmaker_extruder_map_script(mapping, used);
    return r.ok() && r.script == script;
}

} // namespace Slic3r
