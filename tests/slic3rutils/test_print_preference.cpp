#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "slic3r/Utils/SnapmakerPrintPreference.hpp"

using namespace Slic3r;

// The expected strings are our reading of Snapmaker Orca's Flutter page (resources/web/flutter_web/main.b347974c91352c2f.js):
// the page builds one "SET_PRINT_PREFERENCE KEY=<key> VALUE=<v>" line per key (line 148187), joins them with "\n" (line ~39870),
// in the insertion order of its preference map bed_level, flow_calibrate, time_lapse_camera (line 40793), with 0/1 ints (line 151459).
// This shows our code agrees with that reading, not that the firmware honours the commands.
TEST_CASE("The Snapmaker print preference script has the page's lines in the page's order", "[PrintPreference]")
{
    CHECK(build_snapmaker_print_preference_script(false, false, false) ==
          "SET_PRINT_PREFERENCE KEY=bed_level VALUE=0\n"
          "SET_PRINT_PREFERENCE KEY=flow_calibrate VALUE=0\n"
          "SET_PRINT_PREFERENCE KEY=time_lapse_camera VALUE=0");
    // Arguments are (flow_calibrate, time_lapse, bed_level)
    CHECK(build_snapmaker_print_preference_script(true, true, true) ==
          "SET_PRINT_PREFERENCE KEY=bed_level VALUE=1\n"
          "SET_PRINT_PREFERENCE KEY=flow_calibrate VALUE=1\n"
          "SET_PRINT_PREFERENCE KEY=time_lapse_camera VALUE=1");
    CHECK(build_snapmaker_print_preference_script(true, false, false) ==
          "SET_PRINT_PREFERENCE KEY=bed_level VALUE=0\n"
          "SET_PRINT_PREFERENCE KEY=flow_calibrate VALUE=1\n"
          "SET_PRINT_PREFERENCE KEY=time_lapse_camera VALUE=0");
    CHECK(build_snapmaker_print_preference_script(false, true, false) ==
          "SET_PRINT_PREFERENCE KEY=bed_level VALUE=0\n"
          "SET_PRINT_PREFERENCE KEY=flow_calibrate VALUE=0\n"
          "SET_PRINT_PREFERENCE KEY=time_lapse_camera VALUE=1");
    CHECK(build_snapmaker_print_preference_script(false, false, true) ==
          "SET_PRINT_PREFERENCE KEY=bed_level VALUE=1\n"
          "SET_PRINT_PREFERENCE KEY=flow_calibrate VALUE=0\n"
          "SET_PRINT_PREFERENCE KEY=time_lapse_camera VALUE=0");
}

TEST_CASE("Every combination yields a script the validator accepts", "[PrintPreference]")
{
    for (int bits = 0; bits < 8; ++bits)
        CHECK(is_snapmaker_print_preference_script(build_snapmaker_print_preference_script(bits & 1, bits & 2, bits & 4)));
}

TEST_CASE("The validator accepts only the three preference lines with 0 or 1", "[PrintPreference]")
{
    const std::string ok = build_snapmaker_print_preference_script(true, false, true);
    CHECK(is_snapmaker_print_preference_script(ok));

    CHECK_FALSE(is_snapmaker_print_preference_script(""));
    CHECK_FALSE(is_snapmaker_print_preference_script(ok + "\n"));
    CHECK_FALSE(is_snapmaker_print_preference_script(ok + "\nG28"));
    CHECK_FALSE(is_snapmaker_print_preference_script("G28"));
    CHECK_FALSE(is_snapmaker_print_preference_script("SET_PRINT_PREFERENCE KEY=bed_level VALUE=1"));
    // Wrong order, wrong value, wrong key, extra text on a line
    CHECK_FALSE(is_snapmaker_print_preference_script("SET_PRINT_PREFERENCE KEY=flow_calibrate VALUE=0\n"
                                                     "SET_PRINT_PREFERENCE KEY=bed_level VALUE=0\n"
                                                     "SET_PRINT_PREFERENCE KEY=time_lapse_camera VALUE=0"));
    CHECK_FALSE(is_snapmaker_print_preference_script("SET_PRINT_PREFERENCE KEY=bed_level VALUE=2\n"
                                                     "SET_PRINT_PREFERENCE KEY=flow_calibrate VALUE=0\n"
                                                     "SET_PRINT_PREFERENCE KEY=time_lapse_camera VALUE=0"));
    CHECK_FALSE(is_snapmaker_print_preference_script("SET_PRINT_PREFERENCE KEY=bed_level VALUE=0\n"
                                                     "SET_PRINT_PREFERENCE KEY=flow_calibrate VALUE=0\n"
                                                     "SET_PRINT_PREFERENCE KEY=time_lapse_camera VALUE=0; M112"));
    CHECK_FALSE(is_snapmaker_print_preference_script("SET_PRINT_PREFERENCE KEY=bed_level VALUE=0\n"
                                                     "SET_PRINT_PREFERENCE KEY=nozzle VALUE=0\n"
                                                     "SET_PRINT_PREFERENCE KEY=time_lapse_camera VALUE=0"));
}

// The golden strings below come from our reading of the Flutter page (main.b347974c91352c2f.js ~39855-39875, ~148178, map built at ~150065):
// "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=<filament, 0-based> MAP_EXTRUDER=<extruder, 0-based>\n" per used filament, then
// "SET_PRINT_USED_EXTRUDERS EXTRUDERS=<distinct values, comma separated>". They show agreement with that reading, not with the printer.
TEST_CASE("The extruder map script follows the page's lines for a non-identity mapping", "[PrintPreference][ExtruderMap]")
{
    const auto r = build_snapmaker_extruder_map_script({2, 0, 3});
    REQUIRE(r.ok());
    CHECK(r.script ==
          "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=0 MAP_EXTRUDER=2\n"
          "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=1 MAP_EXTRUDER=0\n"
          "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=2 MAP_EXTRUDER=3\n"
          "SET_PRINT_USED_EXTRUDERS EXTRUDERS=2,0,3");
    CHECK(is_snapmaker_extruder_map_script(r.script));
}

TEST_CASE("Two filaments on one extruder list that extruder once; unused filaments are left out", "[PrintPreference][ExtruderMap]")
{
    const auto r = build_snapmaker_extruder_map_script({1, -1, 1, 3}, {true, false, true, true});
    REQUIRE(r.ok());
    CHECK(r.script ==
          "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=0 MAP_EXTRUDER=1\n"
          "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=2 MAP_EXTRUDER=1\n"
          "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=3 MAP_EXTRUDER=3\n"
          "SET_PRINT_USED_EXTRUDERS EXTRUDERS=1,3");
    CHECK(is_snapmaker_extruder_map_script(r.script));
}

TEST_CASE("Invalid extruder mappings are rejected", "[PrintPreference][ExtruderMap]")
{
    CHECK_FALSE(build_snapmaker_extruder_map_script({0, -1}).ok());                     // used but unmapped
    CHECK_FALSE(build_snapmaker_extruder_map_script({0, 4}).ok());                      // extruder out of range
    CHECK_FALSE(build_snapmaker_extruder_map_script({0, -2}).ok());                     // negative
    CHECK_FALSE(build_snapmaker_extruder_map_script({0, 1}, {true}).ok());              // used list size mismatch
    CHECK_FALSE(build_snapmaker_extruder_map_script(std::vector<int>(33, 0)).ok());     // absurd size
    CHECK_FALSE(build_snapmaker_extruder_map_script({}, {}, 2).ok());                   // several used filaments, no mapping
    CHECK(build_snapmaker_extruder_map_script({0, -1}, {true, false}).ok());            // unmapped but unused
    for (const char* bad : {"", "G28", "SET_PRINT_USED_EXTRUDERS EXTRUDERS=0",
                            "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=0 MAP_EXTRUDER=9\nSET_PRINT_USED_EXTRUDERS EXTRUDERS=9",
                            "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=0 MAP_EXTRUDER=1\nSET_PRINT_USED_EXTRUDERS EXTRUDERS=2",
                            "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=-1 MAP_EXTRUDER=1\nSET_PRINT_USED_EXTRUDERS EXTRUDERS=1",
                            "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=0 MAP_EXTRUDER=1\nSET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=0 MAP_EXTRUDER=1\n"
                            "SET_PRINT_USED_EXTRUDERS EXTRUDERS=1"})
        CHECK_FALSE(is_snapmaker_extruder_map_script(bad));
}

TEST_CASE("An empty mapping sends no map lines unless several filaments are used", "[PrintPreference][ExtruderMap]")
{
    const auto none = build_snapmaker_extruder_map_script({});
    CHECK(none.ok());
    CHECK(none.script.empty());
    const auto one = build_snapmaker_extruder_map_script({}, {}, 1);
    CHECK(one.ok());
    CHECK(one.script.empty());
    CHECK_FALSE(build_snapmaker_extruder_map_script({}, {}, 2).ok());
}

TEST_CASE("PrintParams::ams_mapping parses strictly", "[PrintPreference][ExtruderMap]")
{
    std::vector<int> m;
    CHECK(parse_snapmaker_ams_mapping("", m));
    CHECK(m.empty());
    CHECK(parse_snapmaker_ams_mapping("[2,0,3]", m));
    CHECK(m == std::vector<int>({2, 0, 3}));
    CHECK_FALSE(parse_snapmaker_ams_mapping("{\"a\":1}", m));
    CHECK_FALSE(parse_snapmaker_ams_mapping("[0,\"1\"]", m));
    CHECK_FALSE(parse_snapmaker_ams_mapping("[0,1.5]", m));
    CHECK_FALSE(parse_snapmaker_ams_mapping("[9]", m));
    CHECK_FALSE(parse_snapmaker_ams_mapping("[18446744073709551615]", m)); // must not wrap to -1
    CHECK_FALSE(parse_snapmaker_ams_mapping("[256]", m));
    CHECK(parse_snapmaker_ams_mapping("[0,1,-1,254,255]", m)); // unused and external-tray ids become -1
    CHECK(m == std::vector<int>({0, 1, -1, -1, -1}));
    CHECK_FALSE(parse_snapmaker_ams_mapping("[-2]", m));
    CHECK_FALSE(parse_snapmaker_ams_mapping("not json", m));
}
