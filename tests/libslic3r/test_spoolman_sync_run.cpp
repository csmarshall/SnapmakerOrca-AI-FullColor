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
