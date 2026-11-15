// Save-time Snapmaker shape of a project config with mixed slots (ticket #122, slice 1: pure transform, not wired).
//
// What these tests cannot prove: the expected rows come from this build's serializer (which mirrors Snapmaker's)
// plus one row string copied from a real Snapmaker Orca 2.4.1 project, so they show agreement with those, not that
// Snapmaker Orca reopens the result with the right virtual filaments. That needs the manual check in the record.
#include <catch2/catch_all.hpp>

#include <nlohmann/json.hpp>

#include "libslic3r/MixedFilamentLegacy.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

namespace {

// The 6 rows Snapmaker Orca 2.4.1 wrote for 4 physical filaments (auto generate on), byte for byte.
const std::string k_real_snapmaker_4 =
    "1,2,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u19;1,3,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u20;"
    "1,4,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u21;2,3,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u22;"
    "2,4,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u23;3,4,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u24";

DynamicPrintConfig project_config(size_t n)
{
    DynamicPrintConfig c = DynamicPrintConfig::full_print_config();
    std::vector<std::string> cols, ids;
    static const char *palette[] = { "#FF0000", "#00FF00", "#0000FF", "#FFFF00", "#00FFFF", "#FF00FF", "#101010", "#202020" };
    for (size_t i = 0; i < n; ++i) {
        cols.push_back(palette[i % 8]);
        ids.push_back("Preset" + std::to_string(i + 1));
    }
    c.set_key_value("filament_colour", new ConfigOptionStrings(cols));
    c.set_key_value("filament_multi_colour", new ConfigOptionStrings(cols));
    c.set_key_value("filament_colour_type", new ConfigOptionStrings(std::vector<std::string>(n, "1")));
    c.set_key_value("filament_settings_id", new ConfigOptionStrings(ids));
    c.set_key_value("filament_map", new ConfigOptionInts(std::vector<int>(n, 1)));
    return c;
}

// n physical filaments, then one converted (marker) slot per row of `defs`.
DynamicPrintConfig converted(size_t n, const std::string &defs)
{
    DynamicPrintConfig c = project_config(n);
    REQUIRE(convert_legacy_mixed_filaments(c, defs).slots_added > 0);
    return c;
}

const std::vector<std::string> &strs(const DynamicPrintConfig &c, const char *key) { return c.option<ConfigOptionStrings>(key)->values; }

std::vector<std::string> split_rows(const std::string &s)
{
    std::vector<std::string> rows;
    size_t                   pos = 0;
    while (pos <= s.size()) {
        size_t e = s.find(';', pos);
        if (e == std::string::npos)
            e = s.size();
        rows.push_back(s.substr(pos, e - pos));
        pos = e + 1;
    }
    return rows;
}

// Every row must load in Snapmaker as exactly one live virtual filament.
void check_row_valid(const std::string &row, size_t n)
{
    LegacyMixedRow r;
    REQUIRE(parse_legacy_mixed_row(row, r));
    CHECK(r.a >= 1);
    CHECK(r.b >= 1);
    CHECK(r.a <= n);
    CHECK(r.b <= n);
    CHECK(r.a != r.b);
    CHECK(r.enabled);
    CHECK_FALSE(r.deleted);
}

// 4 physical + converted marker slots 5,6 (custom rows) + slot 7 converted then made native (empty marker).
DynamicPrintConfig two_converted_one_native()
{
    DynamicPrintConfig d = converted(4, "1,2,1,1,30,0,g,w,m2,z0,xa0,xb0,d0,o0,u7;2,3,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u9;3,4,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u11");
    d.option<ConfigOptionStrings>("filament_mixed_engine_row")->values[6]      = "";
    d.option<ConfigOptionStrings>("filament_mixed_components")->values[6]      = "3,4";
    d.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios")->values[6] = "0.7000,0.3000";
    return d;
}

struct Shaped : SnapmakerProjectShape
{
    DynamicPrintConfig config;
};

Shaped shape_of(const DynamicPrintConfig &full)
{
    Shaped s;
    static_cast<SnapmakerProjectShape &>(s) = make_snapmaker_project_shape(full, s.config);
    return s;
}

} // namespace

TEST_CASE("Snapmaker shape: a config without mixed slots comes back unchanged", "[MixedFilament][snapmaker_shape]")
{
    const DynamicPrintConfig c = project_config(3);
    const Shaped s = shape_of(c);
    CHECK_FALSE(s.transformed);
    CHECK(s.mixed_filament_definitions.empty());
    CHECK(s.sidecar.empty());
    CHECK(s.config == c);
}

TEST_CASE("Snapmaker shape: Snapmaker's own 2.4.1 rows survive a convert and save byte for byte", "[MixedFilament][snapmaker_shape]")
{
    const DynamicPrintConfig c = converted(4, k_real_snapmaker_4);
    REQUIRE(strs(c, "filament_colour").size() == 10);
    const Shaped s = shape_of(c);
    REQUIRE(s.transformed);
    CHECK(s.mixed_filament_definitions == k_real_snapmaker_4);
    CHECK(strs(s.config, "filament_colour").size() == 4);
    CHECK(strs(s.config, "filament_settings_id").size() == 4);
}

TEST_CASE("Snapmaker shape: n physical plus k mixed slots", "[MixedFilament][snapmaker_shape]")
{
    const DynamicPrintConfig c = two_converted_one_native();
    REQUIRE(strs(c, "filament_colour").size() == 7);
    const DynamicPrintConfig before = c;
    const Shaped s = shape_of(c);
    REQUIRE(s.transformed);
    CHECK(s.config.option<ConfigOptionBools>("filament_is_mixed")->values == std::vector<unsigned char>(4, 0));
    for (const char *key : { "filament_colour", "filament_multi_colour", "filament_colour_type", "filament_settings_id",
                             "filament_mixed_components", "filament_mixed_sublayer_ratios", "filament_mixed_engine_row" })
        CHECK(strs(s.config, key).size() == 4);
    CHECK(s.config.option<ConfigOptionInts>("filament_map")->values.size() == 4);
    CHECK(strs(s.config, "filament_settings_id") == std::vector<std::string>({ "Preset1", "Preset2", "Preset3", "Preset4" }));

    const std::vector<std::string> rows = split_rows(s.mixed_filament_definitions);
    REQUIRE(rows.size() == 3);
    // slots 5 and 6: marker rows verbatim, in slot order
    CHECK(rows[0] == strs(c, "filament_mixed_engine_row")[4]);
    CHECK(rows[1] == strs(c, "filament_mixed_engine_row")[5]);
    CHECK(rows[0].rfind("1,2,1,1,30,", 0) == 0);
    CHECK(rows[1].rfind("2,3,1,1,50,", 0) == 0);
    // slot 7: generated for the native 3,4 at 0.7/0.3
    LegacyMixedRow r;
    REQUIRE(parse_legacy_mixed_row(rows[2], r));
    CHECK(r.a == 3);
    CHECK(r.b == 4);
    CHECK(r.mix_b_percent == 30);
    CHECK(r.custom);
    CHECK_FALSE(r.origin_auto);
    CHECK(r.enabled);
    // stable ids: the generated row must not collide with the marker rows' u7 and u9
    CHECK(r.stable_id > 9);
    for (const std::string &row : rows)
        check_row_valid(row, 4);
    // ids line up: the parser, run with n = 4, finds all three as live rows 5, 6, 7
    CHECK(parse_legacy_mixed_definitions(s.mixed_filament_definitions, 4).rows.size() == 3);
    CHECK(c == before); // input untouched
}

TEST_CASE("Snapmaker shape: degenerate slots get a valid placeholder row and keep the count", "[MixedFilament][snapmaker_shape]")
{
    DynamicPrintConfig c = two_converted_one_native();
    SECTION("0 sentinel after a physical delete") {
        c.option<ConfigOptionStrings>("filament_mixed_components")->values[6] = "0,4";
        c.option<ConfigOptionStrings>("filament_mixed_engine_row")->values[6] = "";
    }
    SECTION("a == b") {
        c.option<ConfigOptionStrings>("filament_mixed_components")->values[6] = "2,2";
    }
    SECTION("single surviving component") {
        c.option<ConfigOptionStrings>("filament_mixed_components")->values[6] = "3";
    }
    SECTION("component above the physical count") {
        c.option<ConfigOptionStrings>("filament_mixed_components")->values[6] = "3,9";
    }
    SECTION("a marker row Snapmaker would skip (a == b) is replaced, not emitted") {
        c.option<ConfigOptionStrings>("filament_mixed_engine_row")->values[5] = "2,2,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u9";
    }
    SECTION("a marker row that is disabled holds no virtual id, so it is replaced") {
        c.option<ConfigOptionStrings>("filament_mixed_engine_row")->values[5] = "2,3,0,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u9";
    }
    const Shaped s = shape_of(c);
    REQUIRE(s.transformed);
    const std::vector<std::string> rows = split_rows(s.mixed_filament_definitions);
    REQUIRE(rows.size() == 3);
    for (const std::string &row : rows)
        check_row_valid(row, 4);
    CHECK_FALSE(s.warnings.empty());
}

TEST_CASE("Snapmaker shape: 3+ components are shown as their first pair, with a warning", "[MixedFilament][snapmaker_shape]")
{
    DynamicPrintConfig c = two_converted_one_native();
    c.option<ConfigOptionStrings>("filament_mixed_components")->values[6]      = "1,2,3";
    c.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios")->values[6] = "0.5000,0.2500,0.2500";
    const Shaped s = shape_of(c);
    REQUIRE(s.transformed);
    LegacyMixedRow r;
    REQUIRE(parse_legacy_mixed_row(split_rows(s.mixed_filament_definitions)[2], r));
    CHECK(r.a == 1);
    CHECK(r.b == 2);
    CHECK(r.mix_b_percent == 33);
    CHECK(s.warnings.size() == 1);
    // the full data is in the sidecar
    const nlohmann::json side = nlohmann::json::parse(s.sidecar);
    CHECK(side["keys"]["filament_mixed_components"]["v"].get<std::string>().find("1,2,3") != std::string::npos);
}

TEST_CASE("Snapmaker shape: the sidecar carries version, n, total, digest and the stripped tails", "[MixedFilament][snapmaker_shape]")
{
    const Shaped s = shape_of(two_converted_one_native());
    REQUIRE(s.transformed);
    const nlohmann::json side = nlohmann::json::parse(s.sidecar);
    CHECK(side["version"].get<int>() == kMixedSidecarVersion);
    CHECK(side["n"].get<size_t>() == 4);
    CHECK(side["total"].get<size_t>() == 7);
    CHECK(side["digest"].get<std::string>() == mixed_sidecar_digest(4, s.mixed_filament_definitions));
    CHECK(side["digest"].get<std::string>().size() == 16);
    // a different string or n gives a different digest
    CHECK(mixed_sidecar_digest(4, s.mixed_filament_definitions + "x") != side["digest"].get<std::string>());
    CHECK(mixed_sidecar_digest(5, s.mixed_filament_definitions) != side["digest"].get<std::string>());
    REQUIRE(side["keys"].contains("filament_settings_id"));
    CHECK(side["keys"]["filament_settings_id"]["idx"] == nlohmann::json::array({ 4, 5, 6 }));
    // the per-slot preset of the stripped slots is kept (converted slots take component A's preset)
    const std::string tail = side["keys"]["filament_settings_id"]["v"].get<std::string>();
    CHECK(tail.find("Preset1") != std::string::npos);
    CHECK(tail.find("Preset2") != std::string::npos);
    CHECK(tail.find("Preset3") != std::string::npos);
}

TEST_CASE("Snapmaker shape: idempotent on an already shaped config", "[MixedFilament][snapmaker_shape]")
{
    const Shaped first  = shape_of(two_converted_one_native());
    REQUIRE(first.transformed);
    const Shaped second = shape_of(first.config);
    CHECK_FALSE(second.transformed);
    CHECK(second.config == first.config);
    CHECK(second.mixed_filament_definitions.empty());
    CHECK(second.sidecar.empty());
}

TEST_CASE("Snapmaker shape: the input is not mutated", "[MixedFilament][snapmaker_shape]")
{
    const DynamicPrintConfig c      = two_converted_one_native();
    const DynamicPrintConfig before = c;
    shape_of(c);
    CHECK(c == before);
    CHECK(strs(c, "filament_colour").size() == 7);
}

TEST_CASE("Snapmaker shape: inverts the converter's extension, variants and groups included", "[MixedFilament][snapmaker_shape]")
{
    DynamicPrintConfig base = project_config(3);
    base.set_key_value("filament_self_index", new ConfigOptionInts({ 1, 1, 2, 2, 3, 3 }));
    base.set_key_value("filament_extruder_variant", new ConfigOptionStrings({ "L", "R", "L", "R", "L", "R" }));
    base.set_key_value("nozzle_temperature", new ConfigOptionInts({ 200, 201, 210, 211, 220, 221 }));
    base.set_key_value("inherits_group", new ConfigOptionStrings({ "P", "F1", "F2", "F3", "M" }));
    DynamicPrintConfig c = base;
    REQUIRE(convert_legacy_mixed_filaments(c, "2,3,1,1,40,0,g,w,m2,z0,xa0,xb0,d0,o0,u1;1,3,1,1,60,0,g,w,m2,z0,xa0,xb0,d0,o0,u2").slots_added == 2);
    REQUIRE(c.option<ConfigOptionInts>("nozzle_temperature")->values.size() == 10);
    REQUIRE(c.option<ConfigOptionStrings>("inherits_group")->values.size() == 7);
    const Shaped s = shape_of(c);
    REQUIRE(s.transformed);
    for (const char *key : { "filament_self_index", "filament_extruder_variant", "nozzle_temperature", "inherits_group", "filament_colour",
                             "filament_settings_id", "filament_map", "filament_colour_type" })
        { INFO(key); CHECK(*s.config.option(key) == *base.option(key)); }
}

TEST_CASE("Snapmaker shape: a non-contiguous mixed tail or fewer than two physical filaments is left alone", "[MixedFilament][snapmaker_shape]")
{
    DynamicPrintConfig c = two_converted_one_native();
    c.option<ConfigOptionBools>("filament_is_mixed")->values[5] = 0;
    Shaped s = shape_of(c);
    CHECK_FALSE(s.transformed);
    CHECK(s.config == c);
    CHECK_FALSE(s.warnings.empty());

    DynamicPrintConfig one = project_config(1);
    resize_mixed_filament_metadata(one, 1, 1);
    one.option<ConfigOptionBools>("filament_is_mixed")->values[0] = 1;
    s = shape_of(one);
    CHECK_FALSE(s.transformed);
}
