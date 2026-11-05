#include "SpoolmanJoin.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <mutex>

#include <nlohmann/json.hpp>

namespace Slic3r {

std::string spoolman_uid_from_bytes(const nlohmann::json& bytes)
{
    if (!bytes.is_array() || bytes.empty())
        return {};
    std::string out;
    bool        any = false;
    for (const auto& b : bytes) {
        if (!b.is_number_integer())
            return {};
        const long v = b.get<long>();
        if (v < 0 || v > 255)
            return {};
        char buf[3];
        std::snprintf(buf, sizeof buf, "%02x", static_cast<unsigned>(v));
        out += buf;
        any = any || v != 0;
    }
    return any ? out : std::string();
}

std::string spoolman_normalize_uid(const std::string& raw)
{
    std::string s;
    for (char c : raw)
        if (c != '"' && c != '\'' && !std::isspace(static_cast<unsigned char>(c)))
            s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

namespace {

double num_or(const nlohmann::json& o, const char* key, double fallback)
{
    auto it = o.find(key);
    return (it != o.end() && it->is_number()) ? it->get<double>() : fallback;
}

std::string str_or_empty(const nlohmann::json& o, const char* key)
{
    auto it = o.find(key);
    return (it != o.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

// extra.card_uids is a JSON-encoded string: "\"04ab\"", or possibly an encoded list. Returns every UID in it.
std::vector<std::string> card_uids_of(const nlohmann::json& extra)
{
    std::vector<std::string> uids;
    if (!extra.is_object())
        return uids;
    auto it = extra.find("card_uids");
    if (it == extra.end() || !it->is_string())
        return uids;
    const std::string raw = it->get<std::string>();
    auto              inner = nlohmann::json::parse(raw, nullptr, false);
    if (!inner.is_discarded() && inner.is_array()) {
        for (const auto& e : inner)
            if (e.is_string())
                uids.push_back(spoolman_normalize_uid(e.get<std::string>()));
    } else {
        uids.push_back(spoolman_normalize_uid(raw));
    }
    uids.erase(std::remove(uids.begin(), uids.end(), std::string()), uids.end());
    return uids;
}

} // namespace

std::map<std::string, SpoolmanSpoolInfo> spoolman_parse_spools(const std::string& body)
{
    std::map<std::string, SpoolmanSpoolInfo> out;
    auto                                     json = nlohmann::json::parse(body, nullptr, false);
    if (json.is_discarded())
        return out;
    const nlohmann::json* list = &json;
    if (json.is_object()) {
        auto it = json.find("result");
        if (it == json.end())
            return out;
        list = &*it;
    }
    if (!list->is_array())
        return out;

    for (const auto& sp : *list) {
        if (!sp.is_object() || !sp.contains("id") || !sp["id"].is_number_integer())
            continue;
        if (sp.value("archived", false) == true)
            continue;
        const auto extra_it = sp.find("extra");
        const auto uids = extra_it != sp.end() ? card_uids_of(*extra_it) : std::vector<std::string>{};
        if (uids.empty())
            continue;

        SpoolmanSpoolInfo info;
        info.spool_id           = sp["id"].get<int>();
        info.remaining_weight_g = num_or(sp, "remaining_weight", -1.0);
        info.used_weight_g      = num_or(sp, "used_weight", -1.0);
        info.initial_weight_g   = num_or(sp, "initial_weight", -1.0);
        auto fit                = sp.find("filament");
        if (fit != sp.end() && fit->is_object()) {
            const auto& f = *fit;
            info.name      = str_or_empty(f, "name");
            info.material  = str_or_empty(f, "material");
            info.color_hex = str_or_empty(f, "color_hex");
            if (!info.color_hex.empty() && info.color_hex[0] == '#')
                info.color_hex.erase(0, 1);
            info.nozzle_temp = static_cast<int>(num_or(f, "settings_extruder_temp", 0));
            info.bed_temp    = static_cast<int>(num_or(f, "settings_bed_temp", 0));
            info.density     = num_or(f, "density", 0);
            info.diameter    = num_or(f, "diameter", 0);
            auto vit         = f.find("vendor");
            if (vit != f.end() && vit->is_object())
                info.vendor = str_or_empty(*vit, "name");
        }
        for (const auto& uid : uids)
            out.emplace(uid, info); // first spool wins if two share a UID
    }
    return out;
}

std::vector<SpoolmanSpoolInfo> spoolman_join_slots(const std::vector<std::string>&                  slot_uids,
                                                   const std::map<std::string, SpoolmanSpoolInfo>& by_uid)
{
    std::vector<SpoolmanSpoolInfo> out(slot_uids.size());
    for (size_t i = 0; i < slot_uids.size(); ++i) {
        if (slot_uids[i].empty())
            continue;
        auto it = by_uid.find(spoolman_normalize_uid(slot_uids[i]));
        if (it != by_uid.end())
            out[i] = it->second;
    }
    return out;
}

std::vector<std::string> spoolman_slot_uids(const nlohmann::json& info)
{
    std::vector<std::string> out;
    if (!info.is_array())
        return out;
    for (const auto& slot : info)
        out.push_back(slot.is_object() && slot.contains("CARD_UID") ? spoolman_uid_from_bytes(slot["CARD_UID"]) : std::string());
    return out;
}

nlohmann::json spoolman_to_json(const SpoolmanSpoolInfo& s)
{
    nlohmann::json j = {{"spool_id", s.spool_id},
                        {"name", s.name},
                        {"material", s.material},
                        {"vendor", s.vendor},
                        {"color", s.color_hex},
                        {"spoolman_nozzle_temp", s.nozzle_temp},
                        {"spoolman_bed_temp", s.bed_temp},
                        {"spoolman_density", s.density},
                        {"spoolman_diameter", s.diameter}};
    j["remaining_weight_g"] = s.remaining_weight_g >= 0 ? nlohmann::json(s.remaining_weight_g) : nlohmann::json(nullptr);
    j["used_weight_g"]      = s.used_weight_g >= 0 ? nlohmann::json(s.used_weight_g) : nlohmann::json(nullptr);
    j["initial_weight_g"]   = s.initial_weight_g >= 0 ? nlohmann::json(s.initial_weight_g) : nlohmann::json(nullptr);
    return j;
}

namespace {
std::mutex                                       g_cache_mutex;
std::map<std::string, std::vector<SpoolmanSpoolInfo>> g_cache;
} // namespace

void spoolman_cache_set(const std::string& dev_id, const std::vector<SpoolmanSpoolInfo>& slots)
{
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    g_cache[dev_id] = slots;
}

nlohmann::json spoolman_cache_get()
{
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    nlohmann::json              out = nlohmann::json::array();
    for (const auto& [dev, slots] : g_cache) {
        nlohmann::json arr = nlohmann::json::array();
        for (size_t i = 0; i < slots.size(); ++i)
            if (slots[i].valid()) {
                nlohmann::json j = spoolman_to_json(slots[i]);
                j["slot"]        = static_cast<int>(i);
                arr.push_back(std::move(j));
            }
        if (!arr.empty())
            out.push_back({{"dev_id", dev}, {"slots", arr}});
    }
    return out;
}

} // namespace Slic3r
