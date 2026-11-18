#pragma once

// Send-flow slice S4b (docs/records/snapmaker-u1-send-flow-design.md section 3): the pre-print bridge profile's logic, wx-free.
// One SnapmakerPreprintSession per pre-print dialog. It joins the S4a gate (SnapmakerPreprintGate.hpp, which decides but never touches a
// printer) to an injected SnapmakerPreprintPrinter (everything printer-facing). The pre-print SnapmakerDeviceBridge profile calls these
// methods and turns the result into a page reply; nothing here knows about the webview.
//
// Every call that touches the printer is BLOCKING (HTTP). The bridge runs them through Options::preprint_run (off the UI thread in the app,
// inline in tests).

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <boost/log/trivial.hpp>

#include "PreprintCapabilityStore.hpp"
#include "SnapmakerDeviceBridge.hpp"
#include "slic3r/Utils/SnapmakerPreprintGate.hpp"
#include "slic3r/Utils/SnapmakerPreprintPrinter.hpp" // the printer-facing interface (implemented by MoonrakerPreprintPrinter)

namespace Slic3r { namespace GUI {

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
    case D::RefusedStartDisallowed: return "upload only: nothing is sent to the printer";
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
    // The remote file name is refused unless snapmaker_preprint_filename_ok() (allowlist; it also goes into SDCARD_PRINT_FILE FILENAME="...").
    std::uint64_t open(Open o, std::string& error)
    {
        if (!snapmaker_preprint_filename_ok(o.gate.remote_filename)) {
            m_gate.close();
            set_epoch(0);
            error = "the remote file name is not usable";
            return 0;
        }
        if (!capability_path_ok(o.capability_path) || !sha256_hex_ok(o.sha256_hex)) { // F4
            m_gate.close();
            set_epoch(0);
            error = "the capability path or the checksum is not usable";
            return 0;
        }
        const std::uint64_t epoch = m_gate.open(o.gate, error);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_epoch = epoch;
        m_capability_path = epoch ? std::move(o.capability_path) : std::string();
        m_sha256_hex      = epoch ? std::move(o.sha256_hex) : std::string();
        m_size            = epoch ? o.gate.snapshot_size : 0;
        m_started_forwarded = false;
        m_accepted_script.clear();
        m_accepted_data = nlohmann::json::object();
        return epoch;
    }
    // A non-modal, user-visible notice (the plater's notification manager, wired in PreprintDialog.cpp). Called on the thread that
    // handles sw_SendGCodes; the owner's callback must hand off to the UI thread itself. Does not change what is sent or started.
    void set_notifier(std::function<void(const std::string&)> notifier)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_notifier = std::move(notifier);
    }
    std::uint64_t epoch() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_epoch;
    }
    SnapmakerPreprintState state() const { return m_gate.state(); }
    // The pinned printer while `epoch` is live (the one printer the send flow targets); nullopt otherwise.
    std::optional<SnapmakerPreprintPin> pinned_printer(std::uint64_t epoch) const { return m_gate.pinned_printer(epoch); }
    // The context's machine model ("" if none), for the local-device record. No printer contact.
    std::string context_machine_model() const
    {
        nlohmann::json out;
        if (m_context && m_context->filament_mapping(out) && out.is_object() && out.contains("machine_model") && out["machine_model"].is_string())
            return out["machine_model"].get<std::string>();
        return {};
    }

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
        // #142: the page sends the same script up to three times (pre-print configuration, preferences, and again after our -1 on
        // sw_StartLocalPrint). An exact repeat of the script the printer was already given is answered ok without spending the budget and
        // WITHOUT forwarding it a second time. Only an exact string match to a script that passed every gate and was forwarded (or
        // deliberately dropped under #141) counts; anything else takes the full path below.
        if (auto again = repeat_of_accepted(epoch, script)) return *again;
        // F6: the cheap refusals first, so a page that keeps calling after Closed (or with a bad script) causes no printer traffic.
        if (const auto d = m_gate.precheck_configure(epoch, script); d != SnapmakerPreprintDecision::Allowed)
            return SnapmakerPreprintReply::fail(std::string("refused: ") + snapmaker_preprint_decision_name(d));
        try {
            r = m_gate.request_configure(epoch, script, m_printer->connected_pin(), [this] { return printer_idle(); }, &parsed);
        } catch (...) {
            return SnapmakerPreprintReply::fail("refused: the printer could not be asked");
        }
        if (!r.allowed()) return SnapmakerPreprintReply::fail(std::string("refused: ") + snapmaker_preprint_decision_name(r.decision));

        bool sent = false;
        bool dropped = false;
        bool identity_assumed = false;
        std::string why = "the printer did not accept the script";
        try {
            std::string to_send = script;
            // #141: when every line was left out (identity map, firmware without the map commands) there is nothing to forward: answer ok.
            if (probe_script(script, parsed, to_send, why, dropped, identity_assumed)) sent = to_send.empty() ? true : m_printer->send_gcode(to_send);
        } catch (...) {
            sent = false;
        }
        // #142: recorded BEFORE confirm(): the gate only reaches Configured (which repeat_configure_ok() requires) inside confirm(), so a
        // repeat can never see a gap in which the script is accepted but not yet recorded (it would otherwise be forwarded twice).
        nlohmann::json data = nlohmann::json::object();
        if (dropped) {
            data["dropped_preferences"] = true; // F7: the printer has no preference command: only the map and used lines were sent
            // Snapmaker's page sends the preferences without checking and says nothing; we tell the user they were not applied (#119).
            std::function<void(const std::string&)> notifier;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                notifier = m_notifier;
            }
            if (notifier)
                notifier("The printer does not know the print-preference command (SET_PRINT_PREFERENCES), so the time-lapse, bed-leveling and "
                         "flow-calibration choices were not applied. The print itself is not affected.");
        }
        if (identity_assumed) {
            data["identity_map_assumed"] = true; // #141
            data["note"]                 = snapmaker_identity_map_note();
            std::function<void(const std::string&)> notifier;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                notifier = m_notifier;
            }
            if (notifier) notifier(snapmaker_identity_map_note());
        }
        if (sent) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_accepted_script = script;
            m_accepted_data   = data;
        }
        const bool applied = m_gate.confirm(r.ticket, sent);
        if (!sent) return SnapmakerPreprintReply::fail("refused: " + why);
        // F2: the printer HAS the script; a context closed meanwhile does not undo that, so this is not "refused".
        if (!applied) data["context_closed"] = true;
        return SnapmakerPreprintReply::success(std::move(data));
    }

    // sw_MachinePrintStart {filename}: one start of the pinned file.
    SnapmakerPreprintReply start(std::uint64_t epoch, const nlohmann::json& params)
    {
        if (!params.is_object() || !params.contains("filename") || !params["filename"].is_string()) return SnapmakerPreprintReply::fail("failure");
        const std::string filename = params["filename"].get<std::string>();
        SnapmakerPreprintResult r;
        if (const auto d = m_gate.precheck_start(epoch, filename); d != SnapmakerPreprintDecision::Allowed) // F6, see configure()
            return SnapmakerPreprintReply::fail(std::string("refused: ") + snapmaker_preprint_decision_name(d));
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
        set_start_forwarding(true);
        try {
            started = m_printer->start_print(filename);
        } catch (...) {
            started = false;
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_start_forwarding = false;
            if (started) m_started_forwarded = true; // recorded even when the context was closed meanwhile
        }
        const bool applied = m_gate.confirm(r.ticket, started);
        if (!started) return SnapmakerPreprintReply::fail("refused: the printer did not start the print");
        nlohmann::json data = nlohmann::json::object();
        if (!applied) data["context_closed"] = true; // F2: started on the printer; the page just closed first
        return SnapmakerPreprintReply::success(std::move(data));
    }

    // sw_StartLocalPrint: always -1 / "failure" so the page falls back to the validated gcode plus printer.print.start. A recorded
    // difference from Snapmaker (docs/u1-differences-from-snapmaker.md). Touches nothing.
    static SnapmakerPreprintReply start_local_print() { return SnapmakerPreprintReply::fail("failure"); }

    // sw_SetFilamentMappingComplete / sw_FinishFilamentMapping, and the bridge's shutdown: the page (or host) is done. F2: bound to the
    // bridge's epoch: a stale bridge (the context was reopened) closes nothing and returns nullopt, so it must not fire preprint_done either.
    // Otherwise closes the gate (voiding in-flight tickets), clears the context and returns whether the print was started: true also when a
    // start request is on the wire right now (it may land; reporting "not started" then would be the dangerous answer).
    std::optional<bool> finish(std::uint64_t epoch)
    {
        if (epoch == 0 || epoch != this->epoch()) return std::nullopt;
        m_gate.close();
        if (m_context) m_context->clear();
        return started_forwarded();
    }
    // True when a start was sent to the printer and it answered cleanly, or one is being sent right now.
    bool started_forwarded() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_started_forwarded || m_start_forwarding;
    }

private:
    // #142: ok again for an exact repeat of the accepted script while the gate is Configured, idle (no request in flight) and live.
    // Contacts nothing and changes nothing (no budget, no forward, no notice).
    std::optional<SnapmakerPreprintReply> repeat_of_accepted(std::uint64_t epoch, const std::string& script)
    {
        nlohmann::json data;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (epoch == 0 || epoch != m_epoch || m_accepted_script.empty() || script != m_accepted_script) return std::nullopt;
            data = m_accepted_data;
        }
        if (m_gate.repeat_configure_ok(epoch) != SnapmakerPreprintDecision::Allowed) return std::nullopt;
        BOOST_LOG_TRIVIAL(info) << "SnapmakerPreprintSession: an identical repeat of the accepted script is answered ok and not sent again";
        return SnapmakerPreprintReply::success(std::move(data));
    }
    void set_epoch(std::uint64_t e)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_epoch = e;
    }
    void set_start_forwarding(bool v)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_start_forwarding = v;
    }
    static bool capability_path_ok(const std::string& p)
    {
        const std::string prefix = PreprintCapabilityStore::path_prefix;
        if (p.size() <= prefix.size() || p.size() > 1024 || p.compare(0, prefix.size(), prefix) != 0) return false;
        for (unsigned char c : p)
            if (c < 0x20 || c > 0x7e || c == '"' || c == '\\' || c == '\'' || c == '<' || c == '>') return false;
        return true;
    }
    static bool sha256_hex_ok(const std::string& h)
    {
        if (h.size() != 64) return false;
        for (unsigned char c : h)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
        return true;
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
    bool probe_script([[maybe_unused]] const std::string& script, const SnapmakerPreprintGcode& g, std::string& to_send, std::string& why, bool& dropped, bool& identity_assumed)
    {
        // #141: a map or used command the printer is KNOWN not to have is left out when the map is the identity; any other map, and any probe
        // that cannot tell, refuses the script. The lines of the commands the printer does have are still sent.
        std::vector<std::string> missing;
        for (const char* cmd : {"SET_PRINT_EXTRUDER_MAP", "SET_PRINT_USED_EXTRUDERS"}) {
            const auto k = m_printer->command_known(cmd);
            if (k == SnapmakerPreprintPrinter::Known::Yes) continue;
            if (k == SnapmakerPreprintPrinter::Known::No && snapmaker_extruder_map_pairs_are_identity(g.map)) {
                missing.push_back(cmd);
                continue;
            }
            why = std::string(k == SnapmakerPreprintPrinter::Known::No ? "the printer has no " : "could not tell whether the printer has ") + cmd;
            if (k == SnapmakerPreprintPrinter::Known::No) why += ", and the map is not the identity so it cannot be left out";
            return false;
        }
        if (!missing.empty()) {
            identity_assumed = true;
            BOOST_LOG_TRIVIAL(warning) << "SnapmakerPreprintSession: the printer has no " << missing.front() << ", the identity map is assumed and its lines are not sent";
            to_send = snapmaker_script_without_commands(to_send, missing);
        }
        if (g.prefs.empty()) return true;
        const char* pref_cmd = g.shape == SnapmakerPreprintShape::Plural ? "SET_PRINT_PREFERENCES" : "SET_PRINT_PREFERENCE";
        const auto  k        = m_printer->command_known(pref_cmd);
        if (k == SnapmakerPreprintPrinter::Known::Unreadable) {
            why = std::string("could not tell whether the printer has ") + pref_cmd;
            return false;
        }
        if (k == SnapmakerPreprintPrinter::Known::No) { // F7: reported in the reply data and logged
            dropped = true;
            BOOST_LOG_TRIVIAL(warning) << "SnapmakerPreprintSession: the printer has no " << pref_cmd << ", the print preferences are not applied (the map is)";
            // keep the map + used lines only (no trailing newline, like print_uploaded_file)
            const std::size_t at = to_send.find("SET_PRINT_PREFERENCE");
            to_send = to_send.substr(0, at);
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

    std::function<void(const std::string&)>   m_notifier; // guarded by m_mutex
    std::shared_ptr<SnapmakerPreprintContext> m_context;
    std::shared_ptr<SnapmakerPreprintPrinter> m_printer;
    SnapmakerPreprintGate                     m_gate;
    mutable std::mutex                        m_mutex;
    std::uint64_t                             m_epoch = 0;
    std::string                               m_capability_path, m_sha256_hex;
    std::uint64_t                             m_size = 0;
    std::string                               m_accepted_script;   // #142, guarded by m_mutex: the last script that passed the gate and was forwarded/dropped
    nlohmann::json                            m_accepted_data = nlohmann::json::object(); // its reply data (notes, without context_closed)
    bool                                      m_started_forwarded = false, m_start_forwarding = false;
};

}} // namespace Slic3r::GUI
