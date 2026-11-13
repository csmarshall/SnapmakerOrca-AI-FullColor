#pragma once

// The JSON Snapmaker's pre-print web page reads through sw_GetFileFilamentMapping, built from plain data so it needs no plate, no wx and no
// printer. Forklifted from Snapmaker/OrcaSlicer (branch main) SSWCP::build_filament_mapping_json, src/slic3r/GUI/SSWCP.cpp ~2531, with the
// helpers it uses (preprint_color_to_int, FilamentColor::FromMultiColors / BuildPreprintColorMultiItem, SSWCPProtocol::append_flow_type_fields).
//
// Differences from Snapmaker, on purpose:
//   * input is a struct the caller fills from the plate (the later dialog), not a read of wxGetApp();
//   * `filename` is the display name only; there is no path read, no thumbnail file load (thumbnails arrive as base64 data) and `filepath`
//     is emitted only when the caller supplies one;
//   * strict bounds and finite-number checks (Snapmaker has none); a violation makes the builder fail rather than emit a partial object;
//   * high-flow nozzles do not exist in this tree: filament_volume_type / nozzle_volume_types are "standard" unless the caller says
//     "high_flow" for a nozzle.
// Index convention (#118, SnapmakerPrintPreference.hpp): filament index is the project filament index, 0-based; the extruder is the
// printer extruder, 0-based. filament_extruder_map is emitted the way Snapmaker emits it: an object of decimal strings, "<filament>": "<extruder>".

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "SnapmakerPrintPreference.hpp"

namespace Slic3r {

constexpr int         snapmaker_preprint_max_thumbnails      = 8;
constexpr std::size_t snapmaker_preprint_max_thumbnail_bytes = 2 * 1024 * 1024; // of base64 text, per thumbnail
constexpr std::size_t snapmaker_preprint_max_name_bytes      = 512;

struct SnapmakerPreprintThumbnail
{
    std::string png_base64; // the base64 text only, no "data:" prefix
    double      width  = 0;
    double      height = 0;
};

struct SnapmakerPreprintPlate
{
    double estimated_time_s = 0; // normal-mode print time, seconds
    // One entry per project filament (index 0-based). colour is "#RRGGBB" or "#RRGGBBAA".
    std::vector<std::string> filament_colour;
    std::vector<std::string> filament_multi_colors; // optional, "|"-separated colours per filament (config filament_multi_colors)
    std::vector<int>         filament_colour_mode;  // optional, config filament_colour_mode: 0 segment, anything else gradient
    std::vector<std::string> filament_type;
    std::vector<double>      filament_density;      // g/cm3
    std::vector<double>      filament_diameter;     // mm
    std::map<int, double>    volume_mm3;            // used filament volume per filament index
    std::vector<double>      nozzle_diameter;       // one per printer extruder
    std::vector<std::string> nozzle_volume_types;   // optional, "standard" / "high_flow" per extruder
    std::map<int, int>       filament_extruder_map; // filament index (0-based) -> extruder (0-based); empty = key omitted
    std::string              machine_model;
    std::string              display_name;          // the file name the page shows
    std::string              filepath;              // optional; omitted from the JSON when empty
    std::vector<SnapmakerPreprintThumbnail> thumbnails;
};

namespace preprint_detail {

// Snapmaker preprint_color_to_int: "#RRGGBB" or "#RRGGBBAA" (7 or 9 chars) -> its hex value as a number; anything else 0.
inline long long color_to_int(const std::string& color)
{
    if ((color.size() != 7 && color.size() != 9) || color.front() != '#')
        return 0;
    long long result = 0;
    for (std::size_t i = 1; i < color.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(color[i]);
        int                 d = -1;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        if (d < 0)
            return 0;
        result = result * 16 + d;
    }
    return result;
}

inline std::string trim(const std::string& s)
{
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// FilamentColorLibrary NormalizeHexColorNoFallback: optional '#', 6 hex digits or 8 (alpha dropped), upper-cased, "#RRGGBB"; "" if invalid.
inline std::string normalize_hex(const std::string& color)
{
    std::string v = trim(color);
    if (!v.empty() && v.front() == '#')
        v.erase(v.begin());
    if (v.size() == 8)
        v = v.substr(0, 6);
    if (v.size() != 6)
        return {};
    for (char& ch : v) {
        const unsigned char u = static_cast<unsigned char>(ch);
        if (!std::isxdigit(u))
            return {};
        ch = static_cast<char>(std::toupper(u));
    }
    return "#" + v;
}

// BuildPreprintColorMultiItem(FilamentColor::FromMultiColors(multi, mode, fallback)): {mode, nums, colors[] without '#'}.
inline nlohmann::json multi_item(const std::string& multi, int mode_config, const std::string& fallback)
{
    std::vector<std::string> colors;
    std::stringstream        ss(multi);
    std::string              tok;
    while (std::getline(ss, tok, '|')) {
        const std::string n = normalize_hex(tok);
        if (!n.empty())
            colors.push_back(n);
    }
    if (colors.empty()) {
        const std::string fb = normalize_hex(fallback);
        colors.push_back(fb.empty() ? "#26A69A" : fb);
    }
    const bool gradient = colors.size() > 1 && mode_config != 0; // FilamentColorModeFromConfig: 0 is Segment, anything else Gradient
    nlohmann::json out_colors = nlohmann::json::array();
    for (const auto& c : colors)
        out_colors.push_back(c.substr(1));
    return nlohmann::json{{"mode", gradient ? 1 : 0}, {"nums", colors.size()}, {"colors", out_colors}};
}

inline bool finite_nonneg(double v) { return std::isfinite(v) && v >= 0; }

inline bool clean_text(const std::string& s, std::size_t max)
{
    if (s.size() > max)
        return false;
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7f)
            return false;
    return true;
}

} // namespace preprint_detail

// Fills `out` with the object sw_GetFileFilamentMapping replies with. False (and `error` set, `out` an empty object) when the plate data is
// outside the bounds (at most snapmaker_max_filaments filaments, snapmaker_max_extruders extruders, snapmaker_preprint_max_thumbnails
// thumbnails; finite non-negative numbers; map indices in range; clean display name). Keys, in Snapmaker's order and present only when their
// input is: estimated_time, filament_color (numbers), filament_color_rgba (the raw strings), filament_color_multi, filament_type,
// filament_volume_type, nozzle_volume_types, nozzle_diameters (one-decimal strings), filament_weight, filament_weight_total,
// filament_used_mm, filament_extruder_map, nozzle_info, machine_model, thumbnails [{url,width,height}], filename, filepath.
inline bool build_snapmaker_preprint_mapping_json(const SnapmakerPreprintPlate& p, nlohmann::json& out, std::string& error)
{
    using namespace preprint_detail;
    out = nlohmann::json::object();
    auto fail = [&](const std::string& why) {
        out   = nlohmann::json::object();
        error = why;
        return false;
    };
    const std::size_t nf = p.filament_colour.size();
    if (nf > std::size_t(snapmaker_max_filaments)) return fail("too many filaments: " + std::to_string(nf));
    if (p.filament_type.size() > std::size_t(snapmaker_max_filaments) || p.filament_density.size() > std::size_t(snapmaker_max_filaments) ||
        p.filament_diameter.size() > std::size_t(snapmaker_max_filaments) || p.filament_multi_colors.size() > std::size_t(snapmaker_max_filaments) ||
        p.filament_colour_mode.size() > std::size_t(snapmaker_max_filaments))
        return fail("too many per-filament entries");
    if (p.nozzle_diameter.size() > std::size_t(snapmaker_max_extruders) || p.nozzle_volume_types.size() > std::size_t(snapmaker_max_extruders))
        return fail("too many extruders");
    if (p.thumbnails.size() > std::size_t(snapmaker_preprint_max_thumbnails)) return fail("too many thumbnails");
    if (!finite_nonneg(p.estimated_time_s)) return fail("invalid estimated time");
    for (double v : p.filament_density) if (!finite_nonneg(v)) return fail("invalid filament density");
    for (double v : p.filament_diameter) if (!finite_nonneg(v)) return fail("invalid filament diameter");
    for (double v : p.nozzle_diameter) if (!finite_nonneg(v)) return fail("invalid nozzle diameter");
    for (const auto& kv : p.volume_mm3) {
        if (kv.first < 0) return fail("negative filament index in the volumes");
        if (!finite_nonneg(kv.second)) return fail("invalid filament volume");
    }
    for (const auto& kv : p.filament_extruder_map) {
        if (kv.first < 0 || kv.first >= snapmaker_max_filaments || (nf > 0 && std::size_t(kv.first) >= nf))
            return fail("filament index out of range in the extruder map: " + std::to_string(kv.first));
        if (kv.second < 0 || kv.second >= snapmaker_max_extruders)
            return fail("extruder out of range in the extruder map: " + std::to_string(kv.second));
    }
    if (!clean_text(p.display_name, snapmaker_preprint_max_name_bytes) || !clean_text(p.filepath, snapmaker_preprint_max_name_bytes) ||
        !clean_text(p.machine_model, 128))
        return fail("invalid name, path or model text");
    for (const auto& t : p.thumbnails) {
        if (t.png_base64.size() > snapmaker_preprint_max_thumbnail_bytes) return fail("thumbnail too large");
        if (!std::isfinite(t.width) || !std::isfinite(t.height) || t.width <= 0 || t.height <= 0) return fail("invalid thumbnail size");
        for (unsigned char c : t.png_base64)
            if (!(std::isalnum(c) || c == '+' || c == '/' || c == '='))
                return fail("thumbnail is not base64");
    }

    nlohmann::json r = nlohmann::json::object();
    r["estimated_time"] = p.estimated_time_s;

    if (nf > 0) {
        std::vector<long long> numbers(nf, 0);
        nlohmann::json         multi = nlohmann::json::array();
        for (std::size_t i = 0; i < nf; ++i) {
            numbers[i] = color_to_int(p.filament_colour[i]);
            multi.push_back(multi_item(i < p.filament_multi_colors.size() ? p.filament_multi_colors[i] : std::string(),
                                       i < p.filament_colour_mode.size() ? p.filament_colour_mode[i] : 0, p.filament_colour[i]));
        }
        r["filament_color"]       = numbers;
        r["filament_color_rgba"]  = p.filament_colour;
        r["filament_color_multi"] = multi;
    }

    // Snapmaker: count = max(types, colours); the type is get_at(i) (the first one past the end), trimmed.
    std::size_t type_count = std::max(p.filament_type.size(), nf);
    if (!p.filament_type.empty()) {
        std::vector<std::string> types;
        for (std::size_t i = 0; i < type_count; ++i)
            types.push_back(trim(i < p.filament_type.size() ? p.filament_type[i] : p.filament_type.front()));
        r["filament_type"] = types;
    }

    // append_flow_type_fields: count = max(types, colours), at least 1; high flow does not exist here so every filament is "standard".
    r["filament_volume_type"] = std::vector<std::string>(std::max<std::size_t>(type_count, 1), "standard");
    {
        std::vector<std::string> nozzle_types;
        const std::size_t        n = p.nozzle_volume_types.empty() ? p.nozzle_diameter.size() : p.nozzle_volume_types.size();
        for (std::size_t i = 0; i < n; ++i)
            nozzle_types.push_back(i < p.nozzle_volume_types.size() && p.nozzle_volume_types[i] == "high_flow" ? "high_flow" : "standard");
        r["nozzle_volume_types"] = nozzle_types;
    }

    if (!p.nozzle_diameter.empty()) {
        std::vector<std::string> d;
        for (double v : p.nozzle_diameter) {
            std::ostringstream s;
            s << std::fixed << std::setprecision(1) << v;
            d.push_back(s.str());
        }
        r["nozzle_diameters"] = d;
    }

    if (!p.filament_density.empty()) {
        std::vector<double> grams(p.filament_density.size(), 0);
        double              total = 0;
        for (const auto& kv : p.volume_mm3) {
            if (std::size_t(kv.first) >= p.filament_density.size()) continue;
            grams[kv.first] = p.filament_density[kv.first] * kv.second * 0.001;
            total += grams[kv.first];
        }
        r["filament_weight"]       = grams;
        r["filament_weight_total"] = total;
    }

    if (!p.filament_diameter.empty()) {
        std::vector<double> mm(p.filament_diameter.size(), 0);
        for (const auto& kv : p.volume_mm3) {
            if (std::size_t(kv.first) >= p.filament_diameter.size()) continue;
            const double d = p.filament_diameter[kv.first];
            if (d > 0)
                mm[kv.first] = kv.second / (3.14159265358979323846 * d * 0.5 * d * 0.5);
        }
        r["filament_used_mm"] = mm;
    }

    if (!p.filament_extruder_map.empty()) {
        nlohmann::json m = nlohmann::json::object();
        for (const auto& kv : p.filament_extruder_map)
            m[std::to_string(kv.first)] = std::to_string(kv.second);
        r["filament_extruder_map"] = m;
    }

    if (!p.nozzle_diameter.empty()) {
        std::vector<std::string> info;
        for (double dd : p.nozzle_diameter) {
            const float f = float(dd);
            info.push_back(std::abs(f - 0.2f) < 1e-5f ? "0.2" : std::abs(f - 0.4f) < 1e-5f ? "0.4" : std::abs(f - 0.6f) < 1e-5f ? "0.6" :
                           std::abs(f - 0.8f) < 1e-5f ? "0.8" : std::to_string(f));
        }
        r["nozzle_info"] = info;
    }

    if (!p.machine_model.empty())
        r["machine_model"] = p.machine_model;

    nlohmann::json thumbs = nlohmann::json::array();
    for (const auto& t : p.thumbnails)
        thumbs.push_back({{"url", "data:image/png;base64," + t.png_base64}, {"width", t.width}, {"height", t.height}});
    r["thumbnails"] = thumbs;
    r["filename"]   = p.display_name;
    if (!p.filepath.empty())
        r["filepath"] = p.filepath;

    out = std::move(r);
    error.clear();
    return true;
}

} // namespace Slic3r
