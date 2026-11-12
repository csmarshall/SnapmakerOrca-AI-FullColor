// filament_spoolman_link must never change G-code and must not invalidate any slicing step. VERIFIED by these tests, including the
// mutation check (remove the GCode.cpp banned_keys entry or the Print.cpp steps_ignore entry: the matching test fails). The tests cannot
// show anything about other keys or about G-code produced by a different code path (e.g. the G-code viewer).
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include <algorithm>
#include <vector>
#include "libslic3r/Spoolman/SpoolmanLink.hpp"
#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

DynamicPrintConfig config_with_link(const std::string &link)
{
    DynamicPrintConfig c = DynamicPrintConfig::full_print_config();
    c.set_key_value("filament_spoolman_link", new ConfigOptionStrings{link});
    return c;
}

std::string some_link()
{
    Spoolman::Link l;
    l.spoolman_id = 42;
    l.external_id = "polymaker_pla_red";
    l.sources     = {{"filament_density", "spoolman"}};
    return Spoolman::serialize(l).value();
}

} // namespace

TEST_CASE("filament_spoolman_link is kept out of the G-code and does not change it", "[Spoolman][Regression]")
{
    const std::string link = some_link();
    const std::string with    = slice({make_cube(10., 10., 1.)}, config_with_link(link));
    const std::string without = slice({make_cube(10., 10., 1.)}, config_with_link(""));
    REQUIRE_FALSE(with.empty());
    CHECK(with.find("filament_spoolman_link") == std::string::npos);
    CHECK(with.find("polymaker_pla_red") == std::string::npos);
    CHECK(with == without);
}

TEST_CASE("Changing only filament_spoolman_link invalidates no print or object step", "[Spoolman][Regression]")
{
    Print  print;
    Model  model;
    DynamicPrintConfig config = config_with_link("");
    init_print({make_cube(10., 10., 1.)}, print, model, config);
    gcode(print);
    REQUIRE(print.is_step_done(psGCodeExport));
    const PrintObject &object = *print.objects().front();
    for (PrintObjectStep s : {posSlice, posPerimeters, posInfill, posSupportMaterial})
        REQUIRE(object.is_step_done(s));
    REQUIRE(print.is_step_done(psWipeTower));
    REQUIRE(print.is_step_done(psSkirtBrim));

    DynamicPrintConfig changed = config_with_link(some_link());
    const auto status = print.apply(model, changed);
    // Print::apply() invalidates psGCodeExport for ANY change of the full config (the placeholder parser sees every key, so custom G-code
    // could use it), exactly as for filament_notes. That is the only step touched; the regenerated G-code is byte-identical (test above).
    CHECK(status == PrintBase::APPLY_STATUS_INVALIDATED);
    CHECK_FALSE(print.is_step_done(psGCodeExport));
    // Everything that costs time stays done. Without the Print.cpp steps_ignore entry the key falls to the invalidate-everything fallback.
    CHECK(print.is_step_done(psWipeTower));
    CHECK(print.is_step_done(psSkirtBrim));
    const PrintObject &after = *print.objects().front();
    for (PrintObjectStep s : {posSlice, posPerimeters, posInfill, posSupportMaterial})
        CHECK(after.is_step_done(s));

    // Control: the same re-slice WITHOUT a link change keeps psGCodeExport.
    gcode(print);
    REQUIRE(print.is_step_done(psGCodeExport));
    CHECK(print.apply(model, changed) == PrintBase::APPLY_STATUS_UNCHANGED);
    CHECK(print.is_step_done(psGCodeExport));
}

// ---- real inheriting presets through PresetBundle::full_config ----------------------------------------------------------------------
namespace {

// A bundle with a system filament preset "SysParent" and one user preset per entry of `links`, each inheriting it and differing from it by
// the link ONLY (an empty link string means an identical copy). filament_presets selects them in order.
PresetBundle make_bundle(const std::vector<std::string> &links)
{
    PresetBundle bundle;
    DynamicPrintConfig parent_cfg = bundle.filaments.default_preset().config;
    parent_cfg.option<ConfigOptionString>("inherits", true)->value = "";
    Preset &parent = bundle.filaments.load_preset(std::string(), "SysParent", parent_cfg, false);
    parent.is_system  = true;
    parent.is_visible = true;

    bundle.filament_presets.clear();
    for (size_t i = 0; i < links.size(); ++i) {
        DynamicPrintConfig c = parent_cfg;
        c.option<ConfigOptionString>("inherits", true)->value = "SysParent";
        c.set_key_value("filament_spoolman_link", new ConfigOptionStrings{links[i]});
        const std::string name = "UserChild" + std::to_string(i);
        bundle.filaments.load_preset(std::string(), name, c, false).is_visible = true;
        bundle.filament_presets.push_back(name);
    }
    bundle.filaments.select_preset_by_name(bundle.filament_presets.front(), true);
    return bundle;
}

std::vector<std::string> different_settings_of(const DynamicPrintConfig &full)
{
    const auto *o = full.option<ConfigOptionStrings>("different_settings_to_system");
    return o ? o->values : std::vector<std::string>{};
}

bool mentions_link(const std::vector<std::string> &v)
{
    return std::any_of(v.begin(), v.end(), [](const std::string &e) { return e.find("filament_spoolman_link") != std::string::npos; });
}

} // namespace

TEST_CASE("An inheriting preset with a link does not list the key in different_settings_to_system", "[Spoolman][Regression]")
{
    const std::string link = some_link();
    for (size_t n : {size_t(1), size_t(2)}) {
        INFO("filaments: " << n);
        std::vector<std::string> links(n, "");
        links[0] = link;
        PresetBundle with = make_bundle(links);
        const DynamicPrintConfig full_with = with.full_config();

        // the link really is in the full config, one entry per filament, in order
        const auto *opt = full_with.option<ConfigOptionStrings>("filament_spoolman_link");
        REQUIRE(opt != nullptr);
        REQUIRE(opt->values.size() == n);
        CHECK(opt->values[0] == link);
        if (n == 2)
            CHECK(opt->values[1].empty());

        CHECK_FALSE(mentions_link(different_settings_of(full_with)));
        // and identical to the same bundle without any link
        PresetBundle without = make_bundle(std::vector<std::string>(n, ""));
        CHECK(different_settings_of(full_with) == different_settings_of(without.full_config()));
    }
}

TEST_CASE("G-code from inheriting filament presets is identical with and without a link", "[Spoolman][Regression]")
{
    const std::string link = some_link();
    for (size_t n : {size_t(1), size_t(2)}) {
        INFO("filaments: " << n);
        std::vector<std::string> links(n, "");
        links[0] = link;
        PresetBundle with    = make_bundle(links);
        PresetBundle without = make_bundle(std::vector<std::string>(n, ""));
        const std::string g_with    = slice({make_cube(10., 10., 1.)}, with.full_config());
        const std::string g_without = slice({make_cube(10., 10., 1.)}, without.full_config());
        REQUIRE_FALSE(g_with.empty());
        CHECK(g_with.find("filament_spoolman_link") == std::string::npos);
        CHECK(g_with.find("polymaker_pla_red") == std::string::npos);
        CHECK(g_with == g_without);
    }
}

TEST_CASE("Two presets may carry the same link (Save as copies it; nothing deduplicates)", "[Spoolman]")
{
    const std::string link = some_link();
    PresetBundle b = make_bundle({link, link});
    const DynamicPrintConfig full = b.full_config();
    const auto *opt = full.option<ConfigOptionStrings>("filament_spoolman_link");
    REQUIRE(opt != nullptr);
    REQUIRE(opt->values.size() == 2);
    CHECK(opt->values[0] == link);
    CHECK(opt->values[1] == link);
    // Follow-up for S5/S6: matching a Spoolman filament to presets must handle several presets with one link.
}
