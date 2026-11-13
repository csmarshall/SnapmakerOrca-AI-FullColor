#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "libslic3r/Spoolman/SpoolmanCatalog.hpp"
#include "libslic3r/Spoolman/SpoolmanParent.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

using namespace Slic3r::Spoolman;

namespace {

ParentCandidate cand(std::string name, std::string type, std::string vendor, std::string setting_id = "", std::string filament_id = "",
                     std::string alias = "")
{
    return {std::move(name), std::move(alias), std::move(type), std::move(vendor), std::move(setting_id), std::move(filament_id)};
}

// Names modelled on resources/profiles/Snapmaker/filament and OrcaFilamentLibrary (nozzle suffixes dropped to the one the caller kept).
// Deliberately lists the PLAIN preset BEFORE the flagged one so list order cannot make a test pass.
std::vector<ParentCandidate> realistic()
{
    return {
        cand("Generic PETG @U1 0.4 nozzle", "PETG", "Generic", "gP", "GFG00"),
        cand("Generic PETG HF @U1 0.4 nozzle", "PETG", "Generic", "gPH", "GFG99"),
        cand("Generic PETG-CF @U1 0.4 nozzle", "PETG-CF", "Generic", "gPC", "GFG98"),
        cand("Generic PLA @U1 0.4 nozzle", "PLA", "Generic", "gL", "GFL00"),
        cand("Generic PLA High Speed @U1 0.4 nozzle", "PLA", "Generic", "gLH", "GFL01"),
        cand("Generic PLA Silk @U1 0.4 nozzle", "PLA", "Generic", "gLS", "GFL02"),
        cand("Generic PLA Matte @U1 0.4 nozzle", "PLA", "Generic", "gLM", "GFL03"),
        cand("Generic PLA-CF @U1 0.4 nozzle", "PLA-CF", "Generic", "gLC", "GFL04"),
        cand("Generic ABS @U1 0.4 nozzle", "ABS", "Generic", "gA", "GFB00"),
        cand("Generic TPU @U1 0.4 nozzle", "TPU", "Generic", "gT", "GFU01"),
        cand("Snapmaker PETG @U1", "PETG", "Snapmaker", "sP", "OF4WB37S"),
        cand("Snapmaker PETG HF @U1 0.4 nozzle", "PETG", "Snapmaker", "9NnYt52azW8zCp4a", "OFHF"),
        cand("Snapmaker PETG Translucent @U1 0.4 nozzle", "PETG", "Snapmaker", "sPT", "OFTR"),
        cand("Snapmaker PLA Matte @U1", "PLA", "Snapmaker", "sLM", "OFMAT"),
        cand("Bambu PETG HF", "PETG", "Bambu Lab", "bPH", "GFG98B"),
    };
}

Filament fil(std::string name, std::string vendor, std::string material)
{
    Filament f;
    f.id = 1;
    f.name = std::move(name);
    if (!vendor.empty()) {
        f.vendor.emplace();
        f.vendor->name = std::move(vendor);
    }
    f.material = std::move(material);
    return f;
}

std::string chosen_name(const ParentChoice &c) { return c.chosen ? c.chosen->name : std::string("<none>"); }

bool has_note(const ParentChoice &c, const std::string &needle)
{
    return std::any_of(c.notes.begin(), c.notes.end(), [&](const std::string &n) { return n.find(needle) != std::string::npos; });
}

std::vector<Filament> fixture_filaments()
{
    std::ifstream in(std::string(TEST_DATA_DIR) + "/spoolman/filament.json");
    REQUIRE(in.is_open());
    auto r = parse_filaments(nlohmann::json::parse(in));
    REQUIRE(r.ok());
    return r.items;
}

const Filament &by_id(const std::vector<Filament> &v, int id)
{
    for (auto &f : v)
        if (f.id == id)
            return f;
    FAIL("fixture filament not found");
    return v.front();
}


// The REAL candidate list: tests/data/spoolman/candidates_u1_0.4.tsv, generated from resources/profiles by make_candidates.py.
const std::vector<ParentCandidate> &real_candidates()
{
    static std::vector<ParentCandidate> v;
    if (v.empty()) {
        std::ifstream in(std::string(TEST_DATA_DIR) + "/spoolman/candidates_u1_0.4.tsv");
        REQUIRE(in.is_open());
        std::string line;
        while (std::getline(in, line)) {
            std::vector<std::string> col;
            std::stringstream ss(line);
            std::string c;
            while (std::getline(ss, c, '\t'))
                col.push_back(c);
            col.resize(7);
            v.push_back(cand(col[0], col[2], col[3], col[4], col[5], col[1]));
            v.back().printer_listed = col[6] == "1";
        }
    }
    REQUIRE(v.size() > 300);
    return v;
}

const ParentOptions kU1{"@U1 0.4 nozzle"};

} // namespace

TEST_CASE("Spoolman parent: material alone must not beat the HF flag (known-bad a)", "[Spoolman]")
{
    // Plain PETG listed first, HF second, no vendor candidate: the Generic tier must pick the HF one.
    std::vector<ParentCandidate> c = {cand("Generic PETG @U1 0.4 nozzle", "PETG", "Generic"),
                                      cand("Generic PETG HF @U1 0.4 nozzle", "PETG", "Generic")};
    auto r = choose_parent(fil("PETG HF Red", "Acme", "PETG"), c);
    REQUIRE(r.chosen != nullptr);
    CHECK(r.chosen->name == "Generic PETG HF @U1 0.4 nozzle");
    CHECK(r.index == 1);
    CHECK(r.rule == "material family with flags");
    // and the reverse order gives the same answer
    std::reverse(c.begin(), c.end());
    CHECK(chosen_name(choose_parent(fil("PETG HF Red", "Acme", "PETG"), c)) == "Generic PETG HF @U1 0.4 nozzle");
    // HF requested but only plain PETG exists: none, NOT the plain one
    std::vector<ParentCandidate> only_plain = {cand("Generic PETG @U1 0.4 nozzle", "PETG", "Generic")};
    auto n = choose_parent(fil("PETG HF Red", "Acme", "PETG"), only_plain);
    CHECK(n.chosen == nullptr);
    CHECK(n.index == -1);
}

TEST_CASE("Spoolman parent: filament 22's external_id does not drive the choice (known-bad b)", "[Spoolman]")
{
    auto fils = fixture_filaments();
    const Filament &f22 = by_id(fils, 22);
    REQUIRE(f22.name.find("PolyLite") == 0);
    REQUIRE(f22.name.find("ABS Light Grey") != std::string::npos); // the fixture has a UTF-8 trademark sign and a no-break space after PolyLite
    REQUIRE(f22.external_id.value() == "polymaker_abs_polyliteabsdarkgrey_1000_175_c");
    auto cands = realistic();
    auto r = choose_parent(f22, cands);
    REQUIRE(r.chosen != nullptr);
    CHECK(r.chosen->name == "Generic ABS @U1 0.4 nozzle");
    CHECK(has_note(r, "disagree"));
    // Changing the external_id to name another colour, or removing it, changes the chosen preset not at all
    Filament g = f22;
    g.external_id = "polymaker_abs_polyliteabsredblack_1000_175_c";
    CHECK(chosen_name(choose_parent(g, cands)) == "Generic ABS @U1 0.4 nozzle");
    g.external_id.reset();
    auto r2 = choose_parent(g, cands);
    CHECK(chosen_name(r2) == "Generic ABS @U1 0.4 nozzle");
    CHECK_FALSE(has_note(r2, "disagree"));
    // An external_id naming ANOTHER MATERIAL's profile slug does not help either
    g.external_id = "snapmaker_petg_hf_red";
    CHECK(chosen_name(choose_parent(g, cands)) == "Generic ABS @U1 0.4 nozzle");
}

TEST_CASE("Spoolman parent: an unrequested flag loses (known-bad c)", "[Spoolman]")
{
    auto cands = realistic();
    // "Basic" is in the flag vocabulary and no candidate carries it: none, not plain PLA (conservative, documented guess)
    CHECK(chosen_name(choose_parent(fil("Basic Red", "", "PLA"), cands)) == "<none>");
    // plain PLA, name carries only a colour
    auto r = choose_parent(fil("Red", "NoSuchVendor", "PLA"), cands);
    CHECK(chosen_name(r) == "Generic PLA @U1 0.4 nozzle" );
    CHECK(r.rule == "material alone");
    CHECK(has_note(r, "material only"));
    // Silk requested: Silk wins, not plain, not Matte
    CHECK(chosen_name(choose_parent(fil("Silk Gold", "NoSuchVendor", "PLA"), cands)) == "Generic PLA Silk @U1 0.4 nozzle");
    // CF and High Speed are not mistaken for each other
    CHECK(chosen_name(choose_parent(fil("PLA-CF Black", "NoSuchVendor", "PLA"), cands)) == "Generic PLA-CF @U1 0.4 nozzle");
    CHECK(chosen_name(choose_parent(fil("Carbon Fiber PLA", "NoSuchVendor", "PLA"), cands)) == "Generic PLA-CF @U1 0.4 nozzle");
    CHECK(chosen_name(choose_parent(fil("High Speed PLA Blue", "NoSuchVendor", "PLA"), cands)) == "Generic PLA High Speed @U1 0.4 nozzle");
    // Without a plain Generic candidate, plain PLA must NOT fall to a flagged one
    std::vector<ParentCandidate> flagged_only = {cand("Generic PLA Silk @U1", "PLA", "Generic"), cand("Generic PLA Matte @U1", "PLA", "Generic")};
    CHECK(chosen_name(choose_parent(fil("Red", "NoSuchVendor", "PLA"), flagged_only)) == "<none>");
}

TEST_CASE("Spoolman parent: equally good candidates give none, never list order (known-bad d)", "[Spoolman]")
{
    std::vector<ParentCandidate> c = {cand("Generic PETG @U1 0.4 nozzle", "PETG", "Generic"), cand("Generic PETG @U1 0.6 nozzle", "PETG", "Generic")};
    auto r = choose_parent(fil("Blue", "", "PETG"), c);
    CHECK(r.chosen == nullptr);
    CHECK(r.reason.rfind("ambiguous: ", 0) == 0);
    CHECK(r.reason.find("0.4 nozzle") != std::string::npos);
    CHECK(r.reason.find("0.6 nozzle") != std::string::npos);
    std::reverse(c.begin(), c.end());
    CHECK(choose_parent(fil("Blue", "", "PETG"), c).chosen == nullptr);
    // a tie in the vendor tier is not rescued by a Generic candidate
    std::vector<ParentCandidate> v = {cand("Snapmaker PETG @A", "PETG", "Snapmaker"), cand("Snapmaker PETG @B", "PETG", "Snapmaker"),
                                      cand("Generic PETG @U1", "PETG", "Generic")};
    CHECK(choose_parent(fil("Blue", "Snapmaker", "PETG"), v).chosen == nullptr);
    // a tie on setting_id is ambiguous too
    Filament f = fil("x", "", "PETG");
    f.extras["setting_id"] = "dup";
    std::vector<ParentCandidate> s = {cand("A", "PETG", "", "dup"), cand("B", "PETG", "", "dup")};
    CHECK(choose_parent(f, s).chosen == nullptr);
}

TEST_CASE("Spoolman parent: setting_id beats everything else (known-bad e)", "[Spoolman]")
{
    auto cands = realistic();
    Filament f = fil("PETG HF Red", "Snapmaker", "PETG"); // rules 3/4 would choose the Snapmaker HF preset
    f.extras["setting_id"]                 = "gL";        // hand-picked: Generic PLA
    f.extras["slicer_settings_orcaslicer"] = R"({"id":"sPT"})";
    auto r = choose_parent(f, cands);
    REQUIRE(r.chosen != nullptr);
    CHECK(r.chosen->name == "Generic PLA @U1 0.4 nozzle");
    CHECK(r.rule == "setting_id extra");
    // an unknown setting_id falls through (with a note), here to the OFD id
    f.extras["setting_id"] = "nonexistent";
    auto r2 = choose_parent(f, cands);
    CHECK(chosen_name(r2) == "Snapmaker PETG Translucent @U1 0.4 nozzle");
    CHECK(has_note(r2, "matches no candidate"));
    // setting_id is exact, not a prefix or case-folded
    f.extras["setting_id"] = "GL";
    f.extras.erase("slicer_settings_orcaslicer");
    f.name = "x";
    f.material = "";
    CHECK(choose_parent(f, cands).chosen == nullptr);
}

TEST_CASE("Spoolman parent: OFD slicer settings order id, profile_name, generic_id", "[Spoolman]")
{
    auto cands = realistic();
    cands[0].alias = "Generic PETG";
    Filament f = fil("anything", "", "ABS");
    auto with = [&](const std::string &json) { f.extras["slicer_settings_orcaslicer"] = json; return choose_parent(f, cands); };
    CHECK(chosen_name(with(R"({"id":"OFHF"})")) == "Snapmaker PETG HF @U1 0.4 nozzle");  // filament_id
    CHECK(with(R"({"id":"OFHF"})").rule == "OFD slicer settings: id");
    CHECK(chosen_name(with(R"({"id":"9NnYt52azW8zCp4a"})")) == "Snapmaker PETG HF @U1 0.4 nozzle"); // setting_id
    CHECK(chosen_name(with(R"({"profile_name":"Generic PETG"})")) == "Generic PETG @U1 0.4 nozzle"); // alias
    CHECK(with(R"({"profile_name":"Generic PETG"})").rule == "OFD slicer settings: profile_name");
    CHECK(chosen_name(with(R"({"profile_name":"Generic TPU @U1 0.4 nozzle"})")) == "Generic TPU @U1 0.4 nozzle");
    CHECK(chosen_name(with(R"({"generic_id":"GFU01"})")) == "Generic TPU @U1 0.4 nozzle");
    CHECK(with(R"({"generic_id":"GFU01"})").rule == "OFD slicer settings: generic_id");
    // id outranks profile_name, which outranks generic_id
    CHECK(chosen_name(with(R"({"generic_id":"GFU01","profile_name":"Generic PETG","id":"OFHF"})")) == "Snapmaker PETG HF @U1 0.4 nozzle");
    CHECK(chosen_name(with(R"({"generic_id":"GFU01","profile_name":"Generic PETG","id":"nope"})")) == "Generic PETG @U1 0.4 nozzle");
    // an OFD id naming nothing: falls through to the family rules, which find ABS
    CHECK(chosen_name(with(R"({"id":"nope"})")) == "Generic ABS @U1 0.4 nozzle");
    // wrong-typed values are ignored
    CHECK(chosen_name(with(R"({"id":5,"profile_name":["Generic PETG"]})")) == "Generic ABS @U1 0.4 nozzle");
}

TEST_CASE("Spoolman parent: malformed, oversized and deep OFD JSON falls through without a crash (known-bad f)", "[Spoolman]")
{
    auto cands = realistic();
    Filament f = fil("Red", "NoSuchVendor", "ABS");
    const std::string plain = "Generic ABS @U1 0.4 nozzle";
    auto with = [&](const std::string &json) { f.extras["slicer_settings_orcaslicer"] = json; return choose_parent(f, cands); };
    for (const std::string &bad : {std::string("{"), std::string("not json"), std::string(""), std::string("[1,2]"), std::string("\"s\""),
                                   std::string("{\"id\":"), std::string("null"), std::string("\xFF\xFE{}")}) {
        INFO(bad);
        CHECK(chosen_name(with(bad)) == plain);
    }
    // oversized: a valid object whose id WOULD match, padded past 4096 bytes, is ignored
    std::string big = R"({"id":"OFHF","pad":")" + std::string(5000, 'x') + "\"}";
    auto r = with(big);
    CHECK(chosen_name(r) == plain);
    CHECK(has_note(r, "4096"));
    // deep: 20 nested arrays
    std::string deep = R"({"id":"OFHF","d":)" + std::string(20, '[') + std::string(20, ']') + "}";
    auto d = with(deep);
    CHECK(chosen_name(d) == plain);
    CHECK(has_note(d, "too deep"));
    // an undecoded extra is not trusted
    f.extras["slicer_settings_orcaslicer"] = R"({"id":"OFHF"})";
    f.extras_undecoded.insert("slicer_settings_orcaslicer");
    CHECK(chosen_name(choose_parent(f, cands)) == plain);
}

TEST_CASE("Spoolman parent: vendor is matched by name, not id (known-bad g)", "[Spoolman]")
{
    auto cands = realistic();
    // Two Spoolman vendor records with the same name and different ids (fixtures: Snapmaker 7 and 9) behave identically
    Filament a = fil("PETG HF Red", "Snapmaker", "PETG");
    a.vendor->id = 7;
    Filament b = a;
    b.vendor->id = 9;
    CHECK(chosen_name(choose_parent(a, cands)) == "Snapmaker PETG HF @U1 0.4 nozzle");
    CHECK(chosen_name(choose_parent(b, cands)) == "Snapmaker PETG HF @U1 0.4 nozzle");
    CHECK(choose_parent(a, cands).rule == "vendor + name + material");
    // case-insensitive
    b.vendor->name = "SNAPMAKER";
    CHECK(chosen_name(choose_parent(b, cands)) == "Snapmaker PETG HF @U1 0.4 nozzle");
    // same words, other vendor: the Bambu HF preset is never taken for a Snapmaker filament, and Snapmaker's is not taken for Bambu's
    Filament c = fil("PETG HF Black", "Bambu Lab", "PETG");
    CHECK(chosen_name(choose_parent(c, cands)) == "Bambu PETG HF");
    // vendor-specific plain beats Generic plain
    Filament d = fil("Blue", "Snapmaker", "PETG");
    CHECK(chosen_name(choose_parent(d, cands)) == "Snapmaker PETG @U1");
    // a vendor word that is not a known vendor: Generic
    Filament e = fil("PETG HF Blue", "Overture", "PETG");
    CHECK(chosen_name(choose_parent(e, cands)) == "Generic PETG HF @U1 0.4 nozzle");
}

TEST_CASE("Spoolman parent: nothing compatible gives none, never an arbitrary preset (known-bad h)", "[Spoolman]")
{
    auto cands = realistic();
    auto r = choose_parent(fil("Natural", "Polymaker", "PVA"), cands);
    CHECK(r.chosen == nullptr);
    CHECK(r.index == -1);
    CHECK(r.rule.empty());
    CHECK(r.reason.find("PVA") != std::string::npos);
    CHECK(choose_parent(fil("Red", "X", "PLA"), {}).chosen == nullptr);          // empty list
    CHECK(choose_parent(fil("Red", "X", ""), cands).chosen == nullptr);          // no material
    CHECK(choose_parent(fil("Glow PLA Green", "YOUSU", "PLA"), cands).chosen == nullptr); // Glow requested, no Glow candidate
    CHECK(choose_parent(fil("Matte PLA Red", "X", "PLA"), {cand("Generic PLA", "PLA", "Generic")}).chosen == nullptr);
    // candidate type differs from material: PCTG is not PETG
    CHECK(choose_parent(fil("PCTG Black", "CHCKX", "PCTG"), cands).chosen == nullptr);
}

TEST_CASE("Spoolman parent: hand-computed cases not derived from the code's tables", "[Spoolman]")
{
    // 1. Name 'Matte PLA Lava Orange' (Snapmaker, PLA): strip 'Lava','Orange' (colours) and 'PLA' (material); 'Matte' remains as the
    //    one flag. Candidate 'Snapmaker PLA Matte @U1' is vendor Snapmaker, type PLA, words {Matte}: equal => rule 3, even though
    //    'Generic PLA Matte' is also listed earlier.
    const auto cs = realistic();
    auto r1 = choose_parent(fil("Matte PLA Lava Orange", "Snapmaker", "PLA"), cs);
    CHECK(chosen_name(r1) == "Snapmaker PLA Matte @U1");
    CHECK(r1.rule == "vendor + name + material");
    // 2. Same name from a vendor with no profile: vendor tier is empty, Generic tier holds exactly one preset with only Matte.
    auto r2 = choose_parent(fil("Matte PLA Lava Orange", "Overture", "PLA"), cs);
    CHECK(chosen_name(r2) == "Generic PLA Matte @U1 0.4 nozzle");
    CHECK(r2.rule == "material family with flags");
    // 3. 'PETG+ Black' (Inland): '+' dropped and noted, Black is a colour, no flag => plain Generic PETG by material only.
    auto r3 = choose_parent(fil("PETG+ Black", "Inland", "PETG"), cs);
    CHECK(chosen_name(r3) == "Generic PETG @U1 0.4 nozzle");
    CHECK(has_note(r3, "'+'"));
    CHECK(has_note(r3, "material only"));
    // 4. 'High Flow PETG' is HF in words
    CHECK(chosen_name(choose_parent(fil("High Flow PETG Green", "Q", "PETG"), cs)) == "Generic PETG HF @U1 0.4 nozzle");
}

TEST_CASE("Spoolman parent: the 22 real fixture filaments against a realistic candidate list", "[Spoolman]")
{
    auto fils  = fixture_filaments();
    auto cands = realistic();
    struct Expect { int id; const char *name; const char *rule; };
    const Expect expect[] = {
        {1,  "Generic PETG @U1 0.4 nozzle",        "material alone"},                // PETG+ Black (Inland)
        {2,  "<none>", ""},                                                          // PVA: no candidate
        {3,  "Generic PETG @U1 0.4 nozzle",        "material alone"},                // Transparent (Sunlu)
        {7,  "Generic PETG @U1 0.4 nozzle",        "material alone"},                // Blue (CHCKX)
        {8,  "Generic PETG @U1 0.4 nozzle",        "material alone"},                // Black (Overture)
        {9,  "<none>", ""},                                                          // PCTG: no candidate
        {10, "<none>", ""},                                                          // Glow PLA: no Glow candidate
        {14, "Snapmaker PETG HF @U1 0.4 nozzle",   "vendor + name + material"},
        {16, "Snapmaker PETG HF @U1 0.4 nozzle",   "vendor + name + material"},      // PETG HF Red
        {17, "Snapmaker PLA Matte @U1",            "vendor + name + material"},
        {21, "Snapmaker PLA Matte @U1",            "vendor + name + material"},
        {19, "Generic TPU @U1 0.4 nozzle",         "material alone"},
        {22, "Generic ABS @U1 0.4 nozzle",         "material alone"},                // PolyLite ABS Light Grey
    };
    for (const Expect &e : expect) {
        auto r = choose_parent(by_id(fils, e.id), cands);
        std::cout << "[parent] filament " << e.id << " '" << by_id(fils, e.id).name << "' -> " << chosen_name(r) << " (" << (r.rule.empty() ? r.reason : r.rule)
                  << ")" << std::endl;
        INFO("filament " << e.id);
        CHECK(chosen_name(r) == e.name);
        CHECK(r.rule == e.rule);
    }
    // never plain PETG for the HF Snapmaker filaments, whatever else changes
    for (int id : {14, 15, 16})
        CHECK(chosen_name(choose_parent(by_id(fils, id), cands)).find("HF") != std::string::npos);
    // the same data with the Snapmaker presets removed: HF still never falls to plain PETG
    std::vector<ParentCandidate> generic_only(cands.begin(), cands.begin() + 10);
    CHECK(chosen_name(choose_parent(by_id(fils, 16), generic_only)) == "Generic PETG HF @U1 0.4 nozzle");
    // no fixture filament ever yields a preset of another material
    for (auto &f : fils) {
        auto r = choose_parent(f, cands);
        if (r.chosen) {
            INFO(f.id);
            CHECK(r.chosen->filament_type == f.material);
        }
    }
}

TEST_CASE("Spoolman parent: vocabularies are what the header says", "[Spoolman]")
{
    CHECK(flag_words() == std::vector<std::string>{"aero", "basic", "cf", "esd", "fr", "gf", "glow", "hf", "highspeed", "matte", "rapid", "silk"});
    auto c = colour_words();
    CHECK(std::is_sorted(c.begin(), c.end()));
    // every colour word used by a fixture NAME is covered, so none is mistaken for a descriptive word
    for (const char *w : {"black", "white", "red", "blue", "green", "purple", "rainbow", "orange", "grey", "natural", "transparent", "ivory", "carbon", "lava", "light"})
        CHECK(std::find(c.begin(), c.end(), w) != c.end());
}

// ---- real-profile behaviour ----

TEST_CASE("Spoolman parent: twins @<printer> and @System: the printer-specific one wins, different families still tie", "[Spoolman]")
{
    const auto &c = real_candidates();
    struct Row { const char *name, *vendor, *material, *expect; };
    const Row rows[] = {
        {"Blue", "NoSuchVendor", "PETG", "Generic PETG @U1 0.4 nozzle"},
        {"Black", "NoSuchVendor", "TPU", "Generic TPU @U1 0.4 nozzle"},
        {"Blue", "NoSuchVendor", "PLA", "Generic PLA @U1 0.4 nozzle"},
        {"Silk PLA Gold", "NoSuchVendor", "PLA", "Generic PLA Silk @U1 0.4 nozzle"},
        {"PLA High Speed", "NoSuchVendor", "PLA", "Generic PLA High Speed @U1 0.4 nozzle"},
        {"PETG+ Black", "Inland", "PETG", "Generic PETG @U1 0.4 nozzle"},
    };
    for (const Row &r : rows) {
        INFO(r.name << " / " << r.material);
        // the printer_listed flag would separate the twins on its own, so it is cleared here: this checks the token mechanism
        std::vector<ParentCandidate> unlisted = c;
        for (ParentCandidate &u : unlisted)
            u.printer_listed = false;
        auto with = choose_parent(fil(r.name, r.vendor, r.material), unlisted, kU1);
        CHECK(chosen_name(with) == r.expect);
        CHECK(has_note(with, "twin"));
        // with the flag and no token the twins are also separated (the listed one is the Generic-listed tier)
        CHECK(chosen_name(choose_parent(fil(r.name, r.vendor, r.material), c)) == r.expect);
        // without the token the same input is ambiguous between the twins (never decided by list order)
        auto without = choose_parent(fil(r.name, r.vendor, r.material), unlisted);
        CHECK(without.chosen == nullptr);
        CHECK(without.reason.rfind("ambiguous: ", 0) == 0);
    }
    // Different families never get the tie-break, even when one carries the token. Both candidates below classify identically (same words
    // once lowercased, same tier) and differ only in the name before '@': a tie-break that ignored the family would pick the @U1 one.
    for (const char *other : {"GENERIC PETG @System", "Generic-PETG @System", "Generic PETG Natural @System"}) {
        INFO(other);
        std::vector<ParentCandidate> fam = {cand("Generic PETG @U1 0.4 nozzle", "PETG", "Generic"), cand(other, "PETG", "Generic")};
        auto r = choose_parent(fil("Blue", "", "PETG"), fam, kU1);
        CHECK(r.chosen == nullptr);
        CHECK(r.reason.rfind("ambiguous: ", 0) == 0);
    }
}

TEST_CASE("Spoolman parent: CF/GF from the type suffix and from the name, on both sides", "[Spoolman]")
{
    const auto &c = real_candidates();
    struct Row { const char *name, *material, *expect; };
    const Row rows[] = {
        {"PLA CF", "PLA", "Generic PLA-CF @U1 0.4 nozzle"},
        {"PETG Carbon Fiber", "PETG", "Generic PETG-CF @U1 0.4 nozzle"},
        {"PETG-CF", "PETG", "Generic PETG-CF @U1 0.4 nozzle"},
        {"PLA Carbon Fibre Black", "PLA-CF", "Generic PLA-CF @U1 0.4 nozzle"},
        {"Carbon Fiber PETG", "PETG-CF", "Generic PETG-CF @U1 0.4 nozzle"},
        {"Black", "PLA-CF", "Generic PLA-CF @U1 0.4 nozzle"}, // flag only in the material
        {"Black", "PLA", "Generic PLA @U1 0.4 nozzle"},       // plain PLA never takes the CF preset
    };
    for (const Row &r : rows) {
        INFO(r.name << " / " << r.material);
        CHECK(chosen_name(choose_parent(fil(r.name, "NoSuchVendor", r.material), c, kU1)) == r.expect);
    }
    // GF: the real list has Generic PP-GF only; PETG-GF has no Generic candidate, so none (not PETG-CF, not plain PETG)
    CHECK(chosen_name(choose_parent(fil("Teal", "NoSuchVendor", "PETG-GF"), c, kU1)) == "<none>");
    // candidate side: type 'PLA-CF' (Snapmaker) is PLA + cf, name-only 'Elegoo PLA-CF' (type PLA) is cf too
    CHECK(chosen_name(choose_parent(fil("PLA-CF Black", "Snapmaker", "PLA"), c, kU1)) == "Snapmaker PLA-CF @U1 0.4 nozzle");
    CHECK(chosen_name(choose_parent(fil("PLA-CF Black", "Elegoo", "PLA"), c, kU1)) == "Elegoo PLA-CF @System");
}

TEST_CASE("Spoolman parent: '+' markers compare exactly first", "[Spoolman]")
{
    const auto &c = real_candidates();
    struct Row { const char *vendor, *name, *expect; };
    const Row rows[] = {
        {"Bambu Lab", "PLA Silk+", "Bambu PLA Silk+ @System"},
        {"Bambu Lab", "PLA Silk", "Bambu PLA Silk @System"},
        {"Bambu Lab", "PLA Tough+", "Bambu PLA Tough+ @System"},
        {"Bambu Lab", "PLA Tough", "Bambu PLA Tough @System"},
        {"Elegoo", "PLA+", "Elegoo PLA+ @System"},
        {"Elegoo", "PLA", "Elegoo PLA @System"},
        {"eSUN", "PLA+", "eSUN PLA+ @System"},
    };
    for (const Row &r : rows) {
        INFO(r.vendor << " " << r.name);
        auto x = choose_parent(fil(r.name, r.vendor, "PLA"), c, kU1);
        CHECK(chosen_name(x) == r.expect);
        CHECK(x.rule == "vendor + name + material");
        CHECK_FALSE(has_note(x, "'+'"));
    }
    // eSUN 'PLA' must NOT silently become 'eSUN PLA+': the Generic plain preset wins, no '+' mismatch was needed
    auto e = choose_parent(fil("PLA", "eSUN", "PLA"), c, kU1);
    CHECK(chosen_name(e) == "Generic PLA @U1 0.4 nozzle");
    CHECK(e.rule == "material alone");
    // when nothing else exists, the '+' mismatch is used but SAID
    std::vector<ParentCandidate> only = {cand("eSUN PLA+ @System", "PLA", "eSUN")};
    auto m = choose_parent(fil("PLA", "eSUN", "PLA"), only);
    CHECK(chosen_name(m) == "eSUN PLA+ @System");
    CHECK(has_note(m, "'+' marker differs"));
    // material 'PLA+' carries the marker (item: material normalisation)
    CHECK(chosen_name(choose_parent(fil("Red", "Elegoo", "PLA+"), c, kU1)) == "Elegoo PLA+ @System");
}

TEST_CASE("Spoolman parent: vendor from the name when the inherited vendor is Generic, and the Bambu alias", "[Spoolman]")
{
    // The inherited filament_vendor of these Snapmaker presets is Generic in some profile trees: the name's first word decides.
    std::vector<ParentCandidate> c = {cand("PLA Matte @System", "PLA", "Generic"), cand("Snapmaker PLA Glow @U1 0.4 nozzle", "PLA", "Generic"),
                                      cand("Snapmaker PETG Translucent @U1 0.4 nozzle", "PETG", "Generic"),
                                      cand("Generic PLA @U1 0.4 nozzle", "PLA", "Generic")};
    c[0].name = "Generic PLA Matte @System";
    CHECK(chosen_name(choose_parent(fil("PLA Glow Green", "Snapmaker", "PLA"), c, kU1)) == "Snapmaker PLA Glow @U1 0.4 nozzle");
    CHECK(choose_parent(fil("PLA Glow Green", "Snapmaker", "PLA"), c, kU1).rule == "vendor + name + material");
    CHECK(chosen_name(choose_parent(fil("PETG Translucent Blue", "Snapmaker", "PETG"), c, kU1)) == "Snapmaker PETG Translucent @U1 0.4 nozzle");
    // another vendor's Glow request does not take Snapmaker's preset
    CHECK(chosen_name(choose_parent(fil("PLA Glow Green", "YOUSU", "PLA"), c, kU1)) == "<none>");
    // alias: 'Bambu' (Spoolman) vs 'Bambu Lab' (profile vendor), both spellings
    const auto &real = real_candidates();
    CHECK(chosen_name(choose_parent(fil("PLA Basic", "Bambu", "PLA"), real, kU1)) == "Bambu PLA Basic @System");
    CHECK(chosen_name(choose_parent(fil("PLA Basic", "Bambu Lab", "PLA"), real, kU1)) == "Bambu PLA Basic @System");
}

TEST_CASE("Spoolman parent: stop-words and sizes never decide", "[Spoolman]")
{
    const auto &c = real_candidates();
    auto s = choose_parent(fil("PLA+ Filament 1.75mm", "SUNLU", "PLA"), c, kU1);
    CHECK(chosen_name(s) == "SUNLU PLA+ @System"); // not 'SUNLU PLA+ 2.0'
    CHECK(s.rule == "vendor + name + material");
    CHECK_FALSE(has_note(s, "material only"));
    CHECK(chosen_name(choose_parent(fil("PolyTerra PLA Filament", "Polymaker", "PLA"), c, kU1)) == "PolyTerra PLA @System");
    CHECK(chosen_name(choose_parent(fil("PLA+ 2.0 1kg Spool", "SUNLU", "PLA"), c, kU1)) == "SUNLU PLA+ 2.0 @System");
    CHECK(chosen_name(choose_parent(fil("Red 1.75mm 1kg", "NoSuchVendor", "PLA"), c, kU1)) == "Generic PLA @U1 0.4 nozzle");
    auto sw = stop_words();
    CHECK(std::find(sw.begin(), sw.end(), "filament") != sw.end());
}

TEST_CASE("Spoolman parent: material strings are normalised", "[Spoolman]")
{
    const auto &c = real_candidates();
    CHECK(chosen_name(choose_parent(fil("Gold", "NoSuchVendor", "Silk PLA"), c, kU1)) == "Generic PLA Silk @U1 0.4 nozzle");
    CHECK(chosen_name(choose_parent(fil("Gold", "NoSuchVendor", "PLA Silk"), c, kU1)) == "Generic PLA Silk @U1 0.4 nozzle");
    CHECK(chosen_name(choose_parent(fil("Gold", "Overture", "PLA Silk"), c, kU1)) == "Overture Silk PLA @System");
    CHECK(chosen_name(choose_parent(fil("Red", "Elegoo", "PLA+"), c, kU1)) == "Elegoo PLA+ @System");
}

TEST_CASE("Spoolman parent: an ambiguous setting_id returns none instead of falling through (pinned)", "[Spoolman]")
{
    // Deliberate: setting_id matching two candidates stops here. Falling through to the name rules would silently pick a preset the user's
    // own id did not name. A setting_id matching NOTHING does fall through (tested elsewhere).
    std::vector<ParentCandidate> c = {cand("Generic PETG @A", "PETG", "Generic", "dup"), cand("Generic PETG @B", "PETG", "Generic", "dup"),
                                      cand("Generic PETG HF @U1", "PETG", "Generic", "ok")};
    Filament f = fil("PETG HF Red", "", "PETG");
    f.extras["setting_id"] = "dup";
    auto r = choose_parent(f, c);
    CHECK(r.chosen == nullptr);
    CHECK(r.reason.rfind("ambiguous: ", 0) == 0);
    f.extras["setting_id"] = "missing";
    CHECK(chosen_name(choose_parent(f, c)) == "Generic PETG HF @U1");
}

TEST_CASE("Spoolman parent: external_id colour check compares whole words", "[Spoolman]")
{
    auto cands = realistic();
    Filament f = fil("Red PLA", "Sunlu", "PLA");
    f.external_id = "sunlu_pla_titan_tanned_1000";
    CHECK_FALSE(has_note(choose_parent(f, cands), "disagree")); // 'tan' is not inside 'titan' or 'tanned'
    f.external_id = "sunlu_pla_tan_1000";
    CHECK(has_note(choose_parent(f, cands), "disagree"));        // 'tan' as a whole word does
    f.external_id = "sunlu_pla_red_1000";
    CHECK_FALSE(has_note(choose_parent(f, cands), "disagree")); // same colour as the name
    f.external_id = "sunlu_pla_tailored_1000";
    CHECK_FALSE(has_note(choose_parent(f, cands), "disagree")); // 'red' inside 'tailored' is not a word
}

TEST_CASE("Spoolman parent: real-data sweep, 22 fixtures and invented names against the real candidates", "[Spoolman]")
{
    const auto &c = real_candidates();
    struct Row { const char *name, *vendor, *material, *expect, *rule; };
    const char *R3 = "vendor + name + material", *RF = "material family with flags", *RM = "material alone";
    // Expectations written by hand from the profile names (see the comments), not from the code's vocabularies.
    const Row invented[] = {
        {"PLA Silk+", "Bambu Lab", "PLA", "Bambu PLA Silk+ @System", R3},
        {"PLA Silk", "Bambu Lab", "PLA", "Bambu PLA Silk @System", R3},
        {"PLA Tough+", "Bambu Lab", "PLA", "Bambu PLA Tough+ @System", R3},
        {"PLA Tough", "Bambu Lab", "PLA", "Bambu PLA Tough @System", R3},
        {"PLA+", "Elegoo", "PLA", "Elegoo PLA+ @System", R3},
        {"PLA", "Elegoo", "PLA", "Elegoo PLA @System", R3},
        {"PLA", "eSUN", "PLA", "Generic PLA @U1 0.4 nozzle", RM},       // eSUN has only PLA+/Basic/Marble/Matte: plain goes to Generic
        {"PLA+", "eSUN", "PLA", "eSUN PLA+ @System", R3},
        {"PLA+ Filament 1.75mm", "SUNLU", "PLA", "SUNLU PLA+ @System", R3},
        {"PolyTerra PLA Filament", "Polymaker", "PLA", "PolyTerra PLA @System", R3},
        {"PLA Glow Green", "Snapmaker", "PLA", "Snapmaker PLA Glow @U1 0.4 nozzle", R3},
        {"PETG Translucent Blue", "Snapmaker", "PETG", "Snapmaker PETG Translucent @U1 0.4 nozzle", R3},
        {"Silk PLA Gold", "Inland", "PLA", "Generic PLA Silk @U1 0.4 nozzle", RF},
        {"PLA High Speed", "Inland", "PLA", "Generic PLA High Speed @U1 0.4 nozzle", RF},
        {"PLA Carbon Fibre Black", "Hatchbox", "PLA-CF", "Generic PLA-CF @U1 0.4 nozzle", RF},
        {"Carbon Fiber PETG", "Hatchbox", "PETG-CF", "Generic PETG-CF @U1 0.4 nozzle", RF},
        {"PLA-CF Black", "Snapmaker", "PLA-CF", "Snapmaker PLA-CF @U1 0.4 nozzle", R3},
        {"Red", "Hatchbox", "PLA", "Generic PLA @U1 0.4 nozzle", RM},
        {"ABS Red", "Hatchbox", "ABS", "Snapmaker ABS @U1 0.4 nozzle", RM}, // CHANGED (was Generic ABS @System): the printer vendor's U1 preset
        {"Matte PLA Red", "Overture", "PLA", "Overture Matte PLA @System", R3},
        {"Silk PLA Blue", "Overture", "PLA", "Overture Silk PLA @System", R3},
        {"PETG HF", "Elegoo", "PETG", "Elegoo PETG HF @System", R3},
        {"PETG HF", "Bambu Lab", "PETG", "Bambu PETG HF @System", R3},
        {"PLA Marble", "Hatchbox", "PLA", "Generic PLA @U1 0.4 nozzle", RM},   // unknown word 'marble' ignored in the family rule, noted
        {"PLA Basic", "Bambu", "PLA", "Bambu PLA Basic @System", R3},
        // deliberate none:
        {"PLA Basic", "Hatchbox", "PLA", "<none>", ""},        // Basic requested, no Generic Basic
        {"Natural", "Hatchbox", "Nylon", "<none>", ""},        // no Nylon candidate
        {"PETG Glow", "YOUSU", "PETG", "<none>", ""},          // no Glow PETG
        {"Teal", "Hatchbox", "PETG-GF", "<none>", ""},         // no Generic PETG-GF
        {"Black", "Hatchbox", "", "<none>", ""},               // no material
    };
    for (const Row &r : invented) {
        auto x = choose_parent(fil(r.name, r.vendor, r.material), c, kU1);
        std::cout << "[sweep] " << r.vendor << " | " << r.name << " | " << r.material << " -> " << chosen_name(x) << " ("
                  << (x.rule.empty() ? x.reason : x.rule) << ")" << std::endl;
        INFO(r.vendor << " | " << r.name << " | " << r.material);
        CHECK(chosen_name(x) == r.expect);
        CHECK(x.rule == r.rule);
        if (std::string(r.expect) == "<none>")
            CHECK_FALSE(x.reason.empty());
    }
    // the 22 real fixtures
    auto fils = fixture_filaments();
    struct Fx { int id; const char *expect; const char *rule; };
    const Fx fx[] = {
        {1,  "Generic PETG @U1 0.4 nozzle", RM},   // Inland PETG+ Black: no Inland preset, '+' mismatch to Generic (noted)
        {2,  "Generic PVA @System", RM},           // Polymaker PVA, no Polymaker PVA preset
        {3,  "SUNLU PETG @System", R3}, {4, "SUNLU PETG @System", R3}, {5, "SUNLU PETG @System", R3}, {6, "SUNLU PETG @System", R3},
        {7,  "Generic PETG @U1 0.4 nozzle", RM},   // CHCKX
        {8,  "Generic PETG @U1 0.4 nozzle", RM},   // Overture has no PETG preset
        {9,  "Generic PCTG @System", RM},
        {10, "<none>", ""}, {11, "<none>", ""}, {12, "<none>", ""}, {13, "<none>", ""}, // Glow PLA, no Generic Glow
        {14, "Snapmaker PETG HF @U1 0.4 nozzle", R3}, {15, "Snapmaker PETG HF @U1 0.4 nozzle", R3}, {16, "Snapmaker PETG HF @U1 0.4 nozzle", R3},
        {17, "Generic PLA Matte @System", RF}, {18, "Generic PLA Matte @System", RF}, {21, "Generic PLA Matte @System", RF}, // no Snapmaker Matte
        {19, "Generic TPU @U1 0.4 nozzle", RM},
        {20, "Generic PETG @U1 0.4 nozzle", RM},
        {22, "PolyLite ABS @System", R3},          // Polymaker PolyLite ABS; external_id says dark grey, name says light grey, name wins
    };
    CHECK(std::size(fx) == 22);
    for (const Fx &e : fx) {
        auto x = choose_parent(by_id(fils, e.id), c, kU1);
        std::cout << "[sweep] fixture " << e.id << " '" << by_id(fils, e.id).name << "' -> " << chosen_name(x) << " (" << (x.rule.empty() ? x.reason : x.rule) << ")" << std::endl;
        INFO("fixture " << e.id);
        CHECK(chosen_name(x) == e.expect);
        CHECK(x.rule == e.rule);
        if (std::string(e.expect) == "<none>")
            CHECK(x.reason.find("no compatible candidate") != std::string::npos);
    }
    CHECK(has_note(choose_parent(by_id(fils, 22), c, kU1), "disagree"));
    CHECK(has_note(choose_parent(by_id(fils, 1), c, kU1), "'+' marker differs"));
}

TEST_CASE("Spoolman parent: the token matches at the '@' suffix boundary", "[Spoolman]")
{
    std::vector<ParentCandidate> c = {cand("Generic PETG @U1 0.4 nozzle HF", "PETG", "Generic"), cand("Generic PETG @System", "PETG", "Generic"),
                                      cand("Generic PETG @U1 0.4 nozzle", "PETG", "Generic")};
    CHECK(chosen_name(choose_parent(fil("Blue", "", "PETG"), c, ParentOptions{"@U1 0.4 nozzle"})) == "Generic PETG @U1 0.4 nozzle");
    CHECK(chosen_name(choose_parent(fil("Blue", "", "PETG"), c, ParentOptions{"@U1 0.4 nozzle HF"})) == "Generic PETG @U1 0.4 nozzle HF");
    CHECK(chosen_name(choose_parent(fil("Blue", "", "PETG"), c, ParentOptions{"@System"})) == "Generic PETG @System");
    // a token that is only a prefix of a longer word does not match
    std::vector<ParentCandidate> d = {cand("Generic PETG @U1 0.4 nozzles", "PETG", "Generic"), cand("Generic PETG @System", "PETG", "Generic")};
    CHECK(choose_parent(fil("Blue", "", "PETG"), d, ParentOptions{"@U1 0.4 nozzle"}).chosen == nullptr);
}

TEST_CASE("Spoolman parent: digit-glued and functional flags never fall back to the plain preset", "[Spoolman]")
{
    const auto &c = real_candidates();
    struct Row { const char *name, *material, *expect; };
    const Row rows[] = {
        {"PLA CF20", "PLA", "Generic PLA-CF @U1 0.4 nozzle"},
        {"PETG-CF10", "PETG", "Generic PETG-CF @U1 0.4 nozzle"},
        {"ABS GF30", "ABS", "<none>"},       // the only ABS-GF preset is Bambu's, not Generic
        {"PA12 CF15", "PA", "Generic PA-CF @System"},
        {"PETG-ESD", "PETG", "<none>"},      // only vendor ESD presets exist
        {"PLA Carbon", "PLA", "Generic PLA-CF @U1 0.4 nozzle"},
        {"PETG Carbon", "PETG", "Generic PETG-CF @U1 0.4 nozzle"},
        {"PLA Carbon Black", "PLA", "Generic PLA @U1 0.4 nozzle"}, // Carbon attached to a colour stays a colour
        {"PC FR", "PC", "<none>"},
    };
    for (const Row &r : rows) {
        INFO(r.name << " / " << r.material);
        auto x = choose_parent(fil(r.name, "Hatchbox", r.material), c, kU1);
        CHECK(chosen_name(x) == r.expect);
    }
}

TEST_CASE("Spoolman parent: blends of several materials give none in the family rules", "[Spoolman]")
{
    const auto &c = real_candidates();
    struct Row { const char *vendor, *name, *material, *reason; };
    const Row rows[] = {
        {"Hatchbox", "PC-ABS", "PC", "blend: abs"},
        {"Hatchbox", "PLA PETG Blend", "PLA", "blend: petg"},
        {"Polymaker", "CoPA", "PA", "blend: copa"},
    };
    for (const Row &r : rows) {
        INFO(r.name);
        auto x = choose_parent(fil(r.name, r.vendor, r.material), c, kU1);
        CHECK(x.chosen == nullptr);
        CHECK(x.reason == r.reason);
    }
    // aesthetic or filler words stay lenient
    for (const char *n : {"PLA Marble", "PLA Wood"}) {
        auto x = choose_parent(fil(n, "Hatchbox", "PLA"), c, kU1);
        CHECK(chosen_name(x) == "Generic PLA @U1 0.4 nozzle");
        CHECK(has_note(x, "words ignored"));
    }
    // a vendor preset with exactly those words is still found by rule 3
    std::vector<ParentCandidate> v = {cand("Acme PC-ABS @System", "PC", "Acme")};
    CHECK(chosen_name(choose_parent(fil("PC-ABS", "Acme", "PC"), v)) == "Acme PC-ABS @System");
}

// #31x: a Polymaker ABS on a U1 must not inherit "Generic ABS @System" (empty compatible_printers: every printer, with that library's 105 C
// plate temperature, which the U1 rejects) while the printer vendor lists "Snapmaker ABS @U1 0.4 nozzle" for the printer.
TEST_CASE("Spoolman parent: a printer-listed vendor preset beats a Generic library preset listed for every printer", "[Spoolman]")
{
    Slic3r::Spoolman::Filament f;
    f.name = "PolyLite(TM) ABS Light Grey";
    f.material = "ABS";
    f.vendor = Slic3r::Spoolman::Vendor{};
    f.vendor->name = "Polymaker";
    auto listed = [](ParentCandidate c) { c.printer_listed = true; return c; };
    std::vector<ParentCandidate> c = {cand("Generic ABS @System", "ABS", "Generic"), listed(cand("Snapmaker ABS @U1 0.4 nozzle", "ABS", "Snapmaker"))};
    auto r = choose_parent(f, c);
    REQUIRE(r.chosen);
    CHECK(r.chosen->name == "Snapmaker ABS @U1 0.4 nozzle");
    CHECK(r.reason.find("printer vendor's, listed for the printer") != std::string::npos);
    // the other order gives the same answer
    std::reverse(c.begin(), c.end());
    CHECK(chosen_name(choose_parent(f, c)) == "Snapmaker ABS @U1 0.4 nozzle");
    // a Generic preset listed for the printer still beats the printer vendor's (existing behaviour, PETG/TPU)
    std::vector<ParentCandidate> g = {listed(cand("Snapmaker ABS @U1 0.4 nozzle", "ABS", "Snapmaker")), listed(cand("Generic ABS @U1 0.4 nozzle", "ABS", "Generic"))};
    CHECK(chosen_name(choose_parent(f, g)) == "Generic ABS @U1 0.4 nozzle");
    // two printer-vendor presets are still ambiguous
    std::vector<ParentCandidate> a = {listed(cand("Snapmaker ABS @U1 0.4 nozzle", "ABS", "Snapmaker")), listed(cand("Snapmaker ABS Pro @U1 0.4 nozzle", "ABS", "Snapmaker"))};
    a[1].name = "Snapmaker ABS @U1 0.4 nozzle B";
    CHECK_FALSE(choose_parent(f, a).chosen);
    // nothing listed: the old Generic tier
    std::vector<ParentCandidate> n = {cand("Generic ABS @System", "ABS", "Generic")};
    CHECK(chosen_name(choose_parent(f, n)) == "Generic ABS @System");
}

TEST_CASE("Spoolman parent: the preferred token comes from the data, not from a printer name list", "[Spoolman]")
{
    const auto &c = real_candidates();
    CHECK(preferred_token_for_printer("Snapmaker U1 (0.4 nozzle)", c) == "@U1 0.4 nozzle");
    CHECK(preferred_token_for_printer("Snapmaker U1 (0.6 nozzle)", c).find("0.4") == std::string::npos);
    CHECK(preferred_token_for_printer("Some Other Printer", c).empty());
    CHECK(preferred_token_for_printer("", c).empty());
}
