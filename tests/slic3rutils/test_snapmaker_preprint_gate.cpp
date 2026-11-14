#include <atomic>
#include <cctype>
#include <catch2/catch_test_macros.hpp>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "slic3r/Utils/SnapmakerPreprintGate.hpp"

// Goldens here come from our own builders (SnapmakerPrintPreference.hpp, SnapmakerPreprintMapping.hpp, build_snapmaker_*_preference_script)
// and from our reading of the page JS (main.b347974c91352c2f.js 39805-39816, 148176-148187): the literals pinned below are that reading
// typed out by hand. They show the validator agrees with those, NOT that firmware or the page behaves that way, and not that the page
// never sends a shape we reject.

using namespace Slic3r;
using D = SnapmakerPreprintDecision;

namespace {
// Plate uses filaments 0,1,2; non-identity map 0->2, 1->0, 2->2 (extruder 1 unused). mapping[f] = extruder.
const std::vector<int> kMapping   = {2, 0, 2};
const std::vector<int> kFilaments = {0, 1, 2};
const std::vector<std::pair<std::string, int>> kPrefs = {{"bed_level", 1}, {"flow_calibrate", 0}};

std::string plural_golden(const std::vector<std::pair<std::string, int>>& prefs = kPrefs) { return build_snapmaker_plural_preference_script(kMapping, prefs); }
std::string singular_golden() { return build_snapmaker_singular_preference_script(kMapping, true, false, true); }

// The page's own order (bed_level, flow_calibrate, time_lapse_camera), typed from the JS reading, plate {0,1}, map 0->1, 1->0.
const std::string kPagePlural = "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=0 MAP_EXTRUDER=1\nSET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=1 MAP_EXTRUDER=0\n"
                                "SET_PRINT_USED_EXTRUDERS EXTRUDERS=1,0\nSET_PRINT_PREFERENCES BED_LEVEL=1 FLOW_CALIBRATE=0 TIME_LAPSE_CAMERA=1";
const std::string kPageSingular = "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=0 MAP_EXTRUDER=1\nSET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=1 MAP_EXTRUDER=0\n"
                                  "SET_PRINT_USED_EXTRUDERS EXTRUDERS=1,0\nSET_PRINT_PREFERENCE KEY=bed_level VALUE=1\n"
                                  "SET_PRINT_PREFERENCE KEY=flow_calibrate VALUE=0\nSET_PRINT_PREFERENCE KEY=time_lapse_camera VALUE=1";

SnapmakerPreprintGcode check(const std::string& s, int extruders = 4, const std::vector<int>& f = kFilaments) { return validate_snapmaker_preprint_gcode(s, f, extruders); }
bool rejects(const std::string& s, int extruders = 4) { return !check(s, extruders).ok; }
bool rejects_with(const std::string& s, const char* why) { const auto r = check(s); return !r.ok && r.error.find(why) != std::string::npos; }
// Replace the first occurrence of `what` in `s`.
std::string repl(std::string s, const std::string& what, const std::string& with)
{
    const auto i = s.find(what);
    REQUIRE(i != std::string::npos);
    return s.replace(i, what.size(), with);
}

const SnapmakerPreprintPin kPin{"192.168.1.50", "7125", "U1-123"};
SnapmakerPreprintOpen open_params(bool configure_required = true)
{
    SnapmakerPreprintOpen o;
    o.printer            = kPin;
    o.remote_filename    = "job.gcode";
    o.plate_filaments    = kFilaments;
    o.extruder_count     = 4;
    o.snapshot_size      = 1234;
    o.configure_required = configure_required;
    return o;
}
const SnapmakerPreprintGate::IdlePredicate     kIdle = [] { return true; };
const SnapmakerPreprintGate::IdlePredicate     kBusy = [] { return false; };
const SnapmakerPreprintGate::MetadataPredicate kMetaOk = [](const std::string&, std::uint64_t sz) { return sz == 1234; };
const SnapmakerPreprintGate::MetadataPredicate kMetaBad = [](const std::string&, std::uint64_t) { return false; };

// A gate opened with the given params plus its epoch, and shorthands that confirm success.
struct G
{
    SnapmakerPreprintGate g;
    std::uint64_t         e = 0;
    explicit G(bool configure_required = true)
    {
        std::string err;
        e = g.open(open_params(configure_required), err);
        REQUIRE(err.empty());
        REQUIRE(e > 0);
    }
    SnapmakerPreprintResult configure(const std::string& s = plural_golden(), const SnapmakerPreprintPin& p = kPin, const SnapmakerPreprintGate::IdlePredicate& i = kIdle)
    {
        return g.request_configure(e, s, p, i);
    }
    SnapmakerPreprintResult start(const std::string& f = "job.gcode", const SnapmakerPreprintPin& p = kPin, const SnapmakerPreprintGate::IdlePredicate& i = kIdle,
                                  const SnapmakerPreprintGate::MetadataPredicate& m = kMetaOk)
    {
        return g.request_start(e, f, p, i, m);
    }
    D configure_ok(const std::string& s = plural_golden()) { auto r = configure(s); if (r.allowed()) CHECK(g.confirm(r.ticket, true)); return r.decision; }
    D start_ok() { auto r = start(); if (r.allowed()) CHECK(g.confirm(r.ticket, true)); return r.decision; }
};

// Blocks inside a predicate until released, so a test can act while a request is "in flight".
struct Latch
{
    std::mutex              m;
    std::condition_variable cv;
    bool                    entered = false, released = false;
    void enter_and_wait()
    {
        std::unique_lock<std::mutex> l(m);
        entered = true;
        cv.notify_all();
        cv.wait(l, [&] { return released; });
    }
    void wait_entered()
    {
        std::unique_lock<std::mutex> l(m);
        cv.wait(l, [&] { return entered; });
    }
    void release()
    {
        std::lock_guard<std::mutex> l(m);
        released = true;
        cv.notify_all();
    }
};
} // namespace

TEST_CASE("goldens: the builders' output is accepted, with a non-identity map", "[SnapmakerPreprintGate]")
{
    const std::string pl = plural_golden();
    REQUIRE(pl == "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=0 MAP_EXTRUDER=2\nSET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=1 MAP_EXTRUDER=0\n"
                  "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=2 MAP_EXTRUDER=2\nSET_PRINT_USED_EXTRUDERS EXTRUDERS=2,0\n"
                  "SET_PRINT_PREFERENCES BED_LEVEL=1 FLOW_CALIBRATE=0");
    auto r = check(pl);
    REQUIRE(r.ok);
    CHECK(r.shape == SnapmakerPreprintShape::Plural);
    CHECK(r.map.size() == 3);
    CHECK(r.prefs.size() == 2);
    // map + used only, each newline-terminated: accepted, labelled Plural (header comment)
    auto np = check(plural_golden({}));
    CHECK(np.ok);
    CHECK(np.shape == SnapmakerPreprintShape::Plural);
    auto s = check(singular_golden());
    REQUIRE(s.ok);
    CHECK(s.shape == SnapmakerPreprintShape::Singular);
    CHECK(s.prefs.size() == 3);
}

TEST_CASE("literals pinned from the page JS, all three keys in the page's order, both shapes", "[SnapmakerPreprintGate]")
{
    auto p = check(kPagePlural, 2, {0, 1});
    REQUIRE(p.ok);
    CHECK(p.shape == SnapmakerPreprintShape::Plural);
    REQUIRE(p.prefs.size() == 3);
    CHECK(p.prefs[0].first == "bed_level");
    CHECK(p.prefs[1].first == "flow_calibrate");
    CHECK(p.prefs[2].first == "time_lapse_camera");
    auto s = check(kPageSingular, 2, {0, 1});
    REQUIRE(s.ok);
    CHECK(s.shape == SnapmakerPreprintShape::Singular);
    CHECK(s.prefs == p.prefs);
    // our builders reproduce the literals (same reading, typed twice)
    CHECK(build_snapmaker_plural_preference_script({1, 0}, {{"bed_level", 1}, {"flow_calibrate", 0}, {"time_lapse_camera", 1}}) == kPagePlural);
    CHECK(build_snapmaker_singular_preference_script({1, 0}, false, true, true) == kPageSingular);
}

TEST_CASE("choice recorded: preference-only scripts are refused (fail closed)", "[SnapmakerPreprintGate]")
{
    // The exact bytes the page would send if it had no map / used lines (R7 with an empty map table, aSb likewise).
    CHECK(rejects("SET_PRINT_PREFERENCES BED_LEVEL=1 FLOW_CALIBRATE=0 TIME_LAPSE_CAMERA=1"));
    CHECK(rejects("SET_PRINT_PREFERENCE KEY=bed_level VALUE=1\nSET_PRINT_PREFERENCE KEY=flow_calibrate VALUE=0"));
    // map + used with no newline and no preference line is also refused
    CHECK(rejects(plural_golden({}).substr(0, plural_golden({}).size() - 1)));
}

TEST_CASE("every key of the allow-list is produced by the singular builder", "[SnapmakerPreprintGate]")
{
    const std::string s = build_snapmaker_print_preference_script(true, true, true);
    for (const char* k : snapmaker_print_preference_keys)
        CHECK(s.find(std::string("KEY=") + k + " ") != std::string::npos);
}

TEST_CASE("validator rejects structure", "[SnapmakerPreprintGate]")
{
    const std::string pl = plural_golden();
    const std::string sg = singular_golden();
    CHECK(rejects(""));
    CHECK(rejects(pl + "\nG28"));
    CHECK(rejects("G28\n" + pl));
    CHECK(rejects(sg + "\nM104 S200"));
    CHECK(rejects(pl + " "));
    CHECK(rejects(" " + pl));
    CHECK(rejects(pl + "\n"));  // newline after the plural preference line
    CHECK(rejects(sg + "\n"));  // trailing newline after singular
    CHECK(rejects(repl(pl, "CONFIG_EXTRUDER=1", "CONFIG_EXTRUDER=0")));                      // duplicate CONFIG_EXTRUDER
    CHECK(rejects(repl(pl, "CONFIG_EXTRUDER=2", "CONFIG_EXTRUDER=3")));                      // set differs from the plate
    CHECK(rejects(pl, 2));                                                                    // MAP 2 >= 2 extruders
    CHECK(rejects(repl(pl, "EXTRUDERS=2,0", "EXTRUDERS=2,1")));                              // USED mismatch
    CHECK(rejects(repl(pl, "EXTRUDERS=2,0", "EXTRUDERS=2")));
    CHECK(rejects(repl(pl, "EXTRUDERS=2,0", "EXTRUDERS=2,0,0")));
    CHECK(rejects(repl(pl, "BED_LEVEL", "BED_LEVELS")));                                     // unknown key
    CHECK(rejects(repl(pl, "BED_LEVEL", "bed_level")));                                      // plural wants upper case
    CHECK(rejects(repl(sg, "bed_level", "BED_LEVEL")));                                      // singular wants lower case
    CHECK(rejects(repl(pl, "BED_LEVEL=1", "BED_LEVEL=2")));                                  // value out of range
    CHECK(rejects(repl(sg, "VALUE=1", "VALUE=7")));
    CHECK(rejects(pl + " BED_LEVEL=1"));                                                      // duplicate key
    CHECK(rejects(pl.substr(0, pl.find("SET_PRINT_USED")) + "SET_PRINT_PREFERENCES BED_LEVEL=1")); // no used line
    CHECK(rejects("SET_PRINT_USED_EXTRUDERS EXTRUDERS=0\nSET_PRINT_PREFERENCES BED_LEVEL=1"));    // no map
    CHECK(rejects("SET_PRINT_PREFERENCES BED_LEVEL=1\n" + pl));                               // preference before map
    CHECK(rejects(pl + "\nSET_PRINT_PREFERENCE KEY=bed_level VALUE=1"));                      // plural + singular mixed
    CHECK(rejects(plural_golden() + "\nSET_PRINT_PREFERENCEX KEY=bed_level VALUE=1"));
    CHECK(rejects(singular_golden() + "\nSET_PRINT_PREFERENCES BED_LEVEL=1"));
    CHECK(rejects(sg.substr(0, sg.find("SET_PRINT_PREFERENCE") - 1)));              // map + used, no newline, no preferences
}

TEST_CASE("validator: each guard is isolated by its error text", "[SnapmakerPreprintGate]")
{
    const std::string pl = plural_golden();
    const std::string sg = singular_golden();
    // length cap: spaces after a valid script would otherwise fail on a different rule
    CHECK(rejects_with(std::string(5000, 'A'), "too long"));
    CHECK(rejects_with(pl + std::string(snapmaker_preprint_max_gcode_bytes, ' '), "too long"));
    CHECK(check(pl).ok);
    // ';' guard (printable, so only the comment rule catches it)
    CHECK(rejects_with(pl + " ;x", "comment"));
    CHECK(rejects_with("; hi\n" + pl, "comment"));
    CHECK(rejects_with(repl(sg, "VALUE=1", "VALUE=1;"), "comment"));
    // control / non-ASCII guard, in a key, a value, and a map slot
    const std::vector<std::string> bad = {std::string("\0", 1), "\t", "\r", "\x7f", "\xE2\x80\xA8" /*U+2028*/, "\xC2\x85" /*U+0085*/, "\xC3\xA9", "\x1b", "\xFF"};
    for (const auto& b : bad) {
        CHECK(rejects_with(repl(pl, "BED_LEVEL=1", "BED_LEVEL=1" + b), "control or non-ASCII"));
        CHECK(rejects_with(repl(pl, "BED_LEVEL", "BED" + b + "_LEVEL"), "control or non-ASCII"));
        CHECK(rejects_with(repl(sg, "VALUE=1", "VALUE=" + b + "1"), "control or non-ASCII"));
        CHECK(rejects_with(repl(pl, "MAP_EXTRUDER=2\n", "MAP_EXTRUDER=2" + b + "\n"), "control or non-ASCII"));
    }
    // blank lines, isolated: each is otherwise a well-formed script
    CHECK(rejects_with(repl(sg, "\nSET_PRINT_PREF", "\n\nSET_PRINT_PREF"), "blank line"));
    CHECK(rejects_with(repl(pl, "\nSET_PRINT_USED", "\n\nSET_PRINT_USED"), "blank line"));
    CHECK(rejects_with("\n" + pl, "blank line"));
    CHECK(rejects_with(plural_golden({}) + "\n", "blank line"));
}

TEST_CASE("validator: malformed numbers in every numeric slot", "[SnapmakerPreprintGate]")
{
    const std::string pl = plural_golden();
    const std::string sg = singular_golden();
    const std::vector<std::string> bad = {"100", "010", "00", "4294967297", "99999999999999999999", "0x1", "+1", "-1", "1a", "a", "", " 1", "1 ", "1.0", "1e0"};
    for (const auto& b : bad) {
        INFO("token '" << b << "'");
        CHECK(rejects(repl(pl, "CONFIG_EXTRUDER=0 ", "CONFIG_EXTRUDER=" + b + " ")));
        CHECK(rejects(repl(pl, "MAP_EXTRUDER=2\n", "MAP_EXTRUDER=" + b + "\n")));
        CHECK(rejects(repl(pl, "EXTRUDERS=2,0", "EXTRUDERS=" + b + ",0")));
        CHECK(rejects(repl(pl, "EXTRUDERS=2,0", "EXTRUDERS=2," + b)));
        CHECK(rejects(repl(pl, "BED_LEVEL=1", "BED_LEVEL=" + b)));
        CHECK(rejects(repl(sg, "VALUE=1", "VALUE=" + b)));
    }
    // a valid two-digit filament index is accepted (so the rejections above are about the token, not the slot)
    CHECK(check(repl(pl, "CONFIG_EXTRUDER=2 ", "CONFIG_EXTRUDER=12 "), 4, {0, 1, 12}).ok);
}

TEST_CASE("gate: no open context refuses everything", "[SnapmakerPreprintGate]")
{
    SnapmakerPreprintGate g;
    CHECK(g.request_configure(0, plural_golden(), kPin, kIdle).decision == D::RefusedNoContext);
    CHECK(g.request_start(0, "job.gcode", kPin, kIdle, kMetaOk).decision == D::RefusedNoContext);
}

TEST_CASE("gate: open validates its parameters and leaves the gate Idle on error", "[SnapmakerPreprintGate]")
{
    auto bad = [](auto mutate) {
        SnapmakerPreprintGate g;
        std::string           err;
        CHECK(g.open(open_params(), err) > 0); // a good open first: a failed reopen must void it
        auto o = open_params();
        mutate(o);
        CHECK(g.open(o, err) == 0);
        CHECK(!err.empty());
        CHECK(g.state() == SnapmakerPreprintState::Idle);
        CHECK(g.request_start(1, "job.gcode", kPin, kIdle, kMetaOk).decision != D::Allowed);
    };
    bad([](SnapmakerPreprintOpen& o) { o.printer.host.clear(); });
    bad([](SnapmakerPreprintOpen& o) { o.printer.port.clear(); });
    bad([](SnapmakerPreprintOpen& o) { o.printer.serial.clear(); });
    bad([](SnapmakerPreprintOpen& o) { o.remote_filename.clear(); });
    bad([](SnapmakerPreprintOpen& o) { o.extruder_count = 0; });
    bad([](SnapmakerPreprintOpen& o) { o.extruder_count = 5; });
    bad([](SnapmakerPreprintOpen& o) { o.plate_filaments.clear(); });
    bad([](SnapmakerPreprintOpen& o) { o.plate_filaments = {0, 0}; });
    bad([](SnapmakerPreprintOpen& o) { o.plate_filaments = {-1}; });
    bad([](SnapmakerPreprintOpen& o) { o.plate_filaments = {snapmaker_max_filaments}; });
}

TEST_CASE("gate: happy path and state transitions need confirm", "[SnapmakerPreprintGate]")
{
    G t;
    CHECK(t.g.state() == SnapmakerPreprintState::Open);
    auto c = t.configure();
    REQUIRE(c.allowed());
    CHECK(t.g.state() == SnapmakerPreprintState::Open); // Configured only on confirm
    CHECK(t.g.confirm(c.ticket, true));
    CHECK(t.g.state() == SnapmakerPreprintState::Configured);
    auto s = t.start();
    REQUIRE(s.allowed());
    CHECK(t.g.state() == SnapmakerPreprintState::Configured); // Started only on confirm
    CHECK(t.g.confirm(s.ticket, true));
    CHECK(t.g.state() == SnapmakerPreprintState::Started);
    CHECK(t.start().decision == D::RefusedAlreadyStarted);
    CHECK(t.configure().decision == D::RefusedState);
    CHECK_FALSE(t.g.confirm(s.ticket, true)); // a ticket is single use
}

TEST_CASE("gate: a failed forward rolls back and keeps the budgets", "[SnapmakerPreprintGate]")
{
    G t;
    for (int i = 0; i < 3; ++i) { // failed configures never use a slot
        auto c = t.configure();
        REQUIRE(c.allowed());
        CHECK(t.g.confirm(c.ticket, false));
        CHECK(t.g.state() == SnapmakerPreprintState::Open);
    }
    CHECK(t.configure_ok() == D::Allowed);
    CHECK(t.configure_ok() == D::Allowed);
    CHECK(t.configure_ok() == D::RefusedTooManyScripts);
    auto s = t.start();
    REQUIRE(s.allowed());
    CHECK(t.g.confirm(s.ticket, false)); // failed start: back to Configured, budget intact
    CHECK(t.g.state() == SnapmakerPreprintState::Configured);
    CHECK(t.start_ok() == D::Allowed);
    CHECK(t.g.state() == SnapmakerPreprintState::Started);
    CHECK(t.start_ok() == D::RefusedAlreadyStarted);
}

TEST_CASE("gate: an unconfirmed ticket keeps the gate in flight", "[SnapmakerPreprintGate]")
{
    G t;
    auto c = t.configure();
    REQUIRE(c.allowed());
    CHECK(t.configure().decision == D::RefusedInFlight);
    CHECK(t.start().decision == D::RefusedInFlight);
    CHECK(t.g.confirm(c.ticket, true));
    auto s = t.start();
    REQUIRE(s.allowed());
    CHECK(t.start().decision == D::RefusedAlreadyStarted);
    CHECK(t.configure().decision == D::RefusedAlreadyStarted);
}

TEST_CASE("gate: start before Configured is refused only when options are non-empty", "[SnapmakerPreprintGate]")
{
    G t(true);
    CHECK(t.start().decision == D::RefusedNotConfigured);
    CHECK(t.g.state() == SnapmakerPreprintState::Open);
    G u(false);
    CHECK(u.start_ok() == D::Allowed);
}

TEST_CASE("gate: exactly one forced re-send", "[SnapmakerPreprintGate]")
{
    G t;
    CHECK(t.configure_ok() == D::Allowed);
    CHECK(t.configure_ok() == D::Allowed); // the page's fallback
    CHECK(t.configure_ok() == D::RefusedTooManyScripts);
    CHECK(t.start_ok() == D::Allowed);
}

TEST_CASE("gate: an invalid or busy script does not consume a slot", "[SnapmakerPreprintGate]")
{
    G t;
    CHECK(t.configure("G28").decision == D::RefusedInvalid);
    CHECK(t.configure(plural_golden(), kPin, kBusy).decision == D::RefusedPrinterBusy);
    CHECK(t.configure(plural_golden(), kPin, nullptr).decision == D::RefusedPrinterBusy); // fail closed
    CHECK(t.g.state() == SnapmakerPreprintState::Open);
    CHECK(t.configure_ok() == D::Allowed);
    CHECK(t.configure_ok() == D::Allowed);
}

TEST_CASE("gate: refusals at start", "[SnapmakerPreprintGate]")
{
    G t(false);
    CHECK(t.start("other.gcode").decision == D::RefusedFilename);
    CHECK(t.start("").decision == D::RefusedFilename);
    CHECK(t.start("job.gcode", kPin, kBusy).decision == D::RefusedPrinterBusy); // printing / paused
    CHECK(t.start("job.gcode", kPin, nullptr).decision == D::RefusedPrinterBusy);
    CHECK(t.start("job.gcode", kPin, kIdle, kMetaBad).decision == D::RefusedMetadata);
    CHECK(t.start("job.gcode", kPin, kIdle, nullptr).decision == D::RefusedMetadata);
    CHECK(t.g.state() == SnapmakerPreprintState::Open); // none of the refusals started it
    CHECK(t.start_ok() == D::Allowed);
}

TEST_CASE("gate: the remote filename is compared as an exact string", "[SnapmakerPreprintGate]")
{
    G t(false);
    for (const char* f : {"JOB.gcode", "Job.gcode", "/job.gcode", "gcodes/job.gcode", "job.gcode ", " job.gcode", "job.gcode\n", "job.gcod", "job.gcode.gcode"})
        CHECK(t.start(f).decision == D::RefusedFilename);
    CHECK(t.start_ok() == D::Allowed);
}

TEST_CASE("gate: the printer pin is compared as exact strings (fail closed on any difference)", "[SnapmakerPreprintGate]")
{
    G t(false);
    const std::vector<SnapmakerPreprintPin> others = {
        {"192.168.1.51", "7125", "U1-123"}, {"192.168.1.50", "7126", "U1-123"}, {"192.168.1.50", "80", "U1-123"}, {"192.168.1.50", "07125", "U1-123"},
        {"192.168.1.50", "7125", "other"},   {"192.168.1.50", "7125", "u1-123"}, {"192.168.1.50", "7125", ""},      {"192.168.1.50.", "7125", "U1-123"},
        {"192.168.1.50 ", "7125", "U1-123"}, {"", "", ""}};
    for (const auto& p : others) {
        CHECK(t.start("job.gcode", p).decision == D::RefusedPrinterChanged);
        CHECK(t.configure(plural_golden(), p).decision == D::RefusedPrinterChanged);
    }
    // hostnames: case and a trailing dot differ from the pin (the caller must canonicalise both sides identically)
    SnapmakerPreprintGate g2;
    std::string           err;
    auto                  o = open_params(false);
    o.printer.host = "u1.local";
    const auto e = g2.open(o, err);
    REQUIRE(e > 0);
    for (const char* h : {"U1.local", "u1.local.", "u1.LOCAL"}) {
        SnapmakerPreprintPin p{h, "7125", "U1-123"};
        CHECK(g2.request_start(e, "job.gcode", p, kIdle, kMetaOk).decision == D::RefusedPrinterChanged);
    }
    CHECK(g2.request_start(e, "job.gcode", SnapmakerPreprintPin{"u1.local", "7125", "U1-123"}, kIdle, kMetaOk).allowed());
}

TEST_CASE("gate: closed from any state refuses everything", "[SnapmakerPreprintGate]")
{
    for (int stage = 0; stage < 3; ++stage) {
        G t;
        if (stage >= 1) t.configure_ok();
        if (stage >= 2) t.start_ok();
        t.g.close();
        CHECK(t.g.state() == SnapmakerPreprintState::Closed);
        CHECK(t.configure().decision == D::RefusedClosed);
        CHECK(t.start().decision == D::RefusedClosed);
    }
    SnapmakerPreprintGate idle_gate; // close on a never-opened gate stays Idle
    idle_gate.close();
    CHECK(idle_gate.state() == SnapmakerPreprintState::Idle);
}

TEST_CASE("gate: reopen bumps the epoch and refuses stale dialogs", "[SnapmakerPreprintGate]")
{
    SnapmakerPreprintGate g;
    std::string           err;
    const auto            e1 = g.open(open_params(false), err);
    const auto            e2 = g.open(open_params(false), err);
    CHECK(e2 > e1);
    CHECK(g.request_start(e1, "job.gcode", kPin, kIdle, kMetaOk).decision == D::RefusedStaleEpoch);
    CHECK(g.request_configure(e1, plural_golden(), kPin, kIdle).decision == D::RefusedStaleEpoch);
    CHECK(g.request_start(e2 + 1, "job.gcode", kPin, kIdle, kMetaOk).decision == D::RefusedStaleEpoch);
    CHECK(g.request_start(0, "job.gcode", kPin, kIdle, kMetaOk).decision == D::RefusedStaleEpoch);
    CHECK(g.state() == SnapmakerPreprintState::Open);
    CHECK(g.request_start(e2, "job.gcode", kPin, kIdle, kMetaOk).allowed());
    // an old ticket cannot confirm into the new open
    SnapmakerPreprintGate h;
    const auto            f1 = h.open(open_params(false), err);
    const auto            r1 = h.request_start(f1, "job.gcode", kPin, kIdle, kMetaOk);
    REQUIRE(r1.allowed());
    const auto f2 = h.open(open_params(false), err);
    CHECK_FALSE(h.confirm(r1.ticket, true));
    CHECK(h.state() == SnapmakerPreprintState::Open);
    CHECK(h.request_start(f2, "job.gcode", kPin, kIdle, kMetaOk).allowed());
}

TEST_CASE("gate: close or reopen during a predicate wins, for configure and start", "[SnapmakerPreprintGate]")
{
    {
        G t;
        auto r = t.configure(plural_golden(), kPin, [&] { t.g.close(); return true; });
        CHECK(r.decision == D::RefusedClosed);
        CHECK(t.g.state() == SnapmakerPreprintState::Closed);
    }
    {
        G t(false);
        auto r = t.start("job.gcode", kPin, [&] { t.g.close(); return true; }, kMetaOk);
        CHECK(r.decision == D::RefusedClosed);
        CHECK(t.g.state() == SnapmakerPreprintState::Closed);
    }
    {
        G t(false); // closed while the METADATA predicate runs (idle already passed)
        auto r = t.start("job.gcode", kPin, kIdle, [&](const std::string&, std::uint64_t) { t.g.close(); return true; });
        CHECK(r.decision == D::RefusedClosed);
        CHECK(t.g.state() == SnapmakerPreprintState::Closed);
    }
    {
        G t(false); // reopened while a predicate runs: the old request is refused, the new open is untouched
        std::string err;
        auto r = t.start("job.gcode", kPin, kIdle, [&](const std::string&, std::uint64_t) { t.g.open(open_params(false), err); return true; });
        CHECK_FALSE(r.allowed());
        CHECK(t.g.state() == SnapmakerPreprintState::Open);
    }
    {
        G t; // closed between Allowed and confirm: the confirm is void
        auto r = t.configure();
        REQUIRE(r.allowed());
        t.g.close();
        CHECK_FALSE(t.g.confirm(r.ticket, true));
        CHECK(t.g.state() == SnapmakerPreprintState::Closed);
    }
}

TEST_CASE("gate: a throwing predicate is a refusal and does not wedge the gate", "[SnapmakerPreprintGate]")
{
    G t(false);
    const SnapmakerPreprintGate::IdlePredicate throwing_idle = []() -> bool { throw std::runtime_error("boom"); };
    const SnapmakerPreprintGate::MetadataPredicate throwing_meta = [](const std::string&, std::uint64_t) -> bool { throw std::runtime_error("boom"); };
    CHECK(t.configure(plural_golden(), kPin, throwing_idle).decision == D::RefusedPrinterBusy);
    CHECK(t.start("job.gcode", kPin, throwing_idle).decision == D::RefusedPrinterBusy);
    CHECK(t.start("job.gcode", kPin, kIdle, throwing_meta).decision == D::RefusedMetadata);
    // not in flight afterwards
    auto c = t.configure();
    REQUIRE(c.allowed());
    CHECK(t.g.confirm(c.ticket, true));
    CHECK(t.start_ok() == D::Allowed);
}

TEST_CASE("gate: RefusedInFlight is deterministic while a predicate blocks", "[SnapmakerPreprintGate]")
{
    { // configure blocked in its idle predicate
        G       t;
        Latch   l;
        SnapmakerPreprintResult first;
        std::thread th([&] { first = t.configure(plural_golden(), kPin, [&] { l.enter_and_wait(); return true; }); });
        l.wait_entered();
        CHECK(t.configure().decision == D::RefusedInFlight);
        CHECK(t.start().decision == D::RefusedInFlight);
        l.release();
        th.join();
        REQUIRE(first.allowed());
        CHECK(t.g.confirm(first.ticket, true));
    }
    { // start blocked in its metadata predicate
        G       t(false);
        Latch   l;
        SnapmakerPreprintResult first;
        std::thread th([&] { first = t.start("job.gcode", kPin, kIdle, [&](const std::string&, std::uint64_t) { l.enter_and_wait(); return true; }); });
        l.wait_entered();
        CHECK(t.start().decision == D::RefusedAlreadyStarted);
        CHECK(t.configure().decision == D::RefusedAlreadyStarted);
        l.release();
        th.join();
        REQUIRE(first.allowed());
        CHECK(t.g.confirm(first.ticket, true));
        CHECK(t.g.state() == SnapmakerPreprintState::Started);
    }
}

TEST_CASE("gate: concurrent starts allow exactly one", "[SnapmakerPreprintGate]")
{
    for (int round = 0; round < 50; ++round) {
        G                        t(false);
        std::atomic<int>         allowed{0};
        std::vector<std::thread> ts;
        for (int i = 0; i < 8; ++i)
            ts.emplace_back([&] {
                auto r = t.start();
                if (r.allowed()) {
                    ++allowed;
                    t.g.confirm(r.ticket, true);
                }
            });
        for (auto& th : ts) th.join();
        CHECK(allowed == 1);
        CHECK(t.g.state() == SnapmakerPreprintState::Started);
    }
}

TEST_CASE("gate: concurrent configures never exceed the cap, and configure/start/close race safely", "[SnapmakerPreprintGate]")
{
    for (int round = 0; round < 50; ++round) {
        G                        t;
        std::atomic<int>         allowed{0}, started{0};
        std::vector<std::thread> ts;
        for (int i = 0; i < 6; ++i)
            ts.emplace_back([&] {
                auto r = t.configure();
                if (r.allowed() && t.g.confirm(r.ticket, true)) ++allowed;
            });
        for (int i = 0; i < 4; ++i)
            ts.emplace_back([&] {
                auto r = t.start();
                if (r.allowed() && t.g.confirm(r.ticket, true)) ++started;
            });
        ts.emplace_back([&] { t.g.close(); });
        for (auto& th : ts) th.join();
        CHECK(allowed <= snapmaker_preprint_max_configures);
        CHECK(started <= 1);
        CHECK(t.g.state() == SnapmakerPreprintState::Closed);
    }
}

// S5: upload-only in the send dialog means the context never allows a start, even in the right state with the right file.
TEST_CASE("gate: allow_start=false refuses every start and every configure (read-only upload)", "[SnapmakerPreprintGate]")
{
    SnapmakerPreprintGate g;
    auto o = open_params(false);
    o.allow_start = false;
    std::string err;
    const auto e = g.open(o, err);
    REQUIRE(e > 0);
    CHECK(g.precheck_start(e, "job.gcode") == D::RefusedStartDisallowed);
    CHECK(g.request_start(e, "job.gcode", kPin, kIdle, kMetaOk).decision == D::RefusedStartDisallowed);
    CHECK(g.state() == SnapmakerPreprintState::Open);
    // Upload-only is read-only toward the printer: the configure script is refused as well (the page's send route is not shown to need it).
    CHECK(g.precheck_configure(e, plural_golden()) == D::RefusedStartDisallowed);
    CHECK(g.request_configure(e, plural_golden(), kPin, kIdle).decision == D::RefusedStartDisallowed);
    CHECK(g.state() == SnapmakerPreprintState::Open);
    // default stays permissive
    SnapmakerPreprintGate g2;
    const auto e2 = g2.open(open_params(false), err);
    CHECK(g2.request_start(e2, "job.gcode", kPin, kIdle, kMetaOk).decision == D::Allowed);
}
