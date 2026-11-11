#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <fstream>
#include <string>

using json = nlohmann::json;

static json load_spoolman_fixture(const std::string &name)
{
    std::ifstream in(std::string(TEST_DATA_DIR) + "/spoolman/" + name);
    REQUIRE(in.is_open());
    return json::parse(in);
}

// Smoke test: the captured (comment-stripped) Spoolman responses are present and shaped as the sync code will expect.
TEST_CASE("Spoolman fixtures load", "[Spoolman]")
{
    const json filaments = load_spoolman_fixture("filament.json");
    REQUIRE(filaments.contains("result"));
    const json &list = filaments["result"];
    REQUIRE(list.is_array());
    CHECK(list.size() == 22);

    bool found_16 = false;
    for (const json &f : list)
        if (f.at("id").get<int>() == 16)
            found_16 = true;
    CHECK(found_16);

    const json vendors = load_spoolman_fixture("vendor.json");
    REQUIRE(vendors.contains("result"));
    CHECK_FALSE(vendors["result"].empty());

    // The other captures must at least parse and carry the same envelope.
    for (const char *name : {"spool.json", "field_spool.json", "field_filament.json"})
        CHECK(load_spoolman_fixture(name).contains("result"));
}
