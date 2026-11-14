#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <boost/filesystem.hpp>
#include <fstream>
#include <sstream>

#include "slic3r/GUI/PreprintDialogController.hpp"
#include "slic3r/Utils/NetworkAgentFactory.hpp"

// S5. The controller is driven with fakes only: a manual-clock scheduler, a view that records calls, a fake printer interface. No window,
// webview, socket or printer exists here. These tests show the lifecycle ORDER and the refusals; they cannot show that a real WKWebView
// installs its script handler, that the bundled page loads and talks to the bridge, or that nothing hangs (#84): only a live GUI run and
// then a live send on an idle printer (with Charles's OK) can.

using namespace Slic3r;
using namespace Slic3r::GUI;
namespace fs = boost::filesystem;
using ms = std::chrono::milliseconds;

namespace {

TEST_CASE("route predicate: Snapmaker agent connected -> pre-print; everything else -> legacy or Bambu", "[SendDialog][Routing]")
{
    NetworkAgentFactory::register_all_agents();
    // Capability, per agent (registered by register_all_agents; the Moonraker, Orca, Bambu and Creality agents must not claim it).
    CHECK(NetworkAgentFactory::printer_agent_supports_preprint_page(SNAPMAKER_PRINTER_AGENT_ID));
    CHECK_FALSE(NetworkAgentFactory::printer_agent_supports_preprint_page(MOONRAKER_PRINTER_AGENT_ID));
    CHECK_FALSE(NetworkAgentFactory::printer_agent_supports_preprint_page(BBL_PRINTER_AGENT_ID));
    CHECK_FALSE(NetworkAgentFactory::printer_agent_supports_preprint_page(ORCA_PRINTER_AGENT_ID));
    CHECK_FALSE(NetworkAgentFactory::printer_agent_supports_preprint_page("crealityprint"));
    CHECK_FALSE(NetworkAgentFactory::printer_agent_supports_preprint_page("qidi"));
    CHECK_FALSE(NetworkAgentFactory::printer_agent_supports_preprint_page("octoprint"));
    CHECK_FALSE(NetworkAgentFactory::printer_agent_supports_preprint_page(""));
    CHECK_FALSE(NetworkAgentFactory::printer_agent_supports_preprint_page("unknown-agent"));

    // The pure predicate.
    CHECK(NetworkAgentFactory::use_preprint_send_flow(true, true));
    CHECK_FALSE(NetworkAgentFactory::use_preprint_send_flow(true, false)); // disconnected -> legacy
    CHECK_FALSE(NetworkAgentFactory::use_preprint_send_flow(false, true));
    CHECK_FALSE(NetworkAgentFactory::use_preprint_send_flow(false, false));
    CHECK(NetworkAgentFactory::use_preprint_send_flow(std::string(SNAPMAKER_PRINTER_AGENT_ID), true));
    CHECK_FALSE(NetworkAgentFactory::use_preprint_send_flow(std::string(SNAPMAKER_PRINTER_AGENT_ID), false));
    CHECK_FALSE(NetworkAgentFactory::use_preprint_send_flow(std::string(MOONRAKER_PRINTER_AGENT_ID), true)); // generic Moonraker -> legacy
    CHECK_FALSE(NetworkAgentFactory::use_preprint_send_flow(std::string(BBL_PRINTER_AGENT_ID), true));

    // Bambu is decided first and is unchanged: a Bambu agent never reaches the pre-print predicate as true.
    CHECK(NetworkAgentFactory::use_bambu_send_dialog(true, true, BBL_PRINTER_AGENT_ID));
    CHECK(NetworkAgentFactory::use_bambu_send_dialog(false, true, BBL_PRINTER_AGENT_ID));
    CHECK_FALSE(NetworkAgentFactory::use_bambu_send_dialog(false, true, SNAPMAKER_PRINTER_AGENT_ID));
}

TEST_CASE("pre-print file names are sanitised deterministically onto the gate's allowlist", "[SendDialog][Preprint]")
{
    struct Row { const char* in; const char* out; bool changed; };
    const Row rows[] = {
        {"plate_1.gcode", "plate_1.gcode", false},
        {"my part.gcode", "my part.gcode", false},                            // spaces are allowed
        {"dir/sub dir/a-b_c.1.gcode", "dir/sub dir/a-b_c.1.gcode", false},
        {"Bob's part.gcode", "Bob_s part{H}.gcode", true},                      // apostrophe
        {"caf\xC3\xA9 \xE2\x82\xAC.gcode", "caf_ _{H}.gcode", true},            // unicode: one '_' per code point
        {"\xF0\x9F\x98\x80.gcode", "_{H}.gcode", true},                          // 4-byte code point -> one '_'
        {"name", "name.gcode", true},                                         // suffix added
        {"name.GCODE", "name.gcode", true},                                   // suffix normalised, not doubled
        {"a.gcode.gcode", "a.gcode.gcode", false},                            // only one suffix is stripped and re-added
        {"../../etc/passwd", "etc/passwd{H}.gcode", true},                        // traversal segments dropped
        {"/abs/path.gcode", "abs/path.gcode", true},
        {"a\\b.gcode", "a/b.gcode", true},                                    // backslash is a separator
        {"a//b/./c.gcode", "a/b/c{H}.gcode", true},
        {"  spaced  .gcode", "spaced.gcode", true},
        {"q\"uote;semi#hash=eq.gcode", "q_uote_semi_hash_eq{H}.gcode", true},
        {"tab\there\nnl.gcode", "tab_here_nl{H}.gcode", true},
        {"", "print{H}.gcode", true},
        {".gcode", "print{H}.gcode", true},
        {"///", "print{H}.gcode", true},
        {"dir/.gcode", "dir.gcode", true},
    };
    for (const auto& r : rows) {
        INFO("in: " << r.in);
        const auto n = snapmaker_preprint_sanitise_filename(r.in);
        std::string expect = r.out;
        const auto  at     = expect.find("{H}");
        if (at != std::string::npos) expect.replace(at, 3, "_" + snapmaker_preprint_name_hash8(r.in));
        CHECK(n.name == expect);
        CHECK(n.lossy == (at != std::string::npos));
        CHECK(n.changed == r.changed);
        CHECK(snapmaker_preprint_filename_ok(n.name));
        CHECK(snapmaker_preprint_sanitise_filename(n.name).name == n.name); // idempotent
    }
    // Collisions: different originals that sanitise to the same text get different names (hash of the original).
    CHECK(snapmaker_preprint_sanitise_filename("a b'.gcode").name != snapmaker_preprint_sanitise_filename("a b\".gcode").name);
    CHECK(snapmaker_preprint_sanitise_filename("\xC3\xA9.gcode").name != snapmaker_preprint_sanitise_filename("\xC3\xA8.gcode").name);
    CHECK(snapmaker_preprint_sanitise_filename("x'.gcode").name == snapmaker_preprint_sanitise_filename("x'.gcode").name); // deterministic
    // Length cap: a very long name still passes and keeps the suffix.
    const auto longn = snapmaker_preprint_sanitise_filename(std::string(2000, 'x') + ".gcode");
    CHECK(longn.name.size() <= 512);
    CHECK(snapmaker_preprint_filename_ok(longn.name));
    // The capability display name is a strict subset of what the store accepts.
    for (const char* n : {"my part.gcode", "dir/sub dir/a b.gcode", "Bob_s part.gcode", "x.gcode"})
        CHECK(PreprintCapabilityStore::valid_display_name(snapmaker_preprint_capability_display_name(n)));
}

// ---- fakes ----

struct Log
{
    std::vector<std::string> v;
    void add(const std::string& s) { v.push_back(s); }
    int index(const std::string& s) const { auto it = std::find(v.begin(), v.end(), s); return it == v.end() ? -1 : int(it - v.begin()); }
    int count(const std::string& s) const { return int(std::count(v.begin(), v.end(), s)); }
};

struct FakeScheduler : PreprintScheduler
{
    std::chrono::steady_clock::time_point t = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
    struct Task { std::chrono::steady_clock::time_point due; std::function<void()> fn; };
    std::vector<Task> tasks;
    std::chrono::steady_clock::time_point now() override { return t; }
    void call_after(std::function<void()> fn) override { tasks.push_back({t, std::move(fn)}); }
    void call_later(ms d, std::function<void()> fn) override { tasks.push_back({t + d, std::move(fn)}); }
    bool hold_bg = false; // keep the UI continuation of a background job until release_held()
    std::vector<std::function<void()>> held;
    void run_background(std::function<void()> work, std::function<void()> on_ui) override
    {
        work();
        if (hold_bg) held.push_back(std::move(on_ui)); else on_ui();
    }
    void release_held() { auto h = std::move(held); held.clear(); for (auto& f : h) f(); }
    // Runs every task due up to now+d in due order, moving the clock; tasks they add are honoured.
    void advance(ms d)
    {
        const auto target = t + d;
        for (;;) {
            auto it = std::min_element(tasks.begin(), tasks.end(), [](const Task& a, const Task& b) { return a.due < b.due; });
            if (it == tasks.end() || it->due > target) break;
            t = std::max(t, it->due);
            auto fn = std::move(it->fn);
            tasks.erase(it);
            fn();
        }
        t = target;
    }
    void pump() { advance(ms(0)); }
};

struct ViewState
{
    bool handler = false;
    std::uint64_t epoch = 0;
    bool send_only = false;
    bool destroyed = false;
    std::string title;
};

struct FakeView : PreprintView
{
    std::shared_ptr<ViewState> s;
    Log& log;
    FakeView(std::shared_ptr<ViewState> st, Log& l) : s(std::move(st)), log(l) { log.add("view:create"); }
    ~FakeView() override { log.add("view:dtor"); }
    bool script_handler_added() override { return s->handler; }
    void show() override { log.add("view:show"); }
    void raise() override { log.add("view:raise"); }
    void set_title(const std::string& t) override { s->title = t; }
    void attach_bridge(std::uint64_t e) override { s->epoch = e; log.add("view:attach_bridge"); }
    void load_page(bool so) override { s->send_only = so; log.add("view:load_page"); }
    void destroy() override { s->destroyed = true; log.add("view:destroy"); }
};

struct FakePrinter : SnapmakerPreprintPrinter
{
    SnapmakerPreprintPin pin{"u1.local", "7125", "SN123"};
    SnapmakerPreprintPin connected_pin() override { return pin; }
    std::optional<std::string> print_state() override { return std::string("standby"); }
    std::optional<std::uint64_t> remote_file_size(const std::string&) override { return std::nullopt; }
    Known command_known(const std::string&) override { return Known::Yes; }
    bool send_gcode(const std::string&) override { return true; }
    bool start_print(const std::string&) override { return true; }
};

struct Fixture
{
    Log log;
    std::shared_ptr<FakeScheduler> sched = std::make_shared<FakeScheduler>();
    std::shared_ptr<PreprintCapabilityStore> store = std::make_shared<PreprintCapabilityStore>();
    std::shared_ptr<SnapmakerPreprintContext> ctx = std::make_shared<SnapmakerPreprintContext>();
    std::shared_ptr<FakePrinter> printer = std::make_shared<FakePrinter>();
    std::shared_ptr<SnapmakerPreprintSession> session = std::make_shared<SnapmakerPreprintSession>(ctx, printer);
    std::shared_ptr<PreprintDialogController> c;
    std::vector<std::shared_ptr<ViewState>> views;
    std::vector<std::string> errors, infos;
    int switches = 0;
    fs::path dir = fs::temp_directory_path() / fs::unique_path("preprint-ctl-%%%%-%%%%");
    fs::path gcode;

    Fixture()
    {
        fs::create_directories(dir);
        gcode = dir / "plate.gcode";
        std::ofstream(gcode.string(), std::ios::binary) << "G28\nG1 X1\n";
        PreprintDialogController::Deps d;
        d.scheduler = sched;
        d.store     = store;
        d.context   = ctx;
        d.session   = session;
        d.bind_printer = [this] { log.add("bind"); return printer->pin; };
        d.make_view = [this](std::uint64_t) -> std::unique_ptr<PreprintView> {
            views.push_back(std::make_shared<ViewState>());
            return std::make_unique<FakeView>(views.back(), log);
        };
        d.report_error = [this](const std::string& m) { errors.push_back(m); log.add("error"); };
        d.notify_info = [this](const std::string& m) { infos.push_back(m); };
        d.switch_to_device_tab = [this] { ++switches; log.add("switch_tab"); };
        c = PreprintDialogController::create(std::move(d));
    }
    ~Fixture() { boost::system::error_code ec; fs::remove_all(dir, ec); }

    PreprintRequest request(bool start = true, bool sw = true) const
    {
        PreprintRequest r;
        r.gcode_path           = gcode;
        r.requested_name       = "Bob's part.gcode";
        r.start_requested      = start;
        r.switch_to_device_tab = sw;
        r.plate.filament_colour = {"#FF0000", "#00FF00"};
        r.plate.filament_type   = {"PLA", "PETG"};
        r.plate.nozzle_diameter = {0.4, 0.4, 0.4, 0.4};
        r.plate.machine_model   = "Snapmaker U1";
        r.plate_filaments       = {0, 1};
        r.extruder_count        = 4;
        return r;
    }
    // Opens and drives it to Live (handler installed immediately).
    void open_live(bool start = true, bool sw = true)
    {
        REQUIRE(c->open(request(start, sw)) == PreprintDialogController::Result::Opened);
        views.back()->handler = true;
        sched->advance(kPreprintPollInterval * 2);
        REQUIRE(c->state() == PreprintDialogController::State::Live);
    }
};

TEST_CASE("controller waits for the script handler before loading the page, with no nested loop", "[SendDialog][Preprint]")
{
    Fixture f;
    REQUIRE(f.c->open(f.request()) == PreprintDialogController::Result::Opened);
    // Shown non-modally on about:blank (the view's own business); nothing loaded, nothing bound, nothing opened yet.
    CHECK(f.c->state() == PreprintDialogController::State::WaitingForHandler);
    CHECK(f.log.index("view:show") >= 0);
    CHECK(f.log.index("view:load_page") < 0);
    CHECK(f.log.index("bind") < 0);
    CHECK(f.session->epoch() == 0);
    f.sched->advance(ms(1000)); // handler still missing: keeps waiting
    CHECK(f.c->state() == PreprintDialogController::State::WaitingForHandler);
    CHECK(f.log.index("view:load_page") < 0);
    CHECK(f.errors.empty());
    f.views.back()->handler = true;
    f.sched->advance(kPreprintPollInterval * 2);
    CHECK(f.c->state() == PreprintDialogController::State::Live);
    CHECK(f.session->epoch() != 0);
    CHECK(f.views.back()->epoch == f.session->epoch());
    CHECK(f.log.count("view:load_page") == 1);
    // Order: snapshot registered (before the window), show, handler, bind, bridge attached, only then the page.
    CHECK(f.log.index("view:create") < f.log.index("view:show"));
    CHECK(f.log.index("view:show") < f.log.index("bind"));
    CHECK(f.log.index("bind") < f.log.index("view:attach_bridge"));
    CHECK(f.log.index("view:attach_bridge") < f.log.index("view:load_page"));
    // The title shows the sanitised name, and the user is told it changed.
    CHECK(f.c->remote_filename() == snapmaker_preprint_sanitise_filename("Bob's part.gcode").name);
    CHECK(f.views.back()->title == f.c->remote_filename() + " -> u1.local"); // the target printer is in the title
    CHECK(f.c->name_changed());
    REQUIRE(f.infos.size() == 1); // the user is told about the rename
    CHECK(f.infos[0].find(f.c->remote_filename()) != std::string::npos);
}

TEST_CASE("snapshot is registered before the session opens and is revoked on close", "[SendDialog][Preprint]")
{
    Fixture f;
    REQUIRE(f.c->open(f.request()) == PreprintDialogController::Result::Opened);
    // The snapshot exists as soon as open() returns, before any window event: a later re-slice cannot change it.
    const std::string cap = f.c->capability_path();
    REQUIRE_FALSE(cap.empty());
    CHECK(f.store->has_live_entries());
    CHECK(f.session->epoch() == 0);
    // Overwrite the source after open: the snapshot is what is served.
    std::ofstream(f.gcode.string(), std::ios::binary | std::ios::trunc) << "DIFFERENT";
    f.views.back()->handler = true;
    f.sched->advance(kPreprintPollInterval * 2);
    REQUIRE(f.c->state() == PreprintDialogController::State::Live);
    CHECK(f.session->state() == SnapmakerPreprintState::Open);
    auto serve = f.store->begin_serve(cap);
    REQUIRE(serve.status == PreprintCapabilityStore::Status::Ok);
    CHECK(serve.size == 10); // "G28\nG1 X1\n"
    f.store->finish_serve(cap, false);
    f.c->on_user_closed(f.c->open_id());
    CHECK(f.store->begin_serve(cap).status == PreprintCapabilityStore::Status::Gone);
    CHECK_FALSE(f.store->has_live_entries());
    CHECK(f.session->state() == SnapmakerPreprintState::Closed);
}

TEST_CASE("watchdog closes the dialog when the handler never installs", "[SendDialog][Preprint]")
{
    Fixture f;
    REQUIRE(f.c->open(f.request()) == PreprintDialogController::Result::Opened);
    const std::string cap = f.c->capability_path();
    f.sched->advance(kPreprintHandlerWatchdog - ms(100));
    CHECK(f.c->state() == PreprintDialogController::State::WaitingForHandler);
    CHECK(f.errors.empty());
    f.sched->advance(ms(300));
    CHECK(f.c->state() == PreprintDialogController::State::Closed);
    REQUIRE(f.errors.size() == 1);
    CHECK(f.views.back()->destroyed);
    CHECK(f.log.index("view:load_page") < 0);
    CHECK(f.log.index("bind") < 0);
    CHECK(f.log.count("switch_tab") == 0);
    CHECK(f.store->begin_serve(cap).status == PreprintCapabilityStore::Status::Gone);
    // A new open afterwards works (not wedged), and a stale poll from the first one does nothing.
    REQUIRE(f.c->open(f.request()) == PreprintDialogController::Result::Opened);
    f.views.back()->handler = true;
    f.sched->advance(kPreprintPollInterval * 2);
    CHECK(f.c->state() == PreprintDialogController::State::Live);
}

TEST_CASE("a second Print click raises the live dialog instead of opening another", "[SendDialog][Preprint]")
{
    Fixture f;
    REQUIRE(f.c->open(f.request()) == PreprintDialogController::Result::Opened);
    const std::string cap = f.c->capability_path();
    CHECK(f.c->open(f.request()) == PreprintDialogController::Result::AlreadyOpen); // while waiting
    f.views.back()->handler = true;
    f.sched->advance(kPreprintPollInterval * 2);
    REQUIRE(f.c->state() == PreprintDialogController::State::Live);
    CHECK(f.c->open(f.request()) == PreprintDialogController::Result::AlreadyOpen); // while live
    CHECK(f.log.count("view:raise") == 2);
    CHECK(f.views.size() == 1);
    CHECK(f.log.count("view:create") == 1);
    CHECK(f.c->capability_path() == cap);
    CHECK(f.c->raise_if_live());
    CHECK(f.log.count("view:raise") == 3);
    f.c->on_user_closed(f.c->open_id());
    CHECK_FALSE(f.c->raise_if_live());
    CHECK(f.c->open(f.request()) == PreprintDialogController::Result::Opened); // closed: a new one is allowed
    CHECK(f.views.size() == 2);
}

TEST_CASE("a re-slice while open invalidates the dialog with a 'reopen' message", "[SendDialog][Preprint]")
{
    Fixture f;
    f.open_live();
    const std::string cap = f.c->capability_path();
    f.c->invalidate();
    CHECK(f.c->state() == PreprintDialogController::State::Closed);
    REQUIRE(f.errors.size() == 1);
    CHECK(f.errors[0].find("changed") != std::string::npos);
    CHECK(f.errors[0].find("eopen") != std::string::npos);
    CHECK(f.views.back()->destroyed);
    CHECK(f.session->state() == SnapmakerPreprintState::Closed);
    CHECK(f.store->begin_serve(cap).status == PreprintCapabilityStore::Status::Gone);
    f.sched->advance(ms(1000));
    CHECK(f.switches == 0); // invalidation never switches tabs
    f.c->invalidate(); // not live: a no-op
    CHECK(f.errors.size() == 1);
}

TEST_CASE("the Device-tab switch happens only after the dialog is destroyed, in a later turn", "[SendDialog][Preprint]")
{
    Fixture f;
    f.open_live(true, true);
    f.c->on_page_done(f.c->open_id(), true);
    CHECK(f.log.count("view:destroy") == 1);
    CHECK(f.switches == 0); // not from inside the callback
    f.sched->pump();
    CHECK(f.switches == 1);
    CHECK(f.log.index("view:destroy") < f.log.index("switch_tab"));
    f.sched->advance(ms(1000));
    CHECK(f.switches == 1);
    f.c->on_page_done(f.c->open_id(), true); // a late second completion changes nothing
    f.sched->pump();
    CHECK(f.switches == 1);
}

TEST_CASE("no tab switch when not asked, when the start did not happen, or when the user closed the window", "[SendDialog][Preprint]")
{
    {   Fixture f; f.open_live(true, false); f.c->on_page_done(f.c->open_id(), true); f.sched->advance(ms(100)); CHECK(f.switches == 0); }
    {   Fixture f; f.open_live(true, true);  f.c->on_page_done(f.c->open_id(), false); f.sched->advance(ms(100)); CHECK(f.switches == 0); } // start wanted, not started
    {   Fixture f; f.open_live(true, true);  f.c->on_user_closed(f.c->open_id()); f.sched->advance(ms(100)); CHECK(f.switches == 0); }
    {   Fixture f; f.open_live(false, true); f.c->on_page_done(f.c->open_id(), false); f.sched->advance(ms(100)); CHECK(f.switches == 1); } // upload only: done = success
}

TEST_CASE("upload-only configures the gate so no start is ever allowed", "[SendDialog][Preprint]")
{
    Fixture up;
    up.open_live(/*start=*/false);
    CHECK(up.views.back()->send_only); // the upload-only page variant
    const auto r = up.session->start(up.views.back()->epoch, nlohmann::json{{"filename", up.c->remote_filename()}});
    CHECK_FALSE(r.ok);
    CHECK(r.message.find("upload only") != std::string::npos);

    Fixture go;
    go.open_live(/*start=*/true);
    CHECK_FALSE(go.views.back()->send_only);
    const auto r2 = go.session->start(go.views.back()->epoch, nlohmann::json{{"filename", go.c->remote_filename()}});
    CHECK_FALSE(r2.ok);                                                  // not configured yet...
    CHECK(r2.message.find("upload only") == std::string::npos);          // ...but for that reason, not the upload-only one
    CHECK(r2.message.find("preferences") != std::string::npos);
}

TEST_CASE("upload-only also refuses the configure script (read-only toward the printer)", "[SendDialog][Preprint]")
{
    Fixture f;
    f.open_live(/*start=*/false);
    const auto r = f.session->configure(f.views.back()->epoch,
                                        nlohmann::json{{"script", build_snapmaker_plural_preference_script({0, 1}, {})}});
    CHECK_FALSE(r.ok);
    CHECK(r.message.find("upload only") != std::string::npos);
}

TEST_CASE("close or invalidate while waiting for the handler leaves nothing behind", "[SendDialog][Preprint]")
{
    Fixture f;
    REQUIRE(f.c->open(f.request()) == PreprintDialogController::Result::Opened);
    const std::string cap = f.c->capability_path();
    f.c->on_user_closed(f.c->open_id());
    CHECK(f.c->state() == PreprintDialogController::State::Closed);
    CHECK(f.views.back()->destroyed);
    CHECK(f.store->begin_serve(cap).status == PreprintCapabilityStore::Status::Gone);
    f.views.back()->handler = true; // a stale poll must not bind or open anything
    f.sched->advance(kPreprintHandlerWatchdog * 2);
    CHECK(f.log.index("bind") < 0);
    CHECK(f.session->epoch() == 0);
    CHECK(f.errors.empty());
}

TEST_CASE("a bind that finishes after the close does nothing", "[SendDialog][Preprint]")
{
    Fixture f;
    REQUIRE(f.c->open(f.request()) == PreprintDialogController::Result::Opened);
    f.views.back()->handler = true;
    f.sched->hold_bg = true;
    f.sched->advance(kPreprintPollInterval * 2);
    REQUIRE(f.c->state() == PreprintDialogController::State::Binding);
    f.c->invalidate();
    f.sched->hold_bg = false;
    f.sched->release_held();
    CHECK(f.c->state() == PreprintDialogController::State::Closed);
    CHECK(f.session->epoch() == 0);
    CHECK(f.log.index("view:attach_bridge") < 0);
    CHECK(f.log.index("view:load_page") < 0);
}

TEST_CASE("an invalidate during the snapshot copy revokes the late snapshot", "[SendDialog][Preprint]")
{
    Fixture f;
    f.sched->hold_bg = true;
    REQUIRE(f.c->open(f.request()) == PreprintDialogController::Result::Opened);
    CHECK(f.c->state() == PreprintDialogController::State::Snapshotting);
    CHECK(f.c->raise_if_live()); // already counts as live: a second click opens nothing
    CHECK(f.c->open(f.request()) == PreprintDialogController::Result::AlreadyOpen);
    f.c->invalidate();
    f.sched->hold_bg = false;
    f.sched->release_held();
    CHECK(f.log.index("view:create") < 0);
    CHECK_FALSE(f.store->has_live_entries());
    CHECK(f.c->state() == PreprintDialogController::State::Closed);
}

TEST_CASE("a late callback of an earlier open cannot close a later one", "[SendDialog][Preprint]")
{
    Fixture f;
    f.open_live();
    const auto first = f.c->open_id();
    f.c->on_user_closed(first);
    f.open_live();
    REQUIRE(f.c->open_id() != first);
    f.c->on_page_done(first, true);
    f.c->on_user_closed(first);
    f.c->on_page_error(first, "x");
    f.sched->advance(ms(100));
    CHECK(f.c->state() == PreprintDialogController::State::Live);
    CHECK(f.switches == 0);
    f.c->on_page_done(f.c->open_id(), true);
    f.sched->pump();
    CHECK(f.switches == 1);
}

TEST_CASE("destroying the controller destroys a live window exactly once", "[SendDialog][Preprint]")
{
    Fixture f;
    f.open_live();
    const std::string cap = f.c->capability_path();
    f.c.reset();
    CHECK(f.log.count("view:destroy") == 1);
    CHECK(f.log.index("view:destroy") < f.log.index("view:dtor"));
    CHECK(f.views.back()->destroyed);
    CHECK(f.store->begin_serve(cap).status == PreprintCapabilityStore::Status::Gone);
    f.sched->advance(std::chrono::minutes(15)); // pending timers hold only weak references
    CHECK(f.switches == 0);
}

TEST_CASE("page-load watchdog, page errors and the capability TTL close with a message", "[SendDialog][Preprint]")
{
    {   // the page never says anything
        Fixture f; f.open_live();
        f.sched->advance(kPreprintPageLoadWatchdog - ms(500));
        CHECK(f.c->state() == PreprintDialogController::State::Live);
        f.sched->advance(ms(1000));
        CHECK(f.c->state() == PreprintDialogController::State::Closed);
        CHECK(f.errors.size() == 1);
    }
    {   // the page spoke: the watchdog stays quiet
        Fixture f; f.open_live();
        f.c->on_page_message(f.c->open_id());
        f.sched->advance(kPreprintPageLoadWatchdog * 2);
        CHECK(f.c->state() == PreprintDialogController::State::Live);
    }
    {   // a webview error
        Fixture f; f.open_live();
        f.c->on_page_error(f.c->open_id(), "net::ERR");
        CHECK(f.c->state() == PreprintDialogController::State::Closed);
        REQUIRE(f.errors.size() == 1);
        CHECK(f.errors[0].find("net::ERR") != std::string::npos);
    }
    {   // TTL: a warning a minute before, then a clear close
        Fixture f; f.open_live();
        f.c->on_page_message(f.c->open_id());
        const auto ttl = std::chrono::duration_cast<ms>(PreprintCapabilityStore::capability_ttl);
        f.sched->advance(ttl - kPreprintTtlWarning - ms(1000));
        CHECK(f.infos.size() == 1); // only the rename notice so far
        f.sched->advance(ms(2000));
        CHECK(f.infos.size() == 2);
        CHECK(f.c->state() == PreprintDialogController::State::Live);
        f.sched->advance(kPreprintTtlWarning);
        CHECK(f.c->state() == PreprintDialogController::State::Closed);
        REQUIRE(f.errors.size() == 1);
        CHECK(f.errors[0].find("expired") != std::string::npos);
    }
}

TEST_CASE("send route: Bambu first, then pre-print, else legacy", "[SendDialog][Routing]")
{
    NetworkAgentFactory::register_all_agents();
    using R = NetworkAgentFactory::SendRoute;
    const auto route = [](bool bbl, bool agents, const char* id, bool conn) { return NetworkAgentFactory::choose_send_route(bbl, agents, id, conn); };
    CHECK(route(true, false, BBL_PRINTER_AGENT_ID, true) == R::Bambu);
    CHECK(route(true, true, SNAPMAKER_PRINTER_AGENT_ID, true) == R::Bambu); // Bambu network wins even with a Snapmaker agent id
    CHECK(route(false, true, BBL_PRINTER_AGENT_ID, false) == R::Bambu);
    CHECK(route(false, true, SNAPMAKER_PRINTER_AGENT_ID, true) == R::Preprint);
    CHECK(route(false, false, SNAPMAKER_PRINTER_AGENT_ID, true) == R::Preprint);
    CHECK(route(false, true, SNAPMAKER_PRINTER_AGENT_ID, false) == R::Legacy);
    CHECK(route(false, true, MOONRAKER_PRINTER_AGENT_ID, true) == R::Legacy);
    CHECK(route(false, true, "crealityprint", true) == R::Legacy);
    CHECK(route(false, true, "", true) == R::Bambu); // empty id counts as Bambu, as before
}

TEST_CASE("a failed bind or an unusable pin refuses and cleans up", "[SendDialog][Preprint]")
{
    Fixture f;
    f.printer->pin = SnapmakerPreprintPin{}; // nothing connected / serial unreadable
    REQUIRE(f.c->open(f.request()) == PreprintDialogController::Result::Opened);
    const std::string cap = f.c->capability_path();
    f.views.back()->handler = true;
    f.sched->advance(kPreprintPollInterval * 2);
    CHECK(f.c->state() == PreprintDialogController::State::Closed);
    REQUIRE(f.errors.size() == 1);
    CHECK(f.log.index("view:load_page") < 0);
    CHECK(f.views.back()->destroyed);
    CHECK(f.store->begin_serve(cap).status == PreprintCapabilityStore::Status::Gone);
}

TEST_CASE("a missing plate G-code is refused before any window exists", "[SendDialog][Preprint]")
{
    Fixture f;
    auto r = f.request();
    r.gcode_path = f.dir / "nope.gcode";
    f.c->open(std::move(r)); // the snapshot runs in the background; the refusal arrives as a notification
    CHECK(f.log.index("view:create") < 0);
    CHECK(f.errors.size() == 1);
    CHECK(f.c->state() != PreprintDialogController::State::Live);
}

std::string read_all(const fs::path& p)
{
    std::ifstream in(p.string(), std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

TEST_CASE("the pre-print path uses no modal API", "[SendDialog][Preprint]")
{
    // Source scan, not behaviour: the new dialog, the controller and Plater::send_preprint must stay free of modal loops (#84). The ONE
    // deliberate modal call is PrintHostSendDialog in Plater.cpp's marked helper (no webview in it, destroyed before the page's webview exists).
    const char* banned[] = {"ShowModal", "EndModal", "ErrorDialog", "show_error", "wxMessageBox", "RichMessageDialog", "MessageDialog", "Yield("};
    const fs::path src = fs::path(__FILE__).parent_path().parent_path().parent_path() / "src" / "slic3r" / "GUI";
    auto scan = [&](const std::string& label, const std::string& text) {
        for (const char* b : banned) {
            INFO(label << " contains " << b);
            CHECK(text.find(b) == std::string::npos);
        }
    };
    for (const char* name : {"PreprintDialogController.hpp", "PreprintDialog.hpp", "PreprintDialog.cpp"}) {
        REQUIRE(fs::exists(src / name));
        scan(name, read_all(src / name));
    }
    const std::string plater = read_all(src / "Plater.cpp");
    const auto section = [&](const char* begin, const char* end) {
        const auto a = plater.find(begin);
        const auto b = plater.find(end);
        REQUIRE(a != std::string::npos);
        REQUIRE(b != std::string::npos);
        REQUIRE(b > a);
        return plater.substr(a, b - a);
    };
    const std::string body = section("// BEGIN send_preprint", "// END send_preprint");
    REQUIRE(body.size() > 500); // the markers really bracket the function
    REQUIRE(body.find("controller->open(") != std::string::npos);
    scan("Plater::send_preprint", body);
    const std::string helper = section("// BEGIN preprint send dialog helper", "// END preprint send dialog helper");
    CHECK(helper.find("ShowModal") != std::string::npos);
    std::size_t modal_calls = 0;
    for (auto at = helper.find("ShowModal("); at != std::string::npos; at = helper.find("ShowModal(", at + 1)) ++modal_calls;
    CHECK(modal_calls == 1);
    CHECK(helper.find("wxWebView") == std::string::npos);
}

} // namespace
