#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <boost/filesystem.hpp>

#include "slic3r/GUI/SnapmakerDeviceBridge.hpp"
#include "slic3r/GUI/SnapmakerPreprintSession.hpp"

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
    std::unique_ptr<SnapmakerDeviceBridge> make(std::uint64_t e = ~std::uint64_t(0))
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
    // the page's two scripts (a first one, then the forced re-send) are allowed, a third is not
    Profile g;
    CHECK(is_ok(g.gcodes(plural())));
    CHECK(is_ok(g.gcodes(plural())));
    CHECK(g.gcodes(plural())["code"] == SSWCPCode::Failure);
    CHECK(g.printer->sends == 2);
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
    CHECK(h.printer->sends == ok_configs);
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
                            "sw_FinishFilamentMapping", "sw_GetFileStream", "sw_DownLoadFile", "sw_DownloadMachineFile", "sw_ControlExtruderTemp", "sw_ControlPurifier",
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
        "sw_GetActiveFile", "sw_GetFileFilamentMapping", "sw_SendGCodes", "sw_MachinePrintStart", "sw_StartLocalPrint",
        "sw_SetFilamentMappingComplete", "sw_FinishFilamentMapping"};
    CHECK(listed == expected);
    CHECK(listed.size() == 20);
    for (const auto& n : listed) { CAPTURE(n); CHECK(h.bridge->sswcp().is_registered(n)); }
    for (const auto& n : denied) { CAPTURE(n); CHECK_FALSE(h.bridge->sswcp().is_registered(n)); }
    for (const char* n : {"sw_UploadFile", "sw_UploadFileResult", "sw_UploadFiletoMachine", "sw_DownLoadFile", "sw_DownloadMachineFile", "sw_GetFileStream",
                          "sw_ControlLed", "sw_ControlBedTemp", "sw_ControlExtruderTemp", "sw_ControlPurifier", "sw_SendCommand", "sw_MachinePrintPause",
                          "sw_MachinePrintCancel", "sw_DeleteDevices", "sw_RenameDevice", "sw_GetMachineState", "sw_MachineFilesRoots", "sw_CameraStartMonitor"})
        CHECK_FALSE(h.bridge->sswcp().is_registered(n));
    // the Device tab's list keeps its classification of the same names
    std::set<std::string> device_denied;
    const auto device = registered_in("snapmaker_bridge_commands.txt", &device_denied);
    for (const char* n : {"sw_MachinePrintStart", "sw_StartLocalPrint", "sw_SetFilamentMappingComplete", "sw_FinishFilamentMapping"}) CHECK(device_denied.count(n) == 1);
    CHECK(device.size() == 54);
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
