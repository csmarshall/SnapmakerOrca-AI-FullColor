#pragma once

// Send-flow slice S5: turns the file name the user typed in PrintHostSendDialog into the remote name the pre-print gate accepts
// (snapmaker_preprint_filename_ok: [A-Za-z0-9._ -] and '/', ".gcode" suffix, no empty / "." / ".." segment, at most 512 bytes).
// Deterministic and total: the same input always gives the same output and the output always passes the allowlist. The name is only ever
// rewritten HERE, before the context opens, and the rewritten name is what is pinned, shown to the user and sent; the gate itself never rewrites.

#include <cctype>
#include <string>

#include "SnapmakerPreprintGate.hpp"

namespace Slic3r {

struct SnapmakerPreprintName
{
    std::string name;            // always passes snapmaker_preprint_filename_ok()
    bool        changed = false; // name differs from the requested text (so the user must be told)
};

inline SnapmakerPreprintName snapmaker_preprint_sanitise_filename(const std::string& requested)
{
    static const std::string suffix = ".gcode";
    // 1. Per code point: allowlist characters stay, every other UTF-8 code point (or invalid byte) becomes one '_'. '\\' counts as a separator.
    std::string mapped;
    for (std::size_t i = 0; i < requested.size(); ++i) {
        const unsigned char c = requested[i];
        if (c >= 0x80 && c < 0xC0) continue; // continuation byte of a code point already replaced (or a stray one: dropped)
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == ' ' || c == '-' || c == '/';
        mapped += c == '\\' ? '/' : ok ? char(c) : '_';
    }
    // 2. Segments: trim spaces at both ends, drop empty / "." / ".." ones (no traversal, no absolute path, no empty segment).
    std::string joined;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= mapped.size(); ++i) {
        if (i < mapped.size() && mapped[i] != '/') continue;
        std::string seg = mapped.substr(start, i - start);
        start = i + 1;
        const auto b = seg.find_first_not_of(' ');
        seg = b == std::string::npos ? std::string() : seg.substr(b, seg.find_last_not_of(' ') - b + 1);
        if (seg.empty() || seg == "." || seg == "..") continue;
        if (!joined.empty()) joined += '/';
        joined += seg;
    }
    // 3. One ".gcode" suffix (case-insensitive match of an existing one), never doubled.
    if (joined.size() >= suffix.size()) {
        std::string tail = joined.substr(joined.size() - suffix.size());
        for (char& ch : tail) ch = char(std::tolower((unsigned char)ch));
        if (tail == suffix) joined.erase(joined.size() - suffix.size());
    }
    // Removing the suffix can leave trailing spaces or a trailing '/' ("dir/.gcode"); strip them, and give an empty stem a name.
    while (!joined.empty() && (joined.back() == ' ' || joined.back() == '/')) joined.pop_back();
    if (joined.empty()) joined = "print";
    // 4. Length: keep the front, the suffix always fits.
    const std::size_t max_stem = 512 - suffix.size();
    if (joined.size() > max_stem) {
        joined.resize(max_stem);
        while (!joined.empty() && (joined.back() == ' ' || joined.back() == '/')) joined.pop_back();
        if (joined.empty()) joined = "print";
    }
    SnapmakerPreprintName out;
    out.name = joined + suffix;
    if (!snapmaker_preprint_filename_ok(out.name)) out.name = "print" + suffix; // unreachable by construction; fail closed to a fixed name
    out.changed = out.name != requested;
    return out;
}

// The part of a remote name a PreprintCapabilityStore display name may carry (letters, digits . _ - + ( ) only, 1..128, not "." / ".."):
// the last path segment with every other character (space included) as '_'. The capability URL path is not the printer-side name.
inline std::string snapmaker_preprint_capability_display_name(const std::string& remote)
{
    const auto  slash = remote.find_last_of('/');
    std::string seg   = slash == std::string::npos ? remote : remote.substr(slash + 1);
    for (char& c : seg) {
        const unsigned char u = c;
        if (!((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '.' || u == '_' || u == '-')) c = '_';
    }
    if (seg.size() > 128) seg = seg.substr(seg.size() - 128);
    if (seg.empty() || seg == "." || seg == "..") seg = "print.gcode";
    return seg;
}

} // namespace Slic3r
