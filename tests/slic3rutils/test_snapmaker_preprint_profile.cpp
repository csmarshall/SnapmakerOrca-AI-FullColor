#include <algorithm>
#include <atomic>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <boost/filesystem.hpp>

#include "slic3r/GUI/SnapmakerDeviceBridge.hpp"
#include "slic3r/GUI/SnapmakerPreprintSession.hpp"
#include "slic3r/Utils/SnapmakerPreprintMapping.hpp"

// S4b: the pre-print bridge profile wired to the gate. Everything printer-facing is a FAKE (FakePrinter): no socket is opened. These
// tests show the profile calls the printer interface in the intended order, with the intended strings, and refuses what the gate refuses.
// They cannot show that real Moonraker/Klipper/firmware answers the way the fake does, that the vendored page sends these shapes in this
// order, or that MoonrakerPrinterAgent's adapter (not built here) reads the same pin the gate pinned.

using namespace Slic3r;
using namespace Slic3r::GUI;
using nlohmann::json;
using Known = SnapmakerPreprintPrinter::Known;

namespace {
const std::vector<int> kMapping   = {2, 0}; // filament 0 -> extruder 2, filament 1 -> extruder 0
const std::string      kFile      = "plate_1.gcode";
const std::uint64_t    kSize      = 1234;
const std::string      kSha       = "00ff10203040506070809000a0b0c0d0e0f0112233445566778899aabbccddee";
const std::string      kCap       = "/web/flutter_web/cap/TOKEN/plate_1.gcode";
const std::vector<std::pair<std::string, int>> kPrefs = {{"bed_level", 1}, {"flow_calibrate", 0}};

std::string plural() { return build_snapmaker_plural_preference_script(kMapping, kPrefs); }
std::string plural_no_prefs() { return build_snapmaker_plural_preference_script(kMapping, {}); }
std::string singular() { return build_snapmaker_singular_preference_script(kMapping, true, false, true); }

struct FakePrinter : SnapmakerPreprintPrinter
{
    std::mutex                 m;
    std::vector<std::string>   calls;     // every call in order, "kind:arg"
    SnapmakerPreprintPin       pin{"u1.local", "7125", "SN123"};
    std::optional<std::string> state = std::string("standby");
    std::optional<std::uint64_t> size = kSize;
    std::map<std::string, Known> known;   // absent = Yes
    bool                       send_ok = true, start_ok = true;
    std::function<void()>      in_send, in_start; // run INSIDE the printer call (a second click arriving while one is in flight)
    int                        sends = 0, starts = 0;

    void rec(const std::string& s) { std::lock_guard<std::mutex> l(m); calls.push_back(s); }
    SnapmakerPreprintPin connected_pin() override { rec("pin"); std::lock_guard<std::mutex> l(m); return pin; }
    std::optional<std::string> print_state() override { rec("state"); std::lock_guard<std::mutex> l(m); return state; }
    std::optional<std::uint64_t> remote_file_size(const std::string& f) override { rec("size:" + f); std::lock_guard<std::mutex> l(m); return size; }
    Known command_known(const std::string& c) override { rec("known:" + c); std::lock_guard<std::mutex> l(m); auto it = known.find(c); return it == known.end() ? Known::Yes : it->second; }
    bool send_gcode(const std::string& s) override { rec("send:" + s); { std::lock_guard<std::mutex> l(m); ++sends; } if (in_send) in_send(); return send_ok; }
    bool start_print(const std::string& f) override { rec("start:" + f); { std::lock_guard<std::mutex> l(m); ++starts; } if (in_start) in_start(); return start_ok; }
    std::vector<std::string> sent_scripts() { std::vector<std::string> o; for (auto& c : calls) if (c.rfind("send:", 0) == 0) o.push_back(c.substr(5)); return o; }
    std::vector<std::string> kinds() { std::vector<std::string> o; for (auto& c : calls) o.push_back(c.substr(0, c.find(':') == std::string::npos ? c.size() : c.find(':'))); return o; }
    void forget() { calls.clear(); }
};

SnapmakerPreprintPlate plate()
{
    SnapmakerPreprintPlate p;
    p.filament_colour       = {"#FF0000", "#00FF00"};
    p.filament_type         = {"PLA", "PETG"};
    p.nozzle_diameter       = {0.4, 0.4, 0.4, 0.4};
    p.filament_extruder_map = {{0, 2}, {1, 0}};
    p.machine_model         = "Snapmaker U1";
    p.display_name          = kFile;
    return p;
}

struct Profile
{
    std::shared_ptr<FakePrinter>              printer = std::make_shared<FakePrinter>();
    std::shared_ptr<SnapmakerPreprintContext> ctx     = std::make_shared<SnapmakerPreprintContext>();
    std::shared_ptr<SnapmakerPreprintSession> session = std::make_shared<SnapmakerPreprintSession>(ctx, printer);
    std::vector<std::string>                  sent;
    std::vector<bool>                         done_calls;
    std::vector<std::function<void()>>        posted; // F3: the fixture's "CallAfter": preprint_done is only ever queued here by a handler
    void pump() { auto q = std::move(posted); posted.clear(); for (auto& f : q) f(); }
    std::vector<std::pair<std::function<void()>, std::function<void()>>> held;
    bool                                      hold = false;
    std::uint64_t                             epoch = 0;
    std::unique_ptr<SnapmakerDeviceBridge>    bridge;
    const std::string                         good = "http://127.0.0.1:5000/web/flutter_web/index.html";

    explicit Profile(bool configure_required = true, bool make_bridge = true)
    {
        std::string err;
        REQUIRE(ctx->set(plate(), {}, err));
        REQUIRE(open(configure_required));
        if (make_bridge) bridge = make();
    }
    std::uint64_t open(bool configure_required = true, const std::string& remote = kFile)
    {
        SnapmakerPreprintSession::Open o;
        o.gate.printer            = printer->pin;
        o.gate.remote_filename    = remote;
        o.gate.plate_filaments    = {0, 1};
        o.gate.extruder_count     = 4;
        o.gate.snapshot_size      = kSize;
        o.gate.configure_required = configure_required;
        o.capability_path         = kCap;
        o.sha256_hex              = kSha;
        std::string err;
        epoch = session->open(o, err);
        return epoch;
    }
    std::unique_ptr<SnapmakerDeviceBridge> make(std::uint64_t e = ~std::uint64_t(0), const std::function<void(SnapmakerDeviceBridge::Options&)>& tweak = nullptr)
    {
        SnapmakerDeviceBridge::Options o;
        o.port             = 5000;
        o.http_host        = "http://127.0.0.1:5000";
        o.preprint         = ctx;
        o.preprint_session = session;
        o.preprint_epoch   = e == ~std::uint64_t(0) ? epoch : e;
        o.run_script       = [this](const std::string& js) { sent.push_back(js); };
        o.preprint_done    = [this](bool started) { done_calls.push_back(started); };
        o.preprint_post    = [this](std::function<void()> fn) { posted.push_back(std::move(fn)); };
        o.preprint_run     = [this](std::function<void()> work, std::function<void()> on_ui) {
            if (hold) held.emplace_back(std::move(work), std::move(on_ui));
            else { work(); on_ui(); }
        };
        if (tweak) tweak(o);
        return std::make_unique<SnapmakerDeviceBridge>(std::move(o));
    }
    json reply(std::size_t n) const
    {
        const std::string prefix = "window.postMessage(JSON.stringify(", suffix = "), '*');";
        return json::parse(sent.at(n).substr(prefix.size(), sent.at(n).size() - prefix.size() - suffix.size()));
    }
    json call(const std::string& cmd, const json& params = json::object(), SnapmakerDeviceBridge* b = nullptr)
    {
        if (!b) b = bridge.get();
        const std::size_t before = sent.size();
        b->on_script_message(good, json{{"header", {{"id", 1}}}, {"payload", {{"cmd", cmd}, {"params", params}}}}.dump());
        REQUIRE(sent.size() > before);        // a nested click (made from inside a fake printer call) replies first; this reply is the last
        if (!nested_ok) REQUIRE(sent.size() == before + 1);
        return reply(sent.size() - 1)["payload"];
    }
    bool nested_ok = false;
    json gcodes(const std::string& script) { return call("sw_SendGCodes", {{"script", script}}); }
    json start(const std::string& f = kFile) { return call("sw_MachinePrintStart", {{"filename", f}}); }
};
bool is_ok(const json& r) { return r["code"] == 200; }
} // namespace

TEST_CASE("pre-print profile: happy path in order, one send and one start reach the fake", "[SSWCP][preprint_profile]")
{
    Profile h;
    const json file = h.call("sw_GetActiveFile");
    REQUIRE(is_ok(file));
    const json d = file["data"];
    CHECK(d["url"] == "http://127.0.0.1:5000" + kCap);
    CHECK(d["origin_size"] == kSize);
    CHECK(d["checksum"] == "AP8QIDBAUGBwgJAAoLDA0ODwESIzRFVmd4iZqrvM3e4="); // base64 of the 32 bytes of kSha
    CHECK(d["file_name"] == kFile);
    std::set<std::string> keys;
    for (auto it = d.begin(); it != d.end(); ++it) keys.insert(it.key());
    CHECK(keys == std::set<std::string>{"file_name", "metadata", "url", "origin_size", "checksum", "is_zip"}); // no filesystem path
    const json map = h.call("sw_GetFileFilamentMapping");
    REQUIRE(is_ok(map));
    CHECK(map["data"]["filename"] == kFile);
    CHECK(h.printer->calls.empty()); // reads touch nothing

    REQUIRE(is_ok(h.gcodes(plural())));
    CHECK(h.printer->kinds() == std::vector<std::string>{"pin", "state", "known", "known", "known", "send"});
    CHECK(h.printer->calls[2] == "known:SET_PRINT_EXTRUDER_MAP");
    CHECK(h.printer->calls[3] == "known:SET_PRINT_USED_EXTRUDERS");
    CHECK(h.printer->calls[4] == "known:SET_PRINT_PREFERENCES");
    CHECK(h.printer->sent_scripts() == std::vector<std::string>{plural()});
    h.printer->forget();

    REQUIRE(is_ok(h.start()));
    CHECK(h.printer->calls == std::vector<std::string>{"pin", "state", "size:" + kFile, "start:" + kFile});
    CHECK(h.printer->sends == 1);
    CHECK(h.printer->starts == 1);

    REQUIRE(is_ok(h.call("sw_SetFilamentMappingComplete", {{"status", "success"}, {"message", "ok"}})));
    CHECK(h.done_calls.empty()); // F3: nothing fired inside the handler
    h.pump();
    CHECK(h.done_calls == std::vector<bool>{true});
    CHECK(h.session->state() == SnapmakerPreprintState::Closed);
    CHECK(h.printer->sends == 1);
    CHECK(h.printer->starts == 1);
}

TEST_CASE("pre-print profile: a script that is not one of the two shapes is refused and nothing is sent or probed", "[SSWCP][preprint_profile]")
{
    Profile h;
    const std::vector<json> bad = {
        json::array({"G28"}),                                   // not a string
        json{{"script", json::array({plural_no_prefs()})}},     // array of lines
        json{{"script", "G28"}}, json{{"script", "M104 S200"}}, json{{"script", plural() + "\nG28"}},
        json{{"script", "SET_PRINT_PREFERENCES BED_LEVEL=1"}},  // preferences without a map
        json{{"script", plural_no_prefs() + "; x"}},
        json{{"script", ""}}, json::object(), json{{"scr", plural()}},
        json{{"script", build_snapmaker_plural_preference_script({2, 2, 0}, {})}}, // CONFIG_EXTRUDER set differs from the plate's
    };
    for (const auto& p : bad) {
        CAPTURE(p.dump());
        h.printer->forget();
        const json r = h.call("sw_SendGCodes", p);
        CHECK(r["code"] == SSWCPCode::Failure);
        CHECK(h.printer->sends == 0);
        for (const auto& c : h.printer->calls) CHECK(c.rfind("known:", 0) != 0);
    }
    CHECK(h.printer->sends == 0);
    // not consumed: a good script still goes through afterwards
    CHECK(is_ok(h.gcodes(plural())));
    CHECK(h.printer->sends == 1);
}

TEST_CASE("pre-print profile: the singular shape is accepted and probes SET_PRINT_PREFERENCE", "[SSWCP][preprint_profile]")
{
    Profile h;
    REQUIRE(is_ok(h.gcodes(singular())));
    CHECK(h.printer->calls[4] == "known:SET_PRINT_PREFERENCE");
    CHECK(h.printer->sent_scripts() == std::vector<std::string>{singular()});
}

TEST_CASE("pre-print profile: start while printing, paused or unreadable is refused; a later idle start works", "[SSWCP][preprint_profile]")
{
    Profile h;
    REQUIRE(is_ok(h.gcodes(plural())));
    for (const char* s : {"printing", "paused", "weird", ""}) {
        CAPTURE(s);
        h.printer->state = std::string(s);
        h.printer->forget();
        CHECK(h.start()["code"] == SSWCPCode::Failure);
        CHECK(h.printer->starts == 0);
    }
    h.printer->state = std::nullopt; // GET failed
    CHECK(h.start()["code"] == SSWCPCode::Failure);
    CHECK(h.printer->starts == 0);
    // configure while printing: nothing sent either
    Profile g;
    g.printer->state = std::string("printing");
    CHECK(g.gcodes(plural())["code"] == SSWCPCode::Failure);
    CHECK(g.printer->sends == 0);
    for (const char* s : {"idle", "standby", "complete", "cancelled", "error"}) {
        CAPTURE(s);
        Profile k;
        k.printer->state = std::string(s);
        REQUIRE(is_ok(k.gcodes(plural())));
        CHECK(is_ok(k.start()));
    }
    h.printer->state = std::string("complete");
    CHECK(is_ok(h.start())); // refusals did not spend the start
    CHECK(h.printer->starts == 1);
}

TEST_CASE("pre-print profile: the printer state is asked fresh at every request", "[SSWCP][preprint_profile]")
{
    Profile h;
    REQUIRE(is_ok(h.gcodes(plural())));
    h.printer->forget();
    h.start("other.gcode");           // refused by filename before any printer fact is read
    h.printer->state = std::string("printing");
    h.start();
    h.printer->state = std::string("standby");
    h.start();
    int states = 0;
    for (const auto& c : h.printer->calls) states += c == "state";
    CHECK(states == 2);
}

TEST_CASE("pre-print profile: a size mismatch or a missing remote file refuses the start", "[SSWCP][preprint_profile]")
{
    Profile h;
    REQUIRE(is_ok(h.gcodes(plural())));
    h.printer->size = kSize + 1;
    CHECK(h.start()["code"] == SSWCPCode::Failure);
    h.printer->size = kSize - 1;
    CHECK(h.start()["code"] == SSWCPCode::Failure);
    h.printer->size = std::nullopt;
    CHECK(h.start()["code"] == SSWCPCode::Failure);
    CHECK(h.printer->starts == 0);
    h.printer->size = kSize;
    CHECK(is_ok(h.start()));
    CHECK(h.printer->starts == 1);
}

TEST_CASE("pre-print profile: only the pinned file name starts; params are strict", "[SSWCP][preprint_profile]")
{
    Profile h;
    REQUIRE(is_ok(h.gcodes(plural())));
    h.printer->forget();
    for (const json& p : {json{{"filename", "other.gcode"}}, json{{"filename", "PLATE_1.gcode"}}, json{{"filename", kFile + " "}}, json{{"filename", "../" + kFile}},
                          json{{"filename", ""}}, json{{"filename", 7}}, json{{"filename", json::array({kFile})}}, json::object(), json{{"file", kFile}}}) {
        CAPTURE(p.dump());
        CHECK(h.call("sw_MachinePrintStart", p)["code"] == SSWCPCode::Failure);
    }
    CHECK(h.printer->starts == 0);
    for (const auto& c : h.printer->calls) CHECK(c.rfind("size:", 0) != 0); // refused before the printer was asked about a file
}

TEST_CASE("pre-print profile: a second start is refused, and so is a script after the start", "[SSWCP][preprint_profile]")
{
    Profile h;
    REQUIRE(is_ok(h.gcodes(plural())));
    REQUIRE(is_ok(h.start()));
    CHECK(h.start()["code"] == SSWCPCode::Failure);
    CHECK(h.gcodes(plural())["code"] == SSWCPCode::Failure);
    CHECK(h.printer->starts == 1);
    CHECK(h.printer->sends == 1);
}

TEST_CASE("pre-print profile: a failed configure forward rolls back; a start is refused until a script went through", "[SSWCP][preprint_profile]")
{
    Profile h(/*configure_required=*/true);
    h.printer->send_ok = false;
    CHECK(h.gcodes(plural())["code"] == SSWCPCode::Failure);
    CHECK(h.session->state() == SnapmakerPreprintState::Open);
    CHECK(h.start()["code"] == SSWCPCode::Failure);
    CHECK(h.printer->starts == 0);
    h.printer->send_ok = true;
    CHECK(is_ok(h.gcodes(plural()))); // a failed forward does not spend the configure budget
    CHECK(is_ok(h.start()));
    CHECK(h.printer->starts == 1);
}

TEST_CASE("pre-print profile: a failed start forward is rolled back and may be retried once", "[SSWCP][preprint_profile]")
{
    Profile h;
    REQUIRE(is_ok(h.gcodes(plural())));
    h.printer->start_ok = false;
    CHECK(h.start()["code"] == SSWCPCode::Failure);
    CHECK(h.session->state() == SnapmakerPreprintState::Configured);
    h.printer->start_ok = true;
    CHECK(is_ok(h.start()));
    CHECK(h.start()["code"] == SSWCPCode::Failure);
    CHECK(h.printer->starts == 2);
}

TEST_CASE("pre-print profile: with no options to configure, a start needs no script", "[SSWCP][preprint_profile]")
{
    Profile h(/*configure_required=*/false);
    CHECK(is_ok(h.start()));
    CHECK(h.printer->sends == 0);
    CHECK(h.printer->starts == 1);
}

TEST_CASE("pre-print profile: StartLocalPrint answers -1 failure; the page's fallback (forced script, then printer.print.start) works", "[SSWCP][preprint_profile]")
{
    Profile h;
    const json r = h.call("sw_StartLocalPrint", {{"type", "local"}, {"path", kFile}, {"options", json::object()}});
    CHECK(r["code"] == -1);
    CHECK(r["message"] == "failure");
    CHECK(h.printer->calls.empty());
    CHECK(h.printer->starts == 0);
    REQUIRE(is_ok(h.gcodes(plural())));   // forced R7
    REQUIRE(is_ok(h.start()));            // n5 -> printer.print.start
    CHECK(h.printer->sends == 1);
    CHECK(h.printer->starts == 1);
    // #142: the page's identical re-sends are answered ok but forwarded only once, however many there are
    Profile g;
    CHECK(is_ok(g.gcodes(plural())));
    CHECK(is_ok(g.gcodes(plural())));
    CHECK(is_ok(g.gcodes(plural())));
    CHECK(g.printer->sends == 1);
}

TEST_CASE("pre-print session: an identical repeat of the accepted script is ok without a second forward or budget (#142)", "[SSWCP][preprint_profile]")
{
    const std::vector<int> identity = {0, 1};
    const std::string      identity_script = build_snapmaker_plural_preference_script(identity, {});
    const auto             no_map_cmds = [](Profile& h) {
        h.printer->known["SET_PRINT_EXTRUDER_MAP"]   = Known::No;
        h.printer->known["SET_PRINT_USED_EXTRUDERS"] = Known::No;
    };
    SECTION("the real page sequence: script, script, StartLocalPrint -1, script, print.start (identity map dropped, nothing forwarded)") {
        Profile h;
        no_map_cmds(h);
        std::vector<std::string> notices;
        h.session->set_notifier([&notices](const std::string& m) { notices.push_back(m); });
        const json a = h.gcodes(identity_script);
        const json b = h.gcodes(identity_script);
        CHECK(h.call("sw_StartLocalPrint", {{"type", "local"}, {"path", kFile}, {"options", json::object()}})["code"] == -1);
        const json c = h.gcodes(identity_script);
        for (const json* r : {&a, &b, &c}) {
            CHECK(is_ok(*r));
            CHECK((*r)["data"]["identity_map_assumed"] == true);
        }
        CHECK(h.printer->sends == 0);
        CHECK(notices.size() == 1); // the user is told once
        h.printer->forget();
        CHECK(is_ok(h.start()));
        CHECK(h.printer->starts == 1);
        // print.start still passes every gate: a fresh idle read, the pin, the remote size
        const auto k = h.printer->kinds();
        CHECK(std::count(k.begin(), k.end(), "state") == 1);
        CHECK(std::count(k.begin(), k.end(), "size") == 1);
        CHECK(std::count(k.begin(), k.end(), "pin") >= 1);
    }
    SECTION("a forwarded script is forwarded once; its repeats are ok and not sent, even while the printer is busy") {
        Profile h;
        REQUIRE(is_ok(h.gcodes(plural())));
        h.printer->state = std::string("printing");
        h.printer->forget();
        CHECK(is_ok(h.gcodes(plural())));
        CHECK(is_ok(h.gcodes(plural())));
        CHECK(h.printer->sends == 1);
        CHECK(h.printer->calls.empty()); // a repeat contacts nothing
        CHECK(h.start()["code"] == SSWCPCode::Failure); // a busy printer still refuses the start
        CHECK(h.printer->starts == 0);
    }
    SECTION("a different second script is not a repeat: it takes the budget and is forwarded; a third distinct one is refused") {
        Profile h;
        REQUIRE(is_ok(h.gcodes(plural())));
        REQUIRE(is_ok(h.gcodes(plural_no_prefs())));
        CHECK(h.printer->sends == 2);
        CHECK(h.gcodes(singular())["code"] == SSWCPCode::Failure);
        CHECK(h.printer->sends == 2);
    }
    SECTION("the comparison is the exact text: a one-character difference or a different command is not a repeat") {
        Profile h;
        REQUIRE(is_ok(h.gcodes(plural())));
        std::string edited = plural();
        edited.back() = edited.back() == '1' ? '0' : '1'; // flips the last preference value
        CHECK(is_ok(h.gcodes(edited)));                    // validated and sent as a second script, not waved through
        CHECK(h.printer->sends == 2);
        CHECK(h.gcodes(plural())["code"] == SSWCPCode::Failure); // the budget is now spent, and the first text is no longer the accepted one
        CHECK(h.gcodes(plural() + "\nM104 S300")["code"] == SSWCPCode::Failure);
        CHECK(h.gcodes(plural() + "\n")["code"] == SSWCPCode::Failure);
        CHECK(h.printer->sends == 2);
    }
    SECTION("a refused first script is not remembered: nothing counts as accepted") {
        Profile h;
        h.printer->send_ok = false;
        CHECK(h.gcodes(plural())["code"] == SSWCPCode::Failure);
        CHECK(h.gcodes(plural())["code"] == SSWCPCode::Failure); // the repeat is a real attempt again, not an ok
        CHECK(h.printer->sends == 2);
        h.printer->send_ok = true;
        CHECK(is_ok(h.gcodes(plural())));
    }
    SECTION("identity map + unreadable probe on the first script: refused, and its repeat is refused too") {
        Profile h;
        h.printer->known["SET_PRINT_EXTRUDER_MAP"] = Known::Unreadable;
        CHECK(h.gcodes(identity_script)["code"] == SSWCPCode::Failure);
        CHECK(h.gcodes(identity_script)["code"] == SSWCPCode::Failure);
        CHECK(h.printer->sends == 0);
    }
    SECTION("non-identity map + missing command: refused, and a repeat is refused") {
        Profile h;
        h.printer->known["SET_PRINT_EXTRUDER_MAP"] = Known::No;
        CHECK(h.gcodes(plural())["code"] == SSWCPCode::Failure);
        CHECK(h.gcodes(plural())["code"] == SSWCPCode::Failure);
        CHECK(h.printer->sends == 0);
    }
    SECTION("after the start, a repeat is refused like any script") {
        Profile h;
        REQUIRE(is_ok(h.gcodes(plural())));
        REQUIRE(is_ok(h.start()));
        CHECK(h.gcodes(plural())["code"] == SSWCPCode::Failure);
        CHECK(h.printer->sends == 1);
    }
    SECTION("a reopen forgets the accepted script; a repeat from the old epoch or a closed context is refused") {
        Profile h;
        REQUIRE(is_ok(h.gcodes(plural())));
        const std::uint64_t old_epoch = h.epoch;
        h.open();
        CHECK_FALSE(h.session->configure(old_epoch, {{"script", plural()}}).ok);
        CHECK(h.session->configure(h.epoch, {{"script", plural()}}).ok); // the new epoch remembers nothing: the script is forwarded afresh
        CHECK(h.printer->sends == 2);
        REQUIRE(h.session->finish(h.epoch).has_value());
        CHECK_FALSE(h.session->configure(h.epoch, {{"script", plural()}}).ok);
        CHECK(h.printer->sends == 2);
    }
    SECTION("upload-only: a script is refused, a repeat too") {
        Profile h(true, false);
        SnapmakerPreprintSession::Open o;
        o.gate.printer = h.printer->pin; o.gate.remote_filename = kFile; o.gate.plate_filaments = {0, 1}; o.gate.extruder_count = 4;
        o.gate.snapshot_size = kSize; o.gate.allow_start = false; o.capability_path = kCap; o.sha256_hex = kSha;
        std::string err;
        const std::uint64_t e = h.session->open(o, err);
        CHECK_FALSE(h.session->configure(e, {{"script", plural()}}).ok);
        CHECK_FALSE(h.session->configure(e, {{"script", plural()}}).ok);
        CHECK(h.printer->sends == 0);
    }
    SECTION("a repeat arriving while the first is still being forwarded is refused (in flight), not answered ok") {
        Profile h;
        std::vector<json> nested;
        h.nested_ok = true;
        h.printer->in_send = [&] { nested.push_back(h.gcodes(plural())); };
        REQUIRE(is_ok(h.gcodes(plural())));
        REQUIRE(nested.size() == 1);
        CHECK(nested[0]["code"] == SSWCPCode::Failure);
        CHECK(h.printer->sends == 1);
    }
}

TEST_CASE("pre-print profile: a request from an earlier epoch is refused after a reopen", "[SSWCP][preprint_profile]")
{
    Profile h;
    auto stale = h.make(h.epoch); // bridge of the first open
    REQUIRE(is_ok(h.call("sw_GetActiveFile", json::object(), stale.get())));
    const std::uint64_t first = h.epoch;
    std::string err;
    REQUIRE(h.open() != first); // reopen
    h.printer->forget();
    CHECK(h.call("sw_GetActiveFile", json::object(), stale.get())["code"] == SSWCPCode::Failure);
    CHECK(h.call("sw_GetFileFilamentMapping", json::object(), stale.get())["code"] == SSWCPCode::Failure);
    CHECK(h.call("sw_SendGCodes", {{"script", plural()}}, stale.get())["code"] == SSWCPCode::Failure);
    CHECK(h.call("sw_MachinePrintStart", {{"filename", kFile}}, stale.get())["code"] == SSWCPCode::Failure);
    CHECK(h.printer->sends == 0);
    CHECK(h.printer->starts == 0);
    auto fresh = h.make();
    CHECK(is_ok(h.call("sw_GetActiveFile", json::object(), fresh.get())));
    CHECK(is_ok(h.call("sw_SendGCodes", {{"script", plural()}}, fresh.get())));
    // a bridge built for epoch 0 (never opened) is refused as well
    auto zero = h.make(0);
    CHECK(h.call("sw_GetActiveFile", json::object(), zero.get())["code"] == SSWCPCode::Failure);
}

TEST_CASE("pre-print profile: unknown or unreadable commands decide whether the script is sent", "[SSWCP][preprint_profile]")
{
    SECTION("SET_PRINT_EXTRUDER_MAP unknown: refused, nothing sent, retry possible")
    {
        Profile h;
        h.printer->known["SET_PRINT_EXTRUDER_MAP"] = Known::No;
        CHECK(h.gcodes(plural())["code"] == SSWCPCode::Failure);
        CHECK(h.printer->sends == 0);
        CHECK(h.start()["code"] == SSWCPCode::Failure); // still unconfigured
        h.printer->known.clear();
        CHECK(is_ok(h.gcodes(plural())));
    }
    SECTION("SET_PRINT_USED_EXTRUDERS unknown: refused")
    {
        Profile h;
        h.printer->known["SET_PRINT_USED_EXTRUDERS"] = Known::No;
        CHECK(h.gcodes(plural())["code"] == SSWCPCode::Failure);
        CHECK(h.printer->sends == 0);
    }
    SECTION("map or used unreadable (help could not be read): refused, fail closed")
    {
        Profile h;
        h.printer->known["SET_PRINT_EXTRUDER_MAP"] = Known::Unreadable;
        CHECK(h.gcodes(plural())["code"] == SSWCPCode::Failure);
        h.printer->known.clear();
        h.printer->known["SET_PRINT_USED_EXTRUDERS"] = Known::Unreadable;
        CHECK(h.gcodes(plural())["code"] == SSWCPCode::Failure);
        CHECK(h.printer->sends == 0);
    }
    SECTION("SET_PRINT_PREFERENCES unknown (plural): the map and used lines are still sent, the preference line is not (#118)")
    {
        Profile h;
        h.printer->known["SET_PRINT_PREFERENCES"] = Known::No;
        REQUIRE(is_ok(h.gcodes(plural())));
        CHECK(h.printer->sent_scripts() == std::vector<std::string>{build_snapmaker_extruder_map_script(kMapping).script});
        CHECK(h.printer->sent_scripts()[0].find("PREFERENCE") == std::string::npos);
        CHECK(is_ok(h.start()));
    }
    SECTION("SET_PRINT_PREFERENCE unknown (singular): map and used lines only")
    {
        Profile h;
        h.printer->known["SET_PRINT_PREFERENCE"] = Known::No;
        REQUIRE(is_ok(h.gcodes(singular())));
        CHECK(h.printer->sent_scripts() == std::vector<std::string>{build_snapmaker_extruder_map_script(kMapping).script});
    }
    SECTION("the singular command unknown does not matter for a plural script (the plural command is the one probed)")
    {
        Profile h;
        h.printer->known["SET_PRINT_PREFERENCE"] = Known::No;
        REQUIRE(is_ok(h.gcodes(plural())));
        CHECK(h.printer->sent_scripts() == std::vector<std::string>{plural()});
    }
    SECTION("preference command unreadable: refused")
    {
        Profile h;
        h.printer->known["SET_PRINT_PREFERENCES"] = Known::Unreadable;
        CHECK(h.gcodes(plural())["code"] == SSWCPCode::Failure);
        CHECK(h.printer->sends == 0);
    }
    SECTION("a script with no preference line probes only the map and used commands")
    {
        Profile h;
        REQUIRE(is_ok(h.gcodes(plural_no_prefs())));
        CHECK(h.printer->kinds() == std::vector<std::string>{"pin", "state", "known", "known", "send"});
    }
}

TEST_CASE("pre-print profile: the pin must match; an unreadable or changed identity refuses everything", "[SSWCP][preprint_profile]")
{
    Profile h;
    h.printer->pin.serial = "OTHER";
    CHECK(h.gcodes(plural())["code"] == SSWCPCode::Failure);
    h.printer->pin = {"u1.local", "7125", ""}; // serial unreadable
    CHECK(h.gcodes(plural())["code"] == SSWCPCode::Failure);
    h.printer->pin = {"u1.local", "7126", "SN123"};
    CHECK(h.gcodes(plural())["code"] == SSWCPCode::Failure);
    h.printer->pin = {"u1.local", "7125", "SN123"};
    REQUIRE(is_ok(h.gcodes(plural())));
    h.printer->pin.host = "other.local";
    CHECK(h.start()["code"] == SSWCPCode::Failure);
    CHECK(h.printer->starts == 0);
    CHECK(h.printer->sends == 1);
}

TEST_CASE("pre-print profile: open refuses an incomplete pin or a remote name that could break out of the start command", "[SSWCP][preprint_profile]")
{
    Profile h(true, false);
    CHECK(h.open(true, "a\"b.gcode") == 0);
    CHECK(h.open(true, "a\nG28.gcode") == 0);
    CHECK(h.open(true, "../a.gcode") == 0);
    CHECK(h.open(true, "/abs.gcode") == 0);
    CHECK(h.open(true, "") == 0);
    h.printer->pin.serial.clear();
    CHECK(h.open(true) == 0);
    // after a failed open nothing is live: a bridge for the old epoch is refused
    auto b = h.make(1);
    CHECK(h.call("sw_GetActiveFile", json::object(), b.get())["code"] == SSWCPCode::Failure);
    CHECK(h.call("sw_SendGCodes", {{"script", plural()}}, b.get())["code"] == SSWCPCode::Failure);
    CHECK(h.printer->sends == 0);
}

TEST_CASE("pre-print profile: SetFilamentMappingComplete and FinishFilamentMapping close the gate and the context", "[SSWCP][preprint_profile]")
{
    for (const char* cmd : {"sw_SetFilamentMappingComplete", "sw_FinishFilamentMapping"}) {
        CAPTURE(cmd);
        Profile h;
        REQUIRE(is_ok(h.gcodes(plural())));
        REQUIRE(is_ok(h.call(cmd, {{"event", 1}}))); // event 1 (CustomFlowRegroup) is ignored, the context still closes
        CHECK(h.done_calls.empty()); // F3: deferred
        h.pump();
        CHECK(h.done_calls == std::vector<bool>{false}); // closed before the start
        CHECK(h.session->state() == SnapmakerPreprintState::Closed);
        CHECK_FALSE(h.ctx->active());
        h.printer->forget();
        CHECK(h.call("sw_GetActiveFile")["code"] == SSWCPCode::Failure);
        CHECK(h.call("sw_GetFileFilamentMapping")["code"] == SSWCPCode::Failure);
        CHECK(h.start()["code"] == SSWCPCode::Failure);
        CHECK(h.gcodes(plural())["code"] == SSWCPCode::Failure);
        CHECK(h.printer->starts == 0);
        CHECK(h.printer->sends == 1);
    }
}

TEST_CASE("pre-print profile: clicks arriving while a printer call is in flight are refused", "[SSWCP][preprint_profile]")
{
    SECTION("during the script forward")
    {
        Profile h;
        std::vector<json> nested;
        h.nested_ok = true;
        h.printer->in_send = [&] {
            nested.push_back(h.start());
            nested.push_back(h.gcodes(plural()));
        };
        REQUIRE(is_ok(h.gcodes(plural())));
        REQUIRE(nested.size() == 2);
        CHECK(nested[0]["code"] == SSWCPCode::Failure);
        CHECK(nested[1]["code"] == SSWCPCode::Failure);
        CHECK(h.printer->starts == 0);
        CHECK(h.printer->sends == 1);
    }
    SECTION("during the start forward")
    {
        Profile h;
        REQUIRE(is_ok(h.gcodes(plural())));
        std::vector<json> nested;
        h.nested_ok = true;
        h.printer->in_start = [&] {
            nested.push_back(h.start());
            nested.push_back(h.gcodes(plural()));
        };
        REQUIRE(is_ok(h.start()));
        CHECK(nested[0]["code"] == SSWCPCode::Failure);
        CHECK(nested[1]["code"] == SSWCPCode::Failure);
        CHECK(h.printer->starts == 1);
        CHECK(h.printer->sends == 1);
    }
    SECTION("replies wait for the executor, and a bridge destroyed meanwhile drops them")
    {
        Profile h;
        h.hold = true;
        h.bridge->on_script_message(h.good, json{{"header", {{"id", 1}}}, {"payload", {{"cmd", "sw_SendGCodes"}, {"params", {{"script", plural()}}}}}}.dump());
        h.bridge->on_script_message(h.good, json{{"header", {{"id", 2}}}, {"payload", {{"cmd", "sw_MachinePrintStart"}, {"params", {{"filename", kFile}}}}}}.dump());
        CHECK(h.sent.empty());
        REQUIRE(h.held.size() == 2);
        auto jobs = std::move(h.held);
        h.held.clear();
        for (auto& j : jobs) { j.first(); }  // both jobs run off-thread in order: the script, then the start (needs the script first)
        h.bridge.reset();                   // page gone before the UI hop
        for (auto& j : jobs) { j.second(); }
        CHECK(h.sent.empty());
        CHECK(h.printer->sends == 1);
        CHECK(h.printer->starts == 1);
    }
}

TEST_CASE("pre-print profile: racing threads cannot start twice or configure thrice", "[SSWCP][preprint_profile][threads]")
{
    Profile h(true, false);
    std::atomic<int> ok_starts{0}, ok_configs{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < 12; ++i)
        ts.emplace_back([&] {
            if (h.session->configure(h.epoch, {{"script", plural()}}).ok) ++ok_configs;
            if (h.session->start(h.epoch, {{"filename", kFile}}).ok) ++ok_starts;
        });
    for (auto& t : ts) t.join();
    CHECK(ok_starts == 1);
    CHECK(h.printer->starts == 1);
    CHECK(ok_configs <= snapmaker_preprint_max_configures);
    CHECK(ok_configs >= 1);
    CHECK(h.printer->sends == 1); // #142: identical repeats are answered ok, never forwarded again
}

namespace {
struct DeviceTab
{
    std::vector<std::string> sent;
    std::vector<std::string> control_methods;
    int                      queries = 0;
    std::shared_ptr<SnapmakerPreprintContext> ctx = std::make_shared<SnapmakerPreprintContext>();
    SnapmakerDeviceBridge    bridge;
    const std::string        good = "http://127.0.0.1:5000/web/flutter_web/index.html";
    DeviceTab()
        : bridge([this] {
              SnapmakerDeviceBridge::Options o; // exactly the Device tab's shape: no pre-print session
              o.port = 5000; o.http_host = "http://127.0.0.1:5000";
              o.preprint = ctx;
              o.run_script = [this](const std::string& js) { sent.push_back(js); };
              o.control = [this](const std::string& m, const json&, std::function<void(bool, json)> done) { control_methods.push_back(m); done(true, json{{"state", "success"}}); };
              o.query_state = [this](const std::vector<std::string>&, std::function<void(bool, json)> done) { ++queries; done(true, json{{"status", json::object()}}); };
              return o;
          }())
    {}
    json reply(std::size_t n) const
    {
        const std::string prefix = "window.postMessage(JSON.stringify(", suffix = "), '*');";
        return json::parse(sent.at(n).substr(prefix.size(), sent.at(n).size() - prefix.size() - suffix.size()));
    }
    json call(const std::string& cmd, const json& params = json::object())
    {
        const std::size_t before = sent.size();
        bridge.on_script_message(good, json{{"header", {{"id", 1}}}, {"payload", {{"cmd", cmd}, {"params", params}}}}.dump());
        REQUIRE(sent.size() == before + 1);
        return reply(before)["payload"];
    }
};
} // namespace

TEST_CASE("Device-tab profile is unchanged: print start, local print, uploads and the pre-print gcode shapes stay denied", "[SSWCP][preprint_profile][device_tab]")
{
    DeviceTab d;
    for (const char* cmd : {"sw_MachinePrintStart", "sw_StartLocalPrint", "sw_UploadFile", "sw_UploadFileResult", "sw_UploadFiletoMachine", "sw_SetFilamentMappingComplete",
                            "sw_FinishFilamentMapping", "sw_GetFileStream", "sw_DownLoadFile", "sw_DownloadMachineFile", "sw_ControlExtruderTemp",
                            "sw_SendCommand", "sw_RenameDevice", "sw_DeleteDevices"}) {
        CAPTURE(cmd);
        CHECK_FALSE(d.bridge.sswcp().is_registered(cmd));
        CHECK(d.call(cmd, {{"filename", kFile}})["code"] == 501);
    }
    // sw_SendGCodes exists on the Device tab (nozzle temperature) but accepts none of the pre-print shapes
    REQUIRE(d.bridge.sswcp().is_registered("sw_SendGCodes"));
    for (const auto& script : {plural(), plural_no_prefs(), singular(), std::string("SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=0 MAP_EXTRUDER=1\n"),
                               std::string("SET_PRINT_PREFERENCES BED_LEVEL=1"), std::string("SDCARD_PRINT_FILE FILENAME=\"x.gcode\""), std::string("G28")}) {
        CAPTURE(script);
        CHECK(d.call("sw_SendGCodes", {{"script", script}})["code"] != 200);
        CHECK(d.call("sw_SendGCodes", {{"script", json::array({script})}})["code"] != 200);
    }
    CHECK(d.control_methods.empty());
    // and its pre-print reads still answer from the context alone, with no capability URL or checksum
    std::string err;
    REQUIRE(d.ctx->set(plate(), {}, err));
    const json f = d.call("sw_GetActiveFile");
    CHECK(f["data"].size() == 2);
    CHECK_FALSE(f["data"].contains("url"));
}

namespace {
std::set<std::string> registered_in(const char* list_file, std::set<std::string>* denied = nullptr)
{
    const boost::filesystem::path file = boost::filesystem::path(PROFILES_DIR).parent_path() / "web" / list_file;
    std::ifstream in(file.string());
    REQUIRE(in.good());
    std::set<std::string> registered;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string cls, name;
        if (!(ss >> cls >> name) || cls[0] == '#') continue;
        REQUIRE((cls == "REGISTERED" || cls == "DENIED"));
        (cls == "REGISTERED" ? registered : (denied ? *denied : registered)).insert(name);
    }
    return registered;
}
} // namespace

TEST_CASE("the pre-print profile registers exactly the REGISTERED lines of snapmaker_preprint_bridge_commands.txt", "[SSWCP][preprint_profile][list]")
{
    std::set<std::string> denied;
    const auto listed = registered_in("snapmaker_preprint_bridge_commands.txt", &denied);
    Profile h;
    const std::set<std::string> expected = {
        // the core's built-in commands (SSWCP cannot unregister them; none touches the printer)
        "sw_FileLog", "sw_SubUserUpdatePrivacy", "sw_SubscribePageStateChange", "sw_SubscribeForegroundChange", "sw_SubscribeRecentFiles",
        "sw_SubscribeLocalDevices", "sw_SubscribeCacheKey", "sw_SubscribeUserLoginState", "sw_GetUserLoginState", "sw_UserLogin",
        "sw_GetSoftwareInfo", "sw_UploadEvent", "sw_GetLocalDevices",
        // the pre-print profile
        "sw_GetConnectedMachine", "sw_GetActiveFile", "sw_GetFileFilamentMapping", "sw_SendGCodes", "sw_MachinePrintStart", "sw_StartLocalPrint",
        "sw_SetFilamentMappingComplete", "sw_FinishFilamentMapping",
        // #137 (ADR 0010 amendment 2026-11-17): the read-only family, shared with the Device tab, pinned-device only
        "sw_GetMachineState", "sw_SubscribeMachineState", "sw_SetSubscribeFilter", "sw_UnSubscribeMachineState", "sw_StopMachineStateSubscription",
        "sw_GetMachineSystemInfo", "sw_SystemGetDeviceInfo", "sw_GetCache", "sw_SetCache", "sw_RemoveCache", "sw_UnsubscribeCacheKeys",
        "sw_UpdateMachineFilamentInfo", "sw_MachineHeartbeat", "sw_exception_query", "sw_create_mqtt_client", "sw_mqtt_connect", "sw_mqtt_disconnect",
        "sw_mqtt_subscribe", "sw_mqtt_unsubscribe", "sw_mqtt_publish"};
    CHECK(listed == expected);
    CHECK(listed.size() == 41);
    for (const auto& n : listed) { CAPTURE(n); CHECK(h.bridge->sswcp().is_registered(n)); }
    for (const auto& n : denied) { CAPTURE(n); CHECK_FALSE(h.bridge->sswcp().is_registered(n)); }
    for (const char* n : {"sw_UploadFile", "sw_UploadFileResult", "sw_UploadFiletoMachine", "sw_DownLoadFile", "sw_DownloadMachineFile", "sw_GetFileStream",
                          "sw_ControlLed", "sw_ControlBedTemp", "sw_ControlExtruderTemp", "sw_ControlPurifier", "sw_SendCommand", "sw_MachinePrintPause",
                          "sw_MachinePrintCancel", "sw_DeleteDevices", "sw_RenameDevice", "sw_MachineFilesRoots", "sw_CameraStartMonitor", "sw_FileGetStatus",
                          "sw_PrinterDefectDetection", "sw_SnapLog", "sw_AddDevice", "sw_Connect", "sw_ConnectOtherMachine", "sw_Disconnect", "sw_GetPincode",
                          "sw_StartMachineFind", "sw_StopMachineFind", "sw_DeleteRecentFiles", "sw_mqtt_set_engine", "sw_test_mqtt_moonraker",
                          "sw_GetMachineObjects", "sw_GetPrinterInfo", "sw_MachineFilesMetadata", "sw_GetFileListPage", "sw_FilesThumbnailsBase64"})
        CHECK_FALSE(h.bridge->sswcp().is_registered(n));
    // the Device tab's list keeps its classification of the same names
    std::set<std::string> device_denied;
    const auto device = registered_in("snapmaker_bridge_commands.txt", &device_denied);
    for (const char* n : {"sw_MachinePrintStart", "sw_StartLocalPrint", "sw_SetFilamentMappingComplete", "sw_FinishFilamentMapping"}) CHECK(device_denied.count(n) == 1);
    CHECK(device.size() == 57);
}

// ---- #124: sw_GetLocalDevices answers the one pinned printer, and nothing else ----

TEST_CASE("pre-print sw_GetLocalDevices returns only the pinned printer, from what the app already knows", "[SSWCP][preprint_profile][local_devices]")
{
    Profile h(true, false);
    h.printer->pin = SnapmakerPreprintPin{"192.0.2.10", "7125", "SNPLACEHOLDER"};
    REQUIRE(h.open(true) != 0);
    h.bridge = h.make();
    h.printer->forget();

    const json r = h.call("sw_GetLocalDevices");
    REQUIRE(is_ok(r));
    REQUIRE(r["data"].is_array());
    REQUIRE(r["data"].size() == 1);
    const json& d = r["data"][0];
    CHECK(d["sn"] == "SNPLACEHOLDER");
    CHECK(d["dev_id"] == "192.0.2.10");
    CHECK(d["ip"] == "192.0.2.10");
    CHECK(d["link_mode"] == "lan");
    CHECK(d["machine_type"] == "Snapmaker U1");
    CHECK(d["connected"] == true);
    CHECK(d["online"] == true);
    CHECK(d.contains("name"));
    CHECK(d.contains("clientId"));
    CHECK(d.contains("port"));
    CHECK(d["error"] == "");
    CHECK(h.printer->calls.empty()); // answered from the pinned identity: the printer was not contacted
}

TEST_CASE("pre-print sw_GetLocalDevices is empty when no printer is pinned, the bridge is stale or the context is closed", "[SSWCP][preprint_profile][local_devices]")
{
    SECTION("stale epoch") {
        Profile h;
        auto stale = h.make(h.epoch + 1);
        const json r = h.call("sw_GetLocalDevices", json::object(), stale.get());
        CHECK(is_ok(r));
        CHECK(r["data"].is_array());
        CHECK(r["data"].empty());
    }
    SECTION("closed context") {
        Profile h;
        h.session->finish(h.epoch);
        const json r = h.call("sw_GetLocalDevices");
        CHECK(r["data"].is_array());
        CHECK(r["data"].empty());
    }
    SECTION("never opened") {
        Profile h(true, false);
        h.session->finish(h.epoch);
        h.bridge = h.make(0);
        const json r = h.call("sw_GetLocalDevices");
        CHECK(r["data"].is_array());
        CHECK(r["data"].empty());
    }
}

// ---- #137: sw_GetConnectedMachine answers the same pinned record, read-only ----

TEST_CASE("pre-print sw_GetConnectedMachine returns the pinned printer's record, the same as sw_GetLocalDevices, without contacting it", "[SSWCP][preprint_profile][connected_machine]")
{
    Profile h(true, false);
    h.printer->pin = SnapmakerPreprintPin{"192.0.2.10", "7125", "SNPLACEHOLDER"};
    REQUIRE(h.open(true) != 0);
    h.bridge = h.make();
    h.printer->forget();

    const json r = h.call("sw_GetConnectedMachine");
    REQUIRE(is_ok(r));
    REQUIRE(r["data"].is_object());
    CHECK(r["data"]["sn"] == "SNPLACEHOLDER");
    CHECK(r["data"]["connected"] == true);
    const json list = h.call("sw_GetLocalDevices");
    REQUIRE(list["data"].size() == 1);
    CHECK(r["data"] == list["data"][0]);
    CHECK(h.printer->calls.empty());
}

TEST_CASE("pre-print sw_GetConnectedMachine is an empty record when no printer is pinned, the bridge is stale or the context is closed", "[SSWCP][preprint_profile][connected_machine]")
{
    SECTION("stale epoch") {
        Profile h;
        auto stale = h.make(h.epoch + 1);
        const json r = h.call("sw_GetConnectedMachine", json::object(), stale.get());
        CHECK(is_ok(r));
        CHECK(r["data"] == json::object());
    }
    SECTION("closed context") {
        Profile h;
        h.session->finish(h.epoch);
        const json r = h.call("sw_GetConnectedMachine");
        CHECK(r["data"] == json::object());
    }
}

// ---- #137 (ADR 0010 amendment 2026-11-17): the read-only family on the pre-print page, pinned device only ----

namespace {
// What the family may reach: only Options::query_state (a Klipper objects query) and Options::fetch_resource (a GET). The PreprintDialog sets
// neither today, so these hooks are how the tests see what the handlers WOULD send if it did. Nothing else is hooked: no control, no camera.
struct ReadHooks
{
    std::vector<std::vector<std::string>> queries;
    std::vector<std::string>              fetches;
    void attach(SnapmakerDeviceBridge::Options& o)
    {
        o.query_state = [this](const std::vector<std::string>& objects, std::function<void(bool, json)> done) {
            queries.push_back(objects);
            done(true, json{{"eventtime", 7}, {"status", {{"print_stats", {{"state", "standby"}}}}}});
        };
        o.fetch_resource = [this](const std::string& path, std::size_t, std::function<void(bool, std::string)> done) {
            fetches.push_back(path);
            done(true, json{{"result", {{"system_info", {{"product_info", {{"serial_number", "SNPLACEHOLDER"}}}}}}}}.dump());
        };
    }
};
json family_msg(const std::string& cmd, const json& params, const std::string& event_id = "")
{
    json p = {{"cmd", cmd}, {"params", params}};
    if (!event_id.empty()) p["event_id"] = event_id;
    return json{{"header", {{"id", 1}}}, {"payload", p}};
}
const std::vector<std::string> kFamilyNoState = {"sw_SetSubscribeFilter", "sw_UnSubscribeMachineState", "sw_StopMachineStateSubscription",
                                                 "sw_SystemGetDeviceInfo", "sw_GetCache", "sw_SetCache", "sw_RemoveCache", "sw_UnsubscribeCacheKeys",
                                                 "sw_UpdateMachineFilamentInfo", "sw_MachineHeartbeat", "sw_exception_query", "sw_create_mqtt_client",
                                                 "sw_mqtt_connect", "sw_mqtt_disconnect", "sw_mqtt_subscribe", "sw_mqtt_unsubscribe", "sw_mqtt_publish"};
} // namespace

TEST_CASE("pre-print read-only family: every handler answers for the pinned device and reaches the printer only with reads", "[SSWCP][preprint_profile][read_family]")
{
    Profile    h(true, false);
    ReadHooks  hooks;
    h.printer->pin = SnapmakerPreprintPin{"192.0.2.10", "7125", "SNPLACEHOLDER"};
    REQUIRE(h.open(true) != 0);
    h.bridge = h.make(~std::uint64_t(0), [&](SnapmakerDeviceBridge::Options& o) { hooks.attach(o); });
    h.printer->forget();

    for (const auto& cmd : kFamilyNoState) {
        CAPTURE(cmd);
        const json r = h.call(cmd, json::object());
        CHECK(is_ok(r));
    }
    CHECK(h.call("sw_create_mqtt_client")["data"] == json{{"id", "emu-1"}, {"type", "mqtts"}}); // inert local emulation, as on the Device tab

    const json st = h.call("sw_GetMachineState", {{"objects", {{"print_stats", nullptr}, {"configfile", nullptr}}}});
    REQUIRE(is_ok(st));
    CHECK(st["data"]["status"]["print_stats"]["state"] == "standby");
    CHECK(st["data"]["status"]["configfile"] == json::object()); // not on the object allow-list: asked for, never queried
    for (const auto& q : hooks.queries) CHECK(q == std::vector<std::string>{"print_stats"});
    CHECK_FALSE(hooks.queries.empty());

    const json si = h.call("sw_GetMachineSystemInfo");
    REQUIRE(is_ok(si));
    CHECK(si["data"]["system_info"]["product_info"]["serial_number"] == "SNPLACEHOLDER");
    for (const auto& f : hooks.fetches) CHECK(f == "/machine/system_info"); // the one GET the Device tab does for this
    CHECK_FALSE(hooks.fetches.empty());

    // a subscription: acked, then a push of the state (notify_status_update), never anything but the same query
    const std::size_t before = h.sent.size();
    h.bridge->on_script_message(h.good, family_msg("sw_SubscribeMachineState", json::object(), "pre-sub").dump());
    CHECK(h.sent.size() > before);
    CHECK(h.bridge->sswcp().subscriber_count("sw_SubscribeMachineState") == 1);
    CHECK(is_ok(h.call("sw_UnSubscribeMachineState")));
    CHECK(h.bridge->sswcp().subscriber_count("sw_SubscribeMachineState") == 0);

    CHECK(h.printer->calls.empty());                  // the session's printer interface (pin, state, send, start) was never touched
    CHECK(h.printer->sends == 0);
    CHECK(h.printer->starts == 0);
}

TEST_CASE("pre-print sw_UpdateMachineFilamentInfo and sw_SetCache keep nothing outside this bridge's memory", "[SSWCP][preprint_profile][read_family]")
{
    Profile    h(true, false);
    ReadHooks  hooks;
    h.printer->pin = SnapmakerPreprintPin{"192.0.2.10", "7125", "SNPLACEHOLDER"};
    REQUIRE(h.open(true) != 0);
    h.bridge = h.make(~std::uint64_t(0), [&](SnapmakerDeviceBridge::Options& o) { hooks.attach(o); });
    h.printer->forget();
    CHECK(is_ok(h.call("sw_UpdateMachineFilamentInfo", {{"objects", json::array({{{"key", "SNPLACEHOLDER"}, {"value", "[1]"}}})}})));
    CHECK(h.bridge->filament_info().at("SNPLACEHOLDER") == "[1]");  // in-memory page state, bounded, not written anywhere
    CHECK(is_ok(h.call("sw_SetCache", {{"key", "k"}, {"value", "v"}})));
    { const json c = h.call("sw_GetCache", {{"key", "k"}})["data"]; CHECK((c.is_null() || c == json::object())); } // sw_SetCache is not backed by a store at all
    CHECK(hooks.queries.empty());
    CHECK(hooks.fetches.empty());
    CHECK(h.printer->calls.empty());
}

TEST_CASE("pre-print read-only family answers empty for a stale epoch or a closed context and contacts nothing", "[SSWCP][preprint_profile][read_family]")
{
    ReadHooks hooks;
    auto tweak = [&](SnapmakerDeviceBridge::Options& o) { hooks.attach(o); };
    auto check_all = [&](Profile& h, SnapmakerDeviceBridge* b) {
        for (const auto& cmd : kFamilyNoState) {
            CAPTURE(cmd);
            const json r = h.call(cmd, cmd == "sw_UpdateMachineFilamentInfo" ? json{{"objects", json::array({{{"key", "k"}, {"value", "v"}}})}} : json::object(), b);
            CHECK(is_ok(r));
            if (cmd != "sw_create_mqtt_client")
                CHECK((r["data"].is_null() || r["data"] == json::object()));
        }
        const json st = h.call("sw_GetMachineState", json::object(), b);
        CHECK(is_ok(st));
        CHECK(st["data"]["status"] == json::object());
        const json si = h.call("sw_GetMachineSystemInfo", json::object(), b);
        CHECK(is_ok(si));
        CHECK((si["data"].is_null() || si["data"] == json::object()));
        b->on_script_message(h.good, family_msg("sw_SubscribeMachineState", json::object(), "pre-sub").dump());
        b->tick();
        b->tick();
        b->tick();
        CHECK(hooks.queries.empty());
        CHECK(hooks.fetches.empty());
        CHECK(h.printer->calls.empty());
        CHECK(b->filament_info().empty());
    };
    SECTION("stale epoch") {
        Profile h;
        auto stale = h.make(h.epoch + 1, tweak);
        check_all(h, stale.get());
    }
    SECTION("closed context") {
        Profile h;
        auto b = h.make(~std::uint64_t(0), tweak);
        h.session->finish(h.epoch);
        check_all(h, b.get());
    }
}

// ---- #137: the pre-print dialog's real readers (snapmaker_attach_pinned_readers), pinned device only ----

namespace {
// Stands in for the agent's two read-only calls. Records what would go to the printer; answers like Moonraker does for print_task_config.
struct RawPrinter
{
    std::vector<std::vector<std::string>> queries;
    std::vector<std::string>              fetches;
    bool                                  pinned = true; // what the dialog's pin check (the agent still on the pinned host) would say
    void attach(SnapmakerDeviceBridge::Options& o)
    {
        snapmaker_attach_pinned_readers(
            o,
            [this](const std::vector<std::string>& objects, std::function<void(bool, json)> done) {
                queries.push_back(objects);
                done(true, json{{"eventtime", 7},
                                {"status", {{"print_task_config", {{"filament_vendor", json::array({"Snapmaker", "Snapmaker", "NONE", "NONE"})},
                                                                   {"filament_type", json::array({"PLA", "PETG", "NONE", "NONE"})},
                                                                   {"filament_sub_type", json::array({"SnapSpeed", "Basic", "NONE", "NONE"})}}}}}});
            },
            [this](const std::string& path, std::size_t, std::function<void(bool, std::string)> done) {
                fetches.push_back(path);
                done(true, json{{"result", {{"system_info", {{"product_info", {{"serial_number", "SNPLACEHOLDER"}}}}}}}}.dump());
            },
            [this] { return pinned; });
    }
};
} // namespace

TEST_CASE("pre-print readers: sw_GetMachineState returns the pinned printer's print_task_config, with queries only", "[SSWCP][preprint_profile][read_family][readers]")
{
    Profile    h(true, false);
    RawPrinter raw;
    h.printer->pin = SnapmakerPreprintPin{"192.0.2.10", "7125", "SNPLACEHOLDER"};
    REQUIRE(h.open(true) != 0);
    h.bridge = h.make(~std::uint64_t(0), [&](SnapmakerDeviceBridge::Options& o) { raw.attach(o); });
    h.printer->forget();

    const json st = h.call("sw_GetMachineState", {{"objects", {{"print_task_config", nullptr}, {"filament_detect", nullptr}, {"print_stats", nullptr}}}});
    REQUIRE(is_ok(st));
    const json cfg = st["data"]["status"]["print_task_config"];
    CHECK(cfg["filament_vendor"] == json::array({"Snapmaker", "Snapmaker", "NONE", "NONE"}));
    CHECK(cfg["filament_type"][1] == "PETG");
    CHECK(cfg["filament_sub_type"][0] == "SnapSpeed");
    REQUIRE_FALSE(raw.queries.empty());
    for (const auto& q : raw.queries) { // only allow-listed objects reach the printer
        CHECK(std::count(q.begin(), q.end(), "print_task_config") == 1);
        for (const auto& name : q) CHECK(snapmaker_state_object_allowed(name));
    }
    const json si = h.call("sw_GetMachineSystemInfo");
    REQUIRE(is_ok(si));
    for (const auto& f : raw.fetches) CHECK(f == "/machine/system_info");
    CHECK(h.printer->calls.empty()); // no session printer call: nothing written, nothing started
    CHECK(h.printer->sends == 0);
    CHECK(h.printer->starts == 0);
}

TEST_CASE("pre-print readers: nothing is read when the agent is no longer on the pinned printer", "[SSWCP][preprint_profile][read_family][readers]")
{
    Profile    h(true, false);
    RawPrinter raw;
    raw.pinned = false;
    h.printer->pin = SnapmakerPreprintPin{"192.0.2.10", "7125", "SNPLACEHOLDER"};
    REQUIRE(h.open(true) != 0);
    h.bridge = h.make(~std::uint64_t(0), [&](SnapmakerDeviceBridge::Options& o) { raw.attach(o); });
    const json st = h.call("sw_GetMachineState", {{"objects", {{"print_task_config", nullptr}}}});
    CHECK(is_ok(st));
    CHECK(st["data"]["status"] == json::object()); // the empty state, no slot data
    h.call("sw_GetMachineSystemInfo");
    CHECK(raw.queries.empty());
    CHECK(raw.fetches.empty());
}

TEST_CASE("pre-print readers: a stale epoch or a closed context gets the empty state and reaches nothing", "[SSWCP][preprint_profile][read_family][readers]")
{
    RawPrinter raw;
    auto tweak = [&](SnapmakerDeviceBridge::Options& o) { raw.attach(o); };
    auto check = [&](Profile& h, SnapmakerDeviceBridge* b) {
        const json st = h.call("sw_GetMachineState", {{"objects", {{"print_task_config", nullptr}}}}, b);
        CHECK(is_ok(st));
        CHECK(st["data"]["status"] == json::object());
        h.call("sw_GetMachineSystemInfo", json::object(), b);
        CHECK(raw.queries.empty());
        CHECK(raw.fetches.empty());
    };
    SECTION("stale epoch") { Profile h; auto b = h.make(h.epoch + 1, tweak); check(h, b.get()); }
    SECTION("closed context") { Profile h; auto b = h.make(~std::uint64_t(0), tweak); h.session->finish(h.epoch); check(h, b.get()); }
}

// ---- review addendum (F2, F3, F4, F7, F8) ----

namespace {
struct ThrowingPrinter : FakePrinter
{
    std::set<std::string> throws;
    void maybe(const char* k) { if (throws.count(k)) throw std::runtime_error(k); }
    SnapmakerPreprintPin connected_pin() override { maybe("pin"); return FakePrinter::connected_pin(); }
    std::optional<std::string> print_state() override { maybe("state"); return FakePrinter::print_state(); }
    std::optional<std::uint64_t> remote_file_size(const std::string& f) override { maybe("size"); return FakePrinter::remote_file_size(f); }
    Known command_known(const std::string& c) override { maybe("known"); return FakePrinter::command_known(c); }
    bool send_gcode(const std::string& s) override { maybe("send"); return FakePrinter::send_gcode(s); }
    bool start_print(const std::string& f) override { maybe("start"); return FakePrinter::start_print(f); }
};
} // namespace

TEST_CASE("pre-print session: a throwing printer refuses and never starts (every catch)", "[SSWCP][preprint_profile]")
{
    for (const char* where : {"pin", "state", "known", "send"}) {
        CAPTURE(where);
        auto tp = std::make_shared<ThrowingPrinter>();
        auto ctx = std::make_shared<SnapmakerPreprintContext>();
        SnapmakerPreprintSession session(ctx, tp);
        std::string err;
        REQUIRE(ctx->set(plate(), {}, err));
        SnapmakerPreprintSession::Open o;
        o.gate = {tp->pin, kFile, {0, 1}, 4, kSize, true};
        o.capability_path = kCap;
        o.sha256_hex = kSha;
        const auto e = session.open(o, err);
        REQUIRE(e != 0);
        tp->throws = {where};
        CHECK_FALSE(session.configure(e, {{"script", plural()}}).ok);
        CHECK(tp->sends == 0);
        tp->throws.clear();
        CHECK(session.configure(e, {{"script", plural()}}).ok); // the failed attempt left the gate usable
    }
    for (const char* where : {"pin", "state", "size", "start"}) {
        CAPTURE(where);
        auto tp = std::make_shared<ThrowingPrinter>();
        auto ctx = std::make_shared<SnapmakerPreprintContext>();
        SnapmakerPreprintSession session(ctx, tp);
        std::string err;
        REQUIRE(ctx->set(plate(), {}, err));
        SnapmakerPreprintSession::Open o;
        o.gate = {tp->pin, kFile, {0, 1}, 4, kSize, false};
        o.capability_path = kCap;
        o.sha256_hex = kSha;
        const auto e = session.open(o, err);
        REQUIRE(e != 0);
        tp->throws = {where};
        CHECK_FALSE(session.start(e, {{"filename", kFile}}).ok);
        CHECK(session.state() != SnapmakerPreprintState::Started);
        tp->throws.clear();
        CHECK(session.start(e, {{"filename", kFile}}).ok);
    }
}

TEST_CASE("pre-print session: forwarded but closed meanwhile is success with context_closed, and the start is still reported", "[SSWCP][preprint_profile]")
{
    SECTION("start")
    {
        Profile h;
        REQUIRE(is_ok(h.gcodes(plural())));
        h.printer->in_start = [&] { h.session->finish(h.epoch); }; // the page finishes while the start is on the wire
        const json r = h.start();
        CHECK(is_ok(r));
        CHECK(r["data"]["context_closed"] == true);
        CHECK(h.printer->starts == 1);
        CHECK(h.session->started_forwarded());
    }
    SECTION("script")
    {
        Profile h;
        h.printer->in_send = [&] { h.session->finish(h.epoch); };
        const json r = h.gcodes(plural());
        CHECK(is_ok(r));
        CHECK(r["data"]["context_closed"] == true);
    }
    SECTION("finish during an in-flight start reports started")
    {
        Profile h;
        REQUIRE(is_ok(h.gcodes(plural())));
        std::optional<bool> during;
        h.printer->in_start = [&] { during = h.session->finish(h.epoch); };
        h.start();
        REQUIRE(during.has_value());
        CHECK(*during == true); // a start is on the wire: not "not started"
    }
}

TEST_CASE("pre-print session: F2 a stale bridge closes nothing and reports nothing", "[SSWCP][preprint_profile]")
{
    Profile h;
    const std::uint64_t old_epoch = h.epoch;
    auto old_bridge = h.make(old_epoch);
    REQUIRE(h.open() != 0); // reopen: old_epoch is stale
    CHECK(h.epoch != old_epoch);
    CHECK_FALSE(h.session->finish(old_epoch).has_value());
    CHECK(h.session->state() == SnapmakerPreprintState::Open);
    CHECK(h.ctx->active());
    h.call("sw_SetFilamentMappingComplete", {}, old_bridge.get());
    h.pump();
    CHECK(h.done_calls.empty());
    CHECK(h.session->state() == SnapmakerPreprintState::Open);
    old_bridge->shutdown();
    old_bridge.reset();
    CHECK(h.done_calls.empty());
    CHECK(h.session->state() == SnapmakerPreprintState::Open); // the new epoch's gate is untouched
}

TEST_CASE("pre-print session: F4 a bad checksum or capability path is refused at open", "[SSWCP][preprint_profile]")
{
    for (int mode = 0; mode < 8; ++mode) {
        Profile h;
        std::string err;
        SnapmakerPreprintSession::Open o;
        o.gate = {h.printer->pin, kFile, {0, 1}, 4, kSize, true};
        o.capability_path = kCap;
        o.sha256_hex = kSha;
        switch (mode) {
        case 0: o.sha256_hex = kSha.substr(1); break;
        case 1: o.sha256_hex = kSha + "0"; break;
        case 2: o.sha256_hex = std::string(64, 'g'); break;
        case 3: o.sha256_hex.clear(); break;
        case 4: o.capability_path.clear(); break;
        case 5: o.capability_path = "/other/path/x.gcode"; break;
        case 6: o.capability_path = "/web/flutter_web/cap/"; break;
        default: o.capability_path = "/web/flutter_web/cap/T/a\"b.gcode"; break;
        }
        INFO("mode " << mode);
        CHECK(h.session->open(o, err) == 0);
    }
}

TEST_CASE("pre-print session: identity map + map commands known missing is answered ok without forwarding the map lines, with a note (#141)", "[SSWCP][preprint_profile]")
{
    const std::vector<int> identity = {0, 1};
    const std::string      script_no_prefs = build_snapmaker_plural_preference_script(identity, {});
    const std::string      script_prefs    = build_snapmaker_plural_preference_script(identity, kPrefs);
    SECTION("both missing, no preferences: ok, nothing forwarded, note in data and notifier, the start still works") {
        Profile h;
        std::vector<std::string> notices;
        h.session->set_notifier([&notices](const std::string& m) { notices.push_back(m); });
        h.printer->known["SET_PRINT_EXTRUDER_MAP"]   = Known::No;
        h.printer->known["SET_PRINT_USED_EXTRUDERS"] = Known::No;
        const json r = h.gcodes(script_no_prefs);
        REQUIRE(is_ok(r));
        CHECK(h.printer->sends == 0);
        CHECK(r["data"]["identity_map_assumed"] == true);
        CHECK(r["data"]["note"].get<std::string>().find("identity mapping assumed") != std::string::npos);
        REQUIRE(notices.size() == 1);
        CHECK(notices[0].find("identity mapping assumed") != std::string::npos);
        CHECK(is_ok(h.start()));
        CHECK(h.printer->starts == 1);
    }
    SECTION("both missing, preferences present: only the preference line is forwarded") {
        Profile h;
        h.printer->known["SET_PRINT_EXTRUDER_MAP"]   = Known::No;
        h.printer->known["SET_PRINT_USED_EXTRUDERS"] = Known::No;
        REQUIRE(is_ok(h.gcodes(script_prefs)));
        REQUIRE(h.printer->sent_scripts().size() == 1);
        CHECK(h.printer->sent_scripts()[0].find("SET_PRINT_EXTRUDER_MAP") == std::string::npos);
        CHECK(h.printer->sent_scripts()[0].find("SET_PRINT_USED_EXTRUDERS") == std::string::npos);
        CHECK(h.printer->sent_scripts()[0].find("SET_PRINT_PREFERENCES") == 0);
    }
    SECTION("only the used command missing: the map lines still go") {
        Profile h;
        h.printer->known["SET_PRINT_USED_EXTRUDERS"] = Known::No;
        REQUIRE(is_ok(h.gcodes(script_no_prefs)));
        REQUIRE(h.printer->sent_scripts().size() == 1);
        CHECK(h.printer->sent_scripts()[0].find("SET_PRINT_EXTRUDER_MAP") == 0);
        CHECK(h.printer->sent_scripts()[0].find("SET_PRINT_USED_EXTRUDERS") == std::string::npos);
    }
    SECTION("non-identity map + missing command: still refused, nothing sent, no note") {
        Profile h;
        std::vector<std::string> notices;
        h.session->set_notifier([&notices](const std::string& m) { notices.push_back(m); });
        h.printer->known["SET_PRINT_EXTRUDER_MAP"] = Known::No;
        const json r = h.gcodes(plural()); // kMapping = {2,0}
        CHECK(r["code"] == SSWCPCode::Failure);
        CHECK(h.printer->sends == 0);
        CHECK(notices.empty());
    }
    SECTION("identity map + unreadable probe: still refused (fail closed)") {
        Profile h;
        h.printer->known["SET_PRINT_EXTRUDER_MAP"] = Known::Unreadable;
        CHECK(h.gcodes(script_no_prefs)["code"] == SSWCPCode::Failure);
        CHECK(h.printer->sends == 0);
    }
    SECTION("identity map + one command missing and the other unreadable: refused") {
        Profile h;
        h.printer->known["SET_PRINT_EXTRUDER_MAP"]   = Known::No;
        h.printer->known["SET_PRINT_USED_EXTRUDERS"] = Known::Unreadable;
        CHECK(h.gcodes(script_no_prefs)["code"] == SSWCPCode::Failure);
        CHECK(h.printer->sends == 0);
    }
    SECTION("commands present, identity map: forwarded unchanged, no note") {
        Profile h;
        const json r = h.gcodes(script_no_prefs);
        REQUIRE(is_ok(r));
        CHECK(h.printer->sent_scripts() == std::vector<std::string>{script_no_prefs});
        CHECK_FALSE(r["data"].contains("identity_map_assumed"));
    }
}

TEST_CASE("snapmaker_extruder_map_is_identity: used filaments only, bounded by the extruder count (#141)", "[SSWCP][preprint_profile][ExtruderChoice]")
{
    CHECK(snapmaker_extruder_map_is_identity({0, 1, 2, 3}));
    CHECK(snapmaker_extruder_map_is_identity({0, 1}));
    CHECK(snapmaker_extruder_map_is_identity({}));                  // nothing used: vacuous, callers have nothing to send
    CHECK(snapmaker_extruder_map_is_identity({-1, -1}));
    CHECK(snapmaker_extruder_map_is_identity({0, -1, 2, 255}));     // unused entries ignored
    CHECK(snapmaker_extruder_map_is_identity({-1, 1, 254, 3}));
    CHECK_FALSE(snapmaker_extruder_map_is_identity({1, 0}));        // a permutation
    CHECK_FALSE(snapmaker_extruder_map_is_identity({0, 0}));
    CHECK_FALSE(snapmaker_extruder_map_is_identity({2, 0}));
    CHECK_FALSE(snapmaker_extruder_map_is_identity({-1, 0}));       // filament 1 on extruder 0
    CHECK_FALSE(snapmaker_extruder_map_is_identity({0, 1, 2, 3, 4}));      // longer than the extruder count and the extra one is used
    CHECK(snapmaker_extruder_map_is_identity({0, 1, 2, 3, -1, -1}));       // longer, but the extra entries are unused
    CHECK_FALSE(snapmaker_extruder_map_is_identity({0, 1, 2}, 2));         // explicit extruder count
    CHECK_FALSE(snapmaker_extruder_map_is_identity({7}));
    CHECK(snapmaker_extruder_map_pairs_are_identity({{0, 0}, {1, 1}}));
    CHECK_FALSE(snapmaker_extruder_map_pairs_are_identity({}));
    CHECK_FALSE(snapmaker_extruder_map_pairs_are_identity({{0, 0}, {1, 0}}));
    CHECK_FALSE(snapmaker_extruder_map_pairs_are_identity({{0, 0}, {4, 4}}));
    CHECK(snapmaker_script_without_commands("A x\nB y\nC z", {"B"}) == "A x\nC z");
    CHECK(snapmaker_script_without_commands("A x\nB y", {"A", "B"}).empty());
    CHECK(snapmaker_script_without_commands("AB x\nB y", {"A"}) == "AB x\nB y"); // a prefix of a longer command name is not that command
}

TEST_CASE("pre-print session: F7 a dropped preference command is said in the reply", "[SSWCP][preprint_profile]")
{
    Profile h;
    h.printer->known["SET_PRINT_PREFERENCES"] = Known::No;
    const json r = h.gcodes(plural());
    REQUIRE(is_ok(r));
    CHECK(r["data"]["dropped_preferences"] == true);
    Profile g;
    const json r2 = g.gcodes(plural());
    REQUIRE(is_ok(r2));
    CHECK_FALSE(r2["data"].contains("dropped_preferences"));
}

TEST_CASE("pre-print session: a dropped preference command is also shown to the user, once, as a notice (#119)", "[SSWCP][preprint_profile]")
{
    Profile h;
    std::vector<std::string> notices;
    h.session->set_notifier([&notices](const std::string& m) { notices.push_back(m); });
    h.printer->known["SET_PRINT_PREFERENCES"] = Known::No;
    REQUIRE(is_ok(h.gcodes(plural())));
    REQUIRE(notices.size() == 1);
    CHECK(notices[0].find("SET_PRINT_PREFERENCES") != std::string::npos);
    CHECK(notices[0].find("not applied") != std::string::npos);
    // the printer has the command: no notice
    Profile g;
    std::vector<std::string> none;
    g.session->set_notifier([&none](const std::string& m) { none.push_back(m); });
    REQUIRE(is_ok(g.gcodes(plural())));
    CHECK(none.empty());
}

TEST_CASE("pre-print bridge: F3 preprint_done is deferred and re-entrancy safe; F8 shutdown and destructor report once", "[SSWCP][preprint_profile]")
{
    SECTION("deferred out of the handler; the callback destroys the bridge")
    {
        Profile h;
        h.bridge->on_script_message(h.good, json{{"header", {{"id", 1}}}, {"payload", {{"cmd", "sw_SetFilamentMappingComplete"}, {"params", json::object()}}}}.dump());
        CHECK(h.done_calls.empty());
        REQUIRE(h.posted.size() == 1);
        // the host destroys the bridge from inside preprint_done
        int fired = 0;
        auto b = std::move(h.bridge);
        SnapmakerDeviceBridge* raw = b.get();
        (void) raw;
        h.pump();
        CHECK(h.done_calls == std::vector<bool>{false});
        b.reset(); // the destructor must not fire a second time
        CHECK(h.done_calls.size() == 1);
        (void) fired;
    }
    SECTION("callback re-enters shutdown()")
    {
        Profile h(true, false);
        SnapmakerDeviceBridge* bp = nullptr;
        int calls = 0;
        SnapmakerDeviceBridge::Options o;
        o.port = 5000; o.http_host = "http://127.0.0.1:5000"; o.preprint = h.ctx; o.preprint_session = h.session; o.preprint_epoch = h.epoch;
        o.run_script = [&](const std::string&) {};
        o.preprint_done = [&](bool) { ++calls; if (bp) bp->shutdown(); };
        auto b = std::make_unique<SnapmakerDeviceBridge>(std::move(o));
        bp = b.get();
        b->shutdown();
        CHECK(calls == 1);
        b.reset();
        CHECK(calls == 1);
    }
    SECTION("destructor closes the session and reports the real started state (F8)")
    {
        Profile h;
        REQUIRE(is_ok(h.gcodes(plural())));
        REQUIRE(is_ok(h.start()));
        h.bridge.reset();
        CHECK(h.done_calls == std::vector<bool>{true});
        CHECK(h.session->state() == SnapmakerPreprintState::Closed);
    }
    SECTION("shutdown() closes the session and voids an in-flight ticket (F8)")
    {
        Profile h;
        h.printer->in_send = [&] { h.bridge->shutdown(); };
        h.bridge->on_script_message(h.good, json{{"header", {{"id", 1}}}, {"payload", {{"cmd", "sw_SendGCodes"}, {"params", {{"script", plural()}}}}}}.dump());
        CHECK(h.session->state() == SnapmakerPreprintState::Closed);
        CHECK(h.done_calls == std::vector<bool>{false});
        CHECK(h.sent.empty()); // the bridge is shut down: the late reply is dropped
    }
    SECTION("without a post hook the report waits for tick()")
    {
        Profile h(true, false);
        SnapmakerDeviceBridge::Options o;
        int calls = 0;
        o.port = 5000; o.http_host = "http://127.0.0.1:5000"; o.preprint = h.ctx; o.preprint_session = h.session; o.preprint_epoch = h.epoch;
        o.run_script = [&](const std::string&) {};
        o.preprint_done = [&](bool) { ++calls; };
        SnapmakerDeviceBridge b(std::move(o));
        b.on_script_message(h.good, json{{"header", {{"id", 1}}}, {"payload", {{"cmd", "sw_FinishFilamentMapping"}, {"params", json::object()}}}}.dump());
        CHECK(calls == 0);
        b.tick();
        CHECK(calls == 1);
    }
}

TEST_CASE("pre-print bridge: the alive flag drops a reply when the bridge was shut down but its instance is still held", "[SSWCP][preprint_profile]")
{
    Profile h;
    h.hold = true;
    h.bridge->on_script_message(h.good, json{{"header", {{"id", 1}}}, {"payload", {{"cmd", "sw_SendGCodes"}, {"params", {{"script", plural()}}}}}}.dump());
    REQUIRE(h.held.size() == 1);
    auto jobs = std::move(h.held);
    h.held.clear();
    jobs[0].first();
    h.bridge->shutdown(); // the bridge object (and so m_sswcp and the instance) still exist; only the alive flag is false
    jobs[0].second();
    CHECK(h.sent.empty());
}

// #136: which filaments the pre-print request describes. These fixtures are SHAPED the way PartPlate fills a fresh slice
// (print_statistics.total_volumes_per_extruder, mm3, 0-based physical ids) and a .gcode.3mf-loaded plate (FilamentInfo::used_m, metres); they
// show the function agrees with that reading, not that a real slice produces those shapes.
TEST_CASE("pre-print used filaments: a fresh slice reads the per-filament volumes (mm3) and ignores the empty 3MF list", "[SSWCP][preprint_profile][used_filaments]")
{
    SnapmakerUsedFilamentsInput in;
    in.total_volumes_mm3 = {{3, 1500.0}, {0, 2000.0}, {1, 250.5}};
    in.filament_diameter = {1.75, 1.75, 1.75, 1.75};
    SnapmakerUsedFilaments out;
    std::string            err = "stale";
    REQUIRE(snapmaker_collect_used_filaments(in, out, err));
    CHECK(err.empty());
    CHECK(out.ids == std::vector<int>{0, 1, 3});
    CHECK(out.volume_mm3.at(0) == 2000.0); // taken as is, mm3: no length round trip
    CHECK(out.volume_mm3.at(1) == 250.5);
    CHECK(out.volume_mm3.at(3) == 1500.0);
    CHECK(out.volume_mm3.size() == 3);
}

TEST_CASE("pre-print used filaments: a volume whose filament has no diameter entry is still kept (no diameter is needed for a volume)", "[SSWCP][preprint_profile][used_filaments]")
{
    SnapmakerUsedFilamentsInput in;
    in.total_volumes_mm3 = {{5, 10.0}};
    SnapmakerUsedFilaments out;
    std::string            err;
    REQUIRE(snapmaker_collect_used_filaments(in, out, err));
    CHECK(out.ids == std::vector<int>{5});
    CHECK(out.volume_mm3.at(5) == 10.0);
}

TEST_CASE("pre-print used filaments: a .gcode.3mf-loaded plate falls back to used_m (metres) with the volume from the diameter", "[SSWCP][preprint_profile][used_filaments]")
{
    SnapmakerUsedFilamentsInput in;
    in.loaded            = {{2, 1.0f}, {0, 0.5f}, {-1, 9.f}, {7, 1.0f}}; // 7 has no diameter, -1 is dropped
    in.filament_diameter = {2.0, 1.75, 2.0};
    SnapmakerUsedFilaments out;
    std::string            err;
    REQUIRE(snapmaker_collect_used_filaments(in, out, err));
    CHECK(out.ids == std::vector<int>{2, 0, 7}); // file order
    // 1 m of 2.0 mm filament = 1000 mm * pi * 1^2 mm2
    CHECK(out.volume_mm3.at(2) == Catch::Approx(1000.0 * 3.14159265358979323846).epsilon(1e-9));
    CHECK(out.volume_mm3.at(0) == Catch::Approx(500.0 * 3.14159265358979323846).epsilon(1e-9));
    CHECK(out.volume_mm3.count(7) == 0);
}

TEST_CASE("pre-print used filaments: the fresh volumes win over a stale 3MF list; zero or non-finite volumes are not use", "[SSWCP][preprint_profile][used_filaments]")
{
    SnapmakerUsedFilamentsInput in;
    in.total_volumes_mm3 = {{1, 100.0}, {2, 0.0}, {3, std::nan("")}};
    in.loaded            = {{0, 1.f}};
    in.filament_diameter = {1.75, 1.75};
    SnapmakerUsedFilaments out;
    std::string            err;
    REQUIRE(snapmaker_collect_used_filaments(in, out, err));
    CHECK(out.ids == std::vector<int>{1});
}

TEST_CASE("pre-print used filaments: nothing used is the 'no used filaments' error", "[SSWCP][preprint_profile][used_filaments]")
{
    SnapmakerUsedFilamentsInput in;
    in.total_volumes_mm3 = {{0, 0.0}};
    in.loaded            = {{-1, 1.f}};
    SnapmakerUsedFilaments out;
    std::string            err;
    CHECK_FALSE(snapmaker_collect_used_filaments(in, out, err));
    CHECK(err == "The plate reports no used filaments.");
    CHECK(out.ids.empty());
}
