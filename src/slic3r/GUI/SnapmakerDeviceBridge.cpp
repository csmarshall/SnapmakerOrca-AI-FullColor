#include "SnapmakerDeviceBridge.hpp"

#include <algorithm>
#include <cctype>

#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI {

std::string snapmaker_bare_host(const std::string& address)
{
    std::string h = address;
    if (auto p = h.find("://"); p != std::string::npos)
        h.erase(0, p + 3);
    if (auto p = h.find_first_of("/?#"); p != std::string::npos)
        h.erase(p);
    if (!h.empty() && h.front() != '[') { // not a bracketed IPv6 literal
        if (auto p = h.rfind(':'); p != std::string::npos)
            h.erase(p);
    }
    return h;
}

nlohmann::json snapmaker_device_to_json(const SnapmakerDeviceInfo& d)
{
    const std::string sn   = d.sn.empty() ? d.dev_id : d.sn;
    const std::string name = d.name.empty() ? d.dev_id : d.name;
    return {{"sn", sn},
            {"id", d.dev_id},
            {"dev_id", d.dev_id},
            {"name", name},
            {"device_name", name},
            {"ip", snapmaker_bare_host(d.ip.empty() ? d.dev_id : d.ip)},
            {"port", 1884},
            {"clientId", "orca-" + sn},
            {"link_mode", "lan"},
            {"machine_type", d.model},
            {"connected", d.connected},
            {"online", d.online},
            {"error", d.error}};
}

nlohmann::json snapmaker_devices_to_json(const std::vector<SnapmakerDeviceInfo>& devices)
{
    nlohmann::json out = nlohmann::json::array();
    for (const auto& d : devices)
        out.push_back(snapmaker_device_to_json(d));
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

SnapmakerDeviceBridge::SnapmakerDeviceBridge(Options options) : m_options(std::move(options)), m_sswcp(make_services(m_options))
{
    // The device-oriented commands live here, not in the SSWCP core, because they need the device source. Still an
    // explicit allow-list entry: the page's second startup query for "which machine is connected".
    m_sswcp.register_command(
        "sw_GetConnectedMachine",
        [this](SSWCP_Instance& i) {
            nlohmann::json record = nlohmann::json::object(); // nothing connected: {} (the page treats it as no machine)
            const std::vector<SnapmakerDeviceInfo> devices = m_options.devices ? m_options.devices() : std::vector<SnapmakerDeviceInfo>{};
            for (const auto& d : devices)
                if (d.connected) {
                    record = snapmaker_device_to_json(d);
                    break;
                }
            i.reply_ok(std::move(record));
        },
        false);
}

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
