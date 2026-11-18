#include "NetworkGuard.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <set>

#include <boost/asio/ip/address_v6.hpp>
#include <boost/log/trivial.hpp>

namespace Slic3r {
namespace NetworkGuard {

namespace {

std::string lower(std::string s)
{
    for (char &c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ends_with(const std::string &s, const std::string &suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Parse one inet_aton component: decimal, 0x hex or leading-0 octal. False when malformed or > max.
bool parse_ipv4_component(const std::string &s, uint64_t max, uint64_t &out)
{
    if (s.empty())
        return false;
    int      base = 10;
    size_t   i    = 0;
    if (s.size() > 1 && s[0] == '0') {
        if (s[1] == 'x' || s[1] == 'X') {
            base = 16;
            i    = 2;
            if (s.size() == 2)
                return false;
        } else {
            base = 8;
            i    = 1;
        }
    }
    uint64_t v = 0;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        int        d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (base == 16 && std::isxdigit(static_cast<unsigned char>(c))) d = std::tolower(c) - 'a' + 10;
        else return false;
        if (d >= base)
            return false;
        v = v * uint64_t(base) + uint64_t(d);
        if (v > max)
            return false;
    }
    out = v;
    return true;
}

// True when every dot-separated label looks numeric (digits, or 0x...): then the OS resolver treats it as an IPv4 literal, never DNS.
bool looks_numeric(const std::string &host)
{
    size_t start = 0;
    while (true) {
        const size_t dot   = host.find('.', start);
        const std::string label = host.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (label.empty())
            return false;
        const bool hex = label.size() >= 2 && label[0] == '0' && (label[1] == 'x' || label[1] == 'X');
        for (size_t i = hex ? 2 : 0; i < label.size(); ++i)
            if (!(hex ? std::isxdigit(static_cast<unsigned char>(label[i])) : std::isdigit(static_cast<unsigned char>(label[i])) != 0))
                return false;
        if (dot == std::string::npos)
            return true;
        start = dot + 1;
    }
}

// inet_aton semantics. False when malformed.
bool parse_ipv4(const std::string &host, uint32_t &out)
{
    std::vector<std::string> parts;
    size_t                   start = 0;
    while (true) {
        const size_t dot = host.find('.', start);
        parts.push_back(host.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
        if (dot == std::string::npos)
            break;
        start = dot + 1;
    }
    if (parts.size() > 4)
        return false;
    uint32_t addr = 0;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        uint64_t v;
        if (!parse_ipv4_component(parts[i], 255, v))
            return false;
        addr |= uint32_t(v) << (24 - 8 * i);
    }
    const size_t   last      = parts.size() - 1;
    const unsigned last_bits = 32 - 8 * unsigned(last);
    uint64_t       v;
    if (!parse_ipv4_component(parts.back(), (uint64_t(1) << last_bits) - 1, v))
        return false;
    addr |= uint32_t(v);
    out = addr;
    return true;
}

bool ipv4_is_local(uint32_t a)
{
    const uint8_t b0 = a >> 24, b1 = (a >> 16) & 0xff;
    return b0 == 127 || b0 == 10 || (b0 == 172 && b1 >= 16 && b1 <= 31) || (b0 == 192 && b1 == 168) || (b0 == 169 && b1 == 254);
}

bool ipv6_is_local(const boost::asio::ip::address_v6 &a)
{
    if (a.is_loopback())
        return true;
    if (a.is_v4_mapped())
        return false; // see header: conservative
    const auto b = a.to_bytes();
    if ((b[0] & 0xfe) == 0xfc) return true;                    // fc00::/7 ULA
    if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) return true;    // fe80::/10 link-local
    return false;
}

} // namespace

std::string extract_host(const std::string &input)
{
    std::string s = input;
    // trim ASCII whitespace
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    if (s.empty())
        return {};
    // A bare IPv6 literal without brackets ("::1", "fe80::1%en0") has no URL structure.
    if (std::count(s.begin(), s.end(), ':') >= 2 && s.find('/') == std::string::npos && s.find('[') == std::string::npos) {
        std::string literal = s;
        const size_t pct    = literal.find('%');
        if (pct != std::string::npos) literal.erase(pct);
        boost::system::error_code ec;
        boost::asio::ip::make_address_v6(literal, ec);
        if (!ec)
            return lower(literal);
    }
    const size_t scheme = s.find("://");
    if (scheme != std::string::npos)
        s.erase(0, scheme + 3);
    else if (s.compare(0, 2, "//") == 0)
        s.erase(0, 2);
    // authority ends at the first / ? # or backslash
    const size_t end = s.find_first_of("/?#\\");
    if (end != std::string::npos)
        s.erase(end);
    // userinfo: everything up to the last '@'
    const size_t at = s.rfind('@');
    if (at != std::string::npos)
        s.erase(0, at + 1);
    std::string host;
    if (!s.empty() && s[0] == '[') {
        const size_t close = s.find(']');
        if (close == std::string::npos)
            return {};
        const std::string rest = s.substr(close + 1);
        if (!rest.empty() && rest[0] != ':')
            return {};
        std::string literal = s.substr(1, close - 1);
        const size_t pct    = literal.find('%');
        if (pct != std::string::npos) literal.erase(pct);
        boost::system::error_code ec;
        boost::asio::ip::make_address_v6(literal, ec);
        if (ec)
            return {};
        return lower(literal);
    }
    const size_t colon = s.find(':');
    host               = colon == std::string::npos ? s : s.substr(0, colon);
    if (colon != std::string::npos) {
        // port must be digits (or empty)
        for (size_t i = colon + 1; i < s.size(); ++i)
            if (!std::isdigit(static_cast<unsigned char>(s[i])))
                return {};
    }
    if (!host.empty() && host.back() == '.')
        host.pop_back();
    if (host.empty())
        return {};
    for (char c : host)
        if (!(std::isalnum(static_cast<unsigned char>(c)) && static_cast<unsigned char>(c) < 128) && c != '-' && c != '_' && c != '.')
            return {}; // percent-encoding, non-ASCII, spaces, control characters
    if (host.front() == '.' || host.find("..") != std::string::npos)
        return {};
    return lower(host);
}

bool is_local_network_host(const std::string &url_or_host, const std::vector<std::string> &user_allowed_hosts)
{
    const std::string host = extract_host(url_or_host);
    if (host.empty())
        return false;

    for (const std::string &allowed : user_allowed_hosts)
        if (!allowed.empty() && extract_host(allowed) == host)
            return true;

    if (host.find(':') != std::string::npos) { // IPv6 literal (extract_host validated it)
        boost::system::error_code ec;
        const auto                a = boost::asio::ip::make_address_v6(host, ec);
        return !ec && ipv6_is_local(a);
    }

    if (looks_numeric(host)) {
        uint32_t a;
        return parse_ipv4(host, a) && ipv4_is_local(a);
    }

    if (host == "localhost" || ends_with(host, ".localhost") || ends_with(host, ".local") || ends_with(host, ".lan") ||
        host == "home.arpa" || ends_with(host, ".home.arpa"))
        return true;
    return host.find('.') == std::string::npos; // bare hostname ("printer")
}

namespace {
std::atomic_bool         g_allowed{false};
std::mutex               g_mutex;
std::vector<std::string> g_hosts;
std::set<std::string>    g_logged_hosts;
} // namespace

void set_internet_access_allowed(bool allowed) { g_allowed.store(allowed); }
bool internet_access_allowed() { return g_allowed.load(); }

void set_user_allowed_hosts(const std::vector<std::string> &hosts)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_hosts = hosts;
}

std::vector<std::string> user_allowed_hosts()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_hosts;
}

bool is_request_allowed(const std::string &url)
{
    if (internet_access_allowed())
        return true;
    return is_local_network_host(url, user_allowed_hosts());
}

std::string blocked_message() { return "Blocked: network access outside the local network is off (Preferences)"; }

std::string redacted_target(const std::string &url)
{
    const std::string host = extract_host(url);
    std::string       s    = url;
    std::string       scheme;
    const size_t      sp = s.find("://");
    if (sp != std::string::npos) {
        scheme = s.substr(0, sp + 3);
        s.erase(0, sp + 3);
    }
    std::string path;
    const size_t slash = s.find('/');
    if (slash != std::string::npos) {
        path = s.substr(slash);
        const size_t cut = path.find_first_of("?#");
        if (cut != std::string::npos) path.erase(cut);
    }
    return scheme + (host.empty() ? "<invalid-host>" : host) + path;
}

void log_blocked(const std::string &url)
{
    const std::string host = extract_host(url);
    bool              first;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        first = g_logged_hosts.insert(host).second;
    }
    if (first)
        BOOST_LOG_TRIVIAL(info) << "NetworkGuard: blocked request to " << redacted_target(url)
                                << " (further blocked requests to this host are not logged)";
    else
        BOOST_LOG_TRIVIAL(debug) << "NetworkGuard: blocked request to " << redacted_target(url);
}

void log_state()
{
    BOOST_LOG_TRIVIAL(info) << "NetworkGuard: access outside the local network is " << (internet_access_allowed() ? "ALLOWED" : "OFF (default)");
}

} // namespace NetworkGuard
} // namespace Slic3r
