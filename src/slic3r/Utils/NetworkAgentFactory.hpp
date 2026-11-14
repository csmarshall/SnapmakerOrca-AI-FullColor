#ifndef __NETWORK_AGENT_FACTORY_HPP__
#define __NETWORK_AGENT_FACTORY_HPP__

#include "CloudProvider.hpp"
#include "ICloudServiceAgent.hpp"
#include "IPrinterAgent.hpp"
#include "NetworkAgent.hpp"
#include "OrcaCloudServiceAgent.hpp"
#include "BBLCloudServiceAgent.hpp"
#include "BBLNetworkPlugin.hpp"
#include "libslic3r/AppConfig.hpp"
#include <memory>
#include <string>
#include <functional>
#include <utility>
#include <vector>

namespace Slic3r {
static constexpr char ORCA_PRINTER_AGENT_ID[] = "orca";
static constexpr char BBL_PRINTER_AGENT_ID[] = "bbl";
static constexpr char MOONRAKER_PRINTER_AGENT_ID[] = "moonraker";
static constexpr char SNAPMAKER_PRINTER_AGENT_ID[] = "snapmaker";

// Factory function type for creating printer agents
using PrinterAgentFactory =
    std::function<std::shared_ptr<IPrinterAgent>(std::shared_ptr<ICloudServiceAgent> cloud_agent, const std::string& log_dir)>;

// Information about a registered printer agent
struct PrinterAgentInfo
{
    std::string         id;           // Registry/config key, e.g. "orca" or a plugin AgentInfo::id
    std::string         display_name; // e.g., "Orca Native", "Bambu Lab"
    std::string         plugin_identifier;     // Empty for built-ins, otherwise <plugin_key>;<uuid>;<capability_name>
    PrinterAgentFactory factory;      // Function to create the agent
    bool                needs_print_host = false; // IPrinterAgent::needs_print_host() of this agent, read once at registration
    bool                supports_preprint_page = false; // IPrinterAgent::supports_preprint_page(), read once at registration

    bool is_plugin() const { return !plugin_identifier.empty(); }
    PrinterAgentInfo(const std::string& id_, const std::string& display_name_, PrinterAgentFactory factory_)
        : id(id_), display_name(display_name_), factory(std::move(factory_))
    {}

    PrinterAgentInfo(const std::string& id_, const std::string& display_name_, const std::string& plugin_identifier, PrinterAgentFactory factory_)
        : id(id_), display_name(display_name_), plugin_identifier(plugin_identifier), factory(std::move(factory_))
    {}
};

/**
 * NetworkAgentFactory - Factory for creating network agent instances
 *
 * This factory creates cloud agents and printer agents for the networking subsystem.
 * The architecture separates cloud services (authentication, project sync) from
 * printer communication (device discovery, print jobs).
 *
 * Startup flow:
 *   1. Call register_all_agents() during app initialization
 *   2. Cloud agent created at startup via create_agent_from_config()
 *   3. Printer agent created on-demand when a printer is selected
 *
 * Usage:
 *   // At app startup (before any agent creation)
 *   NetworkAgentFactory::register_all_agents();
 *
 *   // Create NetworkAgent with cloud agent only
 *   auto agent = create_agent_from_config(log_dir, app_config);
 *
 *   // When printer is selected - create printer agent from registry
 *   auto printer = NetworkAgentFactory::create_printer_agent_by_id("orca", cloud, log_dir);
 */
class NetworkAgentFactory
{
public:
    // ========================================================================
    // Printer Agent Registry
    // ========================================================================

    /**
     * Register all built-in printer agents.
     * Must be called once during application initialization, before any
     * calls to get_registered_printer_agents() or create_printer_agent_by_id().
     */
    static void register_all_agents();

    /**
     * Register a printer agent type
     *
     * @param id Unique identifier for the agent (e.g., "orca", "bbl")
     * @param display_name Human-readable name for UI
     * @param factory Factory function to create the agent
     * @return true if registration succeeded, false if already registered
     */
    static bool register_printer_agent(const std::string& id, const std::string& display_name, PrinterAgentFactory factory,
                                       bool needs_print_host = false, bool supports_preprint_page = false);

    /**
     * Check if an agent registry/config key is registered
     */
    static bool is_printer_agent_registered(const std::string& id);

    /**
     * Get info about a registered agent by registry/config key
     */
    static const PrinterAgentInfo* get_printer_agent_info(const std::string& id);

    /**
     * IPrinterAgent::needs_print_host() for a registry key, usable where only the id is at hand (a saved machine, a preset).
     * The built-in "moonraker" id counts even before registration, so headless code behaves the same.
     */
    static bool printer_agent_needs_print_host(const std::string& id);

    /**
     * Whether the Bambu-style Send/Print dialog's Timelapse and Auto Bed Leveling options apply to printers of this agent.
     * They are Bambu capability flags that the Moonraker-family agents (Snapmaker U1) never send; Snapmaker Orca's U1
     * send dialog has neither (Plater.cpp on_action_print_plate -> send_gcode_legacy). An empty id counts as Bambu.
     */
    static bool printer_agent_supports_bambu_print_options(const std::string& id);

    /**
     * Which dialog the Print plate / Print all action opens: true = the Bambu-style SelectMachineDialog, false = the plain
     * print-host send dialog (Plater::send_gcode_legacy). Snapmaker Orca picks the Bambu dialog only for use_bbl_network()
     * (Plater.cpp:15689-15706); we additionally keep it in agents mode for a Bambu agent, and send every other agent
     * (Moonraker, Snapmaker, Creality, Qidi, ...) through the legacy dialog. Pass the resolved agent id.
     */
    static bool use_bambu_send_dialog(bool use_bbl_network, bool use_printer_agents, const std::string& agent_id);

    /**
     * IPrinterAgent::supports_preprint_page() for a registry key; false for an unknown id.
     */
    static bool printer_agent_supports_preprint_page(const std::string& id);

    /**
     * Which route a non-Bambu Print plate / Print all takes (send-flow S5): true = the page-driven pre-print dialog, false = the legacy
     * print-host send dialog (Plater::send_gcode_legacy, with its #117 print_host fallback). True only when the agent advertises the
     * capability AND a device is connected (the pre-print dialog pins the connected device; it never falls back to the preset's host).
     * Pure: the Bambu choice (use_bambu_send_dialog) is made first and is unchanged.
     */
    static bool use_preprint_send_flow(bool agent_supports_preprint_page, bool device_connected);
    static bool use_preprint_send_flow(const std::string& agent_id, bool device_connected);

    /**
     * Whether the Send/Print dialog offers Time-lapse, Auto Bed Leveling and Flow Calibration as real toggles: they are sent as
     * SET_PRINT_PREFERENCE lines before the print starts (SnapmakerPrintPreference.hpp). Only the Snapmaker agent does this.
     */
    static bool printer_agent_supports_snapmaker_print_preferences(const std::string& id);

    /**
     * Return the full plugin reference for a plugin-backed printer agent ID, or empty for built-ins.
     */
    static std::string get_printer_agent_plugin_identifier(const std::string& id);

    /**
     * Get all registered printer agents (for UI population)
     */
    static std::vector<PrinterAgentInfo> get_registered_printer_agents();

    /**
     * Create a printer agent by registry/config key
     *
     * Returns a cached instance if one exists for the given key, otherwise
     * creates a new agent via the registered factory and caches it.
     *
     * @param id Agent registry/config key to create
     * @param cloud_agent Cloud agent for token access
     * @param log_dir Directory for log files
     * @return Shared pointer to IPrinterAgent, or nullptr if ID not found
     */
    static std::shared_ptr<IPrinterAgent> create_printer_agent_by_id(const std::string&                  id,
                                                                     std::shared_ptr<ICloudServiceAgent> cloud_agent,
                                                                     const std::string&                  log_dir);

    /**
     * Clear the printer agent cache.
     * Calls disconnect_printer() on each cached agent and releases all shared_ptrs.
     * Should be called during application shutdown before destroying the NetworkAgent.
     */
    static void clear_printer_agent_cache();

    // ========================================================================
    // Cloud Agent Factory
    // ========================================================================

    /**
     * Create a cloud service agent based on provider type.
     * Handles authentication, project sync, and other cloud services.
     *
     * @param provider Which implementation to use (Orca or BBL)
     * @param log_dir Directory for log files
     * @return Shared pointer to ICloudServiceAgent implementation
     */
    static std::shared_ptr<ICloudServiceAgent> create_cloud_agent(const std::string& provider, const std::string& log_dir)
    {
        if (provider == ORCA_CLOUD_PROVIDER) {
            return std::make_shared<OrcaCloudServiceAgent>(log_dir);
        } else if (provider == BBL_CLOUD_PROVIDER) {
            auto& plugin = BBLNetworkPlugin::instance();
            if (!plugin.is_loaded()) {
                return nullptr;
            }
            if (!plugin.has_agent()) {
                plugin.create_agent(log_dir);
            }
            if (!plugin.has_agent()) {
                return nullptr;
            }
            return std::make_shared<BBLCloudServiceAgent>();
        }
        return nullptr;
    }

    // Plugin printer agents
    static void register_python_plugin(const std::string& plugin_key);
    static void deregister_python_plugin(const std::string& plugin_key);

    static void register_python_printer_agent(const std::string& plugin_key, const std::string& capability_name);
    static void deregister_python_printer_agent(const std::string& plugin_key, const std::string& capability_name);

private:
    // Factory is not instantiable
    NetworkAgentFactory()                                      = delete;
    ~NetworkAgentFactory()                                     = delete;
    NetworkAgentFactory(const NetworkAgentFactory&)            = delete;
    NetworkAgentFactory& operator=(const NetworkAgentFactory&) = delete;
};

/**
 * Create a NetworkAgent from AppConfig settings (main entry point)
 *
 * Creates a NetworkAgent with cloud agent only. The printer agent is created
 * separately when a printer is selected, via create_printer_agent_by_id().
 *
 * Cloud provider: Always creates OrcaCloudServiceAgent as the primary provider.
 * Third-party cloud agents (e.g., Bambu) are created from the cloud_providers
 * AppConfig setting and added via NetworkAgent::add_cloud_agent().
 *
 * @param log_dir Directory for log files
 * @param app_config Application configuration object
 * @return NetworkAgent with cloud agent, or nullptr on failure
 */
std::unique_ptr<NetworkAgent> create_agent_from_config(const std::string& log_dir, AppConfig* app_config);

} // namespace Slic3r

#endif // __NETWORK_AGENT_FACTORY_HPP__
