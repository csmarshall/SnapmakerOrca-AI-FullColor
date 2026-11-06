#include "SnapmakerDeviceBridge.hpp"

#include <algorithm>
#include <cctype>

#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI {

nlohmann::json snapmaker_devices_to_json(const std::vector<SnapmakerDeviceInfo>& devices)
{
    nlohmann::json out = nlohmann::json::array();
    for (const auto& d : devices)
        out.push_back({{"dev_id", d.dev_id}, {"name", d.name}, {"ip", d.ip}, {"model", d.model}, {"online", d.online}, {"error", d.error}});
    return out;
}

bool snapmaker_is_page_origin(const std::string& url, uint16_t port)
{
    const std::string prefix = "http://127.0.0.1:" + std::to_string(port);
    if (port == 0 || url.compare(0, prefix.size(), prefix) != 0)
        return false;
    // The authority must end exactly after the port: "/", "?", "#" or end of string.
    if (url.size() == prefix.size())
        return true;
    const char next = url[prefix.size()];
    return next == '/' || next == '?' || next == '#';
}

std::string snapmaker_user_agent(const std::string& version, bool dark)
{
#if defined(_WIN32)
    const char* platform = "Windows NT 10.0; Win64; x64";
#elif defined(__APPLE__)
    const char* platform = "Macintosh; Intel Mac OS X 10_15_7";
#else
    const char* platform = "X11; Linux x86_64";
#endif
    return "SM-Slicer/v" + version + (dark ? " (dark) " : " (light) ") + "Mozilla/5.0 (" + platform +
           ") AppleWebKit/605.1.15 (KHTML, like Gecko)";
}

namespace {
SSWCPServices make_services(SnapmakerDeviceBridge::Options& o)
{
    SSWCPServices s;
    s.sink = [&o](const std::string& js) {
        if (o.run_script)
            o.run_script(js);
    };
    s.local_devices = [&o]() { return snapmaker_devices_to_json(o.devices ? o.devices() : std::vector<SnapmakerDeviceInfo>{}); };
    s.http_host     = o.http_host;
    return s;
}
} // namespace

SnapmakerDeviceBridge::SnapmakerDeviceBridge(Options options) : m_options(std::move(options)), m_sswcp(make_services(m_options)) {}

bool SnapmakerDeviceBridge::on_script_message(const std::string& page_url, const std::string& message)
{
    if (m_closed)
        return false;
    if (!snapmaker_is_page_origin(page_url, m_options.port)) {
        BOOST_LOG_TRIVIAL(warning) << "SnapmakerDeviceBridge: dropped a script message from a foreign origin";
        return false;
    }
    m_sswcp.handle_web_message(message);
    return true;
}

void SnapmakerDeviceBridge::refresh_devices()
{
    if (m_closed)
        return;
    nlohmann::json current = snapmaker_devices_to_json(m_options.devices ? m_options.devices() : std::vector<SnapmakerDeviceInfo>{});
    if (current == m_last_devices)
        return;
    m_last_devices = current;
    m_sswcp.push("sw_SubscribeLocalDevices", current);
}

void SnapmakerDeviceBridge::tick()
{
    if (m_closed)
        return;
    m_sswcp.expire_due();
    refresh_devices();
}

void SnapmakerDeviceBridge::shutdown()
{
    m_closed = true;
    m_sswcp.drop_all_subscriptions();
}

}} // namespace Slic3r::GUI
