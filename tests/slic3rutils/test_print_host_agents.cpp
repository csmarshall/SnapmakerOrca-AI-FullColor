#include <catch2/catch_test_macros.hpp>

#include <slic3r/GUI/DeviceCore/DevManager.h>
#include <slic3r/GUI/DeviceManager.hpp>
#include <libslic3r/AppConfig.hpp>
#include "slic3r/Utils/CrealityPrintAgent.hpp"
#include "slic3r/Utils/IPrinterAgent.hpp"
#include "slic3r/Utils/MoonrakerPrinterAgent.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/Utils/NetworkAgentFactory.hpp"
#include "slic3r/Utils/OrcaPrinterAgent.hpp"
#include "slic3r/Utils/QidiPrinterAgent.hpp"
#include "slic3r/Utils/SnapmakerPrinterAgent.hpp"

using namespace Slic3r;

namespace {
// An agent with a chosen id and chosen needs_print_host(), standing in for the Moonraker family without a network.
class IdAgent final : public OrcaPrinterAgent
{
public:
    IdAgent(std::string id, bool host_based) : OrcaPrinterAgent(""), m_info{std::move(id), "test", "1.0", "test"}, m_host_based(host_based) {}
    AgentInfo get_agent_info() override { return m_info; }
    bool      needs_print_host() const override { return m_host_based; }

private:
    AgentInfo m_info;
    bool      m_host_based;
};
// The registry is process-global and a duplicate id is ignored, so every case registers what it needs itself.
void register_test_agents()
{
    auto null_factory = [](std::shared_ptr<ICloudServiceAgent>, const std::string&) -> std::shared_ptr<IPrinterAgent> { return nullptr; };
    NetworkAgentFactory::register_printer_agent("orca-test::host-based-1", "t", null_factory, true);
    NetworkAgentFactory::register_printer_agent("orca-test::not-host-based-1", "t", null_factory);
}
} // namespace

TEST_CASE("needs_print_host: the Moonraker family needs a typed-in host, nobody else does", "[printhost][device_ui]")
{
    CHECK(MoonrakerPrinterAgent("").needs_print_host());
    CHECK(SnapmakerPrinterAgent("").needs_print_host());
    CHECK(QidiPrinterAgent("").needs_print_host());
    CHECK(CrealityPrintAgent("").needs_print_host());
    CHECK_FALSE(OrcaPrinterAgent("").needs_print_host());

    // By id, with the registry: built-in moonraker counts even unregistered; bbl/orca/unknown do not.
    CHECK(NetworkAgentFactory::printer_agent_needs_print_host(MOONRAKER_PRINTER_AGENT_ID));
    CHECK_FALSE(NetworkAgentFactory::printer_agent_needs_print_host(BBL_PRINTER_AGENT_ID));
    CHECK_FALSE(NetworkAgentFactory::printer_agent_needs_print_host(ORCA_PRINTER_AGENT_ID));
    CHECK_FALSE(NetworkAgentFactory::printer_agent_needs_print_host("no-such-agent"));
    register_test_agents();
    CHECK(NetworkAgentFactory::printer_agent_needs_print_host("orca-test::host-based-1"));
    CHECK_FALSE(NetworkAgentFactory::printer_agent_needs_print_host("orca-test::not-host-based-1"));
}

TEST_CASE("Connection button visibility is decided by capability, BBL unchanged", "[printhost]")
{
    //                         bbl_net agents host
    CHECK_FALSE(connection_button_visible(true, false, false)); // BBL: always hidden
    CHECK_FALSE(connection_button_visible(true, true, true));
    CHECK_FALSE(connection_button_visible(true, true, false));
    CHECK(connection_button_visible(false, false, false));      // agents off: classic button, any printer
    CHECK(connection_button_visible(false, false, true));
    CHECK_FALSE(connection_button_visible(false, true, false)); // agents on, agent without a host: still hidden (as before)
    CHECK(connection_button_visible(false, true, true));        // agents on, host-based agent: shown (the change)
}

TEST_CASE("A blank access code is only replaced by the Bambu placeholder for non-host agents", "[printhost]")
{
    CHECK(effective_access_code("", false) == "88888888");
    CHECK(effective_access_code("", true).empty());
    CHECK(effective_access_code("abc123", false) == "abc123");
    CHECK(effective_access_code("abc123", true) == "abc123");
    CHECK(effective_access_code("88888888", true) == "88888888"); // typed on purpose: kept
}

TEST_CASE("dev_id_from_address derives one id per host", "[printhost][DeviceManager]")
{
    CHECK(MachineObject::dev_id_from_address("192.168.1.5") == "192.168.1.5");
    CHECK(MachineObject::dev_id_from_address("http://192.168.1.5") == "192.168.1.5");
    CHECK(MachineObject::dev_id_from_address("https://u1.local/") == "u1.local");
    CHECK(MachineObject::dev_id_from_address("http://u1.local/some/path") == "u1.local");
    CHECK(MachineObject::dev_id_from_address("u1.local:7125") == "u1.local:7125");
    CHECK(MachineObject::dev_id_from_address("http://u1.local:7125/") == "u1.local:7125");
    CHECK(MachineObject::dev_id_from_address("u1.local", "7125") == "u1.local:7125");
    CHECK(MachineObject::dev_id_from_address("http://u1.local:80", "7125") == "u1.local:7125"); // port field wins
    // Idempotent: feeding an id back in gives the same id.
    CHECK(MachineObject::dev_id_from_address(MachineObject::dev_id_from_address("http://u1.local:7125/")) == "u1.local:7125");
}

TEST_CASE("Host-based agents get exactly one machine per host, whatever id the caller passes", "[printhost][DeviceManager]")
{
    register_test_agents();
    AppConfig config;
    NetworkAgent network(nullptr, std::make_shared<IdAgent>("orca-test::host-based-1", true));
    DeviceManager manager(&network, false, &config);

    BBLocalMachine typed; // what the Bambu 'Bind with Access Code' dialog builds: typed serial number, URL as ip
    typed.dev_id   = "SERIAL123";
    typed.dev_name = "U1";
    typed.dev_ip   = "http://u1.local:7125/";
    BBLocalMachine selected; // what GUI_App::select_machine builds: the address as id and ip
    selected.dev_id   = "u1.local:7125";
    selected.dev_ip   = "u1.local:7125";
    selected.dev_name = "u1.local:7125";

    MachineObject* a = manager.insert_local_device(typed, "lan", "free", "", "");
    MachineObject* b = manager.insert_local_device(selected, "lan", "free", "", "");
    REQUIRE(a != nullptr);
    CHECK(a == b);
    CHECK(a->get_dev_id() == "u1.local:7125");
    CHECK(manager.get_local_machinelist().size() == 1);
    CHECK(manager.get_my_machine_list("orca-test::host-based-1").size() == 1);
    // Typed twice, differently spelled: still one.
    typed.dev_ip = "u1.local:7125";
    manager.insert_local_device(typed, "lan", "free", "", "");
    CHECK(manager.get_local_machinelist().size() == 1);
    // A different host is a different machine.
    selected.dev_ip = "u2.local:7125";
    manager.insert_local_device(selected, "lan", "free", "", "");
    CHECK(manager.get_local_machinelist().size() == 2);

    // A blank API key is a usable machine for a host-based agent (no key is sent) ...
    CHECK(a->has_access_right());
    CHECK(a->get_access_code().empty());
}

TEST_CASE("Other agents keep the id they were given and still need an access code", "[printhost][DeviceManager]")
{
    register_test_agents();
    AppConfig config;
    NetworkAgent network(nullptr, std::make_shared<IdAgent>("orca-test::not-host-based-1", false));
    DeviceManager manager(&network, false, &config);

    BBLocalMachine m;
    m.dev_id   = "SERIAL123";
    m.dev_ip   = "192.0.2.10";
    m.dev_name = "x";
    MachineObject* o = manager.insert_local_device(m, "lan", "free", "", "");
    REQUIRE(o != nullptr);
    CHECK(o->get_dev_id() == "SERIAL123");
    CHECK_FALSE(o->has_access_right()); // blank code, not a host-based agent: unchanged behaviour
    o->set_access_code("12345678", false);
    CHECK(o->has_access_right());
}

TEST_CASE("Moonraker read-only resource allow-list", "[printhost][device_ui]")
{
    CHECK(MoonrakerPrinterAgent::is_readable_resource_path("/machine/system_info"));
    CHECK(MoonrakerPrinterAgent::is_readable_resource_path("/server/files/metadata?filename=a%20b.gcode"));
    CHECK(MoonrakerPrinterAgent::is_readable_resource_path("/server/files/thumbnails?filename=a.gcode"));
    CHECK(MoonrakerPrinterAgent::is_readable_resource_path("/server/files/gcodes/.thumbs/a-300x300.png"));
    for (const char* bad : {"", "machine/system_info", "/printer/gcode/script", "/printer/print/start", "/server/files/delete",
                            "/server/files/gcodes/", "/server/files/gcodes/../../etc/passwd", "/server/files/gcodes//x", "/server/files/metadata",
                            "/machine/system_info?x=1 HTTP/1.1", "/server/files/gcodes/a b.png", "/server/files/gcodes/a\\b",
                            "/machine/reboot", "/server/database/item", "http://evil/machine/system_info", "/server/files/gcodes/x#y"})
    {
        CAPTURE(bad);
        CHECK_FALSE(MoonrakerPrinterAgent::is_readable_resource_path(bad));
    }
}
