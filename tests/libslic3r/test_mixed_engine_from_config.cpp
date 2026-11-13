// Slice 4b of the mixed-filament engine port: config -> engine (mixed_engine_from_config), the marker the
// slice-1 converter writes, the delete-physical remap of engine rows, and the range clamping of the global keys.
//
// What these cannot catch: the converter-vs-direct comparison feeds both engines from the same Snapmaker
// row strings through the same row parser, so a parser misreading would agree on both sides; the golden
// transcripts of test_mixed_filament_engine.cpp pin the arithmetic, not this plumbing.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "libslic3r/Format/STL.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/MixedFilamentEngine.hpp"
#include "libslic3r/MixedFilamentLegacy.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "test_utils.hpp"

using namespace Slic3r;

namespace {

// 4 physical filaments + the given mixed slots appended (flag, components, engine row).
struct SlotSpec { const char *components; const char *row; };

DynamicPrintConfig config_with(size_t num_physical, const std::vector<SlotSpec> &mixed)
{
    DynamicPrintConfig c = DynamicPrintConfig::full_print_config();
    const size_t       total = num_physical + mixed.size();
    std::vector<std::string>   cols(total, "#808080"), comps(total), rows(total);
    std::vector<unsigned char> flags(total, 0);
    for (size_t k = 0; k < mixed.size(); ++k) {
        flags[num_physical + k] = 1;
        comps[num_physical + k] = mixed[k].components;
        rows[num_physical + k]  = mixed[k].row;
    }
    c.set_key_value("filament_colour", new ConfigOptionStrings(cols));
    c.set_key_value("filament_is_mixed", new ConfigOptionBools(flags));
    c.set_key_value("filament_mixed_components", new ConfigOptionStrings(comps));
    c.set_key_value("filament_mixed_engine_row", new ConfigOptionStrings(rows));
    return c;
}

const char *kRowA = "1,2,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u1";
const char *kRowB = "1,3,1,1,25,0,g,w,m0,z0,xa0,xb0,d0,o0,u2,cm1";
const char *kRowC = "2,4,1,1,75,0,g,w,m2,z0,xa0,xb0,d0,o0,u3";

DynamicPrintConfig store_and_reload(DynamicPrintConfig config, const std::string &path)
{
    Model model;
    REQUIRE(load_stl((std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl").c_str(), &model));
    model.add_default_instances();
    ScopedTemporaryDir backup_dir("orca_engine_cfg_src");
    model.set_backup_path(backup_dir.string());
    PlateData *plate = new PlateData();
    plate->plate_index = 0;
    StoreParams sp;
    sp.path     = path.c_str();
    sp.model    = &model;
    sp.config   = &config;
    sp.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
    sp.plate_data_list.push_back(plate);
    REQUIRE(store_bbs_3mf(sp));
    delete plate;

    Model dst_model;
    ScopedTemporaryDir dst_backup_dir("orca_engine_cfg_dst");
    dst_model.set_backup_path(dst_backup_dir.string());
    DynamicPrintConfig dst_config;
    ConfigSubstitutionContext ctxt{ForwardCompatibilitySubstitutionRule::Enable};
    PlateDataPtrs dst_plates;
    std::vector<Preset *> project_presets;
    bool is_bbl_3mf = false, is_orca_3mf = false;
    Semver file_version;
    REQUIRE(load_bbs_3mf(path.c_str(), &dst_config, &ctxt, &dst_model, &dst_plates, &project_presets, &is_bbl_3mf, &is_orca_3mf,
                         &file_version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
    release_PlateData_list(dst_plates);
    return dst_config;
}

} // namespace

TEST_CASE("mixed_engine_from_config: ids align with slot positions and num_physical is size minus mixed", "[MixedFilament][EngineConfig]")
{
    const DynamicPrintConfig c = config_with(4, { {"1,2", kRowA}, {"1,3", kRowB}, {"2,4", kRowC} });
    MixedEngineSlotsReport   rep;
    const MixedFilamentEngine e = mixed_engine_from_config(c, &rep);

    REQUIRE(rep.valid);
    CHECK(rep.num_physical == 4); // 7 slots, 3 mixed; size() would have said 7
    CHECK(rep.num_mixed == 3);
    CHECK(rep.warnings.empty());
    CHECK(e.num_physical() == 4);
    REQUIRE(e.mixed_filaments().size() == 3);
    CHECK(e.mixed_filament_from_id(5, 4)->component_b == 2);
    CHECK(e.mixed_filament_from_id(6, 4)->component_b == 3);
    CHECK(e.mixed_filament_from_id(6, 4)->ui_mode == 1);
    CHECK(e.mixed_filament_from_id(7, 4)->component_a == 2);
    CHECK(e.mixed_filament_from_id(7, 4)->mix_b_percent == 75);
    CHECK(e.mixed_filament_from_id(4, 4) == nullptr); // physical

    // PrintConfig (static) takes the same path as DynamicPrintConfig.
    PrintConfig pc;
    pc.apply(c, true);
    const MixedFilamentEngine e2 = mixed_engine_from_config(pc);
    REQUIRE(e2.mixed_filaments().size() == 3);
    CHECK(e2.mixed_filament_from_id(7, 4)->component_a == 2);
}

TEST_CASE("mixed_engine_from_config: a native slot between engine rows keeps later ids aligned", "[MixedFilament][EngineConfig]")
{
    const DynamicPrintConfig c = config_with(4, { {"1,2", kRowA}, {"3,4", ""}, {"2,4", kRowC} });
    MixedEngineSlotsReport   rep;
    const MixedFilamentEngine e = mixed_engine_from_config(c, &rep);
    REQUIRE(rep.valid);
    REQUIRE(e.mixed_filaments().size() == 3);
    CHECK_FALSE(is_engine_row(e.mixed_filaments()[1])); // native placeholder, never resolved here
    CHECK(e.mixed_filament_from_id(6, 4)->component_a == 3);
    CHECK(e.mixed_filament_from_id(7, 4)->component_a == 2); // not shifted down to 6
    CHECK(e.mixed_filament_from_id(7, 4)->mix_b_percent == 75);
}

TEST_CASE("mixed_engine_from_config: the empty marker means native, no engine and no warning", "[MixedFilament][EngineConfig]")
{
    const DynamicPrintConfig c = config_with(4, { {"1,2", ""}, {"3,4", ""} });
    MixedEngineSlotsReport   rep;
    const MixedFilamentEngine e = mixed_engine_from_config(c, &rep);
    CHECK_FALSE(rep.valid);
    CHECK(rep.warnings.empty());
    CHECK(e.mixed_filaments().empty());

    // A config with no such key at all (older project) is the same.
    DynamicPrintConfig bare;
    bare.set_key_value("filament_colour", new ConfigOptionStrings({"#000000", "#111111"}));
    CHECK(mixed_engine_from_config(bare, &rep).mixed_filaments().empty());
    CHECK(rep.warnings.empty());
}

TEST_CASE("mixed_engine_from_config: inconsistent or malformed input gives an empty engine and a warning, never a crash", "[MixedFilament][EngineConfig]")
{
    auto expect_disabled = [](const DynamicPrintConfig &c) {
        MixedEngineSlotsReport rep;
        const MixedFilamentEngine e = mixed_engine_from_config(c, &rep);
        CHECK_FALSE(rep.valid);
        CHECK(e.mixed_filaments().empty());
        CHECK(rep.warnings.size() >= 1);
    };

    SECTION("malformed row") { expect_disabled(config_with(4, { {"1,2", kRowA}, {"1,3", "not a row"} })); }
    SECTION("truncated row") { expect_disabled(config_with(4, { {"1,2", "1,2,1"} })); }
    SECTION("component outside the physical range") { expect_disabled(config_with(4, { {"1,2", "1,9,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u1"} })); }
    SECTION("component zero (deleted sentinel)") { expect_disabled(config_with(4, { {"0,2", "0,2,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u1"} })); }
    SECTION("several rows in one marker") { expect_disabled(config_with(4, { {"1,2", std::string(std::string(kRowA) + ";" + kRowB).c_str()} })); }
    SECTION("marker on a physical slot")
    {
        DynamicPrintConfig c = config_with(4, { {"1,2", kRowA} });
        c.option<ConfigOptionStrings>("filament_mixed_engine_row")->values[0] = kRowB;
        expect_disabled(c);
    }
    SECTION("mixed slot not in the tail")
    {
        DynamicPrintConfig c = config_with(4, { {"1,2", kRowA} });
        c.option<ConfigOptionBools>("filament_is_mixed")->values = {1, 0, 0, 0, 0};
        expect_disabled(c);
    }
    SECTION("more rows than slots")
    {
        DynamicPrintConfig c = config_with(4, { {"1,2", kRowA} });
        c.option<ConfigOptionStrings>("filament_mixed_engine_row")->values.resize(40, kRowA);
        expect_disabled(c);
    }
    SECTION("no physical slot left")
    {
        DynamicPrintConfig c = config_with(1, { {"1,1", kRowA} });
        c.option<ConfigOptionBools>("filament_is_mixed")->values = {1, 1};
        expect_disabled(c);
    }
    SECTION("missing is_mixed")
    {
        DynamicPrintConfig c = config_with(4, { {"1,2", kRowA} });
        c.erase("filament_is_mixed");
        expect_disabled(c);
    }
}

TEST_CASE("mixed_engine_from_config: hostile strings", "[MixedFilament][EngineConfig]")
{
    auto build = [](const std::string &row) {
        MixedEngineSlotsReport rep;
        MixedFilamentEngine e = mixed_engine_from_config(config_with(4, { {"1,2", row.c_str()} }), &rep);
        return std::make_pair(rep.valid, e.mixed_filaments().size());
    };
    CHECK(build(std::string(100000, '1')) == std::make_pair(false, size_t(0)));
    CHECK(build(std::string(100000, ',')) == std::make_pair(false, size_t(0)));
    CHECK(build("1,2,1,1,99999999999999999999,0,g,w,m2,z0,xa0,xb0,d0,o0,u1") .second <= 1);
    CHECK(build("1,2,1,1,50,0,g,w,m2,z-5,xa1e999,xb-nan,d0,o0,u18446744073709551616").second <= 1);
    CHECK(build(std::string("1,2,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u1,") + std::string(5000, '[') ).second <= 1);
    CHECK(build(std::string("\0\0\0,1,2", 7)).first == false);
    CHECK(build("1,2,1,1,50,0,g1111111111111111111111,w1/1/1,m9,z0,xa0,xb0,d0,o0,u1").second <= 1);
}

TEST_CASE("mixed_engine_from_config: global keys are validated at the consumer", "[MixedFilament][EngineConfig]")
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    SECTION("defaults produce no warning")
    {
        const DynamicPrintConfig c = DynamicPrintConfig::full_print_config();
        std::vector<std::string> w;
        const MixedEngineGlobals g = mixed_engine_globals_from_config(c, &w);
        CHECK(w.empty());
        CHECK(g.height_lower_bound == 0.04f);
        CHECK(g.height_upper_bound == 0.16f);
        CHECK(g.region_collapse);
    }
    SECTION("out-of-range values are clamped with a warning")
    {
        DynamicPrintConfig c = DynamicPrintConfig::full_print_config();
        c.option<ConfigOptionFloat>("mixed_color_layer_height_a")->value        = -3;
        c.option<ConfigOptionFloat>("mixed_color_layer_height_b")->value        = 1e9;
        c.option<ConfigOptionFloat>("mixed_filament_height_lower_bound")->value = 0.0;
        c.option<ConfigOptionFloat>("mixed_filament_height_upper_bound")->value = 1e30;
        c.option<ConfigOptionFloat>("mixed_filament_surface_indentation")->value = -50;
        c.option<ConfigOptionFloat>("mixed_filament_pointillism_pixel_size")->value = -1;
        std::vector<std::string> w;
        const MixedEngineGlobals g = mixed_engine_globals_from_config(c, &w);
        CHECK(g.color_layer_height_a == 0.f);
        CHECK(g.color_layer_height_b == 100.f);
        CHECK(g.height_lower_bound == 0.01f);
        CHECK(g.height_upper_bound == 100.f);
        CHECK(g.surface_indentation == -2.f);
        CHECK(g.pointillism_pixel_size == 0.f);
        CHECK(w.size() == 6);
    }
    SECTION("NaN and infinity fall back to the defaults")
    {
        DynamicPrintConfig c = DynamicPrintConfig::full_print_config();
        c.option<ConfigOptionFloat>("mixed_filament_height_lower_bound")->value = nan;
        c.option<ConfigOptionFloat>("mixed_filament_height_upper_bound")->value = inf;
        c.option<ConfigOptionFloat>("mixed_filament_surface_indentation")->value = -inf;
        const MixedEngineGlobals g = mixed_engine_globals_from_config(c);
        CHECK(g.height_lower_bound == 0.04f);
        CHECK(g.height_upper_bound == 0.16f);
        CHECK(g.surface_indentation == 0.f);
    }
    SECTION("a bad global warns on the engine report but the engine still builds")
    {
        DynamicPrintConfig c = config_with(4, { {"1,2", kRowA} });
        c.option<ConfigOptionFloat>("mixed_filament_height_lower_bound")->value = nan;
        MixedEngineSlotsReport rep;
        const MixedFilamentEngine e = mixed_engine_from_config(c, &rep);
        CHECK(rep.valid);
        CHECK(e.mixed_filaments().size() == 1);
        CHECK(rep.warnings.size() == 1);
    }
}

TEST_CASE("convert_legacy_mixed_filaments writes the engine-row marker for every imported row", "[MixedFilament][EngineConfig][legacy]")
{
    // 4 physical, rows A(enabled) B(disabled) C(enabled): converted slots are 5 and 6 (the disabled row is dropped).
    const std::string defs = "1,2,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u1;"
                             "1,3,0,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u2;"
                             "2,4,1,1,20,0,g,w,m0,z0,xa0.5,xb0,d0,o0,u3,cm1";
    DynamicPrintConfig c = DynamicPrintConfig::full_print_config();
    c.set_key_value("filament_colour", new ConfigOptionStrings(std::vector<std::string>{"#FF0000", "#00FF00", "#0000FF", "#FFFF00"}));
    c.set_key_value("filament_settings_id", new ConfigOptionStrings(std::vector<std::string>{"P1", "P2", "P3", "P4"}));
    const LegacyMixedConversion conv = convert_legacy_mixed_filaments(c, defs);
    REQUIRE(conv.slots_added == 2);

    const auto &rows = c.option<ConfigOptionStrings>("filament_mixed_engine_row")->values;
    REQUIRE(rows.size() == 6);
    CHECK(rows[0].empty());
    CHECK(rows[3].empty()); // physical slots stay unmarked
    CHECK_FALSE(rows[4].empty());
    CHECK_FALSE(rows[5].empty());
    CHECK(rows[4].find(';') == std::string::npos);

    // The marker carries the Snapmaker row: it parses back to the same fields.
    const std::vector<MixedFilament> back5 = parse_mixed_filaments(rows[4], 4);
    const std::vector<MixedFilament> back6 = parse_mixed_filaments(rows[5], 4);
    REQUIRE(back5.size() == 1);
    REQUIRE(back6.size() == 1);
    CHECK(back5[0].component_b == 2);
    CHECK(back6[0].component_a == 2);
    CHECK(back6[0].component_b == 4);
    CHECK(back6[0].mix_b_percent == 20);
    CHECK(back6[0].component_a_surface_offset == 0.5f);
    CHECK(back6[0].ui_mode == 1);
    CHECK(back5[0].stable_id != back6[0].stable_id);

    // And it slices like the Snapmaker rows: engine over the converted config == engine over the original rows.
    MixedEngineSlotsReport rep;
    const MixedFilamentEngine from_cfg = mixed_engine_from_config(c, &rep);
    REQUIRE(rep.valid);
    std::vector<MixedFilament> snap = parse_mixed_filaments(defs, 4);
    snap.erase(snap.begin() + 1); // disabled row
    MixedFilamentEngine direct(snap);
    direct.apply_gradient_settings(0, 0.04f, 0.16f);
    for (unsigned id : {5u, 6u})
        for (int layer = 0; layer < 24; ++layer)
            CHECK(from_cfg.resolve(id, 4, layer, 0.2f * (layer + 1), 0.2f) == direct.resolve(id, 4, layer, 0.2f * (layer + 1), 0.2f));
}

TEST_CASE("Imported engine rows round-trip through a project 3MF", "[MixedFilament][EngineConfig][3mf]")
{
    DynamicPrintConfig c = DynamicPrintConfig::full_print_config();
    c.set_key_value("filament_colour", new ConfigOptionStrings(std::vector<std::string>{"#FF0000", "#00FF00", "#0000FF", "#FFFF00"}));
    c.set_key_value("filament_settings_id", new ConfigOptionStrings(std::vector<std::string>{"P1", "P2", "P3", "P4"}));
    REQUIRE(convert_legacy_mixed_filaments(c, std::string(kRowA) + ";" + kRowB + ";" + kRowC).slots_added == 3);
    const auto written = c.option<ConfigOptionStrings>("filament_mixed_engine_row")->values;

    ScopedTemporaryFile temp(".3mf");
    const DynamicPrintConfig back = store_and_reload(c, temp.string());
    CHECK(back.option<ConfigOptionStrings>("filament_mixed_engine_row")->values == written);

    MixedEngineSlotsReport rep;
    const MixedFilamentEngine e = mixed_engine_from_config(back, &rep);
    REQUIRE(rep.valid);
    CHECK(e.mixed_filaments().size() == 3);
}

TEST_CASE("Deleting a physical filament remaps component ids inside engine rows", "[MixedFilament][EngineConfig]")
{
    const std::vector<unsigned char> is_mixed = {0, 0, 0, 0, 1, 1, 1};
    const std::string rowD = "2,4,1,1,50,0,g234,w40/30/30,m2,z0,xa0,xb0,d0,o0,u7,3,2,[10]4";
    std::vector<std::string> rows = {"", "", "", "", kRowA, "", rowD};

    SECTION("deleting a filament below the references shifts them down")
    {
        remap_mixed_engine_rows_on_delete(is_mixed, rows, 1); // delete filament 1 (0-based 0)
        CHECK(rows[5].empty());                                // native slot untouched
        CHECK(rows[4].rfind("0,1,", 0) == 0); // component 1 was the deleted filament -> 0; component 2 shifted to 1
        CHECK(parse_mixed_filaments(rows[4], 4).empty()); // so the row no longer parses
    }
    SECTION("references above the deleted id shift down, gradient ids and pattern included")
    {
        remap_mixed_engine_rows_on_delete(is_mixed, rows, 1);
        const std::vector<MixedFilament> d = parse_mixed_filaments(rows[6], 12);
        REQUIRE(d.size() == 1);
        CHECK(d[0].component_a == 1);
        CHECK(d[0].component_b == 3);
        CHECK(d[0].gradient_component_ids == "123");
        CHECK(d[0].gradient_component_weights == "40/30/30");
        CHECK(d[0].manual_pattern == "2,1,93"); // [10] shifts to 9, which is a single-digit token
    }
    SECTION("a gradient or pattern that names the deleted id is cleared, not corrupted")
    {
        remap_mixed_engine_rows_on_delete(is_mixed, rows, 3);
        const std::vector<MixedFilament> d = parse_mixed_filaments(rows[6], 12);
        REQUIRE(d.size() == 1);
        CHECK(d[0].component_a == 2);
        CHECK(d[0].component_b == 3);
        CHECK(d[0].gradient_component_ids.empty());
        CHECK(d[0].manual_pattern.empty());
    }
    SECTION("a row naming the deleted filament as a component disables the engine with a warning")
    {
        DynamicPrintConfig c = config_with(4, { {"1,2", kRowA} });
        std::vector<std::string> r = c.option<ConfigOptionStrings>("filament_mixed_engine_row")->values;
        remap_mixed_engine_rows_on_delete(c.option<ConfigOptionBools>("filament_is_mixed")->values, r, 2);
        c.option<ConfigOptionStrings>("filament_mixed_engine_row")->values = r;
        MixedEngineSlotsReport rep;
        CHECK(mixed_engine_from_config(c, &rep).mixed_filaments().empty());
        CHECK_FALSE(rep.valid);
        CHECK(rep.warnings.size() == 1);
    }
    SECTION("garbage rows are left alone")
    {
        std::vector<std::string> g = {"", "", "", "", "garbage", std::string(5000, 'x'), "1,2;1,3"};
        const std::vector<std::string> before = g;
        remap_mixed_engine_rows_on_delete(is_mixed, g, 2);
        CHECK(g == before);
    }
}

TEST_CASE("PresetBundle deleting a physical filament remaps engine rows end to end", "[MixedFilament][EngineConfig][Bundle]")
{
    PresetBundle bundle;
    bundle.set_num_filaments(5u, std::string("#FF0000"));
    bundle.project_config.option<ConfigOptionBools>("filament_is_mixed")->values            = {0, 0, 0, 0, 1};
    bundle.project_config.option<ConfigOptionStrings>("filament_mixed_components")->values  = {"", "", "", "", "2,4"};
    bundle.project_config.option<ConfigOptionStrings>("filament_mixed_engine_row")->values  =
        {"", "", "", "", "2,4,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u9"};

    bundle.update_num_filaments(0); // delete physical filament 1

    CHECK(bundle.project_config.option<ConfigOptionStrings>("filament_mixed_components")->values.back() == "1,3");
    const auto &rows = bundle.project_config.option<ConfigOptionStrings>("filament_mixed_engine_row")->values;
    REQUIRE(rows.size() == 4);
    const std::vector<MixedFilament> r = parse_mixed_filaments(rows.back(), 3);
    REQUIRE(r.size() == 1);
    CHECK(r[0].component_a == 1);
    CHECK(r[0].component_b == 3);
    CHECK(r[0].stable_id == 9);
}
