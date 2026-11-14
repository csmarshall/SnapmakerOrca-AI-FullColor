#pragma once

// Send-flow slice S4a (docs/records/snapmaker-u1-send-flow-design.md section 3): pure, wx-free and printer-free.
//   1. validate_snapmaker_preprint_gcode: the ONLY gcode strings the vendored pre-print page may pass through sw_SendGCodes while a
//      pre-print context is open. Two shapes, both read from the page JS (main.b347974c91352c2f.js 39805-39816, 148176-148187), never run:
//        plural   (R7, setMachinePreferencesConfig): N x "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=<i> MAP_EXTRUDER=<i>\n",
//                 "SET_PRINT_USED_EXTRUDERS EXTRUDERS=<i>(,<i>)*\n", then optionally ONE "SET_PRINT_PREFERENCES K=V( K=V)*" with no
//                 trailing newline and upper-case keys;
//        singular (aSb, setPrePrintConfiguration): the same map and used lines, then "SET_PRINT_PREFERENCE KEY=<k> VALUE=<v>" lines;
//                 all lines joined by "\n", no trailing newline, at least one preference line.
//      A script of map + used lines only, each "\n"-terminated (no preference line), IS accepted and labelled Plural.
//      Not accepted (fail closed; unverified whether the page ever sends them): a script without a map or used line (preferences only),
//      map + used with no trailing newline, a trailing newline after a preference line, a blank line.
//   2. SnapmakerPreprintGate: Idle -> Open -> Configured -> Started -> Closed under one mutex. It decides; it never touches a printer.
//      Printer facts (print_stats idle, metadata size) arrive as injected predicates, evaluated OUTSIDE the mutex (they may be slow),
//      with an in-flight flag so two racing callers cannot both pass; a close during evaluation wins.
// The goldens in the tests come from the builders in SnapmakerPrintPreference.hpp / SnapmakerPreprintMapping.hpp and from our reading
// of the page JS: they show agreement, not firmware behaviour.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "SnapmakerPreprintMapping.hpp"
#include "SnapmakerPrintPreference.hpp"

namespace Slic3r {

constexpr std::size_t snapmaker_preprint_max_gcode_bytes = 4096; // 32 map lines at 57 bytes is 1824; generous, still bounded
constexpr int         snapmaker_preprint_max_configures  = 2;    // the page's first script plus its one forced re-send

enum class SnapmakerPreprintShape { Plural, Singular };

struct SnapmakerPreprintGcode
{
    bool                                     ok = false;
    std::string                              error;
    SnapmakerPreprintShape                   shape = SnapmakerPreprintShape::Plural;
    std::vector<std::pair<int, int>>         map;   // CONFIG_EXTRUDER -> MAP_EXTRUDER, in script order
    std::vector<int>                         used;  // EXTRUDERS, in script order
    std::vector<std::pair<std::string, int>> prefs; // lower-case key, 0/1
};

// The pinned printer identity. host, port and serial (and SnapmakerPreprintOpen::remote_filename) are compared as EXACT strings: no case
// folding, no trailing-dot or port normalisation. The caller must canonicalise the pinned value and the "current" value it presents
// identically; a mismatch fails closed (refused).
struct SnapmakerPreprintPin
{
    std::string host, port, serial;
    bool operator==(const SnapmakerPreprintPin& o) const { return host == o.host && port == o.port && serial == o.serial; }
    bool operator!=(const SnapmakerPreprintPin& o) const { return !(*this == o); }
};

// The remote name of the pre-print file, which also goes into SDCARD_PRINT_FILE FILENAME="...": characters [A-Za-z0-9._ -] and '/' only, the
// ".gcode" suffix, no empty / "." / ".." segment (so no leading or trailing '/'), at most 512 bytes. Stricter than the Device tab's name check on
// purpose (no quote, backslash, '#', ';', '=', control characters, non-ASCII).
inline bool snapmaker_preprint_filename_ok(const std::string& n)
{
    static const std::string suffix = ".gcode";
    if (n.size() <= suffix.size() || n.size() > 512 || n.compare(n.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= n.size(); ++i) {
        if (i < n.size()) {
            const unsigned char c = n[i];
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == ' ' || c == '-' || c == '/';
            if (!ok) return false;
        }
        if (i == n.size() || n[i] == '/') {
            const std::string seg = n.substr(start, i - start);
            if (seg.empty() || seg == "." || seg == "..") return false;
            start = i + 1;
        }
    }
    return true;
}

namespace preprint_gate_detail {

// Decimal digits only, no sign, no leading zero, at most 2 digits.
inline bool parse_uint(const std::string& s, int& out)
{
    if (s.empty() || s.size() > 2 || (s.size() > 1 && s[0] == '0'))
        return false;
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    out = v;
    return true;
}
inline bool starts_with(const std::string& s, const char* p) { return s.compare(0, std::char_traits<char>::length(p), p) == 0; }
inline std::vector<std::string> split(const std::string& s, char sep)
{
    std::vector<std::string> out;
    std::size_t              pos = 0;
    for (std::size_t i; (i = s.find(sep, pos)) != std::string::npos; pos = i + 1)
        out.push_back(s.substr(pos, i - pos));
    out.push_back(s.substr(pos));
    return out;
}
inline bool pref_key_allowed(const std::string& lower)
{
    for (const char* k : snapmaker_print_preference_keys)
        if (lower == k) return true;
    return false;
}
inline std::string to_lower(std::string s)
{
    for (char& c : s) c = char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return s;
}
inline std::string to_upper(std::string s)
{
    for (char& c : s) c = char(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
    return s;
}

} // namespace preprint_gate_detail

// `plate_filaments` = the project filament indices the plate uses (from the open context); `extruder_count` = printer extruders.
inline SnapmakerPreprintGcode validate_snapmaker_preprint_gcode(const std::string& gcode, const std::vector<int>& plate_filaments, int extruder_count)
{
    using namespace preprint_gate_detail;
    SnapmakerPreprintGcode r;
    auto fail = [&r](const std::string& why) {
        r = SnapmakerPreprintGcode{};
        r.error = why;
        return r;
    };
    if (extruder_count < 1 || extruder_count > snapmaker_max_extruders) return fail("invalid extruder count");
    if (gcode.empty()) return fail("empty script");
    if (gcode.size() > snapmaker_preprint_max_gcode_bytes) return fail("script too long");
    for (unsigned char c : gcode)
        if (c != '\n' && (c < 0x20 || c > 0x7e)) return fail("control or non-ASCII character");
    if (gcode.find(';') != std::string::npos) return fail("comment character");

    std::vector<std::string> lines = split(gcode, '\n');
    const bool               terminated = lines.back().empty(); // text ended in "\n"
    if (terminated) lines.pop_back();
    if (lines.empty()) return fail("no lines");
    for (const auto& l : lines)
        if (l.empty()) return fail("blank line");

    // Shape: decided by the first preference line, if any.
    std::size_t first_pref = lines.size();
    for (std::size_t i = 0; i < lines.size(); ++i)
        if (starts_with(lines[i], "SET_PRINT_PREFERENCE")) { first_pref = i; break; }
    if (first_pref == lines.size()) {
        if (!terminated) return fail("script without preferences must end with a newline");
        r.shape = SnapmakerPreprintShape::Plural;
    } else {
        r.shape = starts_with(lines[first_pref], "SET_PRINT_PREFERENCES ") ? SnapmakerPreprintShape::Plural : SnapmakerPreprintShape::Singular;
        if (r.shape == SnapmakerPreprintShape::Plural) {
            if (terminated || first_pref + 1 != lines.size()) return fail("the plural preference line must be last, once, with no newline");
        } else if (terminated) {
            return fail("trailing newline after the preferences");
        }
    }
    // map lines + used line
    if (first_pref < 2) return fail("a map line and a used-extruders line are required");
    const std::size_t used_idx = first_pref - 1;
    std::set<int>     config_set, map_values;
    for (std::size_t i = 0; i < used_idx; ++i) {
        const std::string  prefix = "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=";
        const std::string& l = lines[i];
        if (!starts_with(l, prefix.c_str())) return fail("unexpected command: " + l.substr(0, 40));
        const std::size_t mid = l.find(" MAP_EXTRUDER=", prefix.size());
        if (mid == std::string::npos) return fail("malformed map line");
        int k = 0, v = 0;
        if (!parse_uint(l.substr(prefix.size(), mid - prefix.size()), k) || !parse_uint(l.substr(mid + 14), v)) return fail("malformed map number");
        if (!config_set.insert(k).second) return fail("duplicate CONFIG_EXTRUDER");
        if (v >= extruder_count) return fail("MAP_EXTRUDER out of range");
        map_values.insert(v);
        r.map.emplace_back(k, v);
    }
    const std::set<int> plate(plate_filaments.begin(), plate_filaments.end());
    if (config_set != plate) return fail("CONFIG_EXTRUDER set differs from the plate's filaments");
    {
        const std::string  prefix = "SET_PRINT_USED_EXTRUDERS EXTRUDERS=";
        const std::string& l = lines[used_idx];
        if (!starts_with(l, prefix.c_str())) return fail("expected the used-extruders line");
        std::set<int> seen;
        for (const auto& tok : split(l.substr(prefix.size()), ',')) {
            int e = 0;
            if (!parse_uint(tok, e)) return fail("malformed used-extruder number");
            if (!seen.insert(e).second) return fail("duplicate used extruder");
            r.used.push_back(e);
        }
        if (seen != map_values) return fail("USED_EXTRUDERS differs from the distinct MAP_EXTRUDER values");
    }
    // preferences
    std::set<std::string> keys;
    auto add_pref = [&](const std::string& key_lower, const std::string& val) -> bool {
        int v = 0;
        if (!pref_key_allowed(key_lower) || !parse_uint(val, v) || v > 1 || !keys.insert(key_lower).second) return false;
        r.prefs.emplace_back(key_lower, v);
        return true;
    };
    if (first_pref < lines.size()) {
        if (r.shape == SnapmakerPreprintShape::Plural) {
            const std::string prefix = "SET_PRINT_PREFERENCES ";
            for (const auto& kv : split(lines[first_pref].substr(prefix.size()), ' ')) {
                const std::size_t eq = kv.find('=');
                if (eq == std::string::npos) return fail("malformed preference");
                const std::string key = kv.substr(0, eq);
                if (key != to_upper(key) || !add_pref(to_lower(key), kv.substr(eq + 1))) return fail("preference rejected: " + kv.substr(0, 40));
            }
        } else {
            for (std::size_t i = first_pref; i < lines.size(); ++i) {
                const std::string prefix = "SET_PRINT_PREFERENCE KEY=";
                const std::string& l = lines[i];
                const std::size_t mid = l.find(" VALUE=");
                if (!starts_with(l, prefix.c_str()) || mid == std::string::npos) return fail("malformed preference line");
                if (!add_pref(l.substr(prefix.size(), mid - prefix.size()), l.substr(mid + 7))) return fail("preference rejected: " + l.substr(0, 60));
            }
        }
    }
    r.ok = true;
    return r;
}

// The plural (R7) script as the page builds it: map lines and the used line (each "\n"-terminated), then at most one
// SET_PRINT_PREFERENCES line (upper-case keys, in the order given, no trailing newline). Built from our existing map builder.
inline std::string build_snapmaker_plural_preference_script(const std::vector<int>& mapping, const std::vector<std::pair<std::string, int>>& prefs)
{
    const SnapmakerExtruderMapResult m = build_snapmaker_extruder_map_script(mapping);
    if (!m.ok() || m.script.empty()) return {};
    std::string out = m.script + "\n";
    if (!prefs.empty()) {
        out += "SET_PRINT_PREFERENCES";
        char sep = ' ';
        for (const auto& kv : prefs) {
            out += sep + preprint_gate_detail::to_upper(kv.first) + "=" + std::to_string(kv.second);
            sep = ' ';
        }
    }
    return out;
}
// The singular (aSb) script: map + used lines, "\n", then the three SET_PRINT_PREFERENCE lines of the existing builder.
inline std::string build_snapmaker_singular_preference_script(const std::vector<int>& mapping, bool flow_calibrate, bool time_lapse, bool bed_level)
{
    const SnapmakerExtruderMapResult m = build_snapmaker_extruder_map_script(mapping);
    if (!m.ok() || m.script.empty()) return {};
    return m.script + "\n" + build_snapmaker_print_preference_script(flow_calibrate, time_lapse, bed_level);
}

enum class SnapmakerPreprintState { Idle, Open, Configured, Started, Closed };

enum class SnapmakerPreprintDecision {
    Allowed,
    RefusedNoContext,      // nothing open
    RefusedStaleEpoch,     // the request carries an epoch from an earlier open()
    RefusedClosed,
    RefusedState,          // wrong state for the request (e.g. a script after Started)
    RefusedInvalid,        // script failed validation
    RefusedTooManyScripts, // a third script
    RefusedNotConfigured,  // start before the script when the page's options are non-empty
    RefusedAlreadyStarted, // also: a start already in flight
    RefusedInFlight,       // another request is awaiting its confirm()
    RefusedFilename,
    RefusedPrinterChanged,
    RefusedPrinterBusy,    // print_stats not idle, a throwing predicate, or no predicate (fail closed)
    RefusedMetadata        // remote size differs from the snapshot, a throwing predicate, or no predicate (fail closed)
};

struct SnapmakerPreprintOpen
{
    SnapmakerPreprintPin  printer;
    std::string           remote_filename;
    std::vector<int>      plate_filaments;
    int                   extruder_count = 0;
    std::uint64_t         snapshot_size = 0;
    bool                  configure_required = false; // the page's options are non-empty: a validated script must precede the start
};

// Proof that a request was Allowed. The caller must forward the script / start to the printer IMMEDIATELY and then call
// SnapmakerPreprintGate::confirm(ticket, forward_succeeded). Until then the gate is "in flight" and refuses every other request.
struct SnapmakerPreprintTicket
{
    std::uint64_t epoch = 0;
    std::uint64_t op = 0;
};

struct SnapmakerPreprintResult
{
    SnapmakerPreprintDecision decision = SnapmakerPreprintDecision::RefusedNoContext;
    SnapmakerPreprintTicket   ticket; // valid only when decision == Allowed
    bool                      allowed() const { return decision == SnapmakerPreprintDecision::Allowed; }
};

class SnapmakerPreprintGate
{
public:
    using IdlePredicate     = std::function<bool()>;
    using MetadataPredicate = std::function<bool(const std::string& filename, std::uint64_t expected_size)>;
    using D                 = SnapmakerPreprintDecision;

    // Validates and opens. Returns the epoch (> 0) every later request must present; 0 with `error` set and the gate left Idle when the
    // parameters are unusable.
    std::uint64_t open(SnapmakerPreprintOpen o, std::string& error)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_epoch;
        ++m_op;
        m_state = SnapmakerPreprintState::Idle;
        m_open = SnapmakerPreprintOpen{};
        m_in_flight = m_start_in_flight = false;
        m_configures = 0;
        error = validate_open(o);
        if (!error.empty()) return 0;
        m_open = std::move(o);
        m_state = SnapmakerPreprintState::Open;
        return m_epoch;
    }
    // Invalidate / close from any state; an outstanding ticket becomes void.
    void close()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_state != SnapmakerPreprintState::Idle) m_state = SnapmakerPreprintState::Closed;
        ++m_op;
        m_in_flight = m_start_in_flight = false;
    }
    SnapmakerPreprintState state() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_state;
    }

    // F6: the cheap refusals of request_configure / request_start (no printer data needed), so the caller can refuse a closed, stale, busy or
    // malformed request WITHOUT reading the printer. Pure: changes nothing. Allowed here does not mean request_* will allow.
    SnapmakerPreprintDecision precheck_configure(std::uint64_t epoch, const std::string& script) const
    {
        std::vector<int> filaments;
        int              extruders = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (auto d = state_refusal(epoch, /*start=*/false); d != D::Allowed) return d;
            if (m_configures >= snapmaker_preprint_max_configures) return D::RefusedTooManyScripts;
            filaments = m_open.plate_filaments;
            extruders = m_open.extruder_count;
        }
        return validate_snapmaker_preprint_gcode(script, filaments, extruders).ok ? D::Allowed : D::RefusedInvalid;
    }
    SnapmakerPreprintDecision precheck_start(std::uint64_t epoch, const std::string& filename) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (auto d = state_refusal(epoch, /*start=*/true); d != D::Allowed) return d;
        if (m_open.configure_required && m_state != SnapmakerPreprintState::Configured) return D::RefusedNotConfigured;
        if (filename.empty() || filename != m_open.remote_filename) return D::RefusedFilename;
        return D::Allowed;
    }

    // Validates one sw_SendGCodes string. Allowed = forward `script` now, then confirm(). Nothing is sent here. A throwing predicate
    // is a refusal. The state becomes Configured only on confirm(ticket, true).
    SnapmakerPreprintResult request_configure(std::uint64_t epoch, const std::string& script, const SnapmakerPreprintPin& current_printer,
                                              const IdlePredicate& idle, SnapmakerPreprintGcode* parsed = nullptr)
    {
        std::vector<int> filaments;
        int              extruders = 0;
        std::uint64_t    op = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (auto d = state_refusal(epoch, /*start=*/false); d != D::Allowed) return {d, {}};
            if (m_configures >= snapmaker_preprint_max_configures) return {D::RefusedTooManyScripts, {}};
            if (current_printer != m_open.printer) return {D::RefusedPrinterChanged, {}};
            filaments = m_open.plate_filaments;
            extruders = m_open.extruder_count;
            m_in_flight = true;
            op = ++m_op;
        }
        SnapmakerPreprintGcode g = validate_snapmaker_preprint_gcode(script, filaments, extruders);
        const bool             idle_ok = g.ok && safe_call([&] { return idle && idle(); });
        std::lock_guard<std::mutex> lock(m_mutex);
        if (op != m_op) return {state_refusal_after_void(epoch, false), {}}; // closed or reopened meanwhile
        if (!g.ok || !idle_ok) {
            m_in_flight = false;
            return {!g.ok ? D::RefusedInvalid : D::RefusedPrinterBusy, {}};
        }
        if (parsed) *parsed = std::move(g);
        return {D::Allowed, {m_epoch, op}};
    }

    // One start of exactly the pinned file on the pinned printer. Allowed = forward the start now, then confirm(). Nothing is sent
    // here. Started only on confirm(ticket, true); confirm(ticket, false) returns to the earlier state with the start budget intact.
    SnapmakerPreprintResult request_start(std::uint64_t epoch, const std::string& filename, const SnapmakerPreprintPin& current_printer,
                                          const IdlePredicate& idle, const MetadataPredicate& metadata_matches)
    {
        std::string   expected;
        std::uint64_t size = 0;
        std::uint64_t op = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (auto d = state_refusal(epoch, /*start=*/true); d != D::Allowed) return {d, {}};
            if (m_open.configure_required && m_state != SnapmakerPreprintState::Configured) return {D::RefusedNotConfigured, {}};
            if (filename.empty() || filename != m_open.remote_filename) return {D::RefusedFilename, {}};
            if (current_printer != m_open.printer) return {D::RefusedPrinterChanged, {}};
            expected = m_open.remote_filename;
            size = m_open.snapshot_size;
            m_in_flight = m_start_in_flight = true;
            op = ++m_op;
        }
        const bool idle_ok = safe_call([&] { return idle && idle(); });
        const bool meta_ok = idle_ok && safe_call([&] { return metadata_matches && metadata_matches(expected, size); });
        std::lock_guard<std::mutex> lock(m_mutex);
        if (op != m_op) return {state_refusal_after_void(epoch, true), {}};
        if (!idle_ok || !meta_ok) {
            m_in_flight = m_start_in_flight = false;
            return {!idle_ok ? D::RefusedPrinterBusy : D::RefusedMetadata, {}};
        }
        return {D::Allowed, {m_epoch, op}};
    }

    // Reports whether forwarding succeeded. True when the ticket was current and the transition was applied (or, for a failure, rolled
    // back); false for a stale/void ticket (closed, reopened, already confirmed): nothing changes then.
    bool confirm(const SnapmakerPreprintTicket& t, bool forwarded_ok)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_in_flight || t.op != m_op || t.epoch != m_epoch || m_state == SnapmakerPreprintState::Closed || m_state == SnapmakerPreprintState::Idle)
            return false;
        const bool was_start = m_start_in_flight;
        m_in_flight = m_start_in_flight = false;
        if (forwarded_ok) {
            if (was_start) m_state = SnapmakerPreprintState::Started;
            else {
                ++m_configures;
                m_state = SnapmakerPreprintState::Configured;
            }
        }
        return true;
    }

private:
    static std::string validate_open(const SnapmakerPreprintOpen& o)
    {
        if (o.printer.host.empty() || o.printer.port.empty() || o.printer.serial.empty()) return "the pinned printer identity is incomplete";
        if (!snapmaker_preprint_filename_ok(o.remote_filename)) return "the remote file name is not usable";
        if (o.extruder_count < 1 || o.extruder_count > snapmaker_max_extruders) return "invalid extruder count";
        if (o.plate_filaments.empty()) return "the plate uses no filaments";
        std::set<int> seen;
        for (int f : o.plate_filaments)
            if (f < 0 || f >= snapmaker_max_filaments || !seen.insert(f).second) return "invalid or duplicate plate filament index";
        return {};
    }
    template<class F> static bool safe_call(F&& f)
    {
        try {
            return f();
        } catch (...) {
            return false;
        }
    }
    // Caller holds the mutex.
    SnapmakerPreprintDecision state_refusal(std::uint64_t epoch, bool start) const
    {
        if (epoch != m_epoch) return D::RefusedStaleEpoch;
        switch (m_state) {
        case SnapmakerPreprintState::Idle: return D::RefusedNoContext;
        case SnapmakerPreprintState::Closed: return D::RefusedClosed;
        case SnapmakerPreprintState::Started: return start ? D::RefusedAlreadyStarted : D::RefusedState;
        default: break;
        }
        if (m_start_in_flight) return D::RefusedAlreadyStarted;
        if (m_in_flight) return D::RefusedInFlight;
        return D::Allowed;
    }
    // After the context was closed/reopened during an evaluation: whatever the gate says now, never Allowed.
    SnapmakerPreprintDecision state_refusal_after_void(std::uint64_t epoch, bool start) const
    {
        const D d = state_refusal(epoch, start);
        return d == D::Allowed ? D::RefusedState : d;
    }

    mutable std::mutex     m_mutex;
    SnapmakerPreprintState m_state = SnapmakerPreprintState::Idle;
    SnapmakerPreprintOpen  m_open;
    int                    m_configures = 0;
    bool                   m_in_flight = false, m_start_in_flight = false;
    std::uint64_t          m_epoch = 0; // bumped by every open(); requests must present the one open() returned
    std::uint64_t          m_op = 0;    // bumped when a ticket is issued and on open/close, voiding tickets and evaluations in progress
};

} // namespace Slic3r
