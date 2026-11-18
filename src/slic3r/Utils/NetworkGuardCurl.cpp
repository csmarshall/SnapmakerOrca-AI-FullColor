#include "NetworkGuardCurl.hpp"

#include "libslic3r/NetworkGuard.hpp"

#include <cstring>

#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/address_v6.hpp>
#include <boost/log/trivial.hpp>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
#endif

namespace Slic3r {
namespace NetworkGuard {

namespace {

struct Url
{
    CURLU *h = curl_url();
    ~Url() { curl_url_cleanup(h); }
    Url(const Url &) = delete;
    Url() = default;
    bool set(const std::string &s, unsigned flags) { return curl_url_set(h, CURLUPART_URL, s.c_str(), flags) == CURLUE_OK; }
    bool get(CURLUPart part, std::string &out) const
    {
        char *p = nullptr;
        if (curl_url_get(h, part, &p, 0) != CURLUE_OK || p == nullptr)
            return false;
        out = p;
        curl_free(p);
        return true;
    }
};

bool has_control_chars(const std::string &s)
{
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7f)
            return true; // includes NUL, which CURLOPT_URL (c_str) would truncate at
    return false;
}

} // namespace

bool parse_url(const std::string &url, ParsedUrl &out)
{
    if (url.empty() || has_control_chars(url))
        return false;
    Url u;
    if (!u.h || !u.set(url, CURLU_GUESS_SCHEME))
        return false;
    ParsedUrl p;
    if (!u.get(CURLUPART_URL, p.url) || !u.get(CURLUPART_SCHEME, p.scheme) || !u.get(CURLUPART_HOST, p.host))
        return false;
    if (p.scheme != "http" && p.scheme != "https")
        return false;
    out = std::move(p);
    return true;
}

bool request_allowed(const std::string &url, std::string *checked_url)
{
    if (internet_access_allowed()) {
        if (checked_url) *checked_url = url;
        return true;
    }
    ParsedUrl p;
    if (!parse_url(url, p))
        return false;
    // extract_host() runs on the host curl reported, never on the raw string.
    if (!is_local_network_host(p.host, user_allowed_hosts()))
        return false;
    if (checked_url) *checked_url = p.url;
    return true;
}

bool resolve_redirect(const std::string &base_url, const std::string &location, std::string &absolute_url)
{
    if (location.empty() || has_control_chars(location) || has_control_chars(base_url))
        return false;
    Url u;
    if (!u.h || !u.set(base_url, 0) || !u.set(location, 0)) // second set on a populated handle = RFC 3986 reference resolution
        return false;
    return u.get(CURLUPART_URL, absolute_url);
}

bool redirect_allowed(const std::string &base_url, const std::string &location)
{
    std::string absolute;
    return resolve_redirect(base_url, location, absolute) && request_allowed(absolute);
}

bool connect_allowed(const std::string &effective_url, const curl_sockaddr *addr, std::string *address_text)
{
    if (internet_access_allowed())
        return true;
    if (addr == nullptr)
        return false;
    std::string text;
    if (addr->family == AF_INET && addr->addrlen >= int(sizeof(sockaddr_in))) {
        const sockaddr_in &sin = *reinterpret_cast<const sockaddr_in *>(&addr->addr); // libcurl's real storage is larger than struct sockaddr
        boost::asio::ip::address_v4::bytes_type b;
        std::memcpy(b.data(), &sin.sin_addr, b.size());
        text = boost::asio::ip::address_v4(b).to_string();
    } else if (addr->family == AF_INET6 && addr->addrlen >= int(sizeof(sockaddr_in6))) {
        const sockaddr_in6 &sin6 = *reinterpret_cast<const sockaddr_in6 *>(&addr->addr);
        boost::asio::ip::address_v6::bytes_type b;
        std::memcpy(b.data(), &sin6.sin6_addr, b.size());
        text = boost::asio::ip::address_v6(b).to_string(); // no zone: scope does not change the class
    } // any other family (unix socket, ...) stays empty = refused
    if (address_text) *address_text = text;
    ParsedUrl p;
    if (!parse_url(effective_url, p))
        return false;
    return is_connect_allowed(p.host, text, user_allowed_hosts());
}

curl_socket_t open_socket_cb(void *clientp, curlsocktype purpose, curl_sockaddr *addr)
{
    auto *guard = static_cast<ConnectGuard *>(clientp);
    if (purpose == CURLSOCKTYPE_IPCXN && guard != nullptr && addr != nullptr) {
        std::string effective;
        char       *eff = nullptr;
        if (guard->curl != nullptr && curl_easy_getinfo(guard->curl, CURLINFO_EFFECTIVE_URL, &eff) == CURLE_OK && eff != nullptr)
            effective = eff;
        std::string text;
        if (!connect_allowed(effective, addr, &text)) {
            guard->blocked         = true;
            guard->blocked_host    = effective;
            guard->blocked_address = text;
            BOOST_LOG_TRIVIAL(info) << "NetworkGuard: refused connection to " << redacted_target(effective) << " resolved to " << (text.empty() ? "<unsupported address>" : text);
            return CURL_SOCKET_BAD;
        }
    }
    return ::socket(addr->family, addr->socktype, addr->protocol);
}

void install_connect_guard(CURL *curl, ConnectGuard *guard)
{
    guard->curl = curl;
    guard->blocked = false;
    guard->blocked_host.clear();
    guard->blocked_address.clear();
    curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, open_socket_cb);
    curl_easy_setopt(curl, CURLOPT_OPENSOCKETDATA, guard);
}

} // namespace NetworkGuard
} // namespace Slic3r
