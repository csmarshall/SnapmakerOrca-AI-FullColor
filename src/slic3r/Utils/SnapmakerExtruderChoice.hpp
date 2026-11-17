#pragma once

// Pure resolver and validator for the filament-to-extruder choice of a legacy U1 send (#120 slice 1). No GUI, no network.
// Index convention: filament and extruder indices are 0-based, as in build_snapmaker_extruder_map_script and
// PrintParams::ams_mapping (one int per project filament, -1 = not used). That the printer reads the same base is INFERRED from the page.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

#include <nlohmann/json.hpp>

#include "SnapmakerPrintPreference.hpp"

namespace Slic3r {

// A filament the plate uses. spoolman_id < 0 = no link.
struct SnapmakerUsedFilament
{
    int         index = 0; // project filament index, 0-based
    std::string type;      // e.g. "PLA"
    std::string colour;    // "#RRGGBB" or "#RRGGBBAA"
    int         spoolman_id = -1;
};

// What the printer reports for one tool (RFID / Spoolman join data, already available as plain values). An empty type and colour = nothing known.
struct SnapmakerSlotInfo
{
    std::string type;
    std::string colour;
    int         spoolman_id = -1;
};

inline std::string snapmaker_upper(std::string s)
{
    for (char& c : s)
        c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Squared Euclidean distance in RGB (alpha ignored). Returns -1 for text that is not #RRGGBB or #RRGGBBAA.
inline long snapmaker_colour_distance(const std::string& a, const std::string& b)
{
    auto parse = [](const std::string& s, int rgb[3]) {
        if ((s.size() != 7 && s.size() != 9) || s[0] != '#')
            return false;
        for (size_t i = 1; i < s.size(); ++i)
            if (!std::isxdigit(static_cast<unsigned char>(s[i])))
                return false;
        for (int c = 0; c < 3; ++c)
            rgb[c] = int(std::stoi(s.substr(1 + 2 * size_t(c), 2), nullptr, 16));
        return true;
    };
    int x[3], y[3];
    if (!parse(a, x) || !parse(b, y))
        return -1;
    long d = 0;
    for (int c = 0; c < 3; ++c)
        d += long(x[c] - y[c]) * long(x[c] - y[c]);
    return d;
}

struct SnapmakerExtruderSuggestion
{
    std::vector<int> map;             // per project filament (size = highest used index + 1), -1 = not used
    bool             oversubscribed = false; // more used filaments than extruders: some extruder is shared
};

// Suggested pre-fill: for every (filament, slot) pair the cost is ordered (spool link differs, type differs, colour distance, not the identity
// slot, slot number); pairs are taken cheapest first, each filament and each slot once. Filaments left over (more used than slots) take their
// cheapest slot regardless and oversubscribed is set. Deterministic: ties fall to the lower filament index, then the lower slot.
// Unknown colour/type count as the worst match, so with no printer data the result is the identity.
inline SnapmakerExtruderSuggestion suggest_snapmaker_extruder_map(const std::vector<SnapmakerUsedFilament>& used, const std::vector<SnapmakerSlotInfo>& slots)
{
    SnapmakerExtruderSuggestion out;
    if (used.empty() || slots.empty())
        return out;
    int max_index = -1;
    for (const auto& f : used)
        max_index = std::max(max_index, f.index);
    if (max_index < 0)
        return out;
    out.map.assign(size_t(max_index) + 1, -1);

    using Cost = std::tuple<int, int, long, int, int, int, int>; // link, type, colour, identity, filament index, slot, filament position
    struct Pair { Cost cost; size_t f; int s; };
    std::vector<Pair> pairs;
    for (size_t f = 0; f < used.size(); ++f) {
        if (used[f].index < 0)
            continue;
        for (size_t s = 0; s < slots.size(); ++s) {
            const auto& u  = used[f];
            const auto& sl = slots[s];
            const bool  link_match = u.spoolman_id >= 0 && u.spoolman_id == sl.spoolman_id;
            const bool  type_match = !u.type.empty() && snapmaker_upper(u.type) == snapmaker_upper(sl.type);
            const long  d          = snapmaker_colour_distance(u.colour, sl.colour);
            pairs.push_back({Cost{link_match ? 0 : 1, type_match ? 0 : 1, d < 0 ? 3 * 255 * 255 + 1 : d, u.index == int(s) ? 0 : 1, u.index, int(s), int(f)}, f, int(s)});
        }
    }
    std::stable_sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.cost < b.cost; });
    std::vector<bool> slot_taken(slots.size(), false);
    std::vector<bool> done(used.size(), false);
    for (const auto& p : pairs) {
        if (done[p.f] || slot_taken[size_t(p.s)])
            continue;
        done[p.f]                       = true;
        slot_taken[size_t(p.s)]         = true;
        out.map[size_t(used[p.f].index)] = p.s;
    }
    for (const auto& p : pairs) { // leftovers share their cheapest slot
        if (done[p.f])
            continue;
        done[p.f]                        = true;
        out.map[size_t(used[p.f].index)] = p.s;
        out.oversubscribed               = true;
    }
    return out;
}

enum class SnapmakerMapRefusal { None, Unchosen, InvalidIndex, Unmapped, OutOfRange, Duplicate };

struct SnapmakerMapValidation
{
    SnapmakerMapRefusal refusal        = SnapmakerMapRefusal::None;
    std::string         message;               // user-facing, set when refused
    bool                oversubscribed = false; // OK, but more used filaments than extruders, so some extruder is shared
    bool                ok() const { return refusal == SnapmakerMapRefusal::None; }
};

// map[f] = extruder for project filament f (-1 or missing = not chosen). Entries of unused filaments are ignored. An empty map, or one that
// chooses nothing, is "unchosen": the start is refused and nothing is guessed. Two used filaments on one extruder are refused unless there are
// more used filaments than extruders, and then every extruder must still be used as far as it can be (sharing only where forced).
inline SnapmakerMapValidation validate_snapmaker_extruder_map(const std::vector<int>& map, const std::vector<SnapmakerUsedFilament>& used,
                                                              const std::vector<SnapmakerSlotInfo>& slots)
{
    SnapmakerMapValidation v;
    auto refuse = [&v](SnapmakerMapRefusal r, std::string m) {
        v.refusal = r;
        v.message = std::move(m);
        return v;
    };
    const int extruders = std::min<int>(int(slots.size()), snapmaker_max_extruders);
    if (used.empty())
        return v;
    bool any_chosen = false;
    for (int e : map)
        any_chosen = any_chosen || e >= 0;
    if (!any_chosen)
        return refuse(SnapmakerMapRefusal::Unchosen, "No filament-to-extruder assignment was chosen. Choose an extruder for each filament and confirm before printing.");
    std::vector<int> count(size_t(std::max(extruders, 0)), 0);
    for (const auto& f : used) {
        if (f.index < 0 || f.index >= snapmaker_max_filaments)
            return refuse(SnapmakerMapRefusal::InvalidIndex, "Filament " + std::to_string(f.index + 1) + " is not a valid filament number.");
        const int e = size_t(f.index) < map.size() ? map[size_t(f.index)] : -1;
        if (e < 0)
            return refuse(SnapmakerMapRefusal::Unmapped, "Please select an extruder for filament " + std::to_string(f.index + 1) + ".");
        if (e >= extruders)
            return refuse(SnapmakerMapRefusal::OutOfRange, "Filament " + std::to_string(f.index + 1) + " is assigned to extruder " + std::to_string(e + 1) + ", which this printer does not have.");
        ++count[size_t(e)];
    }
    const int used_count = int(used.size());
    int       distinct   = 0;
    for (int c : count)
        distinct += c > 0 ? 1 : 0;
    if (distinct < std::min(used_count, extruders))
        return refuse(SnapmakerMapRefusal::Duplicate, "Two filaments are assigned to the same extruder. Give each filament its own extruder.");
    v.oversubscribed = used_count > extruders;
    return v;
}

struct SnapmakerAmsMappingResult
{
    std::string json;  // PrintParams::ams_mapping: "[2,0,3]"; empty when refused
    std::string error; // user-facing, set when refused
    bool        ok() const { return error.empty(); }
};

// Converts a confirmed, valid map into PrintParams::ams_mapping. Unused filaments become -1. A map that does not validate gives no JSON
// (so an unchosen map can never reach the agent as an identity or empty mapping).
inline SnapmakerAmsMappingResult snapmaker_map_to_ams_mapping(const std::vector<int>& map, const std::vector<SnapmakerUsedFilament>& used,
                                                              const std::vector<SnapmakerSlotInfo>& slots)
{
    SnapmakerAmsMappingResult r;
    const SnapmakerMapValidation v = validate_snapmaker_extruder_map(map, used, slots);
    if (!v.ok() || used.empty()) {
        r.error = v.ok() ? "No filament is used by this plate." : v.message;
        return r;
    }
    int max_index = 0;
    for (const auto& f : used)
        max_index = std::max(max_index, f.index);
    std::vector<int> out(size_t(max_index) + 1, -1);
    for (const auto& f : used)
        out[size_t(f.index)] = map[size_t(f.index)];
    r.json = nlohmann::json(out).dump();
    return r;
}

// ---- #120 fix round: are two printer addresses the same server? Compared as scheme + host + port (+ path) after normalising case, a missing
// scheme (http), the default port of the scheme and trailing slashes. Anything unparsable or empty is NOT the same (fail closed). ----
inline bool snapmaker_normalize_host(const std::string& in, std::string& out)
{
    std::string s;
    for (char c : in) { if (static_cast<unsigned char>(c) <= 0x20) return false; s += c; }
    if (s.empty()) return false;
    std::string scheme = "http";
    if (const auto at = s.find("://"); at != std::string::npos) {
        scheme = s.substr(0, at);
        s.erase(0, at + 3);
    }
    for (char& c : scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (scheme != "http" && scheme != "https") return false;
    std::string path;
    if (const auto slash = s.find('/'); slash != std::string::npos) { path = s.substr(slash); s.erase(slash); }
    while (!path.empty() && path.back() == '/') path.pop_back();
    if (s.find('@') != std::string::npos || s.empty()) return false; // userinfo is not a form either side uses
    std::string host = s, port;
    if (s.front() == '[') {
        const auto close = s.find(']');
        if (close == std::string::npos) return false;
        host = s.substr(0, close + 1);
        if (close + 1 < s.size()) { if (s[close + 1] != ':') return false; port = s.substr(close + 2); }
    } else if (const auto colon = s.rfind(':'); colon != std::string::npos) {
        host = s.substr(0, colon);
        port = s.substr(colon + 1);
    }
    if (host.empty() || host == "[]") return false;
    for (char c : port) if (c < '0' || c > '9') return false;
    if (port.size() > 5) return false;
    if (port.empty() || std::stoi(port) == (scheme == "https" ? 443 : 80)) port = scheme == "https" ? "443" : "80";
    else port = std::to_string(std::stoi(port));
    for (char& c : host) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    out = scheme + "://" + host + ":" + port + path;
    return true;
}

inline bool snapmaker_same_host(const std::string& a, const std::string& b)
{
    std::string na, nb;
    return snapmaker_normalize_host(a, na) && snapmaker_normalize_host(b, nb) && na == nb;
}

// ---- Storage root rule (#120 fix round 2): SDCARD_PRINT_FILE starts a file under the printer's "gcodes" root only. ----
// Before the upload: the chosen upload storage must be empty (Moonraker's default, gcodes) or exactly "gcodes".
inline bool snapmaker_upload_storage_ok(const std::string& storage) { return storage.empty() || storage == "gcodes"; }

// After the upload: the root the SERVER reported in item.root must be exactly "gcodes" (and the chosen storage must have been ok). Anything else
// (config, timelapse, empty, missing) means the file is not where the start looks, so nothing is started; `reason` says why.
inline bool snapmaker_stored_root_allows_start(const std::string& chosen_storage, const std::string& reply_root, std::string& reason)
{
    if (!snapmaker_upload_storage_ok(chosen_storage)) {
        reason = "the file was uploaded to a storage other than gcodes, so nothing was started";
        return false;
    }
    if (reply_root != "gcodes") {
        reason = reply_root.empty() ? "the printer did not report which storage root it stored the file in, so nothing was started"
                                    : "the printer stored the file in the '" + reply_root + "' root, not gcodes, so nothing was started";
        return false;
    }
    return true;
}

// ---- Legacy send decision (#120 slice 2): what the legacy send may do with a Snapmaker printer. Pure, fail closed. ----
enum class SnapmakerLegacySendKind {
    Unchanged,     // not a Snapmaker printer: the legacy upload (and its post action) behaves exactly as before
    UploadOnly,    // upload the file, never start (also the choice when no start was asked for)
    StartViaAgent, // start through the agent with ams_mapping (map + preferences + SDCARD_PRINT_FILE)
    Refuse         // a start was asked for but the confirmed map is missing or invalid: nothing is sent
};

struct SnapmakerLegacySendInput
{
    bool snapmaker_printer = false; // U1 / Snapmaker printer
    bool agent_present     = false; // a usable printer agent exists for the selected printer
    bool agent_connected   = false; // and it is connected
    bool start_requested   = false; // the user chose upload-and-print
    bool map_confirmed     = false; // the user confirmed the mapping control
    std::string agent_base_url;     // where the agent's start would go
    std::string upload_host;        // where the upload goes (the print host of the job)
    std::string upload_storage;     // the storage root chosen for the upload ("" = the default, gcodes)
    std::vector<int>                   map;
    std::vector<SnapmakerUsedFilament> used;
    std::vector<SnapmakerSlotInfo>     slots;
};

struct SnapmakerLegacySendDecision
{
    SnapmakerLegacySendKind kind = SnapmakerLegacySendKind::Refuse;
    std::string             ams_mapping;              // set only for StartViaAgent
    std::string             message;                  // user-facing, set for Refuse
    bool                    oversubscribed    = false; // StartViaAgent with some extruder shared: show the notice
    bool                    upload_only_notice = false; // a start was asked for but is not possible: tell the user it was upload only
};

inline SnapmakerLegacySendDecision decide_snapmaker_legacy_send(const SnapmakerLegacySendInput& in)
{
    SnapmakerLegacySendDecision d;
    if (!in.snapmaker_printer) {
        d.kind = SnapmakerLegacySendKind::Unchanged;
        return d;
    }
    if (!in.start_requested) {
        d.kind = SnapmakerLegacySendKind::UploadOnly;
        return d;
    }
    if (!in.agent_present || !in.agent_connected) {
        d.kind               = SnapmakerLegacySendKind::UploadOnly;
        d.upload_only_notice = true;
        d.message            = "The printer is not connected, so the file is uploaded but the print is not started. Connect the printer and start it from the Device page.";
        return d;
    }
    if (!snapmaker_same_host(in.agent_base_url, in.upload_host)) {
        // The upload and the start would reach different printers: never start one printer's file on another.
        d.kind               = SnapmakerLegacySendKind::UploadOnly;
        d.upload_only_notice = true;
        d.message            = "The connected printer is not the host this file is uploaded to, so the file is uploaded but the print is not started. Start it from the Device page.";
        return d;
    }
    if (!snapmaker_upload_storage_ok(in.upload_storage)) {
        d.message = "The print can only be started from the gcodes storage; choose gcodes or upload without printing.";
        return d;
    }
    if (!in.map_confirmed) {
        d.message = "Confirm the filament-to-extruder assignment before printing.";
        return d;
    }
    const SnapmakerMapValidation v = validate_snapmaker_extruder_map(in.map, in.used, in.slots);
    const SnapmakerAmsMappingResult r = snapmaker_map_to_ams_mapping(in.map, in.used, in.slots);
    if (!v.ok() || !r.ok()) {
        d.message = v.ok() ? r.error : v.message;
        return d;
    }
    d.kind           = SnapmakerLegacySendKind::StartViaAgent;
    d.ams_mapping    = r.json;
    d.oversubscribed = v.oversubscribed;
    return d;
}

} // namespace Slic3r
