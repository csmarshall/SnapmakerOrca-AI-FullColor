#include <catch2/catch_test_macros.hpp>

#include "libslic3r/NetworkGuard.hpp"
#include "slic3r/Utils/Http.hpp"

#include <string>

using namespace Slic3r;

// The guard fires before curl is touched, so these never open a socket.
TEST_CASE("Http refuses a non-local host while network access outside the LAN is off", "[NetworkGuard][Http]")
{
    NetworkGuard::set_user_allowed_hosts({});
    NetworkGuard::set_internet_access_allowed(false);

    bool        completed = false;
    bool        errored   = false;
    std::string message;
    unsigned    status = 99;
    Http::get("https://example.com/secret/path?token=abc")
        .on_complete([&](std::string, unsigned) { completed = true; })
        .on_error([&](std::string, std::string error, unsigned http_status) {
            errored = true;
            message = error;
            status  = http_status;
        })
        .perform_sync();

    CHECK_FALSE(completed);
    CHECK(errored);
    CHECK(message == NetworkGuard::blocked_message());
    CHECK(status == 0);
}

// ---- #139 security review hardening: curl's parser decides the host, the redirect is resolved by curl, the connect address is checked ----

#include "slic3r/Utils/NetworkGuardCurl.hpp"

#include <cstring>
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <sys/un.h>
#endif

namespace {
struct GuardOff
{
    GuardOff(std::vector<std::string> allowed = {})
    {
        NetworkGuard::set_user_allowed_hosts(allowed);
        NetworkGuard::set_internet_access_allowed(false);
    }
    ~GuardOff() { NetworkGuard::set_user_allowed_hosts({}); }
};

// The host curl itself reports for a URL, asked of libcurl directly (not through the guard), or "<rejected>".
std::string curl_host(const std::string &url)
{
    CURLU *h = curl_url();
    std::string out = "<rejected>";
    if (curl_url_set(h, CURLUPART_URL, url.c_str(), CURLU_GUESS_SCHEME) == CURLUE_OK) {
        char *p = nullptr;
        if (curl_url_get(h, CURLUPART_HOST, &p, 0) == CURLUE_OK && p) { out = p; curl_free(p); }
    }
    curl_url_cleanup(h);
    return out;
}
} // namespace

TEST_CASE("NetworkGuard: the host the guard checks is the host curl reports, for tricky URLs", "[NetworkGuard][Http]")
{
    GuardOff off;
    struct Row { const char *url; bool allowed; };
    const std::vector<Row> rows{
        // benign
        {"http://192.168.1.50:7125/printer/info", true}, {"HTTP://Printer.LOCAL/x", true}, {"http://[fe80::1]:7125/", true}, {"printer.local/x", true},
        {"http://printer/", true}, {"http://user:pw@10.0.0.1:8080/x", true}, {"http://10.0.0.1./x", true},
        // userinfo and delimiter tricks: the host is whatever curl says. Browsers read a backslash or %2f as ending the authority (host evil.com);
        // curl 7.75 keeps them in the userinfo and connects to 192.168.1.1, so that is what the guard must classify (and it does: allowed=true).
        {"http://192.168.1.1@evil.com/", false}, {"http://192.168.1.1:80@evil.com/", false}, {"http://evil.com#@192.168.1.1/", false},
        {"http://evil.com?@192.168.1.1/", false}, {"http://evil.com/@192.168.1.1", false}, {"http://evil.com\\@192.168.1.1/", true},
        {"http://192.168.1.1\\@evil.com/", false}, {"http://a@192.168.1.1@evil.com/", false}, {"http://evil.com%2f@192.168.1.1/", true},
        {"//evil.com/x", false}, {"//192.168.1.1@evil.com", false}, {"HtTp://EVIL.com/", false}, {"evil.com", false},
        // whitespace / control characters inside
        {"http://evil.com\t.local/", false}, {"http://evil.com\n.local/", false}, {"http://192.168.1.1 @evil.com/", false}, {" http://192.168.1.1/", false},
        {"http://192.168.1.1/\r\nHost: evil.com", false},
        // encodings, dots, zone ids, IDN, length
        {"http://%31%39%32.168.1.1/", false}, {"http://evil.com./", false}, {"http://evil.com../", false}, {"http://evil.com.local.evil.com/", false},
        {"http://[fe80::1%25en0]/", true}, {"http://[fe80::1%en0]/", true}, {"http://[::ffff:192.168.1.1]/", false}, {"http://[::ffff:8.8.8.8]/", false},
        {"http://b\xc3\xbc" "cher.example/", false}, {"http://xn--bcher-kva.example/", false}, {"http://p\xc3\xa4.local/", false},
        // numeric forms
        {"http://0x7f.1/", true}, {"http://2130706433/", true}, {"http://0177.0.0.1/", true}, {"http://8.8.8.8/", false}, {"http://0x08080808/", false},
        {"http://134744072/", false}, {"http://0.0.0.0/", false},
        // schemes
        {"ftp://192.168.1.1/", false}, {"file:///etc/passwd", false}, {"gopher://192.168.1.1/", false}, {"javascript:alert(1)", false}, {"", false},
    };
    for (const Row &r : rows) {
        const std::string url = r.url;
        CAPTURE(url);
        NetworkGuard::ParsedUrl p;
        const bool parsed = NetworkGuard::parse_url(url, p);
        std::string checked;
        const bool allowed = NetworkGuard::request_allowed(url, &checked);
        CHECK(allowed == r.allowed);
        // #144: the TLS default is the exact complement (local = unverified, everything else incl. unparsable = verified)
        CHECK(NetworkGuard::default_tls_verify(url) == !r.allowed);
        if (parsed && url.find('\0') == std::string::npos) {
            // the host the guard classified is the host libcurl reports for the same input
            CHECK(p.host == curl_host(url));
            // and CURLOPT_URL is set from the normalised URL that was checked: same host again
            CHECK(curl_host(p.url) == p.host);
        }
        if (allowed) CHECK(checked == p.url);
    }
    CHECK(NetworkGuard::request_allowed("http://anything.example/", nullptr) == false);
    // very long hosts and labels: curl rejects or the guard blocks, never allows
    CHECK_FALSE(NetworkGuard::request_allowed("http://" + std::string(5000, 'a') + ".com/"));
    CHECK_FALSE(NetworkGuard::request_allowed(std::string("http://192.168.1.1\0@evil.com/", 29)));
    CHECK_FALSE(NetworkGuard::request_allowed(std::string("http://evil.com\0.local/", 23)));
    CHECK_FALSE(NetworkGuard::request_allowed("http://" + std::string(300, 'a') + ".com/"));
}

TEST_CASE("NetworkGuard: user-allowed host classified from curl's host, switch ON leaves the URL alone", "[NetworkGuard][Http]")
{
    {
        GuardOff off({"http://Spoolman.Example.com:7912"});
        CHECK(NetworkGuard::request_allowed("https://spoolman.example.com:7912/api/v1/spool"));
        CHECK_FALSE(NetworkGuard::request_allowed("https://spoolman.example.com@evil.com/"));
        CHECK_FALSE(NetworkGuard::request_allowed("https://evil.com#@spoolman.example.com/"));
    }
    NetworkGuard::set_internet_access_allowed(true);
    std::string checked;
    CHECK(NetworkGuard::request_allowed("weird url, not parsed", &checked));
    CHECK(checked == "weird url, not parsed");
    NetworkGuard::set_internet_access_allowed(false);
}

TEST_CASE("NetworkGuard: redirects are resolved by curl against the current URL", "[NetworkGuard][Http]")
{
    GuardOff off;
    const std::string base = "http://192.168.1.50:7125/a/b?x=1";
    // stays local
    for (const char *loc : {"/printer/info", "info", "../c", "?q=2", "#frag", "//192.168.1.60/x", "http://printer.local/x", "http://[fe80::1]/", "http://evil.com\\@192.168.1.1/", "HTTP://10.0.0.1/", "http:/relative-looking"})
        { CAPTURE(loc); CHECK(NetworkGuard::redirect_allowed(base, loc)); }
    // leaves the LAN, in every spelling
    for (const char *loc : {"http://evil.com/", "//evil.com/", "https://8.8.8.8/", "http://0x08080808/", "http://134744072/", "http://[2001:db8::1]/",
                            "http://192.168.1.1@evil.com/", "http://evil.com#@192.168.1.1/", "HTTPS://EVIL.com",
                            "ftp://192.168.1.1/", "file:///etc/passwd", "http://evil.com\t.local/", "", "http://", "http://%31.2.3.4/", "http://evil.com./"})
        { CAPTURE(loc); CHECK_FALSE(NetworkGuard::redirect_allowed(base, loc)); }
    // relative to a user-allowed public host stays on that host, absolute elsewhere does not
    GuardOff off2({"spoolman.example.com"});
    CHECK(NetworkGuard::redirect_allowed("https://spoolman.example.com/a", "/b"));
    CHECK_FALSE(NetworkGuard::redirect_allowed("https://spoolman.example.com/a", "https://evil.com/b"));
    CHECK_FALSE(NetworkGuard::redirect_allowed("not a url", "/b"));

    std::string abs;
    REQUIRE(NetworkGuard::resolve_redirect(base, "//other.local/z", abs));
    CHECK(abs == "http://other.local/z");
    REQUIRE(NetworkGuard::resolve_redirect(base, "../c", abs));
    CHECK(abs == "http://192.168.1.50:7125/c");
}

namespace {
// libcurl hands the callback a curl_sockaddr whose storage really is a sockaddr_storage; mirror that so sockaddr_in6 fits.
struct SockAddrBox
{
    curl_sockaddr a;
    char          pad[128];
    SockAddrBox() { std::memset(this, 0, sizeof(*this)); }
    curl_sockaddr *operator&() { return &a; }
};
SockAddrBox make_v4(const char *dotted)
{
    SockAddrBox b;
    b.a.family = AF_INET; b.a.socktype = SOCK_STREAM; b.a.protocol = IPPROTO_TCP; b.a.addrlen = sizeof(sockaddr_in);
    sockaddr_in *sin = reinterpret_cast<sockaddr_in *>(&b.a.addr); sin->sin_family = AF_INET; inet_pton(AF_INET, dotted, &sin->sin_addr);
    return b;
}
SockAddrBox make_v6(const char *text)
{
    SockAddrBox b;
    b.a.family = AF_INET6; b.a.socktype = SOCK_STREAM; b.a.protocol = IPPROTO_TCP; b.a.addrlen = sizeof(sockaddr_in6);
    sockaddr_in6 *sin = reinterpret_cast<sockaddr_in6 *>(&b.a.addr); sin->sin6_family = AF_INET6; inet_pton(AF_INET6, text, &sin->sin6_addr);
    return b;
}
} // namespace

TEST_CASE("NetworkGuard: connect_allowed judges the resolved socket address", "[NetworkGuard][Http]")
{
    GuardOff off({"https://spoolman.example.com"});
    std::string text;
    auto v4 = make_v4("8.8.8.8");
    CHECK_FALSE(NetworkGuard::connect_allowed("http://printer/x", &v4, &text));
    CHECK(text == "8.8.8.8");
    v4 = make_v4("192.168.1.9");
    CHECK(NetworkGuard::connect_allowed("http://printer/x", &v4));
    v4 = make_v4("100.64.0.1");
    CHECK_FALSE(NetworkGuard::connect_allowed("http://printer.local/x", &v4));
    auto v6 = make_v6("::ffff:8.8.8.8");
    CHECK_FALSE(NetworkGuard::connect_allowed("http://printer.lan/x", &v6, &text));
    v6 = make_v6("::ffff:127.0.0.1");
    CHECK(NetworkGuard::connect_allowed("http://printer.lan/x", &v6));
    v6 = make_v6("2606:4700::1111");
    CHECK_FALSE(NetworkGuard::connect_allowed("http://u1.local/x", &v6));
    v6 = make_v6("fe80::1");
    CHECK(NetworkGuard::connect_allowed("http://u1.local/x", &v6));
    // user-allowed host may resolve to a public address
    v4 = make_v4("93.184.216.34");
    CHECK(NetworkGuard::connect_allowed("https://spoolman.example.com/api", &v4));
    CHECK_FALSE(NetworkGuard::connect_allowed("https://spoolman.example.com.evil.com/api", &v4));
    // unusable inputs fail closed
    CHECK_FALSE(NetworkGuard::connect_allowed("", &v4));
    CHECK_FALSE(NetworkGuard::connect_allowed("http://printer/x", nullptr));
    SockAddrBox unix_sock; unix_sock.a.family = AF_UNIX; unix_sock.a.addrlen = 4;
    CHECK_FALSE(NetworkGuard::connect_allowed("http://printer/x", &unix_sock));
    // switch ON: no restriction
    NetworkGuard::set_internet_access_allowed(true);
    v4 = make_v4("8.8.8.8");
    CHECK(NetworkGuard::connect_allowed("http://printer/x", &v4));
    NetworkGuard::set_internet_access_allowed(false);
}

// Drives the real libcurl hook with no network: CURLOPT_RESOLVE injects "DNS", and the hook refuses before any socket() call.
TEST_CASE("NetworkGuard: a local-looking name that resolves to a public address is refused by the curl hook", "[NetworkGuard][Http]")
{
    GuardOff off;
    CURL *curl = curl_easy_init();
    REQUIRE(curl != nullptr);
    char error[CURL_ERROR_SIZE] = {0};
    curl_slist *resolve = curl_slist_append(nullptr, "printer.lan:9:8.8.8.8");
    resolve = curl_slist_append(resolve, "dotless:9:2606:4700::1111");
    NetworkGuard::ConnectGuard guard;
    curl_easy_setopt(curl, CURLOPT_URL, "http://printer.lan:9/x");
    curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve);
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 2L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
    NetworkGuard::install_connect_guard(curl, &guard);
    const CURLcode rc = curl_easy_perform(curl);
    CHECK(rc == CURLE_COULDNT_CONNECT);
    CHECK(guard.blocked);
    CHECK(guard.blocked_address == "8.8.8.8");
    CHECK(guard.blocked_host.find("printer.lan") != std::string::npos);

    curl_easy_setopt(curl, CURLOPT_URL, "http://dotless:9/x");
    NetworkGuard::install_connect_guard(curl, &guard);
    CHECK(curl_easy_perform(curl) == CURLE_COULDNT_CONNECT);
    CHECK(guard.blocked);
    CHECK(guard.blocked_address == "2606:4700::1111");
    curl_slist_free_all(resolve);
    curl_easy_cleanup(curl);
}

// ---- #144: TLS verification default by host class ----

TEST_CASE("default_tls_verify: public hosts verify, LAN and local names do not, unparsable fails closed", "[NetworkGuard][Http][TlsVerify]")
{
    NetworkGuard::set_user_allowed_hosts({});
    for (const char *url : {"https://example.com/", "https://api.github.com/x", "http://8.8.8.8/", "https://[2001:4860:4860::8888]/", "evil.com",
                            "http://192.168.1.1@evil.com/"})
        CHECK(NetworkGuard::default_tls_verify(url));
    for (const char *url : {"https://192.168.1.50/", "https://10.1.2.3:8443/", "https://printer/", "https://u1.local/", "https://u1.lan/",
                            "http://127.0.0.1:7125/", "https://localhost/", "https://[::1]/", "https://[fe80::1]/", "https://172.16.0.9/"})
        CHECK_FALSE(NetworkGuard::default_tls_verify(url));
    for (const char *url : {"", "http://", "ftp://example.com/", "http://exa mple.com/"})
        CHECK(NetworkGuard::default_tls_verify(url));
}

TEST_CASE("default_tls_verify: user-allowed hosts stay unverified", "[NetworkGuard][Http][TlsVerify]")
{
    CHECK(NetworkGuard::default_tls_verify("https://printer.example.com/", {}));
    CHECK_FALSE(NetworkGuard::default_tls_verify("https://printer.example.com/", {"printer.example.com"}));
    CHECK_FALSE(NetworkGuard::default_tls_verify("https://printer.example.com/x", {"https://printer.example.com:7125"}));
    CHECK(NetworkGuard::default_tls_verify("https://other.example.com/", {"printer.example.com"}));
    {
        GuardOff off({"printer.example.com"});
        CHECK_FALSE(NetworkGuard::default_tls_verify("https://printer.example.com/"));
    }
}

// The constructor applies the default, an explicit tls_verify() wins, printer_tls() is the per-printer switch. No socket is opened.
TEST_CASE("Http: constructor TLS default follows the host class and tls_verify() overrides it", "[NetworkGuard][Http][TlsVerify]")
{
    NetworkGuard::set_user_allowed_hosts({});
    CHECK(Http::get("https://example.com/").tls_verify_enabled());
    CHECK_FALSE(Http::get("https://192.168.1.50/").tls_verify_enabled());
    CHECK_FALSE(Http::get("http://printer.local:7125/").tls_verify_enabled());
    CHECK(Http::get("not a url ://").tls_verify_enabled());

    CHECK_FALSE(Http::get("https://example.com/").tls_verify(false).tls_verify_enabled());
    CHECK(Http::get("https://192.168.1.50/").tls_verify(true).tls_verify_enabled());

    // printhost_ssl_verify on: LAN host verifies; off: the host-class default stays
    CHECK(Http::get("https://192.168.1.50/").printer_tls("", true).tls_verify_enabled());
    CHECK_FALSE(Http::get("https://192.168.1.50/").printer_tls("", false).tls_verify_enabled());
    CHECK(Http::get("https://example.com/").printer_tls("", false).tls_verify_enabled());
}
