// Tests for filament_spoolman_link: the Link helper (parse/serialize) and the option's storage behaviour (config, ini, preset JSON, project 3MF).
// The slicing side (G-code unchanged, no invalidation) lives in tests/fff_print/test_spoolman_link_gcode.cpp.
// VERIFIED: what is asserted below. NOT covered: a build without the key reading a project that has it (see the report), the GUI.
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <boost/filesystem.hpp>

#include <climits>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "libslic3r/Config.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Spoolman/SpoolmanLink.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"
#include "test_utils.hpp"

using namespace Slic3r;
using namespace Slic3r::Spoolman;

namespace {

Link sample_link()
{
    Link l;
    l.spoolman_id  = 42;
    l.external_id  = "polymaker_pla_\"red\"";
    l.sources      = {{"filament_density", "spoolman"}, {"nozzle_temperature", "preset"}};
    return l;
}

// A value of exactly 4096 bytes that parse() still accepts.
std::string max_size_link_text()
{
    Link l;
    l.spoolman_id = 7;
    l.external_id = "x";
    std::string t = serialize(l);
    l.external_id = std::string(kMaxLinkBytes - t.size() + 1, 'x');
    return serialize(l);
}

} // namespace

TEST_CASE("Link round trips through serialize and parse", "[Spoolman]")
{
    const Link l = sample_link();
    auto       p = parse(serialize(l));
    REQUIRE(p.has_value());
    CHECK(*p == l);
    CHECK(serialize(*p) == serialize(l));

    Link u;
    u.spoolman_id = INT_MAX;
    u.external_id = "caf\xC3\xA9 \xE2\x9C\x93 \xF0\x9F\x98\x80";
    u.sources     = {{"k\xC3\xA9y", "v\xC3\xA4l"}};
    auto q = parse(serialize(u));
    REQUIRE(q.has_value());
    CHECK(*q == u);
}

TEST_CASE("Link parse rejects malformed text and never throws", "[Spoolman]")
{
    for (const char *bad : {"", "   ", "nope", "{", "[]", "42", "\"s\"", "null", "{\"id\":}", "{\"id\":1", "{\"id\":\"1\"}",
                            "{\"id\":1.5}", "{\"id\":true}", "{\"id\":null}", "{\"external_id\":\"a\"}",
                            "{\"id\":1,\"external_id\":5}", "{\"id\":1,\"sources\":[]}", "{\"id\":1,\"sources\":{\"a\":1}}",
                            "{\"id\":1,\"sources\":\"x\"}"}) {
        INFO(bad);
        CHECK_FALSE(parse(bad).has_value());
    }
    CHECK_FALSE(parse(std::string_view("{\"id\":1}\0garbage", 16)).has_value());
}

TEST_CASE("Link id range is 1..INT_MAX", "[Spoolman]")
{
    CHECK(parse("{\"id\":1}").has_value());
    CHECK(parse("{\"id\":2147483647}")->spoolman_id == INT_MAX);
    for (const char *bad : {"{\"id\":0}", "{\"id\":-1}", "{\"id\":2147483648}", "{\"id\":4294967296}", "{\"id\":18446744073709551615}",
                            "{\"id\":-9223372036854775808}", "{\"id\":1e3}"}) {
        INFO(bad);
        CHECK_FALSE(parse(bad).has_value());
    }
}

TEST_CASE("Link missing optional fields give defaults, a missing id gives nullopt", "[Spoolman]")
{
    auto p = parse("{\"id\":5}");
    REQUIRE(p.has_value());
    CHECK(p->spoolman_id == 5);
    CHECK(p->external_id.empty());
    CHECK(p->sources.empty());
    auto n = parse("{\"id\":5,\"external_id\":null,\"sources\":null}");
    REQUIRE(n.has_value());
    CHECK(n->external_id.empty());
    CHECK(n->sources.empty());
    // unknown keys are ignored
    CHECK(parse("{\"id\":5,\"future\":{\"a\":1}}").has_value());
}

TEST_CASE("Link size and depth caps", "[Spoolman]")
{
    const std::string max = max_size_link_text();
    REQUIRE(max.size() == kMaxLinkBytes);
    CHECK(parse(max).has_value());

    Link l;
    l.spoolman_id = 7;
    l.external_id = std::string(kMaxLinkBytes, 'x');
    CHECK_FALSE(parse(serialize(l)).has_value());

    // depth: unknown keys count too. 8 levels of nesting pass, 9 do not.
    auto nested = [](int levels) {
        std::string s = "{\"id\":1,\"x\":";
        for (int i = 1; i < levels; ++i) s += "[";
        s += "1";
        for (int i = 1; i < levels; ++i) s += "]";
        return s + "}";
    };
    CHECK(parse(nested(8)).has_value());   // object (1) + 7 arrays = 8
    CHECK_FALSE(parse(nested(9)).has_value());
    // 4096 bytes of '[' must be rejected quickly and without crashing
    std::string deep = "{\"id\":1,\"x\":" + std::string(2000, '[') + std::string(2000, ']') + "}";
    CHECK_FALSE(parse(deep).has_value());
}

TEST_CASE("serialize enforces the size cap with an empty string", "[Spoolman]")
{
    Link l;
    l.spoolman_id = 3;
    for (int i = 0; i < 200; ++i)
        l.sources["filament_key_number_" + std::to_string(i)] = "spoolman";
    CHECK(serialize(l).empty());          // would be > 4096 bytes
    l.sources.clear();
    l.sources["a"] = "b";
    CHECK_FALSE(serialize(l).empty());
    CHECK(max_size_link_text().size() == kMaxLinkBytes);   // exactly at the cap is still written and parses
    l.external_id = std::string(kMaxLinkBytes, 'x');
    CHECK(serialize(l).empty());
}

TEST_CASE("filament_spoolman_link is a registered filament string option with an empty default", "[Spoolman]")
{
    const ConfigOptionDef *def = print_config_def.get("filament_spoolman_link");
    REQUIRE(def != nullptr);
    CHECK(def->type == coStrings);
    CHECK(def->mode == comDevelop);
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    const auto *opt = cfg.option<ConfigOptionStrings>("filament_spoolman_link");
    REQUIRE(opt != nullptr);
    CHECK(opt->values == std::vector<std::string>{""});
    const auto &keys = Preset::filament_options();
    CHECK(std::find(keys.begin(), keys.end(), "filament_spoolman_link") != keys.end());
    // not offered on any Tab page: it has no label-less GUI entry beyond develop mode, and no page code references it (checked by grep, see report)
}

TEST_CASE("filament_spoolman_link survives config, ini and preset JSON round trips", "[Spoolman]")
{
    Link big = sample_link();
    big.external_id = "q\"uote \\ back\xC3\xA9";
    const std::vector<std::string> values = {serialize(sample_link()), serialize(big), max_size_link_text(), ""};

    for (const std::string &v : values) {
        INFO(v.size());
        DynamicPrintConfig cfg;
        cfg.set_key_value("filament_spoolman_link", new ConfigOptionStrings{v});

        // serialize / deserialize of the option itself (escapes quotes)
        DynamicPrintConfig c2;
        c2.set_key_value("filament_spoolman_link", new ConfigOptionStrings{""});
        c2.set_deserialize_strict("filament_spoolman_link", cfg.opt_serialize("filament_spoolman_link"));
        CHECK(c2.option<ConfigOptionStrings>("filament_spoolman_link")->values == std::vector<std::string>{v});

        ScopedSlic3rTemporaryDir dir("orca_spoolman_link");
        // ini
        const std::string ini = (dir.path() / "p.ini").string();
        cfg.save(ini);
        DynamicPrintConfig c3 = DynamicPrintConfig::full_print_config();
        c3.load_from_ini(ini, ForwardCompatibilitySubstitutionRule::Disable);
        CHECK(c3.option<ConfigOptionStrings>("filament_spoolman_link")->values == std::vector<std::string>{v});

        // preset JSON
        const std::string js = (dir.path() / "p.json").string();
        cfg.save_to_json(js, "name", "user", "1.0.0.0");
        DynamicPrintConfig c4 = DynamicPrintConfig::full_print_config();
        std::map<std::string, std::string> kv;
        std::string reason;
        c4.load_from_json(js, ForwardCompatibilitySubstitutionRule::Disable, kv, reason);
        CHECK(reason.empty());
        CHECK(c4.option<ConfigOptionStrings>("filament_spoolman_link")->values == std::vector<std::string>{v});
    }
}

namespace {

bool store_and_load_3mf(const DynamicPrintConfig &source, DynamicPrintConfig &reloaded, ConfigSubstitutionContext &subst)
{
    ScopedSlic3rTemporaryDir temporary_dir("orca_spoolman_3mf");
    Model                    model;
    ModelObject             *object = model.add_object();
    object->add_volume(make_cube(10.0, 10.0, 10.0));
    object->add_instance();
    model.set_backup_path((temporary_dir.path() / "backup").string());

    const std::string path = (temporary_dir.path() / "link.3mf").string();
    auto              plate = std::make_unique<PlateData>();
    plate->plate_index      = 0;
    StoreParams sp;
    sp.path   = path.c_str();
    sp.model  = &model;
    sp.config = const_cast<DynamicPrintConfig *>(&source);
    sp.plate_data_list.push_back(plate.get());
    sp.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
    if (!store_bbs_3mf(sp))
        return false;

    Model                  reloaded_model;
    PlateDataPtrs          plates;
    std::vector<Preset *>  project_presets;
    bool                   is_bbl = false, is_orca = false;
    Semver                 ver;
    const bool ok = load_bbs_3mf(path.c_str(), &reloaded, &subst, &reloaded_model, &plates, &project_presets, &is_bbl, &is_orca, &ver, nullptr,
                                 LoadStrategy::LoadModel | LoadStrategy::LoadConfig);
    release_PlateData_list(plates);
    return ok;
}

} // namespace

TEST_CASE("filament_spoolman_link survives a project 3MF round trip", "[Spoolman][3mf]")
{
    for (const std::string &v : {serialize(sample_link()), max_size_link_text()}) {
        DynamicPrintConfig source;
        source.set_key_value("filament_spoolman_link", new ConfigOptionStrings{v, ""});
        DynamicPrintConfig         reloaded;
        ConfigSubstitutionContext  subst{ForwardCompatibilitySubstitutionRule::Disable};
        REQUIRE(store_and_load_3mf(source, reloaded, subst));
        const auto *opt = reloaded.option<ConfigOptionStrings>("filament_spoolman_link");
        REQUIRE(opt != nullptr);
        CHECK(opt->values == (std::vector<std::string>{v, ""}));
        CHECK(subst.empty());
    }
}

TEST_CASE("A project saved without filament_spoolman_link loads with the empty default and no warning", "[Spoolman][3mf]")
{
    DynamicPrintConfig source; // an "old" project: no such key
    source.set_deserialize_strict("filament_density", "1.24");
    DynamicPrintConfig         reloaded = DynamicPrintConfig::full_print_config();
    ConfigSubstitutionContext  subst{ForwardCompatibilitySubstitutionRule::Disable};
    REQUIRE(store_and_load_3mf(source, reloaded, subst));
    const auto *opt = reloaded.option<ConfigOptionStrings>("filament_spoolman_link");
    REQUIRE(opt != nullptr);
    CHECK(opt->values == std::vector<std::string>{""});
    CHECK(subst.empty());
    CHECK(subst.unrecogized_keys.empty());
}
