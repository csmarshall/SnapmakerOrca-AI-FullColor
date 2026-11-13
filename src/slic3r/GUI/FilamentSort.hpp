#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <wx/string.h>

namespace Slic3r
{
namespace GUI
{

/** @brief Holds the values a row is ordered by, fixed once the row has been built. */
struct FilamentSortItem
{
    wxString    display_name;
    std::string vendor;
    std::string filament_product;
    size_t      original_index{0};
};

/** @brief Stores the optional vendor-specific filament order. */
class FilamentOrder
{
public:
    FilamentOrder() = default;

    /** @brief Parses a filament-order configuration stream and returns an empty order on invalid input. */
    static FilamentOrder from_stream(std::istream &stream);

    /** @brief Reads a configuration file and returns an empty order when it cannot be used; @p error
     *         receives a reason fragment, which the caller logs together with the component name. */
    static FilamentOrder from_file(const std::filesystem::path &path, std::string *error = nullptr);

    /** @brief Returns the configured rank or the maximum value when no rank exists. */
    size_t rank(const std::string &vendor, const std::string &filament_product) const;

    /** @brief Reports whether the configuration contains at least one valid vendor order. */
    bool empty() const;

private:
    using Order  = std::vector<std::string>;
    using Orders = std::vector<std::pair<std::string, Order>>;

    explicit FilamentOrder(Orders orders);

    Orders m_orders;
};

/** @brief Defines an overridable ordering for filament rows. */
class FilamentSorter
{
public:
    FilamentSorter() = default;
    virtual ~FilamentSorter() = default;

    FilamentSorter(const FilamentSorter &)            = delete;
    FilamentSorter &operator=(const FilamentSorter &) = delete;
    FilamentSorter(FilamentSorter &&)                 = delete;
    FilamentSorter &operator=(FilamentSorter &&)      = delete;

    /** @brief Returns whether @p left precedes @p right. */
    virtual bool less(const FilamentSortItem &left, const FilamentSortItem &right) const;

protected:
    /** @brief Applies the default display-name ordering. */
    bool less_by_name(const FilamentSortItem &left, const FilamentSortItem &right) const;
};

/** @brief Defines an overridable ordering for filament vendors. */
class FilamentVendorSorter
{
public:
    FilamentVendorSorter() = default;
    virtual ~FilamentVendorSorter() = default;

    FilamentVendorSorter(const FilamentVendorSorter &)            = delete;
    FilamentVendorSorter &operator=(const FilamentVendorSorter &) = delete;
    FilamentVendorSorter(FilamentVendorSorter &&)                 = delete;
    FilamentVendorSorter &operator=(FilamentVendorSorter &&)      = delete;

    /** @brief Returns whether vendor @p left precedes vendor @p right. */
    virtual bool less(const std::string &left, const std::string &right) const;
};

/** @brief Prioritizes Snapmaker and Generic vendors before lexical vendor order. */
class SystemFilamentVendorSorter final : public FilamentVendorSorter
{
public:
    bool less(const std::string &left, const std::string &right) const override;
};

/** @brief Applies the configured Snapmaker order before the default name ordering. */
class SystemFilamentSorter final : public FilamentSorter
{
public:
    /** @brief Creates a sorter using the supplied immutable filament order. */
    explicit SystemFilamentSorter(FilamentOrder filament_order);

    bool less(const FilamentSortItem &left, const FilamentSortItem &right) const override;

private:
    FilamentOrder m_filament_order;
};

/** @brief Reports whether a vendor is Snapmaker, ignoring case. */
bool is_snapmaker_vendor(const std::string &vendor);

/** @brief Normalizes the known system vendor names to their canonical spelling. */
std::string canonical_vendor(const std::string &vendor);

/** @brief Returns the product name a configured order matches: the preset name without vendor prefix or printer suffix. */
std::string filament_product_key(const std::string &preset_name, const std::string &vendor);

/** @brief Reads the deployed user copy of the allow-list when it exists, otherwise the shipped resource copy. */
std::filesystem::path choose_allow_list_copy(const std::filesystem::path &user_copy,
                                             const std::filesystem::path &shipped_copy,
                                             bool user_copy_exists);

/** @brief A deployable copy of the allow-list: where it is read from and where it must land. */
struct AllowListCopy
{
    std::filesystem::path source;
    std::filesystem::path target;
};

/** @brief Names the allow-list copy PresetUpdater must deploy for @p vendor, or nothing when the vendor is not
 *         Snapmaker or the source file does not exist. Pure: touches nothing, so it is safe at update-scan time. */
std::optional<AllowListCopy> allow_list_deployment(const std::filesystem::path &source_root,
                                                   const std::filesystem::path &target_root,
                                                   const std::string           &vendor);

/** @brief Copies the allow-list for @p vendor over any deployed copy; returns false when there is nothing to copy or it failed. */
bool deploy_allow_list(const std::filesystem::path &source_root,
                       const std::filesystem::path &target_root,
                       const std::string           &vendor);

/** @brief Deploys the allow-list for every bundle that has one; returns how many deployments failed. */
size_t deploy_allow_lists(const std::filesystem::path       &source_root,
                          const std::filesystem::path       &target_root,
                          const std::vector<std::string>    &bundles);

/** @brief Runs @p install, then deploys the allow-lists whether or not it succeeded (a partial install may still
 *         have replaced vendor directories); returns the install result and, when
 *         @p deploy_failures is given, stores how many deployments failed there. */
bool install_then_deploy(const std::vector<std::string>                              &bundles,
                         const std::function<bool(const std::vector<std::string> &)> &install,
                         const std::filesystem::path                                  &source_root,
                         const std::filesystem::path                                  &target_root,
                         size_t                                                       *deploy_failures = nullptr);

/** @brief Result of locating and reading the allow-list order. */
struct FilamentOrderLoad
{
    FilamentOrder         order;
    std::filesystem::path path;    ///< The file that was read.
    std::string           failure; ///< Empty on success, otherwise the reason fragment from FilamentOrder::from_file.
};

/** @brief Reads the order from <data_dir>/system/Snapmaker/filament when that copy exists, otherwise from
 *         <resources_dir>/profiles/Snapmaker/filament. The caller keeps the result for the process lifetime. */
FilamentOrderLoad load_filament_order(const std::filesystem::path &data_dir, const std::filesystem::path &resources_dir);

/** @brief One system filament preset as the combo box orders it. */
struct SystemFilamentRow
{
    FilamentSortItem item;
    std::string      type;
};

/** @brief Builds a row from the full preset name; vendor is canonicalized and the product key derived. */
SystemFilamentRow make_system_filament_row(const wxString &full_name, const std::string &vendor, const std::string &type, size_t index);

/** @brief Strict weak ordering for the sidebar's System/Unsupported filament presets: Snapmaker's configured order
 *         first, then the previous hard-coded fallbacks (Bambu name list, vendor, type), then name. */
class SystemFilamentComparator
{
public:
    explicit SystemFilamentComparator(FilamentOrder filament_order);

    bool operator()(const SystemFilamentRow &left, const SystemFilamentRow &right) const;

private:
    FilamentOrder                              m_order;
    // Shared so the comparator stays copyable (std algorithms copy it); the sorter itself is immutable.
    std::shared_ptr<const SystemFilamentSorter> m_sorter;
};

} // namespace GUI
} // namespace Slic3r
