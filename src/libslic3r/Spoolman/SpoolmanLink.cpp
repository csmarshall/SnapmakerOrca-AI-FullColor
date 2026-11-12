#include "SpoolmanLink.hpp"

#include <cstdint>
#include <limits>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

namespace Slic3r { namespace Spoolman {

using json = nlohmann::json;

namespace {

// Iterative nesting depth (a primitive is 0, an array/object of primitives 1).
bool depth_ok(const json &root)
{
    std::vector<std::pair<const json *, int>> stack{{&root, 0}};
    while (!stack.empty()) {
        const auto [v, d] = stack.back();
        stack.pop_back();
        if (!v->is_structured())
            continue;
        if (d + 1 > kMaxLinkDepth)
            return false;
        for (const json &c : *v)
            stack.emplace_back(&c, d + 1);
    }
    return true;
}

} // namespace

std::string serialize(const Link &link)
{
    json j = json::object();
    j["id"]          = link.spoolman_id;
    j["external_id"] = link.external_id;
    json s           = json::object();
    for (const auto &[k, v] : link.sources)
        s[k] = v;
    j["sources"] = std::move(s);
    std::string out = j.dump(-1, ' ', false, json::error_handler_t::replace);
    return out.size() > kMaxLinkBytes ? std::string() : out;
}

std::optional<Link> parse(std::string_view text)
{
    if (text.size() > kMaxLinkBytes)
        return std::nullopt;
    // nlohmann treats an embedded NUL as end of input, which would silently ignore trailing garbage: reject it.
    if (text.find('\0') != std::string_view::npos)
        return std::nullopt;
    // max_nesting is not exposed by parse(); the iterative check below bounds the depth afterwards. A deeply nested 4096-byte text is
    // parsed by the library's own (recursive) parser, but 4096 bytes cannot nest deeper than 2048 levels, which it handles.
    json j = json::parse(text.begin(), text.end(), nullptr, false);
    if (j.is_discarded() || !j.is_object() || !depth_ok(j))
        return std::nullopt;

    Link link;
    auto id = j.find("id");
    if (id == j.end())
        return std::nullopt;
    if (id->is_number_unsigned()) {
        const std::uint64_t u = id->get<std::uint64_t>();
        if (u < 1 || u > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
            return std::nullopt;
        link.spoolman_id = static_cast<int>(u);
    } else if (id->is_number_integer()) {
        const std::int64_t i = id->get<std::int64_t>();
        if (i < 1 || i > std::numeric_limits<int>::max())
            return std::nullopt;
        link.spoolman_id = static_cast<int>(i);
    } else
        return std::nullopt;

    auto ext = j.find("external_id");
    if (ext != j.end() && !ext->is_null()) {
        if (!ext->is_string())
            return std::nullopt;
        link.external_id = ext->get<std::string>();
    }
    auto src = j.find("sources");
    if (src != j.end() && !src->is_null()) {
        if (!src->is_object())
            return std::nullopt;
        for (auto it = src->begin(); it != src->end(); ++it) {
            if (!it.value().is_string())
                return std::nullopt;
            link.sources[it.key()] = it.value().get<std::string>();
        }
    }
    return link;
}

}} // namespace Slic3r::Spoolman
