#pragma once

// Read-only join between a Moonraker printer's RFID slots and the Spoolman spools it proxies.
// Everything here is pure (JSON strings in, values out) so it can be unit tested without a network.
// Spoolman values are carried under their own names and never overwrite a slot's preset values.

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

struct SpoolmanSpoolInfo
{
    int         spool_id = 0; // 0 = no spool joined to this slot
    int         filament_id = 0;          // Spoolman filament id (the tray join key spoolman_filament_id), 0 = unknown
    std::string filament_external_id;     // filament.external_id (survives a rebuilt Spoolman)
    std::string filament_setting_id;      // filament.extra.setting_id (decoded)
    std::string name;         // Spoolman filament name
    std::string material;
    std::string vendor;
    std::string color_hex;    // RRGGBB, no '#'
    double      remaining_weight_g = -1.0; // -1 = unknown
    double      used_weight_g      = -1.0;
    double      initial_weight_g   = -1.0;
    int         nozzle_temp        = 0;    // filament.settings_extruder_temp, 0 = unknown
    int         bed_temp           = 0;    // filament.settings_bed_temp, 0 = unknown
    double      density            = 0.0;  // g/cm3, 0 = unknown
    double      diameter           = 0.0;  // mm, 0 = unknown

    bool valid() const { return spool_id > 0; }
};

// Lowercase hex of a CARD_UID byte array ([4,115,...] -> "0473..."). "" if not an array of bytes or all zero.
std::string spoolman_uid_from_bytes(const nlohmann::json& bytes);

// Lowercase, with surrounding whitespace and quotes removed ("\"04AB\"" -> "04ab").
std::string spoolman_normalize_uid(const std::string& raw);

// Parse the body of POST /server/spoolman/proxy for GET /v1/spool ({"result":[...]}; a bare array is accepted
// too) into a map keyed by normalized card UID. Archived spools and spools without a card UID are skipped.
// Returns an empty map on any malformed input.
std::map<std::string, SpoolmanSpoolInfo> spoolman_parse_spools(const std::string& body);

// Slot i of the result is the spool whose card UID equals slot_uids[i] (valid() == false when none).
std::vector<SpoolmanSpoolInfo> spoolman_join_slots(const std::vector<std::string>&                  slot_uids,
                                                   const std::map<std::string, SpoolmanSpoolInfo>& by_uid);

// Slot CARD_UIDs from the body of GET /printer/objects/query?filament_detect ("" for a slot without a tag).
std::vector<std::string> spoolman_slot_uids(const nlohmann::json& filament_detect_info);

// Last joined slot of a printer (invalid when the printer, the slot or a join is unknown).
SpoolmanSpoolInfo spoolman_cache_slot(const std::string& dev_id, int slot);

// S9: write the tray's Spoolman join keys (spoolman_filament_id / _external_id / _setting_id) from a joined spool.
// Writes nothing for an unjoined slot, so the sync tier falls through to the next rule and then upstream's path.
void spoolman_fill_tray_config(DynamicPrintConfig& tray_config, const SpoolmanSpoolInfo& spool);

// Read-only JSON view (spoolman_* names) used by the sync payload and the Remote API.
nlohmann::json spoolman_to_json(const SpoolmanSpoolInfo& s);

// Last joined result per printer, written by the agent and read by the Remote API (thread safe).
void           spoolman_cache_set(const std::string& dev_id, const std::vector<SpoolmanSpoolInfo>& slots);
nlohmann::json spoolman_cache_get(); // array of {"dev_id", "slots":[{"slot":i, ...}]} for joined slots only

} // namespace Slic3r
