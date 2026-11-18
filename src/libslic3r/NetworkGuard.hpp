#ifndef slic3r_NetworkGuard_hpp_
#define slic3r_NetworkGuard_hpp_

// #139 / ADR 0019: "nothing leaves the LAN" switch. Pure, wx-free, no dependency on the GUI or AppConfig: the GUI pushes the
// switch and the user's configured printer / Spoolman hosts in through the setters below; Http.cpp and AppConfig read them.

#include <string>
#include <vector>

namespace Slic3r {
namespace NetworkGuard {

// app_config key (section "app"). Default false: nothing leaves the local network until the user turns it on.
constexpr const char *k_allow_internet_key = "allow_internet_access";

// Extract the lower-cased host from a URL or a bare host ("http://user:pw@Host:8080/x", "[fe80::1]:80", "printer.local.").
// Returns an empty string when the input is not a usable host (empty, bad characters, bad IPv6, percent-encoding, non-ASCII).
std::string extract_host(const std::string &url_or_host);

// True when the host is on the local network: loopback, RFC1918, link-local, ULA, dotless names, *.local, *.lan, *.home.arpa,
// *.localhost, or an exact (case-insensitive) match of an entry of user_allowed_hosts (URLs or bare hosts).
// IPv4 numeric forms (0x7f.1, 2130706433, 0177.0.0.1) are read the way an OS resolver reads them. Anything ambiguous, malformed
// or public is NOT local. IPv4-mapped IPv6 ([::ffff:192.168.1.1]) is deliberately NOT local (add it to the allowed hosts if needed).
bool is_local_network_host(const std::string &url_or_host, const std::vector<std::string> &user_allowed_hosts);

// Process-wide state, set at startup and whenever the preference or the configured hosts change. Thread safe.
void set_internet_access_allowed(bool allowed);
bool internet_access_allowed();
void set_user_allowed_hosts(const std::vector<std::string> &hosts);
std::vector<std::string> user_allowed_hosts();

// The decision used by the HTTP layer: true when the switch is on, or the target is local / a user-configured host.
bool is_request_allowed(const std::string &url);

// Message handed to the request's error callback when a request is blocked.
std::string blocked_message();

// Log (once per host) that a request was blocked. Only host and path are logged: no query, no credentials.
void log_blocked(const std::string &url);

// "scheme://host/path" without userinfo, query or fragment, for logging.
std::string redacted_target(const std::string &url);

// One info line stating the switch state (call once at startup).
void log_state();

} // namespace NetworkGuard
} // namespace Slic3r

#endif
