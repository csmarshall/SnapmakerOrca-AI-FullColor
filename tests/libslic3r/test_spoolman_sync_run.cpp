#include <catch2/catch_test_macros.hpp>
#include "libslic3r/Spoolman/SpoolmanSyncRun.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

using namespace Slic3r::Spoolman;

namespace {
// A poster that queues work; the test thread plays "the GUI thread" and drains it.
struct FakeGui
{
    std::mutex                         m;
    std::condition_variable            cv;
    std::deque<std::function<void()>>  q;
    std::thread::id                    gui = std::this_thread::get_id();

    SyncRunner::PostFn poster()
    {
        return [this](std::function<void()> f) {
            std::lock_guard<std::mutex> l(m);
            q.push_back(std::move(f));
            cv.notify_all();
        };
    }
    void run_one()
    {
        std::function<void()> f;
        {
            std::unique_lock<std::mutex> l(m);
            REQUIRE(cv.wait_for(l, std::chrono::seconds(10), [&] { return !q.empty(); }));
            f = std::move(q.front());
            q.pop_front();
        }
        f();
    }
};

CatalogFetch good_fetch()
{
    CatalogFetch f;
    f.attempted = true;
    f.vendors.status = f.filaments.status = f.spools.status = ParseStatus::Ok;
    f.filaments.items.resize(2);
    return f;
}
} // namespace

TEST_CASE("SyncRunner: success runs fetch off the GUI thread and done on the poster side", "[Spoolman][SyncRun]")
{
    FakeGui     gui;
    SyncRunner  r;
    std::thread::id fetch_thread;
    int         done_calls = 0;
    std::thread::id done_thread;
    auto s = r.start(true, [&] { fetch_thread = std::this_thread::get_id(); return good_fetch(); }, gui.poster(),
                     [&](CatalogFetch f) { ++done_calls; done_thread = std::this_thread::get_id(); CHECK(f.good()); });
    REQUIRE(s == SyncRunner::Start::started);
    gui.run_one();
    CHECK(done_calls == 1);
    CHECK(fetch_thread != gui.gui);
    CHECK(done_thread == gui.gui);
    CHECK_FALSE(r.in_flight());

    SyncOutcome o;
    apply_fetch_to_outcome(o, good_fetch(), false);
    CHECK(o.catalog_state == CatalogState::fresh);
    CHECK(o.catalog_size == 2);
}

TEST_CASE("SyncRunner: failed fetch becomes an unreachable outcome", "[Spoolman][SyncRun]")
{
    FakeGui    gui;
    SyncRunner r;
    CatalogFetch got;
    r.start(true, [] { CatalogFetch f; f.attempted = true; f.vendors = ParseResult<Vendor>::http_failure(0, "timeout"); return f; },
            gui.poster(), [&](CatalogFetch f) { got = f; });
    gui.run_one();
    SyncOutcome o;
    apply_fetch_to_outcome(o, got, true);
    CHECK(o.catalog_state == CatalogState::unreachable);
    CHECK(o.has_previous_sync);
    CHECK_FALSE(got.good());
}

TEST_CASE("SyncRunner: a throwing fetch is reported as not good", "[Spoolman][SyncRun]")
{
    FakeGui    gui;
    SyncRunner r;
    bool       called = false;
    r.start(true, []() -> CatalogFetch { throw std::runtime_error("x"); }, gui.poster(), [&](CatalogFetch f) { called = true; CHECK_FALSE(f.good()); });
    gui.run_one();
    CHECK(called);
}

TEST_CASE("SyncRunner: preference off never fetches", "[Spoolman][SyncRun]")
{
    FakeGui    gui;
    SyncRunner r;
    bool       fetched = false;
    CHECK(r.start(false, [&] { fetched = true; return CatalogFetch{}; }, gui.poster(), [](CatalogFetch) {}) == SyncRunner::Start::preference_off);
    CHECK_FALSE(fetched);
    CHECK_FALSE(r.in_flight());
}

TEST_CASE("SyncRunner: a second start while one is in flight is refused", "[Spoolman][SyncRun]")
{
    FakeGui                 gui;
    SyncRunner              r;
    std::mutex              m;
    std::condition_variable cv;
    bool                    release = false;
    int                     fetches = 0;
    auto slow = [&] {
        ++fetches;
        std::unique_lock<std::mutex> l(m);
        cv.wait(l, [&] { return release; });
        return good_fetch();
    };
    REQUIRE(r.start(true, slow, gui.poster(), [](CatalogFetch) {}) == SyncRunner::Start::started);
    CHECK(r.in_flight());
    CHECK(r.start(true, slow, gui.poster(), [](CatalogFetch) {}) == SyncRunner::Start::busy);
    { std::lock_guard<std::mutex> l(m); release = true; }
    cv.notify_all();
    gui.run_one();
    CHECK(fetches == 1);
    CHECK_FALSE(r.in_flight());
    // and it can start again afterwards
    REQUIRE(r.start(true, [] { return good_fetch(); }, gui.poster(), [](CatalogFetch) {}) == SyncRunner::Start::started);
    gui.run_one();
}

TEST_CASE("SyncRunner: done after the owner is destroyed does nothing", "[Spoolman][SyncRun]")
{
    FakeGui gui;
    bool    called = false;
    {
        SyncRunner r;
        r.start(true, [] { return good_fetch(); }, gui.poster(), [&](CatalogFetch) { called = true; });
    } // destroyed while the worker runs
    gui.run_one();
    CHECK_FALSE(called);
}

// ---- #105: RFID re-read (pure parts; no printer, fakes only) ----

TEST_CASE("RFID re-read: the script is exactly the 8 approved lines", "[Spoolman][RfidReread]")
{
    CHECK(rfid_reread_script() == "FILAMENT_DT_CLEAR CHANNEL=0\nFILAMENT_DT_CLEAR CHANNEL=1\nFILAMENT_DT_CLEAR CHANNEL=2\nFILAMENT_DT_CLEAR CHANNEL=3\n"
                                  "FILAMENT_DT_UPDATE CHANNEL=0\nFILAMENT_DT_UPDATE CHANNEL=1\nFILAMENT_DT_UPDATE CHANNEL=2\nFILAMENT_DT_UPDATE CHANNEL=3");
}

TEST_CASE("RFID re-read: only the four idle Klipper states are allowed", "[Spoolman][RfidReread]")
{
    for (const char *s : {"standby", "complete", "cancelled", "error"})
        CHECK(rfid_reread_allowed(s));
    for (const char *s : {"printing", "paused", "", "Printing", "STANDBY", "Standby", " standby", "standby ", "garbage", "standby\n", "idle"})
        CHECK_FALSE(rfid_reread_allowed(s));
}

namespace {
struct FakeReread
{
    std::optional<std::string> state;     // what the state read returns
    int                        state_reads = 0;
    std::vector<std::string>   sent;
    bool                       send_ok = true;
    RereadTransport transport()
    {
        RereadTransport t;
        t.read_state  = [this] { ++state_reads; return state; };
        t.send_script = [this](const std::string &s) { sent.push_back(s); return send_ok; };
        return t;
    }
};
} // namespace

TEST_CASE("RFID re-read: send refuses when the printer is busy or the state is unknown, and sends nothing", "[Spoolman][RfidReread]")
{
    for (const char *busy : {"printing", "paused", "", "Printing", "garbage"}) {
        FakeReread f;
        f.state = busy;
        CHECK(send_rfid_reread(f.transport()) == RereadSend::busy);
        CHECK(f.sent.empty());
    }
    FakeReread unknown; // state read failed
    CHECK(send_rfid_reread(unknown.transport()) == RereadSend::state_unknown);
    CHECK(unknown.sent.empty());
    RereadTransport no_reader; // no way to read the state at all
    int sends = 0;
    no_reader.send_script = [&](const std::string &) { ++sends; return true; };
    CHECK(send_rfid_reread(no_reader) == RereadSend::state_unknown);
    CHECK(sends == 0);
}

TEST_CASE("RFID re-read: idle state sends exactly one script after re-checking the state", "[Spoolman][RfidReread]")
{
    for (const char *idle : {"standby", "complete", "cancelled", "error"}) {
        FakeReread f;
        f.state = idle;
        CHECK(send_rfid_reread(f.transport()) == RereadSend::sent);
        REQUIRE(f.sent.size() == 1);
        CHECK(f.sent[0] == rfid_reread_script());
        CHECK(f.state_reads == 1);
    }
    FakeReread failing;
    failing.state   = "standby";
    failing.send_ok = false;
    CHECK(send_rfid_reread(failing.transport()) == RereadSend::send_failed);
    CHECK(failing.sent.size() == 1);
}

TEST_CASE("RFID re-read: poll settles only after the snapshot changed and then stayed identical", "[Spoolman][RfidReread]")
{
    using ms = std::chrono::milliseconds;
    ms   clock{0};
    auto now   = [&] { return clock; };
    auto sleep = [&](ms d) { clock += d; };
    auto run   = [&](const std::function<std::optional<std::string>(int)> &script, ms timeout = ms(15000)) {
        int  n     = 0;
        auto fetch = [&]() { return script(n++); };
        return poll_until_settled(fetch, std::string("old"), now, sleep, timeout, ms(1000), 3);
    };
    SECTION("changes then settles -> settled, after 3 identical polls")
    {
        int  n = 0;
        auto r = poll_until_settled([&]() -> std::optional<std::string> { return ++n >= 2 ? "new" : "old"; }, "old", now, sleep, ms(15000),
                                    ms(1000), 3);
        CHECK(r.settled);
        CHECK(r.last == "new");
        CHECK(n == 4); // 1 baseline-equal poll + 3 identical new polls
    }
    SECTION("keeps changing until the timeout -> not settled")
    {
        auto r = run([](int n) -> std::optional<std::string> { return "v" + std::to_string(n); });
        CHECK_FALSE(r.settled);
        CHECK(clock >= ms(15000));
    }
    SECTION("UIDs change at once but the types follow two polls later -> not settled until they do")
    {
        auto r = run([](int n) -> std::optional<std::string> {
            return n < 2 ? "uid=NEW type=PLA" : "uid=NEW type=ABS,PETG"; // first two polls still the cleared default
        });
        CHECK(r.settled);
        CHECK(r.last == "uid=NEW type=ABS,PETG");
        CHECK(clock == ms(5000)); // 2 default polls do not count; 3 settled polls do
    }
    SECTION("never changes -> unconfirmed after the timeout")
    {
        int  n = 0;
        auto r = run([&](int) -> std::optional<std::string> { ++n; return "old"; });
        CHECK_FALSE(r.settled);
        // Identical to the baseline for 5 polls: the wait ends then, not at the 15 s cap (a real run waited the full 15 s for nothing)
        CHECK(clock == ms(5000));
        CHECK(n == 5);
    }
    SECTION("unchanged early-out can be disabled and does not fire while the state differs")
    {
        int  n = 0;
        auto r = poll_until_settled([&]() -> std::optional<std::string> { ++n; return "old"; }, "old", [&] { return clock; },
                                    [&](ms d) { clock += d; }, ms(15000), ms(1000), 3, 0);
        CHECK_FALSE(r.settled);
        CHECK(clock >= ms(15000));
        clock = ms(0);
        auto c = poll_until_settled([&]() -> std::optional<std::string> { return "changing" + std::to_string(clock.count()); }, "old",
                                    [&] { return clock; }, [&](ms d) { clock += d; }, ms(15000), ms(1000), 3, 5);
        CHECK_FALSE(c.settled);
        CHECK(clock >= ms(15000));
    }
    SECTION("failed reads are tolerated: not a change, and they reset the stable run")
    {
        auto r = run([](int n) -> std::optional<std::string> {
            if (n == 2) return std::nullopt; // fail in the middle of the stable run
            return "new";
        });
        CHECK(r.settled);
        CHECK(clock == ms(6000)); // new,new,fail,new,new,new
        auto allfail = run([](int) -> std::optional<std::string> { return std::nullopt; }, ms(3000));
        CHECK_FALSE(allfail.settled);
        CHECK(allfail.last.empty());
    }
}

TEST_CASE("apply_fetch_to_outcome: the last good age reaches the unreachable notice", "[Spoolman][SyncRun]")
{
    CatalogFetch failed;
    failed.attempted = true;
    failed.vendors   = ParseResult<Vendor>::http_failure(0, "timeout");

    SyncOutcome o;
    o.tiers = {};
    apply_fetch_to_outcome(o, failed, true, 3 * 3600 + 5);
    CHECK(o.last_good_age_seconds == 3 * 3600 + 5);
    const SummaryModel with_age = build_summary(o);
    bool found = false;
    for (const auto &n : with_age.notices)
        if (n.text.find("3 hours ago") != std::string::npos)
            found = true;
    CHECK(found);

    // Without an age the previous sync is still reported, as unknown.
    SyncOutcome u;
    apply_fetch_to_outcome(u, failed, true);
    CHECK(u.has_previous_sync);
    CHECK_FALSE(u.last_good_age_seconds.has_value());

    // No earlier sync: an age is ignored.
    SyncOutcome n;
    apply_fetch_to_outcome(n, failed, false, 99);
    CHECK_FALSE(n.last_good_age_seconds.has_value());
}
