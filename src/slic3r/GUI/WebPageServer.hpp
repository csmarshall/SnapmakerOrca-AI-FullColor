#ifndef slic3r_WebPageServer_hpp_
#define slic3r_WebPageServer_hpp_

#include <boost/filesystem/path.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace Slic3r { namespace GUI {

// Serves one static directory (the vendored Snapmaker Flutter bundle, resources/web/flutter_web) to the
// app's own webview. It exists only because the bundle needs http:// (service worker, wasm streaming,
// <base href="/web/flutter_web/">), so it is deliberately small and hostile to everything else:
//
//  * binds 127.0.0.1 only, on an OS-assigned port;
//  * every request needs the per-session random token, sent as the query parameter `wps_token` on the
//    first navigation (start_url()). That request is answered with a 302 to the same URL without the
//    token plus the HttpOnly, SameSite=Strict cookie `orca_wps_<port>`, so the token never stays in
//    the address bar, in document.location or in anything the page reports (the bundle ships PostHog,
//    which sends $current_url). A cookie is accepted if ANY cookie of that name carries the token. A
//    query parameter alone would force the token into every sub-resource URL of a bundle we must not
//    edit. Responses carry `Referrer-Policy: no-referrer`;
//  * Sec-Fetch-Site, when sent, must be same-origin or none (cross-site and same-site are refused);
//  * Host must be exactly 127.0.0.1:<port> and Origin absent or exactly http://127.0.0.1:<port>
//    (DNS rebinding and cross-site requests);
//  * never sends Access-Control-Allow-Origin or any other CORS header;
//  * GET and HEAD only; no directory listing; no endpoint that serves a file by caller-supplied path;
//  * every response (200, 302, 403, 404, 405, HEAD...) carries the Content-Security-Policy read at start() from
//    flutter_web.csp next to the bundle: default-src 'none', hashes for the bundle's few inline blocks, frame-src 'none',
//    no unsafe-inline / unsafe-eval. A missing, empty or malformed policy file makes start() fail (fail closed): the server
//    never serves without a policy;
//  * path traversal (dot segments, encoded separators, NUL, backslash, absolute-form targets and
//    symlinks leaving the root) is rejected.
class WebPageServer
{
public:
    static constexpr const char* url_prefix   = "/web/flutter_web/";
    static constexpr const char* token_param  = "wps_token";

    // `csp_file` defaults to <root>/../flutter_web.csp (resources/web/flutter_web.csp, beside the manifest).
    explicit WebPageServer(boost::filesystem::path root, boost::filesystem::path csp_file = {});
    ~WebPageServer();
    WebPageServer(const WebPageServer&)            = delete;
    WebPageServer& operator=(const WebPageServer&) = delete;

    // Returns false (and logs) if the root does not exist, the policy file is missing/empty/malformed, or the socket cannot be bound.
    bool start();
    void stop();
    bool is_running() const;

    uint16_t           port() const;
    // Per-port, because cookies are not scoped by port: another loopback server (or an older instance
    // of this one) must not be able to shadow or fix this one's cookie.
    std::string        cookie_name() const; // orca_wps_<port>
    const std::string& token() const;
    // http://127.0.0.1:<port>/web/flutter_web/index.html?wps_token=<token>
    // `extra_query` (e.g. "path=2", the bundle's Device route) is appended after the token and survives the 302 that
    // takes the token out of the URL.
    std::string start_url(const std::string& extra_query = std::string()) const;
    // The policy in force (empty before a successful start()).
    const std::string& content_security_policy() const;

private:
    struct Impl;
    std::shared_ptr<Impl> m_impl;
};

// Exposed so each traversal layer can be tested on its own.
namespace web_page_server_detail {
// Layer 1: turns the part of the URL path after the prefix into a relative path, decoding per segment.
// Returns false for "." / ".." / empty segments, encoded or raw "/" and "\\", NUL, ":" and bad escapes.
// An empty remainder or one ending in "/" maps to index.html.
bool segments_to_relative(const std::string& rest, boost::filesystem::path& rel);
// Layer 2: resolves rel under canonical_root following symlinks. Returns an HTTP status: 200 (resolved
// is a regular file inside the root), 403 (escapes the root), 404 (missing or not a regular file).
int resolve_within(const boost::filesystem::path& canonical_root, const boost::filesystem::path& rel, boost::filesystem::path& resolved);
// Parses a policy file: '#' lines and blank lines are ignored, the rest is joined into one line. Returns "" (refuse) for an
// empty result, one containing control characters, 'unsafe-inline' / 'unsafe-eval', or longer than 8 KiB.
std::string parse_csp_file(const std::string& text);
// The policy with `origin` ("http://192.168.1.5", "https://host:8443", no path) added to connect-src, img-src and
// media-src (media-src is created if absent), for pages that must reach the connected printer (camera, print, send). Returns ""
// for anything that is not a plain http(s) origin. Not used yet: the code path is ready for when those routes are enabled.
std::string csp_with_extra_origin(const std::string& policy, const std::string& origin);
} // namespace web_page_server_detail

}} // namespace Slic3r::GUI

#endif
