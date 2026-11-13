#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/FilamentSort.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <vector>

using namespace Slic3r::GUI;

namespace
{

FilamentSortItem make_item(const char *display_name,
                           const char *vendor,
                           const char *filament_product,
                           size_t original_index)
{
    FilamentSortItem item;
    item.display_name     = wxString::FromUTF8(display_name);
    item.vendor           = vendor;
    item.filament_product = filament_product;
    item.original_index   = original_index;
    return item;
}

FilamentOrder parse_order(const char *json)
{
    std::istringstream stream(json);
    return FilamentOrder::from_stream(stream);
}

} // namespace

TEST_CASE("FilamentOrder parses valid configuration and matches vendor and product case-insensitively",
          "[GUI][FilamentSort]")
{
    const FilamentOrder order = parse_order(R"({
        "schema_version": 1,
        "sections": {
            "high_flow": {
                "not_recommended_filaments": ["PLA Wood"],
                "unavailable_filaments": ["TPU 85A"]
            },
            "filament_order": {
                "Snapmaker": ["PLA Matte", "PLA SnapSpeed"]
            }
        }
    })");

    REQUIRE_FALSE(order.empty());
    CHECK(order.rank("Snapmaker", "PLA Matte") == 0);
    CHECK(order.rank("sNaPmAkEr", "PLA SnapSpeed") == 1);
    // Product names are authored in both the preset files and filament_allow_list.json, so a casing drift
    // must not drop the entry: they match case-insensitively, like the vendor key.
    CHECK(order.rank("Snapmaker", "pla matte") == 0);
    CHECK(order.rank("Snapmaker", "Pla sNApsPeed") == 1);
    CHECK(order.rank("sNaPmAkEr", "plA mATTe") == 0);
    CHECK(order.rank("Generic", "PLA Matte") == std::numeric_limits<size_t>::max());
    CHECK(order.rank("Snapmaker", "Unknown") == std::numeric_limits<size_t>::max());
}

TEST_CASE("FilamentOrder rejects invalid configurations and supports sort fallback", "[GUI][FilamentSort]")
{
    const std::vector<const char *> invalid_configs{
        "{",
        R"({"schema_version": 1, "order": {"Snapmaker": ["PLA Matte"]}})",
        R"({"schema_version": 2, "sections": {"filament_order": {"Snapmaker": ["PLA Matte"]}}})",
        R"({"schema_version": 1.0, "sections": {"filament_order": {"Snapmaker": ["PLA Matte"]}}})",
        R"({"schema_version": 1, "sections": []})",
        R"({"schema_version": 1, "sections": {}})",
        R"({"schema_version": 1, "sections": {"filament_order": []}})",
        R"({"schema_version": 1, "sections": {"filament_order": {"Snapmaker": []}}})",
        R"({"schema_version": 1, "sections": {"filament_order": {"Snapmaker": [3]}}})",
        R"({"schema_version": 1, "sections": {"filament_order": {"Snapmaker": [""]}}})",
        R"({"schema_version": 1, "sections": {"filament_order": {"Snapmaker": "PLA Matte"}}})",
        R"({"schema_version": 1, "sections": {"filament_order": {"": ["PLA Matte"]}}})",
    };

    for (const char *json : invalid_configs)
    {
        CHECK(parse_order(json).empty());
    }

    const SystemFilamentSorter sorter(parse_order("{"));
    const FilamentSortItem first  = make_item("Alpha", "Snapmaker", "Unknown", 0);
    const FilamentSortItem second = make_item("Beta", "Snapmaker", "Unknown", 1);
    CHECK(sorter.less(first, second));
    CHECK_FALSE(sorter.less(second, first));
}

TEST_CASE("FilamentSorter orders names by code point and keeps the original order for ties", "[GUI][FilamentSort]")
{
    const FilamentSorter sorter;
    const FilamentSortItem alpha = make_item("alpha", "", "", 0);
    const FilamentSortItem beta  = make_item("Beta", "", "", 1);
    const FilamentSortItem first = make_item("PLA", "", "", 1);
    const FilamentSortItem second = make_item("pla", "", "", 2);

    // Case-sensitive, like the upstream Bambu comparator: uppercase precedes lowercase, so this pair
    // is ordered by code point rather than by the original index.
    CHECK(sorter.less(beta, alpha));
    CHECK_FALSE(sorter.less(alpha, beta));
    CHECK(sorter.less(first, second));
    CHECK_FALSE(sorter.less(second, first));

    // The characters between 'Z' and 'a' sort after uppercase and before lowercase.
    const FilamentSortItem underscore = make_item("_", "", "", 3);
    const FilamentSortItem upper_a    = make_item("A", "", "", 4);
    const FilamentSortItem lower_a    = make_item("a", "", "", 5);
    CHECK(sorter.less(upper_a, underscore));
    CHECK(sorter.less(underscore, lower_a));

    // Identical names still keep the original order, since std::sort is not stable.
    const FilamentSortItem same_first = make_item("PLA", "", "", 3);
    const FilamentSortItem same_last  = make_item("PLA", "", "", 7);
    CHECK(sorter.less(same_first, same_last));
    CHECK_FALSE(sorter.less(same_last, same_first));
}

TEST_CASE("SystemFilamentVendorSorter prioritizes Snapmaker and Generic", "[GUI][FilamentSort]")
{
    const SystemFilamentVendorSorter sorter;

    CHECK(sorter.less("Snapmaker", "Generic"));
    CHECK(sorter.less("Generic", "Other"));
    CHECK(sorter.less("Another", "Other"));
    CHECK_FALSE(sorter.less("Other", "Generic"));
    CHECK_FALSE(sorter.less("SNAPMAKER", "Snapmaker"));

    // Case-sensitive vendor ordering: only the exact spelling takes the Snapmaker bucket, and
    // vendors in the same bucket order by code point (uppercase before lowercase).
    CHECK(sorter.less("Snapmaker", "snapmaker"));
    CHECK(sorter.less("Zebra", "apple"));
    CHECK_FALSE(sorter.less("apple", "Zebra"));
}

TEST_CASE("SystemFilamentSorter applies configured order only to Snapmaker", "[GUI][FilamentSort]")
{
    const FilamentOrder order = parse_order(R"({
        "schema_version": 1,
        "sections": {
            "filament_order": {
                "Snapmaker": ["PLA Matte", "PLA SnapSpeed"]
            }
        }
    })");
    const SystemFilamentSorter sorter(order);

    const FilamentSortItem ordered_first = make_item("Z First", "Snapmaker", "PLA Matte", 0);
    const FilamentSortItem ordered_second = make_item("A Second", "Snapmaker", "PLA SnapSpeed", 1);
    const FilamentSortItem unknown = make_item("B Unknown", "Snapmaker", "ABS", 2);
    CHECK(sorter.less(ordered_first, ordered_second));
    CHECK(sorter.less(ordered_second, unknown));

    const FilamentSortItem generic_first = make_item("A Generic", "Generic", "PLA SnapSpeed", 3);
    const FilamentSortItem generic_second = make_item("B Generic", "Generic", "PLA Matte", 4);
    CHECK(sorter.less(generic_first, generic_second));
}

TEST_CASE("FilamentOrder keeps the first rank when a product name repeats", "[GUI][FilamentSort]")
{
    const FilamentOrder order = parse_order(R"({
        "schema_version": 1,
        "sections": {
            "filament_order": {
                "Snapmaker": ["PLA Matte", "PLA Matte", "PLA SnapSpeed"]
            }
        }
    })");

    REQUIRE_FALSE(order.empty());
    CHECK(order.rank("Snapmaker", "PLA Matte") == 0);
    CHECK(order.rank("Snapmaker", "PLA SnapSpeed") == 2);
}

TEST_CASE("FilamentOrder matches non-ASCII product names", "[GUI][FilamentSort]")
{
    const FilamentOrder order = parse_order(R"({
        "schema_version": 1,
        "sections": {
            "filament_order": {
                "Snapmaker": ["PLA 哑光", "TPU 95A"]
            }
        }
    })");

    REQUIRE_FALSE(order.empty());
    CHECK(order.rank("Snapmaker", "PLA 哑光") == 0);
    CHECK(order.rank("Snapmaker", "TPU 95A") == 1);
    CHECK(order.rank("Snapmaker", "PLA 亮光") == std::numeric_limits<size_t>::max());
}

TEST_CASE("filament_product_key strips the vendor prefix and the printer suffix", "[GUI][FilamentSort]")
{
    CHECK(filament_product_key("Snapmaker PLA SnapSpeed", "Snapmaker") == "PLA SnapSpeed");

    // The strip ignores case, like the configured order's product matching: a casing drift between the
    // vendor label and the preset name must still map to the ordered entry.
    CHECK(filament_product_key("snapmaker PLA Matte", "Snapmaker") == "PLA Matte");
    CHECK(filament_product_key("SNAPMaker ABS", "Snapmaker") == "ABS");

    // Printer variants share one product entry.
    CHECK(filament_product_key("PLA Matte @BBL X1C", "Snapmaker") == "PLA Matte");

    // A word-interior coincidence is not a prefix (vendor "Prusa Polymers" against "Prusament ...").
    CHECK(filament_product_key("Prusament PVB @CORE One", "Prusa Polymers") == "Prusament PVB");

    // An unset vendor, or the schema placeholder, falls back to the preset name's leading word.
    CHECK(filament_product_key("Generic ABS", "") == "ABS");
    CHECK(filament_product_key("Generic ABS", "(Undefined)") == "ABS");
    CHECK(filament_product_key("SingleWord", "") == "SingleWord");
}

TEST_CASE("choose_allow_list_copy prefers the deployed user copy", "[GUI][FilamentSort]")
{
    const std::filesystem::path user_copy    = "system/Snapmaker/filament/filament_allow_list.json";
    const std::filesystem::path shipped_copy = "resources/profiles/Snapmaker/filament/filament_allow_list.json";

    CHECK(choose_allow_list_copy(user_copy, shipped_copy, true) == user_copy);
    // A missing user copy falls back to the shipped one instead of leaving the order unconfigured.
    CHECK(choose_allow_list_copy(user_copy, shipped_copy, false) == shipped_copy);
}

TEST_CASE("is_snapmaker_vendor matches the vendor label case-insensitively", "[GUI][FilamentSort]")
{
    // Preset files spell the vendor inconsistently, so the TopN gate must not depend on casing.
    CHECK(is_snapmaker_vendor("Snapmaker"));
    CHECK(is_snapmaker_vendor("SNAPMAKER"));
    CHECK(is_snapmaker_vendor("sNaPmAkEr"));

    // Only a whole-label match counts: neighbouring vendors must not take the Snapmaker path.
    CHECK_FALSE(is_snapmaker_vendor("Generic"));
    CHECK_FALSE(is_snapmaker_vendor("Snapmaker Lab"));
    CHECK_FALSE(is_snapmaker_vendor("Bambu Lab"));
    CHECK_FALSE(is_snapmaker_vendor(""));
}

TEST_CASE("canonical_vendor normalizes the known system vendors and passes others through", "[GUI][FilamentSort]")
{
    CHECK(canonical_vendor("snapmaker") == "Snapmaker");
    CHECK(canonical_vendor("SNAPMAKER") == "Snapmaker");
    CHECK(canonical_vendor("Snapmaker") == "Snapmaker");
    CHECK(canonical_vendor("generic") == "Generic");
    CHECK(canonical_vendor("GENERIC") == "Generic");

    // Unknown vendors keep their spelling; the empty label stays empty rather than becoming a vendor.
    CHECK(canonical_vendor("Bambu Lab") == "Bambu Lab");
    CHECK(canonical_vendor("Snapmaker Lab") == "Snapmaker Lab");
    CHECK(canonical_vendor("") == "");

    // System rows are grouped by the canonical label, so the TopN gate must agree with it.
    CHECK(is_snapmaker_vendor(canonical_vendor("snapmaker")));
    CHECK_FALSE(is_snapmaker_vendor(canonical_vendor("generic")));
}

TEST_CASE("FilamentOrder::from_file reads a configuration file and reports unusable ones as empty",
          "[GUI][FilamentSort]")
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "filament_order_from_file_test.json";

    SECTION("valid configuration")
    {
        std::ofstream stream(path);
        REQUIRE(stream.good());
        stream << R"({"schema_version": 1, "sections": {"filament_order": {"Snapmaker": ["PLA Matte"]}}})";
        // Close before reading back: the write is not on disk while the stream still buffers it.
        stream.close();

        const FilamentOrder order = FilamentOrder::from_file(path);
        CHECK_FALSE(order.empty());
        CHECK(order.rank("Snapmaker", "PLA Matte") == 0);
    }

    SECTION("missing file")
    {
        std::filesystem::remove(path);
        std::string reason;
        CHECK(FilamentOrder::from_file(path, &reason).empty());
        // The loader logs this reason, so a file that is not there stays distinguishable from one
        // that is there but rejected.
        CHECK(reason == "cannot be opened");
    }

    SECTION("malformed file")
    {
        std::ofstream stream(path);
        REQUIRE(stream.good());
        stream << "{";
        stream.close();

        std::string reason;
        CHECK(FilamentOrder::from_file(path, &reason).empty());
        CHECK(reason == "has an invalid or empty configuration");
    }

    std::filesystem::remove(path);
}

TEST_CASE("the shipped allow-list parses and orders the Snapmaker products", "[GUI][FilamentSort]")
{
    // The shipped file is a data contract with PresetUpdater, which deploys it to the user data
    // directory. A typo in it (schema version, vendor key, syntax) would otherwise drop the whole
    // vendor order at runtime without any visible error.
    std::string         reason;
    const FilamentOrder order = FilamentOrder::from_file(FILAMENT_ALLOW_LIST_FILE, &reason);
    INFO("loader reason: " << reason);
    REQUIRE_FALSE(order.empty());

    // A shipped product proves the vendor key matched; an unknown one proves the name fallback.
    CHECK(order.rank("Snapmaker", "PLA SnapSpeed") != std::numeric_limits<size_t>::max());
    CHECK(order.rank("Snapmaker", "Not A Shipped Product") == std::numeric_limits<size_t>::max());
}

namespace
{

/** @brief Creates a scratch directory that is removed again on scope exit. */
struct ScratchDir
{
    std::filesystem::path path;
    explicit ScratchDir(const char *name) : path(std::filesystem::temp_directory_path() / name)
    {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~ScratchDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};

void write_file(const std::filesystem::path &path, const char *content)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path);
    REQUIRE(stream.good());
    stream << content;
    stream.close();
}

const char *k_order_json = R"({"schema_version": 1, "sections": {"filament_order": {"Snapmaker": ["PLA Matte", "PETG HF"]}}})";
const char *k_other_json = R"({"schema_version": 1, "sections": {"filament_order": {"Snapmaker": ["PETG HF", "PLA Matte"]}}})";

} // namespace

// ---- slice 2: updater deployment helper ----

TEST_CASE("allow_list_deployment names the vendor file and touches nothing", "[GUI][FilamentSort]")
{
    ScratchDir scratch("filament_sort_deploy_test");
    const auto src = scratch.path / "rsrc";
    const auto dst = scratch.path / "data" / "system";
    write_file(src / "Snapmaker" / "filament" / "filament_allow_list.json", k_order_json);

    SECTION("Snapmaker with the file present")
    {
        const auto copy = allow_list_deployment(src, dst, "Snapmaker");
        REQUIRE(copy.has_value());
        CHECK(copy->source == src / "Snapmaker" / "filament" / "filament_allow_list.json");
        CHECK(copy->target == dst / "Snapmaker" / "filament" / "filament_allow_list.json");
        CHECK_FALSE(std::filesystem::exists(dst)); // naming a copy must not create directories
    }
    SECTION("other vendors get nothing")
    {
        write_file(src / "BBL" / "filament" / "filament_allow_list.json", k_order_json);
        CHECK_FALSE(allow_list_deployment(src, dst, "BBL").has_value());
    }
    SECTION("file absent gets nothing and creates nothing")
    {
        std::filesystem::remove(src / "Snapmaker" / "filament" / "filament_allow_list.json");
        CHECK_FALSE(allow_list_deployment(src, dst, "Snapmaker").has_value());
        CHECK_FALSE(std::filesystem::exists(dst / "Snapmaker"));
    }
}

TEST_CASE("deploy_allow_list copies and replaces a stale deployed copy", "[GUI][FilamentSort]")
{
    ScratchDir scratch("filament_sort_deploy_replace_test");
    const auto src = scratch.path / "rsrc";
    const auto dst = scratch.path / "data" / "system";
    const auto deployed = dst / "Snapmaker" / "filament" / "filament_allow_list.json";
    write_file(src / "Snapmaker" / "filament" / "filament_allow_list.json", k_order_json);
    write_file(deployed, k_other_json);

    REQUIRE(deploy_allow_list(src, dst, "Snapmaker"));
    CHECK(FilamentOrder::from_file(deployed).rank("Snapmaker", "PLA Matte") == 0); // stale copy replaced

    // A target directory that does not exist yet is created.
    std::filesystem::remove_all(dst);
    REQUIRE(deploy_allow_list(src, dst, "Snapmaker"));
    CHECK(FilamentOrder::from_file(deployed).rank("Snapmaker", "PLA Matte") == 0);
    CHECK_FALSE(deploy_allow_list(src, dst, "Generic"));
}

// ---- slice 3: path resolution and load ----

TEST_CASE("load_filament_order prefers the deployed copy and falls back to the shipped one", "[GUI][FilamentSort]")
{
    ScratchDir scratch("filament_sort_load_test");
    const auto data = scratch.path / "data";
    const auto res  = scratch.path / "res";
    write_file(res / "profiles" / "Snapmaker" / "filament" / "filament_allow_list.json", k_order_json);

    FilamentOrderLoad shipped = load_filament_order(data, res);
    CHECK(shipped.path == res / "profiles" / "Snapmaker" / "filament" / "filament_allow_list.json");
    CHECK(shipped.failure.empty());
    CHECK(shipped.order.rank("Snapmaker", "PLA Matte") == 0);

    write_file(data / "system" / "Snapmaker" / "filament" / "filament_allow_list.json", k_other_json);
    FilamentOrderLoad deployed = load_filament_order(data, res);
    CHECK(deployed.path == data / "system" / "Snapmaker" / "filament" / "filament_allow_list.json");
    CHECK(deployed.order.rank("Snapmaker", "PETG HF") == 0);
}

TEST_CASE("load_filament_order reports why nothing loaded", "[GUI][FilamentSort]")
{
    ScratchDir scratch("filament_sort_load_missing_test");
    const FilamentOrderLoad none = load_filament_order(scratch.path / "data", scratch.path / "res");
    CHECK(none.order.empty());
    CHECK(none.failure == "cannot be opened");
}

// ---- slice 4: extracted system comparator ----

namespace
{

SystemFilamentRow make_row(const char *full_name, const char *vendor, const char *type, size_t index)
{
    return make_system_filament_row(wxString::FromUTF8(full_name), vendor, type, index);
}

std::vector<std::string> sorted_names(const FilamentOrder &order, std::vector<SystemFilamentRow> rows)
{
    const SystemFilamentComparator cmp(order);
    std::stable_sort(rows.begin(), rows.end(), cmp);
    std::vector<std::string> out;
    for (const auto &r : rows)
        out.push_back(r.item.display_name.ToStdString());
    return out;
}

} // namespace

TEST_CASE("make_system_filament_row derives the product key and canonical vendor", "[GUI][FilamentSort]")
{
    const SystemFilamentRow row = make_row("Snapmaker PLA Matte @U1", "snapmaker", "PLA", 3);
    CHECK(row.item.vendor == "Snapmaker");
    CHECK(row.item.filament_product == "PLA Matte");
    CHECK(row.item.original_index == 3);
    CHECK(row.type == "PLA");
}

TEST_CASE("system comparator: Snapmaker's configured order wins, unlisted fall back as before", "[GUI][FilamentSort]")
{
    const FilamentOrder order = parse_order(k_order_json); // PLA Matte, PETG HF
    const std::vector<SystemFilamentRow> rows = {
        make_row("Generic ABS", "Generic", "ABS", 0),
        make_row("Bambu PLA Basic", "Bambu", "PLA", 1),
        make_row("Snapmaker PETG HF", "Snapmaker", "PETG", 2),
        make_row("Snapmaker ABS", "Snapmaker", "ABS", 3),
        make_row("Snapmaker PLA Matte", "Snapmaker", "PLA", 4),
    };
    const std::vector<std::string> expected = {
        "Snapmaker PLA Matte", "Snapmaker PETG HF", // configured rank 0, 1
        "Bambu PLA Basic",                          // hard-coded Bambu name list, as before, ahead of the vendor fallback
        "Snapmaker ABS",                            // Snapmaker, unlisted: first among the vendor fallbacks
        "Generic ABS"};
    CHECK(sorted_names(order, rows) == expected);
}

TEST_CASE("system comparator: with an empty order it reproduces the previous hard-coded behaviour", "[GUI][FilamentSort]")
{
    const std::vector<SystemFilamentRow> rows = {
        make_row("Zeta Thing", "Zeta", "PLA", 0),
        make_row("Generic PETG", "Generic", "PETG", 1),
        make_row("Generic PLA", "Generic", "PLA", 2),
        make_row("Bambu PETG HF", "Bambu", "PETG", 3),
        make_row("Bambu PLA Basic", "Bambu", "PLA", 4),
        make_row("Bambu PLA Matte", "Bambu", "PLA", 5),
        make_row("Bambu Support For PLA", "Bambu", "Support", 6),
        make_row("Alpha TPU", "Alpha", "TPU", 7),
        make_row("Alpha PLA", "Alpha", "PLA", 8),
    };
    // Names in the hard-coded list first (in list order), then vendor (Bambu, Generic, others),
    // then type (PLA, PETG, ABS, TPU, others), then name.
    const std::vector<std::string> expected = {
        "Bambu PLA Basic", "Bambu PLA Matte", "Bambu PETG HF",
        "Bambu Support For PLA",
        "Generic PLA", "Generic PETG",
        "Alpha PLA", "Zeta Thing", "Alpha TPU"};
    CHECK(sorted_names(FilamentOrder{}, rows) == expected);
}

TEST_CASE("system comparator is a strict weak ordering over mixed input", "[GUI][FilamentSort]")
{
    const FilamentOrder order = parse_order(k_order_json);
    const std::vector<SystemFilamentRow> rows = {
        make_row("Snapmaker PETG HF", "Snapmaker", "PETG", 0), make_row("Snapmaker PLA Matte", "Snapmaker", "PLA", 1),
        make_row("Snapmaker ABS", "Snapmaker", "ABS", 2),      make_row("Bambu PLA Basic", "Bambu", "PLA", 3),
        make_row("Generic PLA", "Generic", "PLA", 4),          make_row("Other PLA", "Other", "PLA", 5),
        make_row("Other PLA", "Other", "PLA", 6)};
    const SystemFilamentComparator cmp(order);
    for (const auto &a : rows) {
        CHECK_FALSE(cmp(a, a));
        for (const auto &b : rows) {
            if (cmp(a, b))
                CHECK_FALSE(cmp(b, a));
            for (const auto &c : rows)
                if (cmp(a, b) && cmp(b, c))
                    CHECK(cmp(a, c));
        }
    }
}
