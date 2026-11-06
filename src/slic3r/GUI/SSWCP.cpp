// SSWCP dispatcher core. Derived from Snapmaker/OrcaSlicer src/slic3r/GUI/SSWCP.cpp (AGPL-3.0); see SSWCP.hpp.
#include "SSWCP.hpp"

#include <algorithm>
#include <exception>
#include <vector>

#include <boost/log/trivial.hpp>

#include "libslic3r_version.h"

using json = nlohmann::json;

namespace Slic3r { namespace GUI {

namespace {

struct Registration
{
    SSWCPHandler handler;
    bool         subscription = false; // a request with an event_id registers a long-lived subscription
};

// Replace everything that could forge or split a log line, and cap the length. The page controls this text.
std::string sanitize_for_log(const json& v)
{
    std::string s = v.is_string() ? v.get<std::string>() : std::string();
    if (s.size() > SSWCP_MAX_LOG_CHARS)
        s.resize(SSWCP_MAX_LOG_CHARS);
    for (char& c : s)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f)
            c = ' ';
    return s;
}

// Maximum bracket nesting outside of strings. A pre-scan so a hostile "[[[[..." never reaches the parser
// (or the destructor of the resulting tree). Returns depth+1 as soon as the limit is exceeded.
int json_nesting_depth(const std::string& s, int limit)
{
    int  depth = 0;
    bool in_str = false, esc = false;
    for (char c : s) {
        if (in_str) {
            if (esc)
                esc = false;
            else if (c == '\\')
                esc = true;
            else if (c == '"')
                in_str = false;
            continue;
        }
        if (c == '"')
            in_str = true;
        else if (c == '[' || c == '{') {
            if (++depth > limit)
                return depth;
        } else if (c == ']' || c == '}') {
            if (depth > 0)
                --depth;
        }
    }
    return depth;
}

} // namespace

// Shared state of one dispatcher. Instances hold it weakly so an instance that outlives the dispatcher
// (a future asynchronous handler) cannot reach a dead sink.
struct SSWCPCore
{
    struct Entry
    {
        std::shared_ptr<SSWCP_Instance>       instance;
        std::chrono::steady_clock::time_point expires;
        bool                                  infinite = false;
    };

    SSWCPServices                                   services;
    mutable std::mutex                              mutex;
    std::unordered_map<std::string, Registration>   registry;
    std::unordered_map<SSWCP_Instance*, Entry>      list;   // in-flight instances
    std::map<std::string, std::shared_ptr<SSWCP_Instance>> subscribe_map; // event_id -> subscription

    std::chrono::steady_clock::time_point now() const
    {
        return services.now ? services.now() : std::chrono::steady_clock::now();
    }

    void emit(const json& header, int code, const std::string& message, const json& data)
    {
        if (!services.sink)
            return;
        json response;
        response["header"]  = header;
        response["payload"] = {{"code", code}, {"message", message}, {"data", data}};
        std::string body;
        try {
            // ensure_ascii: the text is embedded in JavaScript and must not carry raw U+2028/2029 or
            // invalid UTF-8; replace invalid sequences instead of throwing.
            body = response.dump(-1, ' ', true, json::error_handler_t::replace);
        } catch (const std::exception&) {
            return;
        }
        services.sink("window.postMessage(JSON.stringify(" + body + "), '*');");
    }

    void add_to_list(const std::shared_ptr<SSWCP_Instance>& inst, bool infinite)
    {
        std::lock_guard<std::mutex> lock(mutex);
        Entry e;
        e.instance = inst;
        e.infinite = infinite;
        e.expires  = now() + SSWCP::DEFAULT_INSTANCE_TIMEOUT;
        list[inst.get()] = std::move(e);
    }

    void remove_from_list(SSWCP_Instance* inst)
    {
        std::shared_ptr<SSWCP_Instance> keep; // destroy outside the lock
        std::lock_guard<std::mutex>     lock(mutex);
        auto                            it = list.find(inst);
        if (it != list.end()) {
            keep = std::move(it->second.instance);
            list.erase(it);
        }
    }
};

// ---------------------------------------------------------------------------------------------------
// SSWCP_Instance
// ---------------------------------------------------------------------------------------------------

SSWCP_Instance::SSWCP_Instance(std::weak_ptr<SSWCPCore> core, std::string cmd, json header, json params,
                               std::string event_id, SSWCPHandler handler, bool subscription)
    : m_header(std::move(header))
    , m_core(std::move(core))
    , m_cmd(std::move(cmd))
    , m_params(std::move(params))
    , m_event_id(std::move(event_id))
    , m_handler(std::move(handler))
    , m_subscription(subscription)
{}

const SSWCPServices& SSWCP_Instance::services() const
{
    static const SSWCPServices empty;
    auto                       core = m_core.lock();
    return core ? core->services : empty;
}

void SSWCP_Instance::process()
{
    if (!m_event_id.empty()) {
        // Acknowledge with the request header, then switch to the event header for everything after.
        send_to_js();
        m_header             = json::object();
        m_header["event_id"] = m_event_id;
    }
    try {
        m_handler(*this);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "SSWCP: handler for '" << sanitize_for_log(m_cmd) << "' threw: " << e.what();
        handle_general_fail();
    }
}

void SSWCP_Instance::send_to_js()
{
    if (auto core = m_core.lock())
        core->emit(m_header, m_status, m_msg, m_res_data);
}

void SSWCP_Instance::finish_job()
{
    if (m_done)
        return;
    m_done = true;
    if (auto core = m_core.lock())
        core->remove_from_list(this);
}

void SSWCP_Instance::handle_general_fail(int code, const std::string& msg)
{
    m_status = code;
    m_msg    = msg;
    send_to_js();
    // A failed subscribe request leaves nothing to push to.
    if (m_subscribed) {
        m_subscribed = false;
        if (auto core = m_core.lock()) {
            std::lock_guard<std::mutex> lock(core->mutex);
            auto                        it = core->subscribe_map.find(m_event_id);
            if (it != core->subscribe_map.end() && it->second.get() == this)
                core->subscribe_map.erase(it);
        }
    }
    finish_job();
}

void SSWCP_Instance::on_timeout()
{
    handle_general_fail(SSWCPCode::Timeout, "time out");
}

void SSWCP_Instance::reply_ok(json data)
{
    if (m_done) // already answered (e.g. timed out before a delayed result arrived)
        return;
    if (m_subscription && !m_event_id.empty()) {
        // The acknowledgement already went out in process(); the instance now waits for pushes.
        subscribe();
        return;
    }
    m_res_data = std::move(data);
    m_status   = SSWCPCode::Ok;
    m_msg      = "OK";
    send_to_js();
    finish_job();
}

bool SSWCP_Instance::subscribe()
{
    auto core = m_core.lock();
    if (!core)
        return false;
    std::shared_ptr<SSWCP_Instance> replaced; // destroy outside the lock
    {
        std::lock_guard<std::mutex> lock(core->mutex);
        auto                        it = core->subscribe_map.find(m_event_id);
        if (it == core->subscribe_map.end() && core->subscribe_map.size() >= SSWCP_MAX_SUBSCRIPTIONS) {
            // fall through to the failure reply below
        } else {
            if (it != core->subscribe_map.end()) {
                replaced = it->second;
                core->list.erase(replaced.get());
            }
            core->subscribe_map[m_event_id] = shared_from_this();
            m_subscribed                    = true;
            SSWCPCore::Entry e;
            e.instance = shared_from_this();
            e.infinite = true;
            core->list[this] = std::move(e);
            return true;
        }
    }
    handle_general_fail(SSWCPCode::TooMany, "too many subscriptions");
    return false;
}

// ---------------------------------------------------------------------------------------------------
// Startup commands
// ---------------------------------------------------------------------------------------------------

namespace {

// The 13 commands the bundle issues on load (ADR 0005, experiment 7b). Everything else is refused.
void register_startup_commands(SSWCPCore& core)
{
    auto one_shot = [&core](const char* cmd, SSWCPHandler h) { core.registry[cmd] = {std::move(h), false}; };
    auto sub      = [&core](const char* cmd, SSWCPHandler h) { core.registry[cmd] = {std::move(h), true}; };

    // The page's log forwarding. Accepted and written to the app log at debug only: the text is capped and
    // stripped of control characters, no `path`/`file` parameter is read, and nothing is written elsewhere.
    one_shot("sw_FileLog", [](SSWCP_Instance& i) {
        const json& p = i.params();
        std::string level   = p.is_object() && p.contains("level") ? sanitize_for_log(p["level"]) : std::string();
        std::string content = p.is_object() && p.contains("content") ? sanitize_for_log(p["content"]) : std::string();
        BOOST_LOG_TRIVIAL(debug) << "[WCP] (" << level << ") " << content;
        i.reply_ok();
    });

    // Telemetry. Snapmaker forwards this to Sentry; our bridge accepts and DROPS it: no handler here may
    // contact the outside world (the core has no network code; a test pins the empty reply).
    one_shot("sw_UploadEvent", [](SSWCP_Instance& i) { i.reply_ok(); });

    one_shot("sw_GetSoftwareInfo", [](SSWCP_Instance& i) {
        const SSWCPServices& s = i.services();
        json                 d = json::object();
        d["name"]    = s.software_name.empty() ? std::string(SLIC3R_APP_NAME) : s.software_name;
        d["version"] = s.software_version.empty() ? std::string(SLIC3R_VERSION) : s.software_version;
        if (!s.http_host.empty())
            d["http_host"] = s.http_host;
        i.reply_ok(std::move(d));
    });

    // The 2.3.40 bundle reads no key from this reply (its `is_login` is only a key the page sends), so an empty
    // object is the whole answer: "not logged in" is the absence of a user. See docs/snapmaker-page-contract.md.
    one_shot("sw_GetUserLoginState", [](SSWCP_Instance& i) { i.reply_ok(); });
    // Login is a Snapmaker cloud concept; this bridge never talks to it. Reply success, do nothing.
    one_shot("sw_UserLogin", [](SSWCP_Instance& i) { i.reply_ok(); });

    one_shot("sw_GetLocalDevices", [](SSWCP_Instance& i) {
        const SSWCPServices& s = i.services();
        json                 d = s.local_devices ? s.local_devices() : json::array();
        i.reply_ok(d.is_array() ? std::move(d) : json::array());
    });

    auto plain_sub = [](SSWCP_Instance& i) { i.reply_ok(); };
    sub("sw_SubUserUpdatePrivacy", plain_sub);
    sub("sw_SubscribePageStateChange", plain_sub);
    sub("sw_SubscribeForegroundChange", plain_sub);
    sub("sw_SubscribeRecentFiles", plain_sub);
    sub("sw_SubscribeLocalDevices", plain_sub);
    sub("sw_SubscribeUserLoginState", plain_sub);
    sub("sw_SubscribeCacheKey", [](SSWCP_Instance& i) {
        const json& p = i.params();
        if (!p.is_object() || !p.contains("key") || !p["key"].is_string() || p["key"].get_ref<const std::string&>().empty() ||
            p["key"].get_ref<const std::string&>().size() > SSWCP_MAX_EVENT_ID_LEN) {
            i.handle_general_fail(SSWCPCode::BadRequest, "param [key] required");
            return;
        }
        i.reply_ok();
    });
}

} // namespace

// ---------------------------------------------------------------------------------------------------
// SSWCP
// ---------------------------------------------------------------------------------------------------

SSWCP::SSWCP(SSWCPServices services) : m_core(std::make_shared<SSWCPCore>())
{
    m_core->services = std::move(services);
    register_startup_commands(*m_core);
}

SSWCP::~SSWCP() = default;

void SSWCP::register_command(const std::string& cmd, SSWCPHandler handler, bool subscription)
{
    std::lock_guard<std::mutex> lock(m_core->mutex);
    m_core->registry[cmd] = {std::move(handler), subscription};
}

bool SSWCP::is_registered(const std::string& cmd) const
{
    std::lock_guard<std::mutex> lock(m_core->mutex);
    return m_core->registry.count(cmd) != 0;
}

SSWCPOutcome SSWCP::handle_web_message(const std::string& message)
{
    try {
        if (message.size() > SSWCP_MAX_MESSAGE_BYTES) {
            BOOST_LOG_TRIVIAL(warning) << "SSWCP: dropped oversized message (" << message.size() << " bytes)";
            return SSWCPOutcome::TooLarge;
        }
        if (json_nesting_depth(message, SSWCP_MAX_JSON_DEPTH) > SSWCP_MAX_JSON_DEPTH) {
            BOOST_LOG_TRIVIAL(warning) << "SSWCP: dropped message nested deeper than " << SSWCP_MAX_JSON_DEPTH;
            return SSWCPOutcome::TooDeep;
        }

        json msg = json::parse(message, nullptr, /*allow_exceptions*/ false);
        if (msg.is_discarded() || !msg.is_object() || !msg.contains("header") || msg["header"].is_null())
            return SSWCPOutcome::Dropped; // nothing to echo, so nothing can be answered

        const json header = msg["header"];
        auto       bad    = [&](const char* why) {
            m_core->emit(header, SSWCPCode::BadRequest, why, json::object());
            return SSWCPOutcome::BadRequest;
        };

        if (!msg.contains("payload") || !msg["payload"].is_object())
            return bad("payload required");
        const json& payload = msg["payload"];
        if (!payload.contains("cmd") || !payload["cmd"].is_string() || payload["cmd"].get_ref<const std::string&>().empty())
            return bad("cmd must be a non-empty string");

        std::string event_id;
        if (payload.contains("event_id") && !payload["event_id"].is_null()) {
            if (!payload["event_id"].is_string() || payload["event_id"].get_ref<const std::string&>().size() > SSWCP_MAX_EVENT_ID_LEN)
                return bad("event_id must be a string");
            event_id = payload["event_id"].get<std::string>();
        }

        if (header.dump().size() > SSWCP_MAX_HEADER_BYTES) { // kept per pending request: bounded, not echoed
            m_core->emit(json::object(), SSWCPCode::BadRequest, "header too large", json::object());
            return SSWCPOutcome::BadRequest;
        }

        const std::string cmd    = payload["cmd"].get<std::string>();
        json              params = payload.contains("params") ? payload["params"] : json::object();
        if (params.is_null())
            params = json::object();

        // Allow-list lookup. Copy the registration under the lock; run it outside.
        Registration reg;
        {
            std::lock_guard<std::mutex> lock(m_core->mutex);
            auto                        it = m_core->registry.find(cmd);
            if (it == m_core->registry.end()) {
                // fall through to the 501 below
            } else
                reg = it->second;
        }
        if (!reg.handler) {
            BOOST_LOG_TRIVIAL(debug) << "SSWCP: unsupported command '" << sanitize_for_log(cmd) << "'";
            m_core->emit(header, SSWCPCode::Unsupported, "unsupported command", json{{"cmd", cmd}});
            return SSWCPOutcome::Unsupported;
        }

        bool busy = false;
        {
            std::lock_guard<std::mutex> lock(m_core->mutex);
            busy = m_core->list.size() >= SSWCP_MAX_PENDING;
        }
        if (busy) {
            m_core->emit(header, SSWCPCode::TooMany, "too many pending requests", json::object());
            return SSWCPOutcome::Busy;
        }

        auto instance = std::make_shared<SSWCP_Instance>(m_core, cmd, header, std::move(params), event_id, reg.handler,
                                                         reg.subscription);
        // Only a registered subscription command can be long-lived; any other request, with or without an event_id,
        // expires after the default timeout if its handler never answers.
        m_core->add_to_list(instance, reg.subscription);
        instance->process();
        return SSWCPOutcome::Dispatched;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "SSWCP: message handling failed: " << e.what();
        return SSWCPOutcome::Dropped;
    } catch (...) {
        return SSWCPOutcome::Dropped;
    }
}

std::size_t SSWCP::push(const std::string& cmd, const json& data, int code, const std::string& message)
{
    std::vector<std::string> targets;
    {
        std::lock_guard<std::mutex> lock(m_core->mutex);
        for (const auto& kv : m_core->subscribe_map)
            if (kv.second && kv.second->cmd() == cmd)
                targets.push_back(kv.first);
    }
    for (const auto& id : targets)
        m_core->emit(json{{"event_id", id}}, code, message, data);
    return targets.size();
}

std::size_t SSWCP::subscriber_count(const std::string& cmd) const
{
    std::lock_guard<std::mutex> lock(m_core->mutex);
    std::size_t                 n = 0;
    for (const auto& kv : m_core->subscribe_map)
        if (kv.second && kv.second->cmd() == cmd)
            ++n;
    return n;
}

std::size_t SSWCP::drop_subscriptions(const std::string& cmd)
{
    std::vector<std::shared_ptr<SSWCP_Instance>> dropped; // destroy outside the lock
    std::lock_guard<std::mutex>                  lock(m_core->mutex);
    for (auto it = m_core->subscribe_map.begin(); it != m_core->subscribe_map.end();) {
        if (it->second && it->second->cmd() == cmd) {
            m_core->list.erase(it->second.get());
            dropped.push_back(std::move(it->second));
            it = m_core->subscribe_map.erase(it);
        } else
            ++it;
    }
    return dropped.size();
}

void SSWCP::drop_all_subscriptions()
{
    std::vector<std::shared_ptr<SSWCP_Instance>> dropped; // destroy outside the lock
    std::lock_guard<std::mutex>                  lock(m_core->mutex);
    for (auto& kv : m_core->subscribe_map) {
        m_core->list.erase(kv.second.get());
        dropped.push_back(std::move(kv.second));
    }
    m_core->subscribe_map.clear();
}

std::size_t SSWCP::expire_due()
{
    std::vector<std::shared_ptr<SSWCP_Instance>> due;
    {
        std::lock_guard<std::mutex> lock(m_core->mutex);
        const auto                  now = m_core->now();
        for (auto it = m_core->list.begin(); it != m_core->list.end();) {
            if (!it->second.infinite && it->second.expires <= now) {
                due.push_back(it->second.instance);
                it = m_core->list.erase(it);
            } else
                ++it;
        }
    }
    for (auto& inst : due)
        inst->on_timeout(); // outside the lock: it sends through the sink
    return due.size();
}

std::size_t SSWCP::instance_count() const
{
    std::lock_guard<std::mutex> lock(m_core->mutex);
    return m_core->list.size();
}

std::size_t SSWCP::subscription_count() const
{
    std::lock_guard<std::mutex> lock(m_core->mutex);
    return m_core->subscribe_map.size();
}

}} // namespace Slic3r::GUI
