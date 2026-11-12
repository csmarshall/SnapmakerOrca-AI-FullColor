#include "SpoolmanTransport.hpp"

#include <algorithm>
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
        m_entries[key] = Entry{fetched, generation == m_generation};
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

}} // namespace Slic3r::Spoolman
