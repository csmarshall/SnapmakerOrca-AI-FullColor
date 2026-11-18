#ifndef slic3r_NetworkGuardCurl_hpp_
#define slic3r_NetworkGuardCurl_hpp_

// #139 / ADR 0019, curl half of the network guard (the pure classifier is libslic3r/NetworkGuard.hpp). Everything here asks libcurl's own
// URL parser (CURLU, needs libcurl >= 7.62) what host a URL or redirect means, and checks the address libcurl really connects to, so the
// guard cannot disagree with the transfer about where a request goes.

#include <string>

#include <curl/curl.h>

namespace Slic3r {
namespace NetworkGuard {

struct ParsedUrl
{
    std::string url;    // curl's normalised form: what CURLOPT_URL must be set to
    std::string scheme; // lower case
    std::string host;   // as curl will connect to it ("[::1]" is bracketed)
};

// Parse with curl_url (CURLU_GUESS_SCHEME, like CURLOPT_URL). False when curl rejects the URL, it holds control characters, or the
// scheme is not http / https: such a request is blocked while the guard is active.
bool parse_url(const std::string &url, ParsedUrl &out);

// Switch ON: always true, checked_url is left as given. Switch OFF: true only when curl's parse succeeds and the host curl will connect to
// is local or user-configured; checked_url receives curl's normalised URL (set CURLOPT_URL from it).
bool request_allowed(const std::string &url, std::string *checked_url = nullptr);

// Resolve a Location header against the URL of the response that carried it, exactly as RFC 3986 / curl does ("/x", "x", "//host/x",
// "http://host/x", "?q"). False when the base or location is rejected.
bool resolve_redirect(const std::string &base_url, const std::string &location, std::string &absolute_url);

// Is the host of this redirect Location (relative to base_url) acceptable? Fail closed on anything curl rejects.
bool redirect_allowed(const std::string &base_url, const std::string &location);

// Per-transfer state of the connect-time check.
struct ConnectGuard
{
    CURL       *curl = nullptr;
    bool        blocked = false; // set when a connection was refused
    std::string blocked_host;
    std::string blocked_address;
};

// Decision for one connect attempt, factored out of the curl callback so it can be tested with plain values.
bool connect_allowed(const std::string &effective_url, const curl_sockaddr *addr, std::string *address_text = nullptr);

// CURLOPT_OPENSOCKETFUNCTION: refuses (CURL_SOCKET_BAD) a connection to a non-local address unless the host is user-configured.
curl_socket_t open_socket_cb(void *clientp, curlsocktype purpose, curl_sockaddr *addr);

// Install the callback on an easy handle. guard must outlive the transfer.
void install_connect_guard(CURL *curl, ConnectGuard *guard);

} // namespace NetworkGuard
} // namespace Slic3r

#endif
