#include "SpoolmanTransport.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <nlohmann/json.hpp>

namespace Slic3r { namespace Spoolman {

namespace {

// Iterative, string-aware depth scan with early exit once the limit is exceeded.
bool nesting_exceeds(const std::string &raw, int limit)
{
    int  depth = 0;
    bool in_string = false, escaped = false;
    for (char c : raw) {
        if (in_string) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                in_string = false;
        } else if (c == '"')
            in_string = true;
        else if (c == '[' || c == '{') {
            if (++depth > limit)
                return true;
        } else if ((c == ']' || c == '}') && depth > 0)
            --depth;
    }
    return false;
}

template<class T> ParseResult<T> malformed(const std::string &reason)
{
    ParseResult<T> r;
    r.status = ParseStatus::Malformed;
    r.reason = reason;
    return r;
}

template<class T>
ParseResult<T> fetch_one(const HttpGet &get, const char *path, const char *query, ParseResult<T> (*parse_text)(const std::string &))
{
    int         status = 0;
    std::string body;
    try {
        status = get(path, query, body);
    } catch (const std::exception &e) {
        return ParseResult<T>::http_failure(0, std::string("transport exception: ") + e.what());
    } catch (...) {
        return ParseResult<T>::http_failure(0, "transport exception");
    }
    if (status == 0)
        return ParseResult<T>::http_failure(0, "no response (connection failure or timeout)");
    if (status == kStatusTooLarge)
        return ParseResult<T>::http_failure(0, "response too large");
    if (status == kStatusNotV2)
        return ParseResult<T>::http_failure(0, "proxy reply is not v2 format");
    if (status != 200)
        return ParseResult<T>::http_failure(status, "HTTP " + std::to_string(status));
    if (std::string why = check_body_limits(body); !why.empty())
        return malformed<T>(why);
    try {
        return parse_text(body);
    } catch (...) {
        return malformed<T>("parser exception");
    }
}

bool part_good(ParseStatus s) { return s == ParseStatus::Ok || s == ParseStatus::Empty; }

CatalogFetch skipped(const char *reason)
{
    CatalogFetch f;
    f.skipped_reason = reason;
    return f;
}

} // namespace

bool CatalogFetch::good() const
{
    return attempted && part_good(vendors.status) && part_good(filaments.status) && part_good(spools.status);
}

std::string check_body_limits(const std::string &body)
{
    if (body.size() > kMaxBodyBytes)
        return "response too large";
    if (nesting_exceeds(body, kMaxBodyDepth))
        return "response too deep";
    return {};
}

CatalogFetch fetch_catalog(const HttpGet &get)
{
    CatalogFetch f;
    f.attempted = true;
    f.vendors   = fetch_one<Vendor>(get, "/v1/vendor", "", &parse_vendors_text);
    f.filaments = fetch_one<Filament>(get, "/v1/filament", "", &parse_filaments_text);
    f.spools    = fetch_one<Spool>(get, "/v1/spool", "allow_archived=false", &parse_spools_text);
    if (f.spools.status == ParseStatus::Ok) {
        auto &items = f.spools.items;
        items.erase(std::remove_if(items.begin(), items.end(), [](const Spool &s) { return s.archived; }), items.end());
        if (items.empty())
            f.spools.status = ParseStatus::Empty;
    }
    return f;
}

CatalogCache::CatalogCache() : m_clock([] { return std::chrono::steady_clock::now(); }) {}
CatalogCache::CatalogCache(Clock clock) : m_clock(clock ? std::move(clock) : Clock([] { return std::chrono::steady_clock::now(); })) {}

CatalogFetch CatalogCache::get(const std::string &key, const HttpGet &http)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_entries.find(key);
        if (it != m_entries.end() && it->second.fresh)
            return it->second.value;
    }
    unsigned long generation;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        generation = m_generation;
    }
    CatalogFetch fetched = fetch_catalog(http); // no lock held across the network round trip
    if (fetched.good()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        // An invalidate() during the fetch keeps the new value but leaves it stale.
        m_entries[key] = Entry{fetched, generation == m_generation, m_clock()};
    }
    return fetched;
}

void CatalogCache::invalidate(const std::string &key)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_generation;
    if (auto it = m_entries.find(key); it != m_entries.end())
        it->second.fresh = false;
}

void CatalogCache::invalidate_all()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_generation;
    for (auto &kv : m_entries)
        kv.second.fresh = false;
}

std::optional<CatalogFetch> CatalogCache::last_good(const std::string &key) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (auto it = m_entries.find(key); it != m_entries.end())
        return it->second.value;
    return std::nullopt;
}

std::optional<long long> CatalogCache::last_good_age_seconds(const std::string &key) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_entries.find(key);
    if (it == m_entries.end())
        return std::nullopt;
    const auto age = std::chrono::duration_cast<std::chrono::seconds>(m_clock() - it->second.fetched_at).count();
    return age < 0 ? 0 : age;
}

CatalogFetch fetch_catalog_if_enabled(bool preference_on, const StatusFn &status, CatalogCache &cache, const std::string &key,
                                      const HttpGet &http)
{
    if (!preference_on)
        return skipped("preference off");
    std::optional<bool> connected;
    try {
        if (status)
            connected = status();
    } catch (...) {
    }
    if (!connected.has_value())
        return skipped("status unknown");
    if (!*connected)
        return skipped("not connected");
    return cache.get(key, http);
}

std::string moonraker_proxy_request_body(const std::string &path, const std::string &query)
{
    nlohmann::json j;
    j["use_v2_response"] = true;
    j["request_method"]  = "GET"; // read-only on Spoolman; never anything else, never a body
    j["path"]            = path;
    if (!query.empty())
        j["query"] = query;
    return j.dump();
}

int moonraker_proxy_unwrap(int http_status, const std::string &reply, std::string &body)
{
    body.clear();
    if (http_status != 200)
        return http_status;
    if (!check_body_limits(reply).empty()) {
        body = reply;
        return 200;
    }
    try {
        nlohmann::json j = nlohmann::json::parse(reply, nullptr, false);
        if (j.is_object() && j.contains("result") && j["result"].is_object())
            j = j["result"];
        if (!j.is_object() || !j.contains("response") || !j.contains("error"))
            return kStatusNotV2; // not a version 2 proxy answer (also: not JSON)
        const auto &err = j["error"];
        if (err.is_object()) {
            const auto it = err.find("status_code");
            return (it != err.end() && it->is_number_integer() && it->get<int>() != 200 && it->get<int>() > 0) ? it->get<int>() : 502;
        }
        body = j["response"].dump();
        return 200;
    } catch (...) {
        return kStatusNotV2;
    }
}

std::optional<bool> moonraker_spoolman_connected(int http_status, const std::string &reply)
{
    if (http_status != 200 || !check_body_limits(reply).empty())
        return std::nullopt;
    try {
        nlohmann::json j = nlohmann::json::parse(reply, nullptr, false);
        if (j.is_object() && j.contains("result") && j["result"].is_object())
            j = j["result"];
        if (j.is_object() && j.contains("spoolman_connected") && j["spoolman_connected"].is_boolean())
            return j["spoolman_connected"].get<bool>();
    } catch (...) {
    }
    return std::nullopt;
}

namespace {

struct ParsedUrl { std::string scheme, host; int port = 0; std::string path; }; // host as written (lower-case; [..] kept for IPv6)

std::string lower(std::string s)
{
    for (char &c : s)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return s;
}

// Returns a reason, or empty and fills `out`. `port` 0 = not given.
std::string parse_url(const std::string &in, ParsedUrl &out, int &explicit_port)
{
    if (in.size() > kMaxSpoolmanUrlLength)
        return "the URL is too long";
    for (unsigned char c : in) {
        if (c <= 0x20 || c == 0x7f)
            return "the URL must not contain spaces or control characters";
        if (c >= 0x80)
            return "the URL must be plain ASCII";
        if (c == '\\')
            return "the URL must not contain backslashes";
    }
    const size_t sep = in.find("://");
    if (sep == std::string::npos)
        return "the URL must start with http:// or https://";
    out.scheme = lower(in.substr(0, sep));
    if (out.scheme != "http" && out.scheme != "https")
        return "only http:// and https:// URLs are accepted";
    if (in.find('#') != std::string::npos)
        return "the URL must not contain a fragment (#)";
    if (in.find('?') != std::string::npos)
        return "the URL must not contain a query (?)";
    const size_t auth_begin = sep + 3;
    const size_t auth_end   = in.find('/', auth_begin);
    const std::string authority = in.substr(auth_begin, auth_end == std::string::npos ? std::string::npos : auth_end - auth_begin);
    out.path = auth_end == std::string::npos ? std::string() : in.substr(auth_end);
    if (authority.find('@') != std::string::npos)
        return "the URL must not contain credentials (user:password@)";
    std::string host = authority, port;
    if (!authority.empty() && authority[0] == '[') {
        const size_t close = authority.find(']');
        if (close == std::string::npos)
            return "unterminated [IPv6] host";
        host = authority.substr(0, close + 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':')
                return "unexpected text after the [IPv6] host";
            port = authority.substr(close + 2);
            if (port.empty())
                return "the port is empty";
        }
        for (size_t i = 1; i + 1 < host.size(); ++i)
            if (!std::isxdigit(static_cast<unsigned char>(host[i])) && host[i] != ':' && host[i] != '.')
                return "invalid character in the [IPv6] host";
        if (host.size() <= 2)
            return "the URL has no host";
    } else {
        const size_t colon = authority.find(':');
        if (colon != std::string::npos) {
            host = authority.substr(0, colon);
            port = authority.substr(colon + 1);
            if (port.empty())
                return "the port is empty";
        }
        if (host.empty())
            return "the URL has no host";
        for (char c : host)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-' && c != '_')
                return "invalid character in the host name";
    }
    explicit_port = 0;
    if (!port.empty()) {
        if (port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos)
            return "the port must be a number from 1 to 65535";
        const int n = std::stoi(port);
        if (n < 1 || n > 65535)
            return "the port must be a number from 1 to 65535";
        explicit_port = n;
    }
    out.host = lower(host);
    out.port = explicit_port ? explicit_port : (out.scheme == "https" ? 443 : 80);
    while (!out.path.empty() && out.path.back() == '/')
        out.path.pop_back();
    return {};
}

// For redirect targets, which may carry a query: same parse but the query is cut off first.
bool parse_url_lenient(const std::string &in, ParsedUrl &out, int &explicit_port)
{
    std::string t = in.substr(0, in.find_first_of("?#"));
    return parse_url(t, out, explicit_port).empty();
}

std::string origin_key(const ParsedUrl &u) { return u.scheme + "://" + u.host + ":" + std::to_string(u.port); }

} // namespace

SpoolmanUrlCheck validate_spoolman_url(const std::string &input)
{
    SpoolmanUrlCheck r;
    size_t b = input.find_first_not_of(" \t\r\n"), e = input.find_last_not_of(" \t\r\n");
    if (b == std::string::npos) {
        r.ok = true;
        return r;
    }
    const std::string trimmed = input.substr(b, e - b + 1);
    ParsedUrl         u;
    int               explicit_port = 0;
    if (std::string why = parse_url(trimmed, u, explicit_port); !why.empty()) {
        r.reason = why;
        return r;
    }
    r.ok  = true;
    r.url = u.scheme + "://" + u.host + (explicit_port ? ":" + std::to_string(explicit_port) : std::string()) + u.path;
    return r;
}

std::string direct_request_url(const std::string &validated_base, const std::string &path, const std::string &query)
{
    if (!(path == "/v1/vendor" || path == "/v1/filament" || path == "/v1/spool"))
        return {};
    std::string base = validated_base;
    while (!base.empty() && base.back() == '/')
        base.pop_back();
    return base + kSpoolmanApiPrefix + path + (query.empty() ? std::string() : "?" + query);
}

namespace {
// A redirect hop may only land on one of the three catalog endpoints, with the query hop 0 uses (spool: allow_archived=false; others: none).
bool is_catalog_target(const std::string &url)
{
    if (url.find('#') != std::string::npos)
        return false;
    const size_t q     = url.find('?');
    const std::string path  = url.substr(0, q);
    const std::string query = q == std::string::npos ? std::string() : url.substr(q + 1);
    const size_t auth_begin = url.find("://") + 3;
    const size_t path_begin = path.find('/', auth_begin);
    if (path_begin == std::string::npos)
        return false;
    const std::string p = path.substr(path_begin);
    if (p.find("/../") != std::string::npos || p.find("/./") != std::string::npos || p.find('%') != std::string::npos)
        return false;
    const auto ends_with = [&](const std::string &suffix) { return p.size() >= suffix.size() && p.compare(p.size() - suffix.size(), suffix.size(), suffix) == 0; };
    const std::string prefix = kSpoolmanApiPrefix;
    if (ends_with(prefix + "/v1/vendor") || ends_with(prefix + "/v1/filament"))
        return query.empty();
    if (ends_with(prefix + "/v1/spool"))
        return query == "allow_archived=false";
    return false;
}
} // namespace

std::optional<std::string> same_origin_redirect(const std::string &current, const std::string &location)
{
    ParsedUrl cur;
    int       dummy = 0;
    if (!parse_url_lenient(current, cur, dummy))
        return std::nullopt;
    for (unsigned char c : location)
        if (c <= 0x20 || c == 0x7f || c >= 0x80 || c == '\\')
            return std::nullopt;
    if (location.empty())
        return std::nullopt;
    std::string result;
    if (location[0] == '/' && (location.size() == 1 || location[1] != '/')) {
        const size_t auth_begin = current.find("://") + 3;
        const size_t auth_end   = current.find('/', auth_begin);
        result = current.substr(0, auth_end == std::string::npos ? current.size() : auth_end) + location; // same authority, new path
    } else {
        ParsedUrl tgt;
        if (location.find("://") == std::string::npos || !parse_url_lenient(location, tgt, dummy))
            return std::nullopt; // protocol-relative ("//host"), other relative forms and unparsable targets are refused
        if (origin_key(tgt) != origin_key(cur))
            return std::nullopt;
        result = location;
    }
    if (!is_catalog_target(result))
        return std::nullopt;
    return result;
}

HttpGet make_direct_http_get(const std::string &validated_base, DirectFetch fetch)
{
    return [validated_base, fetch](const std::string &path, const std::string &query, std::string &body) {
        body.clear();
        std::string url = direct_request_url(validated_base, path, query);
        if (url.empty() || !fetch)
            return 0;
        for (int hop = 0;; ++hop) {
            std::string location;
            const int   status = fetch(url, location, body);
            if (status < 300 || status >= 400)
                return status;
            body.clear();
            auto next = hop < kMaxDirectRedirects ? same_origin_redirect(url, location) : std::nullopt;
            if (!next)
                return status; // refused: another origin, no usable Location or too many hops
            url = *next;
        }
    };
}

TransportChoice select_spoolman_transport(const std::string &stored_url)
{
    TransportChoice c;
    const SpoolmanUrlCheck v = validate_spoolman_url(stored_url);
    if (v.ok && !v.url.empty()) {
        c.direct = true;
        c.url    = v.url;
    } else if (!v.ok)
        c.reason = v.reason;
    return c;
}

std::string direct_cache_key(const std::string &validated_base) { return "direct:" + validated_base; }

}} // namespace Slic3r::Spoolman
