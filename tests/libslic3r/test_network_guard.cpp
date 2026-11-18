#include <catch2/catch_test_macros.hpp>

#include "libslic3r/NetworkGuard.hpp"
#include "libslic3r/AppConfig.hpp"

#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::NetworkGuard;

namespace {
const std::vector<std::string> none;
bool local(const std::string &h, const std::vector<std::string> &allowed = none) { return is_local_network_host(h, allowed); }
} // namespace

TEST_CASE("NetworkGuard: local addresses and names are local", "[NetworkGuard]")
{
    for (const char *h : {"127.0.0.1", "127.255.0.3", "localhost", "LOCALHOST", "foo.localhost", "[::1]", "::1", "10.1.2.3", "172.16.0.1", "172.31.255.255",
                          "192.168.0.10", "169.254.1.1", "[fe80::1]", "[fe80::1%en0]", "fe80::1%en0", "[fd12:3456::1]", "[fc00::1]", "printer", "PRINTER.", "u1.local",
                          "U1.Local.", "nas.lan", "x.home.arpa", "home.arpa", "http://192.168.1.50:7125/printer/info", "https://printer.local/x?y=1",
                          "http://user:pw@10.0.0.1:8080/x", "http://[fe80::1]:7125/", "0x7f.1", "2130706433", "0177.0.0.1", "0xC0A80001", "192.168.1"})
        { CAPTURE(h); CHECK(local(h)); }
}

TEST_CASE("NetworkGuard: public and tricky hosts are not local", "[NetworkGuard]")
{
    for (const char *h : {"8.8.8.8", "1.1.1.1", "172.15.0.1", "172.32.0.1", "192.169.0.1", "169.253.1.1", "11.0.0.1", "0.0.0.0", "example.com", "api.snapmaker.com",
                          "http://192.168.1.1.evil.com", "http://localhost.evil.com", "http://evil.com#@192.168.1.1", "http://evil.com\\@192.168.1.1",
                          "http://192.168.1.1@evil.com/", "[::ffff:192.168.1.1]", "::ffff:127.0.0.1", "10.0.0.1.nip.io", "printer.local.evil.com", 
                          "x.lan.evil.com", "[2001:db8::1]", "[fec0::1]", "", "   ", "http://", "http:///x", "999.1.1.1", "08.0.0.1", "0x", "1.2.3.4.5",
                          "a..local", ".local", "%31%30.0.0.1", "p\xc3\xa4.local", "[::1", "[::1]x", "host:abc", "10.0.0.1 evil.com", "ftp.example.com:21"})
        { CAPTURE(h); CHECK_FALSE(local(h)); }
}

TEST_CASE("NetworkGuard: extract_host handles URL parts", "[NetworkGuard]")
{
    CHECK(extract_host("http://User:Pw@Example.COM:8080/a/b?c=d#e") == "example.com");
    CHECK(extract_host("example.com.") == "example.com");
    CHECK(extract_host("//host/x") == "host");
    CHECK(extract_host("http://[fe80::1]:80/") == "fe80::1");
    CHECK(extract_host("xn--bcher-kva.example") == "xn--bcher-kva.example");
    CHECK(extract_host("http://evil.com#@192.168.1.1") == "evil.com");
}

TEST_CASE("NetworkGuard: user allowed hosts match exactly and case-insensitively", "[NetworkGuard]")
{
    const std::vector<std::string> allowed{"http://Spoolman.Example.com:7912", "printer.example.org", "8.8.4.4"};
    CHECK(local("spoolman.example.com", allowed));
    CHECK(local("https://SPOOLMAN.example.com/api/v1/spool", allowed));
    CHECK(local("http://printer.example.org:7125", allowed));
    CHECK(local("8.8.4.4", allowed));
    CHECK_FALSE(local("evil.spoolman.example.com", allowed));
    CHECK_FALSE(local("spoolman.example.com.evil.com", allowed));
    CHECK_FALSE(local("8.8.8.8", allowed));
    CHECK_FALSE(local("example.org", allowed));
}

TEST_CASE("NetworkGuard: is_request_allowed follows the switch and the host list", "[NetworkGuard]")
{
    set_user_allowed_hosts({"spoolman.example.com"});
    set_internet_access_allowed(false);
    CHECK(is_request_allowed("http://192.168.1.2/x"));
    CHECK(is_request_allowed("https://spoolman.example.com/api"));
    CHECK_FALSE(is_request_allowed("https://api.snapmaker.com/x"));
    set_internet_access_allowed(true);
    CHECK(is_request_allowed("https://api.snapmaker.com/x"));
    set_internet_access_allowed(false);
    set_user_allowed_hosts({});
    CHECK_FALSE(is_request_allowed("https://spoolman.example.com/api"));
}

TEST_CASE("NetworkGuard: redacted_target drops credentials and query", "[NetworkGuard]")
{
    CHECK(redacted_target("https://user:secret@api.example.com/v1/x?token=abc#f") == "https://api.example.com/v1/x");
    CHECK(blocked_message().find("Blocked") == 0);
}

TEST_CASE("NetworkGuard: update URL overrides are ignored while the switch is off", "[NetworkGuard][AppConfig]")
{
    AppConfig cfg;
    cfg.set("version_check_url", "https://example.com/version");
    cfg.set("orca_updater_url", "https://example.com/profiles");
    cfg.set_bool(k_allow_internet_key, false);
    CHECK(cfg.version_check_url() == std::string());
    CHECK(cfg.profile_update_url() == std::string());
    cfg.set_bool(k_allow_internet_key, true);
    CHECK(cfg.version_check_url() == "https://example.com/version");
    CHECK(cfg.profile_update_url() == "https://example.com/profiles");
}

TEST_CASE("NetworkGuard: is_local_address judges the real connect address", "[NetworkGuard]")
{
    for (const char *a : {"127.0.0.1", "127.9.9.9", "10.0.0.1", "172.16.0.1", "172.31.255.254", "192.168.1.1", "169.254.10.10", "::1", "fe80::1", "fe80::1%en0",
                          "febf::1", "fc00::1", "fd12:3456::1", "::ffff:127.0.0.1", "::ffff:192.168.1.5", "::FFFF:10.1.2.3"})
        { CAPTURE(a); CHECK(is_local_address(a)); }
    for (const char *a : {"8.8.8.8", "1.1.1.1", "100.64.0.1", "100.127.255.254", "0.0.0.0", "255.255.255.255", "172.15.0.1", "172.32.0.1", "192.169.1.1",
                          "169.253.1.1", "::", "2001:db8::1", "2606:4700::1111", "fec0::1", "::ffff:8.8.8.8", "::ffff:100.64.0.1", "::ffff:0.0.0.0", "64:ff9b::7f00:1",
                          "::127.0.0.1", "", "garbage", "127.1", "0x7f.1", "1.2.3.4.5", "127.0.0.1 ", "localhost", "[::1]"})
        { CAPTURE(a); CHECK_FALSE(is_local_address(a)); }
}

TEST_CASE("NetworkGuard: is_connect_allowed needs a local address unless the host is user-allowed", "[NetworkGuard]")
{
    const std::vector<std::string> allowed{"http://spoolman.example.com:7912", "printer.example.org"};
    // a local-looking name that DNS resolved to a public address (the DNS-name bypass)
    CHECK_FALSE(is_connect_allowed("printer", "8.8.8.8", none));
    CHECK_FALSE(is_connect_allowed("http://nas.lan/x", "93.184.216.34", none));
    CHECK_FALSE(is_connect_allowed("evil.local", "2606:4700::1111", none));
    CHECK_FALSE(is_connect_allowed("printer", "::ffff:8.8.8.8", none));
    CHECK_FALSE(is_connect_allowed("printer", "100.64.1.1", none));
    CHECK(is_connect_allowed("printer", "192.168.1.9", none));
    CHECK(is_connect_allowed("http://u1.local:7125/x", "fe80::1", none));
    CHECK(is_connect_allowed("printer", "::ffff:10.0.0.2", none));
    // explicitly allowed hosts may resolve anywhere
    CHECK(is_connect_allowed("https://Spoolman.Example.com/api", "93.184.216.34", allowed));
    CHECK(is_connect_allowed("printer.example.org", "2606:4700::1111", allowed));
    CHECK_FALSE(is_connect_allowed("evil.example.org", "93.184.216.34", allowed));
    CHECK_FALSE(is_connect_allowed("", "192.168.1.1", allowed));
    CHECK_FALSE(is_connect_allowed("printer", "", allowed));
}
