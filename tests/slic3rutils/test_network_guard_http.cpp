#include <catch2/catch_test_macros.hpp>

#include "libslic3r/NetworkGuard.hpp"
#include "slic3r/Utils/Http.hpp"

#include <string>

using namespace Slic3r;

// The guard fires before curl is touched, so these never open a socket.
TEST_CASE("Http refuses a non-local host while network access outside the LAN is off", "[NetworkGuard][Http]")
{
    NetworkGuard::set_user_allowed_hosts({});
    NetworkGuard::set_internet_access_allowed(false);

    bool        completed = false;
    bool        errored   = false;
    std::string message;
    unsigned    status = 99;
    Http::get("https://example.com/secret/path?token=abc")
        .on_complete([&](std::string, unsigned) { completed = true; })
        .on_error([&](std::string, std::string error, unsigned http_status) {
            errored = true;
            message = error;
            status  = http_status;
        })
        .perform_sync();

    CHECK_FALSE(completed);
    CHECK(errored);
    CHECK(message == NetworkGuard::blocked_message());
    CHECK(status == 0);
}
