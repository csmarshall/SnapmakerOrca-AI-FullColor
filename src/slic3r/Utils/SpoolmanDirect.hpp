#ifndef slic3r_SpoolmanDirect_hpp_
#define slic3r_SpoolmanDirect_hpp_

// Direct (no Moonraker) read-only catalog fetch from a Spoolman server URL (#107). The URL is the validated value from
// Spoolman::select_spoolman_transport(app_config "spoolman_server_url"). GET only, redirects are never followed by curl (the pure layer
// follows same-origin ones), no credentials or app headers are sent, same caps and timeouts as the Moonraker proxy path.

#include "libslic3r/Spoolman/SpoolmanTransport.hpp"

#include <optional>
#include <string>

namespace Slic3r { namespace Spoolman {

inline constexpr int    kDirectConnectTimeoutSeconds = 3;
inline constexpr int    kDirectMaxTimeoutSeconds     = 6;
inline constexpr size_t kDirectMaxResponseBytes      = kMaxBodyBytes + 64 * 1024;

// One real GET (Http::get), redirects off. Production DirectFetch.
int direct_http_fetch(const std::string &url, std::string &location, std::string &body);

// Catalog fetch with a process-wide session cache keyed by direct_cache_key(). `preference_on` false => no request.
CatalogFetch fetch_direct_catalog(const std::string &validated_base, bool preference_on);
std::optional<CatalogFetch> last_good_direct_catalog(const std::string &validated_base);
std::optional<long long>    last_good_direct_age_seconds(const std::string &validated_base);

}} // namespace Slic3r::Spoolman

#endif
