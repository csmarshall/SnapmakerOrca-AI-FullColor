#ifndef slic3r_PreprintCapabilityStore_hpp_
#define slic3r_PreprintCapabilityStore_hpp_

#include <boost/filesystem/path.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace Slic3r { namespace GUI {

// One-shot download capabilities for WebPageServer (docs/records/snapmaker-u1-send-flow-design.md, S3).
//
// register_file() COPIES the caller's file into a private (0700 directory, 0600 file) snapshot, recording its size and sha256, and
// returns the URL path "/cap/<256-bit random token>/<display_name>". The store keeps only the snapshot; the caller's path is never
// stored, so a later re-slice or edit of the source cannot change what is served and no request can name a path.
// Lookup is an exact match on the whole URL path (no parsing, no decoding). A capability dies after ONE full transfer, on revoke(),
// or after the TTL; the snapshot is deleted at that point and the path then answers 410, not 404.
// Thread-safe. The store does not serve anything itself: WebPageServer asks begin_serve()/finish_serve().
class PreprintCapabilityStore
{
public:
    static constexpr std::uint64_t max_snapshot_bytes = 512ull * 1024 * 1024; // a U1 plate G-code is well below this
    static constexpr std::chrono::minutes capability_ttl{10};
    static constexpr std::size_t max_display_name = 128;
    static constexpr const char* path_prefix = "/cap/";

    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    struct Registration
    {
        bool          ok = false;
        std::string   error;      // when !ok
        std::string   url_path;   // "/cap/<token>/<display_name>"
        std::uint64_t size = 0;
        std::string   sha256_hex; // of the snapshot
    };

    enum class Status { Ok, NotFound, Gone, Corrupt };
    struct Serve
    {
        Status                  status = Status::NotFound;
        boost::filesystem::path snapshot; // when Ok
        std::uint64_t           size = 0; // when Ok: the exact Content-Length to send
    };

    // `clock` is injectable for tests; `ttl` defaults to capability_ttl.
    explicit PreprintCapabilityStore(Clock clock = {}, std::chrono::milliseconds ttl = capability_ttl);
    ~PreprintCapabilityStore(); // revoke_all()
    PreprintCapabilityStore(const PreprintCapabilityStore&)            = delete;
    PreprintCapabilityStore& operator=(const PreprintCapabilityStore&) = delete;

    // Letters, digits and . _ - + ( ) only; 1..max_display_name; not "." or "..". Anything else is refused (never rewritten).
    static bool valid_display_name(const std::string& name);

    Registration register_file(const boost::filesystem::path& source, const std::string& display_name);

    // Exact-match lookup of a request path (query already stripped). Ok => the entry is now "in flight": a concurrent request gets
    // Gone. Re-checks the snapshot's size and sha256 first; a mismatch deletes the snapshot and gives Corrupt (permanently).
    Serve begin_serve(const std::string& url_path);
    // complete=true after the whole body was written: the capability is consumed. false (aborted transfer): available again.
    void  finish_serve(const std::string& url_path, bool complete);

    void revoke(const std::string& url_path);
    void revoke_all();
    // Expires what is past its TTL (also done on every register/begin_serve).
    void purge_expired();

    bool has_live_entries() const; // for tests
    boost::filesystem::path snapshot_path(const std::string& url_path) const; // diagnostics/tests; empty once dead

private:
    enum class State { Ready, InFlight, Consumed, Revoked, Expired, Corrupt };
    struct Entry
    {
        State                                 state = State::Ready;
        boost::filesystem::path               snapshot;
        std::uint64_t                         size = 0;
        std::string                           sha256_hex;
        std::chrono::steady_clock::time_point expires;
    };
    void kill_locked(Entry& e, State terminal);
    void purge_expired_locked();

    Clock                      m_clock;
    std::chrono::milliseconds  m_ttl;
    mutable std::mutex         m_mutex;
    std::map<std::string, Entry> m_entries; // keyed by the full URL path; dead entries stay as tombstones (410)
    boost::filesystem::path    m_dir;        // created on first registration, removed on destruction
};

}} // namespace Slic3r::GUI

#endif
