// Scenario table and transcript generator shared by the engine test and by the scratch harness that produced
// mixed_engine_golden.hpp from Snapmaker's own MixedFilamentManager. The transcript is a template over the
// manager type so both sides run exactly the same call sequence.
#pragma once
#include <cstdio>
#include <string>
#include <vector>

#include "mixed_filament_golden.hpp"

namespace Slic3r::mixed_engine_scn {

struct Scenario
{
    const char *name;
    std::string rows;      // mixed_filament_definitions string
    size_t      num_physical;
    int         gradient_mode;
    float       lo, hi;
    bool        advanced;
};

inline const std::string k_custom_ratios =
    "1,2,1,1,25,0,g,w,m2,z0,xa0,xb0,d0,o0,u1;"
    "2,3,1,1,70,0,g,w,m2,z0,xa0,xb0,d0,o0,u2;"
    "1,3,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u3;"
    "3,1,1,1,0,0,g,w,m2,z0,xa0,xb0,d0,o0,u4;"
    "1,2,1,1,100,0,g,w,m2,z0,xa0,xb0,d0,o0,u5;"
    "1,2,1,1,37,0,g,w,m2,z0,xa0.3,xb0,d0,o0,u6;"
    "2,3,1,1,81,0,g,w,m2,z0,xa0,xb0.25,d0,o0,u7";
inline const std::string k_ext_pattern =
    "1,2,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u1,[10]1[11]2,3;"
    "3,4,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u2,1122;"
    "5,6,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u3,[12]";
inline const std::string k_grad4 =
    "1,2,1,1,25,0,g1234,w40/30/20/10,m0,z0,xa0,xb0,d0,o0,u1;"
    "1,2,1,1,25,0,g1234,w1/1/1/97,m2,z0,xa0,xb0,d0,o0,u2;"
    "1,2,1,1,25,0,g/2/11/4,w10/60/30,m0,z0,xa0,xb0,d0,o0,u3";

inline std::vector<Scenario> scenarios()
{
    using namespace mixed_golden;
    return {
        { "auto3_cycle", k_auto3, 3, 0, 0.04f, 0.16f, false },
        { "disabled_auto4", k_disabled_auto4, 4, 0, 0.04f, 0.16f, false },
        { "deleted3", k_deleted3, 3, 0, 0.04f, 0.16f, false },
        { "custom_pattern3", k_custom_pattern3, 3, 0, 0.04f, 0.16f, false },
        { "gradient3", k_gradient3, 3, 0, 0.04f, 0.16f, false },
        { "gradient3col3", k_gradient3col3, 3, 0, 0.04f, 0.16f, false },
        { "offsets3", k_offsets3, 3, 0, 0.04f, 0.16f, false },
        { "ratios_cycle", k_custom_ratios, 3, 0, 0.04f, 0.16f, false },
        { "ratios_dither", k_custom_ratios, 3, 0, 0.04f, 0.16f, true },
        { "ratios_height", k_custom_ratios, 3, 1, 0.04f, 0.16f, false },
        { "ratios_height_wide", k_custom_ratios, 3, 1, 0.10f, 0.40f, false },
        { "ext_pattern12", k_ext_pattern, 12, 0, 0.04f, 0.16f, false },
        { "grad4", k_grad4, 12, 0, 0.04f, 0.16f, false },
        { "ext12_auto", k_ext12, 12, 0, 0.04f, 0.16f, false },
    };
}

// One line per probe; every layer in [-3, 59]. Only the manager interface that exists on both sides is used.
template<class Mgr> std::string transcript(const Mgr &m, size_t np)
{
    std::string out;
    char buf[96];
    const size_t total = m.total_filaments(np);
    out += "total=" + std::to_string(total) + "\n";
    for (unsigned id = 1; id <= total + 1; ++id) {
        std::snprintf(buf, sizeof buf, "id%u idx=%d\n", id, m.mixed_index_from_filament_id(id, np));
        out += buf;
        if (id <= np || id > total)
            continue;
        for (int layer = -3; layer <= 59; ++layer) {
            const float z = 0.2f * float(layer + 1), h = 0.2f;
            std::snprintf(buf, sizeof buf, "L%d r=%u", layer, m.resolve(id, np, layer, z, h));
            out += buf;
            std::snprintf(buf, sizeof buf, " rf=%u", m.resolve(id, np, layer, z, h, true));
            out += buf;
            for (int p = 0; p < 3; ++p) {
                std::snprintf(buf, sizeof buf, " p%d=%u", p, m.resolve_perimeter(id, np, layer, p, z, h));
                out += buf;
            }
            std::snprintf(buf, sizeof buf, " e=%u e2=%u", m.effective_painted_region_filament_id(id, np, layer, z, h),
                          m.effective_painted_region_filament_id(id, np, layer, z, h, 0.4f, 0.2f, 0.2f));
            out += buf;
            // float division ties: lround(h / base) at (near) .5 quotients (Snapmaker divides in float; the 2 * 0.02f rows are the
            // ones where a double division gives a different integer, probed over a 0.02 grid)
            static const float ties[][3] = { { 0.12f, 0.18f, 0.18f }, { 0.12f, 0.30f, 0.12f }, { 0.08f, 0.20f, 0.08f },
                                             { 0.06f, 0.09f, 0.06f }, { 0.16f, 0.40f, 0.16f }, { 0.24f, 0.36f, 0.24f },
                                             { 0.12f, 0.18f, 0.30f }, { 0.2f, 0.3f, 0.5f },
                                             { 2 * 0.02f, 5 * 0.02f, 2 * 0.02f }, { 2 * 0.02f, 9 * 0.02f, 2 * 0.02f },
                                             { 2 * 0.02f, 13 * 0.02f, 21 * 0.02f }, { 2 * 0.02f, 23 * 0.02f, 29 * 0.02f } };
            for (const auto &t : ties) {
                std::snprintf(buf, sizeof buf, " t=%u", m.effective_painted_region_filament_id(id, np, layer, z, h, t[1], t[2], t[0]));
                out += buf;
            }
            std::snprintf(buf, sizeof buf, " off=%.4f o=", double(m.component_surface_offset(id, np, layer, z, h)));
            out += buf;
            for (unsigned e : m.ordered_perimeter_extruders(id, np, layer, z, h)) {
                out += std::to_string(e);
                out += '.';
            }
            out += "\n";
        }
    }
    return out;
}

} // namespace Slic3r::mixed_engine_scn
