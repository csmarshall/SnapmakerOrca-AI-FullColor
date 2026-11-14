#pragma once

// Send-flow slice S4b/S4c: the printer-facing interface of the pre-print session (GUI/SnapmakerPreprintSession.hpp). It lives in Utils/ so the
// real adapter (MoonrakerPreprintPrinter) does not include GUI headers; the session and the tests inject any implementation.

#include <cstdint>
#include <optional>
#include <string>

#include "SnapmakerPreprintGate.hpp"

namespace Slic3r {

// Everything the pre-print profile asks of the printer. The real implementation sits on the Snapmaker Moonraker agent's connection;
// tests use a fake. No method may fall back to the printer preset's print_host (#117): the pin is the agent's connected device.
class SnapmakerPreprintPrinter
{
public:
    virtual ~SnapmakerPreprintPrinter() = default;
    enum class Known { Yes, No, Unreadable };
    // The agent's connected device right now (host, port, serial as exact strings, canonicalised the same way every call). Empty
    // fields = nothing connected or the serial could not be read; the gate then refuses (it never matches a complete pin).
    virtual SnapmakerPreprintPin connected_pin() = 0;
    // print_stats.state from a fresh GET, never a cached status; nullopt when it could not be read.
    virtual std::optional<std::string> print_state() = 0;
    // server.files.metadata(filename).size; nullopt when the file or the answer is missing.
    virtual std::optional<std::uint64_t> remote_file_size(const std::string& filename) = 0;
    // Whether the printer registers the G-code command (/printer/gcode/help).
    virtual Known command_known(const std::string& command) = 0;
    // printer.gcode.script through the agent's send_gcode_sync.
    virtual bool send_gcode(const std::string& script) = 0;
    // The start only: SDCARD_PRINT_FILE FILENAME="<filename>" (the map/preference lines were already sent by send_gcode).
    virtual bool start_print(const std::string& filename) = 0;
};

} // namespace Slic3r
