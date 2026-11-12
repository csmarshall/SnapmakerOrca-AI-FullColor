// Tests for the Spoolman catalog upsert (SpoolmanPresets). In-memory PresetBundle with hand-made SYSTEM filament presets (technique of
// tests/fff_print/test_spoolman_link_gcode.cpp); one test builds the system set from the real candidates TSV (names/types/vendors/ids of the
// Snapmaker + OrcaFilamentLibrary profiles) instead of loading the real profile bundle. Expected values are written by hand.
// VERIFIED here: the behaviours listed in SpoolmanPresets.hpp. NOT shown: real profile bundles, GUI/edited-preset refresh, disk persistence
// beyond one preset-JSON round trip, other Spoolman data.
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Spoolman/SpoolmanLink.hpp"
#include "libslic3r/Spoolman/SpoolmanPresets.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <tuple>

using namespace Slic3r;
using namespace Slic3r::Spoolman;

namespace {

const std::vector<std::string> kPlateKeys = {"cool_plate_temp", "eng_plate_temp", "hot_plate_temp", "textured_plate_temp", "textured_cool_plate_temp",
                                             "supertack_plate_temp", "cool_plate_temp_initial_layer", "eng_plate_temp_initial_layer",
                                             "hot_plate_temp_initial_layer", "textured_plate_temp_initial_layer",
                                             "textured_cool_plate_temp_initial_layer", "supertack_plate_temp_initial_layer"};

Preset &add_system(PresetCollection &c, const std::string &name, const std::string &type, const std::string &vendor,
                   std::vector<std::string> compatible = {}, const std::string &setting_id = "", const std::string &filament_id = "")
{
    DynamicPrintConfig cfg = c.default_preset().config;
    cfg.option<ConfigOptionString>("inherits", true)->value = "";
    cfg.set_key_value("filament_type", new ConfigOptionStrings{type});
    cfg.set_key_value("filament_vendor", new ConfigOptionStrings{vendor});
    cfg.set_key_value("nozzle_temperature", new ConfigOptionInts{200});
    cfg.set_key_value("nozzle_temperature_initial_layer", new ConfigOptionInts{205});
    for (const auto &k : kPlateKeys)
        cfg.set_key_value(k, new ConfigOptionInts{55});
    cfg.set_key_value("compatible_printers", new ConfigOptionStrings{compatible});
    Preset &p = c.load_preset(std::string(), name, cfg, false);
    p.is_system   = true;
    p.is_visible  = true;
    p.setting_id  = setting_id;
    p.filament_id = filament_id;
    return p;
}

Preset &add_printer(PresetBundle &b, const std::string &name)
{
    return b.printers.load_preset(std::string(), name, b.printers.default_preset().config, false);
}

// Small bundle: "Generic PETG @T" (all printers) and "Generic PLA @T" (printer "PrinterA" only), printer "PrinterA".
struct Fixture
{
    PresetBundle bundle;
    Fixture()
    {
        add_printer(bundle, "PrinterA");
        add_printer(bundle, "PrinterB");
        add_system(bundle.filaments, "Generic PETG @T", "PETG", "Generic");
        add_system(bundle.filaments, "Generic PLA @T", "PLA", "Generic", {"PrinterA"});
    }
    const Preset *printer(const char *name = "PrinterA") const { return bundle.printers.find_preset(name); }
    UpsertContext ctx(PrinterLimits lim = {}) const
    {
        UpsertContext c;
        c.printer         = printer();
        c.preferred_token = "@T";
        c.limits          = lim;
        return c;
    }
    UpsertReport run(const std::vector<Filament> &cat, PrinterLimits lim = {}) { return upsert_catalog(bundle.filaments, cat, ctx(lim)); }
    const Preset *get(const std::string &name) const { return bundle.filaments.find_preset(name, false); }
};

Filament fil(int id, const std::string &name, const std::string &vendor, const std::string &material, std::optional<double> nozzle = std::nullopt,
             std::optional<double> bed = std::nullopt)
{
    Filament f;
    f.id = id;
    f.name = name;
    f.vendor.emplace();
    f.vendor->name = vendor;
    f.material = material;
    f.settings_extruder_temp = nozzle;
    f.settings_bed_temp = bed;
    return f;
}

int nozzle(const Preset &p) { return p.config.option<ConfigOptionInts>("nozzle_temperature")->values.at(0); }
int ival(const Preset &p, const char *k) { return p.config.option<ConfigOptionInts>(k)->values.at(0); }

std::map<std::string, std::string> snapshot(const PresetCollection &c)
{
    std::map<std::string, std::string> out;
    for (auto it = c.cbegin(); it != c.cend(); ++it) {
        if (!it->is_user())
            continue;
        std::string s;
        for (const auto &k : it->config.keys())
            s += k + "=" + it->config.opt_serialize(k) + "\n";
        out[it->name] = s;
    }
    return out;
}

size_t user_count(const PresetCollection &c)
{
    return std::count_if(c.cbegin(), c.cend(), [](const Preset &p) { return p.is_user(); });
}

const ValueChange *change(const PresetResult &r, const std::string &key)
{
    for (const auto &c : r.changes)
        if (c.key == key)
            return &c;
    return nullptr;
}

Link link_of(const Preset &p)
{
    auto l = parse(p.config.option<ConfigOptionStrings>("filament_spoolman_link")->values.at(0));
    REQUIRE(l.has_value());
    return *l;
}

Preset &add_user(PresetCollection &c, const std::string &name, const std::string &parent, const std::string &link)
{
    DynamicPrintConfig cfg = c.find_preset(parent)->config;
    cfg.option<ConfigOptionString>("inherits", true)->value = parent;
    cfg.set_key_value("nozzle_temperature", new ConfigOptionInts{222});
    cfg.set_key_value("filament_spoolman_link", new ConfigOptionStrings{link});
    Preset &p = c.load_preset(std::string(), name, cfg, false);
    p.is_visible = true;
    return p;
}

const std::string kName = "Acme PETG Red (Spoolman) @T";

} // namespace

TEST_CASE("Spoolman presets: parent candidates are the compatible system presets", "[Spoolman]")
{
    Fixture f;
    auto onA = build_parent_candidates(f.bundle.filaments, f.printer("PrinterA"));
    auto onB = build_parent_candidates(f.bundle.filaments, f.printer("PrinterB"));
    auto all = build_parent_candidates(f.bundle.filaments, nullptr);
    auto names = [](const std::vector<ParentCandidate> &v) {
        std::vector<std::string> n;
        for (auto &c : v) n.push_back(c.name);
        std::sort(n.begin(), n.end());
        return n;
    };
    CHECK(names(onA) == std::vector<std::string>{"Generic PETG @T", "Generic PLA @T"});
    CHECK(names(onB) == std::vector<std::string>{"Generic PETG @T"}); // PLA is for PrinterA only
    CHECK(names(all) == names(onA));
    REQUIRE(onB.size() == 1);
    CHECK(onB[0].filament_type == "PETG");
    CHECK(onB[0].filament_vendor == "Generic");
    // user presets and the default preset are never candidates
    add_user(f.bundle.filaments, "Mine", "Generic PETG @T", "");
    CHECK(build_parent_candidates(f.bundle.filaments, f.printer()).size() == 2);
    // no parent for PLA on PrinterB: skipped, nothing created
    auto r = f.run({fil(1, "PLA Red", "Acme", "PLA")} , {});
    CHECK(r.created.size() == 1); // PrinterA (the fixture's context) has the PLA parent
    Fixture g;
    UpsertContext c = g.ctx();
    c.printer = g.printer("PrinterB");
    auto r2 = upsert_catalog(g.bundle.filaments, {fil(1, "PLA Red", "Acme", "PLA")}, c);
    CHECK(r2.created.empty());
    REQUIRE(r2.skipped.size() == 1);
    CHECK(r2.skipped[0].spoolman_id == 1);
    CHECK(user_count(g.bundle.filaments) == 0);   // (h) no parent: nothing created
}

TEST_CASE("Spoolman presets: create inherits the parent, applies overrides and the link", "[Spoolman]")
{
    Fixture f;
    auto r = f.run({fil(5, "PETG Red", "Acme", "PETG", 250, 70)});
    REQUIRE(r.created.size() == 1);
    CHECK(r.skipped.empty());
    CHECK(r.created[0].preset_name == kName);
    CHECK(r.created[0].parent_name == "Generic PETG @T");
    const Preset *p = f.get(kName);
    REQUIRE(p != nullptr);
    CHECK(p->is_user());
    CHECK(p->inherits() == "Generic PETG @T");
    CHECK(nozzle(*p) == 250);
    CHECK(ival(*p, "nozzle_temperature_initial_layer") == 250);
    CHECK(ival(*p, "hot_plate_temp") == 70);
    // the change list says parent -> Spoolman with the source
    const ValueChange *c = change(r.created[0], "nozzle_temperature");
    REQUIRE(c != nullptr);
    CHECK(c->old_value == "200");
    CHECK(c->new_value == "250");
    CHECK(c->source == "native:settings_extruder_temp");
    Link l = link_of(*p);
    CHECK(l.spoolman_id == 5);
    CHECK(l.sources.at("nozzle_temperature") == "native:settings_extruder_temp");
    CHECK(l.sources.at("hot_plate_temp") == "native:settings_bed_temp");
    // a key Spoolman does not provide stays the parent's
    CHECK(p->config.option<ConfigOptionFloats>("filament_flow_ratio")->values == f.get("Generic PETG @T")->config.option<ConfigOptionFloats>("filament_flow_ratio")->values);
    // the parent is untouched
    CHECK(nozzle(*f.get("Generic PETG @T")) == 200);
}

TEST_CASE("Spoolman presets: a same-name preset that is not linked to the same id is never overwritten (known-bad a)", "[Spoolman]")
{
    for (int variant = 0; variant < 2; ++variant) {
        INFO("variant " << variant << " (0 = unlinked, 1 = linked to another id)");
        Fixture f;
        Link other;
        other.spoolman_id = 99;
        add_user(f.bundle.filaments, kName, "Generic PETG @T", variant == 0 ? "" : *serialize(other));
        const auto before = snapshot(f.bundle.filaments);
        auto r = f.run({fil(5, "PETG Red", "Acme", "PETG", 250, 70)});
        REQUIRE(r.created.size() == 1);
        CHECK(r.updated.empty());
        CHECK(r.created[0].preset_name == "Acme PETG Red (Spoolman) #2 @T");
        CHECK(snapshot(f.bundle.filaments).at(kName) == before.at(kName)); // byte-identical
        CHECK(nozzle(*f.get(kName)) == 222);
        CHECK(nozzle(*f.get("Acme PETG Red (Spoolman) #2 @T")) == 250);
        // and the second run refreshes the #2 preset (found by its link), not a #3
        auto r2 = f.run({fil(5, "PETG Red", "Acme", "PETG", 250, 70)});
        CHECK(r2.created.empty());
        CHECK(r2.unchanged.size() == 1);
        CHECK(user_count(f.bundle.filaments) == 2);
    }
}

TEST_CASE("Spoolman presets: a second run changes nothing (idempotence)", "[Spoolman]")
{
    Fixture f;
    std::vector<Filament> cat = {fil(5, "PETG Red", "Acme", "PETG", 250, 70), fil(6, "PLA Blue", "Acme", "PLA", 210, 60)};
    auto r1 = f.run(cat);
    REQUIRE(r1.created.size() == 2);
    const auto before = snapshot(f.bundle.filaments);
    auto r2 = f.run(cat);
    CHECK(r2.created.empty());
    CHECK(r2.updated.empty());
    CHECK(r2.unchanged.size() == 2);
    CHECK(r2.orphaned.empty());
    CHECK(r2.duplicates.empty());
    CHECK(snapshot(f.bundle.filaments) == before); // config bytes identical
}

TEST_CASE("Spoolman presets: refresh follows Spoolman, a removed value resets to the parent", "[Spoolman]")
{
    Fixture f;
    f.run({fil(5, "PETG Red", "Acme", "PETG", 250, 70)});
    auto r = f.run({fil(5, "PETG Red", "Acme", "PETG", 255, 70)});
    REQUIRE(r.updated.size() == 1);
    CHECK(r.created.empty());
    const ValueChange *c = change(r.updated[0], "nozzle_temperature");
    REQUIRE(c != nullptr);
    CHECK(c->old_value == "250");
    CHECK(c->new_value == "255");
    CHECK(c->source == "native:settings_extruder_temp");
    CHECK(change(r.updated[0], "hot_plate_temp") == nullptr); // bed unchanged
    CHECK(nozzle(*f.get(kName)) == 255);

    // Spoolman no longer has the nozzle temperature: back to the parent's 200 / 205, bed stays
    auto r2 = f.run({fil(5, "PETG Red", "Acme", "PETG", std::nullopt, 70)});
    REQUIRE(r2.updated.size() == 1);
    CHECK(nozzle(*f.get(kName)) == 200);
    CHECK(ival(*f.get(kName), "nozzle_temperature_initial_layer") == 205);
    c = change(r2.updated[0], "nozzle_temperature");
    REQUIRE(c != nullptr);
    CHECK(c->old_value == "255");
    CHECK(c->new_value == "200");
    CHECK(c->source == "parent");
    Link l = link_of(*f.get(kName));
    CHECK(l.sources.count("nozzle_temperature") == 0);
    CHECK(l.sources.count("hot_plate_temp") == 1);
    CHECK(ival(*f.get(kName), "hot_plate_temp") == 70);
}

TEST_CASE("Spoolman presets: a rename keeps the preset name and link", "[Spoolman]")
{
    Fixture f;
    f.run({fil(5, "PETG Red", "Acme", "PETG", 250, 70)});
    auto r = f.run({fil(5, "PETG Crimson", "Acme", "PETG", 252, 70)});
    CHECK(r.created.empty());
    REQUIRE(r.updated.size() == 1);
    CHECK(r.updated[0].preset_name == kName);
    CHECK(user_count(f.bundle.filaments) == 1);
    CHECK(nozzle(*f.get(kName)) == 252);
}

TEST_CASE("Spoolman presets: a vanished filament orphans its preset, an empty catalog orphans nothing", "[Spoolman]")
{
    Fixture f;
    f.run({fil(5, "PETG Red", "Acme", "PETG", 250, 70), fil(6, "PLA Blue", "Acme", "PLA", 210, 60)});
    REQUIRE(user_count(f.bundle.filaments) == 2);
    const auto before = snapshot(f.bundle.filaments);

    auto empty = f.run({});
    CHECK(empty.orphaned.empty());
    CHECK(empty.created.empty());
    CHECK(empty.updated.empty());
    CHECK(snapshot(f.bundle.filaments) == before);

    auto r = f.run({fil(5, "PETG Red", "Acme", "PETG", 250, 70)});
    REQUIRE(r.orphaned.size() == 1);
    CHECK(r.orphaned[0].spoolman_id == 6);
    CHECK(r.orphaned[0].preset_name == "Acme PLA Blue (Spoolman) @T");
    CHECK(r.unchanged.size() == 1);
    CHECK(snapshot(f.bundle.filaments) == before); // kept, untouched
    // it is reported again on the next run, and never deleted
    CHECK(f.run({fil(5, "PETG Red", "Acme", "PETG", 250, 70)}).orphaned.size() == 1);
    CHECK(user_count(f.bundle.filaments) == 2);
    // returns to being refreshed when the filament comes back
    auto back = f.run({fil(5, "PETG Red", "Acme", "PETG", 250, 70), fil(6, "PLA Blue", "Acme", "PLA", 215, 60)});
    CHECK(back.orphaned.empty());
    CHECK(back.updated.size() == 1);
}

TEST_CASE("Spoolman presets: a preset the user deleted comes back as new", "[Spoolman]")
{
    Fixture f;
    f.run({fil(5, "PETG Red", "Acme", "PETG", 250, 70)});
    REQUIRE(f.bundle.filaments.select_preset_by_name("Generic PETG @T", true));
    REQUIRE(f.bundle.filaments.find_preset(kName) != nullptr);
    REQUIRE(f.bundle.filaments.select_preset_by_name(kName, true));
    f.bundle.filaments.delete_current_preset();
    REQUIRE(f.get(kName) == nullptr);
    auto r = f.run({fil(5, "PETG Red", "Acme", "PETG", 250, 70)});
    CHECK(r.created.size() == 1);
}

TEST_CASE("Spoolman presets: printer limits reject only the offending keys", "[Spoolman]")
{
    const Filament hot = [] {
        Filament x = fil(5, "PETG Red", "Acme", "PETG", 450, 105);
        x.density = 1.3;
        return x;
    }();
    {
        Fixture f;
        PrinterLimits lim;
        lim.max_nozzle_temp = 300;
        lim.max_bed_temp    = 100;
        auto r = f.run({hot}, lim);
        REQUIRE(r.created.size() == 1);
        const Preset &p = *f.get(kName);
        CHECK(nozzle(p) == 200);                                   // parent's, NOT 450
        CHECK(ival(p, "nozzle_temperature_initial_layer") == 205);
        CHECK(ival(p, "hot_plate_temp") == 55);                    // parent's, NOT 105
        CHECK(p.config.option<ConfigOptionFloats>("filament_density")->values.at(0) == 1.3); // the rest still applies
        std::vector<std::string> keys;
        for (const auto &x : r.rejected) {
            CHECK(x.reason == "exceeds printer limit");
            CHECK(x.spoolman_id == 5);
            CHECK(x.preset_name == kName);
            keys.push_back(x.key_or_field);
        }
        CHECK(std::count(keys.begin(), keys.end(), "nozzle_temperature=450") == 1);
        CHECK(std::count(keys.begin(), keys.end(), "hot_plate_temp=105") == 1);
        CHECK(keys.size() == 2 + 12);
        const Link l = link_of(p);
        CHECK(l.sources.count("nozzle_temperature") == 0);
        CHECK(l.sources.count("filament_density") == 1);
    }
    {   // limits only on the nozzle: bed 105 is applied
        Fixture f;
        PrinterLimits lim;
        lim.max_nozzle_temp = 300;
        f.run({hot}, lim);
        CHECK(nozzle(*f.get(kName)) == 200);
        CHECK(ival(*f.get(kName), "hot_plate_temp") == 105);
    }
    {   // exactly at the limit is fine
        Fixture f;
        PrinterLimits lim;
        lim.max_nozzle_temp = 450;
        lim.max_bed_temp    = 105;
        auto r = f.run({hot}, lim);
        CHECK(r.rejected.empty());
        CHECK(nozzle(*f.get(kName)) == 450);
    }
    {   // no limits struct: applied
        Fixture f;
        auto r = f.run({hot});
        CHECK(r.rejected.empty());
        CHECK(nozzle(*f.get(kName)) == 450);
        CHECK(ival(*f.get(kName), "hot_plate_temp") == 105);
        // a later run with a limit resets the key to the parent's value
        PrinterLimits lim;
        lim.max_nozzle_temp = 300;
        auto r2 = f.run({hot}, lim);
        REQUIRE(r2.updated.size() == 1);
        CHECK(nozzle(*f.get(kName)) == 200);
    }
}

TEST_CASE("Spoolman presets: two presets with one link, only the generated-name one is refreshed", "[Spoolman]")
{
    Fixture f;
    f.run({fil(5, "PETG Red", "Acme", "PETG", 250, 70)});
    // a Save-as copy: same link, same parent, another name that sorts BEFORE the generated one
    Link l = link_of(*f.get(kName));
    add_user(f.bundle.filaments, "A copy of it", "Generic PETG @T", *serialize(l));
    const auto before = snapshot(f.bundle.filaments);

    auto r = f.run({fil(5, "PETG Red", "Acme", "PETG", 260, 70)});
    CHECK(r.created.empty());
    REQUIRE(r.updated.size() == 1);
    CHECK(r.updated[0].preset_name == kName);
    CHECK(nozzle(*f.get(kName)) == 260);
    REQUIRE(r.duplicates.size() == 1);
    CHECK(r.duplicates[0].preset_name == "A copy of it");
    CHECK(r.duplicates[0].refreshed_preset == kName);
    CHECK(snapshot(f.bundle.filaments).at("A copy of it") == before.at("A copy of it")); // never touched
    CHECK(nozzle(*f.get("A copy of it")) == 222);
}

TEST_CASE("Spoolman presets: unlinked and unparsable-link user presets are never touched", "[Spoolman]")
{
    Fixture f;
    add_user(f.bundle.filaments, "Mine", "Generic PETG @T", "");
    add_user(f.bundle.filaments, "Broken", "Generic PETG @T", "{not json");
    const auto before = snapshot(f.bundle.filaments);
    auto r = f.run({fil(5, "PETG Red", "Acme", "PETG", 250, 70)});
    CHECK(r.created.size() == 1);
    CHECK(r.orphaned.empty());
    CHECK(snapshot(f.bundle.filaments).at("Mine") == before.at("Mine"));
    CHECK(snapshot(f.bundle.filaments).at("Broken") == before.at("Broken"));
    // an embedded project preset with the link is not touched either
    f.get("Mine");
    for (auto it = f.bundle.filaments.lbegin(); it != f.bundle.filaments.end(); ++it)
        if (it->name == kName) {
            it->is_project_embedded = true;
            it->config.option<ConfigOptionInts>("nozzle_temperature")->values = {211};
        }
    auto r2 = f.run({fil(5, "PETG Red", "Acme", "PETG", 250, 70)});
    CHECK(r2.updated.empty());
    CHECK(nozzle(*f.get(kName)) == 211);
}

TEST_CASE("Spoolman presets: name is sanitised, vendor not repeated, duplicate catalog ids skipped", "[Spoolman]")
{
    Fixture f;
    auto r = f.run({fil(1, "Acme PETG 1/2", "Acme", "PETG"), fil(1, "Other", "Acme", "PETG")});
    REQUIRE(r.created.size() == 1);
    CHECK(r.created[0].preset_name == "Acme PETG 1-2 (Spoolman) @T");
    REQUIRE(r.skipped.size() == 1);
    CHECK(r.skipped[0].reason == "duplicate filament id in the catalog");
}

TEST_CASE("Spoolman presets: a generated preset round-trips through the preset JSON with its link", "[Spoolman]")
{
    Fixture f;
    auto r = f.run({fil(5, "PETG Red", "Acme", "PETG", 250, 70)});
    REQUIRE(r.created.size() == 1);
    Preset *p = nullptr;
    for (auto it = f.bundle.filaments.lbegin(); it != f.bundle.filaments.end(); ++it)
        if (it->name == kName)
            p = &*it;
    REQUIRE(p != nullptr);
    CHECK(p->file == f.bundle.filaments.path_from_name(kName));
    ScopedSlic3rTemporaryDir dir("orca_spoolman_presets");
    p->file = (dir.path() / "p.json").string();
    DynamicPrintConfig parent_cfg = f.get("Generic PETG @T")->config;
    p->save(&parent_cfg);

    DynamicPrintConfig loaded = DynamicPrintConfig::full_print_config();
    std::map<std::string, std::string> kv;
    std::string reason;
    loaded.load_from_json(p->file, ForwardCompatibilitySubstitutionRule::Disable, kv, reason);
    CHECK(reason.empty());
    CHECK(loaded.option<ConfigOptionString>("inherits")->value == "Generic PETG @T");
    CHECK(loaded.option<ConfigOptionStrings>("filament_spoolman_link")->values == p->config.option<ConfigOptionStrings>("filament_spoolman_link")->values);
    CHECK(loaded.option<ConfigOptionInts>("nozzle_temperature")->values == std::vector<int>{250});
    CHECK(loaded.option<ConfigOptionStrings>("filament_settings_id")->values == std::vector<std::string>{kName}); // own name, like Save
    CHECK(p->config.option<ConfigOptionStrings>("filament_settings_id")->values == std::vector<std::string>{kName});
}

// ---- review round: names, flags, ambiguity, supersede, limits, contract ------------------------------------------------------------------
namespace {

Preset *mut(PresetCollection &c, const std::string &name)
{
    for (auto it = c.lbegin(); it != c.end(); ++it)
        if (it->name == name)
            return &*it;
    return nullptr;
}

std::string cfg_text(const Preset &p)
{
    std::string s;
    for (const auto &k : p.config.keys())
        s += k + "=" + p.config.opt_serialize(k) + "\n";
    return s;
}

std::string lc(std::string s)
{
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

TEST_CASE("Spoolman presets: names are compared case-insensitively", "[Spoolman]")
{
    {   // the user's own 'ACME PLA (SPOOLMAN) @T' is never overwritten or duplicated under another case
        Fixture f;
        add_user(f.bundle.filaments, "ACME PLA (SPOOLMAN) @T", "Generic PLA @T", "");
        const std::string before = cfg_text(*f.get("ACME PLA (SPOOLMAN) @T"));
        auto r = f.run({fil(1, "PLA", "Acme", "PLA", 210)});
        REQUIRE(r.created.size() == 1);
        CHECK(r.created[0].preset_name == "Acme PLA (Spoolman) #2 @T");
        CHECK(cfg_text(*f.get("ACME PLA (SPOOLMAN) @T")) == before);
    }
    {   // 'PLA' and 'pla' in one catalog: two presets, no two names equal ignoring case
        Fixture f;
        auto r = f.run({fil(1, "PLA", "Acme", "PLA", 210), fil(2, "pla", "Acme", "PLA", 215)});
        REQUIRE(r.created.size() == 2);
        CHECK(lc(r.created[0].preset_name) != lc(r.created[1].preset_name));
        CHECK(r.created[0].preset_name == "Acme PLA (Spoolman) @T");
        CHECK(r.created[1].preset_name == "Acme pla (Spoolman) #2 @T");
        // idempotent
        auto r2 = f.run({fil(1, "PLA", "Acme", "PLA", 210), fil(2, "pla", "Acme", "PLA", 215)});
        CHECK(r2.unchanged.size() == 2);
        CHECK(user_count(f.bundle.filaments) == 2);
    }
}

TEST_CASE("Spoolman presets: generated names are hardened", "[Spoolman]")
{
    auto name_of = [](Filament x) {
        Fixture f;
        auto r = f.run({x});
        REQUIRE(r.created.size() == 1);
        return r.created[0].preset_name;
    };
    {   // 400 bytes of 2-byte characters: capped under 200 bytes with the file extension, tag and suffix kept, cut on a UTF-8 boundary
        const std::string n = name_of(fil(1, std::string(200, '\0').replace(0, 200, [] { std::string s; for (int i = 0; i < 200; ++i) s += "\xC3\xA9"; return s; }()), "Acme", "PETG"));
        CHECK(n.size() + 5 < 200);
        CHECK(n.size() > 150);
        CHECK(n.substr(n.size() - 14) == " (Spoolman) @T");
        const std::string label = n.substr(0, n.size() - 14);
        CHECK(label.size() % 2 == 1);                // "Acme " (5 bytes) + whole 2-byte characters: never a cut character
        CHECK(label.substr(label.size() - 2) == "\xC3\xA9");
    }
    CHECK(name_of(fil(1, ".hidden", "Acme", "PETG")) == "Acme hidden (Spoolman) @T");
    CHECK(name_of(fil(1, "  .. x ", "Acme", "PETG")) == "Acme x (Spoolman) @T");
    CHECK(name_of(fil(1, "PETG Red.", "Acme", "PETG")) == "Acme PETG Red (Spoolman) @T");
    CHECK(name_of(fil(7, "", "Acme", "PETG")) == "Acme Filament 7 (Spoolman) @T");
    CHECK(name_of(fil(7, " . ", "Acme", "PETG")) == "Acme Filament 7 (Spoolman) @T");
    CHECK(name_of(fil(1, "A\x7F" "B\x01" "C", "Acme", "PETG")) == "Acme ABC (Spoolman) @T");
    {   // Windows reserves a FILE whose stem (before the first dot) is a device name: only then is '_' inserted after the stem
        auto bare = [&](const std::string &n) {
            Filament c = fil(1, n, "", "PETG");
            c.vendor.reset();
            return name_of(c);
        };
        CHECK(bare("nul.txt") == "nul_.txt (Spoolman) @T");
        CHECK(bare("LPT9.x") == "LPT9_.x (Spoolman) @T");
        CHECK(bare("CON") == "CON (Spoolman) @T");              // "CON (Spoolman) @T.json" has another stem
        CHECK(bare("con.") == "con (Spoolman) @T");
        CHECK(bare("COM1 ") == "COM1 (Spoolman) @T");
        CHECK(bare("Console Red") == "Console Red (Spoolman) @T");
        CHECK(bare("aux .b") == "aux_ .b (Spoolman) @T");
    }
    // two different long names that truncate to the same text still get distinct presets
    {
        Fixture f;
        const std::string longname(300, 'x');
        auto r = f.run({fil(1, longname + "A", "Acme", "PETG"), fil(2, longname + "B", "Acme", "PETG")});
        REQUIRE(r.created.size() == 2);
        CHECK(lc(r.created[0].preset_name) != lc(r.created[1].preset_name));
        CHECK(r.created[1].preset_name.size() + 5 < 200);
    }
}

TEST_CASE("Spoolman presets: external, project-embedded and bundle presets with a matching link are never touched", "[Spoolman]")
{
    for (int flag = 0; flag < 3; ++flag) {
        INFO("flag " << flag << " (0 external, 1 project-embedded, 2 bundle)");
        Fixture f;
        f.run({fil(5, "PETG Red", "Acme", "PETG", 250, 70)});
        Preset *p = mut(f.bundle.filaments, kName);
        REQUIRE(p != nullptr);
        if (flag == 0) p->is_external = true;
        if (flag == 1) p->is_project_embedded = true;
        if (flag == 2) p->bundle_id = "some-bundle";
        p->config.option<ConfigOptionInts>("nozzle_temperature")->values = {211};
        const std::string before = cfg_text(*p);

        auto r = f.run({fil(5, "PETG Red", "Acme", "PETG", 260, 70)});
        CHECK(r.updated.empty());
        CHECK(r.orphaned.empty());
        REQUIRE(r.created.size() == 1);                                  // a managed preset of its own, under a counter name
        CHECK(r.created[0].preset_name == "Acme PETG Red (Spoolman) #2 @T");
        CHECK(cfg_text(*mut(f.bundle.filaments, kName)) == before);
        CHECK(nozzle(*f.get(kName)) == 211);
    }
}

TEST_CASE("Spoolman presets: several candidates and none named like the generated name is ambiguous, nothing changes", "[Spoolman]")
{
    Fixture f;
    f.run({fil(5, "PETG Red", "Acme", "PETG", 250, 70)});
    Link l = link_of(*f.get(kName));
    add_user(f.bundle.filaments, "Acme PETG Red (Spoolman) - my tune @T", "Generic PETG @T", *serialize(l));
    const auto before = snapshot(f.bundle.filaments);
    // Spoolman renames the filament: generated name changes, two presets match, none is named like it
    auto r = f.run({fil(5, "PETG Crimson", "Acme", "PETG", 270, 70)});
    CHECK(r.created.empty());
    CHECK(r.updated.empty());
    CHECK(r.unchanged.empty());
    REQUIRE(r.ambiguous.size() == 1);
    CHECK(r.ambiguous[0].spoolman_id == 5);
    CHECK(r.ambiguous[0].candidates == std::vector<std::string>{"Acme PETG Red (Spoolman) - my tune @T", kName}); // sorted by name
    CHECK(snapshot(f.bundle.filaments) == before);
    CHECK(user_count(f.bundle.filaments) == 2);
    CHECK(r.orphaned.empty());
}

TEST_CASE("Spoolman presets: a lone preset with the link is refreshed whatever its name (copy of a deleted original)", "[Spoolman]")
{
    // A lone copy cannot be told from a renamed original, so it is refreshed (documented in the header); it is never reported as a duplicate.
    Fixture f;
    Link l;
    l.spoolman_id = 5;
    add_user(f.bundle.filaments, "Acme PETG Red (Spoolman) - my tune @T", "Generic PETG @T", *serialize(l));
    auto r = f.run({fil(5, "PETG Red", "Acme", "PETG", 250, 70)});
    CHECK(r.created.empty());
    REQUIRE(r.updated.size() == 1);
    CHECK(r.updated[0].preset_name == "Acme PETG Red (Spoolman) - my tune @T");
    CHECK(r.duplicates.empty());
    CHECK(r.ambiguous.empty());
}

TEST_CASE("Spoolman presets: a changed parent supersedes the old preset (kept, listed)", "[Spoolman]")
{
    Fixture f;
    const std::string old = "Acme Red (Spoolman) @T";
    f.run({fil(5, "Red", "Acme", "PETG", 250, 70)});
    const std::string before = cfg_text(*f.get(old));
    // Spoolman now says PLA: the PLA parent is chosen, the old PETG-parented preset stays
    auto r = f.run({fil(5, "Red", "Acme", "PLA", 205, 60)});
    REQUIRE(r.created.size() == 1);
    CHECK(r.created[0].parent_name == "Generic PLA @T");
    CHECK(r.created[0].preset_name == "Acme Red (Spoolman) #2 @T"); // same suffix: the old name is taken
    CHECK(f.get("Acme Red (Spoolman) #2 @T")->inherits() == "Generic PLA @T");
    REQUIRE(r.superseded.size() == 1);
    CHECK(r.superseded[0].spoolman_id == 5);
    CHECK(r.superseded[0].preset_name == old);
    CHECK(r.superseded[0].old_parent == "Generic PETG @T");
    CHECK(r.superseded[0].new_parent == "Generic PLA @T");
    CHECK(cfg_text(*f.get(old)) == before);
    CHECK(r.orphaned.empty());
    // and it keeps being reported, still untouched, while the old preset exists
    auto r2 = f.run({fil(5, "Red", "Acme", "PLA", 205, 60)});
    CHECK(r2.unchanged.size() == 1);
    CHECK(r2.superseded.size() == 1);
}

TEST_CASE("Spoolman presets: skipped filaments list their existing linked presets", "[Spoolman]")
{
    Fixture f;
    f.run({fil(5, "PETG Red", "Acme", "PETG", 250, 70)});
    auto r = f.run({fil(5, "Mystery", "Acme", "Nylon")});   // no parent for Nylon now
    REQUIRE(r.skipped.size() == 1);
    CHECK(r.skipped[0].linked_presets == std::vector<std::string>{kName});
    CHECK(r.orphaned.empty()); // the id is still in the catalog
}

TEST_CASE("Spoolman presets: a failed apply keeps the old source in the link (fail closed)", "[Spoolman]")
{
    Fixture f;
    f.run({fil(5, "PETG Red", "Acme", "PETG", 250, 70)});
    Preset *p = mut(f.bundle.filaments, kName);
    REQUIRE(p != nullptr);
    REQUIRE(p->config.erase("hot_plate_temp")); // the preset can no longer take this key
    auto r = f.run({fil(5, "PETG Red", "Acme", "PETG", 250, 71)});
    REQUIRE(r.updated.size() == 1);
    bool found = false;
    for (const auto &x : r.rejected)
        if (x.key_or_field == "hot_plate_temp" && x.reason == "cannot be applied to the preset")
            found = true;
    CHECK(found);
    const Link l = link_of(*f.get(kName));
    CHECK(l.sources.count("hot_plate_temp") == 1);
    CHECK(l.sources.at("hot_plate_temp") == "native:settings_bed_temp");
    CHECK(ival(*f.get(kName), "cool_plate_temp") == 71); // the other plates still followed
}

TEST_CASE("Spoolman presets: printer limits apply to groups", "[Spoolman]")
{
    PrinterLimits lim;
    lim.max_nozzle_temp = 300;
    lim.max_bed_temp    = 100;
    {   // native nozzle 250 is fine, but an OFD initial-layer 320 is not: BOTH nozzle keys are rejected
        Fixture f;
        Filament x = fil(5, "PETG Red", "Acme", "PETG", 250, 70);
        x.extras["slicer_settings_orcaslicer"] = R"({"nozzle_temperature_initial_layer": 320})";
        auto r = f.run({x}, lim);
        REQUIRE(r.created.size() == 1);
        CHECK(nozzle(*f.get(kName)) == 200);
        CHECK(ival(*f.get(kName), "nozzle_temperature_initial_layer") == 205);
        std::map<std::string, std::string> why;
        for (const auto &j : r.rejected) why[j.key_or_field] = j.reason;
        CHECK(why.at("nozzle_temperature=250").find("group") != std::string::npos);
        CHECK(why.at("nozzle_temperature_initial_layer=320") == "exceeds printer limit");
        CHECK(ival(*f.get(kName), "hot_plate_temp") == 70); // bed untouched by the nozzle problem
    }
    {   // one plate's initial-layer key over the limit rejects that plate's pair only
        Fixture f;
        Filament x = fil(5, "PETG Red", "Acme", "PETG", 250, 70);
        x.extras["slicer_settings_orcaslicer"] = R"({"hot_plate_temp_initial_layer": 110})";
        auto r = f.run({x}, lim);
        REQUIRE(r.created.size() == 1);
        CHECK(ival(*f.get(kName), "hot_plate_temp") == 55);
        CHECK(ival(*f.get(kName), "hot_plate_temp_initial_layer") == 55);
        CHECK(ival(*f.get(kName), "cool_plate_temp") == 70);
        CHECK(ival(*f.get(kName), "cool_plate_temp_initial_layer") == 70);
        CHECK(nozzle(*f.get(kName)) == 250);
        CHECK(r.rejected.size() == 2);
    }
    {   // the parent's own value already over the limit is kept and reported as a warning
        Fixture f;
        mut(f.bundle.filaments, "Generic PETG @T")->config.option<ConfigOptionInts>("nozzle_temperature")->values = {350};
        auto r = f.run({fil(5, "PETG Red", "Acme", "PETG")}, lim);
        REQUIRE(r.created.size() == 1);
        CHECK(nozzle(*f.get(kName)) == 350);
        REQUIRE(r.rejected.size() == 1);
        CHECK(r.rejected[0].key_or_field == "nozzle_temperature");
        CHECK(r.rejected[0].warning);
        CHECK(r.rejected[0].reason.find("parent's own value 350") != std::string::npos);
    }
}

TEST_CASE("Spoolman presets: the selection and the edited preset are left alone", "[Spoolman]")
{
    Fixture f;
    PresetCollection &c = f.bundle.filaments;
    add_user(c, "Mine", "Generic PETG @T", "");
    REQUIRE(c.select_preset_by_name("Mine", true));
    c.get_edited_preset().config.option<ConfigOptionInts>("nozzle_temperature")->values = {233}; // dirty edit
    auto state = [&] { return std::make_tuple(c.get_selected_preset_name(), c.get_edited_preset().name, cfg_text(c.get_edited_preset()), c.current_is_dirty()); };
    const auto before = state();
    REQUIRE(std::get<3>(before));

    // creates presets sorting before and after "Mine"
    auto r1 = f.run({fil(5, "PETG Red", "Acme", "PETG", 250, 70), fil(6, "PLA Z", "Zed", "PLA", 210, 60)});
    REQUIRE(r1.created.size() == 2);
    CHECK(state() == before);
    CHECK(r1.needs_reselect.empty());
    // updates one
    auto r2 = f.run({fil(5, "PETG Red", "Acme", "PETG", 255, 70), fil(6, "PLA Z", "Zed", "PLA", 210, 60)});
    REQUIRE(r2.updated.size() == 1);
    CHECK(state() == before);
    CHECK(r2.needs_reselect.empty());

    // when the refreshed preset IS the selected one: reported, edited copy not refreshed
    REQUIRE(c.select_preset_by_name(kName, true));
    auto r3 = f.run({fil(5, "PETG Red", "Acme", "PETG", 260, 70), fil(6, "PLA Z", "Zed", "PLA", 210, 60)});
    REQUIRE(r3.updated.size() == 1);
    CHECK(r3.needs_reselect == std::vector<std::string>{kName});
    CHECK(c.get_edited_preset().config.option<ConfigOptionInts>("nozzle_temperature")->values.at(0) == 255); // stale on purpose
}

TEST_CASE("Spoolman presets: the single Spoolman value goes to every element of a vector option", "[Spoolman]")
{
    Fixture f;
    mut(f.bundle.filaments, "Generic PETG @T")->config.option<ConfigOptionInts>("nozzle_temperature")->values = {200, 210};
    f.run({fil(5, "PETG Red", "Acme", "PETG", 250, 70)});
    CHECK(f.get(kName)->config.option<ConfigOptionInts>("nozzle_temperature")->values == std::vector<int>{250, 250});
}

TEST_CASE("Spoolman presets: names differing only in case refresh their own preset", "[Spoolman]")
{
    // as if both were loaded from a case-sensitive disk: the refresh must start from the targeted preset's OWN config
    Fixture f;
    Link l;
    l.spoolman_id = 5;
    Preset &a = add_user(f.bundle.filaments, "Foo (Spoolman) @T", "Generic PETG @T", *serialize(l));
    a.config.set_key_value("filament_flow_ratio", new ConfigOptionFloats{0.66});
    Preset &b = add_user(f.bundle.filaments, "foo (Spoolman) @T", "Generic PETG @T", *serialize(l));
    b.config.set_key_value("filament_flow_ratio", new ConfigOptionFloats{0.77});
    REQUIRE(f.get("Foo (Spoolman) @T") != f.get("foo (Spoolman) @T"));
    const std::string a_before = cfg_text(*mut(f.bundle.filaments, "Foo (Spoolman) @T"));
    Filament x = fil(5, "foo", "", "PETG", 250, 70);
    x.vendor.reset();
    auto r = f.run({x});
    REQUIRE(r.updated.size() == 1);
    CHECK(r.updated[0].preset_name == "foo (Spoolman) @T");
    REQUIRE(r.duplicates.size() == 1);
    CHECK(r.duplicates[0].preset_name == "Foo (Spoolman) @T");
    const Preset *pa = mut(f.bundle.filaments, "Foo (Spoolman) @T");
    const Preset *pb = mut(f.bundle.filaments, "foo (Spoolman) @T");
    REQUIRE(pa != nullptr);
    REQUIRE(pb != nullptr);
    CHECK(cfg_text(*pa) == a_before);
    CHECK(pb->config.option<ConfigOptionFloats>("filament_flow_ratio")->values.at(0) == 0.77); // its own base, not the other preset's
    CHECK(nozzle(*pb) == 250);
}

TEST_CASE("Spoolman presets: a link that would grow past the cap under fail-closed writes nothing", "[Spoolman]")
{
    Fixture f;
    f.run({fil(5, "PETG Red", "Acme", "PETG", 250, 70)});
    Preset *p = mut(f.bundle.filaments, kName);
    REQUIRE(p != nullptr);
    Link l = link_of(*p);
    l.sources["hot_plate_temp"] = std::string(3000, 'x'); // fits on its own
    p->config.set_key_value("filament_spoolman_link", new ConfigOptionStrings{*serialize(l)});
    REQUIRE(p->config.erase("hot_plate_temp"));            // so applying the bed temperature fails
    const std::string before = cfg_text(*p);
    Filament x = fil(5, "PETG Red", "Acme", "PETG", 250, 71);
    x.external_id = std::string(600, 'e');                 // the new link text with the restored long source exceeds 4096
    auto r = f.run({x});
    CHECK(r.updated.empty());
    CHECK(r.created.empty());
    REQUIRE(r.skipped.size() == 1);
    CHECK(r.skipped[0].reason == "link too large");
    CHECK(cfg_text(*f.get(kName)) == before);              // no partial write
}

TEST_CASE("Spoolman presets: vendor text with ; and quotes stays ONE element", "[Spoolman]")
{
    {   // why apply_value sets strings directly: the generic deserializer mangles these (documented finding)
        ConfigOptionStrings semi, quote;
        REQUIRE(semi.deserialize("Acme; Corp"));
        CHECK(semi.values.size() == 2);
        REQUIRE(quote.deserialize("\"quoted\""));
        CHECK(quote.values == std::vector<std::string>{"quoted"}); // quotes eaten
    }
    for (const std::string &v : std::vector<std::string>{std::string("Acme; Corp"), std::string("Brand \"X\""), std::string("O'Brien"), std::string("back\\slash"),
                                std::string("caf\xC3\xA9 \xE2\x9C\x93"), std::string("\"quoted\"")}) {
        INFO(v);
        Fixture f;
        auto r = f.run({fil(5, "PETG Red", v, "PETG", 250, 70)});
        REQUIRE(r.created.size() == 1);
        const std::string name = r.created[0].preset_name;
        const Preset *p = f.get(name);
        REQUIRE(p != nullptr);
        CHECK(p->config.option<ConfigOptionStrings>("filament_vendor")->values == std::vector<std::string>{v});
        CHECK(link_of(*p).sources.count("filament_vendor") == 1);

        ScopedSlic3rTemporaryDir dir("orca_spoolman_vendor");
        Preset *mp = mut(f.bundle.filaments, name);
        mp->file = (dir.path() / "p.json").string();
        DynamicPrintConfig parent_cfg = f.get("Generic PETG @T")->config;
        mp->save(&parent_cfg);
        DynamicPrintConfig loaded = DynamicPrintConfig::full_print_config();
        std::map<std::string, std::string> kv;
        std::string reason;
        loaded.load_from_json(mp->file, ForwardCompatibilitySubstitutionRule::Disable, kv, reason);
        CHECK(loaded.option<ConfigOptionStrings>("filament_vendor")->values == std::vector<std::string>{v});

        f.bundle.filament_presets = {name};
        f.bundle.filaments.select_preset_by_name(name, true);
        CHECK(f.bundle.full_config().option<ConfigOptionStrings>("filament_vendor")->values == std::vector<std::string>{v});
    }
}

TEST_CASE("Spoolman presets: a differing filament_settings_id alone is written but not reported as an update", "[Spoolman]")
{
    Fixture f;
    f.run({fil(5, "PETG Red", "Acme", "PETG", 250, 70)});
    mut(f.bundle.filaments, kName)->config.set_key_value("filament_settings_id", new ConfigOptionStrings{"something else"});
    auto r = f.run({fil(5, "PETG Red", "Acme", "PETG", 250, 70)});
    CHECK(r.updated.empty());
    CHECK(r.unchanged.size() == 1);
    CHECK(f.get(kName)->config.option<ConfigOptionStrings>("filament_settings_id")->values == std::vector<std::string>{kName});
    CHECK(f.run({fil(5, "PETG Red", "Acme", "PETG", 250, 70)}).unchanged.size() == 1);
}

// ---- real fixtures --------------------------------------------------------------------------------------------------------------------
namespace {

std::vector<Filament> fixture_filaments()
{
    std::ifstream in(std::string(TEST_DATA_DIR) + "/spoolman/filament.json");
    REQUIRE(in.is_open());
    auto r = parse_filaments(nlohmann::json::parse(in));
    REQUIRE(r.ok());
    return r.items;
}

} // namespace

TEST_CASE("Spoolman presets: fixture filament 16 against the system set built from the real candidates TSV", "[Spoolman]")
{
    PresetBundle bundle;
    Preset &printer = add_printer(bundle, "U1-like printer");
    std::ifstream in(std::string(TEST_DATA_DIR) + "/spoolman/candidates_u1_0.4.tsv");
    REQUIRE(in.is_open());
    std::string line;
    size_t n = 0;
    while (std::getline(in, line)) {
        std::vector<std::string> col;
        std::stringstream ss(line);
        std::string c;
        while (std::getline(ss, c, '\t'))
            col.push_back(c);
        col.resize(6);
        add_system(bundle.filaments, col[0], col[2], col[3], {}, col[4], col[5]).alias = col[1];
        ++n;
    }
    REQUIRE(n > 300);

    UpsertContext ctx;
    ctx.printer         = &printer;
    ctx.preferred_token = "@U1 0.4 nozzle";
    const auto fils = fixture_filaments();
    UpsertReport r = upsert_catalog(bundle.filaments, fils, ctx);
    REQUIRE(r.created.size() + r.skipped.size() == fils.size());

    const PresetResult *res = nullptr;
    for (const auto &c : r.created)
        if (c.spoolman_id == 16)
            res = &c;
    REQUIRE(res != nullptr);
    CHECK(res->preset_name == "Snapmaker PETG HF Red (Spoolman) @U1 0.4 nozzle");
    CHECK(res->parent_name == "Snapmaker PETG HF @U1 0.4 nozzle");
    const Preset *p = bundle.filaments.find_preset(res->preset_name);
    REQUIRE(p != nullptr);
    CHECK(p->inherits() == "Snapmaker PETG HF @U1 0.4 nozzle");
    CHECK(p->filament_id == "OFL9aRjN");
    CHECK(nozzle(*p) == 250);
    CHECK(ival(*p, "nozzle_temperature_initial_layer") == 250);
    CHECK(ival(*p, "hot_plate_temp") == 70);
    CHECK(p->config.option<ConfigOptionFloats>("filament_density")->values.at(0) == 1.3);
    CHECK(p->config.option<ConfigOptionStrings>("default_filament_colour")->values.at(0) == "#FF1523");
    const Link l = link_of(*p);
    CHECK(l.spoolman_id == 16);
    CHECK(l.sources.at("nozzle_temperature") == "native:settings_extruder_temp");
    CHECK(l.sources.at("filament_density") == "native:density");
    CHECK(l.sources.at("default_filament_colour") == "native:color_hex");
    // the Glow PLA filaments have no parent in the real candidates: skipped, no preset
    CHECK(std::any_of(r.skipped.begin(), r.skipped.end(), [](const SkippedFilament &s) { return s.spoolman_id == 10; }));
    CHECK(user_count(bundle.filaments) == r.created.size());
    // idempotent against the real set too
    const auto before = snapshot(bundle.filaments);
    UpsertReport r2 = upsert_catalog(bundle.filaments, fils, ctx);
    CHECK(r2.created.empty());
    CHECK(r2.updated.empty());
    CHECK(r2.unchanged.size() == r.created.size());
    CHECK(snapshot(bundle.filaments) == before);
}
