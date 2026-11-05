#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "slic3r/Utils/SpoolmanJoin.hpp"

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
