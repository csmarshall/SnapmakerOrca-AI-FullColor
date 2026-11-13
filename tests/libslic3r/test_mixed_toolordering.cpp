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

void setup(Print &print, Model &model, std::vector<TriangleMesh> meshes, const DynamicPrintConfig &config_in)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.apply(config_in);
    for (TriangleMesh &t : meshes) {
        ModelObject *object = model.add_object();
        object->name += "object.stl";
        object->add_volume(std::move(t));
        object->add_instance();
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
