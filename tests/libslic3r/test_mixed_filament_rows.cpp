// Slice 2 of the mixed-engine port: the MixedFilament row type and its serialization.
//
// The golden strings (mixed_filament_golden.hpp) come from Snapmaker's own serializer. The round-trip tests
// therefore prove that this serializer and the slice-1 parser agree with that code on those strings; they do
// not prove that a real Snapmaker project reopens correctly, and they cannot catch a defect Snapmaker's
// serializer shares with its own loader. The normalisation tests at the bottom are expectations derived by
// reading Snapmaker's source, not generated from it.
#include <catch2/catch_all.hpp>

#include <clocale>
#include <limits>

#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/MixedFilamentLegacy.hpp"
#include "mixed_filament_golden.hpp"

using namespace Slic3r;
using namespace Slic3r::mixed_golden;

namespace {
struct Golden { const char *name; const std::string *text; size_t num_physical; };
}

TEST_CASE("Mixed filament rows: golden strings from Snapmaker's serializer round-trip byte for byte", "[MixedFilament][rows]")
{
    const Golden goldens[] = {
        { "auto3", &k_auto3, 3 },
        { "disabled_auto4", &k_disabled_auto4, 4 },
        { "gradient3", &k_gradient3, 3 },
        { "gradient3col3", &k_gradient3col3, 3 },
        { "offsets3", &k_offsets3, 3 },
        { "ext12", &k_ext12, 12 },
        { "custom12", &k_custom12, 12 },
        { "auto2", &k_auto2, 2 },
    };
    for (const Golden &g : goldens) {
        INFO(g.name);
        const std::vector<MixedFilament> rows = parse_mixed_filaments(*g.text, g.num_physical);
        REQUIRE(!rows.empty());
        CHECK(serialize_mixed_filaments(rows) == *g.text);
        // and the second generation is a fixed point
        CHECK(serialize_mixed_filaments(parse_mixed_filaments(serialize_mixed_filaments(rows), g.num_physical)) == *g.text);
    }
}

TEST_CASE("Mixed filament rows: a deleted row comes back disabled, as after a Snapmaker load and save", "[MixedFilament][rows]")
{
    // Snapmaker's loader forces enabled=false on a row with d1 (MixedFilament.cpp load_custom_entries), so
    // its own load-then-save writes e0,d1 where the fixture has e1,d1. Everything else is verbatim.
    std::string expected = k_deleted3;
    const size_t at = expected.find("1,3,1,0,50");
    REQUIRE(at != std::string::npos);
    expected.replace(at, 10, "1,3,0,0,50");
    const std::vector<MixedFilament> rows = parse_mixed_filaments(k_deleted3, 3);
    CHECK(serialize_mixed_filaments(rows) == expected);
    CHECK(serialize_mixed_filaments(parse_mixed_filaments(expected, 3)) == expected);
}

TEST_CASE("Mixed filament rows: a custom manual-pattern row round-trips with the percentage Snapmaker's loader derives", "[MixedFilament][rows]")
{
    // Snapmaker's serializer wrote mix 50 next to pattern "1212,2", but its loader (and slice 1's parser) re-derives
    // the percentage from the pattern: (1/2 + 1) / 2 = 75. The row re-serialises with 75; everything else is verbatim.
    const std::vector<MixedFilament> rows = parse_mixed_filaments(k_custom_pattern3, 3);
    REQUIRE(rows.size() == 4);
    CHECK(rows[3].manual_pattern == "1212,2");
    CHECK(rows[3].mix_b_percent == 75);
    std::string expected = k_custom_pattern3;
    const size_t at = expected.rfind(",1,1,50,");
    REQUIRE(at != std::string::npos);
    expected.replace(at, 8, ",1,1,75,");
    CHECK(serialize_mixed_filaments(rows) == expected);
}

TEST_CASE("Mixed filament rows: parsed fields land in the right members", "[MixedFilament][rows]")
{
    const std::vector<MixedFilament> rows = parse_mixed_filaments(k_gradient3col3, 3);
    REQUIRE(rows.size() == 4);
    const MixedFilament &m = rows[3];
    CHECK(m.component_a == 1);
    CHECK(m.component_b == 2);
    CHECK(m.custom);
    CHECK(!m.origin_auto);
    CHECK(m.mix_b_percent == 25);
    CHECK(m.gradient_component_ids == "123");
    CHECK(m.gradient_component_weights == "50/30/20");
    CHECK(m.distribution_mode == MixedFilament::LayerCycle);
    CHECK(m.stable_id == 4);

    const std::vector<MixedFilament> g = parse_mixed_filaments(k_gradient3, 3);
    CHECK(g[0].gradient_enabled);
    CHECK(g[0].gradient_start == Catch::Approx(0.8f));
    CHECK(g[0].gradient_end == Catch::Approx(0.2f));
    CHECK(g[0].ui_mode == 3);

    const std::vector<MixedFilament> o = parse_mixed_filaments(k_offsets3, 3);
    CHECK(o[0].local_z_max_sublayers == 4);
    CHECK(o[0].component_a_surface_offset == Catch::Approx(0.1f));
    CHECK(o[0].component_b_surface_offset == Catch::Approx(-0.2f));

    const std::vector<MixedFilament> d = parse_mixed_filaments(k_deleted3, 3);
    CHECK(d[1].deleted);
    CHECK(!d[1].enabled);
}

TEST_CASE("Mixed filament rows: ids above 9 use the slash encoding and a lone id gets a leading slash", "[MixedFilament][rows]")
{
    CHECK(encode_gradient_component_ids({ 1, 2, 3 }) == "123");
    CHECK(encode_gradient_component_ids({ 2, 11, 12 }) == "2/11/12");
    CHECK(encode_gradient_component_ids({ 12 }) == "/12");
    CHECK(encode_gradient_component_ids({}).empty());
    CHECK(decode_gradient_component_ids("/12") == std::vector<unsigned>{ 12 });
    CHECK(decode_gradient_component_ids("2/11/12") == std::vector<unsigned>{ 2, 11, 12 });
    CHECK(decode_gradient_component_ids("1123") == std::vector<unsigned>{ 1, 2, 3 }); // duplicates dropped
    CHECK(decode_gradient_component_ids("123", 2) == std::vector<unsigned>{ 1, 2 });   // validated against num_physical
    CHECK(normalize_gradient_component_ids("/5") == "5");
    CHECK(normalize_gradient_component_ids("1/2/3") == "123");
    // ids above the 64 cap are dropped by normalisation
    CHECK(normalize_gradient_component_ids("1/65/3") == "13");
}

TEST_CASE("Mixed filament rows: weights, patterns and offsets normalise as Snapmaker's source says", "[MixedFilament][rows]")
{
    CHECK(normalize_gradient_component_weights("1/1/1", 3) == "34/33/33");
    CHECK(normalize_gradient_component_weights("50/50", 3).empty()); // count mismatch
    CHECK(normalize_gradient_component_weights("0/0/0", 3).empty());
    CHECK(normalize_gradient_component_weights("5", 0).empty());

    CHECK(normalize_manual_pattern("1212") == "1212");
    CHECK(normalize_manual_pattern("1,2") == "1,2");
    CHECK(normalize_manual_pattern("[5]1") == "51");
    CHECK(normalize_manual_pattern("[10]1") == "[10]1");
    CHECK(normalize_manual_pattern("[010]").empty());
    CHECK(normalize_manual_pattern(",1").empty());
    CHECK(normalize_manual_pattern("1,").empty());
    CHECK(normalize_manual_pattern("x").empty());

    CHECK(format_surface_offset_token(0.f) == "0");
    CHECK(format_surface_offset_token(-0.f) == "0");
    CHECK(format_surface_offset_token(0.1f) == "0.1");
    CHECK(format_surface_offset_token(-0.2f) == "-0.2");
    CHECK(format_surface_offset_token(5.f) == "2");   // clamped to +-2
    CHECK(format_surface_offset_token(-5.f) == "-2");
}

TEST_CASE("Mixed filament rows: serialization normalises and fills stable ids", "[MixedFilament][rows]")
{
    MixedFilament a;                      // stable id 0, defaults, no ui mode
    a.component_a = 1; a.component_b = 2; a.custom = true;
    MixedFilament b = a;
    b.stable_id = 7; b.component_b = 3;
    b.pointillism_all_filaments = true;   // forced off
    b.distribution_mode = MixedFilament::SameLayerPointillisme; // no 3-colour gradient: becomes Simple
    b.mix_b_percent = 250;                // clamped
    b.local_z_max_sublayers = -3;         // clamped
    b.gradient_component_ids = "1/2/3";   // canonical "123"
    b.gradient_component_weights = "2/1/1";
    MixedFilament c = a;                  // another id 0 row: continues above the highest id seen (7)
    c.component_b = 4;
    c.ui_mode = 2;
    c.manual_pattern = "[10]1";
    c.gradient_enabled = true; c.gradient_start = 0.75f; c.gradient_end = 0.25f;

    CHECK(serialize_mixed_filaments({ a, b, c }) ==
          "1,2,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u1;"
          "1,3,1,1,100,0,g123,w50/25/25,m0,z0,xa0,xb0,d0,o0,u7;"
          "1,4,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u8,cm2,r1/0.7500/0.2500,[10]1");
    CHECK(serialize_mixed_filaments({}).empty());
}

TEST_CASE("Mixed filament rows: serialization ignores the process locale", "[MixedFilament][rows]")
{
    MixedFilament m;
    m.custom = true; m.component_a_surface_offset = 0.5f; m.gradient_enabled = true;
    const std::string expected = serialize_mixed_filaments({ m });
    const char *old = std::setlocale(LC_NUMERIC, nullptr);
    const std::string saved = old ? old : "C";
    for (const char *loc : { "de_DE.UTF-8", "fr_FR.UTF-8" })
        if (std::setlocale(LC_NUMERIC, loc))
            break;
    const std::string under = serialize_mixed_filaments({ m });
    std::setlocale(LC_NUMERIC, saved.c_str());
    CHECK(under == expected);
    CHECK(expected.find("xa0.5,") != std::string::npos);
    CHECK(expected.find("r1/0.8000/0.2000") != std::string::npos);
}

TEST_CASE("Mixed filament rows: equality tolerates float noise only", "[MixedFilament][rows]")
{
    MixedFilament a, b;
    CHECK(a == b);
    b.gradient_start += 1e-5f;
    CHECK(a == b);
    b.gradient_start += 1e-2f;
    CHECK(a != b);
    b = a;
    b.stable_id = 9;
    CHECK(a != b);
}

TEST_CASE("Mixed filament rows: rows the parser rejects are absent, not half-parsed", "[MixedFilament][rows]")
{
    CHECK(parse_mixed_filaments("", 3).empty());
    CHECK(parse_mixed_filaments("1,1,1,1,50", 3).empty());  // a == b
    CHECK(parse_mixed_filaments("1,9,1,1,50", 3).empty());  // id above physical count
    CHECK(parse_mixed_filaments("garbage", 3).empty());
    CHECK(parse_mixed_filaments(k_auto3, 1).empty());       // fewer than two physical filaments
}

TEST_CASE("Mixed filament rows: hostile numeric tokens neither wrap nor overflow", "[MixedFilament][rows][security]")
{
    // an id that would wrap to 1 (or 2) through a 32-bit truncation is not an id
    CHECK(decode_gradient_component_ids("4294967297/2/3").size() == 2);
    CHECK(decode_gradient_component_ids("4294967297/2/3") == std::vector<unsigned>{ 2, 3 });
    CHECK(decode_gradient_component_ids("99999999999999999999/2") == std::vector<unsigned>{ 2 });
    CHECK(decode_gradient_component_ids("-1/2") == std::vector<unsigned>{ 2 });
    CHECK(normalize_gradient_component_ids("4294967297/2").find('1') == std::string::npos);

    // weights: out-of-range tokens make the whole list invalid rather than UB
    CHECK(normalize_gradient_component_weights("99999999999999999999/1/1", 3).empty());
    CHECK(normalize_gradient_component_weights("2147483648/1/1", 3).empty());
    CHECK(normalize_gradient_component_weights("-5/1/1", 3) == "72/14/14"); // '-' is a separator: tokens 5,1,1
    // letters are separators, so "1e999" is the tokens 1 and 999
    CHECK(normalize_gradient_component_weights("nan/inf/1e999", 3).empty());               // no digit tokens at all: count mismatch
    CHECK(normalize_gradient_component_weights("1e999/5/5", 4) == "0/99/1/0");             // tokens 1, 999, 5, 5 (largest-remainder rounding)
    CHECK(normalize_gradient_component_weights("1e999999/5/5", 4).empty());                // 999999 is over-long: the over-long path
    // huge but accepted weights are clamped to 10000 and the sum cannot overflow
    CHECK(normalize_gradient_component_weights("99999/99999/99999", 3) == "34/33/33");
    CHECK(normalize_gradient_component_weights("10000/10000/10000", 3) == "34/33/33");

    // thousands of weights: bounded work, empty result
    std::string many;
    for (int i = 0; i < 20000; ++i)
        many += (i ? "/1" : "1");
    CHECK(normalize_gradient_component_weights(many, 3).empty());
    CHECK(normalize_gradient_component_weights(many, 20000).empty());
    CHECK(decode_gradient_component_ids(many).size() == 1);

    // the slice-1 parser: the same tokens in a row do not wrap into valid ids, and a negative stable id is ignored
    {
        const std::vector<MixedFilament> wrap = parse_mixed_filaments("1,2,1,1,50,0,g4294967297/2,w,m2,z0,xa0,xb0,d0,o0,u1", 3);
        REQUIRE(wrap.size() == 1);
        // the over-long id is dropped, not wrapped to 1: only id 2 survives normalisation
        CHECK(decode_gradient_component_ids(wrap[0].gradient_component_ids, 3) == std::vector<unsigned>{ 2 });
    }
    CHECK(serialize_mixed_filaments(parse_mixed_filaments("1,2,1,1,50,0,g4294967297/2,w,m2,z0,xa0,xb0,d0,o0,u1", 3)).find("g4294967297") == std::string::npos);
    const std::vector<MixedFilament> r = parse_mixed_filaments("1,2,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u-1", 3);
    REQUIRE(r.size() == 1);
    CHECK(r[0].stable_id == 0);
}

TEST_CASE("Mixed filament rows: a stable id at the 64-bit limit does not wrap the id counter", "[MixedFilament][rows][security]")
{
    MixedFilament top;
    top.stable_id = std::numeric_limits<uint64_t>::max();
    MixedFilament fresh;           // stable_id 0: needs a new id
    const std::string s = serialize_mixed_filaments({ top, fresh });
    // the counter would wrap to 0 and hand the second row id 0 (which means "unassigned"); it restarts at 1 instead
    CHECK(s.find(",u18446744073709551615;") != std::string::npos);
    CHECK(s.substr(s.rfind(",u") + 2) == "1");
    CHECK(s.rfind(",u0") == std::string::npos);
}
