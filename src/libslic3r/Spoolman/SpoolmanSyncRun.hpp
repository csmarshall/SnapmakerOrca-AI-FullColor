#ifndef slic3r_SpoolmanSyncRun_hpp_
#define slic3r_SpoolmanSyncRun_hpp_

// Orchestration of the background Spoolman catalog fetch (#100). No wx and no GUI: the fetch and the "post to the GUI thread" step are
// injected, so a test drives it with a fake fetch and a synchronous poster.
//  - The fetch callable runs on a worker thread, never on the thread that calls start() nor on the thread the poster delivers to.
//  - `done` runs from the poster (the GUI thread in the app), only if the runner has not been detach()ed or destroyed meanwhile.
//  - At most one fetch is in flight per runner; start() while one is running returns Start::busy and calls nothing.

#include "SpoolmanSummary.hpp"
#include "SpoolmanTransport.hpp"

#include <functional>
#include <memory>
#include <optional>

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

}} // namespace Slic3r::Spoolman

#endif
