#include "MoonrakerPreprintPrinter.hpp"

#include <algorithm>
#include <mutex>

#include <boost/log/trivial.hpp>

#include "Http.hpp"
#include "MoonrakerPrinterAgent.hpp"
#include "nlohmann/json.hpp"
#include "slic3r/GUI/SnapmakerDeviceBridge.hpp" // snapmaker_serial_from_system_info (the one place that reads the serial)

namespace Slic3r {

MoonrakerPreprintPrinter::Snapshot MoonrakerPreprintPrinter::snapshot() const
{
    std::lock_guard<std::recursive_mutex> lock(m_agent->connect_mutex);
    return {m_agent->device_info.base_url, m_agent->device_info.api_key, m_agent->device_info.dev_id};
}

SnapmakerPreprintPin MoonrakerPreprintPrinter::endpoint_of(const std::string& base_url)
{
    std::string rest;
    std::string port;
    if (base_url.rfind("http://", 0) == 0) { rest = base_url.substr(7); port = "80"; }
    else if (base_url.rfind("https://", 0) == 0) { rest = base_url.substr(8); port = "443"; }
    else return {};
    rest = rest.substr(0, rest.find('/'));
    if (rest.empty() || rest.find('@') != std::string::npos) return {};
    std::string host = rest;
    const std::size_t close = rest.rfind(']');
    const std::size_t colon = rest.rfind(':');
    if (colon != std::string::npos && (close == std::string::npos || colon > close)) {
        host = rest.substr(0, colon);
        port = rest.substr(colon + 1);
        if (port.empty() || !std::all_of(port.begin(), port.end(), [](unsigned char c) { return c >= '0' && c <= '9'; })) return {};
    }
    if (host.empty()) return {};
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return {host, port, std::string()};
}

SnapmakerPreprintPin MoonrakerPreprintPrinter::pin_of(const Snapshot& s) const
{
    SnapmakerPreprintPin pin = endpoint_of(s.base_url);
    if (pin.host.empty()) return {}; // nothing connected
    nlohmann::json json;
    std::string    error;
    if (!m_agent->get_json(m_agent->join_url(s.base_url, "/machine/system_info"), s.api_key, [] { return false; }, json, error)) {
        BOOST_LOG_TRIVIAL(warning) << "MoonrakerPreprintPrinter: could not read the printer serial: " << error;
        return pin; // serial stays empty: the gate refuses
    }
    pin.serial = GUI::snapmaker_serial_from_system_info(json);
    return pin;
}

SnapmakerPreprintPin MoonrakerPreprintPrinter::connected_pin()
{
    return m_agent ? pin_of(snapshot()) : SnapmakerPreprintPin{};
}

SnapmakerPreprintPin MoonrakerPreprintPrinter::bind_current()
{
    if (!m_agent) return {};
    const Snapshot       s   = snapshot();
    const SnapmakerPreprintPin pin = pin_of(s);
    std::lock_guard<std::mutex> lock(m_bind_mutex);
    m_bound = !pin.host.empty() && !pin.port.empty() && !pin.serial.empty();
    m_snap  = m_bound ? s : Snapshot{};
    m_pin   = m_bound ? pin : SnapmakerPreprintPin{};
    return pin;
}

bool MoonrakerPreprintPrinter::bound(Snapshot& out, SnapmakerPreprintPin& pin) const
{
    std::lock_guard<std::mutex> lock(m_bind_mutex);
    if (!m_bound) return false;
    out = m_snap;
    pin = m_pin;
    return true;
}

// The agent's current device must still be the bound one (host, port, live serial).
bool MoonrakerPreprintPrinter::still_pinned(const Snapshot&, const SnapmakerPreprintPin& pin)
{
    const SnapmakerPreprintPin now = connected_pin();
    return !now.serial.empty() && now == pin;
}

std::optional<std::string> MoonrakerPreprintPrinter::print_state()
{
    Snapshot s;
    SnapmakerPreprintPin pin;
    if (!m_agent || !bound(s, pin)) return std::nullopt;
    nlohmann::json json;
    std::string    error;
    if (s.base_url.empty() || !m_agent->get_json(m_agent->join_url(s.base_url, "/printer/objects/query?print_stats"), s.api_key, [] { return false; }, json, error))
        return std::nullopt;
    if (!json.is_object() || !json.contains("result") || !json["result"].is_object() || !json["result"].contains("status")) return std::nullopt;
    const auto& status = json["result"]["status"];
    if (!status.is_object() || !status.contains("print_stats") || !status["print_stats"].is_object()) return std::nullopt;
    const auto& ps = status["print_stats"];
    if (!ps.contains("state") || !ps["state"].is_string() || ps["state"].get<std::string>().empty()) return std::nullopt;
    return ps["state"].get<std::string>();
}

std::optional<std::uint64_t> MoonrakerPreprintPrinter::remote_file_size(const std::string& filename)
{
    Snapshot s;
    SnapmakerPreprintPin pin;
    if (!m_agent || !snapmaker_preprint_filename_ok(filename) || !bound(s, pin)) return std::nullopt;
    nlohmann::json json;
    std::string    error;
    if (s.base_url.empty() ||
        !m_agent->get_json(m_agent->join_url(s.base_url, "/server/files/metadata?filename=" + Http::url_encode(filename)), s.api_key, [] { return false; }, json, error))
        return std::nullopt;
    if (!json.is_object() || !json.contains("result") || !json["result"].is_object()) return std::nullopt;
    const auto& r = json["result"];
    if (r.contains("filename") && (!r["filename"].is_string() || r["filename"].get<std::string>() != filename)) return std::nullopt; // an answer for another file
    if (!r.contains("size") || !r["size"].is_number_unsigned()) return std::nullopt;
    return r["size"].get<std::uint64_t>();
}

SnapmakerPreprintPrinter::Known MoonrakerPreprintPrinter::command_known(const std::string& command)
{
    Snapshot s;
    SnapmakerPreprintPin pin;
    if (!m_agent || command.empty() || !bound(s, pin)) return Known::Unreadable;
    bool missing = false;
    if (!m_agent->gcode_command_known(s.base_url, s.api_key, command, missing)) return Known::Unreadable;
    return missing ? Known::No : Known::Yes;
}

// Same request as MoonrakerPrinterAgent::send_gcode_sync, but to the BOUND base URL (that one reads device_info, which may have moved on).
bool MoonrakerPreprintPrinter::post_script(const Snapshot& s, const std::string& script) const
{
    bool        success = false;
    std::string http_error;
    auto        http = Http::post(m_agent->join_url(s.base_url, "/printer/gcode/script"));
    if (!s.api_key.empty()) http.header("X-Api-Key", s.api_key);
    http.header("Content-Type", "application/json")
        .set_post_body(nlohmann::json{{"script", script}}.dump())
        .timeout_connect(5)
        .timeout_max(10)
        .on_complete([&](std::string, unsigned status) {
            if (status == 200) success = true;
            else http_error = "HTTP error: " + std::to_string(status);
        })
        .on_error([&](std::string, std::string err, unsigned status) {
            http_error = err;
            if (status > 0) http_error += " (HTTP " + std::to_string(status) + ")";
        })
        .perform_sync();
    if (!success) BOOST_LOG_TRIVIAL(error) << "MoonrakerPreprintPrinter: send_gcode failed: " << http_error;
    return success;
}

bool MoonrakerPreprintPrinter::send_gcode(const std::string& script)
{
    Snapshot s;
    SnapmakerPreprintPin pin;
    if (!m_agent || script.empty() || !bound(s, pin)) return false;
    if (!still_pinned(s, pin)) {
        BOOST_LOG_TRIVIAL(error) << "MoonrakerPreprintPrinter: the connected printer is no longer the pinned one, the script is not sent";
        return false;
    }
    return post_script(s, script);
}

std::string MoonrakerPreprintPrinter::start_line(const std::string& filename)
{
    return snapmaker_preprint_filename_ok(filename) ? "SDCARD_PRINT_FILE FILENAME=\"" + filename + "\"" : std::string();
}

bool MoonrakerPreprintPrinter::start_print(const std::string& filename)
{
    const std::string line = start_line(filename);
    Snapshot s;
    SnapmakerPreprintPin pin;
    if (!m_agent || line.empty() || !bound(s, pin)) return false;
    if (!still_pinned(s, pin)) {
        BOOST_LOG_TRIVIAL(error) << "MoonrakerPreprintPrinter: the connected printer is no longer the pinned one, the print is not started";
        return false;
    }
    return post_script(s, line); // never print_uploaded_file: that would re-send the map and preferences from PrintParams
}

} // namespace Slic3r
