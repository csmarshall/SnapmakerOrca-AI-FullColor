#pragma once

#include "IPrinterAgent.hpp"
#include "MoonrakerPrinterAgent.hpp"

#include <map>
#include <string>
#include "SpoolmanJoin.hpp"

namespace Slic3r {

class SnapmakerPrinterAgent final : public MoonrakerPrinterAgent
{
public:
    explicit SnapmakerPrinterAgent(std::string log_dir);
    ~SnapmakerPrinterAgent() override = default;

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    DeviceUi         device_ui() const override { return DeviceUi::SnapmakerWeb; }

    bool fetch_filament_info(std::string dev_id, FilamentSyncMode sync_mode = FilamentSyncMode::pull) override;

private:
    // Read-only: POST /server/spoolman/proxy (GET /v1/spool). Fails soft: returns an empty map and logs when
    // Spoolman is not connected, the proxy is missing, or the reply is not what we expect.
    std::map<std::string, SpoolmanSpoolInfo> fetch_spoolman_spools_by_uid();
    // Combine filament_type + filament_sub_type into a unified type string
    static std::string combine_filament_type(const std::string& type, const std::string& sub_type);
};

} // namespace Slic3r
