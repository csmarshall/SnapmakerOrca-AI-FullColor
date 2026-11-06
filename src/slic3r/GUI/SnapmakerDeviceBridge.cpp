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
    register_commands();
}

std::optional<SnapmakerTab> snapmaker_tab_for_target(const std::string& target)
{
    std::string t = target;
    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (t == "home") return SnapmakerTab::Home;
    if (t == "3deditor") return SnapmakerTab::Prepare;
    if (t == "preview") return SnapmakerTab::Preview;
    if (t == "monitor") return SnapmakerTab::Monitor;
    if (t == "multidevice") return SnapmakerTab::MultiDevice;
    if (t == "project") return SnapmakerTab::Project;
    if (t == "calibration") return SnapmakerTab::Calibration;
    return std::nullopt;
}

nlohmann::json snapmaker_empty_machine_state()
{
    return {{"eventtime", 0}, {"status", nlohmann::json::object()}};
}

const std::vector<std::string>& snapmaker_default_state_objects()
{
    static const std::vector<std::string> objs = {"extruder", "extruder1", "extruder2", "extruder3", "heater_bed", "print_stats",
                                                  "display_status", "virtual_sdcard", "gcode_move", "webhooks", "toolhead",
                                                  "print_task_config", "machine_state_manager", "idle_timeout", "fan",
                                                  "filament_detect", "motion_report"};
    return objs;
}

std::vector<std::string> SnapmakerDeviceBridge::objects_from(const nlohmann::json& params) const
{
    std::vector<std::string> out;
    auto take = [&out](const nlohmann::json& objects) {
        if (!objects.is_object())
            return;
        for (auto it = objects.begin(); it != objects.end() && out.size() < 64; ++it) {
            const std::string& k = it.key();
            const bool ok = !k.empty() && k.size() <= 64 &&
                            std::all_of(k.begin(), k.end(), [](unsigned char c) { return std::isalnum(c) || c == '_' || c == ' '; });
            if (ok)
                out.push_back(k);
        }
    };
    if (params.is_object() && params.contains("objects"))
        take(params["objects"]);
    if (out.empty())
        out = m_state_objects;
    if (out.empty())
        out = snapmaker_default_state_objects();
    return out;
}

void SnapmakerDeviceBridge::run_query(const std::vector<std::string>& objects, std::function<void(bool, nlohmann::json)> done)
{
    if (!m_options.query_state) {
        done(false, nlohmann::json::object());
        return;
    }
    m_options.query_state(objects, std::move(done));
}

void SnapmakerDeviceBridge::push_state(bool force)
{
    if (m_closed || m_state_in_flight || m_sswcp.subscriber_count("sw_SubscribeMachineState") == 0)
        return;
    m_state_in_flight = true;
    auto alive        = m_alive;
    run_query(objects_from(nlohmann::json::object()), [this, alive, force](bool ok, nlohmann::json result) {
        if (!*alive)
            return; // bridge gone
        m_state_in_flight = false;
        if (!ok || !result.is_object() || !result.contains("status"))
            return; // fail soft: keep the last state on the page, say nothing
        if (!force && result["status"] == m_last_state_status)
            return;
        m_last_state_status = result["status"];
        m_sswcp.push("sw_SubscribeMachineState", result);
    });
}

void SnapmakerDeviceBridge::register_commands()
{
    // --- the connected machine ---
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

    // --- navigation: the Home card's Control button sends sw_SwitchTab {target:"Monitor"} ---
    m_sswcp.register_command(
        "sw_SwitchTab",
        [this](SSWCP_Instance& i) {
            const nlohmann::json& p = i.params();
            std::optional<SnapmakerTab> tab;
            if (p.is_object() && p.contains("target") && p["target"].is_string())
                tab = snapmaker_tab_for_target(p["target"].get<std::string>());
            if (!tab) {
                i.handle_general_fail(SSWCPCode::BadRequest, "unknown tab target");
                return;
            }
            if (!m_options.switch_tab || !m_options.switch_tab(*tab)) {
                i.handle_general_fail(SSWCPCode::Failure, "cannot switch tab");
                return;
            }
            i.reply_ok();
        },
        false);

    // --- machine state: the Moonraker printer.objects.query result, UNWRAPPED ({eventtime, status}) ---
    m_sswcp.register_command(
        "sw_GetMachineState",
        [this](SSWCP_Instance& i) {
            auto weak  = std::weak_ptr<SSWCP_Instance>(i.shared_from_this());
            auto alive = m_alive;
            // The reply may come later (the query runs off-thread); the dispatcher keeps the instance until it is sent.
            run_query(objects_from(i.params()), [weak, alive](bool ok, nlohmann::json result) {
                if (!*alive)
                    return;
                if (auto inst = weak.lock())
                    inst->reply_ok(ok && result.is_object() && result.contains("status") ? std::move(result) : snapmaker_empty_machine_state());
            });
        },
        false);
    m_sswcp.register_command(
        "sw_SubscribeMachineState",
        [this](SSWCP_Instance& i) {
            if (i.event_id().empty()) { // nothing to push to: behave like one GetMachineState
                auto weak = std::weak_ptr<SSWCP_Instance>(i.shared_from_this());
                auto alive = m_alive;
                run_query(objects_from(i.params()), [weak, alive](bool ok, nlohmann::json result) {
                    if (!*alive)
                        return;
                    if (auto inst = weak.lock())
                        inst->reply_ok(ok && result.is_object() && result.contains("status") ? std::move(result) : snapmaker_empty_machine_state());
                });
                return;
            }
            m_state_objects = objects_from(i.params());
            i.reply_ok(); // the ack went out in process(); this registers the subscription
            push_state(true);
        },
        true);
    m_sswcp.register_command(
        "sw_SetSubscribeFilter",
        [this](SSWCP_Instance& i) {
            m_state_objects = objects_from(i.params());
            i.reply_ok();
        },
        false);
    auto unsubscribe = [this](SSWCP_Instance& i) {
        m_sswcp.drop_subscriptions("sw_SubscribeMachineState");
        i.reply_ok();
    };
    m_sswcp.register_command("sw_UnSubscribeMachineState", unsubscribe, false);
    m_sswcp.register_command("sw_StopMachineStateSubscription", unsubscribe, false);

    // --- the page's MQTT surface, answered locally; no network, no keys (bridge-level emulation) ---
    m_sswcp.register_command(
        "sw_create_mqtt_client", [](SSWCP_Instance& i) { i.reply_ok(nlohmann::json{{"id", "emu-1"}, {"type", "mqtts"}}); }, false);
    for (const char* cmd : {"sw_mqtt_connect", "sw_mqtt_disconnect", "sw_mqtt_subscribe", "sw_mqtt_unsubscribe", "sw_mqtt_publish"})
        m_sswcp.register_command(cmd, [](SSWCP_Instance& i) { i.reply_ok(); }, false);

    // --- the page tells the host the filaments per sn; stored, not used yet ---
    m_sswcp.register_command(
        "sw_UpdateMachineFilamentInfo",
        [this](SSWCP_Instance& i) {
            const nlohmann::json& p = i.params();
            if (p.is_object() && p.contains("objects") && p["objects"].is_array()) {
                for (const auto& o : p["objects"]) {
                    if (!o.is_object() || !o.contains("key") || !o["key"].is_string() || !o.contains("value"))
                        continue;
                    const std::string key = o["key"].get<std::string>();
                    std::string       val = o["value"].is_string() ? o["value"].get<std::string>() : o["value"].dump();
                    if (key.empty() || key.size() > 128 || val.size() > 65536)
                        continue;
                    if (m_filament_info.size() >= 16 && m_filament_info.count(key) == 0)
                        continue;
                    m_filament_info[key] = std::move(val);
                }
            }
            i.reply_ok();
        },
        false);

    // --- read-only commands the harness showed the page accepts `{}` for (contract doc 7.4). The cache family is not
    // backed by a store: the page re-derives what it writes. ---
    for (const char* cmd : {"sw_GetMachineSystemInfo", "sw_SystemGetDeviceInfo", "sw_FileGetStatus", "sw_exception_query",
                            "sw_MachineFilesRoots", "sw_MachineFilesMetadata", "sw_GetCache", "sw_SetCache", "sw_RemoveCache",
                            "sw_UnsubscribeCacheKeys"})
        m_sswcp.register_command(cmd, [](SSWCP_Instance& i) { i.reply_ok(); }, false);
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
    if (++m_ticks % kStatePushEveryTicks == 0)
        push_state(false);
}

void SnapmakerDeviceBridge::shutdown()
{
    m_closed = true;
    *m_alive = false;
    m_sswcp.drop_all_subscriptions();
}

}} // namespace Slic3r::GUI
