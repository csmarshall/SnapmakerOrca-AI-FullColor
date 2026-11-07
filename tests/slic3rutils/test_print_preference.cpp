#include <catch2/catch_test_macros.hpp>

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
