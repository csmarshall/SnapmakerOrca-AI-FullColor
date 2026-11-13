#include "SpoolmanDirect.hpp"

#include "Http.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/log/trivial.hpp>
#include <sstream>

namespace Slic3r { namespace Spoolman {

namespace {

CatalogCache &direct_cache()
{
    static CatalogCache cache;
    return cache;
}

// The value of the LAST Location header in the accumulated header text ("" when none).
std::string last_location(const std::string &headers)
{
    std::istringstream in(headers);
    std::string        line, found;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (boost::istarts_with(line, "location:")) {
            found = line.substr(9);
            const auto b = found.find_first_not_of(" \t");
            found        = b == std::string::npos ? std::string() : found.substr(b);
        }
    }
    return found;
}

} // namespace

int direct_http_fetch(const std::string &url, std::string &location, std::string &body)
{
    int         result = 0;
    std::string headers;
    body.clear();
    location.clear();
    auto http = Http::get(url); // GET: there is no body or method parameter anywhere on this path
    // Nothing the app adds globally (extra headers) may reach a user-entered server; no auth is ever set here.
    http.headers_clear()
        .follow_redirects(false)
        .report_redirects(true)
        .timeout_connect(kDirectConnectTimeoutSeconds)
        .timeout_max(kDirectMaxTimeoutSeconds)
        .size_limit(kDirectMaxResponseBytes)
        .on_header_callback([&](std::string h) { headers = std::move(h); })
        .on_complete([&](std::string reply, unsigned status) {
            body   = std::move(reply);
            result = int(status);
        })
        .on_error([&](std::string, std::string err, unsigned status) {
            BOOST_LOG_TRIVIAL(debug) << "Spoolman direct request failed: " << err;
            body.clear();
            result = (status == 0 && err.find("size exceeded limit") != std::string::npos) ? kStatusTooLarge : int(status);
        })
        .perform_sync();
    if (result >= 300 && result < 400)
        location = last_location(headers);
    return result;
}

CatalogFetch fetch_direct_catalog(const std::string &validated_base, bool preference_on)
{
    return fetch_catalog_if_enabled(preference_on, [] { return std::optional<bool>(true); }, direct_cache(), direct_cache_key(validated_base),
                                    make_direct_http_get(validated_base, &direct_http_fetch));
}

std::optional<CatalogFetch> last_good_direct_catalog(const std::string &validated_base)
{
    return direct_cache().last_good(direct_cache_key(validated_base));
}

std::optional<long long> last_good_direct_age_seconds(const std::string &validated_base)
{
    return direct_cache().last_good_age_seconds(direct_cache_key(validated_base));
}

}} // namespace Slic3r::Spoolman
