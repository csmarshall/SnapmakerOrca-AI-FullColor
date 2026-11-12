// Tests that filament_spoolman_link is excluded from the OrcaCloud preset-sync payload (PresetBundle::get_differed_values_to_update).
// VERIFIED here: in-memory user presets (with and without a system parent) yield a payload without the key while other keys stay; an empty
// link leaves the payload as it is without the key in the config; the local preset JSON save still writes the key.
// NOT shown: the real cloud upload (GUI_App sync loop, OrcaCloudServiceAgent), real profile bundles, 3MF.
#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "test_utils.hpp"

#include <map>
#include <string>

using namespace Slic3r;

namespace {
const char *const kLink = "filament_spoolman_link";

struct SyncFixture
{
    PresetBundle bundle;
    SyncFixture()
    {
        DynamicPrintConfig cfg = bundle.filaments.default_preset().config;
        cfg.option<ConfigOptionString>("inherits", true)->value = "";
        cfg.set_key_value("nozzle_temperature", new ConfigOptionInts{200});
        Preset &p = bundle.filaments.load_preset(std::string(), "Parent @T", cfg, false);
        p.is_system = true;
    }
    // User preset; inherits the system parent when `parent` is set. nozzle_temperature differs from the parent either way.
    Preset &user(const std::string &name, bool parent, const std::string &link)
    {
        DynamicPrintConfig cfg = bundle.filaments.find_preset("Parent @T", false)->config;
        cfg.option<ConfigOptionString>("inherits", true)->value = parent ? "Parent @T" : "";
        cfg.set_key_value("nozzle_temperature", new ConfigOptionInts{250});
        if (!link.empty())
            cfg.set_key_value(kLink, new ConfigOptionStrings{link});
        return bundle.filaments.load_preset(std::string(), name, cfg, false);
    }
    std::map<std::string, std::string> payload(Preset &p)
    {
        std::map<std::string, std::string> kv;
        CHECK(bundle.get_differed_values_to_update(p, kv) == 0);
        return kv;
    }
};
const std::string kJson = R"({"spoolman_id":7})";
} // namespace

TEST_CASE("Spoolman cloud sync: link is excluded from the payload of a preset with a parent", "[Spoolman]")
{
    SyncFixture f;
    Preset &p = f.user("U1", true, kJson);
    REQUIRE(p.config.option<ConfigOptionStrings>(kLink)->values.at(0) == kJson);
    auto kv = f.payload(p);
    CHECK(kv.count(kLink) == 0);
    CHECK(kv.count("nozzle_temperature") == 1);
    CHECK(kv.count("inherits") == 1);
}

TEST_CASE("Spoolman cloud sync: link is excluded from the full-config payload of a preset without a parent", "[Spoolman]")
{
    SyncFixture f;
    Preset &p = f.user("U2", false, kJson);
    auto kv = f.payload(p);
    CHECK(kv.count(kLink) == 0);
    CHECK(kv.count("nozzle_temperature") == 1);
    CHECK(kv.size() > 50); // full config path
}

TEST_CASE("Spoolman cloud sync: an empty link leaves the payload unchanged", "[Spoolman]")
{
    SyncFixture f;
    Preset &with_link = f.user("U3", true, kJson);
    Preset &empty     = f.user("U4", true, "");
    auto a = f.payload(empty);
    auto b = f.payload(with_link);
    a.erase(ORCA_JSON_KEY_UPDATE_TIME); b.erase(ORCA_JSON_KEY_UPDATE_TIME);
    CHECK(a.count(kLink) == 0);
    CHECK(a == b); // link or no link, same payload
    // Same for the full-config path.
    auto c = f.payload(f.user("U5", false, ""));
    auto d = f.payload(f.user("U6", false, kJson));
    c.erase(ORCA_JSON_KEY_UPDATE_TIME); d.erase(ORCA_JSON_KEY_UPDATE_TIME);
    CHECK(c == d);
}

TEST_CASE("Spoolman cloud sync: the local preset file and config still carry the link", "[Spoolman]")
{
    SyncFixture f;
    Preset &p = f.user("U7", true, kJson);
    f.payload(p);
    CHECK(p.config.option<ConfigOptionStrings>(kLink)->values.at(0) == kJson); // payload build did not touch the preset
    ScopedSlic3rTemporaryDir dir("orca_spoolman_cloud");
    p.file = (dir.path() / "p.json").string();
    DynamicPrintConfig parent_cfg = f.bundle.filaments.find_preset("Parent @T", false)->config;
    p.save(&parent_cfg);
    DynamicPrintConfig loaded = DynamicPrintConfig::full_print_config();
    std::map<std::string, std::string> kv;
    std::string reason;
    loaded.load_from_json(p.file, ForwardCompatibilitySubstitutionRule::Disable, kv, reason);
    CHECK(loaded.option<ConfigOptionStrings>(kLink)->values.at(0) == kJson);
}
