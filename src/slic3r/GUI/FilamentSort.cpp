#include "FilamentSort.hpp"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <fstream>
#include <limits>
#include <utility>
#include <vector>

namespace Slic3r
{
namespace GUI
{

namespace
{

constexpr const char *g_snapmaker_vendor = "Snapmaker";
constexpr const char *g_generic_vendor   = "Generic";
constexpr const char *g_undefined_vendor = "(Undefined)";
constexpr int         g_allowlist_schema_version = 1;
constexpr const char *g_allow_list_file_name     = "filament_allow_list.json";

/** @brief Position of @p name in the hard-coded Bambu preset list kept as the fallback after the configured order. */
size_t bambu_name_rank(const wxString &name)
{
    static const char *const names[] = {"Bambu PLA Basic",  "Bambu PLA Matte", "Bambu PETG HF",  "Bambu ABS",
                                        "Bambu PLA Silk",   "Bambu PLA-CF",    "Bambu PLA Galaxy", "Bambu PLA Metal",
                                        "Bambu PLA Marble", "Bambu PETG-CF",   "Bambu PETG Translucent", "Bambu ABS-GF"};
    size_t index = 0;
    for (const char *candidate : names) {
        if (name == wxString::FromUTF8(candidate))
            return index;
        ++index;
    }
    return index;
}

/** @brief Position of @p value in @p list, or the list size when absent. */
size_t position_in(const std::vector<std::string> &list, const std::string &value)
{
    return static_cast<size_t>(std::find(list.begin(), list.end(), value) - list.begin());
}

/** @brief Removes leading and trailing whitespace. */
std::string trimmed(std::string value)
{
    const auto is_space = [](unsigned char character) { return std::isspace(character) != 0; };
    const auto first    = std::find_if_not(value.begin(), value.end(), is_space);
    const auto last     = std::find_if_not(value.rbegin(), value.rend(), is_space).base();
    if (first >= last)
        return std::string();

    return std::string(first, last);
}

/** @brief Compares two strings case-insensitively over ASCII letters; every other byte must match exactly. */
bool ascii_iequal(const std::string &left, const std::string &right)
{
    if (left.size() != right.size())
        return false;

    for (size_t index = 0; index < left.size(); ++index)
    {
        const unsigned char lhs = static_cast<unsigned char>(left[index]);
        const unsigned char rhs = static_cast<unsigned char>(right[index]);
        if (std::tolower(lhs) != std::tolower(rhs))
            return false;
    }
    return true;
}

/** @brief Converts a vendor identifier into the label used by system sorting. */
wxString vendor_label(const std::string &vendor)
{
    return vendor.empty() ? wxString::FromUTF8("System") : wxString::FromUTF8(vendor.c_str());
}

/** @brief Returns the fixed priority bucket for a system vendor. */
int vendor_rank(const std::string &vendor)
{
    // Case-sensitive, like the vendor ordering below. Known vendors reach this point already spelled
    // canonically (PlaterFilamentComboBox calls canonical_vendor() before sorting), so a profile that
    // writes "snapmaker" is still ranked as Snapmaker; only unknown vendors keep their own spelling.
    const wxString label = vendor_label(vendor);
    if (label.Cmp(wxString::FromUTF8(g_snapmaker_vendor)) == 0)
        return 0;
    if (label.Cmp(wxString::FromUTF8(g_generic_vendor)) == 0)
        return 1;
    return 2;
}

/** @brief Compares display names by code point and preserves the original order for ties. */
bool default_name_less(const FilamentSortItem &left, const FilamentSortItem &right)
{
    // Case-sensitive, like the upstream Bambu collation: names that differ only in case are ordered
    // by code point instead of being treated as equal.
    const int name_compare = left.display_name.Cmp(right.display_name);
    if (name_compare != 0)
        return name_compare < 0;
    return left.original_index < right.original_index;
}

} // namespace

FilamentOrder FilamentOrder::from_stream(std::istream &stream)
{
    nlohmann::json root = nlohmann::json::parse(stream, nullptr, false);
    if (root.is_discarded() || !root.is_object())
        return FilamentOrder{};

    const auto schema_version = root.find("schema_version");
    const auto sections_node  = root.find("sections");
    if (schema_version == root.end() || !schema_version->is_number_integer() ||
        *schema_version != g_allowlist_schema_version || sections_node == root.end() || !sections_node->is_object())
        return FilamentOrder{};

    const auto order_node = sections_node->find("filament_order");
    if (order_node == sections_node->end() || !order_node->is_object())
        return FilamentOrder{};

    Orders orders;
    for (const auto &vendor_order : order_node->items())
    {
        if (vendor_order.key().empty() || !vendor_order.value().is_array() || vendor_order.value().empty())
            return FilamentOrder{};

        Order values;
        for (const auto &value : vendor_order.value())
        {
            if (!value.is_string() || value.get_ref<const std::string &>().empty())
                return FilamentOrder{};
            values.emplace_back(value.get_ref<const std::string &>());
        }
        orders.emplace_back(vendor_order.key(), std::move(values));
    }

    if (orders.empty())
        return FilamentOrder{};
    return FilamentOrder(std::move(orders));
}

FilamentOrder FilamentOrder::from_file(const std::filesystem::path &path, std::string *error)
{
    std::ifstream stream(path);
    if (!stream)
    {
        if (error != nullptr)
            *error = "cannot be opened";
        return FilamentOrder{};
    }

    const FilamentOrder order = from_stream(stream);
    if (order.empty() && error != nullptr)
        *error = "has an invalid or empty configuration";
    return order;
}

FilamentOrder::FilamentOrder(Orders orders) : m_orders(std::move(orders))
{
}

size_t FilamentOrder::rank(const std::string &vendor, const std::string &filament_product) const
{
    for (const auto &vendor_order : m_orders)
    {
        if (wxString::FromUTF8(vendor_order.first.c_str()).CmpNoCase(wxString::FromUTF8(vendor.c_str())) != 0)
            continue;

        for (size_t index = 0; index < vendor_order.second.size(); ++index)
        {
            // Product names are authored both in the preset files and in filament_allow_list.json, so they match
            // case-insensitively like the vendor key above: a casing drift must not silently drop an entry.
            if (wxString::FromUTF8(vendor_order.second[index].c_str())
                    .CmpNoCase(wxString::FromUTF8(filament_product.c_str())) == 0)
                return index;
        }
        break;
    }
    return std::numeric_limits<size_t>::max();
}

bool FilamentOrder::empty() const
{
    return m_orders.empty();
}

bool FilamentSorter::less(const FilamentSortItem &left, const FilamentSortItem &right) const
{
    return less_by_name(left, right);
}

bool FilamentSorter::less_by_name(const FilamentSortItem &left, const FilamentSortItem &right) const
{
    return default_name_less(left, right);
}

bool FilamentVendorSorter::less(const std::string &left, const std::string &right) const
{
    // Case-sensitive, like the upstream Bambu vendor table: the vendor axis orders by code point.
    return vendor_label(left).Cmp(vendor_label(right)) < 0;
}

bool SystemFilamentVendorSorter::less(const std::string &left, const std::string &right) const
{
    const int left_rank  = vendor_rank(left);
    const int right_rank = vendor_rank(right);
    if (left_rank != right_rank)
        return left_rank < right_rank;
    return FilamentVendorSorter::less(left, right);
}

SystemFilamentSorter::SystemFilamentSorter(FilamentOrder filament_order)
    : m_filament_order(std::move(filament_order))
{
}

bool SystemFilamentSorter::less(const FilamentSortItem &left, const FilamentSortItem &right) const
{
    // Narrow contract: the caller orders rows by vendor first (PlaterFilamentComboBox::sort_system_rows
    // compares vendors and only falls back to this sorter when they are equivalent), so both sides
    // carry the same vendor here and the Snapmaker gate is symmetric. Passing two different vendors to
    // one call would mix the configured rank with the name order and stop being a strict weak ordering,
    // which std::stable_sort requires.
    if (is_snapmaker_vendor(left.vendor) && is_snapmaker_vendor(right.vendor))
    {
        const size_t left_rank  = m_filament_order.rank(left.vendor, left.filament_product);
        const size_t right_rank = m_filament_order.rank(right.vendor, right.filament_product);
        if (left_rank != right_rank)
            return left_rank < right_rank;
    }

    return less_by_name(left, right);
}

bool is_snapmaker_vendor(const std::string &vendor)
{
    // Deliberately case-insensitive, unlike the ordering above: this gate activates the configured order,
    // and a spelling drift must not silently disable it.
    return wxString::FromUTF8(vendor.c_str()).CmpNoCase(wxString::FromUTF8(g_snapmaker_vendor)) == 0;
}

std::string canonical_vendor(const std::string &vendor)
{
    const wxString label = wxString::FromUTF8(vendor.c_str());
    if (label.CmpNoCase(wxString::FromUTF8(g_snapmaker_vendor)) == 0)
        return g_snapmaker_vendor;
    if (label.CmpNoCase(wxString::FromUTF8(g_generic_vendor)) == 0)
        return g_generic_vendor;
    return vendor;
}

std::string filament_product_key(const std::string &preset_name, const std::string &vendor)
{
    std::string product = trimmed(preset_name);
    std::string owner   = trimmed(vendor);

    // An unset vendor, or the schema placeholder, groups the preset by its own leading word.
    if (owner.empty() || owner == g_undefined_vendor)
        owner = product.substr(0, product.find_first_of(" \t"));

    if (!owner.empty() && product.size() > owner.size() && ascii_iequal(product.substr(0, owner.size()), owner) &&
        product[owner.size()] == ' ')
        product = product.substr(owner.size() + 1);

    // Printer variants share one product name, e.g. "PLA Matte @BBL X1C" is the "PLA Matte" entry.
    const size_t printer_suffix = product.find(" @");
    if (printer_suffix != std::string::npos)
        product = product.substr(0, printer_suffix);

    return trimmed(product);
}

std::filesystem::path choose_allow_list_copy(const std::filesystem::path &user_copy,
                                             const std::filesystem::path &shipped_copy,
                                             bool                         user_copy_exists)
{
    return user_copy_exists ? user_copy : shipped_copy;
}

std::optional<AllowListCopy> allow_list_deployment(const std::filesystem::path &source_root,
                                                   const std::filesystem::path &target_root,
                                                   const std::string           &vendor)
{
    // Exact match: the vendor here is the bundle directory name, which is spelled canonically.
    if (vendor != g_snapmaker_vendor)
        return std::nullopt;

    AllowListCopy copy;
    copy.source = source_root / vendor / "filament" / g_allow_list_file_name;
    copy.target = target_root / vendor / "filament" / g_allow_list_file_name;

    std::error_code ec;
    if (!std::filesystem::is_regular_file(copy.source, ec))
        return std::nullopt;
    return copy;
}

bool deploy_allow_list(const std::filesystem::path &source_root,
                       const std::filesystem::path &target_root,
                       const std::string           &vendor)
{
    const std::optional<AllowListCopy> copy = allow_list_deployment(source_root, target_root, vendor);
    if (!copy)
        return false;
    std::error_code ec;
    std::filesystem::create_directories(copy->target.parent_path(), ec);
    return std::filesystem::copy_file(copy->source, copy->target, std::filesystem::copy_options::overwrite_existing, ec) && !ec;
}

FilamentOrderLoad load_filament_order(const std::filesystem::path &data_dir, const std::filesystem::path &resources_dir)
{
    const std::filesystem::path user_path = data_dir / "system" / g_snapmaker_vendor / "filament" / g_allow_list_file_name;
    const std::filesystem::path shipped_path =
        resources_dir / "profiles" / g_snapmaker_vendor / "filament" / g_allow_list_file_name;

    std::error_code ec;
    const bool      user_exists = std::filesystem::exists(user_path, ec);

    FilamentOrderLoad result;
    result.path  = choose_allow_list_copy(user_path, shipped_path, user_exists);
    result.order = FilamentOrder::from_file(result.path, &result.failure);
    return result;
}

SystemFilamentRow make_system_filament_row(const wxString &full_name, const std::string &vendor, const std::string &type, size_t index)
{
    SystemFilamentRow row;
    row.item.display_name     = full_name;
    row.item.vendor           = canonical_vendor(vendor);
    row.item.filament_product = filament_product_key(full_name.ToStdString(wxConvUTF8), row.item.vendor);
    row.item.original_index   = index;
    row.type                  = type;
    return row;
}

SystemFilamentComparator::SystemFilamentComparator(FilamentOrder filament_order)
    : m_order(filament_order), m_sorter(std::make_shared<const SystemFilamentSorter>(std::move(filament_order)))
{
}

bool SystemFilamentComparator::operator()(const SystemFilamentRow &left, const SystemFilamentRow &right) const
{
    // 1. Snapmaker's configured order wins: ranked Snapmaker presets precede everything else, by rank.
    const auto configured_rank = [this](const SystemFilamentRow &row) {
        return is_snapmaker_vendor(row.item.vendor) ? m_order.rank(row.item.vendor, row.item.filament_product)
                                                    : std::numeric_limits<size_t>::max();
    };
    const size_t left_rank  = configured_rank(left);
    const size_t right_rank = configured_rank(right);
    if (left_rank != right_rank)
        return left_rank < right_rank;

    // 2. Fallback: the hard-coded Bambu name list (listed names first, in list order).
    const size_t left_name  = bambu_name_rank(left.item.display_name);
    const size_t right_name = bambu_name_rank(right.item.display_name);
    if (left_name != right_name)
        return left_name < right_name;

    // 3. Fallback: vendor, as before ("" = no vendor, then Bambu, Generic); Snapmaker's unlisted presets lead the
    //    named vendors because this fork's profiles are Snapmaker-first.
    static const std::vector<std::string> first_vendors = {"", "Snapmaker", "Bambu", "Generic"};
    const size_t left_vendor  = position_in(first_vendors, left.item.vendor);
    const size_t right_vendor = position_in(first_vendors, right.item.vendor);
    if (left_vendor != right_vendor)
        return left_vendor < right_vendor;

    // 4. Fallback: filament type, as before.
    static const std::vector<std::string> first_types = {"PLA", "PETG", "ABS", "TPU"};
    const size_t left_type  = position_in(first_types, left.type);
    const size_t right_type = position_in(first_types, right.type);
    if (left_type != right_type)
        return left_type < right_type;

    // 5. Name, then original position (the sorter's own tie-break).
    return m_sorter->less(left.item, right.item);
}

} // namespace GUI
} // namespace Slic3r
