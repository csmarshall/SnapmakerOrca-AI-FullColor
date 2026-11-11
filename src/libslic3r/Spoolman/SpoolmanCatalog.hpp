#ifndef slic3r_SpoolmanCatalog_hpp_
#define slic3r_SpoolmanCatalog_hpp_

// Pure parsing of Spoolman REST responses (/api/v1/vendor, /filament, /spool). No network, no GUI, no printer-specific code.
//
// VERIFIED against real captures in tests/data/spoolman/ (22 filaments, 30 spools): the {"result": [...]} envelope; optional
// fields that really are absent (settings_extruder_temp on 9 filaments, color_hex on 4); the same vendor name under different
// ids; extra values stored JSON-encoded as strings (e.g. "\"\""); spool.extra.card_uids as a comma separated list; spool.tags[].uid.
// ASSUMED (not seen in captures): a bare array body, explicit JSON null for a field, a null vendor, non-string extra values.
// Those are handled defensively and covered by hand-written inline JSON in the tests, not by captures.
//
// Behaviour notes:
//  - The parser does NO range or semantic validation of values: negative density, "#RRGGBB" colours, duplicate ids or duplicate keys
//    are passed through unchanged. Validation belongs to the mapping step (S2).
//  - Item ids must be integers in 1..INT_MAX, otherwise the whole list is Malformed (with the item index).
//  - A field of the wrong JSON type is treated as absent (nullopt/empty) and recorded once in `warnings`. Absent or null is silent.
//  - Extra values are only decoded when the raw text is at most kMaxExtraBytes long and nests at most kMaxExtraDepth levels, and only
//    a decoded JSON string counts as decoded; everything else keeps its raw text and is listed in `extras_undecoded`.
//  - ParseResult::ok() is true only for Ok; Empty is not ok (it is valid but carries no items).
//  - There is no size cap on the response body; that is the transport layer's job.

#include <nlohmann/json_fwd.hpp>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace Spoolman {

inline constexpr size_t kMaxExtraBytes = 4096;
inline constexpr int    kMaxExtraDepth = 8;

struct Vendor
{
    int         id = 0;
    std::string name;
    std::vector<std::string> warnings;
};

struct Filament
{
    int                   id = 0;
    std::string           name;
    std::optional<Vendor> vendor;
    std::string           material;           // empty when absent
    std::optional<std::string> color_hex;      // as given by Spoolman, no leading '#'
    std::optional<double> density;
    std::optional<double> diameter;
    std::optional<double> weight;
    std::optional<double> spool_weight;
    std::optional<double> price;
    std::optional<double> settings_extruder_temp; // absent is NOT 0
    std::optional<double> settings_bed_temp;
    std::optional<std::string> external_id;
    // Extra field values, JSON-decoded once (Spoolman stores each as a JSON-encoded string).
    std::map<std::string, std::string> extras;
    // Keys of `extras` whose stored text could not be decoded; the value is then the raw text.
    std::set<std::string> extras_undecoded;
    std::vector<std::string> warnings;  // wrong-typed fields that were ignored
};

struct Spool
{
    int                      id = 0;
    std::optional<int>       filament_id;      // from the embedded filament object
    std::optional<double>    remaining_weight;
    bool                     archived = false;
    std::vector<std::string> card_uids;        // from extra.card_uids, trimmed, empties dropped, case as given
    std::vector<std::string> tag_uids;         // from tags[].uid, case as given
    std::map<std::string, std::string> extras;
    std::set<std::string>    extras_undecoded;
    std::vector<std::string> warnings;
};

enum class ParseStatus {
    Ok,          // one or more items
    Empty,       // valid response, zero items (never a reason to delete anything)
    Malformed,   // not JSON, or the wrong shape; see `reason`
    HttpFailure, // constructed by the transport layer, never by the parsers
};

template<class T> struct ParseResult
{
    ParseStatus    status = ParseStatus::Malformed;
    std::vector<T> items;
    std::string    reason;      // short, for Malformed / HttpFailure
    int            http_status = 0; // HttpFailure only; 0 = no response (connect failure, timeout)

    bool ok() const { return status == ParseStatus::Ok; }

    static ParseResult http_failure(int http_status, std::string reason)
    {
        ParseResult r;
        r.status      = ParseStatus::HttpFailure;
        r.http_status = http_status;
        r.reason      = std::move(reason);
        return r;
    }
};

// Each accepts the envelope {"result": [...]} or a bare array.
ParseResult<Vendor>   parse_vendors(const nlohmann::json &body);
ParseResult<Filament> parse_filaments(const nlohmann::json &body);
ParseResult<Spool>    parse_spools(const nlohmann::json &body);
ParseResult<Vendor>   parse_vendors_text(const std::string &body);
ParseResult<Filament> parse_filaments_text(const std::string &body);
ParseResult<Spool>    parse_spools_text(const std::string &body);

// Split a decoded card_uids value ("a,b") into trimmed, non-empty entries.
std::vector<std::string> split_uid_list(const std::string &value);

// Deepest [ / { nesting of JSON text, string-aware: brackets inside quoted strings (with backslash escapes) do not count, so a decoded
// string full of '[' has depth 0 while '["]",["]",...' has its real depth. An unterminated string hides the rest of the text, which
// cannot contain structure anyway. Used to reject hostile extras before a full parse.
int max_nesting(const std::string &raw);

// ASCII case-insensitive UID comparison (Spoolman keeps whatever case the user typed).
bool uid_equal(const std::string &a, const std::string &b);

}} // namespace Slic3r::Spoolman

#endif
