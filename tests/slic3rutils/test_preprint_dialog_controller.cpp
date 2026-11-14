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
        {"Bob's part.gcode", "Bob_s part.gcode", true},                      // apostrophe
        {"caf\xC3\xA9 \xE2\x82\xAC.gcode", "caf_ _.gcode", true},            // unicode: one '_' per code point
        {"\xF0\x9F\x98\x80.gcode", "_.gcode", true},                          // 4-byte code point -> one '_'
        {"name", "name.gcode", true},                                         // suffix added
        {"name.GCODE", "name.gcode", true},                                   // suffix normalised, not doubled
        {"a.gcode.gcode", "a.gcode.gcode", false},                            // only one suffix is stripped and re-added
        {"../../etc/passwd", "etc/passwd.gcode", true},                        // traversal segments dropped
        {"/abs/path.gcode", "abs/path.gcode", true},
        {"a\\b.gcode", "a/b.gcode", true},                                    // backslash is a separator
        {"a//b/./c.gcode", "a/b/c.gcode", true},
        {"  spaced  .gcode", "spaced.gcode", true},
        {"q\"uote;semi#hash=eq.gcode", "q_uote_semi_hash_eq.gcode", true},
        {"tab\there\nnl.gcode", "tab_here_nl.gcode", true},
        {"", "print.gcode", true},
        {".gcode", "print.gcode", true},
        {"///", "print.gcode", true},
        {"dir/.gcode", "dir.gcode", true},
    };
    for (const auto& r : rows) {
        INFO("in: " << r.in);
        const auto n = snapmaker_preprint_sanitise_filename(r.in);
        CHECK(n.name == r.out);
        CHECK(n.changed == r.changed);
        CHECK(snapmaker_preprint_filename_ok(n.name));
        CHECK(snapmaker_preprint_sanitise_filename(n.name).name == n.name); // idempotent
    }
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
    void run_background(std::function<void()> work, std::function<void()> on_ui) override { work(); call_after(std::move(on_ui)); }
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
    std::vector<std::string> errors;
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
        d.make_view = [this]() -> std::unique_ptr<PreprintView> {
            views.push_back(std::make_shared<ViewState>());
            return std::make_unique<FakeView>(views.back(), log);
        };
        d.report_error = [this](const std::string& m) { errors.push_back(m); log.add("error"); };
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
    CHECK(f.views.back()->title == "Bob_s part.gcode");
    CHECK(f.c->remote_filename() == "Bob_s part.gcode");
    CHECK(f.c->name_changed());
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
    f.c->on_user_closed();
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
    f.c->on_user_closed();
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
    f.c->on_page_done(true);
    CHECK(f.log.count("view:destroy") == 1);
    CHECK(f.switches == 0); // not from inside the callback
    f.sched->pump();
    CHECK(f.switches == 1);
    CHECK(f.log.index("view:destroy") < f.log.index("switch_tab"));
    f.sched->advance(ms(1000));
    CHECK(f.switches == 1);
    f.c->on_page_done(true); // a late second completion changes nothing
    f.sched->pump();
    CHECK(f.switches == 1);
}

TEST_CASE("no tab switch when not asked, when the start did not happen, or when the user closed the window", "[SendDialog][Preprint]")
{
    {   Fixture f; f.open_live(true, false); f.c->on_page_done(true); f.sched->advance(ms(100)); CHECK(f.switches == 0); }
    {   Fixture f; f.open_live(true, true);  f.c->on_page_done(false); f.sched->advance(ms(100)); CHECK(f.switches == 0); } // start wanted, not started
    {   Fixture f; f.open_live(true, true);  f.c->on_user_closed(); f.sched->advance(ms(100)); CHECK(f.switches == 0); }
    {   Fixture f; f.open_live(false, true); f.c->on_page_done(false); f.sched->advance(ms(100)); CHECK(f.switches == 1); } // upload only: done = success
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
    CHECK(f.c->open(std::move(r)) == PreprintDialogController::Result::Refused);
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

TEST_CASE("the pre-print path never uses ShowModal", "[SendDialog][Preprint]")
{
    // Source scan, not behaviour: the new dialog and controller must stay non-modal (#84). PrintHostSendDialog (webview-free) is shown
    // modally from Plater.cpp on purpose, first, like Snapmaker; it is not part of these files.
    const fs::path src = fs::path(__FILE__).parent_path().parent_path().parent_path() / "src" / "slic3r" / "GUI";
    for (const char* name : {"PreprintDialogController.hpp", "PreprintDialog.hpp", "PreprintDialog.cpp"}) {
        INFO(name);
        REQUIRE(fs::exists(src / name));
        const std::string text = read_all(src / name);
        CHECK(text.find("ShowModal") == std::string::npos);
        CHECK(text.find("EndModal") == std::string::npos);
    }
}

} // namespace
