#pragma once

// Send-flow slice S4b (docs/records/snapmaker-u1-send-flow-design.md section 3): the pre-print bridge profile's logic, wx-free.
// One SnapmakerPreprintSession per pre-print dialog. It joins the S4a gate (SnapmakerPreprintGate.hpp, which decides but never touches a
// printer) to an injected SnapmakerPreprintPrinter (everything printer-facing). The pre-print SnapmakerDeviceBridge profile calls these
// methods and turns the result into a page reply; nothing here knows about the webview.
//
// Every call that touches the printer is BLOCKING (HTTP). The bridge runs them through Options::preprint_run (off the UI thread in the app,
// inline in tests).

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "SnapmakerDeviceBridge.hpp"
#include "slic3r/Utils/SnapmakerPreprintGate.hpp"

namespace Slic3r { namespace GUI {

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

// A reply for the page. ok => code 200 with `data`; otherwise code + message.
struct SnapmakerPreprintReply
{
    bool           ok = false;
    int            code = SSWCPCode::Failure;
    std::string    message = "failure";
    nlohmann::json data = nlohmann::json::object();
    static SnapmakerPreprintReply success(nlohmann::json d = nlohmann::json::object()) { return {true, SSWCPCode::Ok, "OK", std::move(d)}; }
    static SnapmakerPreprintReply fail(std::string why) { return {false, SSWCPCode::Failure, std::move(why), nlohmann::json::object()}; }
};

constexpr const char* snapmaker_preprint_printer_states_ok[] = {"idle", "standby", "complete", "cancelled", "error"};

inline const char* snapmaker_preprint_decision_name(SnapmakerPreprintDecision d)
{
    using D = SnapmakerPreprintDecision;
    switch (d) {
    case D::Allowed: return "allowed";
    case D::RefusedNoContext: return "no pre-print context";
    case D::RefusedStaleEpoch: return "stale pre-print context";
    case D::RefusedClosed: return "pre-print context closed";
    case D::RefusedState: return "not allowed in this state";
    case D::RefusedInvalid: return "script not accepted";
    case D::RefusedTooManyScripts: return "too many scripts";
    case D::RefusedNotConfigured: return "send the preferences first";
    case D::RefusedAlreadyStarted: return "already started";
    case D::RefusedInFlight: return "another request is in progress";
    case D::RefusedFilename: return "not the pinned file";
    case D::RefusedPrinterChanged: return "the printer changed";
    case D::RefusedPrinterBusy: return "the printer is not idle";
    case D::RefusedMetadata: return "the printer's copy of the file does not match";
    }
    return "refused";
}

class SnapmakerPreprintSession
{
public:
    struct Open
    {
        SnapmakerPreprintOpen gate;            // pinned printer, remote file name, plate filaments, extruder count, snapshot size, configure_required
        std::string           capability_path; // PreprintCapabilityStore::Registration::url_path
        std::string           sha256_hex;      // of the snapshot (Registration::sha256_hex)
    };

    SnapmakerPreprintSession(std::shared_ptr<SnapmakerPreprintContext> context, std::shared_ptr<SnapmakerPreprintPrinter> printer)
        : m_context(std::move(context)), m_printer(std::move(printer))
    {}

    // Opens (or reopens: every earlier epoch becomes stale). Returns the epoch (> 0) the bridge must present; 0 with `error` set otherwise.
    // The remote file name is refused when it could not be a plain gcode name or could break out of SDCARD_PRINT_FILE FILENAME="...".
    std::uint64_t open(Open o, std::string& error)
    {
        if (!snapmaker_valid_gcode_name(o.gate.remote_filename) || o.gate.remote_filename.find('"') != std::string::npos) {
            m_gate.close();
            set_epoch(0);
            error = "the remote file name is not usable";
            return 0;
        }
        const std::uint64_t epoch = m_gate.open(o.gate, error);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_epoch = epoch;
        m_capability_path = epoch ? std::move(o.capability_path) : std::string();
        m_sha256_hex      = epoch ? std::move(o.sha256_hex) : std::string();
        m_size            = epoch ? o.gate.snapshot_size : 0;
        return epoch;
    }
    std::uint64_t epoch() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_epoch;
    }
    SnapmakerPreprintState state() const { return m_gate.state(); }

    // sw_GetActiveFile: the context's file name + metadata whitelist plus the capability URL (host + path), size and base64 sha256.
    // No filesystem path. Fails once closed, reopened or never opened.
    SnapmakerPreprintReply active_file(std::uint64_t epoch, const std::string& http_host) const
    {
        if (!live(epoch)) return SnapmakerPreprintReply::fail("no file is being prepared");
        nlohmann::json out;
        if (!m_context || !m_context->active_file(out)) return SnapmakerPreprintReply::fail("no file is being prepared");
        std::string path, sha;
        std::uint64_t size = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            path = m_capability_path;
            sha  = m_sha256_hex;
            size = m_size;
        }
        out["url"]         = http_host + path;
        out["origin_size"] = size;
        out["checksum"]    = snapmaker_base64(hex_to_bytes(sha));
        out["is_zip"]      = false;
        return SnapmakerPreprintReply::success(std::move(out));
    }
    SnapmakerPreprintReply filament_mapping(std::uint64_t epoch) const
    {
        nlohmann::json out;
        if (!live(epoch) || !m_context || !m_context->filament_mapping(out)) return SnapmakerPreprintReply::fail("no file is being prepared");
        return SnapmakerPreprintReply::success(std::move(out));
    }

    // sw_SendGCodes {script}: exactly one string, validated by the gate, probed on the printer, then sent.
    SnapmakerPreprintReply configure(std::uint64_t epoch, const nlohmann::json& params)
    {
        if (!params.is_object() || !params.contains("script") || !params["script"].is_string()) return SnapmakerPreprintReply::fail("failure");
        const std::string script = params["script"].get<std::string>();
        SnapmakerPreprintGcode parsed;
        SnapmakerPreprintResult r;
        try {
            r = m_gate.request_configure(epoch, script, m_printer->connected_pin(), [this] { return printer_idle(); }, &parsed);
        } catch (...) {
            return SnapmakerPreprintReply::fail("refused: the printer could not be asked");
        }
        if (!r.allowed()) return SnapmakerPreprintReply::fail(std::string("refused: ") + snapmaker_preprint_decision_name(r.decision));

        bool sent = false;
        std::string why = "the printer did not accept the script";
        try {
            std::string to_send = script;
            if (probe_script(script, parsed, to_send, why)) sent = m_printer->send_gcode(to_send);
        } catch (...) {
            sent = false;
        }
        const bool applied = m_gate.confirm(r.ticket, sent);
        if (!sent) return SnapmakerPreprintReply::fail("refused: " + why);
        if (!applied) return SnapmakerPreprintReply::fail("refused: pre-print context closed");
        return SnapmakerPreprintReply::success();
    }

    // sw_MachinePrintStart {filename}: one start of the pinned file.
    SnapmakerPreprintReply start(std::uint64_t epoch, const nlohmann::json& params)
    {
        if (!params.is_object() || !params.contains("filename") || !params["filename"].is_string()) return SnapmakerPreprintReply::fail("failure");
        const std::string filename = params["filename"].get<std::string>();
        SnapmakerPreprintResult r;
        try {
            r = m_gate.request_start(
                epoch, filename, m_printer->connected_pin(), [this] { return printer_idle(); },
                [this](const std::string& name, std::uint64_t size) {
                    const auto remote = m_printer->remote_file_size(name);
                    return remote && *remote == size;
                });
        } catch (...) {
            return SnapmakerPreprintReply::fail("refused: the printer could not be asked");
        }
        if (!r.allowed()) return SnapmakerPreprintReply::fail(std::string("refused: ") + snapmaker_preprint_decision_name(r.decision));
        bool started = false;
        try {
            started = m_printer->start_print(filename);
        } catch (...) {
            started = false;
        }
        const bool applied = m_gate.confirm(r.ticket, started);
        if (!started) return SnapmakerPreprintReply::fail("refused: the printer did not start the print");
        if (!applied) return SnapmakerPreprintReply::fail("refused: pre-print context closed");
        return SnapmakerPreprintReply::success();
    }

    // sw_StartLocalPrint: always -1 / "failure" so the page falls back to the validated gcode plus printer.print.start. A recorded
    // difference from Snapmaker (docs/u1-differences-from-snapmaker.md). Touches nothing.
    static SnapmakerPreprintReply start_local_print() { return SnapmakerPreprintReply::fail("failure"); }

    // sw_SetFilamentMappingComplete / sw_FinishFilamentMapping: the page is done. Closes the gate and clears the context. Returns whether
    // the print had been started.
    bool finish()
    {
        const bool started = m_gate.state() == SnapmakerPreprintState::Started;
        m_gate.close();
        if (m_context) m_context->clear();
        return started;
    }

private:
    void set_epoch(std::uint64_t e)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_epoch = e;
    }
    bool live(std::uint64_t epoch) const
    {
        const auto s = m_gate.state();
        return epoch != 0 && epoch == this->epoch() && (s == SnapmakerPreprintState::Open || s == SnapmakerPreprintState::Configured || s == SnapmakerPreprintState::Started);
    }
    bool printer_idle()
    {
        const auto state = m_printer->print_state();
        if (!state) return false;
        for (const char* ok : snapmaker_preprint_printer_states_ok)
            if (*state == ok) return true;
        return false;
    }
    // Asks the printer about every command of the validated script (a real /printer/gcode/help read each time). A map or used command
    // that is unknown or unreadable refuses the script; an unknown preference command only drops the preference part (#118: the map is
    // still applied); an unreadable one refuses (fail closed).
    bool probe_script(const std::string& script, const SnapmakerPreprintGcode& g, std::string& to_send, std::string& why)
    {
        for (const char* cmd : {"SET_PRINT_EXTRUDER_MAP", "SET_PRINT_USED_EXTRUDERS"}) {
            const auto k = m_printer->command_known(cmd);
            if (k != SnapmakerPreprintPrinter::Known::Yes) {
                why = std::string(k == SnapmakerPreprintPrinter::Known::No ? "the printer has no " : "could not tell whether the printer has ") + cmd;
                return false;
            }
        }
        if (g.prefs.empty()) return true;
        const char* pref_cmd = g.shape == SnapmakerPreprintShape::Plural ? "SET_PRINT_PREFERENCES" : "SET_PRINT_PREFERENCE";
        const auto  k        = m_printer->command_known(pref_cmd);
        if (k == SnapmakerPreprintPrinter::Known::Unreadable) {
            why = std::string("could not tell whether the printer has ") + pref_cmd;
            return false;
        }
        if (k == SnapmakerPreprintPrinter::Known::No) { // keep the map + used lines only (no trailing newline, like print_uploaded_file)
            const std::size_t at = script.find("SET_PRINT_PREFERENCE");
            to_send = script.substr(0, at);
            while (!to_send.empty() && to_send.back() == '\n') to_send.pop_back();
        }
        return true;
    }
    static std::string hex_to_bytes(const std::string& hex)
    {
        auto nib = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
        std::string out;
        for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
            const int a = nib(hex[i]), b = nib(hex[i + 1]);
            if (a < 0 || b < 0) return {};
            out += char(a * 16 + b);
        }
        return out;
    }

    std::shared_ptr<SnapmakerPreprintContext> m_context;
    std::shared_ptr<SnapmakerPreprintPrinter> m_printer;
    SnapmakerPreprintGate                     m_gate;
    mutable std::mutex                        m_mutex;
    std::uint64_t                             m_epoch = 0;
    std::string                               m_capability_path, m_sha256_hex;
    std::uint64_t                             m_size = 0;
};

}} // namespace Slic3r::GUI
