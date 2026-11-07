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
