#pragma once

#include <string>

namespace Slic3r {

// The per-print preferences Snapmaker Orca's send page applies by sending gcode lines to the printer before the print starts
// (resources/web/flutter_web/main.b347974c91352c2f.js: setPrePrintConfiguration ~39855, line builder ~148187, preference map ~40793).
// Nothing in the plate's G-code changes. Only these three fixed lines with a 0 or 1 value are ever produced or accepted.
//
// One "SET_PRINT_PREFERENCE KEY=<key> VALUE=<v>" line per key, joined with "\n" and no trailing newline, in the page's map order:
// bed_level, flow_calibrate, time_lapse_camera.
inline std::string build_snapmaker_print_preference_script(bool flow_calibrate, bool time_lapse, bool bed_level)
{
    auto line = [](const char* key, bool on) { return std::string("SET_PRINT_PREFERENCE KEY=") + key + " VALUE=" + (on ? "1" : "0"); };
    return line("bed_level", bed_level) + "\n" + line("flow_calibrate", flow_calibrate) + "\n" + line("time_lapse_camera", time_lapse);
}

// True only for a script that build_snapmaker_print_preference_script could have produced.
inline bool is_snapmaker_print_preference_script(const std::string& script)
{
    for (int bits = 0; bits < 8; ++bits)
        if (script == build_snapmaker_print_preference_script(bits & 1, bits & 2, bits & 4))
            return true;
    return false;
}

} // namespace Slic3r
