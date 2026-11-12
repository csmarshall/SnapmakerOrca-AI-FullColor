// Tests for the Spoolman matching tier inside PresetBundle::sync_ams_list (S6). In-memory PresetBundle with hand-made filament presets
// (technique of test_spoolman_presets.cpp and the sync_ams_list tests in test_preset_bundle_loading.cpp); trays are built by hand with the
// plain keys spoolman_filament_id / spoolman_external_id / spoolman_setting_id.
// VERIFIED here: tier order (id, external_id, setting_id), no-join trays unchanged, ambiguity reported and not guessed, unparsable links
// and incompatible presets ignored. NOT shown: the Plater.cpp build_tray_config change (GUI, not compiled here), a real printer/profile
// bundle, slot colour handling beyond upstream's, the U1 adapter that fills the keys (S9).
#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Spoolman/SpoolmanLink.hpp"

#include <map>
#include <string>
#include <vector>

using namespace Slic3r;

namespace {

Preset &add(PresetCollection &c, const std::string &name, const std::string &type, bool system, const std::string &filament_id,
            const std::string &link = "", const std::string &setting_id = "", bool compatible = true,
            const std::string &inherits = "")
{
    DynamicPrintConfig cfg = c.default_preset().config;
    cfg.option<ConfigOptionString>("inherits", true)->value = inherits;
    cfg.set_key_value("filament_type", new ConfigOptionStrings{type});
    if (!link.empty())
        cfg.set_key_value("filament_spoolman_link", new ConfigOptionStrings{link});
    Preset &p = c.load_preset(std::string(), name, cfg, false);
    p.is_system     = system;
    p.is_visible    = true;
    p.filament_id   = filament_id;
    p.setting_id    = setting_id;
    p.is_compatible = compatible;
    return p;
}

std::string link(int id, const std::string &ext = "") { return *Spoolman::serialize(Spoolman::Link{id, ext, {}}); }

struct Tray
{
    std::string filament_id = "GFG00";   // RFID says generic "PETG Basic"
    std::string type        = "PETG";
    std::string colour      = "#123456";
    int         sm_id       = -1;        // -1 = key absent
    std::string sm_ext;                  // empty = key absent
    std::string sm_set;
    bool        placeholder = false;
};

struct Opts
{
    std::vector<std::string> initial;     // the slots' current presets (empty = as many default slots as trays)
    bool                     use_map = false;
    bool                     enable_append = true;
    std::map<int, AMSMapInfo> maps;
};

struct Result
{
    std::vector<std::string> presets;
    std::vector<std::string> colours;
    std::vector<std::string> unknowns;
    std::vector<int>         filament_map, volume_map;
    std::vector<std::vector<std::string>> multi;
    SpoolmanTierReport       report;
    unsigned                 count = 0;
};

Result run(PresetBundle &bundle, const std::vector<Tray> &trays, Opts o = {})
{
    bundle.filament_ams_list.clear();
    bundle.set_num_filaments(unsigned(o.initial.empty() ? trays.size() : o.initial.size()), std::string("#111111"));
    if (!o.initial.empty())
        bundle.filament_presets = o.initial;
    int slot = 0;
    for (const Tray &t : trays) {
        DynamicPrintConfig c;
        c.set_key_value("filament_id", new ConfigOptionStrings{t.filament_id});
        c.set_key_value("filament_colour", new ConfigOptionStrings{t.colour});
        c.set_key_value("filament_colour_type", new ConfigOptionStrings{"1"});
        c.set_key_value("filament_multi_colour", new ConfigOptionStrings{t.colour});
        c.set_key_value("filament_type", new ConfigOptionStrings{t.type});
        c.set_key_value("ams_id", new ConfigOptionStrings{"0"});
        c.set_key_value("slot_id", new ConfigOptionStrings{std::to_string(slot)});
        c.set_key_value("filament_slot_placeholder", new ConfigOptionBools{t.placeholder});
        if (t.sm_id >= 0)
            c.set_key_value("spoolman_filament_id", new ConfigOptionInts{t.sm_id});
        if (!t.sm_ext.empty())
            c.set_key_value("spoolman_external_id", new ConfigOptionStrings{t.sm_ext});
        if (!t.sm_set.empty())
            c.set_key_value("spoolman_setting_id", new ConfigOptionStrings{t.sm_set});
        bundle.filament_ams_list.emplace(slot++, std::move(c));
    }
    Result r;
    std::vector<std::pair<DynamicPrintConfig *, std::string>> unknowns;
    MergeFilamentInfo merge;
    r.count   = bundle.sync_ams_list(unknowns, o.use_map, o.maps, o.enable_append, merge, false, &r.report);
    r.presets = bundle.filament_presets;
    r.colours = bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values;
    r.filament_map = bundle.project_config.option<ConfigOptionInts>("filament_map")->values;
    r.volume_map   = bundle.project_config.option<ConfigOptionInts>("filament_volume_map")->values;
    r.multi        = bundle.ams_multi_color_filment;
    for (auto &u : unknowns)
        r.unknowns.push_back(u.second);
    return r;
}

// "Generic PETG Basic" (system, the RFID's preset), "Snapmaker PETG HF" (system, id GFG01), and the Spoolman-backed user preset that
// inherits the HF one (a user preset carries its parent's filament_id) linked to Spoolman filament 16.
struct Fixture
{
    PresetBundle bundle;
    Fixture()
    {
        add(bundle.printers, "PrinterA", "", false, "");
        add(bundle.filaments, "PETG Basic", "PETG", true, "GFG00");
        add(bundle.filaments, "Snapmaker PETG HF", "PETG", true, "GFG01");
        add(bundle.filaments, "Snapmaker PETG HF @T (Spoolman)", "PETG", false, "GFG01", link(16, "ofd_16"), "", true, "Snapmaker PETG HF");
        add(bundle.filaments, "Other (Spoolman)", "PETG", false, "GFG01", link(22, "ofd_22"), "", true, "Snapmaker PETG HF");
    }
};

} // namespace

TEST_CASE("A tray joined to a Spoolman filament selects the linked preset, not the RFID's generic one", "[Spoolman][SyncTier]")
{
    Fixture f;
    Result r = run(f.bundle, {Tray{"GFG00", "PETG", "#123456", 16}});
    REQUIRE(r.count == 1);
    CHECK(r.presets.at(0) == "Snapmaker PETG HF @T (Spoolman)"); // hand-written: not "PETG Basic"
    CHECK(r.colours.at(0) == "#123456");                          // the tray's own colour (S8/S9 handle Spoolman colour)
    REQUIRE(r.report.entries.size() == 1);
    CHECK(r.report.entries[0].rule == "spoolman_id");
    CHECK(r.report.entries[0].preset == "Snapmaker PETG HF @T (Spoolman)");
}

TEST_CASE("Trays without a join are unchanged whether or not linked presets exist", "[Spoolman][SyncTier]")
{
    const std::vector<Tray> trays{Tray{"GFG00", "PETG", "#111111"}, Tray{"GFG01", "PETG", "#222222", 0}, Tray{"NOPE", "PLA", "#333333"},
                                  Tray{"", "", "#444444"}};
    Fixture with_links;
    PresetBundle plain; // same filaments, no links at all
    add(plain.printers, "PrinterA", "", false, "");
    add(plain.filaments, "PETG Basic", "PETG", true, "GFG00");
    add(plain.filaments, "Snapmaker PETG HF", "PETG", true, "GFG01");
    add(plain.filaments, "Snapmaker PETG HF @T (Spoolman)", "PETG", false, "GFG01", "", "", true, "Snapmaker PETG HF");
    add(plain.filaments, "Other (Spoolman)", "PETG", false, "GFG01", "", "", true, "Snapmaker PETG HF");
    Result a = run(with_links.bundle, trays);
    Result b = run(plain, trays);
    CHECK(a.count == b.count);
    CHECK(a.presets == b.presets);
    CHECK(a.colours == b.colours);
    CHECK(a.unknowns == b.unknowns);
    CHECK(a.filament_map == b.filament_map);
    CHECK(a.volume_map == b.volume_map);
    CHECK(a.multi == b.multi);
    CHECK(a.report.entries.empty());
    // hand-written: tray 0 takes "PETG Basic" by filament_id as upstream does
    CHECK(a.presets.at(0) == "PETG Basic");
}

TEST_CASE("The external_id is used when the Spoolman id matches no preset", "[Spoolman][SyncTier]")
{
    Fixture f;
    Result r = run(f.bundle, {Tray{"GFG00", "PETG", "#123456", 999, "ofd_16"}});
    CHECK(r.presets.at(0) == "Snapmaker PETG HF @T (Spoolman)");
    REQUIRE(r.report.entries.size() == 1);
    CHECK(r.report.entries[0].rule == "external_id");
}

TEST_CASE("The setting_id is used when neither id nor external_id match", "[Spoolman][SyncTier]")
{
    Fixture f;
    add(f.bundle.filaments, "User by setting id", "PETG", false, "GFG01", "", "SET-42", true, "Snapmaker PETG HF");
    Result r = run(f.bundle, {Tray{"GFG00", "PETG", "#123456", 999, "nope", "SET-42"}});
    CHECK(r.presets.at(0) == "User by setting id");
    REQUIRE(r.report.entries.size() == 1);
    CHECK(r.report.entries[0].rule == "setting_id");
}

TEST_CASE("The id rule beats the external_id and setting_id rules", "[Spoolman][SyncTier]")
{
    Fixture f;
    add(f.bundle.filaments, "User by setting id", "PETG", false, "GFG01", "", "SET-42", true, "Snapmaker PETG HF");
    Result r = run(f.bundle, {Tray{"GFG00", "PETG", "#123456", 16, "ofd_22", "SET-42"}});
    CHECK(r.presets.at(0) == "Snapmaker PETG HF @T (Spoolman)");
}

TEST_CASE("Two presets with the same link id are reported ambiguous and upstream's choice stands", "[Spoolman][SyncTier]")
{
    Fixture f;
    add(f.bundle.filaments, "Snapmaker PETG HF copy", "PETG", false, "GFG01", link(16), "", true, "Snapmaker PETG HF");
    Result r = run(f.bundle, {Tray{"GFG00", "PETG", "#123456", 16}});
    CHECK(r.presets.at(0) == "PETG Basic"); // upstream: filament_id GFG00
    REQUIRE(r.report.entries.size() == 1);
    CHECK(r.report.entries[0].rule == "ambiguous");
    CHECK(r.report.entries[0].preset.empty());
    CHECK(r.report.entries[0].candidates.size() == 2);
}

TEST_CASE("An unparsable link and an incompatible preset are ignored", "[Spoolman][SyncTier]")
{
    PresetBundle b;
    add(b.printers, "PrinterA", "", false, "");
    add(b.filaments, "PETG Basic", "PETG", true, "GFG00");
    add(b.filaments, "Bad link", "PETG", false, "GFG01", "{\"id\":16"); // not JSON
    add(b.filaments, "No id link", "PETG", false, "GFG01", "{\"external_id\":\"x\"}");
    add(b.filaments, "Incompatible", "PETG", false, "GFG01", link(16), "", false);
    Result r = run(b, {Tray{"GFG00", "PETG", "#123456", 16}});
    CHECK(r.presets.at(0) == "PETG Basic");
    CHECK(r.report.entries.empty());
}

TEST_CASE("A tray joined to an id no preset carries falls through to upstream", "[Spoolman][SyncTier]")
{
    Fixture f;
    PresetBundle plain;
    add(plain.printers, "PrinterA", "", false, "");
    add(plain.filaments, "PETG Basic", "PETG", true, "GFG00");
    add(plain.filaments, "Snapmaker PETG HF", "PETG", true, "GFG01");
    Result a = run(f.bundle, {Tray{"GFG00", "PETG", "#123456", 77}});
    Result b = run(plain, {Tray{"GFG00", "PETG", "#123456"}});
    CHECK(a.presets.at(0) == "PETG Basic");
    CHECK(a.presets == b.presets);
    CHECK(a.unknowns == b.unknowns);
    CHECK(a.report.entries.empty());
}

// ---- Review round: the tier's pick must survive synced_preset_name, empty-id trays, wrong-typed keys, rule limits ----

namespace {
// A user preset that inherits the same parent (GFG01) as the Spoolman-linked presets, linked to Spoolman filament 22.
const std::vector<std::string> kSlot22{"Other (Spoolman)"};
const char *kPick16 = "Snapmaker PETG HF @T (Spoolman)";
} // namespace

TEST_CASE("A slot holding another preset with the same parent filament_id does not override the tier's pick", "[Spoolman][SyncTier]")
{
    Fixture f;
    SECTION("plain overwrite (wholesale) path") {
        Result r = run(f.bundle, {Tray{"GFG01", "PETG", "#123456", 16}}, Opts{kSlot22});
        CHECK(r.presets.at(0) == kPick16);
    }
    SECTION("mapped path (use_map)") {
        Opts o{kSlot22};
        o.use_map = true;
        o.maps[0] = AMSMapInfo{"0", "0"};
        Result r = run(f.bundle, {Tray{"GFG01", "PETG", "#123456", 16}}, o);
        CHECK(r.presets.at(0) == kPick16);
    }
    SECTION("placeholder merge path") {
        Tray empty;
        empty.filament_id = ""; empty.type = ""; empty.placeholder = true;
        Result r = run(f.bundle, {Tray{"GFG01", "PETG", "#123456", 16}, empty}, Opts{{"Other (Spoolman)", "PETG Basic"}});
        CHECK(r.presets.at(0) == kPick16);
        CHECK(r.presets.at(1) == "PETG Basic"); // the empty slot keeps its filament
    }
    SECTION("a slot already holding the pick keeps it") {
        Result r = run(f.bundle, {Tray{"GFG01", "PETG", "#123456", 16}}, Opts{{kPick16}});
        CHECK(r.presets.at(0) == kPick16);
    }
    SECTION("a tray without a join keeps upstream's rule: the slot's user preset for the same filament stays") {
        Result r = run(f.bundle, {Tray{"GFG01", "PETG", "#123456"}}, Opts{kSlot22});
        CHECK(r.presets.at(0) == "Other (Spoolman)"); // upstream: user preset with the tray's filament_id is kept
    }
}

TEST_CASE("Two trays joined to different Spoolman filaments with the same parent and colour both get their preset", "[Spoolman][SyncTier]")
{
    Fixture f;
    const std::vector<Tray> trays{Tray{"GFG01", "PETG", "#123456", 16}, Tray{"GFG01", "PETG", "#123456", 22}};
    SECTION("mapped append: the slot holds the first pick, the second must still be appended") {
        Opts o{{kPick16}};
        o.use_map = true;
        o.maps[0] = AMSMapInfo{"0", "0"};
        Result r = run(f.bundle, trays, o);
        REQUIRE(r.presets.size() == 2);
        CHECK(r.presets.at(0) == kPick16);
        CHECK(r.presets.at(1) == "Other (Spoolman)");
    }
    SECTION("wholesale") {
        Result r = run(f.bundle, trays);
        REQUIRE(r.presets.size() == 2);
        CHECK(r.presets.at(0) == kPick16);
        CHECK(r.presets.at(1) == "Other (Spoolman)");
    }
}

TEST_CASE("A tray with no RFID filament_id but a Spoolman join gets the linked preset", "[Spoolman][SyncTier]")
{
    Fixture f;
    Tray joined;
    joined.filament_id = ""; joined.type = ""; joined.sm_id = 16;
    SECTION("no mapping") {
        Result r = run(f.bundle, {joined});
        CHECK(r.presets.at(0) == kPick16);
    }
    SECTION("use_map") {
        Opts o; o.use_map = true;
        o.maps[0] = AMSMapInfo{"0", "0"};
        Result r = run(f.bundle, {joined}, o);
        REQUIRE(r.presets.size() == 1);
        CHECK(r.presets.at(0) == kPick16);
    }
    SECTION("empty id and no join is identical to a bundle without links") {
        Tray none; none.filament_id = ""; none.type = "";
        for (bool use_map : {false, true}) {
            PresetBundle plain;
            add(plain.printers, "PrinterA", "", false, "");
            add(plain.filaments, "PETG Basic", "PETG", true, "GFG00");
            Opts o; o.use_map = use_map;
            Result a = run(f.bundle, {none}, o);
            Result b = run(plain, {none}, o);
            CHECK(a.presets == b.presets);
            CHECK(a.colours == b.colours);
            CHECK(a.multi == b.multi);
            CHECK(a.count == b.count);
            CHECK(a.report.entries.empty());
        }
    }
}

TEST_CASE("A tray whose slot is unchanged stays unchanged even when joined", "[Spoolman][SyncTier]")
{
    Fixture f;
    f.bundle.filament_ams_list.clear();
    Result before = run(f.bundle, {Tray{"GFG00", "PETG", "#123456"}}); // establish a slot
    // Mark the tray unchanged: upstream keeps the slot's current preset.
    f.bundle.filament_ams_list.begin()->second.set_key_value("filament_changed", new ConfigOptionBools{false});
    f.bundle.filament_ams_list.begin()->second.set_key_value("spoolman_filament_id", new ConfigOptionInts{16});
    std::vector<std::pair<DynamicPrintConfig *, std::string>> unknowns;
    std::map<int, AMSMapInfo> maps;
    MergeFilamentInfo merge;
    f.bundle.sync_ams_list(unknowns, false, maps, true, merge);
    CHECK(f.bundle.filament_presets.at(0) == before.presets.at(0));
}

TEST_CASE("Wrong-typed join keys are ignored, not fatal", "[Spoolman][SyncTier]")
{
    Fixture f;
    auto with = [&](auto set) {
        f.bundle.filament_ams_list.clear();
        f.bundle.set_num_filaments(1u, std::string("#111111"));
        DynamicPrintConfig c;
        c.set_key_value("filament_id", new ConfigOptionStrings{"GFG00"});
        c.set_key_value("filament_colour", new ConfigOptionStrings{"#123456"});
        c.set_key_value("filament_colour_type", new ConfigOptionStrings{"1"});
        c.set_key_value("filament_multi_colour", new ConfigOptionStrings{"#123456"});
        c.set_key_value("filament_type", new ConfigOptionStrings{"PETG"});
        c.set_key_value("ams_id", new ConfigOptionStrings{"0"});
        c.set_key_value("slot_id", new ConfigOptionStrings{"0"});
        set(c);
        f.bundle.filament_ams_list.emplace(0, std::move(c));
        std::vector<std::pair<DynamicPrintConfig *, std::string>> unknowns;
        std::map<int, AMSMapInfo> maps;
        MergeFilamentInfo merge;
        f.bundle.sync_ams_list(unknowns, false, maps, true, merge);
        return f.bundle.filament_presets.at(0);
    };
    CHECK(with([](DynamicPrintConfig &c) { c.set_key_value("spoolman_external_id", new ConfigOptionString("ofd_16")); }) == "PETG Basic");
    CHECK(with([](DynamicPrintConfig &c) { c.set_key_value("spoolman_external_id", new ConfigOptionInts{16}); }) == "PETG Basic");
    CHECK(with([](DynamicPrintConfig &c) { c.set_key_value("spoolman_setting_id", new ConfigOptionBool(true)); }) == "PETG Basic");
    CHECK(with([](DynamicPrintConfig &c) { c.set_key_value("spoolman_filament_id", new ConfigOptionStrings{"16"}); }) == "PETG Basic");
    CHECK(with([](DynamicPrintConfig &c) { c.set_key_value("spoolman_filament_id", new ConfigOptionInt(16)); }) == "PETG Basic");
    CHECK(with([](DynamicPrintConfig &c) { c.set_key_value("spoolman_external_id", new ConfigOptionStrings{}); }) == "PETG Basic");
}

TEST_CASE("Rule limits: system presets only by setting_id, never the default preset, id 0 inactive, external_id exact", "[Spoolman][SyncTier]")
{
    Fixture f;
    // system preset with a setting_id: selected by the setting_id rule only
    add(f.bundle.filaments, "System by setting id", "PETG", true, "GFSYS", "", "SYS-1");
    Result a = run(f.bundle, {Tray{"GFG00", "PETG", "#123456", -1, "", "SYS-1"}});
    CHECK(a.presets.at(0) == "System by setting id");
    REQUIRE(a.report.entries.size() == 1);
    CHECK(a.report.entries[0].rule == "setting_id");
    // a system preset carrying a link is never chosen by id / external_id
    add(f.bundle.filaments, "System with link", "PETG", true, "GFSYS2", link(33, "ofd_33"));
    Result b = run(f.bundle, {Tray{"GFG00", "PETG", "#123456", 33}});
    CHECK(b.presets.at(0) == "PETG Basic");
    Result c = run(f.bundle, {Tray{"GFG00", "PETG", "#123456", -1, "ofd_33"}});
    CHECK(c.presets.at(0) == "PETG Basic");
    // the default preset is never a setting_id candidate even with an empty-matching id
    f.bundle.filaments.default_preset().setting_id = "DEF-1";
    f.bundle.filaments.default_preset().is_compatible = true;
    Result d = run(f.bundle, {Tray{"GFG00", "PETG", "#123456", -1, "", "DEF-1"}});
    CHECK(d.presets.at(0) == "PETG Basic");
    CHECK(d.report.entries.empty());
    // id key present but 0: inactive, nothing joins
    Result e = run(f.bundle, {Tray{"GFG00", "PETG", "#123456", 0}});
    CHECK(e.presets.at(0) == "PETG Basic");
    CHECK(e.report.entries.empty());
    // external_id is compared exactly (case-sensitive)
    Result g = run(f.bundle, {Tray{"GFG00", "PETG", "#123456", -1, "OFD_16"}});
    CHECK(g.presets.at(0) == "PETG Basic");
    CHECK(g.report.entries.empty());
}
