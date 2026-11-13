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

// #84: which dialog the Print plate / Print all action opens. Snapmaker Orca opens the Bambu-style SelectMachineDialog only for
// use_bbl_network() and sends every other printer through send_gcode_legacy (Plater.cpp:15689-15706 in 3-snapmaker-main).
// Our stack also opened it for every printer once use_printer_agents was on, which gave the U1 one "Ext" filament tile.
// The choice is now by agent capability (printer_agent_supports_bambu_print_options), never by printer model (ADR 0003).
// This checks the predicate's own table; it cannot show what the live dialog looks like.
TEST_CASE("Print plate opens the Bambu dialog only for Bambu-network or Bambu-agent printers", "[SendDialog][Routing]")
{
    // Bambu network printer: always the Bambu dialog, whatever the agents switch says.
    CHECK(NetworkAgentFactory::use_bambu_send_dialog(true, false, BBL_PRINTER_AGENT_ID));
    CHECK(NetworkAgentFactory::use_bambu_send_dialog(true, true, BBL_PRINTER_AGENT_ID));
    // Bambu agent in agents mode.
    CHECK(NetworkAgentFactory::use_bambu_send_dialog(false, true, BBL_PRINTER_AGENT_ID));
    // Agents mode off and not a Bambu network: legacy, as Snapmaker.
    CHECK_FALSE(NetworkAgentFactory::use_bambu_send_dialog(false, false, BBL_PRINTER_AGENT_ID));
    CHECK_FALSE(NetworkAgentFactory::use_bambu_send_dialog(false, false, MOONRAKER_PRINTER_AGENT_ID));

    // Agents mode on, non-Bambu agents: legacy send dialog.
    CHECK_FALSE(NetworkAgentFactory::use_bambu_send_dialog(false, true, MOONRAKER_PRINTER_AGENT_ID));
    CHECK_FALSE(NetworkAgentFactory::use_bambu_send_dialog(false, true, SNAPMAKER_PRINTER_AGENT_ID));
    CHECK_FALSE(NetworkAgentFactory::use_bambu_send_dialog(false, true, ORCA_PRINTER_AGENT_ID));
    CHECK_FALSE(NetworkAgentFactory::use_bambu_send_dialog(false, true, "qidi"));
    CHECK_FALSE(NetworkAgentFactory::use_bambu_send_dialog(false, true, "crealityprint"));
}
