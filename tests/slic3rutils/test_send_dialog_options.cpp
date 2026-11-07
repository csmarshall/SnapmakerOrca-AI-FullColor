#include <catch2/catch_test_macros.hpp>

#include "slic3r/Utils/NetworkAgentFactory.hpp"

using namespace Slic3r;

// #84: the Bambu-style Send/Print dialog showed Timelapse and Auto Bed Leveling for the Snapmaker U1, where nothing sends them.
// Snapmaker Orca's U1 send dialog has neither; only Bambu printers (and an unset agent id) get them.
TEST_CASE("Bambu-only send dialog options are offered only to Bambu printers", "[SendDialog]")
{
    CHECK(NetworkAgentFactory::printer_agent_supports_bambu_print_options(BBL_PRINTER_AGENT_ID));
    CHECK(NetworkAgentFactory::printer_agent_supports_bambu_print_options(""));

    CHECK_FALSE(NetworkAgentFactory::printer_agent_supports_bambu_print_options("snapmaker"));
    CHECK_FALSE(NetworkAgentFactory::printer_agent_supports_bambu_print_options(MOONRAKER_PRINTER_AGENT_ID));
    CHECK_FALSE(NetworkAgentFactory::printer_agent_supports_bambu_print_options(ORCA_PRINTER_AGENT_ID));
}

// #85: Time-lapse, Auto Bed Leveling and Flow Calibration are real per-print toggles for the Snapmaker U1, applied as
// SET_PRINT_PREFERENCE lines before the print starts. Only the Snapmaker agent offers them.
// Fallback to the per-agent predicate: the printer-preset route (a PrintConfig option) was judged too invasive for this change.
TEST_CASE("Snapmaker print preference toggles are offered only to the Snapmaker agent", "[SendDialog][PrintPreference]")
{
    CHECK(NetworkAgentFactory::printer_agent_supports_snapmaker_print_preferences(SNAPMAKER_PRINTER_AGENT_ID));
    CHECK(NetworkAgentFactory::printer_agent_supports_snapmaker_print_preferences("snapmaker"));

    CHECK_FALSE(NetworkAgentFactory::printer_agent_supports_snapmaker_print_preferences(MOONRAKER_PRINTER_AGENT_ID));
    CHECK_FALSE(NetworkAgentFactory::printer_agent_supports_snapmaker_print_preferences(BBL_PRINTER_AGENT_ID));
    CHECK_FALSE(NetworkAgentFactory::printer_agent_supports_snapmaker_print_preferences(ORCA_PRINTER_AGENT_ID));
    CHECK_FALSE(NetworkAgentFactory::printer_agent_supports_snapmaker_print_preferences("qidi"));
    CHECK_FALSE(NetworkAgentFactory::printer_agent_supports_snapmaker_print_preferences("crealityprint"));
    CHECK_FALSE(NetworkAgentFactory::printer_agent_supports_snapmaker_print_preferences(""));
}
