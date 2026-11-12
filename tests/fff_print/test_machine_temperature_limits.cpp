// Printer-level maximum bed and nozzle temperature (machine_max_bed_temperature / machine_max_nozzle_temperature).
// 0 = no limit. Over a non-zero limit the value is clamped when the config is applied and a warning is raised by Print::validate.
// VERIFIED here: clamping on both layers, the active plate only, nozzle clamping, unchanged G-code when no limit bites, the warning text,
// preset-list registration. NOT shown: that any particular limit (e.g. 100 for the U1) is the real firmware limit.

#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>
#include <regex>
#include <sstream>
#include <string>

#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {
int first_s(const std::string &gcode, const char *cmd)
{
    std::smatch m;
    if (std::regex_search(gcode, m, std::regex(std::string(cmd) + R"( S(\d+))")))
        return std::stoi(m[1]);
    return -1;
}

// Drop the config-block lines naming the new options, which legitimately differ between runs.
std::string without_limit_lines(const std::string &gcode)
{
    std::istringstream in(gcode);
    std::string        out, line;
    while (std::getline(in, line))
        if (line.find("machine_max_bed_temperature") == std::string::npos && line.find("machine_max_nozzle_temperature") == std::string::npos)
            out += line + "\n";
    return out;
}

std::vector<StringObjectException> warnings_of(const std::vector<std::pair<std::string, std::string>> &items)
{
    Print    print;
    Model    model;
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict("use_relative_e_distances", "0"); // validate() rejects relative E without a reset in the layer G-code
    for (const auto &kv : items)
        config.set_deserialize_strict(kv.first, kv.second);
    init_print({make_cube(20, 20, 20)}, print, model, config);
    std::vector<StringObjectException> warnings;
    const StringObjectException err = print.validate(&warnings);
    INFO("validate error: " << err.string << "; clamps=" << print.temperature_clamps().size() << "; warnings=" << warnings.size());
    CHECK(err.string.empty());
    return warnings;
}

bool has_warning(const std::vector<StringObjectException> &w, const std::string &needle)
{
    return std::any_of(w.begin(), w.end(), [&](const StringObjectException &e) { return e.string.find(needle) != std::string::npos; });
}
} // namespace

TEST_CASE("Machine temperature limits are registered, default to 0 and are in the printer preset list", "[MachineTempLimit]")
{
    const auto &def = print_config_def;
    REQUIRE(def.get("machine_max_bed_temperature") != nullptr);
    REQUIRE(def.get("machine_max_nozzle_temperature") != nullptr);
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    CHECK(config.opt_int("machine_max_bed_temperature") == 0);
    CHECK(config.opt_int("machine_max_nozzle_temperature") == 0);
    const auto &printer_keys = Preset::printer_options();
    CHECK(std::find(printer_keys.begin(), printer_keys.end(), "machine_max_bed_temperature") != printer_keys.end());
    CHECK(std::find(printer_keys.begin(), printer_keys.end(), "machine_max_nozzle_temperature") != printer_keys.end());
}

TEST_CASE("Bed limit clamps both layers of the active plate and warns", "[MachineTempLimit]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "curr_bed_type", "High Temp Plate" },
        { "hot_plate_temp", "110" },
        { "hot_plate_temp_initial_layer", "105" },
        { "machine_max_bed_temperature", "100" },
    });
    const std::string gcode = Slic3r::Test::slice({make_cube(20, 20, 20)}, config);
    CHECK(first_s(gcode, "M190") == 100);
    CHECK(gcode.find("S110") == std::string::npos); // both layers were lowered
    CHECK(gcode.find("S105") == std::string::npos);

    const auto w = warnings_of({ { "curr_bed_type", "High Temp Plate" }, { "hot_plate_temp", "110" },
                                 { "hot_plate_temp_initial_layer", "105" }, { "machine_max_bed_temperature", "100" } });
    CHECK(has_warning(w, "105"));
    CHECK(has_warning(w, "110"));
    CHECK(has_warning(w, "100"));
}

TEST_CASE("Only the first layer over the limit is clamped; the other layer is left alone", "[MachineTempLimit]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "curr_bed_type", "High Temp Plate" },
        { "hot_plate_temp", "90" },
        { "hot_plate_temp_initial_layer", "105" },
        { "machine_max_bed_temperature", "100" },
    });
    const std::string gcode = Slic3r::Test::slice({make_cube(20, 20, 20)}, config);
    CHECK(first_s(gcode, "M190") == 100);
    CHECK(first_s(gcode, "M140") == 90);
}

TEST_CASE("A plate other than the active one does not raise a warning", "[MachineTempLimit]")
{
    const auto w = warnings_of({ { "curr_bed_type", "High Temp Plate" }, { "hot_plate_temp", "60" }, { "hot_plate_temp_initial_layer", "60" },
                                 { "textured_plate_temp", "110" }, { "textured_plate_temp_initial_layer", "110" },
                                 { "machine_max_bed_temperature", "100" } });
    CHECK_FALSE(has_warning(w, "110"));
}

TEST_CASE("Nozzle limit clamps nozzle temperatures and warns", "[MachineTempLimit]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "nozzle_temperature", "320" },
        { "nozzle_temperature_initial_layer", "330" },
        { "machine_max_nozzle_temperature", "300" },
    });
    const std::string gcode = Slic3r::Test::slice({make_cube(20, 20, 20)}, config);
    CHECK(gcode.find("S300") != std::string::npos);
    CHECK(gcode.find("S320") == std::string::npos);
    CHECK(gcode.find("S330") == std::string::npos);

    const auto w = warnings_of({ { "nozzle_temperature", "320" }, { "nozzle_temperature_initial_layer", "330" },
                                 { "machine_max_nozzle_temperature", "300" } });
    CHECK(has_warning(w, "330"));
    CHECK(has_warning(w, "300"));
}

TEST_CASE("Golden: limit 0, or a limit nothing exceeds, leaves the G-code byte-identical", "[MachineTempLimit][Golden]")
{
    DynamicPrintConfig base = DynamicPrintConfig::full_print_config();
    base.set_deserialize_strict({
        { "curr_bed_type", "High Temp Plate" },
        { "hot_plate_temp", "110" },
        { "hot_plate_temp_initial_layer", "105" },
        { "nozzle_temperature", "230" },
        { "nozzle_temperature_initial_layer", "235" },
    });
    DynamicPrintConfig off = base, high = base;
    off.set_deserialize_strict({ { "machine_max_bed_temperature", "0" }, { "machine_max_nozzle_temperature", "0" } });
    high.set_deserialize_strict({ { "machine_max_bed_temperature", "120" }, { "machine_max_nozzle_temperature", "300" } });
    const std::string a = without_limit_lines(Slic3r::Test::slice({make_cube(20, 20, 20)}, base));
    const std::string b = without_limit_lines(Slic3r::Test::slice({make_cube(20, 20, 20)}, off));
    const std::string c = without_limit_lines(Slic3r::Test::slice({make_cube(20, 20, 20)}, high));
    CHECK(first_s(a, "M190") == 105);
    CHECK(a == b);
    CHECK(a == c);
    CHECK_FALSE(has_warning(warnings_of({ { "curr_bed_type", "High Temp Plate" }, { "hot_plate_temp", "110" },
                                          { "hot_plate_temp_initial_layer", "105" } }), "above this printer"));
}
