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
SSWCPServices make_services(SnapmakerDeviceBridge::Options& o, std::function<nlohmann::json()> devices_json)
{
    SSWCPServices s;
    s.sink = [&o](const std::string& js) {
        if (o.run_script)
            o.run_script(js);
    };
    s.local_devices = std::move(devices_json);
    s.http_host     = o.http_host;
    return s;
}
} // namespace

SnapmakerDeviceBridge::SnapmakerDeviceBridge(Options options) : m_options(std::move(options)), m_sswcp(make_services(m_options, [this] { return snapmaker_devices_to_json(current_devices()); }))
{
    register_commands();
}

// The host's machines with sn filled from the serial read off the machine, once known (dev_id until then).
std::vector<SnapmakerDeviceInfo> SnapmakerDeviceBridge::current_devices() const
{
    std::vector<SnapmakerDeviceInfo> devices = m_options.devices ? m_options.devices() : std::vector<SnapmakerDeviceInfo>{};
    for (auto& d : devices)
        if (d.sn.empty())
            if (auto it = m_serials.find(d.dev_id); it != m_serials.end())
                d.sn = it->second;
    return devices;
}

// ---- pure helpers ----

nlohmann::json snapmaker_system_info_result(const nlohmann::json& body)
{
    const nlohmann::json* r = &body;
    if (body.is_object() && body.contains("result") && body["result"].is_object())
        r = &body["result"];
    if (r->is_object() && r->contains("system_info") && (*r)["system_info"].is_object())
        return *r;
    return nlohmann::json::object();
}

std::string snapmaker_serial_from_system_info(const nlohmann::json& body)
{
    const nlohmann::json r = snapmaker_system_info_result(body);
    if (r.contains("system_info") && r["system_info"].contains("product_info") && r["system_info"]["product_info"].is_object()) {
        const auto& pi = r["system_info"]["product_info"];
        if (pi.contains("serial_number") && pi["serial_number"].is_string())
            return pi["serial_number"].get<std::string>();
    }
    return {};
}

bool snapmaker_valid_gcode_name(const std::string& name)
{
    if (name.empty() || name.size() > 512 || name.front() == '/')
        return false;
    for (unsigned char c : name)
        if (c < 0x20 || c == 0x7f || c == '\\')
            return false;
    std::size_t pos = 0;
    while (pos <= name.size()) {
        std::size_t slash = name.find('/', pos);
        if (slash == std::string::npos) slash = name.size();
        const std::string seg = name.substr(pos, slash - pos);
        if (seg == ".." || seg == "." || (seg.empty() && slash != name.size()))
            return false;
        pos = slash + 1;
    }
    return true;
}

std::string snapmaker_percent_encode(const std::string& s, bool keep_slash)
{
    static const char* hex = "0123456789ABCDEF";
    std::string        out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || (keep_slash && c == '/'))
            out += static_cast<char>(c);
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

bool snapmaker_valid_thumbnail_path(const std::string& path)
{
    if (!snapmaker_valid_gcode_name(path))
        return false;
    return path.find_first_of("?#:") == std::string::npos;
}

std::string snapmaker_base64(const std::string& in)
{
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string        out;
    out.reserve((in.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const unsigned v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8) | static_cast<unsigned char>(in[i + 2]);
        out += tbl[v >> 18]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += tbl[v & 63];
    }
    if (i + 1 == in.size()) {
        const unsigned v = static_cast<unsigned char>(in[i]) << 16;
        out += tbl[v >> 18]; out += tbl[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        const unsigned v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8);
        out += tbl[v >> 18]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += '=';
    }
    return out;
}

nlohmann::json snapmaker_thumbnail_error(const std::string& path)
{
    return {{"width", 0}, {"height", 0}, {"size", 0}, {"data", ""}, {"state", "error"}, {"path", path}};
}

nlohmann::json snapmaker_thumbnail_reply(const std::string& path, int width, int height, const std::string& image)
{
    if (image.empty() || image.size() > SNAPMAKER_THUMBNAIL_MAX_BYTES)
        return snapmaker_thumbnail_error(path);
    return {{"width", width}, {"height", height}, {"size", static_cast<long long>(image.size())},
            {"data", snapmaker_base64(image)}, {"state", "success"}, {"path", path}};
}

nlohmann::json snapmaker_pick_thumbnail(const nlohmann::json& list)
{
    nlohmann::json best = nlohmann::json::object();
    long long      best_px = -1;
    if (!list.is_array())
        return best;
    for (const auto& e : list) {
        if (!e.is_object() || !e.contains("thumbnail_path") || !e["thumbnail_path"].is_string() ||
            !e.contains("width") || !e["width"].is_number_integer() || !e.contains("height") || !e["height"].is_number_integer())
            continue;
        if (!snapmaker_valid_thumbnail_path(e["thumbnail_path"].get<std::string>()))
            continue;
        if (e.contains("size") && e["size"].is_number() && e["size"].get<double>() > static_cast<double>(SNAPMAKER_THUMBNAIL_MAX_BYTES))
            continue;
        const long long px = static_cast<long long>(e["width"].get<int>()) * e["height"].get<int>();
        if (px > best_px) { best_px = px; best = e; }
    }
    return best;
}

// Fetches machine.system_info once per machine (callers queue on the fetch in flight) and records its serial.
void SnapmakerDeviceBridge::fetch_system_info(const std::string& dev_id, std::function<void()> then)
{
    if (m_system_info.count(dev_id)) { then(); return; }
    auto& waiters = m_sysinfo_waiters[dev_id];
    waiters.push_back(std::move(then));
    if (waiters.size() > 1)
        return; // a fetch for this machine is already in flight
    auto alive = m_alive;
    auto finish = [this, alive, dev_id](bool ok, nlohmann::json result) {
        if (!*alive)
            return;
        if (ok) {
            const std::string serial = snapmaker_serial_from_system_info(result);
            m_system_info[dev_id] = snapmaker_system_info_result(result);
            if (!serial.empty())
                m_serials[dev_id] = serial;
        } else {
            m_serial_retry_at = m_ticks + 10; // fail soft; try again in about 10 s
        }
        auto cbs = std::move(m_sysinfo_waiters[dev_id]);
        m_sysinfo_waiters.erase(dev_id);
        for (auto& cb : cbs) cb();
        refresh_devices(); // the record's sn may just have changed: tell subscribers
    };
    if (!m_options.fetch_resource) {
        finish(false, nlohmann::json::object());
        return;
    }
    m_options.fetch_resource("/machine/system_info", 256 * 1024, [finish](bool ok, std::string body) {
        nlohmann::json j = ok ? nlohmann::json::parse(body, nullptr, false) : nlohmann::json();
        finish(ok && !j.is_discarded(), j.is_discarded() ? nlohmann::json::object() : j);
    });
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
            const std::vector<SnapmakerDeviceInfo> devices = current_devices();
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

    // --- machine.system_info: the page reads product_info.serial_number from it and must find the sn of our records ---
    m_sswcp.register_command(
        "sw_GetMachineSystemInfo",
        [this](SSWCP_Instance& i) {
            std::string dev_id;
            for (const auto& d : current_devices())
                if (d.connected) { dev_id = d.dev_id; break; }
            if (dev_id.empty()) { i.reply_ok(); return; }
            auto weak = std::weak_ptr<SSWCP_Instance>(i.shared_from_this());
            auto alive = m_alive;
            fetch_system_info(dev_id, [this, weak, alive, dev_id] {
                if (!*alive) return;
                if (auto inst = weak.lock()) {
                    auto it = m_system_info.find(dev_id);
                    inst->reply_ok(it != m_system_info.end() ? it->second : nlohmann::json::object());
                }
            });
        },
        false);

    // --- file metadata: the flat server.files.metadata result, read-only and fail-soft ---
    m_sswcp.register_command(
        "sw_MachineFilesMetadata",
        [this](SSWCP_Instance& i) {
            const nlohmann::json& p = i.params();
            const std::string name = p.is_object() && p.contains("filename") && p["filename"].is_string() ? p["filename"].get<std::string>() : std::string();
            if (!snapmaker_valid_gcode_name(name) || !m_options.fetch_resource) { i.reply_ok(); return; }
            auto weak = std::weak_ptr<SSWCP_Instance>(i.shared_from_this());
            auto alive = m_alive;
            m_options.fetch_resource("/server/files/metadata?filename=" + snapmaker_percent_encode(name, false), 256 * 1024,
                                     [weak, alive](bool ok, std::string body) {
                if (!*alive) return;
                auto inst = weak.lock();
                if (!inst) return;
                nlohmann::json j = ok ? nlohmann::json::parse(body, nullptr, false) : nlohmann::json();
                inst->reply_ok(!j.is_discarded() && j.is_object() && j.contains("result") && j["result"].is_object() ? j["result"] : nlohmann::json::object());
            });
        },
        false);

    // --- thumbnail: sw_FilesThumbnailsBase64 {path:<filename>} -> thumbnails_base64-shaped {width,height,size,data,state,path}.
    // Two read-only GETs: the thumbnail list, then the chosen image by its relative gcode-thumbnail path. ---
    m_sswcp.register_command(
        "sw_FilesThumbnailsBase64",
        [this](SSWCP_Instance& i) {
            const nlohmann::json& p = i.params();
            const std::string name = p.is_object() && p.contains("path") && p["path"].is_string() ? p["path"].get<std::string>() : std::string();
            if (!snapmaker_valid_gcode_name(name) || !m_options.fetch_resource) { i.reply_ok(snapmaker_thumbnail_error(name)); return; }
            auto weak = std::weak_ptr<SSWCP_Instance>(i.shared_from_this());
            auto alive = m_alive;
            auto fetch = m_options.fetch_resource; // copy: used again from the first callback
            fetch("/server/files/thumbnails?filename=" + snapmaker_percent_encode(name, false), 256 * 1024,
                  [weak, alive, fetch, name](bool ok, std::string body) {
                if (!*alive) return;
                auto inst = weak.lock();
                if (!inst) return;
                nlohmann::json j = ok ? nlohmann::json::parse(body, nullptr, false) : nlohmann::json();
                nlohmann::json pick = !j.is_discarded() && j.is_object() && j.contains("result") ? snapmaker_pick_thumbnail(j["result"]) : nlohmann::json::object();
                if (pick.empty()) { inst->reply_ok(snapmaker_thumbnail_error(name)); return; }
                const int w = pick["width"].get<int>(), h = pick["height"].get<int>();
                const std::string rel = pick["thumbnail_path"].get<std::string>();
                fetch("/server/files/gcodes/" + snapmaker_percent_encode(rel, true), SNAPMAKER_THUMBNAIL_MAX_BYTES,
                      [weak, alive, name, w, h](bool ok2, std::string image) {
                    if (!*alive) return;
                    if (auto inst2 = weak.lock())
                        inst2->reply_ok(ok2 ? snapmaker_thumbnail_reply(name, w, h, image) : snapmaker_thumbnail_error(name));
                });
            });
        },
        false);

    // --- read-only commands the harness showed the page accepts `{}` for (contract doc 7.4). The cache family is not
    // backed by a store: the page re-derives what it writes. ---
    for (const char* cmd : {"sw_SystemGetDeviceInfo", "sw_FileGetStatus", "sw_exception_query",
                            "sw_MachineFilesRoots", "sw_MachineHeartbeat", "sw_GetCache", "sw_SetCache", "sw_RemoveCache",
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
    nlohmann::json current = snapmaker_devices_to_json(current_devices());
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
    ++m_ticks;
    // The record's sn must be the serial the machine reports (the page re-keys by it); read it once per connected machine.
    if (m_ticks >= m_serial_retry_at)
        for (const auto& d : current_devices())
            if (d.connected && !m_serials.count(d.dev_id) && !m_sysinfo_waiters.count(d.dev_id)) {
                fetch_system_info(d.dev_id, [] {});
                break;
            }
    if (m_ticks % kStatePushEveryTicks == 0)
        push_state(false);
}

void SnapmakerDeviceBridge::shutdown()
{
    m_closed = true;
    *m_alive = false;
    m_sswcp.drop_all_subscriptions();
}

}} // namespace Slic3r::GUI
