#include "SpoolmanSyncRun.hpp"

#include <atomic>
#include <thread>

namespace Slic3r { namespace Spoolman {

struct SyncRunner::State
{
    std::atomic<bool> busy{false};
    std::atomic<bool> alive{true};
};

SyncRunner::SyncRunner() : m_state(std::make_shared<State>()) {}
SyncRunner::~SyncRunner() { detach(); }

void SyncRunner::detach() { m_state->alive = false; }
bool SyncRunner::in_flight() const { return m_state->busy; }

SyncRunner::Start SyncRunner::start(bool preference_on, FetchFn fetch, PostFn post, DoneFn done)
{
    if (!preference_on)
        return Start::preference_off;
    bool expected = false;
    if (!m_state->busy.compare_exchange_strong(expected, true))
        return Start::busy;
    std::shared_ptr<State> st = m_state;
    try {
        std::thread([st, fetch = std::move(fetch), post = std::move(post), done = std::move(done)]() mutable {
            CatalogFetch result;
            try {
                result = fetch();
            } catch (...) {
                result = CatalogFetch{}; // attempted == false: reported as unreachable
            }
            post([st, done = std::move(done), result = std::move(result)]() mutable {
                st->busy = false;
                if (st->alive)
                    done(std::move(result));
            });
        }).detach();
    } catch (...) {
        m_state->busy = false;
        throw;
    }
    return Start::started;
}

void apply_fetch_to_outcome(SyncOutcome &outcome, const CatalogFetch &fetch, bool has_last_good)
{
    if (fetch.good()) {
        outcome.catalog_state       = CatalogState::fresh;
        outcome.catalog_size        = fetch.filaments.items.size();
        outcome.fetched_age_seconds = 0;
        return;
    }
    // Reached but unusable (malformed body) is a bad response; any transport failure, or no attempt, is unreachable.
    const bool transport_failed = fetch.vendors.status == ParseStatus::HttpFailure || fetch.filaments.status == ParseStatus::HttpFailure ||
                                  fetch.spools.status == ParseStatus::HttpFailure;
    const bool reached = fetch.attempted && !transport_failed;
    outcome.catalog_state    = reached ? CatalogState::bad_response : CatalogState::unreachable;
    outcome.has_previous_sync = has_last_good;
}

}} // namespace Slic3r::Spoolman
