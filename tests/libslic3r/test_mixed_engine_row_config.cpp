// Slice 4a of the mixed-filament engine port: the per-slot engine-row key and the Snapmaker global
// keys. These tests pin the config contract only (presence, defaults, resize, 3MF round trip, native
// projects unchanged); nothing here reads the keys for slicing yet.
//
// What they cannot catch: the defaults are compared with the values copied from Snapmaker's
// PrintConfig.cpp by the same author, so a transcription error in both places would agree.
#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>
#include <vector>

#include "libslic3r/Model.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "test_utils.hpp"

using namespace Slic3r;

namespace {

struct GlobalKey
{
    const char *key;
    ConfigOptionType type;
    double  default_value; // bools as 0/1
    double  min;           // -1e300 = unbounded
    double  max;           // 1e300 = unbounded
};

// Snapmaker Orca PrintConfig.cpp ~4349-4475: name, type, default, min, max.
const GlobalKey kGlobalKeys[] = {
    {"mixed_color_layer_height_a", coFloat, 0.0, 0., 1e300},
    {"mixed_color_layer_height_b", coFloat, 0.0, 0., 1e300},
    {"mixed_filament_gradient_mode", coBool, 0, -1e300, 1e300},
    {"mixed_filament_height_lower_bound", coFloat, 0.04, 0.01, 1e300},
    {"mixed_filament_height_upper_bound", coFloat, 0.16, 0.01, 1e300},
    {"mixed_filament_advanced_dithering", coBool, 0, -1e300, 1e300},
    {"mixed_filament_pointillism_pixel_size", coFloat, 0.0, 0., 1e300},
    {"mixed_filament_pointillism_line_gap", coFloat, 0.0, 0., 1e300},
    {"mixed_filament_component_bias_enabled", coBool, 0, -1e300, 1e300},
    {"mixed_filament_surface_indentation", coFloat, 0.0, -2.0, 2.0},
    {"mixed_filament_region_collapse", coBool, 1, -1e300, 1e300},
};

double option_as_double(const ConfigOption *opt)
{
    if (auto *f = dynamic_cast<const ConfigOptionFloat *>(opt))
        return f->value;
    return dynamic_cast<const ConfigOptionBool *>(opt)->value ? 1. : 0.;
}

// Store a native (no engine rows) 5-slot mixed project, reload it, return the reloaded config.
DynamicPrintConfig store_and_reload(DynamicPrintConfig config, const std::string &path)
{
    Model model;
    REQUIRE(load_stl((std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl").c_str(), &model));
    model.add_default_instances();
    ScopedTemporaryDir backup_dir("orca_engine_row_src");
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
    ScopedTemporaryDir dst_backup_dir("orca_engine_row_dst");
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

DynamicPrintConfig native_mixed_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("filament_colour", new ConfigOptionStrings({"#00AE42", "#FFFF00", "#FF0000", "#0000FF", "#FF6A26"}));
    config.set_key_value("filament_is_mixed", new ConfigOptionBools({false, false, false, false, true}));
    config.set_key_value("filament_mixed_components", new ConfigOptionStrings({"", "", "", "", "3,2"}));
    config.set_key_value("filament_mixed_sublayer_ratios", new ConfigOptionStrings({"", "", "", "", "0.4200,0.5800"}));
    return config;
}

} // namespace

TEST_CASE("Engine-row key and Snapmaker global keys exist with Snapmaker's defaults and ranges", "[MixedFilament][Config]")
{
    const DynamicPrintConfig defaults = DynamicPrintConfig::full_print_config();

    for (const GlobalKey &g : kGlobalKeys) {
        DYNAMIC_SECTION(g.key)
        {
            const ConfigOptionDef *def = print_config_def.get(g.key);
            REQUIRE(def != nullptr);
            CHECK(def->type == g.type);
            REQUIRE(defaults.option(g.key) != nullptr);
            CHECK(option_as_double(defaults.option(g.key)) == g.default_value);
            if (g.min > -1e299) {
                CHECK(def->min == float(g.min));
            }
            if (g.max < 1e299) {
                CHECK(def->max == float(g.max));
            }
        }
    }

    const ConfigOptionDef *row = print_config_def.get("filament_mixed_engine_row");
    REQUIRE(row != nullptr);
    CHECK(row->type == coStrings);
    auto *row_default = dynamic_cast<const ConfigOptionStrings *>(row->default_value.get());
    REQUIRE(row_default != nullptr);
    CHECK(row_default->values == std::vector<std::string>({""}));

    // Typed members exist on the print config (this fails to compile if the header lacks them).
    FullPrintConfig full;
    CHECK(full.mixed_filament_height_lower_bound.value == 0.04);
    CHECK(full.mixed_filament_region_collapse.value == true);
    CHECK(full.filament_mixed_engine_row.values == std::vector<std::string>({""}));

    // The deferred Local-Z and definitions keys must NOT be present yet (slice 5 / replaced by per-slot storage).
    for (const char *absent : {"mixed_filament_definitions", "dithering_z_step_size", "dithering_local_z_mode"})
        CHECK(print_config_def.get(absent) == nullptr);
}

TEST_CASE("filament_mixed_engine_row follows the slot count like the other filament_mixed_* vectors", "[MixedFilament][Config]")
{
    DynamicPrintConfig config;
    config.option<ConfigOptionBools>("filament_is_mixed", true)->values = {false, false, true};
    config.option<ConfigOptionStrings>("filament_mixed_engine_row", true)->values = {"", "", "m2;x"};

    SECTION("same size keeps values")
    {
        resize_mixed_filament_metadata(config, 3, 3);
        CHECK(config.option<ConfigOptionStrings>("filament_mixed_engine_row")->values == std::vector<std::string>({"", "", "m2;x"}));
    }
    SECTION("adding slots pads with empty (native) rows")
    {
        resize_mixed_filament_metadata(config, 3, 5);
        CHECK(config.option<ConfigOptionStrings>("filament_mixed_engine_row")->values == std::vector<std::string>({"", "", "m2;x", "", ""}));
    }
    SECTION("removing the last slot drops its row")
    {
        resize_mixed_filament_metadata(config, 3, 2);
        CHECK(config.option<ConfigOptionStrings>("filament_mixed_engine_row")->values == std::vector<std::string>({"", ""}));
    }
    SECTION("a missing key is created at the right size")
    {
        DynamicPrintConfig bare;
        resize_mixed_filament_metadata(bare, 0, 4);
        CHECK(bare.option<ConfigOptionStrings>("filament_mixed_engine_row")->values == std::vector<std::string>({"", "", "", ""}));
    }
}

TEST_CASE("PresetBundle add/delete slot keeps filament_mixed_engine_row in step", "[MixedFilament][Config][Bundle]")
{
    PresetBundle bundle;
    bundle.set_num_filaments(3u, std::string("#FF0000"));
    auto &rows = bundle.project_config.option<ConfigOptionStrings>("filament_mixed_engine_row")->values;
    REQUIRE(rows == std::vector<std::string>({"", "", ""}));
    rows = {"", "A", "B"};

    SECTION("growing pads native rows")
    {
        bundle.set_num_filaments(5u, std::string("#00FF00"));
        CHECK(bundle.project_config.option<ConfigOptionStrings>("filament_mixed_engine_row")->values ==
              std::vector<std::string>({"", "A", "B", "", ""}));
    }
    SECTION("shrinking truncates")
    {
        bundle.set_num_filaments(2u, std::string("#00FF00"));
        CHECK(bundle.project_config.option<ConfigOptionStrings>("filament_mixed_engine_row")->values == std::vector<std::string>({"", "A"}));
    }
    SECTION("deleting a middle slot erases that slot's row")
    {
        bundle.update_num_filaments(1);
        CHECK(bundle.project_config.option<ConfigOptionStrings>("filament_mixed_engine_row")->values == std::vector<std::string>({"", "B"}));
    }
}

TEST_CASE("Engine rows and the Snapmaker global keys round-trip through a project 3MF", "[MixedFilament][3mf][Config]")
{
    DynamicPrintConfig config = native_mixed_config();
    // Row text uses Snapmaker's token characters, including '|' and ';', to prove nothing splits it.
    const std::vector<std::string> rows = {"", "", "", "", "s17;e1;b0;a1;c2;m2;p[10]1[11]2,3|x"};
    config.set_key_value("filament_mixed_engine_row", new ConfigOptionStrings(rows));
    config.option<ConfigOptionFloat>("mixed_color_layer_height_a", true)->value = 0.12;
    config.option<ConfigOptionFloat>("mixed_color_layer_height_b", true)->value = 0.08;
    config.option<ConfigOptionBool>("mixed_filament_gradient_mode", true)->value = true;
    config.option<ConfigOptionFloat>("mixed_filament_height_lower_bound", true)->value = 0.05;
    config.option<ConfigOptionFloat>("mixed_filament_height_upper_bound", true)->value = 0.2;
    config.option<ConfigOptionBool>("mixed_filament_advanced_dithering", true)->value = true;
    config.option<ConfigOptionFloat>("mixed_filament_pointillism_pixel_size", true)->value = 0.7;
    config.option<ConfigOptionFloat>("mixed_filament_pointillism_line_gap", true)->value = 0.3;
    config.option<ConfigOptionBool>("mixed_filament_component_bias_enabled", true)->value = true;
    config.option<ConfigOptionFloat>("mixed_filament_surface_indentation", true)->value = -1.5;
    config.option<ConfigOptionBool>("mixed_filament_region_collapse", true)->value = false;

    ScopedTemporaryFile temp(".3mf");
    const DynamicPrintConfig back = store_and_reload(config, temp.string());

    auto *row_opt = back.option<ConfigOptionStrings>("filament_mixed_engine_row");
    REQUIRE(row_opt != nullptr);
    CHECK(row_opt->values == rows);
    CHECK(back.option<ConfigOptionFloat>("mixed_color_layer_height_a")->value == 0.12);
    CHECK(back.option<ConfigOptionFloat>("mixed_color_layer_height_b")->value == 0.08);
    CHECK(back.option<ConfigOptionBool>("mixed_filament_gradient_mode")->value == true);
    CHECK(back.option<ConfigOptionFloat>("mixed_filament_height_lower_bound")->value == 0.05);
    CHECK(back.option<ConfigOptionFloat>("mixed_filament_height_upper_bound")->value == 0.2);
    CHECK(back.option<ConfigOptionBool>("mixed_filament_advanced_dithering")->value == true);
    CHECK(back.option<ConfigOptionFloat>("mixed_filament_pointillism_pixel_size")->value == 0.7);
    CHECK(back.option<ConfigOptionFloat>("mixed_filament_pointillism_line_gap")->value == 0.3);
    CHECK(back.option<ConfigOptionBool>("mixed_filament_component_bias_enabled")->value == true);
    CHECK(back.option<ConfigOptionFloat>("mixed_filament_surface_indentation")->value == -1.5);
    CHECK(back.option<ConfigOptionBool>("mixed_filament_region_collapse")->value == false);
}

TEST_CASE("A native mixed project keeps its native keys and only gains default-valued engine keys", "[MixedFilament][3mf][Config]")
{
    ScopedTemporaryFile temp(".3mf");
    const DynamicPrintConfig back = store_and_reload(native_mixed_config(), temp.string());

    // Native mixed vectors are untouched.
    CHECK(back.option<ConfigOptionBools>("filament_is_mixed")->values == std::vector<unsigned char>({0, 0, 0, 0, 1}));
    CHECK(back.option<ConfigOptionStrings>("filament_mixed_components")->values == std::vector<std::string>({"", "", "", "", "3,2"}));
    CHECK(back.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios")->values ==
          std::vector<std::string>({"", "", "", "", "0.4200,0.5800"}));

    // Every engine slot is the empty (native) marker, so no slot is an engine row.
    for (const std::string &row : back.option<ConfigOptionStrings>("filament_mixed_engine_row")->values)
        CHECK(row.empty());

    // Every new global equals its default.
    const DynamicPrintConfig defaults = DynamicPrintConfig::full_print_config();
    for (const GlobalKey &g : kGlobalKeys) {
        DYNAMIC_SECTION(g.key)
        {
            REQUIRE(back.option(g.key) != nullptr);
            CHECK(option_as_double(back.option(g.key)) == option_as_double(defaults.option(g.key)));
        }
    }
}
