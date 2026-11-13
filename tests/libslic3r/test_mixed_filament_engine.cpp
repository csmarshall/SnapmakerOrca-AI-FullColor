// Slice 3 of the mixed-engine port: the resolution half of Snapmaker's MixedFilamentManager.
//
// What the goldens are: mixed_engine_golden.hpp holds an FNV-1a hash and length of a transcript (every probe of
// resolve / resolve_perimeter / effective_painted_region_filament_id / component_surface_offset /
// ordered_perimeter_extruders over layers -3..59, for each scenario) produced by a scratch harness that compiled
// Snapmaker's own MixedFilament.cpp (3-snapmaker-main a00dd4fbac) and drove it through load_custom_entries +
// apply_gradient_settings. The test runs the SAME transcript() template over this tree's rows (slice-1 parser) and
// engine. A pass therefore proves agreement with Snapmaker's code on these scenarios; it does not prove the
// behaviour is right, nor that a print looks right, and it cannot catch a defect Snapmaker shares. The other
// cases are expectations derived by reading Snapmaker's source, or bound / safety checks with no Snapmaker side.
#include <catch2/catch_all.hpp>

#include <climits>
#include <cmath>
#include <limits>

#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/MixedFilamentEngine.hpp"
#include "mixed_engine_golden.hpp"
#include "mixed_engine_scenarios.hpp"

using namespace Slic3r;

namespace {
uint64_t fnv1a(const std::string &t)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : t) { h ^= c; h *= 1099511628211ull; }
    return h;
}

MixedFilamentEngine engine_for(const std::string &rows, size_t np, int mode = 0, float lo = 0.04f, float hi = 0.16f, bool adv = false)
{
    MixedFilamentEngine e(parse_mixed_filaments(rows, np));
    e.apply_gradient_settings(mode, lo, hi, adv);
    return e;
}

MixedFilament row(unsigned a, unsigned b, bool custom = true, int pct = 50)
{
    MixedFilament mf;
    mf.component_a = a;
    mf.component_b = b;
    mf.custom      = custom;
    mf.mix_b_percent = pct;
    return mf;
}
}

TEST_CASE("Mixed engine: transcripts match Snapmaker's manager (agreement, not correctness)", "[MixedFilament][engine][golden]")
{
    const auto scns = mixed_engine_scn::scenarios();
    REQUIRE(scns.size() == std::size(mixed_engine_golden::k_entries));
    for (size_t i = 0; i < scns.size(); ++i) {
        const auto &s = scns[i];
        const auto &g = mixed_engine_golden::k_entries[i];
        INFO(s.name);
        REQUIRE(std::string(g.name) == s.name);
        const MixedFilamentEngine e = engine_for(s.rows, s.num_physical, s.gradient_mode, s.lo, s.hi, s.advanced);
        const std::string t = mixed_engine_scn::transcript(e, s.num_physical);
        CHECK(t.size() == g.length);
        CHECK(fnv1a(t) == g.hash);
    }
}

TEST_CASE("Mixed engine: virtual ids skip disabled and deleted rows", "[MixedFilament][engine]")
{
    // 4 physical, rows A, B(disabled), C: A = 5, C = 6, nothing at 7 (record, slice 3 id-alignment test).
    std::vector<MixedFilament> rows = { row(1, 2), row(1, 3), row(2, 3) };
    rows[1].enabled = false;
    const MixedFilamentEngine e(rows);
    CHECK(e.mixed_index_from_filament_id(4, 4) == -1);
    CHECK(e.mixed_index_from_filament_id(5, 4) == 0);
    CHECK(e.mixed_index_from_filament_id(6, 4) == 2);
    CHECK(e.mixed_index_from_filament_id(7, 4) == -1);
    CHECK(e.filament_id_from_mixed_index(0, 4) == 5);
    CHECK(e.filament_id_from_mixed_index(1, 4) == 0);
    CHECK(e.filament_id_from_mixed_index(2, 4) == 6);
    CHECK(e.total_filaments(4) == 6);
    CHECK(e.resolve(3, 4, 0) == 3); // physical ids pass through
    CHECK(e.resolve(9, 4, 0) == 9); // unknown ids pass through, as in Snapmaker
}

TEST_CASE("Mixed engine: from_slots numbering equals tail slot position", "[MixedFilament][engine]")
{
    // 6 slots: 4 physical + 2 mixed tail slots. Slot ids 5 and 6.
    std::vector<MixedFilament> rows = { row(1, 2), row(3, 4) };
    rows[1].manual_pattern = "1122";
    MixedEngineSlotsReport rep;
    const MixedFilamentEngine e = MixedFilamentEngine::from_slots(rows, 6, &rep);
    REQUIRE(rep.valid);
    CHECK(rep.num_physical == 4);
    CHECK(rep.num_mixed == 2);
    CHECK(rep.warnings.empty());
    CHECK(e.mixed_index_from_filament_id(5, 4) == 0);
    CHECK(e.mixed_index_from_filament_id(6, 4) == 1);
    CHECK(e.resolve(6, 4, 0) == 3);
    CHECK(e.resolve(6, 4, 2) == 4);
}

TEST_CASE("Mixed engine: from_slots refuses inconsistent slots and warns above the paint cap", "[MixedFilament][engine]")
{
    MixedEngineSlotsReport rep;
    // more rows than slots / no physical slot left
    CHECK(MixedFilamentEngine::from_slots({ row(1, 2), row(1, 2) }, 2, &rep).mixed_filaments().empty());
    CHECK(!rep.valid);
    // component past the physical count (3 physical, component 4)
    CHECK(MixedFilamentEngine::from_slots({ row(1, 4) }, 4, &rep).mixed_filaments().empty());
    CHECK(!rep.valid);
    CHECK(!rep.warnings.empty());
    // component 0
    CHECK(MixedFilamentEngine::from_slots({ row(0, 1) }, 3, &rep).mixed_filaments().empty());
    CHECK(!rep.valid);
    // disabled flag on a slot row is ignored, with a warning
    MixedFilament off = row(1, 2);
    off.enabled = false;
    const MixedFilamentEngine fixed = MixedFilamentEngine::from_slots({ off }, 3, &rep);
    CHECK(rep.valid);
    CHECK(rep.warnings.size() == 1);
    CHECK(fixed.mixed_index_from_filament_id(4, 3) == 0);
    // more than 32 slots: still valid (Extruder32 is kept), but warned
    std::vector<MixedFilament> many(30, row(1, 2));
    const MixedFilamentEngine big = MixedFilamentEngine::from_slots(many, kMixedEngineMaxSlots + 1, &rep);
    CHECK(rep.valid);
    CHECK(rep.warnings.size() == 1);
    CHECK(big.mixed_filaments().size() == 30);
    // exactly 32 slots: no warning
    CHECK(MixedFilamentEngine::from_slots(std::vector<MixedFilament>(28, row(1, 2)), 32, &rep).enabled_count() == 28);
    CHECK(rep.warnings.empty());
}

TEST_CASE("Mixed engine: native slots are not engine rows", "[MixedFilament][engine]")
{
    CHECK(!is_engine_row(MixedFilament()));
    CHECK(!is_engine_row(row(1, 2, true, 30)));
    MixedFilament mf;
    mf.manual_pattern = "12";            CHECK(is_engine_row(mf));
    mf = MixedFilament(); mf.gradient_component_ids = "123"; CHECK(is_engine_row(mf));
    mf = MixedFilament(); mf.distribution_mode = int(MixedFilament::LayerCycle); CHECK(is_engine_row(mf));
    mf = MixedFilament(); mf.local_z_max_sublayers = 2; CHECK(is_engine_row(mf));
    mf = MixedFilament(); mf.gradient_enabled = true; CHECK(is_engine_row(mf));
    mf = MixedFilament(); mf.component_b_surface_offset = 0.1f; CHECK(is_engine_row(mf));
    mf = MixedFilament(); mf.ui_mode = 0; CHECK(is_engine_row(mf));
}

TEST_CASE("Mixed engine: ratio arithmetic (derived by reading Snapmaker's source)", "[MixedFilament][engine]")
{
    int a = 4, b = 2;
    MixedFilamentEngine::normalize_ratio_pair(a, b);
    CHECK((a == 2 && b == 1));
    a = 0; b = 0;
    MixedFilamentEngine::normalize_ratio_pair(a, b);
    CHECK((a == 1 && b == 0));
    a = -5; b = 3;
    MixedFilamentEngine::normalize_ratio_pair(a, b);
    CHECK((a == 0 && b == 3));
    CHECK(MixedFilamentEngine::safe_mod(-1, 3) == 2);
    CHECK(MixedFilamentEngine::safe_mod(5, 0) == 0);

    // layer-cycle mode: 25% B -> A,A,A,B ; 70% B -> A,B,B ; 0 -> all A ; 100 -> all B
    MixedFilamentEngine e({ row(1, 2, true, 25), row(1, 2, true, 70), row(1, 2, true, 0), row(1, 2, true, 100) });
    e.apply_gradient_settings(0, 0.04f, 0.16f);
    CHECK((e.mixed_filaments()[0].ratio_a == 3 && e.mixed_filaments()[0].ratio_b == 1));
    CHECK((e.mixed_filaments()[1].ratio_a == 1 && e.mixed_filaments()[1].ratio_b == 2));
    CHECK((e.mixed_filaments()[2].ratio_a == 1 && e.mixed_filaments()[2].ratio_b == 0));
    CHECK((e.mixed_filaments()[3].ratio_a == 0 && e.mixed_filaments()[3].ratio_b == 1));
    std::vector<unsigned> seq;
    for (int l = 0; l < 8; ++l) seq.push_back(e.resolve(3, 2, l)); // id 3 with 2 physical = row 0
    CHECK(seq == std::vector<unsigned>({ 1, 1, 1, 2, 1, 1, 1, 2 }));
    CHECK(e.resolve(4, 2, 0) == 1);
    CHECK(e.resolve(4, 2, 1) == 2);
    CHECK(e.resolve(4, 2, -1) == 2); // negative layers wrap
    // auto rows keep 1:1
    MixedFilamentEngine auto_rows({ row(1, 2, false, 10) });
    auto_rows.apply_gradient_settings(0, 0.04f, 0.16f);
    CHECK(auto_rows.resolve(3, 2, 0) == 1);
    CHECK(auto_rows.resolve(3, 2, 1) == 2);
}

TEST_CASE("Mixed engine: weighted gradient sequence", "[MixedFilament][engine]")
{
    const auto seq = build_weighted_gradient_sequence({ 1, 2, 3 }, { 50, 25, 25 });
    REQUIRE(seq.size() == 4);
    CHECK(std::count(seq.begin(), seq.end(), 1u) == 2);
    CHECK(std::count(seq.begin(), seq.end(), 2u) == 1);
    CHECK(std::count(seq.begin(), seq.end(), 3u) == 1);
    // cycle capped at 48 even for 64 ids
    std::vector<unsigned> ids;
    for (unsigned i = 1; i <= 64; ++i) ids.push_back(i);
    CHECK(build_weighted_gradient_sequence(ids, std::vector<int>(64, 1)).size() <= 64);
    CHECK(build_weighted_gradient_sequence({}, {}).empty());
    // zero weights everywhere fall back to equal weights
    CHECK(build_weighted_gradient_sequence({ 1, 2 }, { 0, 0 }).size() == 2);
}

TEST_CASE("Mixed engine: pattern tokens and manual pattern resolution", "[MixedFilament][engine]")
{
    CHECK(MixedFilamentEngine::split_pattern_group_to_tokens("12[10]3", 12) == std::vector<std::string>({ "1", "2", "10", "3" }));
    CHECK(MixedFilamentEngine::split_pattern_groups("12,,3") == std::vector<std::string>({ "12", "3" }));
    CHECK(MixedFilamentEngine::mix_percent_from_manual_pattern("1122") == 50);
    CHECK(MixedFilamentEngine::mix_percent_from_manual_pattern("2") == 100);
    CHECK(MixedFilamentEngine::mix_percent_from_manual_pattern("") == 50);

    MixedFilament mf = row(3, 4);
    mf.manual_pattern = "12[10]";
    CHECK(MixedFilamentEngine::physical_filament_from_token("1", mf, 12) == 3);
    CHECK(MixedFilamentEngine::physical_filament_from_token("2", mf, 12) == 4);
    CHECK(MixedFilamentEngine::physical_filament_from_token("10", mf, 12) == 10);
    CHECK(MixedFilamentEngine::physical_filament_from_token("10", mf, 9) == 0);   // above the physical count
    CHECK(MixedFilamentEngine::physical_filament_from_token("0", mf, 12) == 0);

    // per-perimeter groups: "12,3" -> perimeter 0 cycles 1,2 ; perimeter 1 is always 3 ; flat form cycles 1,2,3
    MixedFilament g = row(1, 2);
    g.manual_pattern = "12,3";
    MixedFilamentEngine e({ g });
    CHECK(e.resolve(4, 3, 0) == 1);
    CHECK(e.resolve(4, 3, 1) == 2);
    CHECK(e.resolve(4, 3, 2) == 3);
    CHECK(e.resolve_perimeter(4, 3, 1, 0) == 2);
    CHECK(e.resolve_perimeter(4, 3, 1, 1) == 3);
    CHECK(e.resolve_perimeter(4, 3, 1, 7) == 3);  // past the last group clamps to it
    CHECK(e.ordered_perimeter_extruders(4, 3, 0) == std::vector<unsigned>({ 1, 3 }));
    CHECK(e.effective_painted_region_filament_id(4, 3, 0) == 4); // grouped pattern keeps virtual identity
}

TEST_CASE("Mixed engine: surface offset follows the resolved component", "[MixedFilament][engine]")
{
    MixedFilament mf = row(1, 2, true, 50);
    mf.component_b_surface_offset = 0.25f;
    MixedFilamentEngine e({ mf });
    e.apply_gradient_settings(0, 0.04f, 0.16f);
    // 50% -> ratio 1:1: layer 0 is A (no offset), layer 1 is B (offset 0.25)
    CHECK(e.component_surface_offset(3, 2, 0) == 0.f);
    CHECK(e.component_surface_offset(3, 2, 1) == Catch::Approx(0.25f));
    CHECK(e.component_surface_offset(1, 2, 1) == 0.f); // physical id
    CHECK(MixedFilamentEngine::canonical_signed_bias_value(0.3f, 0.f) == Catch::Approx(-0.3f));
    CHECK(MixedFilamentEngine::canonical_signed_bias_value(9.f, 0.f) == Catch::Approx(-2.f)); // clamped to +-2
}

TEST_CASE("Mixed engine: hostile numeric input is bounded", "[MixedFilament][engine][security]")
{
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();

    // bounds from settings: NaN, inf, huge, negative must not crash or produce absurd cycles
    for (float lo : { nan, inf, -inf, 1e30f, -5.f, 0.f })
        for (float hi : { nan, inf, 1e30f, -1.f, 0.f }) {
            MixedFilamentEngine e({ row(1, 2, true, 33), row(1, 2, true, 99) });
            e.apply_gradient_settings(1, lo, hi);
            for (const MixedFilament &mf : e.mixed_filaments()) {
                CHECK(mf.ratio_a >= 1);
                CHECK(mf.ratio_b >= 1);
                CHECK(int64_t(mf.ratio_a) + mf.ratio_b < 100000);
            }
            const unsigned r = e.resolve(3, 2, INT_MIN, nan, inf, true);
            CHECK((r == 1 || r == 2));
        }

    // ratios and layer index at the integer limits
    MixedFilament big = row(1, 2);
    big.ratio_a = INT_MAX;
    big.ratio_b = INT_MAX;
    MixedFilamentEngine e({ big });
    for (int layer : { INT_MIN, -1, 0, 1, INT_MAX }) {
        const unsigned r = e.resolve(3, 2, layer);
        CHECK((r == 1 || r == 2));
    }
    MixedFilamentEngine dither({ big });
    dither.apply_gradient_settings(0, 0.04f, 0.16f, true);
    dither.mixed_filaments()[0].ratio_a = INT_MAX; // apply_gradient_settings recomputed them; put the hostile values back
    dither.mixed_filaments()[0].ratio_b = INT_MAX;
    for (int layer : { INT_MIN, -1, 0, 1, INT_MAX }) {
        const unsigned r = dither.resolve(3, 2, layer);
        CHECK((r == 1 || r == 2));
    }

    // painted-region heights
    MixedFilament a = row(1, 2, false);
    MixedFilamentEngine pe({ a });
    for (float h : { nan, inf, 1e30f, -1.f, 0.f })
        for (int layer : { INT_MIN, -1, 0, INT_MAX }) {
            const unsigned r = pe.effective_painted_region_filament_id(3, 2, layer, 0.f, 0.2f, h, h, 0.2f);
            CHECK((r == 1 || r == 2));
        }

    // hostile pattern and weight tokens (from a project file)
    MixedFilament p = row(1, 2);
    p.manual_pattern = "[99999999999999999999999][18446744073709551616]12";
    MixedFilamentEngine pp({ p });
    CHECK(pp.resolve(3, 2, 0) == 1);
    CHECK(pp.resolve(3, 2, 2) == 1);
    CHECK(pp.resolve(3, 2, 3) == 2); // the over-long tokens do not resolve (not wrapped to a small id); the row falls back to component A
    CHECK(MixedFilamentEngine::physical_filament_from_token("18446744073709551617", p, 99) == 0); // would wrap to 1 with strtoul on 32 bit ids
    CHECK(MixedFilamentEngine::physical_filament_from_token("4294967297", p, 99) == 0);            // 2^32 + 1
    CHECK(MixedFilamentEngine::physical_filament_from_token("-1", p, 99) == 0);
    CHECK(MixedFilamentEngine::physical_filament_from_token("", p, 99) == 0);
    CHECK(decode_gradient_component_weights("99999999999999999999/1/1", 3).empty());
    CHECK(decode_gradient_component_weights("50/25/25", 3) == std::vector<int>({ 50, 25, 25 }));
    CHECK(decode_gradient_component_weights("50/25/25", 2).empty());
    int v = 7;
    CHECK(!parse_bounded_mixed_token("123456", v));
    CHECK(v == 7);
    CHECK(parse_bounded_mixed_token("99999", v));
    CHECK(v == 99999);
    CHECK(!parse_bounded_mixed_token("1a", v));
    CHECK(!parse_bounded_mixed_token("+1", v));

    // gradient ids from a file: huge and duplicated ids never index past the physical count
    MixedFilament gr = row(1, 2);
    gr.distribution_mode = int(MixedFilament::LayerCycle);
    gr.gradient_component_ids = "1/2/99999999999999999999/3/3";
    gr.gradient_component_weights = "1/1/1";
    MixedFilamentEngine ge({ gr });
    for (int l = 0; l < 6; ++l) {
        const unsigned r = ge.resolve(4, 3, l);
        CHECK((r >= 1 && r <= 3));
    }
}
