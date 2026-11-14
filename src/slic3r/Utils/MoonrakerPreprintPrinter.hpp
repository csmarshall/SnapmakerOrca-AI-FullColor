#pragma once

// Send-flow slice S4c (docs/records/snapmaker-u1-send-flow-design.md section 3): the real SnapmakerPreprintPrinter, on top of the Moonraker
// (Snapmaker) agent's connection. A friend of MoonrakerPrinterAgent, so the agent's public surface does not grow.
//
// Rules, each tested against a loopback fake in tests/slic3rutils/test_moonraker_preprint_printer.cpp:
//   - Every read is a fresh, read-only HTTP GET (never status_cache / m_last_good_state). A failure of any kind means "unknown" (the gate refuses).
//   - connected_pin() is the agent's CONNECTED device (device_info.base_url: host and port) plus the serial read live from
//     /machine/system_info each call. It never uses the printer preset's print_host (#117 PrintHost::with_device_host_fallback) and never
//     the Device tab's selected machine; with nothing connected, or an unreadable serial, a field is empty and the gate refuses.
//   - The only writes: send_gcode (printer.gcode.script via the agent's send_gcode_sync) and start_print, which sends exactly
//     SDCARD_PRINT_FILE FILENAME="<name>" (never print_uploaded_file, which would re-send the map and preferences from PrintParams).
//   - F1 (binding): bind_current() pins the agent's connected device (base URL + key + live serial). Every read and write then goes to THAT
//     base URL only. Immediately before send_gcode and start_print the agent's current device is re-read (host, port, live serial) and compared
//     with the pin; a mismatch refuses, so SET_PRINT_* / SDCARD_PRINT_FILE can not land on a device the agent switched to meanwhile. Unbound
//     (nothing connected, serial unreadable) every call fails closed. A same-address printer swap between that check and the POST is not
//     closable from here.
// Everything here blocks on HTTP (5 s connect, 10 s total per request): call it off the UI thread.

#include <memory>
#include <mutex>

#include "SnapmakerPreprintPrinter.hpp"

namespace Slic3r {

class MoonrakerPrinterAgent;

class MoonrakerPreprintPrinter final : public SnapmakerPreprintPrinter
{
public:
    explicit MoonrakerPreprintPrinter(std::shared_ptr<MoonrakerPrinterAgent> agent) : m_agent(std::move(agent)) {}

    // Pins the agent's connected device now and returns its pin (empty fields when nothing is connected or the serial is unreadable; then
    // nothing is bound and every call below fails closed). Call it once when the pre-print context opens.
    SnapmakerPreprintPin         bind_current();
    // The agent's CURRENT device with a live serial (what the gate compares against the pin); does not change the binding.
    SnapmakerPreprintPin         connected_pin() override;
    std::optional<std::string>   print_state() override;
    std::optional<std::uint64_t> remote_file_size(const std::string& filename) override;
    Known                        command_known(const std::string& command) override;
    bool                         send_gcode(const std::string& script) override;
    bool                         start_print(const std::string& filename) override;

    // "http://<host>" of the agent's CURRENT connected device (no HTTP, no serial read), "" when nothing is connected or the address is not a
    // plain host. Used to build the page's CSP origin from the SAME device the pin is taken from (never from the Device tab's selection).
    std::string                  connected_origin() const;

    // The exact start line for a pinned name, or empty when snapmaker_preprint_filename_ok() refuses the name (allowlist, see the gate).
    static std::string start_line(const std::string& filename);
    // "host" and "port" (scheme default when the URL has none) of a base URL such as http://u1.local:7125; both empty when unparseable.
    // The host is lower-cased and keeps IPv6 brackets.
    static SnapmakerPreprintPin endpoint_of(const std::string& base_url);

private:
    struct Snapshot
    {
        std::string base_url, api_key, dev_id;
    };
    // One snapshot of the connection under the agent's lock; every request of a call uses only this copy.
    Snapshot snapshot() const;
    bool     bound(Snapshot& out, SnapmakerPreprintPin& pin) const;
    bool     still_pinned(const Snapshot& s, const SnapmakerPreprintPin& pin);
    bool     post_script(const Snapshot& s, const std::string& script) const;
    SnapmakerPreprintPin pin_of(const Snapshot& s) const;
    mutable std::mutex   m_bind_mutex;
    bool                 m_bound = false;
    Snapshot             m_snap;
    SnapmakerPreprintPin m_pin;
    std::shared_ptr<MoonrakerPrinterAgent> m_agent;
};

} // namespace Slic3r
