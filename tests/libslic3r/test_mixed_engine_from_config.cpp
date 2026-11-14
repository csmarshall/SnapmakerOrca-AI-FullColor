// Slice 4b of the mixed-filament engine port: config -> engine (mixed_engine_from_config), the marker the
// slice-1 converter writes, the delete-physical remap of engine rows, and the range clamping of the global keys.
//
// What these cannot catch: the converter-vs-direct comparison feeds both engines from the same Snapmaker
// row strings through the same row parser, so a parser misreading would agree on both sides; the golden
// transcripts of test_mixed_filament_engine.cpp pin the arithmetic, not this plumbing.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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
    CHECK_FALSE(e.is_engine_row(1));
    CHECK(e.is_engine_row(0));
    CHECK(e.is_engine_row(2)); // native placeholder, never resolved here
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

TEST_CASE("mixed_engine_from_config: an unusable row demotes only its own slot to native", "[MixedFilament][EngineConfig]")
{
    // Structural problems stay fail-closed (tests above); a bad single row must not switch the good rows off.
    const std::vector<std::pair<const char *, std::string>> bad = {
        { "malformed", "not a row" },
        { "truncated", "1,2,1" },
        { "component outside the physical range", "1,9,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u1" },
        { "component zero (deleted sentinel)", "0,2,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u1" },
        { "several rows in one marker", std::string(kRowA) + ";" + kRowB },
    };
    for (const auto &[name, text] : bad) {
        INFO(name);
        MixedEngineSlotsReport rep;
        const MixedFilamentEngine e = mixed_engine_from_config(config_with(4, { {"1,2", kRowA}, {"3,4", text.c_str()} }), &rep);
        CHECK(rep.valid);
        REQUIRE(e.mixed_filaments().size() == 2);
        CHECK(e.is_engine_row(0));
        CHECK_FALSE(e.is_engine_row(1));
        CHECK(e.mixed_filaments()[1].component_a == 3); // placeholder takes the native components
        CHECK(e.mixed_filaments()[1].component_b == 4);
        REQUIRE(rep.warnings.size() == 1);
        CHECK(rep.warnings[0].find("slot 6") != std::string::npos);
    }
}

TEST_CASE("mixed_engine_from_config: hostile strings", "[MixedFilament][EngineConfig]")
{
    auto build = [](const std::string &row) {
        MixedEngineSlotsReport rep;
        MixedFilamentEngine e = mixed_engine_from_config(config_with(4, { {"1,2", row.c_str()} }), &rep);
        return std::make_pair(rep.valid, e.mixed_filaments().size());
    };
    // An unusable row demotes its own slot to native (valid engine, one placeholder row), see the per-row test below.
    CHECK(build(std::string(100000, '1')) == std::make_pair(true, size_t(1)));
    CHECK(build(std::string(100000, ',')) == std::make_pair(true, size_t(1)));
    CHECK(build("1,2,1,1,99999999999999999999,0,g,w,m2,z0,xa0,xb0,d0,o0,u1") .second <= 1);
    CHECK(build("1,2,1,1,50,0,g,w,m2,z-5,xa1e999,xb-nan,d0,o0,u18446744073709551616").second <= 1);
    CHECK(build(std::string("1,2,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u1,") + std::string(5000, '[') ).second <= 1);
    CHECK(build(std::string("\0\0\0,1,2", 7)).second <= 1);
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

    SECTION("a pair row naming the deleted filament is dropped by Snapmaker; here its slot goes native and the others keep working")
    {
        std::vector<std::string> comps = {"", "", "", "", "1,2", "3,4", "2,4"};
        std::vector<std::string> warn;
        remap_mixed_engine_rows_on_delete(is_mixed, rows, 1, &comps, &warn); // delete filament 1
        CHECK(rows[4].empty());      // kRowA used filament 1: marker cleared
        CHECK(comps[4] == "1,0");    // only old filament 2 (now 1) survives; 0 sentinel
        CHECK(warn.size() == 1);
        CHECK(comps[5] == "3,4");    // native slot untouched
    }
    SECTION("references above the deleted id shift down, gradient ids and pattern included")
    {
        remap_mixed_engine_rows_on_delete(is_mixed, rows, 1);
        const std::vector<MixedFilament> d = parse_mixed_filaments(rows[6], 12);
        REQUIRE(d.size() == 1);
        // Deliberate departure from Snapmaker (which leaves a pattern row's pair unshifted, pinning 2,4 here): the pair is
        // shifted unless it is exactly (1,2), because it is still the row's display/ratio colours (ticket #113, decision B).
        CHECK(d[0].component_a == 1);
        CHECK(d[0].component_b == 3);
        CHECK(d[0].gradient_component_ids == "123");
        CHECK(d[0].gradient_component_weights == "40/30/30");
        CHECK(d[0].manual_pattern == "2,1,93"); // [10] shifts to 9, which is a single-digit token
    }
    SECTION("a pattern row with pair 1,2 keeps the symbolic pair; any other pair shifts; weights follow an erased gradient id")
    {
        const std::string rowE = "3,4,1,1,50,0,g125,w50/30/20,m2,z0,xa0,xb0,d0,o0,u8,55";
        const std::string rowF = "1,2,1,1,50,0,g125,w50/30/20,m2,z0,xa0,xb0,d0,o0,u9,55";
        std::vector<std::string> r = {"", "", "", "", rowE, rowF, ""};
        std::vector<std::string> warn;
        remap_mixed_engine_rows_on_delete(is_mixed, r, 2, nullptr, &warn); // delete filament 2: not used by either pattern
        CHECK(warn.empty());
        const std::vector<MixedFilament> e = parse_mixed_filaments(r[4], 12);
        REQUIRE(e.size() == 1);
        CHECK(e[0].component_a == 2);
        CHECK(e[0].component_b == 3);
        CHECK(e[0].manual_pattern == "44");
        CHECK(e[0].gradient_component_ids == "14"); // id 2 erased, 5 -> 4
        CHECK(e[0].gradient_component_weights == "71/29"); // matching weight erased (50/20 renormalised), lengths agree
        const std::vector<MixedFilament> f = parse_mixed_filaments(r[5], 12);
        REQUIRE(f.size() == 1);
        CHECK(f[0].component_a == 1); // exactly 1,2: symbolic, left alone
        CHECK(f[0].component_b == 2);
        CHECK(f[0].manual_pattern == "44");
    }
    SECTION("a pattern row whose pair names the deleted filament but whose pattern does not need it keeps the slot, substitutes the pair member and warns")
    {
        const std::string rowG = "3,4,1,1,50,0,g34,w50/50,m2,z0,xa0,xb0,d0,o0,u8,55";
        std::vector<std::string> r = {"", "", "", "", rowG, "", ""};
        std::vector<std::string> warn;
        remap_mixed_engine_rows_on_delete(is_mixed, r, 3, nullptr, &warn);
        CHECK(warn.size() == 1);
        const std::vector<MixedFilament> g = parse_mixed_filaments(r[4], 12);
        REQUIRE(g.size() == 1);
        CHECK(g[0].component_a == 1); // 3 was deleted: replaced by the lowest id differing from the other member
        CHECK(g[0].component_b == 3);   // 4 shifted
        CHECK(g[0].manual_pattern == "44");
    }
    SECTION("a pattern that names the deleted id drops the row (slot goes native), it is not silently recoloured")
    {
        remap_mixed_engine_rows_on_delete(is_mixed, rows, 3);
        CHECK(rows[6].empty());
    }
    SECTION("a row naming the deleted filament no longer disables the whole engine")
    {
        DynamicPrintConfig c = config_with(4, { {"1,2", kRowA}, {"3,4", kRowB} });
        std::vector<std::string> r = c.option<ConfigOptionStrings>("filament_mixed_engine_row")->values;
        std::vector<std::string> comps = c.option<ConfigOptionStrings>("filament_mixed_components")->values;
        remap_mixed_engine_rows_on_delete(c.option<ConfigOptionBools>("filament_is_mixed")->values, r, 2, &comps);
        CHECK(r[4].empty());   // kRowA (1,2) used 2
        CHECK_FALSE(r[5].empty()); // kRowB (1,3) did not
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

TEST_CASE("clear_mixed_engine_marker clears one slot and reports whether it did", "[MixedFilament][EngineConfig]")
{
    DynamicPrintConfig c = config_with(4, { {"1,2", kRowA}, {"3,4", kRowB} });
    CHECK(clear_mixed_engine_marker(c, 4));
    const auto &rows = c.option<ConfigOptionStrings>("filament_mixed_engine_row")->values;
    CHECK(rows[4].empty());
    CHECK(rows[5] == kRowB);
    CHECK_FALSE(clear_mixed_engine_marker(c, 4)); // already empty
    CHECK_FALSE(clear_mixed_engine_marker(c, 99));
    CHECK_FALSE(clear_mixed_engine_marker(c, 0));
}

TEST_CASE("mixed_engine_marker_problem_slots flags a disabled engine, not a native or healthy one", "[MixedFilament][EngineConfig]")
{
    std::string why;
    CHECK(mixed_engine_marker_problem_slots(config_with(4, { {"1,2", ""} }), &why).empty());
    CHECK(mixed_engine_marker_problem_slots(config_with(4, { {"1,2", kRowA} }), &why).empty());
    // A single garbage row only demotes its own slot; the engine stays on, so nothing is flagged.
    CHECK(mixed_engine_marker_problem_slots(config_with(4, { {"1,2", kRowA}, {"3,4", "garbage"} }), &why).empty());
    // A structural problem (marker on a physical slot) switches the engine off: every marker slot is flagged.
    DynamicPrintConfig bad = config_with(4, { {"1,2", kRowA}, {"3,4", kRowB} });
    bad.option<ConfigOptionStrings>("filament_mixed_engine_row")->values[0] = kRowC;
    const std::vector<size_t> slots = mixed_engine_marker_problem_slots(bad, &why);
    CHECK(slots == std::vector<size_t>{ 0, 4, 5 });
    CHECK_FALSE(why.empty());
}

TEST_CASE("rotate_new_filament_before_mixed keeps the engine marker on its mixed slot", "[MixedFilament][EngineConfig]")
{
    // 4 physical + 2 mixed (marker on the first only), then a new physical slot appended at index 6.
    DynamicPrintConfig c = config_with(4, { {"1,2", kRowA}, {"3,4", ""} });
    for (const char *k : { "filament_colour", "filament_mixed_components", "filament_mixed_engine_row" })
        c.option<ConfigOptionStrings>(k)->values.push_back(std::string(k) == "filament_colour" ? "#FF0000" : "");
    c.option<ConfigOptionBools>("filament_is_mixed")->values.push_back(0);
    rotate_new_filament_before_mixed(c, 4, 6);

    const auto &rows  = c.option<ConfigOptionStrings>("filament_mixed_engine_row")->values;
    const auto &flags = c.option<ConfigOptionBools>("filament_is_mixed")->values;
    REQUIRE(rows.size() == 7);
    CHECK(rows[4].empty());      // the new physical slot
    CHECK(rows[5] == kRowA);     // marker moved with its slot
    CHECK(rows[6].empty());
    CHECK(flags[4] == 0);
    CHECK(flags[5] == 1);
    CHECK(c.option<ConfigOptionStrings>("filament_colour")->values[4] == "#FF0000");
    MixedEngineSlotsReport rep;
    CHECK(mixed_engine_from_config(c, &rep).mixed_filaments().size() == 2); // engine stays valid
    CHECK(rep.valid);

    // No mixed slots: no-op.
    DynamicPrintConfig n = config_with(4, {});
    n.option<ConfigOptionStrings>("filament_mixed_engine_row")->values.push_back("");
    rotate_new_filament_before_mixed(n, 4, 4);
    CHECK(n.option<ConfigOptionStrings>("filament_mixed_engine_row")->values.size() == 5);
}

TEST_CASE("Rows without a stable id keep distinct ids through a delete remap", "[MixedFilament][EngineConfig]")
{
    const std::string noid1 = "2,3,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0";
    const std::string noid2 = "3,4,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0";
    const std::vector<unsigned char> is_mixed = {0, 0, 0, 0, 1, 1, 1};
    std::vector<std::string> rows = {"", "", "", "", noid1, noid2, "2,3,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u7"};
    remap_mixed_engine_rows_on_delete(is_mixed, rows, 1);
    const auto a = parse_mixed_filaments(rows[4], 6);
    const auto b = parse_mixed_filaments(rows[5], 6);
    const auto d = parse_mixed_filaments(rows[6], 6);
    REQUIRE(a.size() == 1);
    REQUIRE(b.size() == 1);
    REQUIRE(d.size() == 1);
    CHECK(d[0].stable_id == 7); // a real id is preserved
    CHECK(a[0].stable_id != 0);
    CHECK(b[0].stable_id != 0);
    CHECK(a[0].stable_id != b[0].stable_id);
    CHECK(a[0].stable_id != d[0].stable_id);
    CHECK(b[0].stable_id != d[0].stable_id);
}

TEST_CASE("from_slots does not reject duplicate stable ids (so the remap must not create them)", "[MixedFilament][EngineConfig]")
{
    MixedFilament a, b;
    a.component_a = b.component_a = 1;
    a.component_b = b.component_b = 2;
    a.stable_id = b.stable_id = 5;
    MixedEngineSlotsReport rep;
    MixedFilamentEngine::from_slots({ a, b }, 4, &rep);
    CHECK(rep.valid); // documents current behaviour: no uniqueness check
}

TEST_CASE("apply_receiver_mix_relocations moves the engine row to the tail and clears the source", "[MixedFilament][EngineConfig][Bundle]")
{
    DynamicPrintConfig c = config_with(2, { {"1,2", kRowA} });
    // Make slot 1 a receiver-owned mix inside the physical region carrying a marker.
    c.option<ConfigOptionBools>("filament_is_mixed")->values[1]                    = 1;
    c.option<ConfigOptionStrings>("filament_mixed_engine_row")->values[1]          = kRowB;
    c.option<ConfigOptionStrings>("filament_mixed_components")->values[1]          = "3,4";
    std::vector<std::vector<std::string>> ams(3);
    // Grow by one tail slot for the move target.
    for (const char *k : { "filament_colour", "filament_mixed_components", "filament_mixed_engine_row" })
        c.option<ConfigOptionStrings>(k)->values.push_back("");
    c.option<ConfigOptionBools>("filament_is_mixed")->values.push_back(0);
    apply_receiver_mix_relocations(c, ams, { { 1, 3 } });
    const auto &rows = c.option<ConfigOptionStrings>("filament_mixed_engine_row")->values;
    CHECK(rows[3] == kRowB);
    CHECK(rows[1].empty());
    CHECK(rows[2] == kRowA);
    CHECK(c.option<ConfigOptionBools>("filament_is_mixed")->values[1] == 0);
}

TEST_CASE("sync_ams_list strips and re-appends mixed slots carrying their engine rows", "[MixedFilament][EngineConfig][Bundle]")
{
    PresetBundle bundle;
    bundle.set_num_filaments(3u, std::string("#808080"));
    bundle.project_config.option<ConfigOptionBools>("filament_is_mixed")->values            = {0, 0, 1};
    bundle.project_config.option<ConfigOptionStrings>("filament_mixed_components")->values  = {"", "", "1,2"};
    bundle.project_config.option<ConfigOptionStrings>("filament_mixed_engine_row")->values  = {"", "", kRowA};
    for (int slot = 0; slot < 3; ++slot) {
        DynamicPrintConfig config;
        config.set_key_value("filament_id", new ConfigOptionStrings{"CFS-PETG"});
        config.set_key_value("filament_colour", new ConfigOptionStrings{"#00FF0" + std::to_string(slot)});
        config.set_key_value("filament_colour_type", new ConfigOptionStrings{"1"});
        config.set_key_value("filament_multi_colour", new ConfigOptionStrings{});
        config.set_key_value("ams_id", new ConfigOptionStrings{"0"});
        config.set_key_value("slot_id", new ConfigOptionStrings{std::to_string(slot)});
        config.set_key_value("filament_type", new ConfigOptionStrings{"PETG"});
        config.set_key_value("filament_slot_placeholder", new ConfigOptionBools{false});
        bundle.filament_ams_list.emplace(slot, std::move(config));
    }
    std::vector<std::pair<DynamicPrintConfig *, std::string>> unknowns;
    std::map<int, AMSMapInfo> maps;
    MergeFilamentInfo merge_info;
    bundle.sync_ams_list(unknowns, false, maps, true, merge_info, false);

    const auto &flags = bundle.project_config.option<ConfigOptionBools>("filament_is_mixed")->values;
    const auto &rows  = bundle.project_config.option<ConfigOptionStrings>("filament_mixed_engine_row")->values;
    REQUIRE(flags.size() == rows.size());
    size_t mixed = 0;
    for (size_t i = 0; i < flags.size(); ++i) {
        if (flags[i]) { ++mixed; CHECK(rows[i] == kRowA); }
        else CHECK(rows[i].empty());
    }
    CHECK(mixed == 1);
}

// ---- Deleting a physical filament: per-layer resolve sequences before and after -----------------------------
//
// The expectations come from Snapmaker's remove_physical_filament rules (check order and adjust rules ported
// verbatim), so these tests prove AGREEMENT with Snapmaker's rules, not that those rules are right: a rule that is
// wrong in Snapmaker (e.g. a pattern row whose pair is not 1,2 keeps its pair unshifted) is pinned here as it is.
// They also cannot see a slot the engine and the slicer number differently; that needs a slice.
namespace {

// Physical ids a mixed slot (0-based k within the mixed tail) resolves to on layers 0..n-1, as the slicer would ask.
std::vector<unsigned int> resolve_sequence(const ConfigBase &config, size_t k, int n = 24)
{
    MixedEngineSlotsReport rep;
    const MixedFilamentEngine e = mixed_engine_from_config(config, &rep);
    std::vector<unsigned int> out;
    if (!rep.valid)
        return out;
    const MixedEngineGlobals g = mixed_engine_globals_from_config(config);
    const size_t np = rep.num_physical;
    for (int layer = 0; layer < n; ++layer)
        out.push_back(mixed_engine_resolve_layer(e, g, unsigned(np + 1 + k), np, layer, 0.2f * float(layer + 1), 0.2f, 0.2f));
    return out;
}

struct Deleted
{
    std::vector<std::vector<unsigned int>> before, after;
    std::vector<std::string>               rows_after, comps_after;
    std::vector<std::string>               warnings;
    bool                                   after_valid = false;
};

// 5 physical + the given rows (ids 6..); deletes physical `del` (1-based) through PresetBundle::update_num_filaments.
Deleted delete_physical(const std::vector<SlotSpec> &mixed, unsigned del)
{
    Deleted d;
    const DynamicPrintConfig before_cfg = config_with(5, mixed);
    for (size_t k = 0; k < mixed.size(); ++k)
        d.before.push_back(resolve_sequence(before_cfg, k));

    PresetBundle bundle;
    bundle.set_num_filaments(unsigned(5 + mixed.size()), std::string("#FF0000"));
    for (const char *key : { "filament_is_mixed", "filament_mixed_components", "filament_mixed_engine_row" })
        bundle.project_config.set_key_value(key, before_cfg.option(key)->clone());
    bundle.update_num_filaments(del - 1);
    d.rows_after  = bundle.project_config.option<ConfigOptionStrings>("filament_mixed_engine_row")->values;
    d.comps_after = bundle.project_config.option<ConfigOptionStrings>("filament_mixed_components")->values;
    d.warnings    = bundle.last_delete_mixed_warnings;
    MixedEngineSlotsReport rep;
    mixed_engine_from_config(bundle.project_config, &rep);
    d.after_valid = rep.valid;
    for (size_t k = 0; k < mixed.size(); ++k)
        d.after.push_back(resolve_sequence(bundle.project_config, k));
    return d;
}

std::vector<unsigned int> shifted_down(std::vector<unsigned int> v, unsigned del)
{
    for (unsigned int &x : v)
        if (x > del)
            --x;
    return v;
}

// pair a,b / enabled / custom / mix% / pattern / gradient ids+weights
std::string row_text(unsigned a, unsigned b, const std::string &pattern = "0", const std::string &g = "g", const std::string &w = "w", const char *mode = "m2")
{
    return std::to_string(a) + "," + std::to_string(b) + ",1,1,50," + pattern + "," + g + "," + w + "," + mode + ",z0,xa0,xb0,d0,o0,u";
}

} // namespace

TEST_CASE("Deleting a physical filament: resolve sequences of unaffected rows are unchanged (shifted ids only)", "[MixedFilament][EngineConfig][Bundle]")
{
    const std::string pair35  = row_text(3, 5) + "1";
    const std::string pat     = row_text(1, 2, "1244") + "2"; // symbolic 1/2 plus literal 4 and 4
    const std::string grad    = row_text(1, 2, "0", "g134", "w50/25/25,r1/0.2/0.8", "m0") + "3";
    const Deleted e = delete_physical({ {"3,5", pair35.c_str()}, {"1,2", pat.c_str()}, {"1,3", grad.c_str()} }, 5);
    REQUIRE(e.after_valid);
    // the gradient row really resolves through its ids (3 appears), not just through its pair
    { std::string dbg; for (auto x : e.before[2]) dbg += std::to_string(x) + " "; INFO(dbg); CHECK(std::count(e.before[2].begin(), e.before[2].end(), 3u) > 0); }
    // 5 is used by pair35 -> that one is dropped; the other two do not use 5
    CHECK(e.rows_after[4].empty());
    for (size_t k : { size_t(1), size_t(2) }) {
        INFO("row " << k);
        REQUIRE_FALSE(e.before[k].empty());
        CHECK(e.after[k] == shifted_down(e.before[k], 5));
    }
}

TEST_CASE("Deleting a physical filament: a row using the deleted id is dropped to native, the rest of the engine survives", "[MixedFilament][EngineConfig][Bundle]")
{
    const std::string keep = row_text(3, 5) + "1";
    const std::string keep45 = row_text(4, 5) + "1";
    SECTION("(a) pair names the deleted id")
    {
        const std::string hit = row_text(2, 4) + "2";
        const Deleted d = delete_physical({ {"3,5", keep.c_str()}, {"2,4", hit.c_str()} }, 2);
        CHECK(d.after_valid); // used to switch the whole engine off
        CHECK(d.after[0] == shifted_down(d.before[0], 2));
        CHECK(d.rows_after[5].empty()); // the slot is kept (6 -> index 4 after the delete) ...
        CHECK_FALSE(d.rows_after[4].empty());
        CHECK(d.comps_after[5] == "3,0"); // ... as a native mix: old 4 -> 3, old 2 deleted
        REQUIRE(d.warnings.size() == 1);
        CHECK(d.warnings[0].find("Mixed slot 7") != std::string::npos);
    }
    SECTION("(c) pattern names the deleted id literally")
    {
        const std::string hit = row_text(1, 2, "1244") + "2";
        const Deleted d = delete_physical({ {"3,5", keep.c_str()}, {"1,2", hit.c_str()} }, 4);
        CHECK(d.after_valid);
        CHECK(d.rows_after[5].empty());
        CHECK(d.after[0] == shifted_down(d.before[0], 4));
        CHECK(d.comps_after[5] == "1,2"); // survivors of {1,2,4,4} minus 4
    }
    SECTION("(c) pattern names the deleted id as symbolic A/B, not as a literal 1/2")
    {
        // pair 3,4 with pattern 1122: the symbols mean filaments 3 and 4. Deleting filament 1 or 2 is irrelevant to it.
        const std::string row = row_text(3, 4, "1122") + "2";
        const Deleted irrelevant = delete_physical({ {"3,5", keep.c_str()}, {"3,4", row.c_str()} }, 1);
        CHECK_FALSE(irrelevant.rows_after[5].empty());
        CHECK(irrelevant.after_valid);
        const Deleted hit = delete_physical({ {"4,5", keep45.c_str()}, {"3,4", row.c_str()} }, 3);
        CHECK(hit.rows_after[5].empty()); // symbolic 1 is filament 3
        CHECK(hit.after_valid);
    }
    SECTION("(d) gradient ids name the deleted id and there is no pattern")
    {
        const std::string hit = row_text(1, 2, "0", "g134", "w50/25/25,r1/0.2/0.8", "m0") + "2";
        const Deleted d = delete_physical({ {"4,5", keep45.c_str()}, {"1,2", hit.c_str()} }, 3);
        CHECK(d.after_valid);
        CHECK(d.rows_after[5].empty());
        CHECK(d.comps_after[5] == "1,3"); // 1 and 4 survive, 4 shifts down to 3
        CHECK(d.after[0] == shifted_down(d.before[0], 3));
    }
}

TEST_CASE("Deleting a physical filament: pattern rows lose the deleted gradient id and weight, and shift a pair other than 1,2", "[MixedFilament][EngineConfig][Bundle]")
{
    // (b) pattern row: Snapmaker leaves component_a/b alone, whatever their value
    const std::string row = row_text(1, 2, "1233", "g134", "w50/25/25,r1/0.2/0.8", "m0") + "2";
    const Deleted d = delete_physical({ {"1,2", row.c_str()} }, 4);
    REQUIRE(d.after_valid);
    CHECK(d.after[0] == shifted_down(d.before[0], 4)); // resolve is by the pattern: 1,2,3,3 unchanged
    const auto r = parse_mixed_filaments(d.rows_after[4], 5);
    REQUIRE(r.size() == 1);
    CHECK(r[0].component_a == 1);
    CHECK(r[0].component_b == 2);
    CHECK(r[0].manual_pattern == "1233");
    CHECK(r[0].gradient_component_ids == "13"); // 4 erased
    CHECK(r[0].gradient_component_weights == "67/33"); // the matching weight (25) is erased with it, 50/25 renormalised

    // Deliberate departure from Snapmaker, which pinned a pattern row whose pair is not 1,2 as left unshifted: the
    // pair is shifted (ticket #113, decision B) because it is still the row's display/ratio colours.
    const std::string odd = row_text(4, 5, "1122") + "3";
    const Deleted q = delete_physical({ {"4,5", odd.c_str()} }, 1);
    const auto qr = parse_mixed_filaments(q.rows_after[4], 5);
    REQUIRE(qr.size() == 1);
    CHECK(qr[0].component_a == 3);
    CHECK(qr[0].component_b == 4);
}
