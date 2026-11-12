#include "SpoolmanPresets.hpp"
#include "SpoolmanLink.hpp"

#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>

namespace Slic3r { namespace Spoolman {

namespace {

const char *const kLinkKey = "filament_spoolman_link";

std::string trim(const std::string &s)
{
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool starts_with_nocase(const std::string &s, const std::string &prefix)
{
    if (prefix.empty() || s.size() < prefix.size())
        return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    return true;
}

std::string lower(std::string s)
{
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ieq(const std::string &a, const std::string &b) { return a.size() == b.size() && lower(a) == lower(b); }

// Illegal file-name characters become '-', control characters and 0x7F are dropped, leading/trailing dots and spaces are stripped.
std::string clean_part(const std::string &in)
{
    std::string s;
    for (char c : in) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F)
            continue;
        s += std::string("/\\:*?\"<>|").find(c) != std::string::npos ? '-' : c;
    }
    const auto b = s.find_first_not_of(" .");
    if (b == std::string::npos)
        return {};
    const auto e = s.find_last_not_of(" .");
    return s.substr(b, e - b + 1);
}

std::string truncate_utf8(std::string s, size_t max)
{
    if (s.size() <= max)
        return s;
    size_t cut = max;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
        --cut;
    s.resize(cut);
    return s;
}

// Windows reserves a file whose stem (text before the FIRST dot, trailing spaces ignored) is a device name, whatever the extension.
// Returns the length of that stem when it is reserved, else 0.
size_t reserved_stem_length(const std::string &file_name)
{
    const size_t dot = file_name.find('.');
    std::string  stem = file_name.substr(0, dot);
    while (!stem.empty() && stem.back() == ' ')
        stem.pop_back();
    const std::string u = lower(stem);
    static const char *const names[] = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
                                        "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    return std::any_of(std::begin(names), std::end(names), [&](const char *n) { return u == n; }) ? stem.size() : 0;
}

constexpr size_t kMaxNameBytes = 199 - 5 - 1; // "<name>.json" stays under 200 bytes, one byte spare for the reserved-name fix

// "<Vendor> <name> (Spoolman)[ #counter] @suffix", capped; counter < 2 = none.
std::string make_name(const Filament &f, const std::string &suffix, int counter)
{
    std::string part = clean_part(trim(f.name));
    if (part.empty())
        part = "Filament " + std::to_string(f.id);
    std::string label = part;
    if (f.vendor) {
        const std::string v = clean_part(trim(f.vendor->name));
        if (!v.empty() && !starts_with_nocase(part, v))
            label = v + " " + part;
    }
    const std::string suf = truncate_utf8(suffix, 60);
    const std::string tail = std::string(" ") + kSpoolmanTag + (counter >= 2 ? " #" + std::to_string(counter) : std::string()) + suf;
    label = clean_part(truncate_utf8(label, kMaxNameBytes > tail.size() + 1 ? kMaxNameBytes - tail.size() : 1));
    if (label.empty())
        label = "F";
    std::string name = label + tail;
    // the file is "<name>.json": a reserved stem gets '_' right after it ("nul.txt (Spoolman)" -> "nul_.txt (Spoolman)")
    if (const size_t n = reserved_stem_length(name + ".json"))
        name.insert(n, "_");
    return name;
}

std::string name_suffix(const std::string &parent_name)
{
    const auto at = parent_name.find('@');
    if (at == std::string::npos)
        return {};
    const std::string s = trim(parent_name.substr(at + 1));
    return s.empty() ? std::string() : " @" + s;
}

std::string first_string(const DynamicPrintConfig &c, const char *key)
{
    const auto *o = dynamic_cast<const ConfigOptionStrings *>(c.option(key));
    return o && !o->values.empty() ? o->values.front() : std::string();
}

std::string display(const ConfigOption *opt)
{
    if (!opt)
        return {};
    if (auto *v = dynamic_cast<const ConfigOptionVectorBase *>(opt)) {
        const auto s = v->vserialize();
        return s.empty() ? std::string() : s.front();
    }
    return opt->serialize();
}

// Sets `key` to the single-element string form of a value; vector options get every element set to it.
bool apply_value(DynamicPrintConfig &cfg, const std::string &key, const std::string &value)
{
    ConfigOption          *opt = cfg.option(key);
    const ConfigOptionDef *def = print_config_def.get(key);
    if (!opt || !def)
        return false;
    // String options are set directly: ConfigOptionStrings::deserialize splits on ';' and treats a leading '"' as a quote.
    if (auto *sv = dynamic_cast<ConfigOptionStrings *>(opt)) {
        for (std::string &e : sv->values)
            e = value;
        if (sv->values.empty())
            sv->values.push_back(value);
        return true;
    }
    if (dynamic_cast<ConfigOptionString *>(opt)) {
        static_cast<ConfigOptionString *>(opt)->value = value;
        return true;
    }
    std::unique_ptr<ConfigOption> tmp(def->create_default_option());
    if (!tmp->deserialize(value))
        return false;
    if (auto *v = dynamic_cast<ConfigOptionVectorBase *>(opt)) {
        const size_t n = v->size();
        v->set(tmp.get());
        if (n > 1)
            v->resize(n, tmp.get());
    } else
        opt->set(tmp.get());
    return true;
}

struct Linked
{
    std::string name;
    std::string inherits;
    Link        link;
};

bool is_nozzle_key(const std::string &k) { return k == "nozzle_temperature" || k == "nozzle_temperature_initial_layer"; }
bool is_bed_key(const std::string &k)
{
    const auto &v = plate_temperature_keys();
    return std::find(v.begin(), v.end(), k) != v.end();
}

// Case-insensitive: names differing only in case are one file on macOS/Windows.
const Preset *find_by_name(const PresetCollection &c, const std::string &name)
{
    for (auto it = c.cbegin(); it != c.cend(); ++it)
        if (ieq(it->name, name))
            return &*it;
    return nullptr;
}

const Preset *find_exact(const PresetCollection &c, const std::string &name)
{
    for (auto it = c.cbegin(); it != c.cend(); ++it)
        if (it->name == name)
            return &*it;
    return nullptr;
}

bool parse_number(const std::string &s, double &out)
{
    char *end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end != s.c_str() && *end == '\0' && std::isfinite(out);
}

// Limit group of a temperature key: nozzle keys share one, each plate's normal + initial-layer keys share one. Empty = not limited.
std::string limit_group(const std::string &k)
{
    if (is_nozzle_key(k))
        return "nozzle";
    if (is_bed_key(k)) {
        const std::string suf = "_initial_layer";
        return k.size() > suf.size() && k.compare(k.size() - suf.size(), suf.size(), suf) == 0 ? k.substr(0, k.size() - suf.size()) : k;
    }
    return {};
}

} // namespace

std::vector<ParentCandidate> build_parent_candidates(const PresetCollection &filaments, const Preset *printer)
{
    std::vector<ParentCandidate> out;
    for (auto it = filaments.cbegin(); it != filaments.cend(); ++it) {
        const Preset &p = *it;
        if (!p.is_system || p.is_default)
            continue;
        if (printer && !is_compatible_with_printer(filaments.get_preset_with_vendor_profile(p), PresetWithVendorProfile(*printer, printer->vendor)))
            continue;
        out.push_back({p.name, p.alias, first_string(p.config, "filament_type"), first_string(p.config, "filament_vendor"), p.setting_id,
                       p.filament_id});
    }
    return out;
}

UpsertReport upsert_catalog(PresetCollection &filaments, const std::vector<Filament> &catalog, const UpsertContext &ctx)
{
    UpsertReport report;

    // Snapshot of the linked user presets (names only: inserting into the collection invalidates pointers).
    std::vector<Linked> linked;
    for (auto it = filaments.cbegin(); it != filaments.cend(); ++it) {
        if (!it->is_user() || it->is_external)
            continue;
        const auto *o = dynamic_cast<const ConfigOptionStrings *>(it->config.option(kLinkKey));
        if (!o || o->values.empty() || o->values.front().empty())
            continue;
        if (auto l = parse(o->values.front()))
            linked.push_back({it->name, it->inherits(), std::move(*l)});
    }
    std::sort(linked.begin(), linked.end(), [](const Linked &a, const Linked &b) { return a.name < b.name; });

    std::set<int> catalog_ids;
    for (const Filament &f : catalog)
        catalog_ids.insert(f.id);
    std::set<int> processed;

    const std::vector<ParentCandidate> candidates = build_parent_candidates(filaments, ctx.printer);
    ParentOptions                      popts;
    popts.preferred_token = ctx.preferred_token;

    auto linked_names = [&](int id) {
        std::vector<std::string> v;
        for (const Linked &l : linked)
            if (l.link.spoolman_id == id)
                v.push_back(l.name);
        return v;
    };
    auto skip = [&](const Filament &f, std::string reason) {
        report.skipped.push_back({f.id, f.name, std::move(reason), linked_names(f.id)});
    };

    for (const Filament &f : catalog) {
        if (!processed.insert(f.id).second) {
            skip(f, "duplicate filament id in the catalog");
            continue;
        }
        const ParentChoice choice = choose_parent(f, candidates, popts);
        if (!choice.chosen) {
            skip(f, choice.reason.empty() ? "no parent preset" : choice.reason);
            continue;
        }
        const Preset *parent_preset = find_exact(filaments, choice.chosen->name);
        if (!parent_preset) {
            skip(f, "parent preset vanished");
            continue;
        }
        const std::string          parent_name = parent_preset->name;
        const std::string          parent_fid  = parent_preset->filament_id;
        const DynamicPrintConfig   parent_cfg  = parent_preset->config;
        const std::string          suffix      = name_suffix(parent_name);
        const std::string          generated   = make_name(f, suffix, 1);

        // existing linked presets of this filament for this parent
        std::vector<const Linked *> matches, exact;
        for (const Linked &l : linked)
            if (l.link.spoolman_id == f.id && l.inherits == parent_name) {
                matches.push_back(&l);
                if (l.name == generated)
                    exact.push_back(&l);
            }
        if (exact.empty()) // no byte-exact name: fall back to a case-insensitive one
            for (const Linked *m : matches)
                if (ieq(m->name, generated))
                    exact.push_back(m);
        const Linked *target = nullptr;
        if (matches.size() == 1)
            target = matches.front();
        else if (matches.size() > 1) {
            if (exact.size() == 1)
                target = exact.front();
            else {
                AmbiguousFilament a;
                a.spoolman_id   = f.id;
                a.filament_name = f.name;
                for (const Linked *m : matches)
                    a.candidates.push_back(m->name);
                report.ambiguous.push_back(std::move(a));
                continue; // nothing modified, nothing created
            }
        }
        for (const Linked &l : linked)
            if (l.link.spoolman_id == f.id && std::find(matches.begin(), matches.end(), &l) == matches.end())
                report.superseded.push_back({f.id, l.name, l.inherits, parent_name});

        // overrides, printer limits (per group)
        MappingResult mapping = map_filament(f, parent_cfg);
        std::map<std::string, std::string> bad_groups; // group -> reason
        for (const Override &o : mapping.overrides) {
            const std::string g = limit_group(o.key);
            if (g.empty())
                continue;
            const auto &limit = is_nozzle_key(o.key) ? ctx.limits.max_nozzle_temp : ctx.limits.max_bed_temp;
            if (!limit)
                continue;
            double v = 0;
            if (!parse_number(o.value, v))
                bad_groups.emplace(g, "not a number");
            else if (v > *limit)
                bad_groups[g] = "exceeds printer limit";
        }
        std::vector<Override>      applied;
        std::vector<RejectedValue> rejected;
        for (Override &o : mapping.overrides) {
            auto bad = bad_groups.find(limit_group(o.key));
            if (!limit_group(o.key).empty() && bad != bad_groups.end()) {
                const auto &limit = is_nozzle_key(o.key) ? ctx.limits.max_nozzle_temp : ctx.limits.max_bed_temp;
                double v = 0;
                const bool self_bad = !parse_number(o.value, v) || v > *limit;
                rejected.push_back({f.id, std::string(), o.key + "=" + o.value,
                                    self_bad ? bad->second : "rejected with its group: " + bad->second, false});
            } else
                applied.push_back(std::move(o));
        }
        for (const Rejected &r : mapping.rejected)
            rejected.push_back({f.id, std::string(), r.key_or_field, r.reason, r.warning});
        // a parent value that already exceeds a limit is kept; say so
        for (const std::string &k : plate_temperature_keys())
            if (ctx.limits.max_bed_temp)
                if (const auto *po = dynamic_cast<const ConfigOptionInts *>(parent_cfg.option(k)))
                    if (!po->values.empty() && po->values.front() > *ctx.limits.max_bed_temp &&
                        std::none_of(applied.begin(), applied.end(), [&](const Override &o) { return o.key == k; }))
                        rejected.push_back({f.id, std::string(), k, "parent's own value " + std::to_string(po->values.front()) + " exceeds printer limit (kept)", true});
        for (const char *k : {"nozzle_temperature", "nozzle_temperature_initial_layer"})
            if (ctx.limits.max_nozzle_temp)
                if (const auto *po = dynamic_cast<const ConfigOptionInts *>(parent_cfg.option(k)))
                    if (!po->values.empty() && po->values.front() > *ctx.limits.max_nozzle_temp &&
                        std::none_of(applied.begin(), applied.end(), [&](const Override &o) { return o.key == k; }))
                        rejected.push_back({f.id, std::string(), k, "parent's own value " + std::to_string(po->values.front()) + " exceeds printer limit (kept)", true});

        Link new_link;
        new_link.spoolman_id = f.id;
        new_link.external_id = f.external_id.value_or(std::string());
        for (const Override &o : applied)
            new_link.sources[o.key] = o.source;
        if (!serialize(new_link)) {
            skip(f, "link too large");
            continue;
        }

        // name of a new preset: never one that exists (case-insensitively)
        std::string name;
        if (target)
            name = target->name;
        else {
            name = generated;
            for (int n = 2; find_by_name(filaments, name); ++n)
                name = make_name(f, suffix, n);
        }
        for (RejectedValue &r : rejected)
            r.preset_name = name;

        // build the new config
        DynamicPrintConfig cfg;
        std::map<std::string, std::string> old_sources;
        const Preset *existing = target ? find_exact(filaments, target->name) : nullptr;
        if (target) {
            cfg         = existing->config;
            old_sources = target->link.sources;
            for (const auto &old : old_sources) {
                const std::string &key = old.first;
                const bool still = std::any_of(applied.begin(), applied.end(), [&](const Override &o) { return o.key == key; });
                if (!still)
                    if (const ConfigOption *po = parent_cfg.option(key))
                        cfg.set_key_value(key, po->clone());
            }
        } else {
            cfg = parent_cfg;
            cfg.option<ConfigOptionString>("inherits", true)->value = parent_name;
        }
        for (const Override &o : applied)
            if (!apply_value(cfg, o.key, o.value)) {
                rejected.push_back({f.id, name, o.key, "cannot be applied to the preset", false});
                auto old = old_sources.find(o.key);
                if (old != old_sources.end())
                    new_link.sources[o.key] = old->second; // fail closed: the old value stays listed
                else
                    new_link.sources.erase(o.key);
            }
        const auto final_link = serialize(new_link);
        if (!final_link) { // the fail-closed path may have restored a longer old source: nothing is written
            skip(f, "link too large");
            continue;
        }
        cfg.set_key_value(kLinkKey, new ConfigOptionStrings{*final_link});
        cfg.set_key_value("filament_settings_id", new ConfigOptionStrings{name});

        PresetResult res;
        res.spoolman_id = f.id;
        res.preset_name = name;
        res.parent_name = parent_name;
        res.parent_rule = choice.rule;
        const DynamicPrintConfig before = target ? existing->config : parent_cfg;
        const std::vector<std::string> diff_keys = before.diff(cfg);
        // filament_settings_id alone (the preset's own name, rewritten on every refresh) is written but never reported as an update
        const bool differs = std::any_of(diff_keys.begin(), diff_keys.end(), [](const std::string &k) { return k != "filament_settings_id"; });
        for (const std::string &key : diff_keys) {
            if (key == kLinkKey || key == "inherits" || key == "filament_settings_id")
                continue;
            ValueChange c;
            c.key       = key;
            c.old_value = display(before.option(key));
            c.new_value = display(cfg.option(key));
            auto s      = new_link.sources.find(key);
            c.source    = s != new_link.sources.end() ? s->second : "parent";
            res.changes.push_back(std::move(c));
        }

        if (!target) {
            Preset &np = filaments.load_preset(filaments.path_from_name(name), name, std::move(cfg), false);
            np.filament_id = parent_fid;
            np.is_visible  = true;
            np.setting_id.clear();
            report.created.push_back(std::move(res));
        } else {
            if (!diff_keys.empty())
                for (auto it = filaments.lbegin(); it != filaments.end(); ++it)
                    if (it->name == name) {
                        it->config = std::move(cfg);
                        break;
                    }
            if (!differs)
                report.unchanged.push_back(std::move(res));
            else {
                if (filaments.get_selected_preset_name() == name || filaments.get_edited_preset().name == name)
                    report.needs_reselect.push_back(name);
                report.updated.push_back(std::move(res));
            }
        }
        report.rejected.insert(report.rejected.end(), rejected.begin(), rejected.end());
        for (const Linked *m : matches)
            if (m != target)
                report.duplicates.push_back({f.id, m->name, name});
    }

    if (!catalog.empty())
        for (const Linked &l : linked)
            if (!catalog_ids.count(l.link.spoolman_id))
                report.orphaned.push_back({l.link.spoolman_id, l.name});
    return report;
}

}} // namespace Slic3r::Spoolman
