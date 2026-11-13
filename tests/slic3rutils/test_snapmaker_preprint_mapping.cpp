#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "slic3r/Utils/SnapmakerPreprintMapping.hpp"

// These goldens were written by reading Snapmaker's SSWCP::build_filament_mapping_json (SSWCP.cpp ~2531) and its helpers, not by running it:
// they show our builder agrees with that reading, not that the page accepts the result.

using namespace Slic3r;
using nlohmann::json;

namespace {
SnapmakerPreprintPlate four_colour_plate()
{
    SnapmakerPreprintPlate p;
    p.estimated_time_s     = 3725;
    p.filament_colour      = {"#FF0000", "#00ff00", "#0000FFAA", "bad"};
    p.filament_type        = {"PLA", " PETG ", "PLA", "TPU"};
    p.filament_density     = {1.24, 1.27, 1.24, 1.2};
    p.filament_diameter    = {1.75, 1.75, 1.75, 1.75};
    p.volume_mm3           = {{0, 1000.0}, {2, 2500.0}};
    p.nozzle_diameter      = {0.4, 0.4, 0.6, 0.5};
    p.filament_extruder_map = {{0, 0}, {1, 1}, {2, 2}, {3, 3}};
    p.machine_model        = "Snapmaker U1 (0.4 nozzle)";
    p.display_name         = "benchy.gcode";
    p.thumbnails           = {{"QUJD", 48, 48}, {"REVG", 300, 300}};
    return p;
}
json build(const SnapmakerPreprintPlate& p)
{
    json out;
    std::string err;
    REQUIRE(build_snapmaker_preprint_mapping_json(p, out, err));
    CHECK(err.empty());
    return out;
}
bool fails(const SnapmakerPreprintPlate& p)
{
    json out = json::array();
    std::string err;
    const bool ok = build_snapmaker_preprint_mapping_json(p, out, err);
    CHECK((ok || (!err.empty() && out == json::object())));
    return !ok;
}
} // namespace

TEST_CASE("preprint mapping: golden JSON for a 4-colour plate", "[SSWCP][preprint]")
{
    const json j = build(four_colour_plate());
    CHECK(j["estimated_time"] == 3725);
    CHECK(j["filament_color"] == json::array({0xFF0000, 0x00FF00, 0x0000FFAA, 0}));
    CHECK(j["filament_color_rgba"] == json::array({"#FF0000", "#00ff00", "#0000FFAA", "bad"}));
    CHECK(j["filament_color_multi"] == json::parse(R"([
        {"mode":0,"nums":1,"colors":["FF0000"]},
        {"mode":0,"nums":1,"colors":["00FF00"]},
        {"mode":0,"nums":1,"colors":["0000FF"]},
        {"mode":0,"nums":1,"colors":["26A69A"]}])"));
    CHECK(j["filament_type"] == json::array({"PLA", "PETG", "PLA", "TPU"}));
    CHECK(j["filament_volume_type"] == json::array({"standard", "standard", "standard", "standard"}));
    CHECK(j["nozzle_volume_types"] == json::array({"standard", "standard", "standard", "standard"}));
    CHECK(j["nozzle_diameters"] == json::array({"0.4", "0.4", "0.6", "0.5"}));
    CHECK(j["nozzle_info"][0] == "0.4");
    CHECK(j["nozzle_info"][2] == "0.6");
    CHECK(j["nozzle_info"][3] == "0.500000"); // Snapmaker falls back to std::to_string(float) for a diameter it has no label for
    REQUIRE(j["filament_weight"].size() == 4);
    CHECK(j["filament_weight"][0].get<double>() == Catch::Approx(1.24));
    CHECK(j["filament_weight"][1].get<double>() == 0.0);
    CHECK(j["filament_weight"][2].get<double>() == Catch::Approx(3.1));
    CHECK(j["filament_weight_total"].get<double>() == Catch::Approx(4.34));
    CHECK(j["filament_used_mm"][0].get<double>() == Catch::Approx(1000.0 / (3.14159265358979 * 0.875 * 0.875)));
    CHECK(j["filament_used_mm"][1].get<double>() == 0.0);
    CHECK(j["filament_extruder_map"] == json::parse(R"({"0":"0","1":"1","2":"2","3":"3"})"));
    CHECK(j["machine_model"] == "Snapmaker U1 (0.4 nozzle)");
    REQUIRE(j["thumbnails"].size() == 2);
    CHECK(j["thumbnails"][0] == json::parse(R"({"url":"data:image/png;base64,QUJD","width":48.0,"height":48.0})"));
    CHECK(j["filename"] == "benchy.gcode");
}

TEST_CASE("preprint mapping: absent optional fields are omitted, not emitted empty", "[SSWCP][preprint]")
{
    SnapmakerPreprintPlate p;
    p.display_name = "a.gcode";
    const json j   = build(p);
    for (const char* k : {"filament_color", "filament_color_rgba", "filament_color_multi", "filament_type", "nozzle_diameters", "filament_weight",
                          "filament_weight_total", "filament_used_mm", "filament_extruder_map", "nozzle_info", "machine_model", "filepath"}) {
        CAPTURE(k);
        CHECK_FALSE(j.contains(k));
    }
    CHECK(j["thumbnails"] == json::array());
    CHECK(j["filename"] == "a.gcode");
    CHECK(j["filament_volume_type"] == json::array({"standard"})); // Snapmaker: at least one entry
}

TEST_CASE("preprint mapping: colour to int", "[SSWCP][preprint]")
{
    using preprint_detail::color_to_int;
    CHECK(color_to_int("#FFFFFF") == 0xFFFFFF);
    CHECK(color_to_int("#abcdef") == 0xABCDEF);
    CHECK(color_to_int("#11223344") == 0x11223344);
    for (const char* bad : {"", "#", "FF0000", "#FF00", "#GG0000", "#FF00000", "#FF0000000"}) {
        CAPTURE(bad);
        CHECK(color_to_int(bad) == 0);
    }
}

TEST_CASE("preprint mapping: nozzle diameters are one-decimal strings", "[SSWCP][preprint]")
{
    SnapmakerPreprintPlate p;
    p.nozzle_diameter = {0.2, 0.4, 0.25, 0.800001};
    const json j      = build(p);
    CHECK(j["nozzle_diameters"] == json::array({"0.2", "0.4", "0.2", "0.8"})); // 0.25 is an exact binary tie: printf-style rounding gives the even digit
    CHECK(j["nozzle_info"][3] == "0.8");  // within 1e-5 of 0.8
    p.nozzle_diameter = {0.80001};
    CHECK(build(p)["nozzle_info"][0] == "0.800010"); // exactly 1e-5 away (as float) is outside Snapmaker's epsilon
}

TEST_CASE("preprint mapping: multi-colour and gradient items follow FilamentColor", "[SSWCP][preprint]")
{
    SnapmakerPreprintPlate p;
    p.filament_colour       = {"#112233", "#112233", "#112233"};
    p.filament_multi_colors = {"#aa0000|#00bb00", "#aa0000|#00bb00", "zz|#00bb00ff"};
    p.filament_colour_mode  = {0, 1, 1};
    const json j            = build(p);
    CHECK(j["filament_color_multi"][0] == json::parse(R"({"mode":0,"nums":2,"colors":["AA0000","00BB00"]})"));
    CHECK(j["filament_color_multi"][1] == json::parse(R"({"mode":1,"nums":2,"colors":["AA0000","00BB00"]})"));
    CHECK(j["filament_color_multi"][2] == json::parse(R"({"mode":0,"nums":1,"colors":["00BB00"]})")); // one colour left: gradient collapses to segment
}

TEST_CASE("preprint mapping: strict bounds", "[SSWCP][preprint]")
{
    CHECK_FALSE(fails(four_colour_plate()));
    { auto p = four_colour_plate(); p.filament_colour.assign(33, "#000000"); CHECK(fails(p)); }
    { auto p = four_colour_plate(); p.filament_colour.assign(32, "#000000"); p.filament_extruder_map.clear(); CHECK_FALSE(fails(p)); }
    { auto p = four_colour_plate(); p.nozzle_diameter.assign(5, 0.4); CHECK(fails(p)); }
    { auto p = four_colour_plate(); p.thumbnails.assign(9, {"QUJD", 1, 1}); CHECK(fails(p)); }
    { auto p = four_colour_plate(); p.thumbnails = {{"QU JD", 1, 1}}; CHECK(fails(p)); }
    { auto p = four_colour_plate(); p.thumbnails = {{"QUJD", 0, 1}}; CHECK(fails(p)); }
    { auto p = four_colour_plate(); p.thumbnails = {{std::string(snapmaker_preprint_max_thumbnail_bytes + 1, 'A'), 1, 1}}; CHECK(fails(p)); }
    { auto p = four_colour_plate(); p.filament_extruder_map = {{0, 4}}; CHECK(fails(p)); }
    { auto p = four_colour_plate(); p.filament_extruder_map = {{0, -1}}; CHECK(fails(p)); }
    { auto p = four_colour_plate(); p.filament_extruder_map = {{4, 0}}; CHECK(fails(p)); }  // beyond the 4 filaments of the plate
    { auto p = four_colour_plate(); p.filament_extruder_map = {{-1, 0}}; CHECK(fails(p)); }
    { auto p = four_colour_plate(); p.estimated_time_s = -1; CHECK(fails(p)); }
    { auto p = four_colour_plate(); p.estimated_time_s = std::nan(""); CHECK(fails(p)); }
    { auto p = four_colour_plate(); p.filament_density[0] = INFINITY; CHECK(fails(p)); }
    { auto p = four_colour_plate(); p.volume_mm3[1] = -5; CHECK(fails(p)); }
    { auto p = four_colour_plate(); p.display_name = "a\nb"; CHECK(fails(p)); }
    { auto p = four_colour_plate(); p.display_name = std::string(513, 'a'); CHECK(fails(p)); }
    { auto p = four_colour_plate(); p.filepath = "/x\ty"; CHECK(fails(p)); }
    // A volume for a filament the plate does not have is skipped, as Snapmaker does.
    { auto p = four_colour_plate(); p.volume_mm3[9] = 5; CHECK_FALSE(fails(p)); }
}

TEST_CASE("preprint mapping: filepath only when supplied", "[SSWCP][preprint]")
{
    auto p = four_colour_plate();
    CHECK_FALSE(build(p).contains("filepath"));
    p.filepath = "/tmp/plate.gcode";
    CHECK(build(p)["filepath"] == "/tmp/plate.gcode");
}
