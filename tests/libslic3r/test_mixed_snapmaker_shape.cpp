// Save-time Snapmaker shape of a project config with mixed slots (ticket #122, slice 1: pure transform; slice 2: save hook, sidecar restore, 3MF round trip).
//
// What these tests cannot prove: the expected rows come from this build's serializer (which mirrors Snapmaker's)
// plus one row string copied from a real Snapmaker Orca 2.4.1 project, so they show agreement with those, not that
// Snapmaker Orca reopens the result with the right virtual filaments. That needs the manual check in the record.
#include <catch2/catch_all.hpp>

#include <nlohmann/json.hpp>

#include <functional>
#include <sstream>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>

#include "libslic3r/Format/ProjectStrategy.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/MixedFilamentLegacy.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "test_utils.hpp"

using namespace Slic3r;

namespace {

// The 6 rows Snapmaker Orca 2.4.1 wrote for 4 physical filaments (auto generate on), byte for byte.
const std::string k_real_snapmaker_4 =
    "1,2,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u19;1,3,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u20;"
    "1,4,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u21;2,3,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u22;"
    "2,4,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u23;3,4,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u24";

DynamicPrintConfig project_config(size_t n)
{
    DynamicPrintConfig c = DynamicPrintConfig::full_print_config();
    std::vector<std::string> cols, ids;
    static const char *palette[] = { "#FF0000", "#00FF00", "#0000FF", "#FFFF00", "#00FFFF", "#FF00FF", "#101010", "#202020" };
    for (size_t i = 0; i < n; ++i) {
        cols.push_back(palette[i % 8]);
        ids.push_back("Preset" + std::to_string(i + 1));
    }
    c.set_key_value("filament_colour", new ConfigOptionStrings(cols));
    c.set_key_value("filament_multi_colour", new ConfigOptionStrings(cols));
    c.set_key_value("filament_colour_type", new ConfigOptionStrings(std::vector<std::string>(n, "1")));
    c.set_key_value("filament_settings_id", new ConfigOptionStrings(ids));
    c.set_key_value("filament_map", new ConfigOptionInts(std::vector<int>(n, 1)));
    return c;
}

// n physical filaments, then one converted (marker) slot per row of `defs`.
DynamicPrintConfig converted(size_t n, const std::string &defs)
{
    DynamicPrintConfig c = project_config(n);
    REQUIRE(convert_legacy_mixed_filaments(c, defs).slots_added > 0);
    return c;
}

const std::vector<std::string> &strs(const DynamicPrintConfig &c, const char *key) { return c.option<ConfigOptionStrings>(key)->values; }

std::vector<std::string> split_rows(const std::string &s)
{
    std::vector<std::string> rows;
    size_t                   pos = 0;
    while (pos <= s.size()) {
        size_t e = s.find(';', pos);
        if (e == std::string::npos)
            e = s.size();
        rows.push_back(s.substr(pos, e - pos));
        pos = e + 1;
    }
    return rows;
}

// Every row must load in Snapmaker as exactly one live virtual filament.
void check_row_valid(const std::string &row, size_t n)
{
    LegacyMixedRow r;
    REQUIRE(parse_legacy_mixed_row(row, r));
    CHECK(r.a >= 1);
    CHECK(r.b >= 1);
    CHECK(r.a <= n);
    CHECK(r.b <= n);
    CHECK(r.a != r.b);
    CHECK(r.enabled);
    CHECK_FALSE(r.deleted);
}

// 4 physical + converted marker slots 5,6 (custom rows) + slot 7 converted then made native (empty marker).
DynamicPrintConfig two_converted_one_native()
{
    DynamicPrintConfig d = converted(4, "1,2,1,1,30,0,g,w,m2,z0,xa0,xb0,d0,o0,u7;2,3,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u9;3,4,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u11");
    d.option<ConfigOptionStrings>("filament_mixed_engine_row")->values[6]      = "";
    d.option<ConfigOptionStrings>("filament_mixed_components")->values[6]      = "3,4";
    d.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios")->values[6] = "0.7000,0.3000";
    return d;
}

struct Shaped : SnapmakerProjectShape
{
    DynamicPrintConfig config;
};

Shaped shape_of(const DynamicPrintConfig &full)
{
    Shaped s;
    static_cast<SnapmakerProjectShape &>(s) = make_snapmaker_project_shape(full, s.config);
    return s;
}

} // namespace

TEST_CASE("Snapmaker shape: a config without mixed slots comes back unchanged", "[MixedFilament][snapmaker_shape]")
{
    const DynamicPrintConfig c = project_config(3);
    const Shaped s = shape_of(c);
    CHECK_FALSE(s.transformed);
    CHECK(s.mixed_filament_definitions.empty());
    CHECK(s.sidecar.empty());
    CHECK(s.config == c);
}

TEST_CASE("Snapmaker shape: Snapmaker's own 2.4.1 rows survive a convert and save byte for byte", "[MixedFilament][snapmaker_shape]")
{
    const DynamicPrintConfig c = converted(4, k_real_snapmaker_4);
    REQUIRE(strs(c, "filament_colour").size() == 10);
    const Shaped s = shape_of(c);
    REQUIRE(s.transformed);
    CHECK(s.mixed_filament_definitions == k_real_snapmaker_4);
    CHECK(strs(s.config, "filament_colour").size() == 4);
    CHECK(strs(s.config, "filament_settings_id").size() == 4);
}

TEST_CASE("Snapmaker shape: n physical plus k mixed slots", "[MixedFilament][snapmaker_shape]")
{
    const DynamicPrintConfig c = two_converted_one_native();
    REQUIRE(strs(c, "filament_colour").size() == 7);
    const DynamicPrintConfig before = c;
    const Shaped s = shape_of(c);
    REQUIRE(s.transformed);
    CHECK(s.config.option<ConfigOptionBools>("filament_is_mixed")->values == std::vector<unsigned char>(4, 0));
    for (const char *key : { "filament_colour", "filament_multi_colour", "filament_colour_type", "filament_settings_id",
                             "filament_mixed_components", "filament_mixed_sublayer_ratios", "filament_mixed_engine_row" })
        CHECK(strs(s.config, key).size() == 4);
    CHECK(s.config.option<ConfigOptionInts>("filament_map")->values.size() == 4);
    CHECK(strs(s.config, "filament_settings_id") == std::vector<std::string>({ "Preset1", "Preset2", "Preset3", "Preset4" }));

    const std::vector<std::string> rows = split_rows(s.mixed_filament_definitions);
    REQUIRE(rows.size() == 3);
    // slots 5 and 6: marker rows verbatim, in slot order
    CHECK(rows[0] == strs(c, "filament_mixed_engine_row")[4]);
    CHECK(rows[1] == strs(c, "filament_mixed_engine_row")[5]);
    CHECK(rows[0].rfind("1,2,1,1,30,", 0) == 0);
    CHECK(rows[1].rfind("2,3,1,1,50,", 0) == 0);
    // slot 7: generated for the native 3,4 at 0.7/0.3
    LegacyMixedRow r;
    REQUIRE(parse_legacy_mixed_row(rows[2], r));
    CHECK(r.a == 3);
    CHECK(r.b == 4);
    CHECK(r.mix_b_percent == 30);
    CHECK(r.custom);
    CHECK_FALSE(r.origin_auto);
    CHECK(r.enabled);
    // stable ids: the generated row must not collide with the marker rows' u7 and u9
    CHECK(r.stable_id > 9);
    for (const std::string &row : rows)
        check_row_valid(row, 4);
    // ids line up: the parser, run with n = 4, finds all three as live rows 5, 6, 7
    CHECK(parse_legacy_mixed_definitions(s.mixed_filament_definitions, 4).rows.size() == 3);
    CHECK(c == before); // input untouched
}

TEST_CASE("Snapmaker shape: degenerate slots get a valid placeholder row and keep the count", "[MixedFilament][snapmaker_shape]")
{
    DynamicPrintConfig c = two_converted_one_native();
    SECTION("0 sentinel after a physical delete") {
        c.option<ConfigOptionStrings>("filament_mixed_components")->values[6] = "0,4";
        c.option<ConfigOptionStrings>("filament_mixed_engine_row")->values[6] = "";
    }
    SECTION("a == b") {
        c.option<ConfigOptionStrings>("filament_mixed_components")->values[6] = "2,2";
    }
    SECTION("single surviving component") {
        c.option<ConfigOptionStrings>("filament_mixed_components")->values[6] = "3";
    }
    SECTION("component above the physical count") {
        c.option<ConfigOptionStrings>("filament_mixed_components")->values[6] = "3,9";
    }
    SECTION("a marker row Snapmaker would skip (a == b) is replaced, not emitted") {
        c.option<ConfigOptionStrings>("filament_mixed_engine_row")->values[5] = "2,2,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u9";
    }
    SECTION("a marker row that is disabled holds no virtual id, so it is replaced") {
        c.option<ConfigOptionStrings>("filament_mixed_engine_row")->values[5] = "2,3,0,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u9";
    }
    const Shaped s = shape_of(c);
    REQUIRE(s.transformed);
    const std::vector<std::string> rows = split_rows(s.mixed_filament_definitions);
    REQUIRE(rows.size() == 3);
    for (const std::string &row : rows)
        check_row_valid(row, 4);
    CHECK_FALSE(s.warnings.empty());
}

TEST_CASE("Snapmaker shape: 3+ components are shown as their first pair, with a warning", "[MixedFilament][snapmaker_shape]")
{
    DynamicPrintConfig c = two_converted_one_native();
    c.option<ConfigOptionStrings>("filament_mixed_components")->values[6]      = "1,2,3";
    c.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios")->values[6] = "0.5000,0.2500,0.2500";
    const Shaped s = shape_of(c);
    REQUIRE(s.transformed);
    LegacyMixedRow r;
    REQUIRE(parse_legacy_mixed_row(split_rows(s.mixed_filament_definitions)[2], r));
    CHECK(r.a == 1);
    CHECK(r.b == 2);
    CHECK(r.mix_b_percent == 33);
    CHECK(s.warnings.size() == 1);
    // the full data is in the sidecar
    const nlohmann::json side = nlohmann::json::parse(s.sidecar);
    CHECK(side["keys"]["filament_mixed_components"]["v"].get<std::string>().find("1,2,3") != std::string::npos);
}

TEST_CASE("Snapmaker shape: the sidecar carries version, n, total, digest and the stripped tails", "[MixedFilament][snapmaker_shape]")
{
    const Shaped s = shape_of(two_converted_one_native());
    REQUIRE(s.transformed);
    const nlohmann::json side = nlohmann::json::parse(s.sidecar);
    CHECK(side["version"].get<int>() == kMixedSidecarVersion);
    CHECK(side["n"].get<size_t>() == 4);
    CHECK(side["total"].get<size_t>() == 7);
    CHECK(side["digest"].get<std::string>() == mixed_sidecar_digest(4, s.mixed_filament_definitions));
    CHECK(side["digest"].get<std::string>().size() == 16);
    // a different string or n gives a different digest
    CHECK(mixed_sidecar_digest(4, s.mixed_filament_definitions + "x") != side["digest"].get<std::string>());
    CHECK(mixed_sidecar_digest(5, s.mixed_filament_definitions) != side["digest"].get<std::string>());
    REQUIRE(side["keys"].contains("filament_settings_id"));
    CHECK(side["keys"]["filament_settings_id"]["idx"] == nlohmann::json::array({ 4, 5, 6 }));
    // the per-slot preset of the stripped slots is kept (converted slots take component A's preset)
    const std::string tail = side["keys"]["filament_settings_id"]["v"].get<std::string>();
    CHECK(tail.find("Preset1") != std::string::npos);
    CHECK(tail.find("Preset2") != std::string::npos);
    CHECK(tail.find("Preset3") != std::string::npos);
}

TEST_CASE("Snapmaker shape: idempotent on an already shaped config", "[MixedFilament][snapmaker_shape]")
{
    const Shaped first  = shape_of(two_converted_one_native());
    REQUIRE(first.transformed);
    const Shaped second = shape_of(first.config);
    CHECK_FALSE(second.transformed);
    CHECK(second.config == first.config);
    CHECK(second.mixed_filament_definitions.empty());
    CHECK(second.sidecar.empty());
}

TEST_CASE("Snapmaker shape: the input is not mutated", "[MixedFilament][snapmaker_shape]")
{
    const DynamicPrintConfig c      = two_converted_one_native();
    const DynamicPrintConfig before = c;
    shape_of(c);
    CHECK(c == before);
    CHECK(strs(c, "filament_colour").size() == 7);
}

TEST_CASE("Snapmaker shape: inverts the converter's extension, variants and groups included", "[MixedFilament][snapmaker_shape]")
{
    DynamicPrintConfig base = project_config(3);
    base.set_key_value("filament_self_index", new ConfigOptionInts({ 1, 1, 2, 2, 3, 3 }));
    base.set_key_value("filament_extruder_variant", new ConfigOptionStrings({ "L", "R", "L", "R", "L", "R" }));
    base.set_key_value("nozzle_temperature", new ConfigOptionInts({ 200, 201, 210, 211, 220, 221 }));
    base.set_key_value("inherits_group", new ConfigOptionStrings({ "P", "F1", "F2", "F3", "M" }));
    DynamicPrintConfig c = base;
    REQUIRE(convert_legacy_mixed_filaments(c, "2,3,1,1,40,0,g,w,m2,z0,xa0,xb0,d0,o0,u1;1,3,1,1,60,0,g,w,m2,z0,xa0,xb0,d0,o0,u2").slots_added == 2);
    REQUIRE(c.option<ConfigOptionInts>("nozzle_temperature")->values.size() == 10);
    REQUIRE(c.option<ConfigOptionStrings>("inherits_group")->values.size() == 7);
    const Shaped s = shape_of(c);
    REQUIRE(s.transformed);
    for (const char *key : { "filament_self_index", "filament_extruder_variant", "nozzle_temperature", "inherits_group", "filament_colour",
                             "filament_settings_id", "filament_map", "filament_colour_type" })
        { INFO(key); CHECK(*s.config.option(key) == *base.option(key)); }
}

TEST_CASE("Snapmaker shape: a non-contiguous mixed tail or fewer than two physical filaments is left alone", "[MixedFilament][snapmaker_shape]")
{
    DynamicPrintConfig c = two_converted_one_native();
    c.option<ConfigOptionBools>("filament_is_mixed")->values[5] = 0;
    Shaped s = shape_of(c);
    CHECK_FALSE(s.transformed);
    CHECK(s.config == c);
    CHECK_FALSE(s.warnings.empty());

    DynamicPrintConfig one = project_config(1);
    resize_mixed_filament_metadata(one, 1, 1);
    one.option<ConfigOptionBools>("filament_is_mixed")->values[0] = 1;
    s = shape_of(one);
    CHECK_FALSE(s.transformed);
}

// ---------------------------------------------------------------------------------------------------------------
// Slice 2: restore_from_sidecar, the strategy predicate and the real 3MF writer and loader. Same limit as above: the
// round trip proves our writer, loader and restore agree with each other, not that Snapmaker Orca reopens the file.
// ---------------------------------------------------------------------------------------------------------------

namespace {

// 4 physical + 5 mixed slots: two converted (marker) rows, a native pair, a 3-component blend and a degenerate slot.
DynamicPrintConfig rich_project()
{
    DynamicPrintConfig c = converted(4, "1,2,1,1,30,0,g,w,m2,z0,xa0,xb0,d0,o0,u7;2,3,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u9;"
                                        "3,4,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u11;1,4,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u12;"
                                        "2,4,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u13");
    auto set = [&](const char *key, size_t slot, const char *v) { c.option<ConfigOptionStrings>(key)->values[slot] = v; };
    set("filament_mixed_engine_row", 6, "");
    set("filament_mixed_components", 6, "3,4");
    set("filament_mixed_sublayer_ratios", 6, "0.7000,0.3000");
    set("filament_mixed_engine_row", 7, "");
    set("filament_mixed_components", 7, "1,2,3");
    set("filament_mixed_sublayer_ratios", 7, "0.5000,0.2500,0.2500");
    set("filament_mixed_gradient_curve", 7, "0,0;1,1");
    set("filament_mixed_engine_row", 8, "");
    set("filament_mixed_components", 8, "0,4");
    set("filament_colour", 7, "#123456");
    return c;
}

std::vector<std::string> sidecar_keys(const std::string &sidecar)
{
    std::vector<std::string> keys;
    for (const auto &item : nlohmann::json::parse(sidecar)["keys"].items())
        keys.push_back(item.key());
    return keys;
}

struct Written
{
    std::string              project_settings; // Metadata/project_settings.config as saved (before any tampering)
    DynamicPrintConfig       loaded;
    std::vector<std::string> plate_warnings;
    int                      object_extruder = 0;
    bool                     ok = false;
};

bool read_entry(const std::string &path, const std::string &entry, std::string &out)
{
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!open_zip_reader(&zip, path))
        return false;
    bool   found = false;
    mz_uint n    = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < n && !found; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st))
            continue;
        std::string name(st.m_filename);
        std::replace(name.begin(), name.end(), '\\', '/');
        if (boost::algorithm::iequals(name, entry)) {
            out.resize(st.m_uncomp_size);
            found = mz_zip_reader_extract_to_mem(&zip, i, out.data(), out.size(), 0) != 0;
        }
    }
    close_zip_reader(&zip);
    return found;
}

// Copies the archive `from` to `to`, replacing Metadata/project_settings.config with `replacement`.
void rewrite_project_settings(const std::string &from, const std::string &to, const std::string &replacement)
{
    mz_zip_archive in;
    mz_zip_zero_struct(&in);
    REQUIRE(open_zip_reader(&in, from));
    mz_zip_archive dst;
    mz_zip_zero_struct(&dst);
    REQUIRE(mz_zip_writer_init_file(&dst, to.c_str(), 0));
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&in); ++i) {
        mz_zip_archive_file_stat st;
        REQUIRE(mz_zip_reader_file_stat(&in, i, &st));
        std::vector<char> data(st.m_uncomp_size);
        if (!data.empty())
            REQUIRE(mz_zip_reader_extract_to_mem(&in, i, data.data(), data.size(), 0));
        const bool   is_cfg = std::string(st.m_filename) == "Metadata/project_settings.config";
        const void  *p      = is_cfg ? static_cast<const void *>(replacement.data()) : static_cast<const void *>(data.data());
        const size_t len    = is_cfg ? replacement.size() : data.size();
        REQUIRE(mz_zip_writer_add_mem(&dst, st.m_filename, p, len, MZ_DEFAULT_COMPRESSION));
    }
    REQUIRE(mz_zip_writer_finalize_archive(&dst));
    mz_zip_writer_end(&dst);
    close_zip_reader(&in);
}

// Saves `config` (plus `extras`) through the real writer, then reloads it through the real loader. `tamper` may
// edit the saved project_settings.config JSON before the reload.
Written save_and_reload(const DynamicPrintConfig &config, const std::vector<std::pair<std::string, std::string>> &extras,
                        const std::function<void(nlohmann::json &)> &tamper = nullptr)
{
    Written                  w;
    ScopedSlic3rTemporaryDir temporary_dir("orca_mixed_roundtrip");
    ScopedTemporaryFile      file(".3mf");
    std::string              path = file.string();

    Model        model;
    ModelObject *object = model.add_object();
    object->add_volume(make_cube(10., 10., 10.));
    object->add_instance();
    object->config.set_key_value("extruder", new ConfigOptionInt(6)); // a mixed slot id
    model.set_backup_path((temporary_dir.path() / "src").string());

    DynamicPrintConfig source = config;
    PlateData          plate;
    plate.plate_index = 0;
    StoreParams sp;
    sp.path                   = path;
    sp.model                  = &model;
    sp.config                 = &source;
    sp.strategy               = SaveStrategy::Zip64 | SaveStrategy::Silence;
    sp.extra_project_settings = extras;
    sp.plate_data_list.push_back(&plate);
    if (!store_bbs_3mf(sp))
        return w;
    if (!read_entry(path, "Metadata/project_settings.config", w.project_settings))
        return w;

    ScopedTemporaryFile edited_file(".3mf");
    if (tamper) {
        nlohmann::json j = nlohmann::json::parse(w.project_settings);
        tamper(j);
        rewrite_project_settings(path, edited_file.string(), j.dump(1, '\t', false));
        path = edited_file.string();
    }

    Model                     reloaded_model;
    ScopedTemporaryDir        loaded_backup("orca_mixed_roundtrip_loaded");
    reloaded_model.set_backup_path(loaded_backup.string());
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Enable };
    PlateDataPtrs             plates;
    std::vector<Preset *>     project_presets;
    bool   is_bbl_3mf = false, is_orca_3mf = false;
    Semver file_version;
    w.ok = load_bbs_3mf(path.c_str(), &w.loaded, &ctxt, &reloaded_model, &plates, &project_presets, &is_bbl_3mf, &is_orca_3mf,
                        &file_version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig);
    for (PlateData *pl : plates)
        if (pl != nullptr)
            for (const auto &sw : pl->warnings)
                w.plate_warnings.push_back(sw.msg);
    release_PlateData_list(plates);
    if (!reloaded_model.objects.empty())
        if (const auto *e = dynamic_cast<const ConfigOptionInt *>(reloaded_model.objects.front()->config.option("extruder")))
            w.object_extruder = e->value;
    return w;
}

std::vector<std::pair<std::string, std::string>> extras_of(const SnapmakerProjectShape &s)
{
    return { { "mixed_filament_definitions", s.mixed_filament_definitions }, { kMixedSidecarKey, s.sidecar } };
}

} // namespace

TEST_CASE("Snapmaker shape: only an explicit project save gets the Snapmaker shape", "[MixedFilament][snapmaker_shape]")
{
    // Plater::save_project
    CHECK(is_explicit_project_save(SaveStrategy::SplitModel | SaveStrategy::ShareMesh));
    CHECK(is_explicit_project_save(SaveStrategy::SplitModel | SaveStrategy::ShareMesh | SaveStrategy::FullPathSources));
    // sliced plate export, backup/autosave, copies, published, plain Silence exports
    CHECK_FALSE(is_explicit_project_save(SaveStrategy::Silence | SaveStrategy::SplitModel | SaveStrategy::WithGcode | SaveStrategy::SkipModel));
    CHECK_FALSE(is_explicit_project_save(SaveStrategy::Backup));
    CHECK_FALSE(is_explicit_project_save(SaveStrategy::SplitModel | SaveStrategy::ShareMesh | SaveStrategy::Silence));
    CHECK_FALSE(is_explicit_project_save(SaveStrategy::SplitModel | SaveStrategy::ShareMesh | SaveStrategy::Silence | SaveStrategy::MinimalPublished));
    CHECK_FALSE(is_explicit_project_save(SaveStrategy::Silence));
}

TEST_CASE("Snapmaker restore: shape then restore gives back the original config exactly", "[MixedFilament][snapmaker_shape]")
{
    const DynamicPrintConfig full = rich_project();
    REQUIRE(strs(full, "filament_colour").size() == 9);
    const Shaped s = shape_of(full);
    REQUIRE(s.transformed);
    DynamicPrintConfig restored;
    std::string        why;
    REQUIRE(restore_from_sidecar(s.config, s.mixed_filament_definitions, s.sidecar, restored, why));
    CHECK(why.empty());
    CHECK(restored == full);
}

TEST_CASE("Snapmaker restore: variants and groups come back too", "[MixedFilament][snapmaker_shape]")
{
    DynamicPrintConfig base = project_config(3);
    base.set_key_value("filament_self_index", new ConfigOptionInts({ 1, 1, 2, 2, 3, 3 }));
    base.set_key_value("filament_extruder_variant", new ConfigOptionStrings({ "L", "R", "L", "R", "L", "R" }));
    base.set_key_value("nozzle_temperature", new ConfigOptionInts({ 200, 201, 210, 211, 220, 221 }));
    base.set_key_value("inherits_group", new ConfigOptionStrings({ "P", "F1", "F2", "F3", "M" }));
    DynamicPrintConfig c = base;
    REQUIRE(convert_legacy_mixed_filaments(c, "2,3,1,1,40,0,g,w,m2,z0,xa0,xb0,d0,o0,u1;1,3,1,1,60,0,g,w,m2,z0,xa0,xb0,d0,o0,u2").slots_added == 2);
    const Shaped s = shape_of(c);
    REQUIRE(s.transformed);
    DynamicPrintConfig restored;
    std::string        why;
    const bool done = restore_from_sidecar(s.config, s.mixed_filament_definitions, s.sidecar, restored, why);
    INFO(why);
    REQUIRE(done);
    CHECK(restored == c);
}

TEST_CASE("Snapmaker restore: a stale, edited or malformed sidecar is refused and nothing is changed", "[MixedFilament][snapmaker_shape]")
{
    const Shaped s = shape_of(rich_project());
    REQUIRE(s.transformed);
    const DynamicPrintConfig marker   = project_config(2);
    DynamicPrintConfig       restored = marker;
    std::string              why;
    const nlohmann::json     good = nlohmann::json::parse(s.sidecar);

    SECTION("definitions edited after the save (digest)") {
        CHECK_FALSE(restore_from_sidecar(s.config, s.mixed_filament_definitions + ";1,2,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u99", s.sidecar, restored, why));
    }
    SECTION("physical filament count changed (n)") {
        DynamicPrintConfig five = s.config;
        five.option<ConfigOptionStrings>("filament_colour")->values.push_back("#000000");
        CHECK_FALSE(restore_from_sidecar(five, s.mixed_filament_definitions, s.sidecar, restored, why));
    }
    SECTION("no sidecar") { CHECK_FALSE(restore_from_sidecar(s.config, s.mixed_filament_definitions, "", restored, why)); }
    SECTION("garbage") { CHECK_FALSE(restore_from_sidecar(s.config, s.mixed_filament_definitions, "{not json", restored, why)); }
    SECTION("not an object") { CHECK_FALSE(restore_from_sidecar(s.config, s.mixed_filament_definitions, "[1,2]", restored, why)); }
    SECTION("unknown version") {
        nlohmann::json j = good;
        j["version"] = 99;
        CHECK_FALSE(restore_from_sidecar(s.config, s.mixed_filament_definitions, j.dump(), restored, why));
    }
    SECTION("a tail that does not fit (too few values)") {
        nlohmann::json j = good;
        j["keys"]["filament_settings_id"]["v"] = "OnlyOne";
        CHECK_FALSE(restore_from_sidecar(s.config, s.mixed_filament_definitions, j.dump(), restored, why));
    }
    SECTION("index out of range") {
        nlohmann::json j = good;
        j["keys"]["filament_settings_id"]["idx"] = nlohmann::json::array({ 4, 5, 99 });
        CHECK_FALSE(restore_from_sidecar(s.config, s.mixed_filament_definitions, j.dump(), restored, why));
    }
    SECTION("a key that is not a vector option") {
        nlohmann::json j = good;
        j["keys"]["layer_height"] = { { "idx", nlohmann::json::array({ 4 }) }, { "v", "0.2" } };
        CHECK_FALSE(restore_from_sidecar(s.config, s.mixed_filament_definitions, j.dump(), restored, why));
    }
    CHECK_FALSE(why.empty());
    CHECK(restored == marker); // never a partial restore
}

TEST_CASE("Snapmaker 3MF: a project without extras is written exactly as before", "[MixedFilament][snapmaker_shape][3mf]")
{
    const DynamicPrintConfig c = project_config(3);
    const Written            w = save_and_reload(c, {});
    REQUIRE(w.ok);
    std::ostringstream expected;
    c.save_to_json(expected, "project_settings", "project", SLIC3R_VERSION);
    CHECK(w.project_settings == expected.str());
    CHECK(w.project_settings.find("mixed_filament_definitions") == std::string::npos);
    CHECK(w.project_settings.find(kMixedSidecarKey) == std::string::npos);
    // and a config without mixed slots yields no extras to write
    CHECK_FALSE(shape_of(c).transformed);
}

TEST_CASE("Snapmaker 3MF: extras are the only difference in project_settings.config", "[MixedFilament][snapmaker_shape][3mf]")
{
    const DynamicPrintConfig c = project_config(3);
    const Written w = save_and_reload(c, { { "mixed_filament_definitions", "1,2,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u1" }, { "orca_ai_mixed_slots", "{}" } });
    REQUIRE(w.ok);
    std::ostringstream plain;
    c.save_to_json(plain, "project_settings", "project", SLIC3R_VERSION);
    nlohmann::json expected = nlohmann::json::parse(plain.str());
    expected["mixed_filament_definitions"] = "1,2,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u1";
    expected["orca_ai_mixed_slots"]        = "{}";
    CHECK(nlohmann::json::parse(w.project_settings) == expected);
}

TEST_CASE("Snapmaker 3MF: save, reopen and restore round trip", "[MixedFilament][snapmaker_shape][3mf]")
{
    const DynamicPrintConfig full = rich_project();
    const Shaped             s    = shape_of(full);
    REQUIRE(s.transformed);

    const Written w = save_and_reload(s.config, extras_of(s));
    REQUIRE(w.ok);

    // The saved file is in Snapmaker's shape: n-long vectors, definitions in Snapmaker's format, sidecar present.
    const nlohmann::json saved = nlohmann::json::parse(w.project_settings);
    CHECK(saved["filament_colour"].size() == 4);
    CHECK(saved["filament_settings_id"].size() == 4);
    CHECK(saved["filament_map"].size() == 4);
    CHECK(saved["filament_is_mixed"].size() == 4);
    REQUIRE(saved["mixed_filament_definitions"].is_string());
    CHECK(saved["mixed_filament_definitions"].get<std::string>() == s.mixed_filament_definitions);
    const std::vector<std::string> rows = split_rows(s.mixed_filament_definitions);
    REQUIRE(rows.size() == 5);
    for (const std::string &row : rows)
        check_row_valid(row, 4);
    CHECK(parse_legacy_mixed_definitions(saved["mixed_filament_definitions"].get<std::string>(), 4).rows.size() == 5);
    REQUIRE(saved[kMixedSidecarKey].is_string());

    // Reopened by this program: the full slot data is back, field for field, and nothing was converted.
    for (const std::string &key : sidecar_keys(s.sidecar)) {
        INFO(key);
        REQUIRE(w.loaded.option(key) != nullptr);
        CHECK(*w.loaded.option(key) == *full.option(key));
    }
    CHECK(strs(w.loaded, "filament_colour").size() == 9);
    CHECK(strs(w.loaded, "filament_mixed_components")[7] == "1,2,3");
    CHECK(strs(w.loaded, "filament_mixed_gradient_curve")[7] == "0,0;1,1");
    CHECK(strs(w.loaded, "filament_mixed_engine_row")[4] == strs(full, "filament_mixed_engine_row")[4]);
    CHECK(w.object_extruder == 6);
    for (const std::string &m : w.plate_warnings) {
        INFO(m);
        CHECK(m.find("both") == std::string::npos);
        CHECK(m.find("could not be used") == std::string::npos);
    }
}

TEST_CASE("Snapmaker 3MF: a stale or stripped sidecar falls back to the converter", "[MixedFilament][snapmaker_shape][3mf]")
{
    const Shaped s = shape_of(rich_project());
    REQUIRE(s.transformed);

    Written w;
    SECTION("digest tampered") {
        w = save_and_reload(s.config, extras_of(s), [](nlohmann::json &j) {
            nlohmann::json side = nlohmann::json::parse(j[kMixedSidecarKey].get<std::string>());
            side["digest"]      = "0000000000000000";
            j[kMixedSidecarKey] = side.dump();
        });
        REQUIRE(w.ok);
        }
    SECTION("sidecar garbage") {
        w = save_and_reload(s.config, extras_of(s), [](nlohmann::json &j) { j[kMixedSidecarKey] = "{nope"; });
        REQUIRE(w.ok);
    }
    SECTION("sidecar removed (a Snapmaker round trip)") {
        w = save_and_reload(s.config, extras_of(s), [](nlohmann::json &j) { j.erase(kMixedSidecarKey); });
        REQUIRE(w.ok);
    }
    // Converter path: one slot per live row (5) after the 4 physical filaments; sidecar-only data is gone.
    CHECK(strs(w.loaded, "filament_colour").size() == 9);
    CHECK(w.loaded.option<ConfigOptionBools>("filament_is_mixed")->values.size() == 9);
    CHECK(strs(w.loaded, "filament_mixed_components")[7] != "1,2,3");
}

// ---------------------------------------------------------------------------------------------------------------
// Fix round (#122): consistency of a restored sidecar, exception safety, stronger round trips.
// ---------------------------------------------------------------------------------------------------------------

namespace {

bool warned_about_sidecar(const Written &w)
{
    for (const std::string &m : w.plate_warnings)
        if (m.find("could not be used") != std::string::npos)
            return true;
    return false;
}

// True when the reopened project took the sidecar path: the full slot list is back with the sidecar-only data of the
// 3-component blend (the converter keeps only the first pair), and no warning says the sidecar was refused.
void check_restore_path_taken(const Written &w, const DynamicPrintConfig &full)
{
    CHECK_FALSE(warned_about_sidecar(w));
    CHECK(strs(w.loaded, "filament_colour").size() == strs(full, "filament_colour").size());
    CHECK(strs(w.loaded, "filament_mixed_components")[7] == "1,2,3");
    CHECK(strs(w.loaded, "filament_mixed_gradient_curve")[7] == "0,0;1,1");
}

// Minimal pure-JSON edit of a good sidecar, restore must refuse it and leave the target untouched.
void check_refused(const Shaped &s, const std::string &sidecar, const std::string &definitions)
{
    const DynamicPrintConfig marker   = project_config(2);
    DynamicPrintConfig       restored = marker;
    std::string              why;
    CHECK_FALSE(restore_from_sidecar(s.config, definitions, sidecar, restored, why));
    CHECK_FALSE(why.empty());
    CHECK(restored == marker);
}

} // namespace

TEST_CASE("Snapmaker restore: a sidecar that passes the digest but is inconsistent fails closed", "[MixedFilament][snapmaker_shape]")
{
    const Shaped s = shape_of(rich_project());
    REQUIRE(s.transformed);
    const nlohmann::json good = nlohmann::json::parse(s.sidecar);
    const std::string   &defs = s.mixed_filament_definitions;
    REQUIRE(good["keys"].contains("filament_settings_id"));
    REQUIRE(good["keys"].contains("filament_is_mixed"));

    // control: the unedited sidecar restores
    DynamicPrintConfig ok;
    std::string        why;
    REQUIRE(restore_from_sidecar(s.config, defs, s.sidecar, ok, why));

    SECTION("empty keys (digest still valid)") {
        nlohmann::json j = good;
        j["keys"]        = nlohmann::json::object();
        check_refused(s, j.dump(), defs);
    }
    SECTION("a per-filament key missing") {
        nlohmann::json j = good;
        j["keys"].erase("filament_settings_id");
        check_refused(s, j.dump(), defs);
    }
    SECTION("filament_is_mixed missing") {
        nlohmann::json j = good;
        j["keys"].erase("filament_is_mixed");
        check_refused(s, j.dump(), defs);
    }
    SECTION("one key has a different index count than the others") {
        nlohmann::json j = good;
        auto &e          = j["keys"]["filament_settings_id"];
        e["idx"].erase(e["idx"].end() - 1);
        e["v"] = "A;B;C;D"; // 4 values for 4 indices: parses, but is one short of the 5 mixed slots
        check_refused(s, j.dump(), defs);
    }
    SECTION("one key has an extra index") {
        nlohmann::json j = good;
        auto &e          = j["keys"]["filament_settings_id"];
        e["idx"].push_back(9);
        e["v"] = e["v"].get<std::string>() + ";X";
        check_refused(s, j.dump(), defs);
    }
    SECTION("oversize index list") {
        nlohmann::json j = good;
        auto &e          = j["keys"]["filament_settings_id"];
        e["idx"]         = nlohmann::json::array();
        for (int i = 4; i < 5000; ++i)
            e["idx"].push_back(i);
        check_refused(s, j.dump(), defs);
    }
    SECTION("wrong tail length for a colour key") {
        nlohmann::json j = good;
        j["keys"]["filament_colour"]["v"] = "#000000;#111111";
        check_refused(s, j.dump(), defs);
    }
    SECTION("definitions row count does not match the slot count (digest recomputed)") {
        std::vector<std::string> rows = split_rows(defs);
        REQUIRE(rows.size() == 5);
        rows.pop_back();
        std::string fewer;
        for (const std::string &r : rows)
            fewer += (fewer.empty() ? "" : ";") + r;
        nlohmann::json j = good;
        j["digest"]      = mixed_sidecar_digest(4, fewer);
        check_refused(s, j.dump(), fewer);
    }
    SECTION("total claims more slots than the definitions and keys carry") {
        nlohmann::json j = good;
        j["total"]       = 10;
        check_refused(s, j.dump(), defs);
    }
}

TEST_CASE("Snapmaker 3MF: a digest-valid but empty sidecar makes the loader use the converter", "[MixedFilament][snapmaker_shape][3mf]")
{
    const Shaped s = shape_of(rich_project());
    REQUIRE(s.transformed);
    const Written w = save_and_reload(s.config, extras_of(s), [](nlohmann::json &j) {
        nlohmann::json side = nlohmann::json::parse(j[kMixedSidecarKey].get<std::string>());
        side["keys"]        = nlohmann::json::object(); // digest is untouched and still valid
        j[kMixedSidecarKey] = side.dump();
    });
    REQUIRE(w.ok);
    CHECK(warned_about_sidecar(w));
    // converter path: 5 live rows after the 4 physical filaments, 3-component data gone, the project still has 9 slots
    CHECK(strs(w.loaded, "filament_colour").size() == 9);
    CHECK(w.loaded.option<ConfigOptionBools>("filament_is_mixed")->values.size() == 9);
    CHECK(strs(w.loaded, "filament_mixed_components")[7] != "1,2,3");
}

TEST_CASE("Snapmaker 3MF: the warning on a refused sidecar says the sidecar does not match", "[MixedFilament][snapmaker_shape][3mf]")
{
    const Shaped s = shape_of(rich_project());
    const Written w = save_and_reload(s.config, extras_of(s), [](nlohmann::json &j) {
        nlohmann::json side = nlohmann::json::parse(j[kMixedSidecarKey].get<std::string>());
        side["digest"]      = "0000000000000000";
        j[kMixedSidecarKey] = side.dump();
    });
    REQUIRE(w.ok);
    bool found = false;
    for (const std::string &m : w.plate_warnings) {
        if (m.find("could not be used") == std::string::npos)
            continue;
        found = true;
        CHECK(m.find("stale or was edited") != std::string::npos);
        CHECK(m.find("edited by another program") == std::string::npos);
    }
    CHECK(found);
}

TEST_CASE("Snapmaker 3MF: tails with special characters round trip through writer and loader", "[MixedFilament][snapmaker_shape][3mf]")
{
    DynamicPrintConfig full = rich_project();
    const std::string  nasty = "Pre\"set; one\nline \\ back\ttab 'q' \xC3\xBC" "n\xC3\xAF" "c\xC3\xB6" "de \xE6\x97\xA5\xE6\x9C\xAC {\"j\":[1,2]}";
    full.option<ConfigOptionStrings>("filament_settings_id")->values[5]  = nasty;
    full.option<ConfigOptionStrings>("filament_settings_id")->values[7]  = ";;;\\\\\"\"";
    full.option<ConfigOptionStrings>("filament_mixed_gradient_curve")->values[7] = "0,0;1,1";
    const Shaped s = shape_of(full);
    REQUIRE(s.transformed);
    const Written w = save_and_reload(s.config, extras_of(s));
    REQUIRE(w.ok);
    check_restore_path_taken(w, full);
    CHECK(strs(w.loaded, "filament_settings_id")[5] == nasty);
    CHECK(strs(w.loaded, "filament_settings_id")[7] == ";;;\\\\\"\"");
    for (const std::string &key : sidecar_keys(s.sidecar)) {
        INFO(key);
        CHECK(*w.loaded.option(key) == *full.option(key));
    }
}

TEST_CASE("Snapmaker shape: invalid UTF-8 in a stripped tail does not throw and still round trips a valid file", "[MixedFilament][snapmaker_shape][3mf]")
{
    DynamicPrintConfig full = rich_project();
    full.option<ConfigOptionStrings>("filament_settings_id")->values[6] = "bad\xFF\xFE" "name";
    Shaped s;
    REQUIRE_NOTHROW(s = shape_of(full));
    REQUIRE(s.transformed);
    REQUIRE_NOTHROW(nlohmann::json::parse(s.sidecar)); // the sidecar itself is valid JSON
    const Written w = save_and_reload(s.config, extras_of(s));
    REQUIRE(w.ok);
    check_restore_path_taken(w, full);
    // Replacement: each invalid byte became U+FFFD (EF BF BD), so this one value reads back altered; all else is exact.
    CHECK(strs(w.loaded, "filament_settings_id")[6] == "bad\xEF\xBF\xBD\xEF\xBF\xBD" "name");
    CHECK(strs(w.loaded, "filament_settings_id")[5] == strs(full, "filament_settings_id")[5]);
}

TEST_CASE("Snapmaker 3MF: variants and groups round trip through writer and loader", "[MixedFilament][snapmaker_shape][3mf]")
{
    DynamicPrintConfig base = project_config(3);
    base.set_key_value("filament_self_index", new ConfigOptionInts({ 1, 1, 2, 2, 3, 3 }));
    base.set_key_value("filament_extruder_variant", new ConfigOptionStrings({ "L", "R", "L", "R", "L", "R" }));
    base.set_key_value("nozzle_temperature", new ConfigOptionInts({ 200, 201, 210, 211, 220, 221 }));
    base.set_key_value("inherits_group", new ConfigOptionStrings({ "P", "F1", "F2", "F3", "M" }));
    // A realistic project has every variant-keyed option at the variant count; the loader inflates a shorter one to the
    // saved variant count, which the restore (correctly, fail closed) cannot tell from a missing sidecar key.
    for (const std::string &key : filament_options_with_variant)
        if (auto *opt = dynamic_cast<ConfigOptionVectorBase *>(base.option(key)); opt != nullptr && opt->size() <= 1) {
            std::unique_ptr<ConfigOption> one(opt->clone());
            opt->resize(6, one.get());
        }
    DynamicPrintConfig c = base;
    REQUIRE(convert_legacy_mixed_filaments(c, "2,3,1,1,40,0,g,w,m2,z0,xa0,xb0,d0,o0,u1;1,3,1,1,60,0,g,w,m2,z0,xa0,xb0,d0,o0,u2").slots_added == 2);
    const Shaped s = shape_of(c);
    REQUIRE(s.transformed);
    const Written w = save_and_reload(s.config, extras_of(s));
    REQUIRE(w.ok);
    CHECK_FALSE(warned_about_sidecar(w));
    CHECK(strs(w.loaded, "filament_colour").size() == 5);
    for (const std::string &key : { "filament_self_index", "filament_extruder_variant", "nozzle_temperature", "inherits_group", "filament_mixed_engine_row" }) {
        INFO(key);
        REQUIRE(w.loaded.option(key) != nullptr);
        CHECK(*w.loaded.option(key) == *c.option(key));
    }
}

TEST_CASE("Snapmaker 3MF: the shaped project_settings.config is the plain dump plus exactly the two extra keys, byte for byte", "[MixedFilament][snapmaker_shape][3mf]")
{
    const Shaped  s = shape_of(rich_project());
    const Written w = save_and_reload(s.config, extras_of(s));
    REQUIRE(w.ok);
    // Independent of the writer's own JSON code: take the plain save_to_json text of the same shaped config and splice
    // the two lines in textually at their sorted positions, escaping only the scalar string values.
    std::ostringstream plain;
    s.config.save_to_json(plain, "project_settings", "project", SLIC3R_VERSION);
    std::vector<std::string> lines;
    {
        std::istringstream in(plain.str());
        for (std::string l; std::getline(in, l);)
            lines.push_back(l);
    }
    for (const auto &kv : std::vector<std::pair<std::string, std::string>>{ { "mixed_filament_definitions", s.mixed_filament_definitions }, { kMixedSidecarKey, s.sidecar } }) {
        const std::string newline = "\t\"" + kv.first + "\": " + nlohmann::json(kv.second).dump() + ",";
        size_t            at      = lines.size();
        for (size_t i = 0; i < lines.size(); ++i)
            if (lines[i].size() > 2 && lines[i][0] == '\t' && lines[i][1] == '"') {
                const std::string key = lines[i].substr(2, lines[i].find('"', 2) - 2);
                if (key > kv.first) {
                    at = i;
                    break;
                }
            }
        REQUIRE(at < lines.size());
        lines.insert(lines.begin() + at, newline);
    }
    std::string expected;
    for (const std::string &l : lines)
        expected += l + "\n";
    CHECK(w.project_settings == expected);
}

TEST_CASE("snapmaker_shape_user_notice turns shape warnings into one bounded sentence", "[snapmaker_shape]")
{
    CHECK(snapmaker_shape_user_notice({}).empty());

    const std::string tail = " OpenSnorca keeps it in full.";
    CHECK(snapmaker_shape_user_notice({ "Mixed filament 5 blends 3 filaments; Snapmaker shows it as a blend of the first two." }) ==
          "Mixed filament detail is simplified for Snapmaker Orca (1 blend(s) of 3+ filaments)." + tail);
    CHECK(snapmaker_shape_user_notice({ "Mixed filament 6 has no valid pair of filaments; Snapmaker shows it as a placeholder." }) ==
          "Mixed filament detail is simplified for Snapmaker Orca (1 placeholder slot(s))." + tail);
    CHECK(snapmaker_shape_user_notice({ "Mixed filament 7 has a Snapmaker row that Snapmaker would skip; it was written from the slot's components instead." }) ==
          "Mixed filament detail is simplified for Snapmaker Orca (1 rebuilt row(s))." + tail);
    CHECK(snapmaker_shape_user_notice({ "something new" }) ==
          "Mixed filament detail is simplified for Snapmaker Orca (1 other)." + tail);

    std::vector<std::string> many;
    for (int i = 0; i < 200; ++i) {
        many.push_back("Mixed filament " + std::to_string(i) + " blends 3 filaments; Snapmaker shows it as a blend of the first two.");
        many.push_back("Mixed filament " + std::to_string(i) + " has no valid pair of filaments; Snapmaker shows it as a placeholder.");
    }
    const std::string msg = snapmaker_shape_user_notice(many);
    CHECK(msg == "Mixed filament detail is simplified for Snapmaker Orca (200 blend(s) of 3+ filaments, 200 placeholder slot(s))." + tail);
    CHECK(msg.size() < 200);
}

TEST_CASE("snapmaker_import_user_notice turns converter warnings into one bounded message", "[snapmaker_import_notice]")
{
    CHECK(snapmaker_import_user_notice({}).empty());

    const std::string head = "Mixed filaments from a Snapmaker project were not carried over exactly (";
    const std::string tail = "). Check them before slicing.";
    CHECK(snapmaker_import_user_notice({ "3 Snapmaker mixed filament row(s) were malformed or referenced missing filaments and were skipped." }) ==
          head + "1 unrepresentable or skipped" + tail);
    CHECK(snapmaker_import_user_notice({ "Snapmaker mixed filament 5 (filaments 1+2) uses a pattern, which this build cannot represent; it was converted to a 50/50 blend of the two filaments." }) ==
          head + "1 unrepresentable or skipped" + tail);
    CHECK(snapmaker_import_user_notice({ "Snapmaker mixed filament 5 (filaments 1+1) is a pure single-filament mix; it was stored as a 1/99 blend." }) ==
          head + "1 unrepresentable or skipped" + tail);
    CHECK(snapmaker_import_user_notice({ "This project has 40 filaments; Snapmaker mixed filament definitions are only converted up to 32, so they were ignored." }) ==
          head + "1 over the 32-slot limit" + tail);
    CHECK(snapmaker_import_user_notice({ "The Snapmaker project defines 9 mixed filaments but only 2 more slots fit (limit 32); the rest were dropped, so objects using them fall back to filament 1." }) ==
          head + "1 over the 32-slot limit" + tail);
    CHECK(snapmaker_import_user_notice({ "This project carries both Snapmaker mixed filament definitions and mixed filament slots; the slots were kept and the Snapmaker definitions ignored." }) ==
          head + "1 Snapmaker definitions ignored" + tail);
    CHECK(snapmaker_import_user_notice({ "This project's mixed filament details (saved by this program) could not be used (digest mismatch); the mixed filaments were rebuilt from the Snapmaker definitions and some detail may be lost." }) ==
          head + "1 saved detail unusable" + tail);
    CHECK(snapmaker_import_user_notice({ "something new" }) == head + "1 other" + tail);

    CHECK(snapmaker_import_user_notice({ "x row(s) were malformed", "something new", "something newer" }) ==
          head + "1 unrepresentable or skipped, 2 other" + tail);

    std::vector<std::string> many;
    for (int i = 0; i < 300; ++i) {
        many.push_back("Snapmaker mixed filament " + std::to_string(i) + " uses a pattern, which this build cannot represent; it was converted.");
        many.push_back("unknown " + std::to_string(i));
    }
    const std::string msg = snapmaker_import_user_notice(many);
    CHECK(msg == head + "300 unrepresentable or skipped, 300 other" + tail);
    CHECK(msg.size() < 200);
}

// ---- flush grids (#128 item 2): Snapmaker sizes them by the physical count (n x n matrix, 2 x n vector) ----

namespace {

// Distinct cell values (100*row + col) so a transposed or mis-strided cut is visible.
DynamicPrintConfig with_flush(DynamicPrintConfig c, size_t blocks = 1)
{
    const size_t total = c.option<ConfigOptionStrings>("filament_colour")->values.size();
    std::vector<double> m, v;
    for (size_t b = 0; b < blocks; ++b)
        for (size_t i = 0; i < total; ++i)
            for (size_t j = 0; j < total; ++j)
                m.push_back(1000. * b + 100. * i + j);
    for (size_t i = 0; i < 2 * total; ++i)
        v.push_back(140. + i);
    c.set_key_value("flush_volumes_matrix", new ConfigOptionFloats(m));
    c.set_key_value("flush_volumes_vector", new ConfigOptionFloats(v));
    return c;
}

const std::vector<double> &flo(const DynamicPrintConfig &c, const char *key) { return c.option<ConfigOptionFloats>(key)->values; }

} // namespace

TEST_CASE("Snapmaker flush: n physical plus k mixed shapes to 4x4 and 2x4", "[MixedFilament][snapmaker_shape]")
{
    const DynamicPrintConfig full = with_flush(two_converted_one_native());
    REQUIRE(flo(full, "flush_volumes_matrix").size() == 49);
    REQUIRE(flo(full, "flush_volumes_vector").size() == 14);
    const Shaped s = shape_of(full);
    REQUIRE(s.transformed);
    // Literals from the real Snapmaker 2.4.1 project with 4 physical filaments.
    CHECK(flo(s.config, "flush_volumes_matrix").size() == 16);
    CHECK(flo(s.config, "flush_volumes_vector").size() == 8);
    for (size_t i = 0; i < 4; ++i)
        for (size_t j = 0; j < 4; ++j)
            CHECK(flo(s.config, "flush_volumes_matrix")[i * 4 + j] == 100. * i + j);
    for (size_t i = 0; i < 8; ++i)
        CHECK(flo(s.config, "flush_volumes_vector")[i] == 140. + i);
}

TEST_CASE("Snapmaker flush: shape then restore is exact for matrix and vector", "[MixedFilament][snapmaker_shape]")
{
    for (size_t blocks : { size_t(1), size_t(2) }) {
        INFO("blocks " << blocks);
        const DynamicPrintConfig full = with_flush(two_converted_one_native(), blocks);
        const Shaped             s    = shape_of(full);
        REQUIRE(s.transformed);
        CHECK(flo(s.config, "flush_volumes_matrix").size() == 16);
        DynamicPrintConfig restored;
        std::string        why;
        REQUIRE(restore_from_sidecar(s.config, s.mixed_filament_definitions, s.sidecar, restored, why));
        CHECK(flo(restored, "flush_volumes_matrix") == flo(full, "flush_volumes_matrix"));
        CHECK(flo(restored, "flush_volumes_vector") == flo(full, "flush_volumes_vector"));
        CHECK(restored == full);
    }
}

TEST_CASE("Snapmaker flush: odd lengths are left alone, no sidecar entry, restore still passes", "[MixedFilament][snapmaker_shape]")
{
    DynamicPrintConfig full = with_flush(two_converted_one_native());
    full.set_key_value("flush_volumes_matrix", new ConfigOptionFloats(std::vector<double>(50, 1.)));
    full.set_key_value("flush_volumes_vector", new ConfigOptionFloats(std::vector<double>(13, 2.)));
    const Shaped s = shape_of(full);
    REQUIRE(s.transformed);
    CHECK(flo(s.config, "flush_volumes_matrix").size() == 50);
    CHECK(flo(s.config, "flush_volumes_vector").size() == 13);
    const nlohmann::json side = nlohmann::json::parse(s.sidecar);
    CHECK_FALSE(side["keys"].contains("flush_volumes_matrix"));
    CHECK_FALSE(side["keys"].contains("flush_volumes_vector"));
    DynamicPrintConfig restored;
    std::string        why;
    REQUIRE(restore_from_sidecar(s.config, s.mixed_filament_definitions, s.sidecar, restored, why));
    CHECK(restored == full);
}

TEST_CASE("Snapmaker flush: a tampered flush entry fails closed and changes nothing", "[MixedFilament][snapmaker_shape]")
{
    const Shaped s = shape_of(with_flush(two_converted_one_native()));
    REQUIRE(s.transformed);
    const DynamicPrintConfig marker   = project_config(2);
    DynamicPrintConfig       restored = marker;
    std::string              why;
    nlohmann::json           j = nlohmann::json::parse(s.sidecar);
    REQUIRE(j["keys"]["flush_volumes_matrix"]["idx"].size() == 33);
    REQUIRE(j["keys"]["flush_volumes_vector"]["idx"].size() == 6);

    SECTION("matrix with one removed cell fewer") {
        j["keys"]["flush_volumes_matrix"]["idx"].erase(j["keys"]["flush_volumes_matrix"]["idx"].size() - 1);
        j["keys"]["flush_volumes_matrix"]["v"] = "1";
    }
    SECTION("matrix removed index out of range") { j["keys"]["flush_volumes_matrix"]["idx"][0] = 4999; }
    SECTION("matrix tail with the wrong number of values") { j["keys"]["flush_volumes_matrix"]["v"] = "1,2,3"; }
    SECTION("vector with the wrong number of removed values") {
        j["keys"]["flush_volumes_vector"]["idx"] = nlohmann::json::array({ 8, 9, 10, 11, 12 });
        j["keys"]["flush_volumes_vector"]["v"]   = "1,2,3,4,5";
    }
    SECTION("matrix restored to a size that is not a whole number of blocks") {
        // 17 removed cells over a 16-cell kept block: 33 total, not a multiple of 49.
        auto &idx = j["keys"]["flush_volumes_matrix"]["idx"];
        idx.erase(idx.size() - 1);
        j["keys"]["flush_volumes_matrix"]["v"] = nlohmann::json(std::string(
            "1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1"));
    }
    CHECK_FALSE(restore_from_sidecar(s.config, s.mixed_filament_definitions, j.dump(), restored, why));
    CHECK_FALSE(why.empty());
    CHECK(restored == marker);
}

TEST_CASE("Snapmaker flush: the digest and n checks still hold", "[MixedFilament][snapmaker_shape]")
{
    const Shaped s = shape_of(with_flush(two_converted_one_native()));
    REQUIRE(s.transformed);
    DynamicPrintConfig restored;
    std::string        why;
    CHECK_FALSE(restore_from_sidecar(s.config, s.mixed_filament_definitions + ";1,2,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u99", s.sidecar, restored, why));
    DynamicPrintConfig five = s.config;
    five.option<ConfigOptionStrings>("filament_colour")->values.push_back("#000000");
    CHECK_FALSE(restore_from_sidecar(five, s.mixed_filament_definitions, s.sidecar, restored, why));
    CHECK(nlohmann::json::parse(s.sidecar)["version"].get<int>() == 1);
}

TEST_CASE("Snapmaker flush: a config without mixed slots keeps its flush keys", "[MixedFilament][snapmaker_shape]")
{
    const DynamicPrintConfig c = with_flush(project_config(3));
    const Shaped             s = shape_of(c);
    CHECK_FALSE(s.transformed);
    CHECK(s.config == c);
    CHECK(flo(s.config, "flush_volumes_matrix").size() == 9);
}

TEST_CASE("Snapmaker 3MF: the file carries a 4x4 matrix and 2x4 vector, our reopen restores the full grid", "[MixedFilament][snapmaker_shape][3mf]")
{
    const DynamicPrintConfig full = with_flush(two_converted_one_native());
    const Shaped             s    = shape_of(full);
    REQUIRE(s.transformed);
    const Written w = save_and_reload(s.config, extras_of(s));
    REQUIRE(w.ok);
    const nlohmann::json saved = nlohmann::json::parse(w.project_settings);
    CHECK(saved["flush_volumes_matrix"].size() == 16);
    CHECK(saved["flush_volumes_vector"].size() == 8);
    CHECK(flo(w.loaded, "flush_volumes_matrix") == flo(full, "flush_volumes_matrix"));
    CHECK(flo(w.loaded, "flush_volumes_vector") == flo(full, "flush_volumes_vector"));
}
