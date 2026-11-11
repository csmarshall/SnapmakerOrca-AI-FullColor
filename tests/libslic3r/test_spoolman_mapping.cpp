#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Spoolman/SpoolmanCatalog.hpp"
#include "libslic3r/Spoolman/SpoolmanMapping.hpp"

#include <clocale>
#include "libslic3r/Preset.hpp"

#include <cmath>
#include <fstream>
#include <locale>
#include <set>
#include <sstream>
#include <string>

using namespace Slic3r;
using namespace Slic3r::Spoolman;

namespace {

// Parent: DynamicPrintConfig::full_print_config() (library defaults, NOT a system profile) with plate temperatures set by hand.
DynamicPrintConfig make_parent(int hot = 60)
{
    DynamicPrintConfig p = DynamicPrintConfig::full_print_config();
    for (const char *k : {"cool_plate_temp", "eng_plate_temp", "textured_plate_temp", "textured_cool_plate_temp", "supertack_plate_temp",
                          "cool_plate_temp_initial_layer", "eng_plate_temp_initial_layer", "textured_plate_temp_initial_layer",
                          "textured_cool_plate_temp_initial_layer", "supertack_plate_temp_initial_layer"})
        p.set_key_value(k, new ConfigOptionInts{55});
    p.set_key_value("hot_plate_temp", new ConfigOptionInts{hot});
    p.set_key_value("hot_plate_temp_initial_layer", new ConfigOptionInts{hot});
    return p;
}

Filament fixture_filament(int id)
{
    std::ifstream in(std::string(TEST_DATA_DIR) + "/spoolman/filament.json");
    REQUIRE(in.is_open());
    auto r = parse_filaments(nlohmann::json::parse(in));
    for (auto &f : r.items)
        if (f.id == id)
            return f;
    FAIL("fixture filament not found");
    return {};
}

const Override *find(const Spoolman::MappingResult &r, const std::string &key)
{
    for (auto &o : r.overrides)
        if (o.key == key)
            return &o;
    return nullptr;
}
const Rejected *find_rej(const Spoolman::MappingResult &r, const std::string &needle)
{
    for (auto &o : r.rejected)
        if (o.key_or_field.find(needle) != std::string::npos)
            return &o;
    return nullptr;
}
int count(const Spoolman::MappingResult &r, const std::string &key)
{
    int n = 0;
    for (auto &o : r.overrides) n += o.key == key;
    return n;
}

Filament with_extra(const std::string &k, const std::string &v)
{
    Filament f;
    f.id = 1;
    f.extras[k] = v;
    return f;
}

} // namespace

TEST_CASE("Spoolman mapping: ofd override of a print option is rejected", "[Spoolman]")
{
    auto r = map_filament(with_extra("slicer_settings_orcaslicer", R"({"layer_height": "0.2"})"), make_parent());
    CHECK(find(r, "layer_height") == nullptr);
    const Rejected *rej = find_rej(r, "layer_height");
    REQUIRE(rej);
    CHECK(rej->reason == "not a filament option");

    auto r2 = map_filament(with_extra("slicer_settings_orcaslicer", R"({"no_such_key_xyz": 1})"), make_parent());
    CHECK(r2.overrides.empty());
    REQUIRE(find_rej(r2, "no_such_key_xyz"));

    // via the extra path a print-option name is an unknown extra field
    auto r3 = map_filament(with_extra("layer_height", "0.2"), make_parent());
    CHECK(r3.overrides.empty());
    REQUIRE(find_rej(r3, "layer_height"));
}

TEST_CASE("Spoolman mapping: filament 16 from the fixtures", "[Spoolman]")
{
    auto r = map_filament(fixture_filament(16), make_parent());
    // expected values written by hand from the fixture (nozzle 250, bed 70, density 1.3, dia 1.75, FF1523, Snapmaker)
    for (const char *k : {"nozzle_temperature", "nozzle_temperature_initial_layer"}) {
        REQUIRE(find(r, k));
        CHECK(find(r, k)->value == "250");
        CHECK(find(r, k)->source == "native:settings_extruder_temp");
    }
    for (const char *k : {"cool_plate_temp", "eng_plate_temp", "hot_plate_temp", "textured_plate_temp", "textured_cool_plate_temp",
                          "supertack_plate_temp", "cool_plate_temp_initial_layer", "hot_plate_temp_initial_layer"}) {
        REQUIRE(find(r, k));
        CHECK(find(r, k)->value == "70");
        CHECK(find(r, k)->source == "native:settings_bed_temp");
    }
    CHECK(find(r, "filament_density")->value == "1.3");
    CHECK(find(r, "filament_density")->source == "native:density");
    CHECK(find(r, "filament_diameter")->value == "1.75");
    CHECK(find(r, "filament_colour") == nullptr); // slot colour is set at assignment, later
    CHECK(find(r, "default_filament_colour")->value == "#FF1523");
    CHECK(find(r, "default_filament_colour")->source == "native:color_hex");
    CHECK(find(r, "filament_vendor")->value == "Snapmaker");
    CHECK(find(r, "filament_vendor")->source == "native:vendor.name");
    CHECK(find(r, "filament_cost") == nullptr); // fixture has no price
    REQUIRE(r.display_only.size() == 1);
    CHECK(r.display_only[0] == "weight=1000");
}

TEST_CASE("Spoolman mapping: precedence extra < ofd and native < ofd, no duplicate keys", "[Spoolman]")
{
    Filament f;
    f.extras["flow_ratio"] = "0.95";
    f.extras["slicer_settings_orcaslicer"] = R"({"filament_flow_ratio": 0.9})";
    auto r = map_filament(f, make_parent());
    REQUIRE(count(r, "filament_flow_ratio") == 1);
    const Override *o = find(r, "filament_flow_ratio");
    CHECK(o->value == "0.9");
    CHECK(o->source == "ofd:slicer_settings_orcaslicer");
    CHECK(o->replaced_source == "extra:flow_ratio");
    CHECK(o->replaced_value == "0.95");

    // native < ofd (native < extra is not tested here: no extra field maps onto a native key)
    Filament g;
    g.settings_extruder_temp = 250;
    g.extras["slicer_settings_orcaslicer"] = R"({"nozzle_temperature": 215})";
    auto r2 = map_filament(g, make_parent());
    CHECK(count(r2, "nozzle_temperature") == 1);
    CHECK(find(r2, "nozzle_temperature")->value == "215");
    CHECK(find(r2, "nozzle_temperature_initial_layer")->value == "250");
}

TEST_CASE("Spoolman mapping: absent fields give no override", "[Spoolman]")
{
    Filament f;
    f.density = 1.24;
    auto r = map_filament(f, make_parent());
    CHECK(find(r, "nozzle_temperature") == nullptr);
    CHECK(find(r, "nozzle_temperature_initial_layer") == nullptr);
    for (auto &k : plate_temperature_keys())
        CHECK(find(r, k) == nullptr);
    REQUIRE(r.overrides.size() == 1);
    CHECK(r.overrides[0].key == "filament_density");

    // the real fixture filament without an extruder temperature (design: 9 of them)
    const Filament none = [] {
        std::ifstream in(std::string(TEST_DATA_DIR) + "/spoolman/filament.json");
        for (auto &fl : parse_filaments(nlohmann::json::parse(in)).items)
            if (!fl.settings_extruder_temp) return fl;
        return Filament{};
    }();
    CHECK(find(map_filament(none, make_parent()), "nozzle_temperature") == nullptr);
}

TEST_CASE("Spoolman mapping: bed temperature only where the parent is non-zero", "[Spoolman]")
{
    Filament f;
    f.settings_bed_temp = 70;
    auto r = map_filament(f, make_parent(/*hot*/ 0));
    CHECK(find(r, "hot_plate_temp") == nullptr);
    CHECK(find(r, "hot_plate_temp_initial_layer") == nullptr);
    CHECK(find(r, "cool_plate_temp")->value == "70");
    CHECK(r.overrides.size() == 10);
}

TEST_CASE("Spoolman mapping: invalid values are rejected, never clamped", "[Spoolman]")
{
    auto parent = make_parent();
    for (auto [field, val] : {std::pair<const char *, const char *>{"flow_ratio", "-1"}, {"flow_ratio", "3"}, {"max_volumetric_speed", "abc"},
                              {"max_fan_speed", "150"}, {"min_fan_speed", "-5"}, {"chamber_temperature", "40.5"}}) {
        auto r = map_filament(with_extra(field, val), parent);
        INFO(field << "=" << val);
        CHECK(r.overrides.empty());
        REQUIRE(r.rejected.size() == 1);
        CHECK_FALSE(r.rejected[0].reason.empty());
    }
    auto r = map_filament(with_extra("max_fan_speed", "150"), parent);
    CHECK(r.rejected[0].reason == "value 150 out of range [0, 100]");
    // ofd path: wrong type for a float option
    auto r2 = map_filament(with_extra("slicer_settings_orcaslicer", R"({"filament_density": "heavy", "enable_pressure_advance": "maybe"})"), parent);
    CHECK(r2.overrides.empty());
    CHECK(r2.rejected.size() == 2);
    // valid boundary values are accepted
    CHECK(find(map_filament(with_extra("max_fan_speed", "100"), parent), "fan_max_speed")->value == "100");
}

TEST_CASE("Spoolman mapping: colour normalisation", "[Spoolman]")
{
    Filament f;
    f.color_hex = "ZZZZZZ";
    auto r = map_filament(f, make_parent());
    CHECK(r.overrides.empty());
    REQUIRE(r.rejected.size() == 1);
    CHECK(r.rejected[0].key_or_field == "color_hex");
    f.color_hex = "#ff1523";
    CHECK(find(map_filament(f, make_parent()), "default_filament_colour")->value == "#FF1523");
    f.color_hex = "FF1523";
    CHECK(find(map_filament(f, make_parent()), "default_filament_colour")->value == "#FF1523");
    f.color_hex = "f0a"; // 3 digits expand
    CHECK(find(map_filament(f, make_parent()), "default_filament_colour")->value == "#FF00AA");
    f.color_hex = "11223344"; // alpha dropped
    CHECK(find(map_filament(f, make_parent()), "default_filament_colour")->value == "#112233");
    for (const char *bad : {"FF15", "##FF1523", "FF 523", "FF152", "#", ""}) {
        f.color_hex = bad;
        INFO(bad);
        CHECK(map_filament(f, make_parent()).overrides.empty());
    }
}

TEST_CASE("Spoolman mapping: undecoded extra rejected, informational fields", "[Spoolman]")
{
    Filament f = with_extra("flow_ratio", "\"0.9");
    f.extras_undecoded.insert("flow_ratio");
    auto r = map_filament(f, make_parent());
    CHECK(r.overrides.empty());
    REQUIRE(r.rejected.size() == 1);
    CHECK(r.rejected[0].reason == "extra value could not be decoded");

    Filament g = with_extra("min_bed_temperature", "50");
    g.extras["max_bed_temperature"] = "90";
    auto r2 = map_filament(g, make_parent());
    CHECK(r2.overrides.empty());
    REQUIRE(r2.rejected.size() == 2);
    for (auto &rej : r2.rejected) {
        CHECK(rej.warning);
        CHECK(rej.reason.find("informational only") == 0);
    }
}

TEST_CASE("Spoolman mapping: extra fields map to the real option keys", "[Spoolman]")
{
    Filament f;
    f.extras = {{"pressure_advance", "0.04"}, {"flow_ratio", "0.98"}, {"max_volumetric_speed", "12"}, {"min_print_temperature", "230"},
                {"max_print_temperature", "260"}, {"chamber_temperature", "45"}, {"min_fan_speed", "20"}, {"dry_temperature", "65"},
                {"dry_time", "6"}, {"setting_id", "abc"}};
    auto r = map_filament(f, make_parent());
    // hand-computed expectations
    CHECK(find(r, "pressure_advance")->value == "0.04");
    CHECK(find(r, "enable_pressure_advance")->value == "1");
    CHECK(find(r, "enable_pressure_advance")->source == "extra:pressure_advance");
    CHECK(find(r, "filament_flow_ratio")->value == "0.98");
    CHECK(find(r, "filament_max_volumetric_speed")->value == "12");
    CHECK(find(r, "nozzle_temperature_range_low")->value == "230");
    CHECK(find(r, "nozzle_temperature_range_high")->value == "260");
    CHECK(find(r, "chamber_temperature")->value == "45");
    CHECK(find(r, "fan_min_speed")->value == "20");
    CHECK(find(r, "filament_dev_ams_drying_temperature")->value == "65");
    CHECK(find(r, "filament_dev_ams_drying_time")->value == "6");
    CHECK(r.overrides.size() == 10);
    CHECK(r.rejected.empty()); // setting_id is for matching only: neither override nor rejection
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Review round: security denylist, limits, locale, extras shape, price, bool/percent/plate keys.

namespace {
std::string ofd_json(const std::string &key, const std::string &json_value)
{
    return "{\"" + key + "\": " + json_value + "}";
}
} // namespace

TEST_CASE("Spoolman mapping: denylisted keys never come from extra or ofd", "[Spoolman]")
{
    auto parent = make_parent();
    for (const char *key : {"filament_start_gcode", "filament_end_gcode", "filament_change_extrusion_role_gcode", "inherits",
                            "compatible_printers", "compatible_printers_condition", "compatible_prints_condition",
                            "filament_plugin_config_overrides", "filament_type", "name", "filament_id", "setting_id",
                            "filament_settings_id", "filament_ids", "filament_spoolman_link", "filament_vendor"}) {
        INFO(key);
        // extra path (field name is the option key; unknown or denied, either way no override)
        auto r1 = map_filament(with_extra(key, "x"), parent);
        CHECK(find(r1, key) == nullptr);
        // ofd path: must be rejected with the denylist reason
        auto r2 = map_filament(with_extra("slicer_settings_orcaslicer", ofd_json(key, "\"G28\"")), parent);
        CHECK(r2.overrides.empty());
        const Rejected *rej = find_rej(r2, key);
        REQUIRE(rej);
        CHECK(rej->reason == "key not allowed from Spoolman data");
    }
    // native vendor still works
    Filament f;
    f.vendor = Vendor{};
    f.vendor->name = "Acme";
    CHECK(find(map_filament(f, parent), "filament_vendor")->value == "Acme");
}

TEST_CASE("Spoolman mapping: string values are length capped and free of control characters", "[Spoolman]")
{
    auto parent = make_parent();
    // strings are only reachable through the native vendor name; extra/ofd cannot set any string option
    auto vendor = [&](const std::string &name) {
        Filament f;
        f.vendor = Vendor{};
        f.vendor->name = name;
        return map_filament(f, parent);
    };
    CHECK(find(vendor("plain"), "filament_vendor")->value == "plain");
    auto nl = vendor("line1\nM104 S999");
    CHECK(nl.overrides.empty());
    REQUIRE(nl.rejected.size() == 1);
    CHECK(nl.rejected[0].reason == "string contains a control character");
    CHECK(vendor(std::string("a\x7f") + "b").overrides.empty());
    CHECK(vendor("a\tb").overrides.empty());
    CHECK(vendor(std::string(256, 'a')).overrides.size() == 1);
    CHECK(vendor(std::string(257, 'a')).overrides.empty());
    // filament_notes is a string option: no longer reachable from ofd, plain or not
    for (const char *v : {"\"plain note\"", "\"a\\nb\""}) {
        auto r = map_filament(with_extra("slicer_settings_orcaslicer", ofd_json("filament_notes", v)), parent);
        CHECK(r.overrides.empty());
        REQUIRE(find_rej(r, "filament_notes"));
        CHECK(find_rej(r, "filament_notes")->reason == "not on the Spoolman allowlist");
    }
}

TEST_CASE("Spoolman mapping: ofd JSON hostility", "[Spoolman]")
{
    auto parent = make_parent();
    auto run = [&](const std::string &text) { return map_filament(with_extra("slicer_settings_orcaslicer", text), parent); };
    // deep array: rejected, no stack trouble
    CHECK(run(ofd_json("filament_density", std::string(100000, '[') + std::string(100000, ']'))).overrides.empty());
    CHECK(run(ofd_json("filament_density", std::string(3000, '[') + "1" + std::string(3000, ']'))).overrides.empty());
    // [[x]] is rejected, [x] accepted
    auto r = run(R"({"filament_density": [[1.2]]})");
    CHECK(r.overrides.empty());
    CHECK(find_rej(r, "filament_density"));
    CHECK(find(run(R"({"filament_density": [1.2]})"), "filament_density")->value == "1.2");
    // oversize text
    auto big = run("{\"filament_notes\": \"" + std::string(5000, 'a') + "\"}");
    CHECK(big.overrides.empty());
    REQUIRE(big.rejected.size() == 1);
    CHECK(big.rejected[0].reason == "text longer than 4096 bytes");
    // malformed, non-object, empty array, null, object value
    for (const char *bad : {"{not json", "[1,2]", "[]", "null", "\"str\"", "5"}) {
        INFO(bad);
        auto b = run(bad);
        CHECK(b.overrides.empty());
        REQUIRE(b.rejected.size() == 1);
        CHECK(b.rejected[0].reason == "not a JSON object");
    }
    CHECK(run(R"({"filament_density": {"a":1}})").overrides.empty());
    CHECK(run(R"({"filament_density": []})").overrides.empty());
    CHECK(run(R"({"filament_density": null})").overrides.empty());
}

TEST_CASE("Spoolman mapping: numeric extras arrive as Spoolman really stores them", "[Spoolman]")
{
    auto body = nlohmann::json::parse(
        R"([{"id":5,"extra":{"flow_ratio":"0.98","pressure_advance":"0.04","dry_time":"6","chamber_temperature":"40"}}])");
    auto parsed = parse_filaments(body);
    REQUIRE(parsed.status == ParseStatus::Ok);
    auto r = map_filament(parsed.items[0], make_parent());
    CHECK(r.rejected.empty());
    CHECK(find(r, "filament_flow_ratio")->value == "0.98");
    CHECK(find(r, "pressure_advance")->value == "0.04");
    CHECK(find(r, "enable_pressure_advance")->value == "1");
    CHECK(find(r, "filament_dev_ams_drying_time")->value == "6");
    CHECK(find(r, "chamber_temperature")->value == "40");
}

namespace {
// Hand-written (not read from the table in the .cpp): key, min, max (all inclusive), whether the value is an integer option.
struct Row { const char *key; double lo; double hi; };
const Row kLimitRows[] = {
    {"filament_diameter", 1, 4}, {"filament_flow_ratio", 0.5, 1.5}, {"filament_density", 0.5, 25}, {"filament_max_volumetric_speed", 1, 100},
    {"pressure_advance", 0, 2}, {"nozzle_temperature", 150, 500}, {"nozzle_temperature_initial_layer", 150, 500},
    {"nozzle_temperature_range_low", 100, 500}, {"nozzle_temperature_range_high", 100, 500}, {"chamber_temperature", 0, 100},
    {"fan_min_speed", 0, 100}, {"fan_max_speed", 0, 100}, {"filament_dev_ams_drying_temperature", 0, 200},
    {"filament_dev_ams_drying_time", 0, 1000}, {"filament_cost", 0, 100000},
    {"supertack_plate_temp", 20, 120}, {"supertack_plate_temp_initial_layer", 20, 120}, {"cool_plate_temp", 20, 300},
    {"cool_plate_temp_initial_layer", 20, 120}, {"textured_cool_plate_temp", 20, 300}, {"textured_cool_plate_temp_initial_layer", 20, 120},
    {"eng_plate_temp", 20, 300}, {"eng_plate_temp_initial_layer", 20, 300}, {"hot_plate_temp", 20, 300},
    {"hot_plate_temp_initial_layer", 20, 300}, {"textured_plate_temp", 20, 300}, {"textured_plate_temp_initial_layer", 20, 300}};
} // namespace

TEST_CASE("Spoolman mapping: every limit row, both boundaries and just outside", "[Spoolman]")
{
    auto parent = make_parent();
    auto via_ofd = [&](const std::string &key, const std::string &v) {
        return map_filament(with_extra("slicer_settings_orcaslicer", ofd_json(key, v)), parent);
    };
    for (const Row &row : kLimitRows) {
        INFO(row.key);
        const std::string k = row.key;
        const bool is_int = k.find("nozzle_temperature") != std::string::npos || k == "chamber_temperature" || k.find("plate_temp") != std::string::npos;
        const double step = is_int ? 1 : 0.001;
        auto num = [](double v) { std::ostringstream ss; ss.imbue(std::locale::classic()); ss.precision(12); ss << v; return ss.str(); };
        CHECK(find(via_ofd(row.key, num(row.lo)), row.key));
        CHECK(find(via_ofd(row.key, num(row.hi)), row.key));
        CHECK_FALSE(find(via_ofd(row.key, num(row.lo - step)), row.key));
        CHECK_FALSE(find(via_ofd(row.key, num(row.hi + step)), row.key));
    }
    // the failing inputs from the review
    for (const char *key : {"filament_density", "filament_flow_ratio", "filament_diameter", "filament_max_volumetric_speed"})
        CHECK_FALSE(find(via_ofd(key, "1e-6"), key));
    CHECK_FALSE(find(via_ofd("nozzle_temperature", "1"), "nozzle_temperature"));
    CHECK_FALSE(find(via_ofd("hot_plate_temp", "300.5"), "hot_plate_temp"));
    CHECK_FALSE(find(via_ofd("hot_plate_temp", "-50"), "hot_plate_temp"));
    CHECK(find(via_ofd("hot_plate_temp", "300"), "hot_plate_temp"));
    for (const char *key : {"idle_temperature", "filament_flush_temp", "filament_retraction_length", "filament_pre_cooling_temperature"})
        for (const char *v : {"1500", "1e30", "-1e30", "2000000000"})
            CHECK(via_ofd(key, v).overrides.empty());
    for (const char *v : {"1e30", "1e12", "abc", "-0.0001"})
        CHECK_FALSE(find(via_ofd("filament_cost", v), "filament_cost"));
    CHECK_FALSE(find(via_ofd("nozzle_temperature_range_low", "501"), "nozzle_temperature_range_low"));
    CHECK_FALSE(find(via_ofd("nozzle_temperature_range_high", "1500"), "nozzle_temperature_range_high"));
    // native values go through the same table
    Filament f;
    f.diameter = 0;
    f.density  = 0;
    auto rn = map_filament(f, parent);
    CHECK(rn.overrides.empty());
    CHECK(rn.rejected.size() == 2);
}

TEST_CASE("Spoolman mapping: number text pinned (pos check) and JSON type coercion", "[Spoolman]")
{
    auto parent = make_parent();
    auto via_ofd = [&](const std::string &key, const std::string &v) {
        return map_filament(with_extra("slicer_settings_orcaslicer", ofd_json(key, v)), parent);
    };
    for (const char *bad : {"\"1,75\"", "\"1.75mm\"", "\"1.75 mm\"", "\"1..75\"", "\"\"", "\"  \""})
        CHECK(via_ofd("filament_diameter", bad).overrides.empty());
    CHECK(find(via_ofd("filament_diameter", "\" 1.75 \""), "filament_diameter")->value == "1.75");
    // multi-element array
    CHECK(via_ofd("filament_density", "[1.2, 3]").overrides.empty());
    // JSON booleans never become numbers
    CHECK(via_ofd("nozzle_temperature", "true").overrides.empty());
    CHECK(via_ofd("filament_density", "false").overrides.empty());
    CHECK(via_ofd("nozzle_temperature", "[true]").overrides.empty());
    CHECK(via_ofd("chamber_temperature", "true").overrides.empty()); // 1 would be in range: only the type rule stops it
    CHECK(via_ofd("fan_max_speed", "[false]").overrides.empty());
    // JSON numbers into a bool option: exactly 0 or 1 only
    CHECK(find(via_ofd("enable_pressure_advance", "1"), "enable_pressure_advance")->value == "1");
    CHECK(find(via_ofd("enable_pressure_advance", "0"), "enable_pressure_advance")->value == "0");
    CHECK(via_ofd("enable_pressure_advance", "2").overrides.empty());
    CHECK(via_ofd("enable_pressure_advance", "0.5").overrides.empty());
    CHECK(find(via_ofd("enable_pressure_advance", "true"), "enable_pressure_advance")->value == "1");
}

TEST_CASE("Spoolman mapping: formatting is locale independent even with a de_DE global C++ locale", "[Spoolman]")
{
    std::locale prev;
    try {
        std::locale::global(std::locale("de_DE.UTF-8"));
    } catch (const std::exception &) {
        WARN("de_DE.UTF-8 locale not installed: global-locale formatting test skipped");
        return;
    }
    Filament f;
    f.density  = 1.3;
    f.diameter = 1.75;
    f.price    = 1234.5;
    f.weight   = 1000;
    auto r = map_filament(f, make_parent());
    std::locale::global(prev);
    CHECK(find(r, "filament_density")->value == "1.3");
    CHECK(find(r, "filament_diameter")->value == "1.75");
    CHECK(find(r, "filament_cost")->value == "1234.5"); // no decimal comma, no digit grouping
}

TEST_CASE("Spoolman mapping: string options are rejected from ofd", "[Spoolman]")
{
    auto parent = make_parent();
    for (const char *key : {"adaptive_pressure_advance_model", "filament_dev_ams_drying_ams_limitations", "filament_extruder_variant",
                            "filament_notes", "filament_ramming_parameters", "volumetric_speed_coefficients"}) {
        INFO(key);
        for (const std::string &v : {std::string("1"), std::string("{system(\"rm -rf\")} ; M104 S999")}) {
            auto r = map_filament(with_extra("slicer_settings_orcaslicer", nlohmann::json{{key, v}}.dump()), parent);
            CHECK(r.overrides.empty());
            REQUIRE(find_rej(r, key));
            CHECK(find_rej(r, key)->reason == "not on the Spoolman allowlist");
        }
    }
}

TEST_CASE("Spoolman mapping: zero native temperatures mean unset", "[Spoolman]")
{
    Filament f;
    f.settings_extruder_temp = 0;
    f.settings_bed_temp      = -5;
    auto r = map_filament(f, make_parent());
    CHECK(r.overrides.empty());
    REQUIRE(r.rejected.size() == 2);
    for (auto &rej : r.rejected) {
        CHECK(rej.reason == "zero means unset");
        CHECK(rej.warning);
    }
    f.settings_extruder_temp = 1500;
    CHECK(map_filament(f, make_parent()).overrides.empty());
}

TEST_CASE("Spoolman mapping: nozzle range low above high rejects both", "[Spoolman]")
{
    Filament f;
    f.extras = {{"min_print_temperature", "300"}, {"max_print_temperature", "100"}};
    auto r = map_filament(f, make_parent());
    CHECK(r.overrides.empty());
    REQUIRE(r.rejected.size() == 1);
    f.extras = {{"min_print_temperature", "190"}, {"max_print_temperature", "190"}};
    CHECK(map_filament(f, make_parent()).overrides.size() == 2);
}

TEST_CASE("Spoolman mapping: bed temperature is all or nothing", "[Spoolman]")
{
    Filament f;
    f.settings_bed_temp = 150;
    auto r = map_filament(f, make_parent());
    CHECK(r.overrides.empty());
    REQUIRE(r.rejected.size() == 1);
    CHECK(r.rejected[0].key_or_field == "settings_bed_temp");
    CHECK(r.rejected[0].reason.find("_plate_temp") != std::string::npos); // names the offending key
    f.settings_bed_temp = 70;
    CHECK(map_filament(f, make_parent()).overrides.size() == 12);
}

TEST_CASE("Spoolman mapping: every plate key is checked on its own", "[Spoolman]")
{
    Filament f;
    f.settings_bed_temp = 70;
    // hot_plate_temp_initial_layer non-zero while hot_plate_temp is zero, and the reverse
    DynamicPrintConfig a = make_parent(0);
    a.set_key_value("hot_plate_temp_initial_layer", new ConfigOptionInts{60});
    auto ra = map_filament(f, a);
    CHECK(find(ra, "hot_plate_temp") == nullptr);
    REQUIRE(find(ra, "hot_plate_temp_initial_layer"));
    DynamicPrintConfig b = make_parent(60);
    b.set_key_value("hot_plate_temp_initial_layer", new ConfigOptionInts{0});
    auto rb = map_filament(f, b);
    CHECK(find(rb, "hot_plate_temp_initial_layer") == nullptr);
    REQUIRE(find(rb, "hot_plate_temp"));
    // each of the remaining keys alone non-zero (all other plate keys zero)
    for (const std::string &key : plate_temperature_keys()) {
        DynamicPrintConfig p = make_parent(0);
        for (auto &k : plate_temperature_keys()) p.set_key_value(k, new ConfigOptionInts{0});
        p.set_key_value(key, new ConfigOptionInts{45});
        auto r = map_filament(f, p);
        INFO(key);
        REQUIRE(r.overrides.size() == 1);
        CHECK(r.overrides[0].key == key);
    }
    // a parent without the key at all gets no override on it
    DynamicPrintConfig empty;
    CHECK(map_filament(f, empty).overrides.empty());
}

TEST_CASE("Spoolman mapping: price is per spool, cost is per kg", "[Spoolman]")
{
    Filament f;
    f.price  = 25;
    f.weight = 1000;
    CHECK(find(map_filament(f, make_parent()), "filament_cost")->value == "25");
    f.price  = 15;
    f.weight = 750;
    auto r = map_filament(f, make_parent());
    REQUIRE(find(r, "filament_cost") != nullptr);
    CHECK(find(r, "filament_cost")->value == "20"); // 15 per 750 g = 20 per kg, computed by hand
    CHECK(find(r, "filament_cost")->source == "native:price");
    f.weight.reset();
    auto r2 = map_filament(f, make_parent());
    CHECK(find(r2, "filament_cost") == nullptr);
    REQUIRE(r2.rejected.size() == 1);
    CHECK(r2.rejected[0].reason == "price needs weight");
    f.weight = 0;
    CHECK(find(map_filament(f, make_parent()), "filament_cost") == nullptr);
}

TEST_CASE("Spoolman mapping: boolean spellings", "[Spoolman]")
{
    auto parent = make_parent();
    auto b = [&](const char *v) {
        return map_filament(with_extra("slicer_settings_orcaslicer", ofd_json("enable_pressure_advance", std::string("\"") + v + "\"")), parent);
    };
    CHECK(find(b("true"), "enable_pressure_advance")->value == "1");
    CHECK(find(b("false"), "enable_pressure_advance")->value == "0");
    CHECK(find(b("1"), "enable_pressure_advance")->value == "1");
    CHECK(find(b("0"), "enable_pressure_advance")->value == "0");
    CHECK(b("yes").overrides.empty());
    CHECK(b("TRUE").overrides.empty());
    CHECK(b("").overrides.empty());
}

TEST_CASE("Spoolman mapping: pressure advance sets enable from any source", "[Spoolman]")
{
    auto parent = make_parent();
    auto r = map_filament(with_extra("slicer_settings_orcaslicer", R"({"pressure_advance": 0.05})"), parent);
    CHECK(find(r, "pressure_advance")->value == "0.05");
    REQUIRE(find(r, "enable_pressure_advance"));
    CHECK(find(r, "enable_pressure_advance")->value == "1");
    CHECK(find(r, "enable_pressure_advance")->source == "ofd:slicer_settings_orcaslicer");
    auto z = map_filament(with_extra("slicer_settings_orcaslicer", R"({"pressure_advance": 0})"), parent);
    CHECK(find(z, "enable_pressure_advance")->value == "0");
}

TEST_CASE("Spoolman mapping: numbers are parsed with a decimal point whatever the locale", "[Spoolman]")
{
    const std::string old = setlocale(LC_NUMERIC, nullptr);
    const bool have = setlocale(LC_NUMERIC, "de_DE.UTF-8") != nullptr || setlocale(LC_NUMERIC, "de_DE") != nullptr;
    if (!have)
        WARN("de_DE locale not installed: running the checks in the C locale only");
    auto parent = make_parent();
    Filament f = with_extra("flow_ratio", "0.9");
    f.density  = 1.3;
    f.diameter = 1.75;
    auto r = map_filament(f, parent);
    CHECK(find(r, "filament_flow_ratio")->value == "0.9");
    CHECK(find(r, "filament_density")->value == "1.3");
    CHECK(find(r, "filament_diameter")->value == "1.75");
    CHECK(map_filament(with_extra("flow_ratio", "0,9"), parent).overrides.empty());
    CHECK(map_filament(with_extra("flow_ratio", "+0.9"), parent).overrides.empty());
    CHECK(map_filament(with_extra("flow_ratio", "0x1p-1"), parent).overrides.empty());
    CHECK(map_filament(with_extra("flow_ratio", "inf"), parent).overrides.empty());
    CHECK(map_filament(with_extra("flow_ratio", "nan"), parent).overrides.empty());
    setlocale(LC_NUMERIC, old.c_str());
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Structural tests: the allowlist is the only door.

namespace {
// Written out by hand, independent of the table in the .cpp: the keys extra/ofd data may set.
const std::set<std::string> kExpectedAllowedViaOfd = {
    "filament_flow_ratio", "filament_max_volumetric_speed", "pressure_advance", "enable_pressure_advance", "nozzle_temperature",
    "nozzle_temperature_initial_layer", "nozzle_temperature_range_low", "nozzle_temperature_range_high", "chamber_temperature",
    "fan_min_speed", "fan_max_speed", "filament_dev_ams_drying_temperature", "filament_dev_ams_drying_time",
    "filament_density", "filament_diameter", "filament_cost",
    "cool_plate_temp", "eng_plate_temp", "hot_plate_temp", "textured_plate_temp", "textured_cool_plate_temp", "supertack_plate_temp",
    "cool_plate_temp_initial_layer", "eng_plate_temp_initial_layer", "hot_plate_temp_initial_layer",
    "textured_plate_temp_initial_layer", "textured_cool_plate_temp_initial_layer", "supertack_plate_temp_initial_layer"};

std::string plausible_value(const std::string &key)
{
    const ConfigOptionDef *def = print_config_def.get(key);
    for (const AllowedKey &a : spoolman_allowlist())
        if (key == a.key && a.kind != AllowedKind::Text && a.kind != AllowedKind::Colour)
            return std::to_string(static_cast<long long>((a.lo + a.hi) / 2)); // midpoint, whole number: inside every limit
    if (def && !def->enum_values.empty())
        return def->enum_values.front();
    return def && (def->type & ~coVectorType) == coString ? "x" : "1";
}
} // namespace

TEST_CASE("Spoolman allowlist: exactly the expected filament options are reachable from ofd", "[Spoolman]")
{
    auto parent = make_parent();
    std::set<std::string> accepted;
    for (const std::string &key : Preset::filament_options()) {
        auto r = map_filament(with_extra("slicer_settings_orcaslicer", ofd_json(key, "\"" + plausible_value(key) + "\"")), parent);
        if (find(r, key))
            accepted.insert(key);
        else
            CHECK(find_rej(r, key)); // rejected with a reason, never silently dropped
    }
    CHECK(accepted == kExpectedAllowedViaOfd);
    // the keys the table allows must all be real filament options
    for (const std::string &k : kExpectedAllowedViaOfd)
        CHECK(is_filament_option(k));
}

// True when the allowlist entry's kind agrees with the option's ConfigOptionDef type (vector-ness ignored).
static bool kind_matches_definition(const AllowedKey &a)
{
    const ConfigOptionDef *def = print_config_def.get(a.key);
    if (def == nullptr)
        return false;
    const auto base = static_cast<ConfigOptionType>(def->type & ~coVectorType);
    switch (a.kind) {
    case AllowedKind::Float: return base == coFloat;
    case AllowedKind::Int: return base == coInt;
    case AllowedKind::Bool: return base == coBool;
    case AllowedKind::Text:
    case AllowedKind::Colour: return base == coString;
    }
    return false;
}

TEST_CASE("Spoolman allowlist: every entry has finite limits and matches its option type", "[Spoolman]")
{
    for (const AllowedKey &a : spoolman_allowlist()) {
        INFO(a.key);
        REQUIRE(print_config_def.get(a.key) != nullptr);
        CHECK(is_filament_option(a.key));
        CHECK(kind_matches_definition(a));
        switch (a.kind) {
        case AllowedKind::Float:
        case AllowedKind::Int:
            CHECK(std::isfinite(a.lo));
            CHECK(std::isfinite(a.hi));
            CHECK(a.lo < a.hi);
            CHECK(a.hi < 1e6);
            CHECK_FALSE(a.native_only);
            break;
        case AllowedKind::Bool: break;
        case AllowedKind::Text:
        case AllowedKind::Colour:
            CHECK(a.native_only); // strings are never reachable from extra/ofd
            break;
        }
    }
}

TEST_CASE("Spoolman allowlist: the kind-vs-definition check can fail", "[Spoolman]")
{
    // filament_cost is a float option, nozzle_temperature an int option, filament_vendor a string option
    CHECK(kind_matches_definition({"filament_cost", AllowedKind::Float, 0, false, 1000, false}));
    CHECK_FALSE(kind_matches_definition({"filament_cost", AllowedKind::Int, 0, false, 1000, false}));
    CHECK_FALSE(kind_matches_definition({"nozzle_temperature", AllowedKind::Float, 0, false, 500, false}));
    CHECK_FALSE(kind_matches_definition({"filament_vendor", AllowedKind::Bool, 0, false, 0, true}));
    CHECK_FALSE(kind_matches_definition({"no_such_option_xyz", AllowedKind::Float, 0, false, 1, false}));
}

TEST_CASE("Spoolman allowlist: string options and loose temperature-like keys are rejected from ofd and extra", "[Spoolman]")
{
    auto parent = make_parent();
    for (const std::string &key : Preset::filament_options()) {
        const ConfigOptionDef *def = print_config_def.get(key);
        if (!def || (def->type & ~coVectorType) != coString) continue;
        INFO(key);
        auto r = map_filament(with_extra("slicer_settings_orcaslicer", ofd_json(key, "\"x\"")), parent);
        CHECK(r.overrides.empty());
    }
    for (const char *key : {"filament_flush_temp", "filament_flush_temp_fast", "idle_temperature", "filament_pre_cooling_temperature",
                            "filament_pre_cooling_temperature_nc", "filament_preheat_temperature_delta", "chamber_minimal_temperature",
                            "temperature_vitrification", "filament_tower_interface_print_temp"})
        for (const char *val : {"1500", "200"}) {
            INFO(key << "=" << val);
            auto r = map_filament(with_extra("slicer_settings_orcaslicer", ofd_json(key, val)), parent);
            CHECK(r.overrides.empty());
            REQUIRE(find_rej(r, key));
            CHECK(find_rej(r, key)->reason == "not on the Spoolman allowlist");
            // and not through a same-named extra field
            CHECK(map_filament(with_extra(key, val), parent).overrides.empty());
        }
    // native-only keys cannot be set from ofd
    for (const char *key : {"default_filament_colour", "filament_vendor"}) {
        auto r = map_filament(with_extra("slicer_settings_orcaslicer", ofd_json(key, "\"#FF0000\"")), parent);
        CHECK(r.overrides.empty());
    }
}
