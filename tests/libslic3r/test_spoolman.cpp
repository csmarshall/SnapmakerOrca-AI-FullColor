#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "libslic3r/Spoolman/SpoolmanCatalog.hpp"

#include <fstream>
#include <cstdint>
#include <iterator>
#include <set>
#include <string>

using json = nlohmann::json;
using namespace Slic3r::Spoolman;

static json load_spoolman_fixture(const std::string &name)
{
    std::ifstream in(std::string(TEST_DATA_DIR) + "/spoolman/" + name);
    REQUIRE(in.is_open());
    return json::parse(in);
}

// Smoke test: the captured (comment-stripped) Spoolman responses are present and shaped as the sync code will expect.
TEST_CASE("Spoolman fixtures load", "[Spoolman]")
{
    const json filaments = load_spoolman_fixture("filament.json");
    REQUIRE(filaments.contains("result"));
    const json &list = filaments["result"];
    REQUIRE(list.is_array());
    CHECK(list.size() == 22);

    bool found_16 = false;
    for (const json &f : list)
        if (f.at("id").get<int>() == 16)
            found_16 = true;
    CHECK(found_16);

    const json vendors = load_spoolman_fixture("vendor.json");
    REQUIRE(vendors.contains("result"));
    CHECK_FALSE(vendors["result"].empty());

    // The other captures must at least parse and carry the same envelope.
    for (const char *name : {"spool.json", "field_spool.json", "field_filament.json"})
        CHECK(load_spoolman_fixture(name).contains("result"));
}

static std::string load_spoolman_text(const std::string &name)
{
    std::ifstream in(std::string(TEST_DATA_DIR) + "/spoolman/" + name);
    REQUIRE(in.is_open());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static const Filament &filament_by_id(const std::vector<Filament> &v, int id)
{
    for (const Filament &f : v)
        if (f.id == id)
            return f;
    FAIL("filament id not found: " << id);
    return v.front();
}

static const Spool &spool_by_id(const std::vector<Spool> &v, int id)
{
    for (const Spool &s : v)
        if (s.id == id)
            return s;
    FAIL("spool id not found: " << id);
    return v.front();
}

TEST_CASE("Spoolman catalog: fixture filaments", "[Spoolman]")
{
    auto r = parse_filaments_text(load_spoolman_text("filament.json"));
    REQUIRE(r.status == ParseStatus::Ok);
    REQUIRE(r.items.size() == 22);

    // (a) known-bad: missing settings_extruder_temp is nullopt, never 0. The fixture has 9 such filaments.
    size_t no_temp = 0, no_colour = 0;
    for (const Filament &f : r.items) {
        if (!f.settings_extruder_temp) ++no_temp;
        else CHECK(*f.settings_extruder_temp > 0);
        if (!f.color_hex) ++no_colour;
    }
    CHECK(no_temp == 9);
    CHECK(no_colour == 4);

    const Filament &f16 = filament_by_id(r.items, 16);
    CHECK(f16.name == "PETG HF Red");
    REQUIRE(f16.vendor);
    CHECK(f16.vendor->id == 7);
    CHECK(f16.vendor->name == "Snapmaker");
    CHECK(f16.material == "PETG");
    CHECK(*f16.color_hex == "FF1523");
    CHECK(*f16.density == 1.3);
    CHECK(*f16.diameter == 1.75);
    CHECK(*f16.weight == 1000.0);
    CHECK(*f16.settings_extruder_temp == 250);
    CHECK(*f16.settings_bed_temp == 70);
    CHECK(*f16.external_id == "ofd_25925a9b-47ed-4192-bee6-0bcb8e4bd35c");
    CHECK_FALSE(f16.price);
    CHECK(f16.extras.empty());
}

TEST_CASE("Spoolman catalog: duplicate vendors are preserved", "[Spoolman]")
{
    // (g) same name, different ids (Snapmaker 7/9, Overture 5/8, Polymaker 2/10): never merged.
    auto r = parse_vendors_text(load_spoolman_text("vendor.json"));
    REQUIRE(r.status == ParseStatus::Ok);
    auto ids_named = [&](const std::string &n) {
        std::set<int> ids;
        for (const Vendor &v : r.items)
            if (v.name == n) ids.insert(v.id);
        return ids;
    };
    CHECK(ids_named("Snapmaker") == std::set<int>{7, 9});
    CHECK(ids_named("Overture") == std::set<int>{5, 8});
    CHECK(ids_named("Polymaker") == std::set<int>{2, 10});
    // every fixture vendor survives
    CHECK(r.items.size() == load_spoolman_fixture("vendor.json")["result"].size());

    // inline
    auto d = parse_vendors_text(R"([{"id":1,"name":"X"},{"id":2,"name":"X"}])");
    REQUIRE(d.status == ParseStatus::Ok);
    CHECK(d.items.size() == 2);
}

TEST_CASE("Spoolman catalog: fixture spools", "[Spoolman]")
{
    auto r = parse_spools_text(load_spoolman_text("spool.json"));
    REQUIRE(r.status == ParseStatus::Ok);
    REQUIRE(r.items.size() == 30);

    // (h)
    const Spool &s26 = spool_by_id(r.items, 26);
    REQUIRE(s26.card_uids.size() == 2);
    CHECK(s26.card_uids[0] == "040d005a5a5a5a");
    CHECK(s26.card_uids[1] == "040e005a5a5a5a");
    CHECK(s26.filament_id == 20);
    CHECK(s26.remaining_weight == 0.0);
    CHECK_FALSE(s26.archived);
    CHECK(spool_by_id(r.items, 14).card_uids.empty());
    CHECK(spool_by_id(r.items, 27).card_uids.empty());
    const Spool &s23 = spool_by_id(r.items, 23);
    REQUIRE(s23.tag_uids.size() == 2);
    CHECK(s23.tag_uids[0] == "040A005A5A5A5A");
    CHECK(s23.tag_uids[1] == "0401005A5A5A5A");
    CHECK(s23.card_uids.size() == 1);

    size_t with_cards = 0;
    for (const Spool &s : r.items)
        if (!s.card_uids.empty()) ++with_cards;
    CHECK(with_cards == 14); // 30 spools: 14 have no card_uids key, 2 have "" (14, 27), 14 have uids
}

TEST_CASE("Spoolman catalog: extras are JSON-decoded", "[Spoolman]")
{
    // (b) the stored text "\"\"" is a JSON string; the decoded value is empty, not two quote characters.
    auto r = parse_filaments_text(R"({"result":[{"id":1,"extra":{"variant":"\"\"","n":"3","s":"\"hi\"","bad":"not json {"}}]})");
    REQUIRE(r.status == ParseStatus::Ok);
    const Filament &f = r.items[0];
    REQUIRE(f.extras.count("variant") == 1);
    CHECK(f.extras.at("variant").empty());
    CHECK(f.extras.at("variant") != "\"\"");
    CHECK(f.extras.at("s") == "hi");
    CHECK(f.extras.at("n") == "3"); // valid JSON but not a string: raw kept
    CHECK(f.extras.at("bad") == "not json {");
    CHECK(f.extras_undecoded == std::set<std::string>{"bad", "n"});

    // spool fixture: card_uids "\"\"" decoded to empty text, not a 2-character uid
    auto sp = parse_spools_text(load_spoolman_text("spool.json"));
    CHECK(spool_by_id(sp.items, 14).extras.at("card_uids").empty());
    CHECK(spool_by_id(sp.items, 14).extras_undecoded.empty());
}

TEST_CASE("Spoolman catalog: malformed, empty and envelope handling", "[Spoolman]")
{
    // (c)
    for (const char *bad : {"not json", "", "42", "\"str\"", "null", R"({"foo":[]})", R"({"result":5})", R"({"result":[1]})", R"({"result":[{"name":"no id"}]})"}) {
        INFO(bad);
        auto r = parse_filaments_text(bad);
        CHECK(r.status == ParseStatus::Malformed);
        CHECK_FALSE(r.reason.empty());
        CHECK(r.items.empty());
        CHECK(parse_spools_text(bad).status == ParseStatus::Malformed);
        CHECK(parse_vendors_text(bad).status == ParseStatus::Malformed);
    }
    // (d)
    for (const char *empty : {R"({"result":[]})", "[]"}) {
        auto r = parse_filaments_text(empty);
        CHECK(r.status == ParseStatus::Empty);
        CHECK(r.reason.empty());
        CHECK(parse_spools_text(empty).status == ParseStatus::Empty);
        CHECK(parse_vendors_text(empty).status == ParseStatus::Empty);
    }
    // an HTTP failure is a distinct status the transport can construct
    auto h = ParseResult<Filament>::http_failure(503, "service unavailable");
    CHECK(h.status == ParseStatus::HttpFailure);
    CHECK(h.http_status == 503);
    CHECK(h.items.empty());
    CHECK(h.status != ParseStatus::Empty);
    CHECK_FALSE(h.ok());
}

TEST_CASE("Spoolman catalog: bare array equals envelope", "[Spoolman]")
{
    // (e)
    const std::string env = load_spoolman_text("filament.json");
    const json        bare = json::parse(env)["result"];
    auto a = parse_filaments_text(env);
    auto b = parse_filaments(bare);
    REQUIRE(a.status == ParseStatus::Ok);
    REQUIRE(b.status == ParseStatus::Ok);
    REQUIRE(a.items.size() == b.items.size());
    for (size_t i = 0; i < a.items.size(); ++i) {
        CHECK(a.items[i].id == b.items[i].id);
        CHECK(a.items[i].name == b.items[i].name);
        CHECK(a.items[i].settings_extruder_temp == b.items[i].settings_extruder_temp);
        CHECK(a.items[i].vendor.has_value() == b.items[i].vendor.has_value());
    }
    auto sa = parse_spools_text(load_spoolman_text("spool.json"));
    auto sb = parse_spools(json::parse(load_spoolman_text("spool.json"))["result"]);
    CHECK(sa.items.size() == sb.items.size());
}

TEST_CASE("Spoolman catalog: hand-written filament snippets", "[Spoolman]")
{
    // (f) vendor missing, null, or not an object; (a) temp absent vs null vs present, on inline data
    auto r = parse_filaments_text(R"([
        {"id":1,"name":"a","material":"PLA"},
        {"id":2,"name":"b","vendor":null,"settings_extruder_temp":null,"settings_bed_temp":0},
        {"id":3,"name":"c","vendor":{"id":4,"name":"V"},"settings_extruder_temp":215.5,"price":12.5,"color_hex":"00FF00"}
    ])");
    REQUIRE(r.status == ParseStatus::Ok);
    REQUIRE(r.items.size() == 3);
    CHECK_FALSE(r.items[0].vendor);
    CHECK_FALSE(r.items[0].settings_extruder_temp);
    CHECK_FALSE(r.items[0].color_hex);
    CHECK(r.items[0].material == "PLA");
    CHECK_FALSE(r.items[1].vendor);
    CHECK_FALSE(r.items[1].settings_extruder_temp);
    REQUIRE(r.items[1].settings_bed_temp); // an explicit 0 is kept as given
    CHECK(*r.items[1].settings_bed_temp == 0);
    REQUIRE(r.items[2].vendor);
    CHECK(r.items[2].vendor->id == 4);
    CHECK(*r.items[2].settings_extruder_temp == 215.5);
    CHECK(*r.items[2].price == 12.5);
    CHECK(*r.items[2].color_hex == "00FF00");
}

TEST_CASE("Spoolman catalog: hand-written spool snippets and uid helpers", "[Spoolman]")
{
    auto r = parse_spools_text(R"({"result":[
        {"id":5,"filament":{"id":9},"remaining_weight":12.5,"archived":true,
         "extra":{"card_uids":"\" AbC , ,def,\""},"tags":[{"uid":"AA11"},{"uid":""},{"uid":"bb22"}]},
        {"id":6,"extra":{},"tags":[]},
        {"id":7,"remaining_weight":null,"filament":null}
    ]})");
    REQUIRE(r.status == ParseStatus::Ok);
    REQUIRE(r.items.size() == 3);
    const Spool &a = r.items[0];
    CHECK(a.filament_id == 9);
    CHECK(*a.remaining_weight == 12.5);
    CHECK(a.archived);
    CHECK(a.card_uids == std::vector<std::string>{"AbC", "def"}); // trimmed, empties dropped, case kept
    CHECK(a.tag_uids == std::vector<std::string>{"AA11", "bb22"});
    CHECK(r.items[1].card_uids.empty());
    CHECK_FALSE(r.items[1].filament_id);
    CHECK_FALSE(r.items[2].remaining_weight); // null is not 0
    CHECK_FALSE(r.items[2].filament_id);

    CHECK(split_uid_list("").empty());
    CHECK(split_uid_list(" , ,").empty());
    CHECK(split_uid_list("a,b").size() == 2);
    CHECK(uid_equal("040D005A", "040d005a"));
    CHECK_FALSE(uid_equal("040d", "040e"));
    CHECK_FALSE(uid_equal("040d", "040d0"));
}

static bool has_warning(const std::vector<std::string> &w, const std::string &field)
{
    for (const std::string &s : w)
        if (s.find(field) != std::string::npos)
            return true;
    return false;
}

TEST_CASE("Spoolman catalog: ids must be integers in 1..INT_MAX", "[Spoolman]")
{
    for (const char *id : {"0", "-1", "2147483648", "4294967297", "18446744073709551615", "1.5", "\"7\"", "null", "true", "1e3"}) {
        INFO("id = " << id);
        const std::string body = std::string("[{\"id\":1},{\"id\":") + id + "}]";
        auto f = parse_filaments_text(body);
        CHECK(f.status == ParseStatus::Malformed);
        CHECK(f.items.empty());
        CHECK(f.reason.find("item 1") != std::string::npos);
        CHECK(f.reason.find("filament") != std::string::npos);
        auto s = parse_spools_text(body);
        CHECK(s.status == ParseStatus::Malformed);
        CHECK(s.reason.find("item 1") != std::string::npos);
        auto v = parse_vendors_text(body);
        CHECK(v.status == ParseStatus::Malformed);
        CHECK(v.reason.find("item 1") != std::string::npos);
        CHECK(v.reason.find("filament") == std::string::npos);
    }
    // the boundaries themselves are fine
    auto ok = parse_vendors_text(R"([{"id":1},{"id":2147483647}])");
    REQUIRE(ok.status == ParseStatus::Ok);
    CHECK(ok.items[1].id == 2147483647);

    // spool's embedded filament id: bad value => nullopt plus a warning, spool still parses
    for (const char *id : {"0", "-3", "2147483648", "18446744073709551615", "2.5", "\"9\""}) {
        INFO("filament.id = " << id);
        auto r = parse_spools_text(std::string("[{\"id\":1,\"filament\":{\"id\":") + id + "}}]");
        REQUIRE(r.status == ParseStatus::Ok);
        CHECK_FALSE(r.items[0].filament_id);
        CHECK(has_warning(r.items[0].warnings, "filament"));
    }
    auto nf = parse_spools_text(R"([{"id":1,"filament":"x"}])");
    REQUIRE(nf.status == ParseStatus::Ok);
    CHECK_FALSE(nf.items[0].filament_id);
    CHECK(has_warning(nf.items[0].warnings, "filament"));
}

TEST_CASE("Spoolman catalog: extras safety", "[Spoolman]")
{
    // deep nesting must not crash and is not parsed
    const std::string deep(5000, '[');
    json body = json::array({json{{"id", 1}, {"extra", json{{"deep", deep}}}}});
    auto r = parse_filaments(body);
    REQUIRE(r.status == ParseStatus::Ok);
    CHECK(r.items[0].extras_undecoded.count("deep") == 1);
    CHECK(r.items[0].extras.at("deep") == deep);

    // oversize raw text is not parsed
    const std::string big = "\"" + std::string(5000, 'a') + "\"";
    auto b = parse_filaments(json::array({json{{"id", 1}, {"extra", json{{"big", big}}}}}));
    CHECK(b.items[0].extras_undecoded.count("big") == 1);
    CHECK(b.items[0].extras.at("big") == big);

    // decodes to something that is not a string: raw text kept and flagged
    for (const char *raw : {"1E5", "123456789012345678901", "null", "true", "3", "[\"a\",\"b\"]", "{\"k\":1}"}) {
        INFO(raw);
        auto e = parse_filaments(json::array({json{{"id", 1}, {"extra", json{{"k", raw}}}}}));
        REQUIRE(e.status == ParseStatus::Ok);
        CHECK(e.items[0].extras.at("k") == raw);
        CHECK(e.items[0].extras_undecoded.count("k") == 1);
    }

    // a card_uids extra holding a JSON array yields no uids, and is flagged
    auto s = parse_spools_text(R"([{"id":1,"extra":{"card_uids":"[\"a\",\"b\"]"}}])");
    REQUIRE(s.status == ParseStatus::Ok);
    CHECK(s.items[0].card_uids.empty());
    CHECK(s.items[0].extras_undecoded.count("card_uids") == 1);

    // a well-formed string at modest depth still decodes
    auto ok = parse_filaments_text(R"([{"id":1,"extra":{"v":"\"[[x]]\""}}])");
    CHECK(ok.items[0].extras.at("v") == "[[x]]");
    CHECK(ok.items[0].extras_undecoded.empty());

    // extra that is not an object: ignored, with a warning
    auto n = parse_filaments_text(R"([{"id":1,"extra":[1,2]},{"id":2,"extra":null}])");
    REQUIRE(n.status == ParseStatus::Ok);
    CHECK(n.items[0].extras.empty());
    CHECK(has_warning(n.items[0].warnings, "extra"));
    CHECK(n.items[1].warnings.empty());
}

TEST_CASE("Spoolman catalog: wrong-typed fields give nullopt and one warning", "[Spoolman]")
{
    auto r = parse_filaments_text(R"([{"id":1,"name":5,"density":"1.24","settings_extruder_temp":"250","settings_bed_temp":[],
        "color_hex":123,"diameter":true,"weight":"x","spool_weight":{},"price":"9","external_id":4,"material":7,
        "vendor":"Acme"},
        {"id":2,"vendor":{"name":"NoId"}},
        {"id":3,"vendor":{"id":4,"name":9}},
        {"id":4,"density":null,"color_hex":null}])");
    REQUIRE(r.status == ParseStatus::Ok);
    const Filament &f = r.items[0];
    CHECK_FALSE(f.density);
    CHECK_FALSE(f.settings_extruder_temp);
    CHECK_FALSE(f.settings_bed_temp);
    CHECK_FALSE(f.color_hex);
    CHECK_FALSE(f.diameter);
    CHECK_FALSE(f.weight);
    CHECK_FALSE(f.spool_weight);
    CHECK_FALSE(f.price);
    CHECK_FALSE(f.external_id);
    CHECK_FALSE(f.vendor);
    for (const char *k : {"name", "density", "settings_extruder_temp", "settings_bed_temp", "color_hex", "diameter", "weight", "spool_weight",
                          "price", "external_id", "material", "vendor"})
        CHECK(has_warning(f.warnings, k));
    CHECK(f.warnings.size() == 12);

    CHECK_FALSE(r.items[1].vendor);
    CHECK(has_warning(r.items[1].warnings, "vendor"));
    // vendor.name non-string: vendor kept with empty name, and a warning on the vendor
    REQUIRE(r.items[2].vendor);
    CHECK(r.items[2].vendor->name.empty());
    CHECK(has_warning(r.items[2].vendor->warnings, "name"));
    // null and absent are silent
    CHECK(r.items[3].warnings.empty());

    auto v = parse_vendors_text(R"([{"id":1,"name":5}])");
    REQUIRE(v.status == ParseStatus::Ok);
    CHECK(has_warning(v.items[0].warnings, "name"));

    auto s = parse_spools_text(R"([{"id":1,"remaining_weight":"5","archived":"yes","tags":[1,{"uid":5},{"nouid":1}],"extra":{"card_uids":"\"a\""}},
        {"id":2,"archived":null,"remaining_weight":null,"tags":null}])");
    REQUIRE(s.status == ParseStatus::Ok);
    const Spool &sp = s.items[0];
    CHECK_FALSE(sp.remaining_weight);
    CHECK_FALSE(sp.archived);
    CHECK(has_warning(sp.warnings, "remaining_weight"));
    CHECK(has_warning(sp.warnings, "archived"));
    CHECK(has_warning(sp.warnings, "tags"));
    CHECK(sp.tag_uids.empty());
    CHECK(sp.card_uids == std::vector<std::string>{"a"});
    CHECK(s.items[1].warnings.empty());

    auto t = parse_spools_text(R"([{"id":1,"tags":"x"}])");
    CHECK(has_warning(t.items[0].warnings, "tags"));
}

TEST_CASE("Spoolman catalog: tag uids are trimmed; values pass through unvalidated", "[Spoolman]")
{
    auto r = parse_spools_text(R"([{"id":1,"tags":[{"uid":"  AB12 \t"},{"uid":"   "}]}])");
    REQUIRE(r.status == ParseStatus::Ok);
    CHECK(r.items[0].tag_uids == std::vector<std::string>{"AB12"});

    // no semantic validation here (S2 does that): negative density, '#' colour and duplicate ids pass
    auto f = parse_filaments_text(R"([{"id":1,"density":-1,"color_hex":"#FF0000"},{"id":1}])");
    REQUIRE(f.status == ParseStatus::Ok);
    CHECK(*f.items[0].density == -1);
    CHECK(*f.items[0].color_hex == "#FF0000");
    CHECK(f.items.size() == 2);
    CHECK(f.items[0].warnings.empty());
}

static std::string nested(int n, const std::string &leaf = "1") { return std::string(n, '[') + leaf + std::string(n, ']'); }

TEST_CASE("Spoolman catalog: max_nesting is string-aware", "[Spoolman]")
{
    CHECK(max_nesting("") == 0);
    CHECK(max_nesting(nested(8)) == 8);
    CHECK(max_nesting(nested(9)) == 9);
    CHECK(max_nesting(std::string(20, '[') + std::string(20, ']')) == 20);
    CHECK(max_nesting(R"({"a":[1,{"b":2}]})") == 3);
    // brackets inside a quoted string do not count, either direction
    CHECK(max_nesting("\"" + std::string(50, '[') + "\"") == 0);
    CHECK(max_nesting(R"(["a]b","c}d"])") == 1);
    CHECK(max_nesting(R"("esc \" [[[[ still in string")") == 0);
    CHECK(max_nesting(R"("ends with backslash pair \\" [[)") == 2);
    // the bypass: ']' inside strings must not decrement the real depth
    std::string bypass;
    for (int i = 0; i < 12; ++i) bypass += "[\"]\",";
    CHECK(max_nesting(bypass) == 12);
    CHECK(max_nesting("[\"unterminated [[[[") == 1);
}

TEST_CASE("Spoolman catalog: extras size and depth caps", "[Spoolman]")
{
    auto one = [](const std::string &raw) {
        auto r = parse_filaments(json::array({json{{"id", 1}, {"extra", json{{"k", raw}}}}}));
        REQUIRE(r.status == ParseStatus::Ok);
        return r.items[0];
    };
    // byte boundary: 4096 raw bytes decodes, 4097 does not
    const std::string at_cap = "\"" + std::string(4094, 'a') + "\"";
    REQUIRE(at_cap.size() == 4096);
    Filament f = one(at_cap);
    CHECK(f.extras_undecoded.empty());
    CHECK(f.extras.at("k") == std::string(4094, 'a'));
    const std::string over = "\"" + std::string(4095, 'a') + "\"";
    f = one(over);
    CHECK(f.extras_undecoded.count("k") == 1);
    CHECK(f.extras.at("k") == over);

    // small but deep (40 bytes): kept raw and flagged
    const std::string deep = std::string(20, '[') + std::string(20, ']');
    f = one(deep);
    CHECK(f.extras_undecoded.count("k") == 1);
    CHECK(f.extras.at("k") == deep);

    // depth 8 vs 9 are both non-strings, so both are flagged and raw; the cap only guards the parser (see max_nesting tests)
    CHECK(one(nested(8)).extras.at("k") == nested(8));
    CHECK(one(nested(9)).extras.at("k") == nested(9));

    // bypass attempt with ']' inside strings: no crash, flagged, raw kept
    std::string bypass;
    for (int i = 0; i < 3000; ++i) bypass += "[\"]\",";
    f = one(bypass);
    CHECK(f.extras_undecoded.count("k") == 1);
    CHECK(f.extras.at("k") == bypass);

    // a legitimate decoded string full of '[' is depth 0 by definition and decodes
    const std::string bracket_string = "\"" + std::string(50, '[') + "\"";
    f = one(bracket_string);
    CHECK(f.extras_undecoded.empty());
    CHECK(f.extras.at("k") == std::string(50, '['));
}

TEST_CASE("Spoolman catalog: real non-string extra values", "[Spoolman]")
{
    auto r = parse_spools(json::parse(R"([{"id":1,"extra":{"card_uids":["a","b"],"n":5,"o":{"x":1}}}])"));
    REQUIRE(r.status == ParseStatus::Ok);
    const Spool &s = r.items[0];
    CHECK(s.card_uids.empty());
    CHECK(s.extras_undecoded == std::set<std::string>{"card_uids", "n", "o"});
    CHECK(s.extras.at("card_uids") == "[\"a\",\"b\"]");
    CHECK(s.extras.at("n") == "5");
    CHECK(s.extras.at("o") == "{\"x\":1}");
}

TEST_CASE("Spoolman catalog: signed int64 ids", "[Spoolman]")
{
    for (std::int64_t id : {std::int64_t(4294967297), std::int64_t(-5), std::int64_t(2147483647) + 1, std::int64_t(0)}) {
        INFO("id = " << id);
        json item;
        item["id"] = id; // int64 held as signed
        REQUIRE(item["id"].is_number_integer());
        const json body = json::array({item});
        CHECK(parse_filaments(body).status == ParseStatus::Malformed);
        CHECK(parse_spools(body).status == ParseStatus::Malformed);
        CHECK(parse_vendors(body).status == ParseStatus::Malformed);
    }
    json ok;
    ok["id"] = std::int64_t(2147483647);
    CHECK(parse_vendors(json::array({ok})).status == ParseStatus::Ok);
}

TEST_CASE("Spoolman catalog: null vendor is silent", "[Spoolman]")
{
    auto r = parse_filaments_text(R"([{"id":1,"vendor":null}])");
    REQUIRE(r.status == ParseStatus::Ok);
    CHECK_FALSE(r.items[0].vendor);
    CHECK(r.items[0].warnings.empty());
}

static const char *const kTooLarge = "<structured value too large>";

TEST_CASE("Spoolman catalog: real structured extras over the cap get a placeholder (small)", "[Spoolman]")
{
    // depth 9 (just over kMaxExtraDepth), and 5000 elements (over the node cap)
    json deep9 = 0;
    for (int i = 0; i < 9; ++i) { json a = json::array(); a.push_back(std::move(deep9)); deep9 = std::move(a); }
    json wide = json::array();
    for (int i = 0; i < 5000; ++i) wide.push_back(i);
    json ok8 = 0;
    for (int i = 0; i < 8; ++i) { json a = json::array(); a.push_back(std::move(ok8)); ok8 = std::move(a); }

    json item = {{"id", 1}};
    item["extra"]["deep9"] = std::move(deep9);
    item["extra"]["wide"]  = std::move(wide);
    item["extra"]["ok8"]   = std::move(ok8);
    json body = json::array();
    body.push_back(std::move(item));
    auto r = parse_filaments(body);
    REQUIRE(r.status == ParseStatus::Ok);
    const Filament &f = r.items[0];
    CHECK(f.extras.at("deep9") == kTooLarge);
    CHECK(f.extras.at("wide") == kTooLarge);
    CHECK(f.extras_undecoded.count("deep9") == 1);
    CHECK(f.extras_undecoded.count("wide") == 1);
    CHECK(has_warning(f.warnings, "deep9"));
    CHECK(has_warning(f.warnings, "wide"));
    // at the depth cap it is dumped as before
    CHECK(f.extras.at("ok8") == nested(8, "0"));
    CHECK(f.extras_undecoded.count("ok8") == 1);
}

TEST_CASE("Spoolman catalog: 100000-level real extra does not overflow the stack", "[Spoolman]")
{
    // Built iteratively with moves only (a copy would recurse). nlohmann 3.10's destructor is iterative (checked in deps_src/nlohmann/json.hpp).
    auto make_deep = [] {
        json v = 0;
        for (int i = 0; i < 100000; ++i) { json a = json::array(); a.push_back(std::move(v)); v = std::move(a); }
        return v;
    };
    {
        json item = {{"id", 1}};
        item["extra"]["deep"] = make_deep();
        json body = json::array();
        body.push_back(std::move(item));
        auto r = parse_filaments(body);
        REQUIRE(r.status == ParseStatus::Ok);
        CHECK(r.items[0].extras.at("deep") == kTooLarge);
        CHECK(r.items[0].extras_undecoded.count("deep") == 1);
        CHECK(has_warning(r.items[0].warnings, "deep"));
    }
    {
        json item = {{"id", 1}};
        item["extra"]["deep"] = make_deep();
        json body = json::array();
        body.push_back(std::move(item));
        auto r = parse_spools(body);
        REQUIRE(r.status == ParseStatus::Ok);
        CHECK(r.items[0].extras.at("deep") == kTooLarge);
        CHECK(r.items[0].extras_undecoded.count("deep") == 1);
        CHECK(has_warning(r.items[0].warnings, "deep"));
    }
}

TEST_CASE("Spoolman catalog: 2000-level nested response text", "[Spoolman]")
{
    const std::string body = "[{\"id\":1,\"extra\":{\"k\":" + nested(2000) + "}}]";
    auto r = parse_filaments_text(body);
    REQUIRE(r.status == ParseStatus::Ok);
    CHECK(r.items[0].extras.at("k") == kTooLarge);
    CHECK(r.items[0].extras_undecoded.count("k") == 1);
}
