#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/SSWCP.hpp"

#include <boost/filesystem.hpp>

#include <set>
#include <string>
#include <vector>

using namespace Slic3r::GUI;
using nlohmann::json;
namespace fs = boost::filesystem;

namespace {

struct Harness
{
    std::vector<std::string>                 sent;
    std::chrono::steady_clock::time_point    clock    = std::chrono::steady_clock::now();
    json                                     devices  = json::array();
    SSWCP                                    sswcp;

    Harness() : sswcp(make_services()) {}

    SSWCPServices make_services()
    {
        SSWCPServices s;
        s.sink              = [this](const std::string& js) { sent.push_back(js); };
        s.now               = [this] { return clock; };
        s.local_devices     = [this] { return devices; };
        s.software_name     = "TestApp";
        s.software_version  = "9.9.9";
        return s;
    }

    // Parses the n-th reply back out of its "window.postMessage(JSON.stringify(<json>), '*');" wrapper.
    json reply(std::size_t n) const
    {
        REQUIRE(n < sent.size());
        const std::string prefix = "window.postMessage(JSON.stringify(";
        const std::string suffix = "), '*');";
        const std::string& js    = sent[n];
        REQUIRE(js.rfind(prefix, 0) == 0);
        REQUIRE(js.size() > prefix.size() + suffix.size());
        REQUIRE(js.compare(js.size() - suffix.size(), suffix.size(), suffix) == 0);
        return json::parse(js.substr(prefix.size(), js.size() - prefix.size() - suffix.size()));
    }
    json last() const { return reply(sent.size() - 1); }

    SSWCPOutcome send(const json& header, const std::string& cmd, const json& params = json::object(), const json& event_id = nullptr)
    {
        json payload = {{"cmd", cmd}, {"params", params}};
        if (!event_id.is_null())
            payload["event_id"] = event_id;
        return sswcp.handle_web_message(json{{"header", header}, {"payload", payload}}.dump());
    }
};

const std::vector<std::string> k_subscribe_cmds = {"sw_SubUserUpdatePrivacy", "sw_SubscribePageStateChange", "sw_SubscribeForegroundChange",
                                                   "sw_SubscribeRecentFiles",  "sw_SubscribeLocalDevices",   "sw_SubscribeUserLoginState"};

} // namespace

TEST_CASE("SSWCP one-shot startup commands echo the header and reply 200", "[SSWCP]")
{
    Harness     h;
    const json  hdr = {{"id", 7}, {"seqid", "abc"}};

    SECTION("sw_GetSoftwareInfo") {
        REQUIRE(h.send(hdr, "sw_GetSoftwareInfo") == SSWCPOutcome::Dispatched);
        REQUIRE(h.sent.size() == 1);
        json r = h.last();
        CHECK(r["header"] == hdr);
        CHECK(r["payload"]["code"] == 200);
        CHECK(r["payload"]["data"]["name"] == "TestApp");
        CHECK(r["payload"]["data"]["version"] == "9.9.9");
    }
    SECTION("sw_GetUserLoginState replies an empty object (no user)") {
        h.send(hdr, "sw_GetUserLoginState");
        json r = h.last();
        CHECK(r["header"] == hdr);
        CHECK(r["payload"]["code"] == 200);
        CHECK(r["payload"]["data"] == json::object());
    }
    SECTION("sw_UserLogin replies 200 and nothing else") {
        h.send(hdr, "sw_UserLogin", {{"user", "x"}, {"password", "y"}});
        REQUIRE(h.sent.size() == 1);
        CHECK(h.last()["payload"]["code"] == 200);
    }
    SECTION("sw_GetLocalDevices uses the provider, default empty") {
        h.send(hdr, "sw_GetLocalDevices");
        CHECK(h.last()["payload"]["data"] == json::array());
        h.devices = json::array({json{{"name", "U1"}}});
        h.send(hdr, "sw_GetLocalDevices");
        CHECK(h.last()["payload"]["data"] == h.devices);
        h.devices = "not an array";
        h.send(hdr, "sw_GetLocalDevices");
        CHECK(h.last()["payload"]["data"] == json::array());
    }
    SECTION("sw_FileLog and sw_UploadEvent are accepted") {
        h.send(hdr, "sw_FileLog", {{"level", "info"}, {"content", "hello"}});
        CHECK(h.last()["payload"]["code"] == 200);
        h.send(hdr, "sw_UploadEvent", {{"traceId", "t"}, {"level", 1}, {"content", "c"}});
        CHECK(h.last()["payload"]["code"] == 200);
        CHECK(h.sswcp.instance_count() == 0);
    }
}

TEST_CASE("SSWCP subscriptions ack, keep their event_id and receive pushes", "[SSWCP]")
{
    Harness h;
    for (const auto& cmd : k_subscribe_cmds) {
        CAPTURE(cmd);
        const std::string eid = "ev-" + cmd;
        h.sent.clear();
        REQUIRE(h.send({{"id", 1}}, cmd, json::object(), eid) == SSWCPOutcome::Dispatched);
        REQUIRE(h.sent.size() == 1);
        CHECK(h.reply(0)["header"] == json{{"id", 1}}); // ack echoes the request header
        CHECK(h.reply(0)["payload"]["code"] == 200);

        h.sent.clear();
        CHECK(h.sswcp.push(cmd, json{{"v", 1}}) == 1);
        REQUIRE(h.sent.size() == 1);
        CHECK(h.reply(0)["header"] == json{{"event_id", eid}});
        CHECK(h.reply(0)["payload"]["data"] == json{{"v", 1}});
    }
    CHECK(h.sswcp.subscription_count() == k_subscribe_cmds.size());
    CHECK(h.sswcp.push("sw_SubscribeCacheKey", json::object()) == 0);

    SECTION("subscriptions never expire, one-shots do") {
        h.clock += std::chrono::hours(1);
        CHECK(h.sswcp.expire_due() == 0);
        CHECK(h.sswcp.subscription_count() == k_subscribe_cmds.size());
    }
    SECTION("same event_id replaces, drop_all clears") {
        h.send({{"id", 2}}, "sw_SubscribeRecentFiles", json::object(), "ev-sw_SubscribeRecentFiles");
        CHECK(h.sswcp.subscription_count() == k_subscribe_cmds.size());
        h.sswcp.drop_all_subscriptions();
        CHECK(h.sswcp.subscription_count() == 0);
        CHECK(h.sswcp.instance_count() == 0);
    }
}

TEST_CASE("SSWCP sw_SubscribeCacheKey needs a string key", "[SSWCP]")
{
    Harness h;
    h.send({{"id", 1}}, "sw_SubscribeCacheKey", {{"key", "abc"}}, "ck");
    CHECK(h.sswcp.subscription_count() == 1);
    CHECK(h.last()["payload"]["code"] == 200);

    for (const json& bad : {json::object(), json{{"key", 5}}, json{{"key", ""}}, json{{"key", json::array()}}, json("str")}) {
        CAPTURE(bad.dump());
        Harness g;
        g.send({{"id", 1}}, "sw_SubscribeCacheKey", bad, "ck");
        CHECK(g.last()["payload"]["code"] == 400);
        CHECK(g.last()["header"] == json{{"event_id", "ck"}});
        CHECK(g.sswcp.subscription_count() == 0);
        CHECK(g.sswcp.instance_count() == 0);
    }
}

TEST_CASE("SSWCP subscription without an event_id gets one reply and is not kept", "[SSWCP]")
{
    Harness h;
    h.send({{"id", 3}}, "sw_SubscribeLocalDevices");
    REQUIRE(h.sent.size() == 1);
    CHECK(h.last()["payload"]["code"] == 200);
    CHECK(h.sswcp.subscription_count() == 0);
    CHECK(h.sswcp.instance_count() == 0);
}

TEST_CASE("SSWCP refuses every command not on the allow-list with a 501", "[SSWCP]")
{
    Harness h;
    for (const std::string cmd : {"sw_MachinePrintStart", "sw_DownLoadFile", "sw_OpenBrowser", "sw_Exit", "sw_UploadFiletoMachine", "SW_GETSOFTWAREINFO",
                                  "sw_GetSoftwareInfo ", "sw_Connect", "__proto__", "test"}) {
        CAPTURE(cmd);
        h.sent.clear();
        CHECK_FALSE(h.sswcp.is_registered(cmd));
        REQUIRE(h.send({{"id", 9}}, cmd, {{"x", 1}}) == SSWCPOutcome::Unsupported);
        REQUIRE(h.sent.size() == 1);
        json r = h.last();
        CHECK(r["header"] == json{{"id", 9}});
        CHECK(r["payload"]["code"] == 501);
        CHECK(r["payload"]["message"] == "unsupported command");
        CHECK(r["payload"]["data"] == json{{"cmd", cmd}});
    }
    CHECK(h.sswcp.instance_count() == 0);
    CHECK(h.sswcp.subscription_count() == 0);

    // With an event_id the refusal is still one reply and registers nothing.
    h.sent.clear();
    h.send({{"id", 9}}, "sw_MachinePrintStart", json::object(), "e1");
    REQUIRE(h.sent.size() == 1);
    CHECK(h.last()["payload"]["code"] == 501);
    CHECK(h.sswcp.subscription_count() == 0);
}

TEST_CASE("SSWCP registers exactly the startup commands", "[SSWCP]")
{
    Harness h;
    const std::set<std::string> expect = {"sw_FileLog",          "sw_SubUserUpdatePrivacy",   "sw_SubscribePageStateChange", "sw_SubscribeForegroundChange",
                                          "sw_SubscribeRecentFiles", "sw_SubscribeLocalDevices", "sw_SubscribeCacheKey",       "sw_SubscribeUserLoginState",
                                          "sw_GetUserLoginState", "sw_UserLogin",              "sw_GetSoftwareInfo",         "sw_UploadEvent",
                                          "sw_GetLocalDevices"};
    for (const auto& c : expect)
        CHECK(h.sswcp.is_registered(c));
    CHECK(expect.size() == 13);
}

TEST_CASE("SSWCP malformed input never throws and replies only where a header exists", "[SSWCP]")
{
    Harness h;

    SECTION("not JSON / not an object / no header: dropped silently") {
        for (const std::string m : {"", "not json", "{", "[]", "42", "\"str\"", "null", "{\"payload\":{\"cmd\":\"sw_UserLogin\"}}",
                                    "{\"header\":null,\"payload\":{\"cmd\":\"sw_UserLogin\"}}"}) {
            CAPTURE(m);
            CHECK_NOTHROW(h.sswcp.handle_web_message(m));
            CHECK(h.sswcp.handle_web_message(m) == SSWCPOutcome::Dropped);
        }
        CHECK(h.sent.empty());
    }
    SECTION("invalid UTF-8 bytes") {
        std::string m = "{\"header\":{\"a\":1},\"payload\":{\"cmd\":\"sw_UserLogin\",\"params\":{\"x\":\"\xff\xfe\"}}}";
        CHECK_NOTHROW(h.sswcp.handle_web_message(m));
        CHECK(h.sswcp.handle_web_message(m) == SSWCPOutcome::Dropped); // the parser rejects it; no header to echo
        CHECK(h.sent.empty());
    }
    SECTION("header present, payload or cmd wrong: 400 with the header echoed") {
        const std::vector<std::string> bodies = {
            R"({"header":{"id":1}})",
            R"({"header":{"id":1},"payload":"x"})",
            R"({"header":{"id":1},"payload":[]})",
            R"({"header":{"id":1},"payload":{}})",
            R"({"header":{"id":1},"payload":{"cmd":5}})",
            R"({"header":{"id":1},"payload":{"cmd":null}})",
            R"({"header":{"id":1},"payload":{"cmd":["sw_UserLogin"]}})",
            R"({"header":{"id":1},"payload":{"cmd":""}})",
            R"({"header":{"id":1},"payload":{"cmd":"sw_UserLogin","event_id":5}})",
            R"({"header":{"id":1},"payload":{"cmd":"sw_UserLogin","event_id":{"a":1}}})",
        };
        for (const auto& m : bodies) {
            CAPTURE(m);
            h.sent.clear();
            SSWCPOutcome o{};
            CHECK_NOTHROW(o = h.sswcp.handle_web_message(m));
            CHECK(o == SSWCPOutcome::BadRequest);
            REQUIRE(h.sent.size() == 1);
            CHECK(h.last()["header"] == json{{"id", 1}});
            CHECK(h.last()["payload"]["code"] == 400);
        }
    }
    SECTION("null params and null event_id are treated as absent") {
        CHECK(h.sswcp.handle_web_message(R"({"header":{"id":1},"payload":{"cmd":"sw_UserLogin","params":null,"event_id":null}})") ==
              SSWCPOutcome::Dispatched);
        CHECK(h.last()["payload"]["code"] == 200);
    }
    SECTION("overlong event_id is refused") {
        h.send({{"id", 1}}, "sw_SubscribeRecentFiles", json::object(), std::string(SSWCP_MAX_EVENT_ID_LEN + 1, 'e'));
        CHECK(h.last()["payload"]["code"] == 400);
        CHECK(h.sswcp.subscription_count() == 0);
    }
}

TEST_CASE("SSWCP enforces the message size cap", "[SSWCP]")
{
    Harness h;
    const std::string pad(SSWCP_MAX_MESSAGE_BYTES, 'a');

    // Valid JSON, valid command, but one byte over the limit.
    std::string over = R"({"header":{"id":1},"payload":{"cmd":"sw_UserLogin","params":{"p":")";
    over += pad.substr(0, SSWCP_MAX_MESSAGE_BYTES - over.size() - 4 + 1);
    over += R"("}}})";
    REQUIRE(over.size() > SSWCP_MAX_MESSAGE_BYTES);
    CHECK(h.sswcp.handle_web_message(over) == SSWCPOutcome::TooLarge);
    CHECK(h.sent.empty());

    // The same message trimmed to exactly the limit is accepted.
    std::string at = R"({"header":{"id":1},"payload":{"cmd":"sw_UserLogin","params":{"p":")";
    at += pad.substr(0, SSWCP_MAX_MESSAGE_BYTES - at.size() - 4);
    at += R"("}}})";
    REQUIRE(at.size() == SSWCP_MAX_MESSAGE_BYTES);
    CHECK(h.sswcp.handle_web_message(at) == SSWCPOutcome::Dispatched);
    CHECK(h.sent.size() == 1);
}

TEST_CASE("SSWCP survives deeply nested JSON", "[SSWCP]")
{
    Harness h;
    const int deep = 100000; // would overflow a recursive walker; stays under the 1 MiB cap

    SECTION("nested arrays as the whole message") {
        std::string m(deep, '[');
        m += std::string(deep, ']');
        CHECK(h.sswcp.handle_web_message(m) == SSWCPOutcome::TooDeep);
    }
    SECTION("nested objects inside params") {
        std::string m = R"({"header":{"id":1},"payload":{"cmd":"sw_UserLogin","params":)";
        for (int i = 0; i < deep; ++i) m += R"({"a":)";
        m += "1";
        m += std::string(deep, '}');
        m += "}}";
        REQUIRE(m.size() < SSWCP_MAX_MESSAGE_BYTES);
        CHECK(h.sswcp.handle_web_message(m) == SSWCPOutcome::TooDeep);
    }
    SECTION("unterminated nesting") {
        CHECK(h.sswcp.handle_web_message(std::string(deep, '[')) == SSWCPOutcome::TooDeep);
    }
    SECTION("brackets inside strings do not count, nesting at the limit passes") {
        json hdr = std::string(3000, '[');
        CHECK(h.send(hdr, "sw_UserLogin") == SSWCPOutcome::Dispatched);

        json nested = 1;
        for (int i = 0; i < SSWCP_MAX_JSON_DEPTH - 4; ++i) nested = json::array({nested});
        CHECK(h.send({{"id", 1}}, "sw_UserLogin", nested) == SSWCPOutcome::Dispatched);

        json too = 1;
        for (int i = 0; i < SSWCP_MAX_JSON_DEPTH + 1; ++i) too = json::array({too});
        CHECK(h.send({{"id", 1}}, "sw_UserLogin", too) == SSWCPOutcome::TooDeep);
    }
    CHECK(h.sswcp.instance_count() == 0);
}

TEST_CASE("SSWCP sw_FileLog with path-like content never touches the filesystem", "[SSWCP]")
{
    Harness h;
    const fs::path dir = fs::temp_directory_path() / fs::unique_path("sswcp-filelog-%%%%-%%%%");
    fs::create_directories(dir);
    const fs::path victim = dir / "victim.txt";
    const fs::path cwd_before = fs::current_path();
    auto count_entries = [](const fs::path& p) { return std::distance(fs::directory_iterator(p), fs::directory_iterator()); };
    const auto cwd_count = count_entries(cwd_before);

    const json p = {{"level", "debug"},
                    {"content", "../../../../etc/passwd\r\nFORGED LOG LINE " + victim.string()},
                    {"path", victim.string()},
                    {"file", victim.string()},
                    {"filename", "../../escape.log"}};
    CHECK(h.send({{"id", 1}}, "sw_FileLog", p) == SSWCPOutcome::Dispatched);
    CHECK(h.last()["payload"]["code"] == 200);
    CHECK_FALSE(fs::exists(victim));
    CHECK(count_entries(dir) == 0);
    CHECK(count_entries(cwd_before) == cwd_count);

    // Non-object params and non-string fields are tolerated.
    h.send({{"id", 1}}, "sw_FileLog", json::array({1, 2}));
    CHECK(h.last()["payload"]["code"] == 200);
    h.send({{"id", 1}}, "sw_FileLog", {{"level", 5}, {"content", json::object()}});
    CHECK(h.last()["payload"]["code"] == 200);

    fs::remove_all(dir);
}

TEST_CASE("SSWCP sw_UploadEvent is dropped: one empty reply each, nothing echoed (the core has no network code)", "[SSWCP]")
{
    Harness h;
    const json p = {{"traceId", "t-1"}, {"level", 3}, {"content", "secret telemetry"}, {"funcModule", "m"}, {"tagKey", "k"}, {"tagValue", "v"}};
    for (int i = 0; i < 5; ++i)
        CHECK(h.send({{"id", i}}, "sw_UploadEvent", p) == SSWCPOutcome::Dispatched);
    REQUIRE(h.sent.size() == 5);
    for (std::size_t i = 0; i < 5; ++i)
        CHECK(h.reply(i)["payload"]["data"] == json::object());
    CHECK(h.sent.back().find("secret telemetry") == std::string::npos);
}

TEST_CASE("SSWCP replies are one JS statement even for hostile text", "[SSWCP]")
{
    Harness h;
    // U+2028, quote, backslash, closing script in the echoed header.
    h.send(json{{"x", "\xe2\x80\xa8</script>\"\\'"}}, "sw_UserLogin");
    REQUIRE(h.sent.size() == 1);
    CHECK(h.sent[0].find('\n') == std::string::npos);
    for (unsigned char c : h.sent[0])
        CHECK(c < 0x80);
    CHECK(h.last()["header"]["x"] == "\xe2\x80\xa8</script>\"\\'");

    // Unknown cmd text is echoed in data.cmd with the same escaping.
    h.sent.clear();
    h.send({{"id", 1}}, "evil');alert(1);//\xe2\x80\xa9");
    REQUIRE(h.sent.size() == 1);
    for (unsigned char c : h.sent[0])
        CHECK(c < 0x80);
}

TEST_CASE("SSWCP subscription cap", "[SSWCP]")
{
    Harness h;
    for (std::size_t i = 0; i < SSWCP_MAX_SUBSCRIPTIONS; ++i)
        h.send({{"id", 1}}, "sw_SubscribeRecentFiles", json::object(), "e" + std::to_string(i));
    CHECK(h.sswcp.subscription_count() == SSWCP_MAX_SUBSCRIPTIONS);
    h.sent.clear();
    h.send({{"id", 1}}, "sw_SubscribeRecentFiles", json::object(), "one-too-many");
    CHECK(h.sswcp.subscription_count() == SSWCP_MAX_SUBSCRIPTIONS);
    CHECK(h.last()["payload"]["code"] == 429);
    // Re-subscribing an existing id is still allowed.
    h.send({{"id", 1}}, "sw_SubscribeRecentFiles", json::object(), "e0");
    CHECK(h.last()["payload"]["code"] == 200);
    CHECK(h.sswcp.subscription_count() == SSWCP_MAX_SUBSCRIPTIONS);
}

TEST_CASE("SSWCP without a sink does not crash", "[SSWCP]")
{
    SSWCP s{SSWCPServices{}};
    CHECK(s.handle_web_message(R"({"header":{"id":1},"payload":{"cmd":"sw_GetSoftwareInfo"}})") == SSWCPOutcome::Dispatched);
    CHECK(s.handle_web_message(R"({"header":{"id":1},"payload":{"cmd":"nope"}})") == SSWCPOutcome::Unsupported);
    CHECK(s.push("sw_SubscribeRecentFiles", json::object()) == 0);
}

TEST_CASE("SSWCP one-shot instances time out, subscriptions do not", "[SSWCP]")
{
    Harness h;
    // A command whose handler never replies, as a future asynchronous command would.
    h.sswcp.register_command("test_never_finishes", [](SSWCP_Instance&) {}, false);
    h.send({{"id", 4}}, "test_never_finishes");
    h.send({{"id", 5}}, "sw_SubscribeRecentFiles", json::object(), "sub");
    REQUIRE(h.sent.size() == 1); // only the subscription ack
    CHECK(h.sswcp.instance_count() == 2);

    h.clock += SSWCP::DEFAULT_INSTANCE_TIMEOUT - std::chrono::milliseconds(1);
    CHECK(h.sswcp.expire_due() == 0);
    h.clock += std::chrono::milliseconds(2);
    CHECK(h.sswcp.expire_due() == 1);
    REQUIRE(h.sent.size() == 2);
    CHECK(h.last()["header"] == json{{"id", 4}});
    CHECK(h.last()["payload"]["code"] == -2);
    CHECK(h.sswcp.instance_count() == 1);
    CHECK(h.sswcp.subscription_count() == 1);
    CHECK(h.sswcp.expire_due() == 0);
}

TEST_CASE("SSWCP a one-shot command with an event_id still expires; only subscriptions are long-lived", "[SSWCP]")
{
    Harness h;
    h.sswcp.register_command("test_never_finishes", [](SSWCP_Instance&) {}, false);
    h.send({{"id", 1}}, "test_never_finishes", json::object(), "has-event-id");
    h.send({{"id", 2}}, "sw_SubscribeRecentFiles", json::object(), "sub");
    const std::size_t sent_before = h.sent.size();
    CHECK(h.sswcp.instance_count() == 2);
    h.clock += SSWCP::DEFAULT_INSTANCE_TIMEOUT + std::chrono::milliseconds(1);
    CHECK(h.sswcp.expire_due() == 1);                  // the one-shot, despite its event_id
    REQUIRE(h.sent.size() == sent_before + 1);
    CHECK(h.last()["payload"]["code"] == -2);
    CHECK(h.sswcp.instance_count() == 1);              // the subscription stays
    CHECK(h.sswcp.subscription_count() == 1);
}

TEST_CASE("SSWCP caps pending requests with a structured 429 and bounds the kept header", "[SSWCP]")
{
    Harness h;
    h.sswcp.register_command("test_never_finishes", [](SSWCP_Instance&) {}, false);
    for (std::size_t i = 0; i < SSWCP_MAX_PENDING; ++i)
        REQUIRE(h.send({{"id", i}}, "test_never_finishes") == SSWCPOutcome::Dispatched);
    CHECK(h.sswcp.instance_count() == SSWCP_MAX_PENDING);
    h.sent.clear();
    CHECK(h.send({{"id", "over"}}, "test_never_finishes") == SSWCPOutcome::Busy);
    REQUIRE(h.sent.size() == 1);
    CHECK(h.last()["payload"]["code"] == 429);
    CHECK(h.last()["header"] == json{{"id", "over"}});
    CHECK(h.sswcp.instance_count() == SSWCP_MAX_PENDING);
    // Subscriptions and unknown commands are held to the same bound.
    CHECK(h.send({{"id", 1}}, "sw_SubscribeRecentFiles", json::object(), "x") == SSWCPOutcome::Busy);
    // Once the pending ones expire there is room again.
    h.clock += SSWCP::DEFAULT_INSTANCE_TIMEOUT + std::chrono::milliseconds(1);
    h.sswcp.expire_due();
    CHECK(h.send({{"id", 2}}, "sw_UserLogin") == SSWCPOutcome::Dispatched);

    // A header bigger than SSWCP_MAX_HEADER_BYTES is refused, with an empty header since it is not echoed.
    h.sent.clear();
    json big = std::string(SSWCP_MAX_HEADER_BYTES + 10, 'h');
    CHECK(h.send(big, "sw_UserLogin") == SSWCPOutcome::BadRequest);
    REQUIRE(h.sent.size() == 1);
    CHECK(h.last()["payload"]["code"] == 400);
    CHECK(h.last()["header"] == json::object());
}
