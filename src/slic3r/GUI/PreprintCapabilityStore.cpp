#include "PreprintCapabilityStore.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <openssl/evp.h>

#include <cctype>
#include <fstream>
#include <random>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = boost::filesystem;

namespace Slic3r { namespace GUI {

namespace {

std::string random_hex(std::size_t bytes)
{
    std::random_device rd;
    static const char  hex[] = "0123456789abcdef";
    std::string        out;
    for (std::size_t i = 0; i < bytes; ++i) {
        const unsigned b = static_cast<unsigned>(rd()) & 0xffu;
        out += hex[b >> 4];
        out += hex[b & 15];
    }
    return out;
}

class Sha256
{
public:
    Sha256() : ctx(EVP_MD_CTX_new()) { ok = ctx && EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1; }
    ~Sha256() { EVP_MD_CTX_free(ctx); }
    Sha256(const Sha256&)            = delete;
    Sha256& operator=(const Sha256&) = delete;
    void update(const char* d, std::size_t n) { ok = ok && EVP_DigestUpdate(ctx, d, n) == 1; }
    bool hex(std::string& out)
    {
        unsigned char md[EVP_MAX_MD_SIZE];
        unsigned      len = 0;
        if (!ok || EVP_DigestFinal_ex(ctx, md, &len) != 1) return false;
        static const char h[] = "0123456789abcdef";
        out.clear();
        for (unsigned i = 0; i < len; ++i) { out += h[md[i] >> 4]; out += h[md[i] & 15]; }
        return true;
    }
private:
    EVP_MD_CTX* ctx;
    bool        ok = false;
};

// size + sha256 of a file; false if unreadable.
bool measure(const fs::path& p, std::uint64_t& size, std::string& sha_hex)
{
    std::ifstream in(p.string(), std::ios::binary);
    if (!in) return false;
    Sha256            h;
    std::vector<char> buf(1 << 20);
    size = 0;
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto n = in.gcount();
        if (n > 0) { h.update(buf.data(), static_cast<std::size_t>(n)); size += static_cast<std::uint64_t>(n); }
    }
    if (!in.eof()) return false;
    return h.hex(sha_hex);
}

} // namespace

constexpr std::chrono::minutes PreprintCapabilityStore::capability_ttl;

PreprintCapabilityStore::PreprintCapabilityStore(Clock clock, std::chrono::milliseconds ttl)
    : m_clock(clock ? std::move(clock) : Clock([] { return std::chrono::steady_clock::now(); })), m_ttl(ttl)
{}

PreprintCapabilityStore::~PreprintCapabilityStore()
{
    revoke_all();
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_dir.empty()) {
        boost::system::error_code ec;
        fs::remove_all(m_dir, ec);
    }
}

bool PreprintCapabilityStore::valid_display_name(const std::string& name)
{
    if (name.empty() || name.size() > max_display_name || name == "." || name == "..") return false;
    for (unsigned char c : name)
        if (!(std::isalnum(c) && c < 0x80) && std::string("._-+()").find(static_cast<char>(c)) == std::string::npos) return false;
    return true;
}

void PreprintCapabilityStore::kill_locked(Entry& e, State terminal)
{
    if (!e.snapshot.empty()) {
        boost::system::error_code ec;
        fs::remove(e.snapshot, ec);
        e.snapshot.clear();
    }
    e.state = terminal;
}

void PreprintCapabilityStore::purge_expired_locked()
{
    const auto now = m_clock();
    for (auto& kv : m_entries)
        if ((kv.second.state == State::Ready || kv.second.state == State::InFlight) && now >= kv.second.expires)
            kill_locked(kv.second, State::Expired);
}

void PreprintCapabilityStore::purge_expired()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    purge_expired_locked();
}

PreprintCapabilityStore::Registration PreprintCapabilityStore::register_file(const fs::path& source, const std::string& display_name)
{
    Registration r;
    if (!valid_display_name(display_name)) { r.error = "invalid display name"; return r; }

    std::lock_guard<std::mutex> lk(m_mutex);
    purge_expired_locked();
    boost::system::error_code ec;
    if (!fs::is_regular_file(source, ec)) { r.error = "source is not a regular file"; return r; }
    if (m_dir.empty()) {
        const fs::path dir = fs::temp_directory_path(ec) / ("orca-preprint-" + random_hex(8));
        if (ec || !fs::create_directory(dir, ec) || ec) { r.error = "cannot create snapshot directory"; return r; }
        fs::permissions(dir, fs::owner_all, ec);
        m_dir = dir;
    }
    const std::string token = random_hex(32); // 256 bits
    const fs::path    snap  = m_dir / token;

    // Copy (with a size cap) while hashing. The snapshot is created exclusively with mode 0600.
    std::ifstream in(source.string(), std::ios::binary);
    if (!in) { r.error = "cannot read source"; return r; }
#ifndef _WIN32
    const int fd = ::open(snap.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) { r.error = "cannot create snapshot"; return r; }
    auto write_all = [fd](const char* d, std::size_t n) {
        while (n > 0) {
            const ssize_t w = ::write(fd, d, n);
            if (w <= 0) return false;
            d += w; n -= static_cast<std::size_t>(w);
        }
        return true;
    };
    struct FdCloser { int fd; ~FdCloser() { ::close(fd); } } closer{fd};
#else
    std::ofstream out(snap.string(), std::ios::binary | std::ios::trunc);
    if (!out) { r.error = "cannot create snapshot"; return r; }
    auto write_all = [&out](const char* d, std::size_t n) { return bool(out.write(d, static_cast<std::streamsize>(n))); };
#endif
    Sha256            h;
    std::vector<char> buf(1 << 20);
    std::uint64_t     total = 0;
    bool              fail = false;
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto n = static_cast<std::size_t>(in.gcount());
        if (n == 0) break;
        total += n;
        if (total > max_snapshot_bytes) { r.error = "file exceeds the size cap"; fail = true; break; }
        if (!write_all(buf.data(), n)) { r.error = "cannot write snapshot"; fail = true; break; }
        h.update(buf.data(), n);
    }
    if (!fail && !in.eof()) { r.error = "cannot read source"; fail = true; }
    std::string sha;
    if (!fail && !h.hex(sha)) { r.error = "hash failed"; fail = true; }
    if (!fail && total == 0) { r.error = "source is empty"; fail = true; }
    if (fail) {
        fs::remove(snap, ec);
        return r;
    }

    Entry e;
    e.snapshot   = snap;
    e.size       = total;
    e.sha256_hex = sha;
    e.expires    = m_clock() + m_ttl;
    r.url_path   = std::string(path_prefix) + token + "/" + display_name;
    m_entries[r.url_path] = std::move(e);
    r.ok         = true;
    r.size       = total;
    r.sha256_hex = sha;
    return r;
}

PreprintCapabilityStore::Serve PreprintCapabilityStore::begin_serve(const std::string& url_path)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    purge_expired_locked();
    Serve s;
    auto  it = m_entries.find(url_path);
    if (it == m_entries.end()) return s; // NotFound
    Entry& e = it->second;
    switch (e.state) {
    case State::Ready: break;
    case State::Corrupt: s.status = Status::Corrupt; return s;
    default: s.status = Status::Gone; return s; // InFlight, Consumed, Revoked, Expired
    }
    std::uint64_t size = 0;
    std::string   sha;
    if (!measure(e.snapshot, size, sha) || size != e.size || sha != e.sha256_hex) {
        BOOST_LOG_TRIVIAL(error) << "PreprintCapabilityStore: snapshot no longer matches its recorded size/sha256; refusing to serve";
        kill_locked(e, State::Corrupt);
        s.status = Status::Corrupt;
        return s;
    }
    e.state    = State::InFlight;
    s.status   = Status::Ok;
    s.snapshot = e.snapshot;
    s.size     = e.size;
    return s;
}

void PreprintCapabilityStore::finish_serve(const std::string& url_path, bool complete)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_entries.find(url_path);
    if (it == m_entries.end() || it->second.state != State::InFlight) return; // revoked/expired meanwhile: stays dead
    if (complete) kill_locked(it->second, State::Consumed);
    else it->second.state = State::Ready;
}

void PreprintCapabilityStore::revoke(const std::string& url_path)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_entries.find(url_path);
    if (it != m_entries.end() && it->second.state != State::Consumed && it->second.state != State::Expired && it->second.state != State::Corrupt)
        kill_locked(it->second, State::Revoked);
}

void PreprintCapabilityStore::revoke_all()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& kv : m_entries)
        if (kv.second.state == State::Ready || kv.second.state == State::InFlight) kill_locked(kv.second, State::Revoked);
}

bool PreprintCapabilityStore::has_live_entries() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& kv : m_entries)
        if (kv.second.state == State::Ready || kv.second.state == State::InFlight) return true;
    return false;
}

fs::path PreprintCapabilityStore::snapshot_path(const std::string& url_path) const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    const auto it = m_entries.find(url_path);
    return it == m_entries.end() ? fs::path() : it->second.snapshot;
}

}} // namespace Slic3r::GUI
