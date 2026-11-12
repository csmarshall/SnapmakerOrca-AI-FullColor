// G-code from a generated Spoolman preset: identical to G-code from a hand-made preset carrying the same overrides, different from the parent's
// (so the overrides are not a no-op), and free of the link. VERIFIED by these tests only; shows nothing about other overrides or printers.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Spoolman/SpoolmanPresets.hpp"
#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

PresetBundle make_bundle()
{
    PresetBundle bundle;
    DynamicPrintConfig cfg = bundle.filaments.default_preset().config;
    cfg.option<ConfigOptionString>("inherits", true)->value = "";
    cfg.set_key_value("filament_type", new ConfigOptionStrings{"PETG"});
    cfg.set_key_value("filament_vendor", new ConfigOptionStrings{"Generic"});
    Preset &parent = bundle.filaments.load_preset(std::string(), "Generic PETG @T", cfg, false);
    parent.is_system  = true;
    parent.is_visible = true;
    return bundle;
}

void select_only(PresetBundle &b, const std::string &name)
{
    b.filament_presets = {name};
    b.filaments.select_preset_by_name(name, true);
}

const std::string kName = "Acme PETG Red (Spoolman) @T";

} // namespace

TEST_CASE("G-code with a generated Spoolman preset equals the hand-overridden one and differs from the parent's", "[Spoolman]")
{
    Spoolman::Filament f;
    f.id = 5;
    f.name = "PETG Red";
    f.vendor.emplace();
    f.vendor->name = "Acme";
    f.material = "PETG";
    f.settings_extruder_temp = 250;
    f.density = 1.3;

    PresetBundle generated = make_bundle();
    auto report = Spoolman::upsert_catalog(generated.filaments, {f}, {});
    REQUIRE(report.created.size() == 1);
    REQUIRE(report.created[0].preset_name == kName);
    select_only(generated, kName);

    // by hand: a user preset inheriting the parent with the overrides written out literally (no link)
    PresetBundle manual = make_bundle();
    DynamicPrintConfig cfg = manual.filaments.find_preset("Generic PETG @T")->config;
    cfg.option<ConfigOptionString>("inherits", true)->value = "Generic PETG @T";
    cfg.set_key_value("nozzle_temperature", new ConfigOptionInts{250});
    cfg.set_key_value("nozzle_temperature_initial_layer", new ConfigOptionInts{250});
    cfg.set_key_value("filament_density", new ConfigOptionFloats{1.3});
    cfg.set_key_value("filament_vendor", new ConfigOptionStrings{"Acme"});
    cfg.set_key_value("default_filament_colour", new ConfigOptionStrings{""});
    manual.filaments.load_preset(std::string(), kName, cfg, false).is_visible = true;
    select_only(manual, kName);

    PresetBundle parent_only = make_bundle();
    select_only(parent_only, "Generic PETG @T");

    const std::string g_generated = slice({make_cube(10., 10., 1.)}, generated.full_config());
    const std::string g_manual    = slice({make_cube(10., 10., 1.)}, manual.full_config());
    const std::string g_parent    = slice({make_cube(10., 10., 1.)}, parent_only.full_config());
    REQUIRE_FALSE(g_generated.empty());
    CHECK(g_generated.find("filament_spoolman_link") == std::string::npos);
    CHECK(g_generated.find("native:settings_extruder_temp") == std::string::npos);
    CHECK(g_generated == g_manual);
    CHECK(g_generated != g_parent);
    CHECK(g_generated.find("M104 S250") != std::string::npos);
}
