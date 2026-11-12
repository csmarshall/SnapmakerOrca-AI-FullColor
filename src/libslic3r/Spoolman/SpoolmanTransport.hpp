#ifndef slic3r_SpoolmanTransport_hpp_
#define slic3r_SpoolmanTransport_hpp_

// Read-only transport seam for the Spoolman catalog (S7 of #95). No sockets, no GUI, no printer-specific code: the caller hands in an
// HttpGet, this layer enforces the hard limits BEFORE any JSON parsing and then calls the parsers from SpoolmanCatalog.hpp.
//
// Requests: only GET /v1/vendor, GET /v1/filament and GET /v1/spool?allow_archived=false. There is no write path in this header.
//
// Limits applied to every body before it reaches nlohmann (whose copy/destroy paths are recursive):
//  - at most kMaxBodyBytes (4 MiB), else Malformed("response too large")
//  - at most kMaxBodyDepth (16) levels of [ / { nesting, measured by an iterative, string-aware scan with early exit, else
//    Malformed("response too deep")
//  HTTP status != 200 => HttpFailure(status); status 0 (connect failure or timeout) => HttpFailure(0). Nothing here throws.
//  An empty list stays Empty: it is never an HTTP failure and must never be treated as a deletion signal by the caller.
//
// VERIFIED: the Moonraker proxy request shape and its version 2 response envelope, and /server/spoolman/status, were checked on
//   2026-11-11 against Moonraker's own docs (docs/external_api/integrations.md) and moonraker/components/spoolman.py
//   (_proxy_spoolman_request: methods GET/POST/PUT/PATCH/DELETE, path must start with "/v1/", a GET must not carry a body,
//   use_v2_response returns {"response":..., "error":null|{"status_code","message"}}). Real Moonraker HTTP replies wrap that in
//   {"result": ...}; the unwrap helpers accept both forms. Not verified against a live Moonraker.
// ASSUMED: Spoolman's `allow_archived` query parameter on /v1/spool (the documented default is false, the parameter is passed anyway);
//   archived spools that still arrive are dropped here as a second line of defence.
//
// The proxy is a POST to Moonraker carrying request_method "GET": read-only on Spoolman, and it changes no Moonraker state.

#include "SpoolmanCatalog.hpp"

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace Slic3r { namespace Spoolman {

inline constexpr size_t kMaxBodyBytes = 4u * 1024u * 1024u;
inline constexpr int    kMaxBodyDepth = 16;

// Sentinel statuses an HttpGet may return besides a real HTTP status (0 = connection failure or timeout):
inline constexpr int kStatusTooLarge = -1; // the transfer was aborted for exceeding the size limit => HttpFailure "response too large"
inline constexpr int kStatusNotV2    = -2; // a Moonraker proxy reply that is not the version 2 format => HttpFailure "proxy reply is not v2 format"

// Returns the HTTP status (0 = connection failure or timeout, or a sentinel above) and fills `body`. Must be read-only (GET).
using HttpGet = std::function<int(const std::string &path, const std::string &query, std::string &body)>;

// Whether Moonraker is connected to Spoolman; nullopt = could not be determined (treated as not connected).
using StatusFn = std::function<std::optional<bool>()>;

struct CatalogFetch
{
    bool attempted = false; // false: no request of any kind was made (see `skipped_reason`)
    std::string skipped_reason; // "preference off", "not connected", "status unknown"; empty when attempted
    ParseResult<Vendor>   vendors;
    ParseResult<Filament> filaments;
    ParseResult<Spool>    spools;

    // Every part is Ok or Empty. Only a good fetch may replace a cached one.
    bool good() const;
};

// Empty string when the body is within the limits, otherwise the reason ("response too large" / "response too deep").
std::string check_body_limits(const std::string &body);

// Fetches the three lists. Never throws. Archived spools are removed from `spools` (all removed => Empty).
CatalogFetch fetch_catalog(const HttpGet &get);

// Session cache keyed by printer (host or id). A good fetch is kept; invalidate() only marks it stale, so a later FAILED fetch never
// destroys the last good result (see last_good()). A failure is not cached as fresh: the next get() tries the network again.
// An invalidate() that happens while a get() is on the network is not lost: the result of that get() is stored but stays stale
// (generation counter, one counter for the whole cache).
class CatalogCache
{
public:
    // Injectable clock (default: std::chrono::steady_clock) so the fetch age is testable.
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    CatalogCache();
    explicit CatalogCache(Clock clock);

    CatalogFetch get(const std::string &key, const HttpGet &http);
    void         invalidate(const std::string &key);
    void         invalidate_all();
    // The most recent good fetch for the key, even if stale; nullopt if there never was one.
    std::optional<CatalogFetch> last_good(const std::string &key) const;
    // Seconds since the last good fetch for the key was stored (clamped to >= 0); nullopt if there never was one.
    std::optional<long long> last_good_age_seconds(const std::string &key) const;

private:
    struct Entry { CatalogFetch value; bool fresh = false; std::chrono::steady_clock::time_point fetched_at; };
    Clock                        m_clock;
    mutable std::mutex           m_mutex;
    unsigned long                m_generation = 0;
    std::map<std::string, Entry> m_entries;
};

// The single gate. With the preference off NOTHING is called (not the status function, not `http`). Not connected or unknown status:
// nothing is fetched. Returns attempted=false with the reason in those cases.
CatalogFetch fetch_catalog_if_enabled(bool preference_on, const StatusFn &status, CatalogCache &cache, const std::string &key,
                                      const HttpGet &http);

// ---- Moonraker proxy encoding (pure) ----
// Body for POST /server/spoolman/proxy. request_method is ALWAYS "GET" and there is never a "body" member.
std::string moonraker_proxy_request_body(const std::string &path, const std::string &query);
// Turns the HTTP reply of that POST into an HttpGet result: `body` receives the Spoolman payload text. A Moonraker-reported
// Spoolman error comes back as its status_code (502 when the error object has none). A reply that is not version 2 (for example an
// older Moonraker ignoring use_v2_response and answering {"result": ...}) or not JSON gives kStatusNotV2. A reply that breaks the body limits is passed through untouched (status 200) so that
// fetch_catalog() rejects it before parsing.
int moonraker_proxy_unwrap(int http_status, const std::string &reply, std::string &body);
// Parses the reply of GET /server/spoolman/status; nullopt when it is not understood.
std::optional<bool> moonraker_spoolman_connected(int http_status, const std::string &reply);


// ---- Direct Spoolman server transport (#107, pure) ----
// An OPTIONAL app-config key `spoolman_server_url` (empty = use the Moonraker proxy). The URL names the Spoolman server itself; the
// direct requests are GET <url>/api/v1/... only. There is no credential field: userinfo in the URL is rejected and no header is sent.
// ASSUMED (from Spoolman's documented REST layout, not checked against a live server): the API lives under /api on the server.
inline constexpr const char *kSpoolmanApiPrefix = "/api";
inline constexpr int         kMaxDirectRedirects = 2; // same-origin hops only

struct SpoolmanUrlCheck
{
    bool        ok = false;
    std::string url;    // normalised (scheme and host lower-cased, no trailing slash); empty when the input was empty (= unset)
    std::string reason; // why it was rejected; empty when ok
};
// Empty / whitespace-only input is ok with an empty url (the setting is unset). Accepts http and https with a host (name, IPv4 or
// [IPv6]), an optional port 1..65535 and an optional path prefix. Rejects other schemes, userinfo, query, fragment, empty host, bad
// port, spaces / control / non-ASCII characters, backslashes and anything over kMaxSpoolmanUrlLength.
inline constexpr size_t kMaxSpoolmanUrlLength = 255;
SpoolmanUrlCheck validate_spoolman_url(const std::string &input);

// One network GET with redirects DISABLED. Returns the HTTP status (0 = failure, kStatusTooLarge) and fills body; `location` receives the
// Location header of a 3xx reply (empty otherwise). There is no method parameter: it can only ever be a GET.
using DirectFetch = std::function<int(const std::string &url, std::string &location, std::string &body)>;

// The full request URL for a catalog path (path must be one of the three catalog paths, else empty).
std::string direct_request_url(const std::string &validated_base, const std::string &path, const std::string &query);
// Resolves a Location header against `current`; only a SAME-ORIGIN target (scheme, host, port) is returned, otherwise nullopt.
std::optional<std::string> same_origin_redirect(const std::string &current, const std::string &location);
// An HttpGet that talks to the Spoolman server directly. Anything but the three catalog paths returns status 0 without a request.
// Follows at most kMaxDirectRedirects same-origin redirects; a redirect to another origin comes back as its 3xx status.
HttpGet make_direct_http_get(const std::string &validated_base, DirectFetch fetch);
// Which transport a catalog fetch uses for the stored `spoolman_server_url` value: direct only for a valid, non-empty URL; an empty or
// invalid value falls back to the Moonraker proxy (`reason` then says why an invalid one was ignored).
struct TransportChoice { bool direct = false; std::string url; std::string reason; };
TransportChoice select_spoolman_transport(const std::string &stored_url);
// Cache key for the direct path (distinct from any Moonraker "<id>@<url>" key).
std::string direct_cache_key(const std::string &validated_base);

}} // namespace Slic3r::Spoolman

#endif
