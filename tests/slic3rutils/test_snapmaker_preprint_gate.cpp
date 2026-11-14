#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <thread>

#include "slic3r/Utils/SnapmakerPreprintGate.hpp"

// Goldens here come from our own builders (SnapmakerPrintPreference.hpp, SnapmakerPreprintMapping.hpp, build_snapmaker_plural_preference_script)
// and from our reading of the page JS (main.b347974c91352c2f.js 39805-39816, 148176-148187). They show the validator agrees with those, NOT
// that firmware or the page behaves that way, and not that the page never sends a shape we reject.

using namespace Slic3r;
using D = SnapmakerPreprintDecision;

namespace {
// Plate uses filaments 0,2,3; non-identity map 0->2, 2->0, 3->2 (extruder 1 unused). mapping[f] = extruder, -1 = unused/unmapped.
const std::vector<int> kMapping   = {2, -1, 0, 2};
const std::vector<int> kFilaments = {0, 2, 3};

std::string plural_golden(const std::vector<std::pair<std::string, int>>& prefs = {{"bed_level", 1}, {"flow_calibrate", 0}})
{
    // build_snapmaker_extruder_map_script wants `used` to skip filament 1
    const auto m = build_snapmaker_extruder_map_script(kMapping, {true, false, true, true});
    std::string out = m.script + "\n";
    if (!prefs.empty()) {
        out += "SET_PRINT_PREFERENCES";
        for (const auto& kv : prefs) {
            std::string k = kv.first;
            for (char& c : k) c = char(std::toupper((unsigned char)c));
            out += " " + k + "=" + std::to_string(kv.second);
        }
    }
    return out;
}
std::string singular_golden()
{
    const auto m = build_snapmaker_extruder_map_script(kMapping, {true, false, true, true});
    return m.script + "\n" + build_snapmaker_print_preference_script(true, false, true);
}
SnapmakerPreprintGcode check(const std::string& s, int extruders = 4) { return validate_snapmaker_preprint_gcode(s, kFilaments, extruders); }
bool rejects(const std::string& s, int extruders = 4) { return !check(s, extruders).ok; }

const SnapmakerPreprintPin kPin{"192.168.1.50", "7125", "U1-123"};
bool yes() { return true; }
bool no() { return false; }

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
} // namespace

TEST_CASE("goldens: the builders' output is accepted, with a non-identity map", "[SnapmakerPreprintGate]")
{
    const std::string pl = plural_golden();
    REQUIRE(pl == "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=0 MAP_EXTRUDER=2\nSET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=2 MAP_EXTRUDER=0\n"
                  "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=3 MAP_EXTRUDER=2\nSET_PRINT_USED_EXTRUDERS EXTRUDERS=2,0\n"
                  "SET_PRINT_PREFERENCES BED_LEVEL=1 FLOW_CALIBRATE=0");
    auto r = check(pl);
    REQUIRE(r.ok);
    CHECK(r.shape == SnapmakerPreprintShape::Plural);
    CHECK(r.map.size() == 3);
    CHECK(r.prefs.size() == 2);
    CHECK(check(plural_golden({})).ok); // no preferences: ends with "\n"
    const std::string sg = singular_golden();
    auto              s = check(sg);
    REQUIRE(s.ok);
    CHECK(s.shape == SnapmakerPreprintShape::Singular);
    CHECK(s.prefs.size() == 3);
}

TEST_CASE("plural builder output is accepted when every filament is used", "[SnapmakerPreprintGate]")
{
    const std::vector<int> mapping = {1, 3, 1};
    const std::string      s = build_snapmaker_plural_preference_script(mapping, {{"time_lapse_camera", 1}, {"bed_level", 0}});
    CHECK(s.find("SET_PRINT_PREFERENCES TIME_LAPSE_CAMERA=1 BED_LEVEL=0") != std::string::npos);
    CHECK(validate_snapmaker_preprint_gcode(s, {0, 1, 2}, 4).ok);
    const std::string t = build_snapmaker_singular_preference_script(mapping, true, true, true);
    CHECK(validate_snapmaker_preprint_gcode(t, {0, 1, 2}, 4).ok);
}

TEST_CASE("every key of the allow-list is produced by the singular builder", "[SnapmakerPreprintGate]")
{
    const std::string s = build_snapmaker_print_preference_script(true, true, true);
    for (const char* k : snapmaker_print_preference_keys)
        CHECK(s.find(std::string("KEY=") + k + " ") != std::string::npos);
}

TEST_CASE("validator rejects", "[SnapmakerPreprintGate]")
{
    const std::string pl = plural_golden();
    const std::string sg = singular_golden();
    CHECK(rejects(""));
    CHECK(rejects(pl + "\nG28"));
    CHECK(rejects("G28\n" + pl));
    CHECK(rejects(pl + " ;x"));
    CHECK(rejects("; hi\n" + pl));
    CHECK(rejects(sg + "\nM104 S200"));
    { std::string c = pl; c.insert(c.find('\n'), "\r"); CHECK(rejects(c)); }
    CHECK(rejects(pl + " "));
    CHECK(rejects(" " + pl));
    CHECK(rejects(pl + "\n"));                      // newline after the plural preference line
    CHECK(rejects(sg + "\n"));                      // trailing newline after singular
    CHECK(rejects(pl.substr(0, pl.find("SET_PRINT_PREFERENCES")) + "\n")); // blank line
    { std::string c = sg; c.replace(c.find("\nSET_PRINT_PREF"), 1, "\n\n"); CHECK(rejects(c)); }
    { std::string c = pl; c.replace(c.find("CONFIG_EXTRUDER=2"), 17, "CONFIG_EXTRUDER=0"); CHECK(rejects(c)); } // duplicate
    { std::string c = pl; c.replace(c.find("CONFIG_EXTRUDER=2"), 17, "CONFIG_EXTRUDER=1"); CHECK(rejects(c)); } // set differs from plate
    CHECK(rejects(pl, 2));                                                                                         // MAP 2 >= 2 extruders
    { std::string c = pl; c.replace(c.find("EXTRUDERS=2,0"), 13, "EXTRUDERS=2,1"); CHECK(rejects(c)); }           // USED mismatch
    { std::string c = pl; c.replace(c.find("EXTRUDERS=2,0"), 13, "EXTRUDERS=2"); CHECK(rejects(c)); }             // USED missing a value
    { std::string c = pl; c.replace(c.find("EXTRUDERS=2,0"), 13, "EXTRUDERS=2,0,0"); CHECK(rejects(c)); }
    CHECK(rejects(pl.substr(0, pl.find("SET_PRINT_USED")) + "SET_PRINT_PREFERENCES BED_LEVEL=1")); // no used line
    CHECK(rejects("SET_PRINT_USED_EXTRUDERS EXTRUDERS=0\nSET_PRINT_PREFERENCES BED_LEVEL=1"));    // no map
    { std::string c = pl; c.replace(c.find("BED_LEVEL"), 9, "BED_LEVELS"); CHECK(rejects(c)); }  // unknown key
    { std::string c = pl; c.replace(c.find("BED_LEVEL"), 9, "bed_level"); CHECK(rejects(c)); }   // plural wants upper case
    { std::string c = sg; c.replace(c.find("bed_level"), 9, "BED_LEVEL"); CHECK(rejects(c)); }   // singular wants lower case
    { std::string c = pl; c.replace(c.find("BED_LEVEL=1"), 11, "BED_LEVEL=2"); CHECK(rejects(c)); } // value out of range
    { std::string c = sg; c.replace(c.find("VALUE=1"), 7, "VALUE=7"); CHECK(rejects(c)); }
    { std::string c = sg; c.replace(c.find("VALUE=1"), 7, "VALUE=01"); CHECK(rejects(c)); }
    { std::string c = pl; c.replace(c.find("BED_LEVEL=1"), 11, "BED_LEVEL=+"); CHECK(rejects(c)); }
    CHECK(rejects(pl + " BED_LEVEL=1"));                                                          // duplicate key
    { std::string c = pl; c.replace(c.find("MAP_EXTRUDER=2\nSET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=2"), 14, "MAP_EXTRUDER=-1"); CHECK(rejects(c)); }
    { std::string c = pl; c.replace(c.find("CONFIG_EXTRUDER=0"), 17, "CONFIG_EXTRUDER=00"); CHECK(rejects(c)); }
    CHECK(rejects(sg.substr(0, sg.find("SET_PRINT_PREFERENCE")).substr(0, sg.find("SET_PRINT_PREFERENCE") - 1))); // singular shape without preferences, no newline
    CHECK(rejects(std::string(5000, 'A')));
    CHECK(rejects(pl + std::string(snapmaker_preprint_max_gcode_bytes, ' ')));
    CHECK(rejects("SET_PRINT_PREFERENCES BED_LEVEL=1\n" + pl)); // preference before map
    CHECK(rejects(pl + "\nSET_PRINT_PREFERENCE KEY=bed_level VALUE=1")); // plural + singular mixed
}

TEST_CASE("validator is not fooled by an extra-command prefix", "[SnapmakerPreprintGate]")
{
    CHECK(rejects(plural_golden() + "\nSET_PRINT_PREFERENCEX KEY=bed_level VALUE=1"));
    CHECK(rejects(singular_golden() + "\nSET_PRINT_PREFERENCES BED_LEVEL=1"));
}

TEST_CASE("gate: no open context refuses everything", "[SnapmakerPreprintGate]")
{
    SnapmakerPreprintGate g;
    CHECK(g.request_configure(plural_golden(), kPin, kIdle) == D::RefusedNoContext);
    CHECK(g.request_start("job.gcode", kPin, kIdle, kMetaOk) == D::RefusedNoContext);
}

TEST_CASE("gate: happy path and state transitions", "[SnapmakerPreprintGate]")
{
    SnapmakerPreprintGate g;
    g.open(open_params());
    CHECK(g.state() == SnapmakerPreprintState::Open);
    SnapmakerPreprintGcode parsed;
    CHECK(g.request_configure(singular_golden(), kPin, kIdle, &parsed) == D::Allowed);
    CHECK(parsed.ok);
    CHECK(g.state() == SnapmakerPreprintState::Configured);
    CHECK(g.request_start("job.gcode", kPin, kIdle, kMetaOk) == D::Allowed);
    CHECK(g.state() == SnapmakerPreprintState::Started);
    CHECK(g.request_start("job.gcode", kPin, kIdle, kMetaOk) == D::RefusedAlreadyStarted);
    CHECK(g.request_configure(plural_golden(), kPin, kIdle) == D::RefusedState);
}

TEST_CASE("gate: start before Configured is refused only when options are non-empty", "[SnapmakerPreprintGate]")
{
    SnapmakerPreprintGate g;
    g.open(open_params(true));
    CHECK(g.request_start("job.gcode", kPin, kIdle, kMetaOk) == D::RefusedNotConfigured);
    CHECK(g.state() == SnapmakerPreprintState::Open);
    g.open(open_params(false));
    CHECK(g.request_start("job.gcode", kPin, kIdle, kMetaOk) == D::Allowed);
}

TEST_CASE("gate: exactly one forced re-send", "[SnapmakerPreprintGate]")
{
    SnapmakerPreprintGate g;
    g.open(open_params());
    CHECK(g.request_configure(plural_golden(), kPin, kIdle) == D::Allowed);
    CHECK(g.request_configure(plural_golden(), kPin, kIdle) == D::Allowed); // the page's fallback
    CHECK(g.request_configure(plural_golden(), kPin, kIdle) == D::RefusedTooManyScripts);
    CHECK(g.request_start("job.gcode", kPin, kIdle, kMetaOk) == D::Allowed);
}

TEST_CASE("gate: an invalid or busy script does not consume a slot", "[SnapmakerPreprintGate]")
{
    SnapmakerPreprintGate g;
    g.open(open_params());
    CHECK(g.request_configure("G28", kPin, kIdle) == D::RefusedInvalid);
    CHECK(g.request_configure(plural_golden(), kPin, kBusy) == D::RefusedPrinterBusy);
    CHECK(g.request_configure(plural_golden(), kPin, nullptr) == D::RefusedPrinterBusy); // fail closed
    CHECK(g.state() == SnapmakerPreprintState::Open);
    CHECK(g.request_configure(plural_golden(), kPin, kIdle) == D::Allowed);
    CHECK(g.request_configure(plural_golden(), kPin, kIdle) == D::Allowed);
}

TEST_CASE("gate: refusals at start", "[SnapmakerPreprintGate]")
{
    SnapmakerPreprintGate g;
    g.open(open_params(false));
    CHECK(g.request_start("other.gcode", kPin, kIdle, kMetaOk) == D::RefusedFilename);
    CHECK(g.request_start("", kPin, kIdle, kMetaOk) == D::RefusedFilename);
    CHECK(g.request_start("job.gcode", SnapmakerPreprintPin{"192.168.1.51", "7125", "U1-123"}, kIdle, kMetaOk) == D::RefusedPrinterChanged);
    CHECK(g.request_start("job.gcode", SnapmakerPreprintPin{kPin.host, kPin.port, "other"}, kIdle, kMetaOk) == D::RefusedPrinterChanged);
    CHECK(g.request_start("job.gcode", kPin, kBusy, kMetaOk) == D::RefusedPrinterBusy); // printing / paused
    CHECK(g.request_start("job.gcode", kPin, nullptr, kMetaOk) == D::RefusedPrinterBusy);
    CHECK(g.request_start("job.gcode", kPin, kIdle, kMetaBad) == D::RefusedMetadata);
    CHECK(g.request_start("job.gcode", kPin, kIdle, nullptr) == D::RefusedMetadata);
    CHECK(g.state() == SnapmakerPreprintState::Open); // none of the refusals started it
    CHECK(g.request_start("job.gcode", kPin, kIdle, kMetaOk) == D::Allowed);
}

TEST_CASE("gate: configure refuses a changed printer", "[SnapmakerPreprintGate]")
{
    SnapmakerPreprintGate g;
    g.open(open_params());
    CHECK(g.request_configure(plural_golden(), SnapmakerPreprintPin{"10.0.0.9", "7125", "U1-123"}, kIdle) == D::RefusedPrinterChanged);
}

TEST_CASE("gate: closed from any state refuses everything", "[SnapmakerPreprintGate]")
{
    for (int stage = 0; stage < 3; ++stage) {
        SnapmakerPreprintGate g;
        g.open(open_params());
        if (stage >= 1) g.request_configure(plural_golden(), kPin, kIdle);
        if (stage >= 2) g.request_start("job.gcode", kPin, kIdle, kMetaOk);
        g.close();
        CHECK(g.state() == SnapmakerPreprintState::Closed);
        CHECK(g.request_configure(plural_golden(), kPin, kIdle) == D::RefusedClosed);
        CHECK(g.request_start("job.gcode", kPin, kIdle, kMetaOk) == D::RefusedClosed);
    }
    SnapmakerPreprintGate idle_gate; // close on a never-opened gate stays Idle
    idle_gate.close();
    CHECK(idle_gate.state() == SnapmakerPreprintState::Idle);
}

TEST_CASE("gate: a close during predicate evaluation wins", "[SnapmakerPreprintGate]")
{
    SnapmakerPreprintGate g;
    g.open(open_params(false));
    const auto closing_idle = [&g] { g.close(); return true; };
    CHECK(g.request_start("job.gcode", kPin, closing_idle, kMetaOk) == D::RefusedClosed);
    CHECK(g.state() == SnapmakerPreprintState::Closed);
}

TEST_CASE("gate: concurrent starts allow exactly one", "[SnapmakerPreprintGate]")
{
    for (int round = 0; round < 50; ++round) {
        SnapmakerPreprintGate g;
        g.open(open_params(false));
        std::atomic<int> allowed{0};
        std::vector<std::thread> ts;
        for (int i = 0; i < 8; ++i)
            ts.emplace_back([&] {
                if (g.request_start("job.gcode", kPin, kIdle, kMetaOk) == D::Allowed) ++allowed;
            });
        for (auto& t : ts) t.join();
        CHECK(allowed == 1);
    }
}

TEST_CASE("gate: concurrent configures never exceed the cap, and set/start/close race safely", "[SnapmakerPreprintGate]")
{
    for (int round = 0; round < 50; ++round) {
        SnapmakerPreprintGate g;
        g.open(open_params());
        std::atomic<int> allowed{0}, started{0};
        std::vector<std::thread> ts;
        for (int i = 0; i < 6; ++i)
            ts.emplace_back([&] {
                if (g.request_configure(plural_golden(), kPin, kIdle) == D::Allowed) ++allowed;
            });
        for (int i = 0; i < 4; ++i)
            ts.emplace_back([&] {
                if (g.request_start("job.gcode", kPin, kIdle, kMetaOk) == D::Allowed) ++started;
            });
        ts.emplace_back([&] { g.close(); });
        for (auto& t : ts) t.join();
        CHECK(allowed <= snapmaker_preprint_max_configures);
        CHECK(started <= 1);
        CHECK(g.state() == SnapmakerPreprintState::Closed);
    }
}
