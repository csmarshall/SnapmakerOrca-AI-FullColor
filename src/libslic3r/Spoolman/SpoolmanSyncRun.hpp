#ifndef slic3r_SpoolmanSyncRun_hpp_
#define slic3r_SpoolmanSyncRun_hpp_

// Orchestration of the background Spoolman catalog fetch (#100). No wx and no GUI: the fetch and the "post to the GUI thread" step are
// injected, so a test drives it with a fake fetch and a synchronous poster.
//  - The fetch callable runs on a worker thread, never on the thread that calls start() nor on the thread the poster delivers to.
//  - `done` runs from the poster (the GUI thread in the app), only if the runner has not been detach()ed or destroyed meanwhile.
//  - At most one fetch is in flight per runner; start() while one is running returns Start::busy and calls nothing.

#include "SpoolmanSummary.hpp"
#include "SpoolmanTransport.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace Slic3r { namespace Spoolman {

class SyncRunner
{
public:
    using FetchFn = std::function<CatalogFetch()>;
    using PostFn  = std::function<void(std::function<void()>)>; // must run its argument on the target (GUI) thread, later
    using DoneFn  = std::function<void(CatalogFetch)>;

    enum class Start { started, preference_off, busy };

    SyncRunner();
    ~SyncRunner(); // equivalent to detach()

    // With preference_on false nothing is called. The worker is detached; it only touches shared state, so destroying the runner is safe.
    Start start(bool preference_on, FetchFn fetch, PostFn post, DoneFn done);
    bool  in_flight() const;
    // The owner is going away: a pending `done` is dropped.
    void  detach();

private:
    struct State;
    std::shared_ptr<State> m_state;
};

// Fills the catalog-related fields of `outcome` from a fetch. A good fetch is "fresh"; otherwise the outcome is unreachable (a catalog that
// was reached but unusable is bad_response) and `last_good` (cache) says whether an earlier sync exists. The cache stores no timestamp,
// so the stale age stays unknown (the dialog then says so).
void apply_fetch_to_outcome(SyncOutcome &outcome, const CatalogFetch &fetch, bool has_last_good);

// ---- #105: RFID tag re-read, offered only by the Spoolman button ----
// The only printer commands this adds: FILAMENT_DT_CLEAR / FILAMENT_DT_UPDATE for channels 0..3, sent as ONE script, and only when the
// printer reports an idle print_stats.state. Everything here is pure or injected so tests never touch a printer.

// Exactly 8 lines joined by '\n': CLEAR CHANNEL=0..3 then UPDATE CHANNEL=0..3. Built from fixed literals and the ints 0..3 only.
std::string rfid_reread_script();

// True only for Klipper's idle print_stats.state values: standby, complete, cancelled, error (case-sensitive, exact). Printing, paused,
// empty and anything unrecognised are refused.
bool rfid_reread_allowed(const std::string &print_stats_state);

struct RereadTransport
{
    std::function<std::optional<std::string>()> read_state;  // read-only GET of print_stats.state; nullopt = could not be read
    std::function<bool(const std::string &)>     send_script; // POST of the script; true on success
};
enum class RereadSend { sent, busy, state_unknown, send_failed };

// The single place the script is sent. Reads the state right now; sends nothing unless it was read and is idle.
RereadSend send_rfid_reread(const RereadTransport &transport);

// Polls `fetch` every `interval` (via the injected clock and sleep) until it returns a value different from `baseline`, or `timeout`
// elapses. A failed read (nullopt) is not a change. Returns whether a change was seen.
bool poll_until_changed(const std::function<std::optional<std::string>()> &fetch, const std::string &baseline,
                        const std::function<std::chrono::milliseconds()> &now, const std::function<void(std::chrono::milliseconds)> &sleep,
                        std::chrono::milliseconds timeout, std::chrono::milliseconds interval);

}} // namespace Slic3r::Spoolman

#endif
