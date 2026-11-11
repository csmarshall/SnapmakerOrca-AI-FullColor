#include "SpoolmanCatalog.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <utility>
#include <nlohmann/json.hpp>

namespace Slic3r { namespace Spoolman {

using json = nlohmann::json;

namespace {

using Warnings = std::vector<std::string>;

void warn(Warnings &w, const std::string &msg) { w.push_back(msg); }

std::optional<double> opt_number(const json &o, const char *key, Warnings &w)
{
    auto it = o.find(key);
    if (it == o.end() || it->is_null())
        return std::nullopt;
    if (!it->is_number()) {
        warn(w, std::string(key) + ": expected a number, ignored");
        return std::nullopt;
    }
    return it->get<double>();
}

std::optional<std::string> opt_string(const json &o, const char *key, Warnings &w)
{
    auto it = o.find(key);
    if (it == o.end() || it->is_null())
        return std::nullopt;
    if (!it->is_string()) {
        warn(w, std::string(key) + ": expected a string, ignored");
        return std::nullopt;
    }
    return it->get<std::string>();
}

// Integer in 1..INT_MAX only; compares in 64 bit before narrowing.
bool id_in_range(const json &v, int &id)
{
    if (v.is_number_unsigned()) {
        const std::uint64_t u = v.get<std::uint64_t>();
        if (u < 1 || u > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
            return false;
        id = static_cast<int>(u);
        return true;
    }
    if (v.is_number_integer()) {
        const std::int64_t i = v.get<std::int64_t>();
        if (i < 1 || i > std::numeric_limits<int>::max())
            return false;
        id = static_cast<int>(i);
        return true;
    }
    return false;
}

bool read_id(const json &o, int &id)
{
    auto it = o.find("id");
    return it != o.end() && id_in_range(*it, id);
}

std::optional<Vendor> parse_vendor_obj(const json &v)
{
    Vendor out;
    if (!v.is_object() || !read_id(v, out.id))
        return std::nullopt;
    out.name = opt_string(v, "name", out.warnings).value_or(std::string());
    return out;
}

constexpr size_t kMaxExtraNodes = 1024;
constexpr const char *kTooLargePlaceholder = "<structured value too large>";

// Iterative (no recursion) check that a real JSON value is small enough to dump(): depth <= kMaxExtraDepth and <= kMaxExtraNodes nodes.
// A primitive has depth 0, an array of primitives depth 1 (same convention as max_nesting).
bool structured_within_caps(const json &root)
{
    std::vector<std::pair<const json *, int>> stack{{&root, 0}};
    size_t nodes = 0;
    while (!stack.empty()) {
        const auto [v, depth] = stack.back();
        stack.pop_back();
        if (++nodes > kMaxExtraNodes)
            return false;
        if (v->is_array() || v->is_object()) {
            if (depth + 1 > kMaxExtraDepth)
                return false;
            for (const json &child : *v)
                stack.emplace_back(&child, depth + 1);
        }
    }
    return true;
}

void parse_extras(const json &o, std::map<std::string, std::string> &extras, std::set<std::string> &undecoded, Warnings &w)
{
    auto it = o.find("extra");
    if (it == o.end() || it->is_null())
        return;
    if (!it->is_object()) {
        warn(w, "extra: expected an object, ignored");
        return;
    }
    for (auto e = it->begin(); e != it->end(); ++e) {
        if (!e.value().is_string()) { // not what Spoolman documents; keep the text, flag it
            undecoded.insert(e.key());
            std::string text;
            if (structured_within_caps(e.value()))
                text = e.value().dump();
            if (text.empty() || text.size() > kMaxExtraBytes) {
                text = kTooLargePlaceholder;
                warn(w, "extra." + e.key() + ": structured value too large, not stored");
            }
            extras[e.key()] = std::move(text);
            continue;
        }
        const std::string raw = e.value().get<std::string>();
        extras[e.key()] = raw;
        if (raw.size() > kMaxExtraBytes || max_nesting(raw) > kMaxExtraDepth) {
            undecoded.insert(e.key());
            continue;
        }
        json decoded = json::parse(raw, nullptr, /*allow_exceptions*/ false);
        if (decoded.is_string())
            extras[e.key()] = decoded.get<std::string>();
        else
            undecoded.insert(e.key()); // invalid JSON, or valid but not a string (number, array, null ...): keep raw
    }
}

std::string trim(const std::string &s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

bool parse_filament_obj(const json &f, Filament &out)
{
    if (!f.is_object() || !read_id(f, out.id))
        return false;
    Warnings &w = out.warnings;
    out.name = opt_string(f, "name", w).value_or(std::string());
    if (auto it = f.find("vendor"); it != f.end() && !it->is_null()) {
        out.vendor = parse_vendor_obj(*it);
        if (!out.vendor)
            warn(w, "vendor: not an object with a valid integer id, ignored");
    }
    out.material               = opt_string(f, "material", w).value_or(std::string());
    out.color_hex              = opt_string(f, "color_hex", w);
    out.density                = opt_number(f, "density", w);
    out.diameter               = opt_number(f, "diameter", w);
    out.weight                 = opt_number(f, "weight", w);
    out.spool_weight           = opt_number(f, "spool_weight", w);
    out.price                  = opt_number(f, "price", w);
    out.settings_extruder_temp = opt_number(f, "settings_extruder_temp", w);
    out.settings_bed_temp      = opt_number(f, "settings_bed_temp", w);
    out.external_id            = opt_string(f, "external_id", w);
    parse_extras(f, out.extras, out.extras_undecoded, w);
    return true;
}

bool parse_spool_obj(const json &s, Spool &out)
{
    if (!s.is_object() || !read_id(s, out.id))
        return false;
    Warnings &w = out.warnings;
    if (auto it = s.find("filament"); it != s.end() && !it->is_null()) {
        int fid = 0;
        if (it->is_object() && read_id(*it, fid))
            out.filament_id = fid;
        else
            warn(w, "filament: not an object with a valid integer id, filament_id ignored");
    }
    out.remaining_weight = opt_number(s, "remaining_weight", w);
    if (auto it = s.find("archived"); it != s.end() && !it->is_null()) {
        if (it->is_boolean())
            out.archived = it->get<bool>();
        else
            warn(w, "archived: expected a boolean, ignored");
    }
    parse_extras(s, out.extras, out.extras_undecoded, w);
    if (auto it = out.extras.find("card_uids"); it != out.extras.end() && out.extras_undecoded.count("card_uids") == 0)
        out.card_uids = split_uid_list(it->second);
    if (auto it = s.find("tags"); it != s.end() && !it->is_null()) {
        if (!it->is_array())
            warn(w, "tags: expected an array, ignored");
        else
            for (size_t i = 0; i < it->size(); ++i) {
                const json &t = (*it)[i];
                if (!t.is_object()) {
                    warn(w, "tags[" + std::to_string(i) + "]: expected an object, ignored");
                    continue;
                }
                if (auto uid = opt_string(t, "uid", w); uid && !trim(*uid).empty())
                    out.tag_uids.push_back(trim(*uid));
            }
    }
    return true;
}

template<class T, class F> ParseResult<T> parse_list(const json &body, const char *what, F parse_one)
{
    ParseResult<T> r;
    const json    *list = nullptr;
    if (body.is_array())
        list = &body;
    else if (body.is_object()) {
        auto it = body.find("result");
        if (it == body.end()) {
            r.reason = std::string(what) + ": object without \"result\"";
            return r;
        }
        if (!it->is_array()) {
            r.reason = std::string(what) + ": \"result\" is not an array";
            return r;
        }
        list = &*it;
    } else {
        r.reason = std::string(what) + ": top level is neither an object nor an array";
        return r;
    }
    for (size_t i = 0; i < list->size(); ++i) {
        T item;
        if (!parse_one((*list)[i], item)) {
            r.items.clear();
            r.reason = std::string(what) + ": item " + std::to_string(i) + " is not an object with an integer id in 1..INT_MAX";
            return r;
        }
        r.items.push_back(std::move(item));
    }
    r.status = r.items.empty() ? ParseStatus::Empty : ParseStatus::Ok;
    return r;
}

template<class T, class P> ParseResult<T> from_text(const std::string &body, P parse)
{
    json j = json::parse(body, nullptr, false);
    if (j.is_discarded()) {
        ParseResult<T> r;
        r.reason = "not valid JSON";
        return r;
    }
    return parse(j);
}

} // namespace

ParseResult<Vendor> parse_vendors(const json &body)
{
    return parse_list<Vendor>(body, "vendor", [](const json &j, Vendor &v) {
        auto p = parse_vendor_obj(j);
        if (p) v = *p;
        return p.has_value();
    });
}
ParseResult<Filament> parse_filaments(const json &body) { return parse_list<Filament>(body, "filament", parse_filament_obj); }
ParseResult<Spool>    parse_spools(const json &body) { return parse_list<Spool>(body, "spool", parse_spool_obj); }

ParseResult<Vendor>   parse_vendors_text(const std::string &b) { return from_text<Vendor>(b, [](const json &j) { return parse_vendors(j); }); }
ParseResult<Filament> parse_filaments_text(const std::string &b) { return from_text<Filament>(b, [](const json &j) { return parse_filaments(j); }); }
ParseResult<Spool>    parse_spools_text(const std::string &b) { return from_text<Spool>(b, [](const json &j) { return parse_spools(j); }); }

std::vector<std::string> split_uid_list(const std::string &value)
{
    std::vector<std::string> out;
    size_t                   pos = 0;
    while (pos <= value.size()) {
        size_t end = value.find(',', pos);
        if (end == std::string::npos)
            end = value.size();
        std::string item = trim(value.substr(pos, end - pos));
        if (!item.empty())
            out.push_back(std::move(item));
        pos = end + 1;
    }
    return out;
}

bool uid_equal(const std::string &a, const std::string &b)
{
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

int max_nesting(const std::string &raw)
{
    int  depth = 0, max_depth = 0;
    bool in_string = false, escaped = false;
    for (char c : raw) {
        if (in_string) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                in_string = false;
        } else if (c == '"')
            in_string = true;
        else if (c == '[' || c == '{')
            max_depth = std::max(max_depth, ++depth);
        else if ((c == ']' || c == '}') && depth > 0)
            --depth;
    }
    return max_depth;
}

}} // namespace Slic3r::Spoolman
