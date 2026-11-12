#ifndef slic3r_SpoolmanTierReport_hpp_
#define slic3r_SpoolmanTierReport_hpp_

// Plain data read out of PresetBundle::sync_ams_list's Spoolman tier (S6). Its own small header so the sync summary model does not have to
// include the heavy PresetBundle.hpp. No logic.

#include <string>
#include <vector>

namespace Slic3r {

// Optional read-out of the Spoolman matching tier inside PresetBundle::sync_ams_list (see its comment in PresetBundle.cpp).
struct SpoolmanTierReport
{
    struct Entry
    {
        int                      slot = -1;     // key of the tray in filament_ams_list
        std::string              rule;          // "spoolman_id" | "external_id" | "setting_id" | "ambiguous"
        std::string              preset;        // chosen preset (empty when ambiguous)
        std::vector<std::string> candidates;    // ambiguous only: every preset that matched under the rule
    };
    std::vector<Entry> entries;
};

} // namespace Slic3r

#endif
