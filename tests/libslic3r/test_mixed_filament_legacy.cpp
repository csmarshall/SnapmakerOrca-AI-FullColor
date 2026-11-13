// Import of Snapmaker "mixed_filament_definitions" into this build's per-slot mixed model.
//
// The golden strings below were produced by Snapmaker's own MixedFilamentManager::serialize_custom_entries
// (built from the 3-snapmaker-main worktree, linked against its libslic3r, 2026-11-12). The tests therefore
// prove that the parser agrees with Snapmaker's serializer and loader rules; they do not prove that a
// real Snapmaker project prints the same here. That needs a Snapmaker-authored 3MF sliced in both.
#include <catch2/catch_all.hpp>

#include <algorithm>
#include <functional>
#include <nlohmann/json.hpp>

#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/MixedFilamentLegacy.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Zipper.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "test_utils.hpp"

using namespace Slic3r;

namespace {

const std::string k_auto3 =
    "1,2,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u1;"
    "1,3,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u2;"
    "2,3,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u3";
const std::string k_disabled_auto4 =
    "1,2,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u1;"
    "1,3,0,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u2;"
    "1,4,1,0,30,0,g,w,m2,z0,xa0,xb0,d0,o1,u3;"
    "2,3,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u4;"
    "2,4,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u5;"
    "3,4,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u6";
const std::string k_deleted3 =
    "1,2,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u1;"
    "1,3,1,0,50,0,g,w,m2,z0,xa0,xb0,d1,o1,u2;"
    "2,3,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u3";
const std::string k_custom_pattern3 =
    "1,2,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u1;"
    "1,3,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u2;"
    "2,3,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u3;"
    "1,3,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u4,1212,2";
const std::string k_gradient3 =
    "1,2,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u1,cm3,r1/0.8000/0.2000;"
    "1,3,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u2;"
    "2,3,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u3";
const std::string k_gradient3col3 =
    "1,2,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u1;"
    "1,3,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u2;"
    "2,3,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u3;"
    "1,2,1,1,25,0,g123,w50/30/20,m0,z0,xa0,xb0,d0,o0,u4";
const std::string k_offsets3 =
    "1,2,1,0,50,0,g,w,m2,z4,xa0.1,xb-0.2,d0,o1,u1;"
    "1,3,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u2;"
    "2,3,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u3";
const std::string k_ext12 =
    "1,2,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u1;"
    "1,3,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u2;"
    "1,4,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u3;"
    "1,5,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u4;"
    "1,6,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u5;"
    "1,7,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u6;"
    "1,8,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u7;"
    "1,9,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u8;"
    "1,10,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u9;"
    "1,11,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u10;"
    "1,12,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u11;"
    "2,3,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u12;"
    "2,4,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u13;"
    "2,5,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u14;"
    "2,6,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u15;"
    "2,7,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u16;"
    "2,8,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u17;"
    "2,9,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u18;"
    "2,10,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u19;"
    "2,11,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u20;"
    "2,12,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u21;"
    "3,4,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u22;"
    "3,5,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u23;"
    "3,6,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u24;"
    "3,7,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u25;"
    "3,8,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u26;"
    "3,9,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u27;"
    "3,10,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u28;"
    "3,11,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u29;"
    "3,12,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u30;"
    "4,5,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u31;"
    "4,6,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u32;"
    "4,7,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u33;"
    "4,8,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u34;"
    "4,9,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u35;"
    "4,10,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u36;"
    "4,11,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u37;"
    "4,12,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u38;"
    "5,6,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u39;"
    "5,7,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u40;"
    "5,8,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u41;"
    "5,9,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u42;"
    "5,10,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u43;"
    "5,11,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u44;"
    "5,12,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u45;"
    "6,7,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u46;"
    "6,8,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u47;"
    "6,9,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u48;"
    "6,10,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u49;"
    "6,11,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u50;"
    "6,12,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u51;"
    "7,8,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u52;"
    "7,9,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u53;"
    "7,10,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u54;"
    "7,11,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u55;"
    "7,12,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u56;"
    "8,9,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u57;"
    "8,10,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u58;"
    "8,11,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u59;"
    "8,12,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u60;"
    "9,10,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u61;"
    "9,11,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u62;"
    "9,12,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u63;"
    "10,11,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u64;"
    "10,12,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u65;"
    "11,12,1,0,50,0,g2/11/12,w,m2,z0,xa0,xb0,d0,o1,u66";
const std::string k_custom12 =
    "10,12,1,1,40,0,g/10,w,m2,z0,xa0,xb0,d0,o0,u1;"
    "2,11,1,1,60,0,g2/11/12,w,m2,z0,xa0,xb0,d0,o0,u2;"
    "3,4,0,1,20,0,g,w,m2,z0,xa0,xb0,d0,o0,u3;"
    "11,12,1,1,75,0,g,w,m2,z0,xa0,xb0,d0,o0,u4";
const std::string k_auto2 =
    "1,2,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u1";

// Snapmaker's rule (MixedFilamentManager::mixed_index_from_filament_id): the virtual id of a row is
// num_physical + 1 + the number of earlier rows that are enabled and not deleted.
std::vector<unsigned> snapmaker_virtual_ids(const LegacyMixedParse &p, size_t n)
{
    std::vector<unsigned> ids;
    unsigned next = unsigned(n) + 1;
    for (const LegacyMixedRow &r : p.rows)
        ids.push_back((r.enabled && !r.deleted) ? next++ : 0);
    return ids;
}

DynamicPrintConfig project_config(size_t n)
{
    DynamicPrintConfig c = DynamicPrintConfig::full_print_config();
    std::vector<std::string> cols, ids;
    static const char *palette[] = { "#FF0000", "#00FF00", "#0000FF", "#FFFF00", "#00FFFF", "#FF00FF",
                                     "#101010", "#202020", "#303030", "#404040", "#505050", "#606060" };
    for (size_t i = 0; i < n; ++i) {
        cols.push_back(palette[i % 12]);
        ids.push_back("Preset" + std::to_string(i + 1));
    }
    c.set_key_value("filament_colour", new ConfigOptionStrings(cols));
    c.set_key_value("filament_multi_colour", new ConfigOptionStrings(cols));
    c.set_key_value("filament_colour_type", new ConfigOptionStrings(std::vector<std::string>(n, "1")));
    c.set_key_value("filament_settings_id", new ConfigOptionStrings(ids));
    c.set_key_value("filament_map", new ConfigOptionInts(std::vector<int>(n, 1)));
    return c;
}

bool rewrite_entries(const std::string &path, const std::function<void(const std::string &name, std::string &data)> &edit)
{
    std::vector<std::pair<std::string, std::string>> entries;
    {
        mz_zip_archive zip;
        mz_zip_zero_struct(&zip);
        REQUIRE(open_zip_reader(&zip, path));
        for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); ++i) {
            mz_zip_archive_file_stat st;
            REQUIRE(mz_zip_reader_file_stat(&zip, i, &st));
            if (st.m_is_directory) continue;
            std::string name(st.m_filename);
            std::replace(name.begin(), name.end(), '\\', '/');
            std::string data((size_t) st.m_uncomp_size, '\0');
            if (st.m_uncomp_size > 0)
                REQUIRE(mz_zip_reader_extract_to_mem(&zip, i, data.data(), data.size(), 0));
            edit(name, data);
            entries.emplace_back(std::move(name), std::move(data));
        }
        close_zip_reader(&zip);
    }
    Zipper z(path);
    for (const auto &e : entries)
        z.add_entry(e.first, e.second.data(), e.second.size());
    z.finalize();
    return true;
}

} // namespace

TEST_CASE("Legacy mixed rows: golden strings from Snapmaker's serializer parse to the same fields", "[MixedFilament][legacy]")
{
    SECTION("3 physical, all auto rows") {
        const LegacyMixedParse p = parse_legacy_mixed_definitions(k_auto3, 3);
        REQUIRE(p.rows.size() == 3);
        CHECK(p.auto_generated);
        CHECK(p.skipped == 0);
        const unsigned pairs[3][2] = { { 1, 2 }, { 1, 3 }, { 2, 3 } };
        for (size_t i = 0; i < 3; ++i) {
            CHECK(p.rows[i].a == pairs[i][0]);
            CHECK(p.rows[i].b == pairs[i][1]);
            CHECK(p.rows[i].enabled);
            CHECK_FALSE(p.rows[i].custom);
            CHECK(p.rows[i].stable_id == i + 1);
            CHECK(p.rows[i].mix_b_percent == 50);
        }
    }
    SECTION("disabled auto row and a 30 percent mix") {
        const LegacyMixedParse p = parse_legacy_mixed_definitions(k_disabled_auto4, 4);
        REQUIRE(p.rows.size() == 6);
        CHECK_FALSE(p.rows[1].enabled);
        CHECK(p.rows[2].mix_b_percent == 30);
        CHECK(p.rows[2].a == 1);
        CHECK(p.rows[2].b == 4);
    }
    SECTION("deleted row is also disabled") {
        const LegacyMixedParse p = parse_legacy_mixed_definitions(k_deleted3, 3);
        REQUIRE(p.rows.size() == 3);
        CHECK(p.rows[1].deleted);
        CHECK_FALSE(p.rows[1].enabled);
    }
    SECTION("custom row with a manual pattern: mix comes from the pattern") {
        const LegacyMixedParse p = parse_legacy_mixed_definitions(k_custom_pattern3, 3);
        REQUIRE(p.rows.size() == 4);
        const LegacyMixedRow &r = p.rows.back();
        CHECK(r.custom);
        CHECK_FALSE(r.origin_auto);
        CHECK(r.manual_pattern == "1212,2");
        CHECK(r.mix_b_percent == 75); // groups "1212" -> 50 percent b, "2" -> 100 percent b
        CHECK(r.stable_id == 4);
    }
    SECTION("gradient row r1/start/end and ui mode") {
        const LegacyMixedParse p = parse_legacy_mixed_definitions(k_gradient3, 3);
        REQUIRE(p.rows.size() == 3);
        CHECK(p.rows[0].gradient_enabled);
        CHECK(p.rows[0].gradient_start == Catch::Approx(0.8f));
        CHECK(p.rows[0].gradient_end == Catch::Approx(0.2f));
        CHECK(p.rows[0].ui_mode == 3);
        CHECK_FALSE(p.rows[1].gradient_enabled);
    }
    SECTION("3 colour gradient ids, weights and layer-cycle mode") {
        const LegacyMixedParse p = parse_legacy_mixed_definitions(k_gradient3col3, 3);
        REQUIRE(p.rows.size() == 4);
        CHECK(p.rows[3].gradient_component_ids == "123");
        CHECK(p.rows[3].gradient_component_weights == "50/30/20");
        CHECK(p.rows[3].distribution_mode == 0);
        CHECK(p.rows[3].mix_b_percent == 25);
    }
    SECTION("surface offsets and Local-Z cap") {
        const LegacyMixedParse p = parse_legacy_mixed_definitions(k_offsets3, 3);
        REQUIRE(p.rows.size() == 3);
        CHECK(p.rows[0].local_z_max_sublayers == 4);
        CHECK(p.rows[0].surface_offset_a == Catch::Approx(0.1f));
        CHECK(p.rows[0].surface_offset_b == Catch::Approx(-0.2f));
    }
    SECTION("12 physical: ids above 9, custom rows only (auto-generate was off)") {
        const LegacyMixedParse p = parse_legacy_mixed_definitions(k_custom12, 12);
        CHECK_FALSE(p.auto_generated);
        REQUIRE(p.rows.size() == 4);
        CHECK(p.rows[0].a == 10);
        CHECK(p.rows[0].b == 12);
        CHECK(p.rows[0].gradient_component_ids == "/10");
        CHECK(p.rows[1].gradient_component_ids == "2/11/12");
        CHECK_FALSE(p.rows[2].enabled);
        CHECK(p.rows[3].mix_b_percent == 75);
    }
    SECTION("a lone auto row means auto-generate was on: missing pairs are appended, enabled") {
        const LegacyMixedParse p = parse_legacy_mixed_definitions(k_auto2 , 3);
        REQUIRE(p.rows.size() == 3);
        CHECK(p.auto_generated);
        CHECK(p.rows[1].a == 1);
        CHECK(p.rows[1].b == 3);
        CHECK(p.rows[2].a == 2);
        CHECK(p.rows[2].b == 3);
        CHECK(p.rows[2].enabled);
    }
}

TEST_CASE("Legacy mixed rows: ids align with Snapmaker's numbering when a row is disabled", "[MixedFilament][legacy]")
{
    // 4 physical, rows A(enabled) B(disabled) C(enabled): converted slots are 5 and 6.
    const std::string defs = "1,2,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u1;"
                             "1,3,0,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u2;"
                             "2,4,1,1,20,0,g,w,m2,z0,xa0,xb0,d0,o0,u3";
    const LegacyMixedParse p = parse_legacy_mixed_definitions(defs, 4);
    const std::vector<unsigned> ids = snapmaker_virtual_ids(p, 4);
    REQUIRE(ids == std::vector<unsigned>({ 5, 0, 6 }));

    DynamicPrintConfig c = project_config(4);
    const LegacyMixedConversion conv = convert_legacy_mixed_filaments(c, defs);
    CHECK(conv.slots_added == 2);
    CHECK(c.option<ConfigOptionStrings>("filament_colour")->values.size() == 6);
    CHECK(c.option<ConfigOptionStrings>("filament_settings_id")->values.size() == 6);
    const auto &is_mixed = c.option<ConfigOptionBools>("filament_is_mixed")->values;
    REQUIRE(is_mixed.size() == 6);
    CHECK(is_mixed == std::vector<unsigned char>({ 0, 0, 0, 0, 1, 1 }));
    const auto &comps = c.option<ConfigOptionStrings>("filament_mixed_components")->values;
    CHECK(comps[4] == "1,2");
    CHECK(comps[5] == "2,4"); // row C (the third row) is slot 6
    CHECK(c.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios")->values[5] == "0.8000,0.2000");
    // the new slot takes component A's preset
    CHECK(c.option<ConfigOptionStrings>("filament_settings_id")->values[5] == "Preset2");
    CHECK(conv.warnings.empty());
}

TEST_CASE("Legacy mixed rows: rejected and malformed rows", "[MixedFilament][legacy]")
{
    const std::string bad =
        "2,2,1,1,50;"                                  // a == b
        "1,9,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u2;"     // id above the physical count
        "banana;"                                       // malformed
        "1,2;"                                          // too few tokens
        "1,2,1,1,50,0,g1/2/7,w,m2,z0,xa0,xb0,d0,o0,u3;" // gradient id above the count
        "1,2,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u4";      // the only good row
    const LegacyMixedParse p = parse_legacy_mixed_definitions(bad, 3);
    CHECK(p.parsed == 6);
    CHECK(p.skipped == 5);
    REQUIRE(p.rows.size() == 1);
    CHECK(p.rows[0].stable_id == 4);

    LegacyMixedRow r;
    CHECK_FALSE(parse_legacy_mixed_row("0,1,1,1,50", r));
    CHECK_FALSE(parse_legacy_mixed_row("1,x,1,1,50", r));
    CHECK(parse_legacy_mixed_row("1,2,1,50", r)); // legacy 4-token form
    CHECK(r.custom);
    CHECK(r.mix_b_percent == 50);

    DynamicPrintConfig c = project_config(3);
    const LegacyMixedConversion conv = convert_legacy_mixed_filaments(c, "garbage;;also garbage");
    CHECK(conv.slots_added == 0);
    CHECK(conv.rows_skipped == 2);
    CHECK_FALSE(conv.warnings.empty());
    CHECK(c.option<ConfigOptionStrings>("filament_colour")->values.size() == 3);
}

TEST_CASE("Legacy mixed rows: unrepresentable rows still get a slot and a warning", "[MixedFilament][legacy]")
{
    DynamicPrintConfig c = project_config(3);
    const std::string defs =
        "1,3,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u1,1212,2;"        // manual pattern
        "1,2,1,1,25,0,g123,w50/30/20,m0,z0,xa0,xb0,d0,o0,u2;"     // 3 colour gradient
        "2,3,1,1,50,0,g,w,m2,z4,xa0.1,xb-0.2,d0,o0,u3;"           // Local-Z cap and offsets
        "1,2,1,1,40,1,g,w,m2,z0,xa0,xb0,d0,o0,u4;"                // pointillism flag
        "1,2,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u5,r1/0.9000/0.1000"; // representable gradient
    const LegacyMixedConversion conv = convert_legacy_mixed_filaments(c, defs);
    REQUIRE(conv.slots_added == 5);
    CHECK(conv.warnings.size() == 4);
    const auto &comps = c.option<ConfigOptionStrings>("filament_mixed_components")->values;
    CHECK(comps[3] == "1,3");
    CHECK(c.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios")->values[3] == "0.2500,0.7500"); // pattern mean
    CHECK(comps[4] == "1,2");
    CHECK(c.option<ConfigOptionBools>("filament_mixed_gradient")->values[7] == 1);
    CHECK(c.option<ConfigOptionStrings>("filament_mixed_gradient_range")->values[7] == "0.9000,0.1000");
    CHECK(c.option<ConfigOptionBools>("filament_mixed_gradient")->values[6] == 0);
}

TEST_CASE("Legacy mixed rows: a project that already has 2.5 mixed slots keeps them", "[MixedFilament][legacy]")
{
    DynamicPrintConfig c = project_config(3);
    c.set_key_value("filament_is_mixed", new ConfigOptionBools({ false, false, true }));
    const LegacyMixedConversion conv = convert_legacy_mixed_filaments(c, k_auto3);
    CHECK(conv.slots_added == 0);
    CHECK(conv.warnings.size() == 1);
    CHECK(c.option<ConfigOptionStrings>("filament_colour")->values.size() == 3);
}

TEST_CASE("Legacy mixed rows: the definitions are converted at 3MF load, and not written back", "[3mf][MixedFilament][legacy]")
{
    Model model;
    REQUIRE(load_stl((std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl").c_str(), &model));
    model.add_default_instances();
    ScopedTemporaryDir backup_dir("orca_legacy_mixed_src");
    model.set_backup_path(backup_dir.string());
    model.objects.front()->config.set_key_value("extruder", new ConfigOptionInt(6));

    DynamicPrintConfig config = project_config(4);
    const std::string defs = "1,2,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u1;"
                             "1,3,0,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u2;"
                             "2,4,1,1,20,0,g,w,m2,z0,xa0,xb0,d0,o0,u3";

    auto store_and_load = [&](const std::string &definitions, const DynamicPrintConfig &cfg, DynamicPrintConfig &dst_config, Model &dst_model, std::string *saved_settings = nullptr) {
        ScopedTemporaryFile temp(".3mf");
        PlateData *plate = new PlateData();
        plate->plate_index = 0;
        StoreParams sp;
        sp.path     = temp.string().c_str();
        sp.model    = &model;
        sp.config   = const_cast<DynamicPrintConfig *>(&cfg);
        sp.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
        sp.plate_data_list.push_back(plate);
        REQUIRE(store_bbs_3mf(sp));
        delete plate;
        if (!definitions.empty())
            rewrite_entries(temp.string(), [&](const std::string &name, std::string &data) {
                if (name == "Metadata/project_settings.config") {
                    nlohmann::json j = nlohmann::json::parse(data);
                    j["mixed_filament_definitions"] = definitions;
                    data = j.dump();
                }
            });
        ScopedTemporaryDir dst_backup("orca_legacy_mixed_dst");
        dst_model.set_backup_path(dst_backup.string());
        ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Enable };
        PlateDataPtrs        plates;
        std::vector<Preset*> presets;
        bool b1 = false, b2 = false;
        Semver v;
        REQUIRE(load_bbs_3mf(temp.string().c_str(), &dst_config, &ctxt, &dst_model, &plates, &presets, &b1, &b2, &v, nullptr,
                             LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
        // resave and look for the Snapmaker key: one-way import
        if (saved_settings != nullptr) {
            ScopedTemporaryFile again(".3mf");
            PlateData *p2 = new PlateData();
            p2->plate_index = 0;
            StoreParams s2;
            s2.path     = again.string().c_str();
            s2.model    = &dst_model;
            s2.config   = &dst_config;
            s2.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
            s2.plate_data_list.push_back(p2);
            REQUIRE(store_bbs_3mf(s2));
            delete p2;
            rewrite_entries(again.string(), [&](const std::string &name, std::string &data) {
                if (name == "Metadata/project_settings.config")
                    *saved_settings = data;
            });
        }
        release_PlateData_list(plates);
    };

    SECTION("Snapmaker rows become slots 5 and 6 and the object's extruder 6 survives") {
        Model dst_model;
        DynamicPrintConfig dst;
        std::string saved;
        store_and_load(defs, config, dst, dst_model, &saved);
        CHECK(dst.option<ConfigOptionStrings>("filament_colour")->values.size() == 6);
        CHECK(dst.option<ConfigOptionBools>("filament_is_mixed")->values == std::vector<unsigned char>({ 0, 0, 0, 0, 1, 1 }));
        REQUIRE(dst_model.objects.size() == 1);
        CHECK(dst_model.objects.front()->config.opt_int("extruder") == 6);
        CHECK(saved.find("mixed_filament_definitions") == std::string::npos);
    }
    SECTION("without definitions the import is unchanged and an out-of-range extruder is still reset") {
        Model dst_model;
        DynamicPrintConfig dst;
        store_and_load("", config, dst, dst_model);
        CHECK(dst.option<ConfigOptionStrings>("filament_colour")->values.size() == 4);
        CHECK(dst_model.objects.front()->config.opt_int("extruder") == 1);
    }
    SECTION("an upstream 2.5 mixed project is untouched even when definitions are present") {
        DynamicPrintConfig cfg = project_config(5);
        cfg.set_key_value("filament_is_mixed", new ConfigOptionBools({ false, false, false, false, true }));
        cfg.set_key_value("filament_mixed_components", new ConfigOptionStrings({ "", "", "", "", "3,2" }));
        cfg.set_key_value("filament_mixed_sublayer_ratios", new ConfigOptionStrings({ "", "", "", "", "0.4200,0.5800" }));
        Model dst_model;
        DynamicPrintConfig dst;
        store_and_load(defs, cfg, dst, dst_model);
        CHECK(dst.option<ConfigOptionStrings>("filament_colour")->values.size() == 5);
        CHECK(dst.option<ConfigOptionStrings>("filament_mixed_components")->values[4] == "3,2");
        CHECK(dst.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios")->values[4] == "0.4200,0.5800");
    }
}
