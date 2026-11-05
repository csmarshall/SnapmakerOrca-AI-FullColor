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
//    first navigation (start_url()) and then as the HttpOnly, SameSite=Strict cookie `orca_wps` that
//    the first response sets. A query parameter alone would force the token into every sub-resource
//    URL of a bundle we must not edit; a cookie alone cannot start the session. Responses carry
//    `Referrer-Policy: no-referrer` so the token in the first URL is not leaked;
//  * Host must be exactly 127.0.0.1:<port> and Origin absent or exactly http://127.0.0.1:<port>
//    (DNS rebinding and cross-site requests);
//  * never sends Access-Control-Allow-Origin or any other CORS header;
//  * GET and HEAD only; no directory listing; no endpoint that serves a file by caller-supplied path;
//  * path traversal (dot segments, encoded separators, NUL, backslash, absolute-form targets and
//    symlinks leaving the root) is rejected.
class WebPageServer
{
public:
    static constexpr const char* url_prefix   = "/web/flutter_web/";
    static constexpr const char* token_param  = "wps_token";
    static constexpr const char* cookie_name  = "orca_wps";

    explicit WebPageServer(boost::filesystem::path root);
    ~WebPageServer();
    WebPageServer(const WebPageServer&)            = delete;
    WebPageServer& operator=(const WebPageServer&) = delete;

    // Returns false (and logs) if the root does not exist or the socket cannot be bound.
    bool start();
    void stop();
    bool is_running() const;

    uint16_t           port() const;
    const std::string& token() const;
    // http://127.0.0.1:<port>/web/flutter_web/index.html?wps_token=<token>
    std::string start_url() const;

private:
    struct Impl;
    std::shared_ptr<Impl> m_impl;
};

}} // namespace Slic3r::GUI

#endif
