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
            // If the poster drops the callback without running it (e.g. the app is shutting down), the last copy's destruction
            // still clears `busy`, so the runner is never left in flight forever.
            struct BusyReset {
                std::shared_ptr<State> st;
                ~BusyReset() { st->busy = false; }
            };
            auto reset = std::make_shared<BusyReset>(BusyReset{st});
            post([reset, st, done = std::move(done), result = std::move(result)]() mutable {
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

CatalogFetch fetch_after_reread(const std::function<void()> &reread, const std::function<void()> &on_reread_error, const SyncRunner::FetchFn &fetch)
{
    // The re-read may already have told the printer to clear its tags, so a failure here must not stop the catalog fetch.
    try {
        reread();
    } catch (...) {
        on_reread_error();
    }
    return fetch();
}

void apply_fetch_to_outcome(SyncOutcome &outcome, const CatalogFetch &fetch, bool has_last_good, std::optional<long long> last_good_age_seconds)
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
    if (has_last_good)
        outcome.last_good_age_seconds = last_good_age_seconds;
}

std::string rfid_reread_script()
{
    std::string script;
    for (const char *verb : {"FILAMENT_DT_CLEAR", "FILAMENT_DT_UPDATE"})
        for (int channel = 0; channel < 4; ++channel) {
            if (!script.empty())
                script += '\n';
            script += std::string(verb) + " CHANNEL=" + std::to_string(channel);
        }
    return script;
}

bool rfid_reread_allowed(const std::string &state)
{
    return state == "standby" || state == "complete" || state == "cancelled" || state == "error";
}

RereadSend send_rfid_reread(const RereadTransport &transport)
{
    if (!transport.read_state || !transport.send_script)
        return RereadSend::state_unknown;
    const std::optional<std::string> state = transport.read_state(); // read immediately before sending, never a cached value
    if (!state)
        return RereadSend::state_unknown;
    if (!rfid_reread_allowed(*state))
        return RereadSend::busy;
    return transport.send_script(rfid_reread_script()) ? RereadSend::sent : RereadSend::send_failed;
}

SettleResult poll_until_settled(const std::function<std::optional<std::string>()> &fetch, const std::string &baseline,
                                const std::function<std::chrono::milliseconds()> &now,
                                const std::function<void(std::chrono::milliseconds)> &sleep, std::chrono::milliseconds timeout,
                                std::chrono::milliseconds interval, int stable_polls, int unchanged_polls)
{
    SettleResult result;
    int          run = 0;
    const auto   start = now();
    for (;;) {
        sleep(interval);
        const std::optional<std::string> value = fetch();
        if (!value) {
            run = 0;
        } else {
            run         = (*value == result.last) ? run + 1 : 1;
            result.last = *value;
            if (*value != baseline && run >= stable_polls) {
                result.settled = true;
                return result;
            }
            // Still exactly the state read before the send: the re-read changed nothing and will not, so stop waiting.
            if (*value == baseline && unchanged_polls > 0 && run >= unchanged_polls)
                return result;
        }
        if (now() - start >= timeout)
            return result;
    }
}

}} // namespace Slic3r::Spoolman
