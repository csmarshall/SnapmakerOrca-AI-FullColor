#ifndef slic3r_SpoolmanLink_hpp_
#define slic3r_SpoolmanLink_hpp_

// The value of the filament option `filament_spoolman_link`: ties a user filament preset to a Spoolman filament. Stored as JSON text
// {"id":N,"external_id":"...","sources":{"key":"source",...}} so it travels in presets and the project 3MF like any option.
// Pure: no network, no GUI.
//
// Rules (parse never throws; anything it rejects gives std::nullopt):
//  - no embedded NUL byte (the JSON library would stop reading there and ignore the rest);
//  - at most kMaxLinkBytes (4096) bytes of text, nesting depth at most kMaxLinkDepth (8), a JSON object;
//  - "id" is REQUIRED, an integer in 1..INT_MAX (same range as the catalog); missing or out of range => nullopt;
//  - "external_id" optional, a string; missing or null => ""; a non-string => nullopt;
//  - "sources" optional, an object whose values are all strings; missing or null => empty; any other shape => nullopt;
//  - unknown top-level keys are ignored (forward compatibility; they are NOT written back by serialize).
// serialize() writes keys in a fixed order (nlohmann sorted), so serialize(parse(serialize(l))) == serialize(l).
// serialize() enforces the same 4096-byte cap: a link whose text would be longer gives std::nullopt ("too large"), so a written link
// can never silently stop loading and "too large" can never be mistaken for "no link" (the empty string). The caller keeps the old value.
//
// VERIFIED: by tests/libslic3r/test_spoolman_link.cpp. ASSUMED: the field names and "id required" are this project's choice, not a
// Spoolman format.
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace Slic3r { namespace Spoolman {

constexpr std::size_t kMaxLinkBytes = 4096;
constexpr int         kMaxLinkDepth = 8;

struct Link
{
    int                                spoolman_id = 0;
    std::string                        external_id;
    std::map<std::string, std::string> sources;

    bool operator==(const Link &o) const { return spoolman_id == o.spoolman_id && external_id == o.external_id && sources == o.sources; }
};

// nullopt = the text would exceed kMaxLinkBytes (too large); the empty string is never returned.
std::optional<std::string> serialize(const Link &link);
std::optional<Link>  parse(std::string_view text);

}} // namespace Slic3r::Spoolman

#endif
