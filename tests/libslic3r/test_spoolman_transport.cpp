#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Spoolman/SpoolmanTransport.hpp"

#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

using json = nlohmann::json;
using namespace Slic3r::Spoolman;

namespace {

std::string read_fixture(const std::string &name)
{
    std::ifstream in(std::string(TEST_DATA_DIR) + "/spoolman/" + name, std::ios::binary);
    REQUIRE(in.is_open());
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// Records every request. `routes` maps path -> {status, body}. Everything it is asked for is, by construction of the HttpGet type, a GET.
struct MockHttp
{
    struct Call { std::string path, query; };
    std::vector<Call>                              calls;
    std::map<std::string, std::pair<int, std::string>> routes;

    HttpGet fn()
    {
        return [this](const std::string &path, const std::string &query, std::string &body) {
            calls.push_back({path, query});
            auto it = routes.find(path);
            if (it == routes.end())
                return 404;
            body = it->second.second;
            return it->second.first;
        };
    }
    static MockHttp fixtures()
    {
        MockHttp m;
        m.routes["/v1/vendor"]   = {200, read_fixture("vendor.json")};
        m.routes["/v1/filament"] = {200, read_fixture("filament.json")};
        m.routes["/v1/spool"]    = {200, read_fixture("spool.json")};
        return m;
    }
};

std::string nested(size_t levels) { return std::string(levels, '[') + std::string(levels, ']'); }

} // namespace

TEST_CASE("Spoolman transport serves the real fixtures through the parsers", "[Spoolman][SpoolmanTransport]")
{
    MockHttp m = MockHttp::fixtures();
    CatalogFetch f = fetch_catalog(m.fn());
    REQUIRE(f.attempted);
    CHECK(f.good());
    CHECK(f.vendors.items.size() == 10);
    CHECK(f.filaments.items.size() == 22);
    CHECK(f.spools.items.size() == 30);
    // Only the three list endpoints, in order, spools with allow_archived=false.
    REQUIRE(m.calls.size() == 3);
    CHECK(m.calls[0].path == "/v1/vendor");
    CHECK(m.calls[1].path == "/v1/filament");
    CHECK(m.calls[2].path == "/v1/spool");
    CHECK(m.calls[2].query == "allow_archived=false");
}

TEST_CASE("Spoolman transport: HTTP failures are HttpFailure", "[Spoolman][SpoolmanTransport]")
{
    for (int status : {500, 0, 404, 503}) {
        MockHttp m = MockHttp::fixtures();
        m.routes["/v1/filament"] = {status, "{}"};
        CatalogFetch f = fetch_catalog(m.fn());
        CAPTURE(status);
        CHECK(f.filaments.status == ParseStatus::HttpFailure);
        CHECK(f.filaments.http_status == status);
        CHECK_FALSE(f.good());
    }
}

TEST_CASE("Spoolman transport: a throwing transport never escapes", "[Spoolman][SpoolmanTransport]")
{
    HttpGet boom = [](const std::string &, const std::string &, std::string &) -> int { throw std::runtime_error("x"); };
    CatalogFetch f = fetch_catalog(boom);
    CHECK(f.vendors.status == ParseStatus::HttpFailure);
    CHECK(f.vendors.http_status == 0);
}

TEST_CASE("Spoolman transport: a failed refresh never replaces the cached good result", "[Spoolman][SpoolmanTransport]")
{
    MockHttp good = MockHttp::fixtures();
    CatalogCache cache;
    CatalogFetch first = cache.get("printer-a", good.fn());
    REQUIRE(first.good());

    cache.invalidate("printer-a");
    MockHttp bad = MockHttp::fixtures();
    bad.routes["/v1/filament"] = {500, ""};
    CatalogFetch second = cache.get("printer-a", bad.fn());
    CHECK_FALSE(second.good());
    CHECK(second.filaments.status == ParseStatus::HttpFailure);

    auto kept = cache.last_good("printer-a");
    REQUIRE(kept.has_value());
    CHECK(kept->filaments.items.size() == 22);
    CHECK(kept->spools.items.size() == 30);

    MockHttp timeout = MockHttp::fixtures();
    timeout.routes["/v1/vendor"] = {0, ""};
    cache.get("printer-a", timeout.fn());
    CHECK(cache.last_good("printer-a")->vendors.items.size() == 10);
}

TEST_CASE("Spoolman transport: an empty result is Empty, never a failure", "[Spoolman][SpoolmanTransport]")
{
    MockHttp m = MockHttp::fixtures();
    m.routes["/v1/filament"] = {200, R"({"result": []})"};
    m.routes["/v1/spool"]    = {200, "[]"};
    CatalogFetch f = fetch_catalog(m.fn());
    CHECK(f.filaments.status == ParseStatus::Empty);
    CHECK(f.spools.status == ParseStatus::Empty);
    CHECK(f.good());
    CHECK(f.filaments.http_status == 0);
}

TEST_CASE("Spoolman transport: oversize and over-deep bodies are rejected before parsing", "[Spoolman][SpoolmanTransport]")
{
    SECTION("5 MiB body")
    {
        MockHttp m = MockHttp::fixtures();
        m.routes["/v1/filament"] = {200, std::string(5u * 1024u * 1024u, ' ') + "[]"};
        CatalogFetch f = fetch_catalog(m.fn());
        CHECK(f.filaments.status == ParseStatus::Malformed);
        CHECK(f.filaments.reason == "response too large");
        CHECK_FALSE(f.good());
    }
    SECTION("exactly at the cap is accepted")
    {
        std::string body = "[]" + std::string(kMaxBodyBytes - 2, ' ');
        REQUIRE(body.size() == kMaxBodyBytes);
        CHECK(check_body_limits(body).empty());
        CHECK(check_body_limits(body + " ") == "response too large");
    }
    SECTION("100000 levels of nesting")
    {
        MockHttp m = MockHttp::fixtures();
        m.routes["/v1/spool"] = {200, nested(100000)};
        CatalogFetch f = fetch_catalog(m.fn());
        CHECK(f.spools.status == ParseStatus::Malformed);
        CHECK(f.spools.reason == "response too deep");
    }
    SECTION("depth 16 passes the scan, 17 does not")
    {
        CHECK(check_body_limits(nested(16)).empty());
        CHECK(check_body_limits(nested(17)) == "response too deep");
        // brackets inside strings do not count
        CHECK(check_body_limits("[\"" + std::string(100, '[') + "\"]").empty());
        // an escaped quote does not end the string, so the brackets after it are still text ...
        CHECK(check_body_limits("[\"a\\\"" + std::string(40, '[') + "\"]").empty());
        // ... but an escaped backslash does not escape the quote after it, so these brackets are structure
        CHECK(check_body_limits("[\"\\\\\"," + std::string(40, '[') + std::string(40, ']') + "]") == "response too deep");
    }
}

TEST_CASE("Spoolman transport: archived spools are skipped", "[Spoolman][SpoolmanTransport]")
{
    MockHttp m = MockHttp::fixtures();
    m.routes["/v1/spool"] = {200, R"({"result":[
        {"id":1,"archived":false,"filament":{"id":3},"remaining_weight":10},
        {"id":2,"archived":true,"filament":{"id":3}},
        {"id":3,"filament":{"id":4}}]})"};
    CatalogFetch f = fetch_catalog(m.fn());
    REQUIRE(f.spools.items.size() == 2);
    CHECK(f.spools.items[0].id == 1);
    CHECK(f.spools.items[1].id == 3);
    CHECK(m.calls[2].query == "allow_archived=false");

    m.routes["/v1/spool"] = {200, R"([{"id":2,"archived":true}])"};
    f = fetch_catalog(m.fn());
    CHECK(f.spools.status == ParseStatus::Empty);
}

TEST_CASE("Spoolman transport: one round trip per session unless invalidated", "[Spoolman][SpoolmanTransport]")
{
    MockHttp m = MockHttp::fixtures();
    CatalogCache cache;
    cache.get("a", m.fn());
    CHECK(m.calls.size() == 3);
    CatalogFetch again = cache.get("a", m.fn());
    CHECK(m.calls.size() == 3);
    CHECK(again.filaments.items.size() == 22);

    cache.get("b", m.fn()); // another printer has its own entry
    CHECK(m.calls.size() == 6);

    cache.invalidate("a");
    cache.get("a", m.fn());
    CHECK(m.calls.size() == 9);
    cache.get("b", m.fn());
    CHECK(m.calls.size() == 9);

    cache.invalidate_all();
    cache.get("b", m.fn());
    CHECK(m.calls.size() == 12);
}

TEST_CASE("Spoolman transport: an invalidate during an in-flight get is not lost", "[Spoolman][SpoolmanTransport]")
{
    MockHttp m = MockHttp::fixtures();
    CatalogCache cache;
    HttpGet racing = [&](const std::string &path, const std::string &query, std::string &body) {
        if (path == "/v1/filament")
            cache.invalidate("a"); // another thread invalidates while this fetch is on the network
        return m.fn()(path, query, body);
    };
    CHECK(cache.get("a", racing).good());
    CHECK(cache.last_good("a").has_value()); // stored, but stale: the next get goes to the network
    const size_t before = m.calls.size();
    cache.get("a", m.fn());
    CHECK(m.calls.size() == before + 3);
}

TEST_CASE("Spoolman transport: preference off or not connected means no request at all", "[Spoolman][SpoolmanTransport]")
{
    MockHttp m = MockHttp::fixtures();
    CatalogCache cache;
    int status_calls = 0;
    StatusFn connected = [&]() -> std::optional<bool> { ++status_calls; return true; };

    SECTION("preference off")
    {
        CatalogFetch f = fetch_catalog_if_enabled(false, connected, cache, "a", m.fn());
        CHECK_FALSE(f.attempted);
        CHECK(f.skipped_reason == "preference off");
        CHECK(m.calls.empty());
        CHECK(status_calls == 0); // not even the Moonraker status probe
    }
    SECTION("status says not connected")
    {
        StatusFn off = [&]() -> std::optional<bool> { ++status_calls; return false; };
        CatalogFetch f = fetch_catalog_if_enabled(true, off, cache, "a", m.fn());
        CHECK_FALSE(f.attempted);
        CHECK(f.skipped_reason == "not connected");
        CHECK(status_calls == 1);
        CHECK(m.calls.empty());
    }
    SECTION("status unknown or throwing")
    {
        StatusFn unknown = []() -> std::optional<bool> { return std::nullopt; };
        CHECK_FALSE(fetch_catalog_if_enabled(true, unknown, cache, "a", m.fn()).attempted);
        StatusFn throws = []() -> std::optional<bool> { throw std::runtime_error("x"); };
        CHECK_FALSE(fetch_catalog_if_enabled(true, throws, cache, "a", m.fn()).attempted);
        CHECK(m.calls.empty());
    }
    SECTION("on and connected fetches")
    {
        CatalogFetch f = fetch_catalog_if_enabled(true, connected, cache, "a", m.fn());
        CHECK(f.attempted);
        CHECK(f.good());
        CHECK(m.calls.size() == 3);
    }
}

TEST_CASE("Spoolman preference defaults to off", "[Spoolman][SpoolmanTransport][AppConfig]")
{
    Slic3r::AppConfig config;
    config.set_defaults();
    CHECK(config.get("spoolman_preset_sync") == "false"); // registered with a value, not just absent (get_bool of "" is also false)
    CHECK_FALSE(config.get_bool("spoolman_preset_sync"));
    config.set_bool("spoolman_preset_sync", true);
    config.set_defaults();
    CHECK(config.get_bool("spoolman_preset_sync"));
}

TEST_CASE("Moonraker proxy request is always a GET without a body", "[Spoolman][SpoolmanTransport]")
{
    for (auto [path, query] : std::vector<std::pair<std::string, std::string>>{{"/v1/vendor", ""}, {"/v1/spool", "allow_archived=false"}}) {
        json j = json::parse(moonraker_proxy_request_body(path, query));
        CHECK(j.at("request_method") == "GET");
        CHECK(j.at("path") == path);
        CHECK_FALSE(j.contains("body"));
        CHECK(j.at("use_v2_response") == true);
        if (query.empty())
            CHECK_FALSE(j.contains("query"));
        else
            CHECK(j.at("query") == query);
    }
}

TEST_CASE("Moonraker proxy reply unwrapping", "[Spoolman][SpoolmanTransport]")
{
    std::string body;
    SECTION("v2 success, with and without the result wrapper")
    {
        CHECK(moonraker_proxy_unwrap(200, R"({"result":{"response":[{"id":1}],"error":null}})", body) == 200);
        CHECK(json::parse(body) == json::parse(R"([{"id":1}])"));
        CHECK(moonraker_proxy_unwrap(200, R"({"response":[],"error":null})", body) == 200);
        CHECK(body == "[]");
    }
    SECTION("a Spoolman error comes back as its status")
    {
        CHECK(moonraker_proxy_unwrap(200, R"({"result":{"response":null,"error":{"status_code":503,"message":"x"}}})", body) == 503);
        CHECK(body.empty());
    }
    SECTION("moonraker errors and junk")
    {
        CHECK(moonraker_proxy_unwrap(503, "whatever", body) == 503);
        CHECK(moonraker_proxy_unwrap(0, "", body) == 0);
        CHECK(moonraker_proxy_unwrap(200, "not json", body) == kStatusNotV2);
        // an older Moonraker that ignores use_v2_response answers {"result": <spoolman json>}
        CHECK(moonraker_proxy_unwrap(200, R"({"result":[1]})", body) == kStatusNotV2);
    }
    SECTION("v2 error object: its status_code, or 502 without one")
    {
        CHECK(moonraker_proxy_unwrap(200, R"({"response":null,"error":{"status_code":500,"message":"x"}})", body) == 500);
        CHECK(moonraker_proxy_unwrap(200, R"({"response":null,"error":{"message":"x"}})", body) == 502);
        CHECK(moonraker_proxy_unwrap(200, R"({"response":null,"error":{"status_code":"x"}})", body) == 502);
    }
    SECTION("sentinels become diagnosable reasons")
    {
        MockHttp m = MockHttp::fixtures();
        HttpGet sentinel = [](const std::string &path, const std::string &, std::string &) { return path == "/v1/vendor" ? kStatusNotV2 : kStatusTooLarge; };
        CatalogFetch f = fetch_catalog(sentinel);
        CHECK(f.vendors.status == ParseStatus::HttpFailure);
        CHECK(f.vendors.reason == "proxy reply is not v2 format");
        CHECK(f.filaments.reason == "response too large");
        CHECK_FALSE(f.good());
    }
    SECTION("a hostile reply is passed through to be rejected by the limits, never parsed here")
    {
        const std::string deep = nested(100000);
        CHECK(moonraker_proxy_unwrap(200, deep, body) == 200);
        CHECK(check_body_limits(body) == "response too deep");
    }
    SECTION("end to end through fetch_catalog")
    {
        HttpGet via_proxy = [](const std::string &path, const std::string &, std::string &out) {
            const std::string payload = path == "/v1/vendor" ? R"({"response":[{"id":1,"name":"V"}],"error":null})"
                                                              : R"({"result":{"response":[],"error":null}})";
            return moonraker_proxy_unwrap(200, payload, out);
        };
        CatalogFetch f = fetch_catalog(via_proxy);
        CHECK(f.vendors.status == ParseStatus::Ok);
        CHECK(f.filaments.status == ParseStatus::Empty);
    }
}

TEST_CASE("Moonraker spoolman status parsing", "[Spoolman][SpoolmanTransport]")
{
    CHECK(moonraker_spoolman_connected(200, R"({"result":{"spoolman_connected":true,"pending_reports":[],"spool_id":null}})") == true);
    CHECK(moonraker_spoolman_connected(200, R"({"spoolman_connected":false})") == false);
    CHECK_FALSE(moonraker_spoolman_connected(200, "{}").has_value());
    CHECK_FALSE(moonraker_spoolman_connected(500, R"({"spoolman_connected":true})").has_value());
    CHECK_FALSE(moonraker_spoolman_connected(0, "").has_value());
}

TEST_CASE("Spoolman transport: the cache records the time of the last good fetch", "[Spoolman][SpoolmanTransport]")
{
    auto now = std::chrono::steady_clock::time_point(std::chrono::hours(1000));
    CatalogCache cache([&now] { return now; });
    CHECK_FALSE(cache.last_good_age_seconds("printer-a").has_value());

    MockHttp good = MockHttp::fixtures();
    REQUIRE(cache.get("printer-a", good.fn()).good());
    CHECK(cache.last_good_age_seconds("printer-a") == 0);
    CHECK_FALSE(cache.last_good_age_seconds("printer-b").has_value());

    now += std::chrono::minutes(90);
    CHECK(cache.last_good_age_seconds("printer-a") == 5400);

    // A failed refresh keeps the old timestamp ...
    cache.invalidate("printer-a");
    MockHttp bad = MockHttp::fixtures();
    bad.routes["/v1/filament"] = {500, ""};
    CHECK_FALSE(cache.get("printer-a", bad.fn()).good());
    CHECK(cache.last_good_age_seconds("printer-a") == 5400);

    // ... and a good one resets it.
    REQUIRE(cache.get("printer-a", good.fn()).good());
    CHECK(cache.last_good_age_seconds("printer-a") == 0);

    // A clock that steps backwards never gives a negative age.
    now -= std::chrono::hours(2);
    CHECK(cache.last_good_age_seconds("printer-a") == 0);
}

// ---- #107: direct Spoolman server transport (pure; fakes only) ----

TEST_CASE("Spoolman direct URL validator matrix", "[Spoolman]")
{
    SECTION("accepted and normalised")
    {
        CHECK(validate_spoolman_url("http://host:7912").url == "http://host:7912");
        CHECK(validate_spoolman_url("https://host").url == "https://host");
        CHECK(validate_spoolman_url("http://host:7912/").url == "http://host:7912");
        CHECK(validate_spoolman_url("HTTP://Spool.Lan:7912//").url == "http://spool.lan:7912");
        CHECK(validate_spoolman_url("  http://10.0.0.5:7912  ").url == "http://10.0.0.5:7912");
        CHECK(validate_spoolman_url("http://[fd00::1]:7912").url == "http://[fd00::1]:7912");
        CHECK(validate_spoolman_url("https://h/spoolman/").url == "https://h/spoolman");
    }
    SECTION("empty means unset")
    {
        auto r = validate_spoolman_url("   ");
        CHECK(r.ok);
        CHECK(r.url.empty());
        CHECK(validate_spoolman_url("").ok);
    }
    SECTION("rejected, each with a reason")
    {
        const std::string long_url = "http://" + std::string(300, 'a');
        for (const char *bad : {"ftp://host", "host:7912", "http://", "http://:7912", "http://user:pw@host", "http://user@host",
                                "http://host?x=1", "http://host/#frag", "http://host/a?b", "http://ho st", "http://host\x01", "http://ho\nst",
                                "http://host:0", "http://host:65536", "http://host:abc", "http://host:", "http://ho$t", "http://host\\a",
                                "http://h\xc3\xb6st", "http://[::1", "http://[]", "file:///etc/passwd", "javascript:alert(1)"}) {
            auto r = validate_spoolman_url(bad);
            INFO(bad);
            CHECK_FALSE(r.ok);
            CHECK_FALSE(r.reason.empty());
            CHECK(r.url.empty());
        }
        auto r = validate_spoolman_url(long_url);
        CHECK_FALSE(r.ok);
        CHECK_FALSE(r.reason.empty());
    }
}

namespace {
struct FakeDirect
{
    struct Call { std::string url; };
    std::vector<Call>                                        calls;
    std::map<std::string, std::tuple<int, std::string, std::string>> routes; // url -> {status, location, body}
    DirectFetch fn()
    {
        return [this](const std::string &url, std::string &location, std::string &body) {
            calls.push_back({url});
            auto it = routes.find(url);
            if (it == routes.end())
                return 404;
            location = std::get<1>(it->second);
            body     = std::get<2>(it->second);
            return std::get<0>(it->second);
        };
    }
};
} // namespace

TEST_CASE("Spoolman direct request building", "[Spoolman]")
{
    CHECK(direct_request_url("http://h:7912", "/v1/vendor", "") == "http://h:7912/api/v1/vendor");
    CHECK(direct_request_url("http://h:7912", "/v1/filament", "") == "http://h:7912/api/v1/filament");
    CHECK(direct_request_url("http://h:7912", "/v1/spool", "allow_archived=false") == "http://h:7912/api/v1/spool?allow_archived=false");
    CHECK(direct_request_url("https://h/sp", "/v1/vendor", "") == "https://h/sp/api/v1/vendor");
    // only the three catalog paths exist
    for (const char *p : {"/v1/spool/1", "/v1/vendor/", "/v1/export/spools", "/v1/", "/api/v1/vendor", "v1/vendor", "/v1/filament/../x", ""})
        CHECK(direct_request_url("http://h", p, "").empty());

    FakeDirect fake;
    fake.routes["http://h:7912/api/v1/vendor"]                          = {200, "", "[]"};
    fake.routes["http://h:7912/api/v1/filament"]                        = {200, "", "[]"};
    fake.routes["http://h:7912/api/v1/spool?allow_archived=false"] = {200, "", "[]"};
    auto f = fetch_catalog(make_direct_http_get("http://h:7912", fake.fn()));
    CHECK(f.good());
    REQUIRE(fake.calls.size() == 3);
    CHECK(fake.calls[0].url == "http://h:7912/api/v1/vendor");
    CHECK(fake.calls[1].url == "http://h:7912/api/v1/filament");
    CHECK(fake.calls[2].url == "http://h:7912/api/v1/spool?allow_archived=false");

    // a non-catalog path never reaches the network
    FakeDirect none;
    std::string body;
    CHECK(make_direct_http_get("http://h", none.fn())("/v1/spool/1", "", body) == 0);
    CHECK(none.calls.empty());
}

TEST_CASE("Spoolman direct redirects", "[Spoolman]")
{
    std::string body;
    SECTION("to another host is refused: one request, the 3xx comes back")
    {
        FakeDirect fake;
        fake.routes["http://h:7912/api/v1/vendor"] = {302, "http://evil.example/api/v1/vendor", "x"};
        CHECK(make_direct_http_get("http://h:7912", fake.fn())("/v1/vendor", "", body) == 302);
        CHECK(fake.calls.size() == 1);
        CHECK(body.empty());
    }
    SECTION("other scheme, other port, protocol-relative, relative and bad targets are refused")
    {
        for (const char *loc : {"https://h:7912/api/v1/vendor", "http://h:7913/api/v1/vendor", "//evil/api/v1/vendor", "api/v1/vendor", "",
                                "http://h:7912.evil.example/x", "ftp://h:7912/x", "/a b"}) {
            FakeDirect fake;
            fake.routes["http://h:7912/api/v1/vendor"] = {301, loc, ""};
            INFO(loc);
            CHECK(make_direct_http_get("http://h:7912", fake.fn())("/v1/vendor", "", body) == 301);
            CHECK(fake.calls.size() == 1);
        }
    }
    SECTION("same origin is followed (path-absolute and absolute), at most 2 hops")
    {
        FakeDirect fake;
        fake.routes["http://h:7912/api/v1/vendor"]  = {301, "/api/v1/vendor/", ""};
        fake.routes["http://h:7912/api/v1/vendor/"] = {308, "http://H:7912/api/v1/vendor2", ""};
        fake.routes["http://H:7912/api/v1/vendor2"] = {200, "", "[1]"};
        CHECK(make_direct_http_get("http://h:7912", fake.fn())("/v1/vendor", "", body) == 200);
        CHECK(body == "[1]");
        CHECK(fake.calls.size() == 3);
        FakeDirect loop;
        loop.routes["http://h/api/v1/vendor"] = {302, "/api/v1/vendor", ""};
        CHECK(make_direct_http_get("http://h", loop.fn())("/v1/vendor", "", body) == 302);
        CHECK(loop.calls.size() == 3); // original + 2 hops
    }
}

TEST_CASE("Spoolman direct path keeps the caps and the gate", "[Spoolman]")
{
    // too large / too deep bodies are rejected by fetch_catalog exactly as on the proxy path
    FakeDirect fake;
    fake.routes["http://h/api/v1/vendor"] = {200, "", std::string(kMaxBodyBytes + 1, ' ')};
    auto f = fetch_catalog(make_direct_http_get("http://h", fake.fn()));
    CHECK(f.vendors.status == Slic3r::Spoolman::ParseStatus::Malformed);
    CHECK(f.vendors.reason == "response too large");
    FakeDirect big;
    big.routes["http://h/api/v1/vendor"] = {kStatusTooLarge, "", ""};
    CHECK(fetch_catalog(make_direct_http_get("http://h", big.fn())).vendors.status == Slic3r::Spoolman::ParseStatus::HttpFailure);
    CHECK(kMaxBodyBytes == 4u * 1024u * 1024u);
    CHECK(kMaxBodyDepth == 16);

    // preference off: nothing is requested on the direct path either
    FakeDirect off;
    CatalogCache cache;
    auto skipped = fetch_catalog_if_enabled(false, [] { return std::optional<bool>(true); }, cache, direct_cache_key("http://h"),
                                            make_direct_http_get("http://h", off.fn()));
    CHECK_FALSE(skipped.attempted);
    CHECK(off.calls.empty());
    CHECK(direct_cache_key("http://h") != "dev@http://h");
}

TEST_CASE("Spoolman transport selection falls back to the Moonraker proxy", "[Spoolman]")
{
    CHECK_FALSE(select_spoolman_transport("").direct);
    CHECK_FALSE(select_spoolman_transport("  ").direct);
    CHECK(select_spoolman_transport("").reason.empty());
    auto bad = select_spoolman_transport("ftp://x");
    CHECK_FALSE(bad.direct);
    CHECK_FALSE(bad.reason.empty());
    auto ok = select_spoolman_transport("http://Host:7912/");
    CHECK(ok.direct);
    CHECK(ok.url == "http://host:7912");
    // the proxy encoding is unchanged: POST body always request_method GET, no body member
    auto j = json::parse(moonraker_proxy_request_body("/v1/vendor", ""));
    CHECK(j["request_method"] == "GET");
    CHECK_FALSE(j.contains("body"));
}
