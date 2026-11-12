#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "slic3r/Utils/SpoolmanJoin.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

namespace {
// Shaped like the real U1 proxy reply: card_uids is a JSON-encoded string.
const char* k_body = R"({"result":[
  {"id":15,"archived":false,"remaining_weight":812.5,"used_weight":187.5,"initial_weight":1000,
   "extra":{"card_uids":"\"04732598CA2A81\""},
   "filament":{"name":"Silk Blue","material":"PLA","color_hex":"1E90FF","vendor":{"name":"Acme"},
               "settings_extruder_temp":215,"settings_bed_temp":60,"density":1.24,"diameter":1.75}},
  {"id":16,"archived":true,"extra":{"card_uids":"\"aabbccdd\""},"filament":{"name":"Old"}},
  {"id":17,"archived":false,"remaining_weight":null,"extra":{},"filament":{"name":"No tag"}},
  {"id":18,"archived":false,"extra":{"card_uids":"[\"01020304\",\"05060708\"]"},"filament":{"name":"Two"}}
]})";
} // namespace

TEST_CASE("Spoolman UID bytes become lowercase zero-padded hex", "[Spoolman]")
{
    CHECK(spoolman_uid_from_bytes(nlohmann::json::parse("[4,115,37,152,202,42,129]")) == "04732598ca2a81");
    CHECK(spoolman_uid_from_bytes(nlohmann::json::parse("[0,0,0,0]")).empty());
    CHECK(spoolman_uid_from_bytes(nlohmann::json::parse("[]")).empty());
    CHECK(spoolman_uid_from_bytes(nlohmann::json::parse("[1,300]")).empty());
    CHECK(spoolman_uid_from_bytes(nlohmann::json::parse("\"x\"")).empty());
}

TEST_CASE("Spoolman UID normalisation strips quotes and case", "[Spoolman]")
{
    CHECK(spoolman_normalize_uid("\"04AB\"") == "04ab");
    CHECK(spoolman_normalize_uid(" 04ab ") == "04ab");
}

TEST_CASE("Spoolman spool list is parsed and keyed by card UID", "[Spoolman]")
{
    auto m = spoolman_parse_spools(k_body);
    REQUIRE(m.count("04732598ca2a81") == 1);
    const auto& s = m.at("04732598ca2a81");
    CHECK(s.spool_id == 15);
    CHECK(s.name == "Silk Blue");
    CHECK(s.vendor == "Acme");
    CHECK(s.color_hex == "1E90FF");
    CHECK(s.remaining_weight_g == 812.5);
    CHECK(s.nozzle_temp == 215);
    CHECK(s.bed_temp == 60);
    CHECK(s.density == 1.24);
    CHECK(s.diameter == 1.75);
    CHECK(m.count("aabbccdd") == 0); // archived
    CHECK(m.count("01020304") == 1); // list-valued card_uids
    CHECK(m.count("05060708") == 1);
    CHECK(m.size() == 3);
}

TEST_CASE("Spoolman parse fails soft on bad input", "[Spoolman]")
{
    CHECK(spoolman_parse_spools("").empty());
    CHECK(spoolman_parse_spools("not json").empty());
    CHECK(spoolman_parse_spools("{\"error\":{}}").empty());
    CHECK(spoolman_parse_spools("{\"result\":5}").empty());
    CHECK(spoolman_parse_spools("[1,null,{\"id\":\"x\"}]").empty());
}

TEST_CASE("Spoolman join leaves unmatched slots empty", "[Spoolman]")
{
    auto by_uid = spoolman_parse_spools(k_body);
    auto detect = nlohmann::json::parse(R"([{"CARD_UID":[4,115,37,152,202,42,129]},{"CARD_UID":[0,0,0,0]},{},{"CARD_UID":[9,9,9,9]}])");
    auto joined = spoolman_join_slots(spoolman_slot_uids(detect), by_uid);
    REQUIRE(joined.size() == 4);
    CHECK(joined[0].spool_id == 15);
    CHECK_FALSE(joined[1].valid());
    CHECK_FALSE(joined[2].valid());
    CHECK_FALSE(joined[3].valid());
}

TEST_CASE("Spoolman JSON view uses distinct spoolman_* names for tool settings", "[Spoolman]")
{
    auto j = spoolman_to_json(spoolman_parse_spools(k_body).at("04732598ca2a81"));
    CHECK(j["spoolman_nozzle_temp"] == 215);
    CHECK(j["remaining_weight_g"] == 812.5);
    CHECK_FALSE(j.contains("nozzle_temp"));
    SpoolmanSpoolInfo none;
    CHECK(spoolman_to_json(none)["remaining_weight_g"].is_null());
}

// ---- S9: the join fills a tray's Spoolman keys (spoolman_filament_id / _external_id / _setting_id). SYNTHETIC UIDs only. ----
namespace {
const char* k_s9_body = R"({"result":[
  {"id":26,"archived":false,"extra":{"card_uids":"\"0a0b0c0d0e0f10\""},
   "filament":{"id":16,"name":"PLA Red","material":"PLA","external_id":"ofd-red","extra":{"setting_id":"\"GFL99\""}}},
  {"id":40,"archived":false,"extra":{"card_uids":"\"dd01dd02\""},"filament":{"id":21,"name":"A"}},
  {"id":41,"archived":false,"extra":{"card_uids":"\"dd01dd02\""},"filament":{"id":22,"name":"B"}}
]})";
} // namespace

TEST_CASE("Spoolman join carries the filament id, external id and setting id", "[Spoolman][S9]")
{
    const auto  m = spoolman_parse_spools(k_s9_body);
    const auto& s = m.at("0a0b0c0d0e0f10");
    CHECK(s.spool_id == 26);
    CHECK(s.filament_id == 16);
    CHECK(s.filament_external_id == "ofd-red");
    CHECK(s.filament_setting_id == "GFL99");
}

TEST_CASE("A card UID shared by two spools is ambiguous and joins nothing", "[Spoolman][S9]")
{
    auto by_uid = spoolman_parse_spools(k_s9_body);
    CHECK(by_uid.count("dd01dd02") == 0);
    auto joined = spoolman_join_slots({"dd01dd02"}, by_uid);
    CHECK_FALSE(joined[0].valid());
}

TEST_CASE("Tray keys are filled from the joined spool of a slot, not at all otherwise", "[Spoolman][S9]")
{
    auto by_uid = spoolman_parse_spools(k_s9_body);
    auto joined = spoolman_join_slots({"0a0b0c0d0e0f10", "", "ffff"}, by_uid);

    DynamicPrintConfig c;
    spoolman_fill_tray_config(c, joined[0]);
    REQUIRE(c.option<ConfigOptionInts>("spoolman_filament_id"));
    CHECK(c.option<ConfigOptionInts>("spoolman_filament_id")->values.front() == 16);
    CHECK(c.option<ConfigOptionStrings>("spoolman_external_id")->values.front() == "ofd-red");
    CHECK(c.option<ConfigOptionStrings>("spoolman_setting_id")->values.front() == "GFL99");

    DynamicPrintConfig none;
    spoolman_fill_tray_config(none, joined[1]);
    spoolman_fill_tray_config(none, joined[2]);
    CHECK(none.option("spoolman_filament_id") == nullptr); // falls through to upstream's path
}

TEST_CASE("The slot lookup reads the cache; an unreachable Spoolman (cache without a join) leaves the tray alone", "[Spoolman][S9]")
{
    auto by_uid = spoolman_parse_spools(k_s9_body);
    spoolman_cache_set("s9-dev", spoolman_join_slots({"", "0a0b0c0d0e0f10"}, by_uid));
    CHECK(spoolman_cache_slot("s9-dev", 1).filament_id == 16);
    CHECK_FALSE(spoolman_cache_slot("s9-dev", 0).valid());
    CHECK_FALSE(spoolman_cache_slot("s9-dev", 9).valid());
    CHECK_FALSE(spoolman_cache_slot("other", 1).valid());

    // fetch failed: the agent joins nothing (empty map), every slot is unjoined
    spoolman_cache_set("s9-dev", spoolman_join_slots({"0a0b0c0d0e0f10"}, {}));
    DynamicPrintConfig c;
    spoolman_fill_tray_config(c, spoolman_cache_slot("s9-dev", 0));
    CHECK(c.option("spoolman_filament_id") == nullptr);
}
