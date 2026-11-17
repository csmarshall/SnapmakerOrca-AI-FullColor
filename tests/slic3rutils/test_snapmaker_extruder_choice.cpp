#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "slic3r/Utils/SnapmakerExtruderChoice.hpp"

using namespace Slic3r;

namespace {
// Generated placeholder fixtures only.
SnapmakerUsedFilament uf(int i, const char* type, const char* colour, int spool = -1) { return {i, type, colour, spool}; }
SnapmakerSlotInfo     sl(const char* type, const char* colour, int spool = -1) { return {type, colour, spool}; }
const std::vector<SnapmakerSlotInfo> four_empty(4);
} // namespace

TEST_CASE("Suggestion prefers the slot with the same type", "[ExtruderChoice]")
{
    const auto s = suggest_snapmaker_extruder_map({uf(0, "PETG", "#FF0000"), uf(1, "PLA", "#FF0000")},
                                                  {sl("PLA", "#FF0000"), sl("ABS", "#FF0000"), sl("PETG", "#FF0000"), sl("TPU", "#FF0000")});
    CHECK(s.map == std::vector<int>({2, 0}));
    CHECK_FALSE(s.oversubscribed);
}

TEST_CASE("Suggestion breaks a type tie by closest colour", "[ExtruderChoice]")
{
    const auto s = suggest_snapmaker_extruder_map({uf(0, "PLA", "#0000FF")}, {sl("PLA", "#FF0000"), sl("PLA", "#0000F0"), sl("PLA", "#00FF00"), sl("PLA", "#FFFFFF")});
    CHECK(s.map == std::vector<int>({1}));
}

TEST_CASE("Suggestion falls back to the identity with no printer data", "[ExtruderChoice]")
{
    const auto s = suggest_snapmaker_extruder_map({uf(0, "PLA", "#112233"), uf(1, "PLA", "#112233"), uf(2, "PLA", "garbage")}, four_empty);
    CHECK(s.map == std::vector<int>({0, 1, 2}));
}

TEST_CASE("Suggestion is deterministic on ties and never doubles a slot", "[ExtruderChoice]")
{
    const std::vector<SnapmakerSlotInfo> same(4, sl("PLA", "#808080"));
    const auto s = suggest_snapmaker_extruder_map({uf(0, "PLA", "#808080"), uf(1, "PLA", "#808080"), uf(2, "PLA", "#808080")}, same);
    CHECK(s.map == std::vector<int>({0, 1, 2}));
    CHECK(suggest_snapmaker_extruder_map({uf(0, "PLA", "#808080"), uf(1, "PLA", "#808080"), uf(2, "PLA", "#808080")}, same).map == s.map);
    // identical filaments, one slot best: the filament whose identity slot it is (1) wins it, the other takes the next best
    const auto t = suggest_snapmaker_extruder_map({uf(0, "PLA", "#FF0000"), uf(1, "PLA", "#FF0000")},
                                                  {sl("ABS", ""), sl("PLA", "#FF0000"), sl("PLA", "#FE0000"), sl("ABS", "")});
    CHECK(t.map == std::vector<int>({2, 1}));
}

TEST_CASE("Suggestion with more used filaments than extruders reports it", "[ExtruderChoice]")
{
    std::vector<SnapmakerUsedFilament> used;
    for (int i = 0; i < 5; ++i)
        used.push_back(uf(i, "PLA", "#808080"));
    const auto s = suggest_snapmaker_extruder_map(used, four_empty);
    CHECK(s.oversubscribed);
    REQUIRE(s.map.size() == 5);
    for (int e : s.map)
        CHECK((e >= 0 && e < 4));
}

TEST_CASE("Suggestion for a single-filament, a high-index and an identity project", "[ExtruderChoice]")
{
    CHECK(suggest_snapmaker_extruder_map({uf(0, "PLA", "#FF0000")}, four_empty).map == std::vector<int>({0}));
    // sparse: only filament 2 used, nothing known -> its own slot, others -1
    CHECK(suggest_snapmaker_extruder_map({uf(2, "PLA", "#FF0000")}, four_empty).map == std::vector<int>({-1, -1, 2}));
    // already identity by data
    const auto s = suggest_snapmaker_extruder_map({uf(0, "PLA", "#FF0000"), uf(1, "ABS", "#00FF00")}, {sl("PLA", "#FF0000"), sl("ABS", "#00FF00"), sl("", ""), sl("", "")});
    CHECK(s.map == std::vector<int>({0, 1}));
    // Spoolman link beats type and colour
    CHECK(suggest_snapmaker_extruder_map({uf(0, "PLA", "#FF0000", 7)}, {sl("PLA", "#FF0000"), sl("ABS", "#000000", 7), sl("", ""), sl("", "")}).map == std::vector<int>({1}));
    CHECK(suggest_snapmaker_extruder_map({}, four_empty).map.empty());
}

TEST_CASE("Colour distance", "[ExtruderChoice]")
{
    CHECK(snapmaker_colour_distance("#FF0000", "#FF0000FF") == 0);
    CHECK(snapmaker_colour_distance("#000000", "#010203") == 14);
    CHECK(snapmaker_colour_distance("red", "#000000") == -1);
    CHECK(snapmaker_colour_distance("#GG0000", "#000000") == -1);
    CHECK(snapmaker_colour_distance("", "") == -1);
}

TEST_CASE("Validation refuses every bad map with a message", "[ExtruderChoice]")
{
    const std::vector<SnapmakerUsedFilament> two{uf(0, "PLA", "#FF0000"), uf(1, "PLA", "#00FF00")};
    auto refusal = [&](const std::vector<int>& m, const std::vector<SnapmakerUsedFilament>& u = {uf(0, "PLA", "#FF0000"), uf(1, "PLA", "#00FF00")}) {
        const auto v = validate_snapmaker_extruder_map(m, u, four_empty);
        if (!v.ok())
            CHECK_FALSE(v.message.empty());
        return v.refusal;
    };
    CHECK(refusal({}) == SnapmakerMapRefusal::Unchosen);
    CHECK(refusal({-1, -1}) == SnapmakerMapRefusal::Unchosen);
    CHECK(refusal({2}) == SnapmakerMapRefusal::Unmapped);      // filament 2 missing from the map
    CHECK(refusal({0, -1}) == SnapmakerMapRefusal::Unmapped);
    CHECK(refusal({0, 4}) == SnapmakerMapRefusal::OutOfRange);
    CHECK(refusal({0, 1}, {uf(-1, "PLA", ""), uf(1, "PLA", "")}) == SnapmakerMapRefusal::InvalidIndex);
    CHECK(refusal({0, 1}, {uf(0, "PLA", ""), uf(40, "PLA", "")}) == SnapmakerMapRefusal::InvalidIndex);
    CHECK(refusal({1, 1}) == SnapmakerMapRefusal::Duplicate);
    CHECK(refusal({0, 1}) == SnapmakerMapRefusal::None);
    CHECK(refusal({3, 0}) == SnapmakerMapRefusal::None);
    // a stale entry for an unused filament is ignored
    CHECK(validate_snapmaker_extruder_map({0, 9}, {uf(0, "PLA", "")}, four_empty).ok());
}

TEST_CASE("Validation over-subscription: sharing only where forced", "[ExtruderChoice]")
{
    std::vector<SnapmakerUsedFilament> five;
    for (int i = 0; i < 5; ++i)
        five.push_back(uf(i, "PLA", ""));
    const auto ok = validate_snapmaker_extruder_map({0, 1, 2, 3, 3}, five, four_empty);
    CHECK(ok.ok());
    CHECK(ok.oversubscribed);
    const auto wasteful = validate_snapmaker_extruder_map({0, 0, 1, 2, 2}, five, four_empty); // extruder 3 idle while sharing
    CHECK(wasteful.refusal == SnapmakerMapRefusal::Duplicate);
    CHECK_FALSE(validate_snapmaker_extruder_map({0, 1}, {uf(0, "PLA", ""), uf(1, "PLA", "")}, four_empty).oversubscribed);
}

TEST_CASE("An unchosen map never converts to ams_mapping", "[ExtruderChoice]")
{
    const std::vector<SnapmakerUsedFilament> u{uf(0, "PLA", ""), uf(1, "PLA", "")};
    for (const std::vector<int>& m : {std::vector<int>{}, std::vector<int>{-1, -1}, std::vector<int>{0, -1}, std::vector<int>{1, 1}}) {
        const auto r = snapmaker_map_to_ams_mapping(m, u, four_empty);
        CHECK_FALSE(r.ok());
        CHECK(r.json.empty());
        CHECK_FALSE(r.error.empty());
    }
    CHECK_FALSE(snapmaker_map_to_ams_mapping({0}, {}, four_empty).ok());
}

TEST_CASE("ams_mapping conversion has the agent's array shape; unused filaments are -1", "[ExtruderChoice]")
{
    const auto r = snapmaker_map_to_ams_mapping({2, 3, 1, 0}, {uf(0, "PLA", ""), uf(2, "PLA", ""), uf(3, "PLA", "")}, four_empty);
    REQUIRE(r.ok());
    CHECK(r.json == "[2,-1,1,0]");
    std::vector<int> parsed;
    REQUIRE(parse_snapmaker_ams_mapping(r.json, parsed));
    CHECK(parsed == std::vector<int>({2, -1, 1, 0}));
}

// Golden: the expected lines are the ones asserted in test_print_preference.cpp for {2,0,3}; those come from our reading of the
// page (main.b347974c91352c2f.js ~39855-39875, map built ~150065). The chain below repeats what MoonrakerPrinterAgent::print_uploaded_file does
// with params.ams_mapping (parse, used = value >= 0, build). It shows agreement with that reading and with our builder, not with the printer.
TEST_CASE("Resolver output round-trips to the golden extruder map lines", "[ExtruderChoice]")
{
    auto agent_script = [](const std::string& ams_mapping) {
        std::vector<int> m;
        REQUIRE(parse_snapmaker_ams_mapping(ams_mapping, m));
        std::vector<bool> used(m.size());
        for (size_t i = 0; i < m.size(); ++i)
            used[i] = m[i] >= 0;
        const auto r = build_snapmaker_extruder_map_script(m, used);
        REQUIRE(r.ok());
        return r.script;
    };
    const auto a = snapmaker_map_to_ams_mapping({2, 0, 3}, {uf(0, "PLA", ""), uf(1, "PLA", ""), uf(2, "PLA", "")}, four_empty);
    REQUIRE(a.ok());
    CHECK(agent_script(a.json) ==
          "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=0 MAP_EXTRUDER=2\n"
          "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=1 MAP_EXTRUDER=0\n"
          "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=2 MAP_EXTRUDER=3\n"
          "SET_PRINT_USED_EXTRUDERS EXTRUDERS=2,0,3");
    const std::vector<SnapmakerUsedFilament> used3{uf(0, "PLA", ""), uf(2, "PLA", ""), uf(3, "PLA", "")};
    // filaments 0 and 2 sharing extruder 1 while extruders are free: refused, not silently sent
    CHECK_FALSE(snapmaker_map_to_ams_mapping({1, -1, 1, 3}, used3, four_empty).ok());
    // identity suggestion (no printer data) also round-trips; unused filament 1 gets no line
    const auto s = suggest_snapmaker_extruder_map(used3, four_empty);
    const auto c = snapmaker_map_to_ams_mapping(s.map, used3, four_empty);
    REQUIRE(c.ok());
    CHECK(c.json == "[0,-1,2,3]");
    CHECK(agent_script(c.json) ==
          "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=0 MAP_EXTRUDER=0\n"
          "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=2 MAP_EXTRUDER=2\n"
          "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=3 MAP_EXTRUDER=3\n"
          "SET_PRINT_USED_EXTRUDERS EXTRUDERS=0,2,3");
}

// ---- Legacy send decision (#120 slice 2) ----
namespace {
SnapmakerLegacySendInput legacy_in()
{
    SnapmakerLegacySendInput in;
    in.snapmaker_printer = true;
    in.agent_present     = true;
    in.agent_connected   = true;
    in.start_requested   = true;
    in.map_confirmed     = true;
    in.used              = {uf(0, "PLA", "#FF0000"), uf(1, "PETG", "#00FF00")};
    in.slots             = four_empty;
    in.map               = {1, 0};
    return in;
}
} // namespace

TEST_CASE("Legacy send: connected, confirmed and valid starts through the agent", "[ExtruderChoice][LegacySend]")
{
    const auto d = decide_snapmaker_legacy_send(legacy_in());
    CHECK(d.kind == SnapmakerLegacySendKind::StartViaAgent);
    CHECK(d.ams_mapping == "[1,0]");
    CHECK_FALSE(d.oversubscribed);
    CHECK_FALSE(d.upload_only_notice);
}

TEST_CASE("Legacy send: unconfirmed or invalid map refuses the start, never identity", "[ExtruderChoice][LegacySend]")
{
    auto in          = legacy_in();
    in.map_confirmed = false;
    auto d           = decide_snapmaker_legacy_send(in);
    CHECK(d.kind == SnapmakerLegacySendKind::Refuse);
    CHECK(d.ams_mapping.empty());
    CHECK_FALSE(d.message.empty());

    in               = legacy_in();
    in.map           = {};
    d                = decide_snapmaker_legacy_send(in);
    CHECK(d.kind == SnapmakerLegacySendKind::Refuse);
    CHECK(d.ams_mapping.empty());

    in     = legacy_in();
    in.map = {1, 1}; // duplicate while extruders are free
    d      = decide_snapmaker_legacy_send(in);
    CHECK(d.kind == SnapmakerLegacySendKind::Refuse);
    CHECK(d.ams_mapping.empty());

    in      = legacy_in();
    in.used.clear(); // nothing to map: no start
    CHECK(decide_snapmaker_legacy_send(in).kind == SnapmakerLegacySendKind::Refuse);
}

TEST_CASE("Legacy send: disconnected or no agent is upload only, with a notice", "[ExtruderChoice][LegacySend]")
{
    auto in            = legacy_in();
    in.agent_connected = false;
    auto d             = decide_snapmaker_legacy_send(in);
    CHECK(d.kind == SnapmakerLegacySendKind::UploadOnly);
    CHECK(d.ams_mapping.empty());
    CHECK(d.upload_only_notice);

    in               = legacy_in();
    in.agent_present = false;
    d                = decide_snapmaker_legacy_send(in);
    CHECK(d.kind == SnapmakerLegacySendKind::UploadOnly);
    CHECK(d.upload_only_notice);

    in                 = legacy_in();
    in.agent_present   = false;
    in.agent_connected = true; // connected flag without an agent is not usable
    CHECK(decide_snapmaker_legacy_send(in).kind == SnapmakerLegacySendKind::UploadOnly);

    in                 = legacy_in();
    in.agent_connected = false;
    in.map_confirmed   = false; // upload-only needs no map
    CHECK(decide_snapmaker_legacy_send(in).kind == SnapmakerLegacySendKind::UploadOnly);
}

TEST_CASE("Legacy send: no start requested is a plain upload", "[ExtruderChoice][LegacySend]")
{
    auto in            = legacy_in();
    in.start_requested = false;
    in.map_confirmed   = false;
    const auto d       = decide_snapmaker_legacy_send(in);
    CHECK(d.kind == SnapmakerLegacySendKind::UploadOnly);
    CHECK_FALSE(d.upload_only_notice);
}

TEST_CASE("Legacy send: non-Snapmaker printers keep the legacy behaviour", "[ExtruderChoice][LegacySend]")
{
    auto in              = legacy_in();
    in.snapmaker_printer = false;
    in.map_confirmed     = false;
    in.agent_connected   = false;
    const auto d         = decide_snapmaker_legacy_send(in);
    CHECK(d.kind == SnapmakerLegacySendKind::Unchanged);
    CHECK(d.ams_mapping.empty());
    CHECK_FALSE(d.upload_only_notice);
}

TEST_CASE("Legacy send: more than four filaments starts with the oversubscribed notice", "[ExtruderChoice][LegacySend]")
{
    auto in = legacy_in();
    in.used.clear();
    for (int i = 0; i < 5; ++i)
        in.used.push_back(uf(i, "PLA", "#FF0000"));
    in.map       = {0, 1, 2, 3, 3};
    const auto d = decide_snapmaker_legacy_send(in);
    CHECK(d.kind == SnapmakerLegacySendKind::StartViaAgent);
    CHECK(d.oversubscribed);
    CHECK(d.ams_mapping == "[0,1,2,3,3]");
}
