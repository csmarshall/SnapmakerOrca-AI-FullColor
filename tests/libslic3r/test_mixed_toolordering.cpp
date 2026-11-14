// Slice 4c of the mixed-filament engine port: engine rows in ToolOrdering::resolve_mixed_filaments.
//
// (a) Native twin: a native 2.5 mixed project (every filament_mixed_engine_row marker empty) must produce the
//     same tool-order transcript as before 4c. The expected hashes below were captured by running this very
//     test on the pre-4c tree (HEAD b38674b725); they pin "unchanged", not "correct".
// (b) Engine rows: the per-layer extruder of a marked slot equals MixedFilamentEngine::resolve for the object's
//     own layer index, i.e. what Snapmaker's ToolOrdering computes. The engine itself is pinned to Snapmaker's
//     goldens in test_mixed_filament_engine.cpp. What this cannot catch: the engine and the expectation here
//     share the same row parser, so a shared misreading of the row format would agree on both sides; the 50/50
//     parity test is the one oracle derived by hand.
// (c) Multi-object plates (risk R2): the engine index is the object-local layer index (Snapmaker's layerCount),
//     not the position in the plate-wide layer list.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "libslic3r/GCode/ToolOrdering.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/MixedFilamentEngine.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;

namespace {

uint64_t fnv1a(const std::string &t)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : t) { h ^= c; h *= 1099511628211ull; }
    return h;
}

// 2 physical filaments plus mixed tail slots. `comps`/`rows` are indexed per mixed slot.
struct Slot { const char *components; const char *ratios; const char *row; bool gradient; };

DynamicPrintConfig make_config(size_t num_physical, const std::vector<Slot> &mixed, bool sublayer, size_t role_slot_1based,
                               std::initializer_list<ConfigBase::SetDeserializeItem> extra = {})
{
    const size_t n = num_physical + mixed.size();
    static const char *palette[] = { "#FF0000", "#00FF00", "#0000FF", "#FFFF00", "#FF00FF", "#00FFFF", "#FF8000", "#8000FF" };
    std::string diameters, colours, flush;
    for (size_t i = 0; i < n; ++i) {
        diameters += (i ? "," : "") + std::string("1.75");
        colours   += (i ? ";" : "") + std::string(palette[i % 8]);
        for (size_t j = 0; j < n; ++j)
            flush += ((i || j) ? "," : "") + std::string(i == j ? "0" : "280");
    }
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({ { "nozzle_diameter", "0.4" }, { "filament_diameter", diameters } });
    config.set_num_filaments(unsigned(n));
    const auto &defaults = FullPrintConfig::defaults();
    for (const char *key : { "filament_type", "filament_vendor", "filament_start_gcode", "filament_change_length", "filament_change_length_nc",
                             "filament_ramming_travel_time", "filament_ramming_travel_time_nc",
                             "filament_ramming_volumetric_speed", "filament_ramming_volumetric_speed_nc",
                             "nozzle_temperature", "nozzle_temperature_initial_layer" })
        static_cast<ConfigOptionVectorBase *>(config.option(key, true))->resize(n, defaults.option(key));
    config.set_deserialize_strict({ { "filament_colour", colours }, { "flush_volumes_matrix", flush } });

    std::vector<unsigned char> flags(n, 0), grad(n, 0), per_part(n, 0);
    std::vector<std::string>   comps(n), ratios(n), rng(n), curve(n), rows(n);
    for (size_t k = 0; k < mixed.size(); ++k) {
        const size_t i = num_physical + k;
        flags[i]  = 1;
        comps[i]  = mixed[k].components;
        ratios[i] = mixed[k].ratios;
        rows[i]   = mixed[k].row;
        grad[i]   = mixed[k].gradient ? 1 : 0;
    }
    config.set_key_value("filament_is_mixed", new ConfigOptionBools(flags));
    config.set_key_value("filament_mixed_components", new ConfigOptionStrings(comps));
    config.set_key_value("filament_mixed_sublayer_ratios", new ConfigOptionStrings(ratios));
    config.set_key_value("filament_mixed_gradient", new ConfigOptionBools(grad));
    config.set_key_value("filament_mixed_gradient_range", new ConfigOptionStrings(rng));
    config.set_key_value("filament_mixed_gradient_curve", new ConfigOptionStrings(curve));
    config.set_key_value("filament_mixed_gradient_per_part", new ConfigOptionBools(per_part));
    config.set_key_value("filament_mixed_engine_row", new ConfigOptionStrings(rows));
    const std::string role = std::to_string(role_slot_1based);
    config.set_deserialize_strict({
        { "enable_mixed_color_sublayer", sublayer ? "1" : "0" },
        { "outer_wall_filament_id", role }, { "inner_wall_filament_id", role }, { "sparse_infill_filament_id", role },
        { "internal_solid_filament_id", role }, { "top_surface_filament_id", role }, { "bottom_surface_filament_id", role },
        { "layer_height", "0.2" }, { "initial_layer_print_height", "0.2" },
    });
    if (extra.size() > 0)
        config.set_deserialize_strict(extra);
    return config;
}

TriangleMesh cube_at(double x, double size_xy, double size_z)
{
    TriangleMesh m(its_make_cube(size_xy, size_xy, size_z));
    m.translate(float(x), 80.f, 0.f);
    return m;
}

void setup(Print &print, Model &model, std::vector<TriangleMesh> meshes, const DynamicPrintConfig &config_in,
           const std::vector<const char *> &object_layer_heights = {})
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.apply(config_in);
    for (TriangleMesh &t : meshes) {
        ModelObject *object = model.add_object();
        object->name += "object.stl";
        object->add_volume(std::move(t));
        object->add_instance();
        const size_t idx = model.objects.size() - 1;
        if (idx < object_layer_heights.size() && object_layer_heights[idx] != nullptr)
        {
            DynamicPrintConfig oc;
            oc.set_deserialize_strict("layer_height", object_layer_heights[idx]);
            object->config.apply(oc);
        }
    }
    for (ModelObject *mo : model.objects) {
        mo->ensure_on_bed();
        print.auto_assign_extruders(mo);
    }
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();
}

// Everything the engine hook may influence per layer: z, tool list, resolution map, sub-layer groups.
std::string transcript(const ToolOrdering &to)
{
    std::string out;
    char        buf[160];
    for (const LayerTools &lt : to) {
        std::snprintf(buf, sizeof buf, "z=%.4f x=", lt.print_z);
        out += buf;
        for (unsigned e : lt.extruders) out += std::to_string(e) + ".";
        out += " r=";
        for (const auto &[slot, res] : lt.mixed_filament_resolution) out += std::to_string(slot) + ">" + std::to_string(res) + ".";
        out += " g=";
        for (const auto &g : lt.mixed_sub_layer_groups) {
            out += std::to_string(g.mixed_slot_0based) + (g.is_gradient ? "G" : "R") + "[";
            for (size_t k = 0; k < g.components_0based.size(); ++k) {
                std::snprintf(buf, sizeof buf, "%u:%.6f,", g.components_0based[k], g.sub_heights[k]);
                out += buf;
            }
            out += "]";
        }
        out += "\n";
    }
    return out;
}

// extruders of the layer at print_z (0-based tool ids), as a sorted vector
std::vector<unsigned> tools_at(const ToolOrdering &to, double z)
{
    for (const LayerTools &lt : to)
        if (std::abs(lt.print_z - z) < 1e-4)
            return lt.extruders;
    return {};
}

} // namespace

// ---- (a) native twin ----------------------------------------------------------------------------------------

namespace {
struct TwinCase { const char *name; bool sublayer; bool gradient; uint64_t hash; size_t length; };
// HASHES captured on the pre-4c tree; see the file header.
const TwinCase k_twins[] = {
    { "whole_layer_drr", false, false, 17018223871349956743ull, 2451 },
    { "sublayer", true, false, 8092644522113306466ull, 4827 },
    { "sublayer_gradient", true, true, 4764846703889736446ull, 4803 },
};
} // namespace

TEST_CASE("4c native twin: a native 2.5 mixed project keeps its tool-order transcript", "[MixedFilament][ToolOrdering][Twin]")
{
    for (const TwinCase &tc : k_twins) {
        INFO(tc.name);
        Print print;
        Model model;
        setup(print, model, { cube_at(80, 20, 20) },
              make_config(2, { { "1,2", "0.6,0.4", "", tc.gradient } }, tc.sublayer, 3));
        const std::string t = transcript(print.tool_ordering());
        INFO("transcript bytes=" << t.size() << " hash=" << fnv1a(t) << "\n" << t.substr(0, 600));
        CHECK(t.size() == tc.length);
        CHECK(fnv1a(t) == tc.hash);
    }
}

// ---- (b) engine rows -----------------------------------------------------------------------------------------

namespace {
// Snapmaker row strings (custom flag 1 = token 4). Fields: a,b,enabled,custom,mix_b%,pointillism,g<ids>,w<weights>,m<ui>,...
const char *kRow50   = "1,2,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u1";
const char *kRow25   = "1,2,1,1,25,0,g,w,m2,z0,xa0,xb0,d0,o0,u2";
const char *kRowAuto = "1,2,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u3"; // non-custom: the A/B height cadence applies

MixedFilamentEngine engine_of(const DynamicPrintConfig &c, MixedEngineGlobals *globals = nullptr)
{
    MixedEngineSlotsReport rep;
    MixedFilamentEngine    e = mixed_engine_from_config(c, &rep);
    REQUIRE(rep.valid);
    if (globals)
        *globals = mixed_engine_globals_from_config(c);
    return e;
}

// 0-based tool the engine picks for object-local layer `i` of slot 3 (1-based id 3, two physical filaments).
unsigned expected_tool(const MixedFilamentEngine &e, const MixedEngineGlobals &g, const PrintObject &o, size_t i, unsigned id1)
{
    const Layer *l = o.layers()[i];
    return mixed_engine_resolve_layer(e, g, id1, 2, int(i), float(l->print_z), float(l->height), float(o.config().layer_height.value)) - 1;
}
} // namespace

TEST_CASE("4c engine row: per-layer tool equals the engine's choice for the object's own layer index", "[MixedFilament][ToolOrdering][EngineRow]")
{
    for (const char *row : { kRow50, kRow25 }) {
        INFO(row);
        const DynamicPrintConfig cfg = make_config(2, { { "1,2", "0.5,0.5", row, false } }, false, 3);
        Print print;
        Model model;
        setup(print, model, { cube_at(80, 20, 20) }, cfg);
        MixedEngineGlobals g;
        const MixedFilamentEngine e = engine_of(cfg, &g);
        const PrintObject &obj = *print.objects().front();
        REQUIRE(obj.layers().size() >= 50);
        std::set<unsigned> seen;
        for (size_t i = 0; i < obj.layers().size(); ++i) {
            const double z = obj.layers()[i]->print_z;
            const std::vector<unsigned> tools = tools_at(print.tool_ordering(), z);
            INFO("layer " << i << " z=" << z);
            REQUIRE(tools.size() == 1);
            CHECK(tools.front() == expected_tool(e, g, obj, i, 3));
            seen.insert(tools.front());
        }
        CHECK(seen == std::set<unsigned>{ 0, 1 });
        // Resolution map points at the same tool, and no sub-layer group exists for an engine row.
        for (const LayerTools &lt : print.tool_ordering()) {
            CHECK(lt.mixed_sub_layer_groups.empty());
            if (!lt.extruders.empty() && lt.mixed_filament_resolution.count(2))
                CHECK(lt.mixed_filament_resolution.at(2) == lt.extruders.front());
        }
    }
}

TEST_CASE("4c engine row: 50/50 row alternates by layer parity, derived by hand", "[MixedFilament][ToolOrdering][EngineRow]")
{
    // Independent of the engine: a 1:1 ratio row cycles A,B,A,B... so adjacent layers always differ and layers
    // two apart agree. Which component leads is read off the engine (pinned to Snapmaker's golden elsewhere).
    const DynamicPrintConfig cfg = make_config(2, { { "1,2", "0.5,0.5", kRow50, false } }, false, 3);
    Print print;
    Model model;
    setup(print, model, { cube_at(80, 20, 20) }, cfg);
    const PrintObject &obj = *print.objects().front();
    std::vector<unsigned> seq;
    for (const Layer *l : obj.layers())
        seq.push_back(tools_at(print.tool_ordering(), l->print_z).at(0));
    REQUIRE(seq.size() >= 50);
    for (size_t i = 0; i + 2 < seq.size(); ++i) {
        CHECK(seq[i] != seq[i + 1]);
        CHECK(seq[i] == seq[i + 2]);
    }
}

TEST_CASE("4c engine row: mixed_color_layer_height_a/b cadence on a non-custom row", "[MixedFilament][ToolOrdering][EngineRow]")
{
    // base 0.2, a = 0.2 -> 1 layer of A; b = 0.4 -> 2 layers of B: cycle A,B,B (Snapmaker resolve_mixed_with_layer_heights).
    const DynamicPrintConfig cfg = make_config(2, { { "1,2", "0.5,0.5", kRowAuto, false } }, false, 3,
                                               { { "mixed_color_layer_height_a", "0.2" }, { "mixed_color_layer_height_b", "0.4" } });
    Print print;
    Model model;
    setup(print, model, { cube_at(80, 20, 20) }, cfg);
    const PrintObject &obj = *print.objects().front();
    for (size_t i = 0; i < obj.layers().size(); ++i) {
        const std::vector<unsigned> tools = tools_at(print.tool_ordering(), obj.layers()[i]->print_z);
        INFO("layer " << i);
        REQUIRE(tools.size() == 1);
        CHECK(tools.front() == (i % 3 == 0 ? 0u : 1u));
    }
}

TEST_CASE("4c engine row: two engine slots and a native slot coexist, native one untouched", "[MixedFilament][ToolOrdering][EngineRow]")
{
    // slot 3 native (empty marker), slot 4 engine. Roles are assigned to slot 4; slot 3 must not become an engine row.
    const DynamicPrintConfig cfg = make_config(2, { { "1,2", "0.6,0.4", "", false }, { "1,2", "0.5,0.5", kRow25, false } }, false, 4);
    Print print;
    Model model;
    setup(print, model, { cube_at(80, 20, 20) }, cfg);
    MixedEngineGlobals g;
    const MixedFilamentEngine e = engine_of(cfg, &g);
    REQUIRE(e.is_engine_row(1));
    REQUIRE(!e.is_engine_row(0));
    const PrintObject &obj = *print.objects().front();
    for (size_t i = 0; i < obj.layers().size(); ++i) {
        const std::vector<unsigned> tools = tools_at(print.tool_ordering(), obj.layers()[i]->print_z);
        REQUIRE(tools.size() == 1);
        CHECK(tools.front() == expected_tool(e, g, obj, i, 4)); // id 4 = second mixed slot, numbered by row
    }
}

// ---- (c) multi-object plates (R2) ------------------------------------------------------------------------------

TEST_CASE("4c engine row: multi-object plate uses each object's own layer index", "[MixedFilament][ToolOrdering][EngineRow][R2]")
{
    // Object 1 prints 0.2 mm layers, object 2 0.3 mm layers, so a plate-wide layer's position differs from both
    // objects' own indices. Snapmaker resolves with the object-local layerCount and unions the tools; so do we.
    const DynamicPrintConfig cfg = make_config(2, { { "1,2", "0.5,0.5", kRow25, false } }, false, 3);
    Print print;
    Model model;
    setup(print, model, { cube_at(60, 20, 20), cube_at(120, 20, 18) }, cfg, { nullptr, "0.3" });
    REQUIRE(print.objects().size() == 2);
    MixedEngineGlobals g;
    const MixedFilamentEngine e = engine_of(cfg, &g);

    std::vector<std::pair<double, std::set<unsigned>>> expected;
    auto add = [&](double z, unsigned t) {
        for (auto &[ez, s] : expected)
            if (std::abs(ez - z) < 1e-4) { s.insert(t); return; }
        expected.push_back({ z, { t } });
    };
    size_t mismatching_global_index = 0;
    for (const PrintObject *o : print.objects())
        for (size_t i = 0; i < o->layers().size(); ++i) {
            add(o->layers()[i]->print_z, expected_tool(e, g, *o, i, 3));
        }
    // The scenario is only meaningful if some plate-wide index differs from the object-local one.
    size_t plate_idx = 0;
    for (const LayerTools &lt : print.tool_ordering()) {
        for (const PrintObject *o : print.objects())
            for (size_t i = 0; i < o->layers().size(); ++i)
                if (std::abs(o->layers()[i]->print_z - lt.print_z) < 1e-4 && i != plate_idx)
                    ++mismatching_global_index;
        ++plate_idx;
    }
    CHECK(mismatching_global_index > 10);

    for (const auto &[z, want] : expected) {
        const std::vector<unsigned> got = tools_at(print.tool_ordering(), z);
        INFO("z=" << z);
        CHECK(std::set<unsigned>(got.begin(), got.end()) == want);
    }
}

// ---- (d) F9 (#113): the "M104 S0 ;cooldown" last-use map must see mixed COMPONENTS ---------------------------
//
// OozePrevention::pre_toolchange decides "this tool is finished" from ToolOrdering::is_last_extrusion_layer, which reads
// m_last_layer_per_extruder, which collect_extruder_statistics() fills from LayerTools::extruders. If that ran before
// resolve_mixed_filaments() rewrote the extruders of the mixed slots, a filament used later only as a component of a
// mixed slot would look finished and get M104 S0 (a cold-nozzle tool change). Object A prints filament 2 (tool 1)
// directly up to 4 mm; object B (20 mm) prints the mixed slot 3 = 1+2, so tool 1 is only ever a component above 4 mm.
// What this cannot catch: it checks the ToolOrdering map the cooldown reads, not the emitted G-code text.
namespace {
void setup_direct_plus_mixed(Print &print, Model &model, const DynamicPrintConfig &config_in)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.apply(config_in);
    int idx = 0;
    for (auto [x, z] : { std::pair{ 60., 4. }, std::pair{ 120., 20. } }) {
        ModelObject *object = model.add_object();
        object->name += "object.stl";
        object->add_volume(cube_at(x, 20, z));
        object->add_instance();
        if (idx++ == 0)
            for (const char *k : { "outer_wall_filament_id", "inner_wall_filament_id", "sparse_infill_filament_id",
                                   "internal_solid_filament_id", "top_surface_filament_id", "bottom_surface_filament_id" }) {
                DynamicPrintConfig oc;
                oc.set_deserialize_strict(k, "2");
                object->config.apply(oc);
            }
    }
    for (ModelObject *mo : model.objects) {
        mo->ensure_on_bed();
        print.auto_assign_extruders(mo);
    }
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();
}

void check_component_counts_as_used(const DynamicPrintConfig &cfg, const char *what)
{
    INFO(what);
    Print print;
    Model model;
    setup_direct_plus_mixed(print, model, cfg);
    const ToolOrdering &to = print.tool_ordering();
    constexpr unsigned  tool = 1; // filament 2
    double last_direct = 0., last_any = 0.;
    bool   component_only_layer = false;
    for (const LayerTools &lt : to) {
        const bool used = std::find(lt.extruders.begin(), lt.extruders.end(), tool) != lt.extruders.end();
        bool in_group = false;
        for (const auto &g : lt.mixed_sub_layer_groups)
            in_group |= std::find(g.components_0based.begin(), g.components_0based.end(), tool) != g.components_0based.end();
        if (used || in_group)
            last_any = lt.print_z;
        if (used && lt.print_z < 4.0 + 1e-6)
            last_direct = lt.print_z;
        if (lt.print_z > 4.0 + 1e-6 && (used || in_group))
            component_only_layer = true;
    }
    REQUIRE(last_direct > 0.);
    REQUIRE(component_only_layer);        // scenario is meaningful: tool 1 is used above object A's top
    REQUIRE(last_any > last_direct + 1.); // ... well above it
    for (const LayerTools &lt : to) {
        INFO("z=" << lt.print_z);
        if (lt.print_z < last_any - 1e-6)
            CHECK_FALSE(to.is_last_extrusion_layer(lt.print_z, tool)); // a cooldown here would precede a later use
    }
    CHECK(to.is_last_extrusion_layer(last_any, tool));
}
} // namespace

TEST_CASE("F9 cooldown last-use map counts mixed components (native, engine row, sub-layer)", "[MixedFilament][ToolOrdering][F9]")
{
    check_component_counts_as_used(make_config(2, { { "1,2", "0.5,0.5", "", false } }, false, 3), "native whole-layer");
    check_component_counts_as_used(make_config(2, { { "1,2", "0.5,0.5", "", false } }, true, 3), "native sub-layer");
    check_component_counts_as_used(make_config(2, { { "1,2", "0.5,0.5", kRow50, false } }, false, 3), "engine row");
}
