#include "SpoolmanMapping.hpp"

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/LocalesUtils.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <climits>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <nlohmann/json.hpp>

namespace Slic3r { namespace Spoolman {

namespace {

using json = nlohmann::json;

// Locale-independent: the classic locale never inserts a decimal comma or digit grouping.
std::string fmt_double(double v)
{
    std::ostringstream ss;
    ss.imbue(std::locale::classic());
    if (std::floor(v) == v && std::fabs(v) < 1e15)
        ss << static_cast<long long>(v);
    else {
        ss.precision(10);
        ss << v;
    }
    return ss.str();
}

std::string trim(const std::string &s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// Decimal-point parsing independent of LC_NUMERIC. Rejects a leading '+', hex floats ("0x1p3"), inf/nan, trailing text and "0,9".
bool parse_number(const std::string &text, double &out)
{
    const std::string t = trim(text);
    if (t.empty() || t[0] == '+')
        return false;
    size_t pos = 0;
    const double v = string_to_double_decimal_point(t, &pos);
    if (pos != t.size() || !std::isfinite(v))
        return false;
    out = v;
    return true;
}

// '#RRGGBB', 'RRGGBB', 'RGB' (expanded) or 'RRGGBBAA' (alpha dropped) -> '#RRGGBB'.
bool normalise_colour(const std::string &text, std::string &out)
{
    std::string t = trim(text);
    if (!t.empty() && t[0] == '#')
        t.erase(0, 1);
    if (!std::all_of(t.begin(), t.end(), [](unsigned char c) { return std::isxdigit(c) != 0; }))
        return false;
    if (t.size() == 3)
        t = std::string{t[0], t[0], t[1], t[1], t[2], t[2]};
    else if (t.size() == 8)
        t.resize(6);
    else if (t.size() != 6)
        return false;
    for (auto &c : t) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    out = "#" + t;
    return true;
}

// Keys that Spoolman data (extra / ofd paths) must never set: they carry G-code, inheritance, compatibility conditions, identity or
// the material type. filament_vendor is allowed only from the native vendor.name field.
bool key_denied_from_spoolman_data(const std::string &key)
{
    static const std::set<std::string> kDeniedKeys = {
        "inherits", "filament_plugin_config_overrides", "filament_type", "name", "filament_id", "setting_id",
        "filament_settings_id", "filament_ids", "filament_spoolman_link", "filament_vendor"};
    auto ends_with = [&](const char *suffix) {
        const size_t n = std::char_traits<char>::length(suffix);
        return key.size() >= n && key.compare(key.size() - n, n, suffix) == 0;
    };
    return kDeniedKeys.count(key) > 0 || ends_with("_gcode") || ends_with("_condition") || key.rfind("compatible_", 0) == 0;
}

constexpr size_t kMaxStringValueBytes = 256;
constexpr size_t kMaxOfdBytes         = 4096;

// THE ALLOWLIST: the only keys Spoolman data may set, each with a type class and explicit finite limits (the option definition's own
// min/max is checked as well; the tighter one wins). Anything not listed is rejected, so a new or loosely-defined Orca option can
// never be reached from Spoolman. To add a key: it must be in Preset::filament_options(), be a number or boolean (never a free
// string, G-code, compatibility, identity or type key), have its units checked, and get a finite min and max; the structural tests in
// test_spoolman_mapping.cpp fail until the test's own expected-key list is updated too. native_only keys are set by the native
// field mapping only and are refused from extra/ofd data.
const std::vector<AllowedKey> kAllowedKeys = [] {
    std::vector<AllowedKey> t = {
        // key, kind, lo, lo_exclusive, hi, native_only                                     units / source
        {"filament_flow_ratio", AllowedKind::Float, 0.5, false, 1.5, false},               // multiplier; OFD flow_ratio
        {"filament_max_volumetric_speed", AllowedKind::Float, 1, false, 100, false},       // mm^3/s; OFD max_volumetric_speed
        {"pressure_advance", AllowedKind::Float, 0, false, 2, false},                      // OFD pressure_advance
        {"enable_pressure_advance", AllowedKind::Bool, 0, false, 1, false},                // follows pressure_advance
        {"nozzle_temperature", AllowedKind::Int, 150, false, 500, false},                  // deg C; native settings_extruder_temp
        {"nozzle_temperature_initial_layer", AllowedKind::Int, 150, false, 500, false},
        {"nozzle_temperature_range_low", AllowedKind::Int, 100, false, 500, false},        // OFD min_print_temperature
        {"nozzle_temperature_range_high", AllowedKind::Int, 100, false, 500, false},       // OFD max_print_temperature
        {"chamber_temperature", AllowedKind::Int, 0, false, 100, false},                   // OFD chamber_temperature
        {"fan_min_speed", AllowedKind::Float, 0, false, 100, false},                       // percent; OFD min_fan_speed
        {"fan_max_speed", AllowedKind::Float, 0, false, 100, false},                       // percent; OFD max_fan_speed
        {"filament_dev_ams_drying_temperature", AllowedKind::Float, 0, false, 200, false}, // deg C (assumed); OFD dry_temperature
        {"filament_dev_ams_drying_time", AllowedKind::Float, 0, false, 1000, false},       // unit unknown (assumed hours); OFD dry_time
        {"filament_density", AllowedKind::Float, 0.5, false, 25, false},                   // g/cm^3; native density
        {"filament_diameter", AllowedKind::Float, 1, false, 4, false},                     // mm; native diameter
        {"filament_cost", AllowedKind::Float, 0, false, 100000, false},                    // money per kg; native price
        {"filament_vendor", AllowedKind::Text, 0, false, 0, true},                         // native vendor.name only
        {"default_filament_colour", AllowedKind::Colour, 0, false, 0, true},               // native color_hex only
        // plate temperatures, deg C: [20, the option definition's own maximum]; native settings_bed_temp
        {"supertack_plate_temp", AllowedKind::Int, 20, false, 120, false},
        {"supertack_plate_temp_initial_layer", AllowedKind::Int, 20, false, 120, false},
        {"cool_plate_temp", AllowedKind::Int, 20, false, 300, false},
        {"cool_plate_temp_initial_layer", AllowedKind::Int, 20, false, 120, false},
        {"textured_cool_plate_temp", AllowedKind::Int, 20, false, 300, false},
        {"textured_cool_plate_temp_initial_layer", AllowedKind::Int, 20, false, 120, false},
        {"eng_plate_temp", AllowedKind::Int, 20, false, 300, false},
        {"eng_plate_temp_initial_layer", AllowedKind::Int, 20, false, 300, false},
        {"hot_plate_temp", AllowedKind::Int, 20, false, 300, false},
        {"hot_plate_temp_initial_layer", AllowedKind::Int, 20, false, 300, false},
        {"textured_plate_temp", AllowedKind::Int, 20, false, 300, false},
        {"textured_plate_temp_initial_layer", AllowedKind::Int, 20, false, 300, false},
    };
    return t;
}();

const AllowedKey *find_allowed(const std::string &key)
{
    for (const AllowedKey &a : kAllowedKeys)
        if (key == a.key)
            return &a;
    return nullptr;
}

// Validate `text` as one element of option `key`. On success returns true and sets `out` to the canonical single-element string.
// `native` is true for values that come from Spoolman's own fields (native-only keys are allowed then).
bool validate_value(const std::string &key, const std::string &text, std::string &out, std::string &reason, bool native)
{
    if (!is_filament_option(key)) {
        reason = "not a filament option";
        return false;
    }
    const AllowedKey *al = find_allowed(key);
    if (!al) {
        reason = "not on the Spoolman allowlist";
        return false;
    }
    if (al->native_only && !native) {
        reason = "key not allowed from Spoolman data";
        return false;
    }
    const ConfigOptionDef *def = print_config_def.get(key);
    const ConfigOptionType base = static_cast<ConfigOptionType>(def->type & ~coVectorType);
    const bool type_ok = (al->kind == AllowedKind::Float && base == coFloat) || (al->kind == AllowedKind::Int && base == coInt) ||
                         (al->kind == AllowedKind::Bool && base == coBool) ||
                         ((al->kind == AllowedKind::Text || al->kind == AllowedKind::Colour) && base == coString);
    if (!type_ok) {
        reason = "allowlist entry does not match the option type";
        return false;
    }
    auto range_ok = [&](double v) {
        if (v < def->min || v > def->max) {
            reason = "value " + text + " out of range [" + fmt_double(def->min) + ", " + fmt_double(def->max) + "]";
            return false;
        }
        if ((al->lo_exclusive ? v <= al->lo : v < al->lo) || v > al->hi) {
            reason = "value " + text + " out of range " + (al->lo_exclusive ? "(" : "[") + fmt_double(al->lo) + ", " + fmt_double(al->hi) + "]";
            return false;
        }
        return true;
    };
    switch (al->kind) {
    case AllowedKind::Float: {
        double v;
        if (!parse_number(text, v)) { reason = "expected a number, got '" + text + "'"; return false; }
        if (!range_ok(v)) return false;
        out = fmt_double(v);
        return true;
    }
    case AllowedKind::Int: {
        double v;
        if (!parse_number(text, v) || std::floor(v) != v) { reason = "expected an integer, got '" + text + "'"; return false; }
        if (v < INT_MIN || v > INT_MAX) { reason = "integer out of int range: " + text; return false; }
        if (!range_ok(v)) return false;
        out = fmt_double(v);
        return true;
    }
    case AllowedKind::Bool: {
        const std::string t = trim(text);
        if (t == "1" || t == "true")  { out = "1"; return true; }
        if (t == "0" || t == "false") { out = "0"; return true; }
        reason = "expected a boolean (0/1/true/false), got '" + text + "'";
        return false;
    }
    case AllowedKind::Colour:
        if (!normalise_colour(text, out)) { reason = "expected a colour #RRGGBB, got '" + text + "'"; return false; }
        return true;
    case AllowedKind::Text:
        if (text.size() > kMaxStringValueBytes) { reason = "string longer than 256 bytes"; return false; }
        for (unsigned char ch : text)
            if (ch < 0x20 || ch == 0x7f) { reason = "string contains a control character"; return false; }
        out = text;
        return true;
    }
    reason = "unsupported option type";
    return false;
}

struct Collector
{
    MappingResult &res;
    std::map<std::string, size_t> index;

    void add_rejected(const std::string &what, const std::string &reason, bool warning = false, const std::string &value = std::string())
    {
        res.rejected.push_back({what, reason, warning, value});
    }

    // Validate and add; later calls with the same key replace earlier ones (higher precedence). `native` sources may set keys that
    // the denylist forbids for extra/ofd data (filament_vendor).
    bool add(const std::string &key, const std::string &value, const std::string &source, bool native = false)
    {
        std::string canon, reason;
        if (!native && key_denied_from_spoolman_data(key))
            reason = "key not allowed from Spoolman data";
        else
            validate_value(key, value, canon, reason, native);
        if (!reason.empty()) {
            add_rejected(key + " (" + source + ")", reason, false, value);
            return false;
        }
        auto it = index.find(key);
        if (it == index.end()) {
            index[key] = res.overrides.size();
            res.overrides.push_back({key, canon, source, "", ""});
        } else {
            Override &o = res.overrides[it->second];
            o.replaced_source = o.source;
            o.replaced_value  = o.value;
            o.value           = canon;
            o.source          = source;
        }
        if (key == "pressure_advance") {
            double v = 0;
            parse_number(canon, v);
            add("enable_pressure_advance", v > 0 ? "1" : "0", source, true);
        }
        return true;
    }

    void remove(const std::string &key)
    {
        res.overrides.erase(std::remove_if(res.overrides.begin(), res.overrides.end(), [&](const Override &o) { return o.key == key; }),
                            res.overrides.end());
        index.clear();
        for (size_t i = 0; i < res.overrides.size(); ++i) index[res.overrides[i].key] = i;
    }
};

const std::vector<std::string> kPlateKeys = {
    "cool_plate_temp", "eng_plate_temp", "hot_plate_temp", "textured_plate_temp", "textured_cool_plate_temp", "supertack_plate_temp",
    "cool_plate_temp_initial_layer", "eng_plate_temp_initial_layer", "hot_plate_temp_initial_layer",
    "textured_plate_temp_initial_layer", "textured_cool_plate_temp_initial_layer", "supertack_plate_temp_initial_layer"};

bool parent_value_nonzero(const DynamicPrintConfig &parent, const std::string &key)
{
    const ConfigOption *o = parent.option(key);
    if (!o)
        return false;
    if (auto *v = dynamic_cast<const ConfigOptionVector<int> *>(o))
        return !v->values.empty() && v->values.front() != 0;
    if (auto *v = dynamic_cast<const ConfigOptionVector<double> *>(o))
        return !v->values.empty() && v->values.front() != 0;
    return false;
}

struct ExtraMap { const char *field; const char *key; };
const ExtraMap kExtraMap[] = {
    {"flow_ratio", "filament_flow_ratio"},
    {"max_volumetric_speed", "filament_max_volumetric_speed"},
    {"min_print_temperature", "nozzle_temperature_range_low"},
    {"max_print_temperature", "nozzle_temperature_range_high"},
    {"chamber_temperature", "chamber_temperature"},
    {"min_fan_speed", "fan_min_speed"},
    {"max_fan_speed", "fan_max_speed"},
    {"dry_temperature", "filament_dev_ams_drying_temperature"},
    {"dry_time", "filament_dev_ams_drying_time"},
};

// A scalar, or an array holding exactly one scalar (unwrapped one level only, no recursion).
std::string json_scalar_text(const json &v, bool &ok)
{
    const json *p = &v;
    if (p->is_array() && p->size() == 1)
        p = &(*p)[0];
    ok = true;
    if (p->is_string()) return p->get<std::string>();
    if (p->is_number()) return fmt_double(p->get<double>());
    if (p->is_boolean()) return p->get<bool>() ? "true" : "false"; // only a bool option accepts this text
    ok = false;
    return {};
}

} // namespace

const std::vector<std::string> &plate_temperature_keys() { return kPlateKeys; }

const std::vector<AllowedKey> &spoolman_allowlist() { return kAllowedKeys; }

bool is_filament_option(const std::string &key)
{
    static const std::set<std::string> keys = [] {
        return std::set<std::string>(Preset::filament_options().begin(), Preset::filament_options().end());
    }();
    return keys.count(key) > 0 && print_config_def.get(key) != nullptr;
}

MappingResult map_filament(const Filament &f, const DynamicPrintConfig &parent)
{
    MappingResult res;
    Collector c{res, {}};

    // ---- native ----
    // Spoolman has no "unset" for temperatures: zero or negative means unset, not an override.
    auto positive_temp = [&](const std::optional<double> &t, const char *field) {
        if (!t) return false;
        if (*t <= 0) {
            c.add_rejected(field, "zero means unset", true, fmt_double(*t));
            return false;
        }
        return true;
    };
    if (positive_temp(f.settings_extruder_temp, "settings_extruder_temp")) {
        const std::string v = fmt_double(*f.settings_extruder_temp);
        c.add("nozzle_temperature", v, "native:settings_extruder_temp", true);
        c.add("nozzle_temperature_initial_layer", v, "native:settings_extruder_temp", true);
    }
    if (positive_temp(f.settings_bed_temp, "settings_bed_temp")) {
        // All or nothing: if any plate key rejects the value, no plate key gets it.
        const std::string v = fmt_double(*f.settings_bed_temp);
        std::vector<std::string> targets;
        std::string              failure;
        for (const auto &k : kPlateKeys)
            if (parent_value_nonzero(parent, k)) {
                std::string canon, reason;
                if (!validate_value(k, v, canon, reason, true) && failure.empty())
                    failure = k + ": " + reason;
                targets.push_back(k);
            }
        if (!failure.empty())
            c.add_rejected("settings_bed_temp", failure, false, v);
        else
            for (const auto &k : targets)
                c.add(k, v, "native:settings_bed_temp", true);
    }
    if (f.density)  c.add("filament_density", fmt_double(*f.density), "native:density", true);
    if (f.diameter) c.add("filament_diameter", fmt_double(*f.diameter), "native:diameter", true);
    if (f.color_hex) {
        std::string col;
        if (!normalise_colour(*f.color_hex, col))
            c.add_rejected("color_hex", "expected a colour #RRGGBB, got '" + *f.color_hex + "'", false, *f.color_hex);
        else
            c.add("default_filament_colour", col, "native:color_hex", true);
    }
    if (f.price) {
        // filament_cost is money per kg; Spoolman's price is per spool of `weight` grams.
        if (f.weight && *f.weight > 0)
            c.add("filament_cost", fmt_double(*f.price * 1000.0 / *f.weight), "native:price", true);
        else
            c.add_rejected("price", "price needs weight");
    }
    if (f.vendor && !f.vendor->name.empty()) c.add("filament_vendor", f.vendor->name, "native:vendor.name", true);

    if (f.weight)       res.display_only.push_back("weight=" + fmt_double(*f.weight));
    if (f.spool_weight) res.display_only.push_back("spool_weight=" + fmt_double(*f.spool_weight));

    // ---- extra ----
    auto reject_undecoded = [&](const std::string &field) {
        if (f.extras_undecoded.count(field)) {
            c.add_rejected(field, "extra value could not be decoded");
            return true;
        }
        return false;
    };
    for (const auto &kv : f.extras) {
        const std::string &field = kv.first;
        const std::string &text  = kv.second;
        if (field == "setting_id" || field == "slicer_settings_orcaslicer")
            continue; // matching only / handled last
        if (reject_undecoded(field))
            continue;
        if (field == "min_bed_temperature" || field == "max_bed_temperature") {
            c.add_rejected(field, "informational only, never applied", true);
            continue;
        }
        if (field == "pressure_advance") {
            c.add("pressure_advance", text, "extra:pressure_advance"); // also sets enable_pressure_advance
            continue;
        }
        bool found = false;
        for (const auto &m : kExtraMap)
            if (field == m.field) {
                c.add(m.key, text, "extra:" + field);
                found = true;
                break;
            }
        if (!found)
            c.add_rejected(field, "unknown extra field");
    }

    // ---- ofd slicer settings, last ----
    const std::string ofd = "slicer_settings_orcaslicer";
    auto it = f.extras.find(ofd);
    if (it != f.extras.end() && !reject_undecoded(ofd)) {
        if (it->second.size() > kMaxOfdBytes)
            c.add_rejected(ofd, "text longer than 4096 bytes");
        else if (max_nesting(it->second) > kMaxExtraDepth)
            c.add_rejected(ofd, "nesting too deep");
        else {
            json j = json::parse(it->second, nullptr, false);
            if (j.is_discarded() || !j.is_object())
                c.add_rejected(ofd, "not a JSON object");
            else
                for (auto kv = j.begin(); kv != j.end(); ++kv) {
                    bool ok;
                    const std::string text = json_scalar_text(kv.value(), ok);
                    if (!ok) {
                        c.add_rejected(kv.key() + " (ofd:" + ofd + ")", "value must be a scalar or a one-element array of a scalar");
                        continue;
                    }
                    c.add(kv.key(), text, "ofd:" + ofd);
                }
        }
    }

    // ---- cross-checks ----
    const Override *lo = nullptr, *hi = nullptr;
    for (const auto &o : res.overrides) {
        if (o.key == "nozzle_temperature_range_low") lo = &o;
        if (o.key == "nozzle_temperature_range_high") hi = &o;
    }
    double lo_v = 0, hi_v = 0;
    if (lo && hi && parse_number(lo->value, lo_v) && parse_number(hi->value, hi_v) && lo_v > hi_v) {
        c.add_rejected("nozzle_temperature_range_low/high", "low " + lo->value + " is above high " + hi->value);
        c.remove("nozzle_temperature_range_low");
        c.remove("nozzle_temperature_range_high");
    }
    return res;
}

}} // namespace Slic3r::Spoolman
