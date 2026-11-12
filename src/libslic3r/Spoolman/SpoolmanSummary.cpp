#include "SpoolmanSummary.hpp"

#include <algorithm>
#include <cmath>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

namespace Slic3r { namespace Spoolman {

namespace {

// All user-visible text lives here, in ONE place. Translation hooks (_L) come with the GUI step.
const char *const kToastPrefix        = "Spoolman: ";
const char *const kToastInSync        = " in sync, ";
const char *const kToastNothing       = "nothing changed";
const char *const kWeightLeft         = " g left";
const char *const kWeightUnknown      = "weight unknown";
const char *const kRuleMatchedBy      = "matched by ";
const char *const kRuleVendorName     = "matched by vendor + name + material";
const char *const kRuleFamily         = "matched by material family";
const char *const kRuleMaterialAlone  = "matched by material only (flags not checked)";
const char *const kRuleSettingIdExtra = "matched by the setting_id custom field";
const char *const kRuleOfdPrefix      = "OFD slicer settings: ";
const char *const kRuleOfdText1       = "matched by OFD slicer settings (";
const char *const kTierSpoolmanId     = "linked Spoolman filament id";
const char *const kTierExternalId     = "matched by Spoolman external id";
const char *const kTierSettingId      = "matched by setting_id";
const char *const kFlagAmbiguous      = "Ambiguous: several presets match this filament (";
const char *const kFlagAmbiguousEnd   = "); nothing was changed.";
const char *const kFlagSuperseded     = "Superseded: ";
const char *const kFlagDuplicate      = "Duplicate: ";
const char *const kFlagReselect       = "Needs re-select: ";
const char *const kFlagOrphaned       = "Orphaned: ";
const char *const kFlagMismatch       = "Mismatch: ";
const char *const kStaleUnreachable   = "Spoolman could not be reached.";
const char *const kStaleBadReply      = "Spoolman sent an unusable reply.";
const char *const kStaleLast          = " Showing the values from the last sync";
const char *const kStaleNoPrevious    = " There are no earlier values to show.";
const char *const kStaleConsequence   = " Slots were matched by the printer's RFID type, the way Orca did before.";
const char *const kProjectCopy1       = "This project had an older copy of \"";
const char *const kProjectCopy2       = "\". Spoolman is the source for this filament, so the project now uses the current library preset.";
const char *const kProjectCopyRows    = " The changed values are listed below.";
const char *const kReadOnlyNote       = "Nothing is written to Spoolman. Edit a Spoolman-backed preset for this session only; \"Save as\" under a new name removes the link.";
const char *const kFetchedJustNow     = "fetched just now";
const char *const kFetchedUnknown     = "fetched at an unknown time";
const char *const kFlagDupTier        = "Duplicate tier entry: ";
const char *const kToastCatalogNone   = " in the catalog, none matched a preset";
const char *const kFetchedPrefix      = "fetched ";
const char *const kLastGoodPrefix     = "last good fetch ";
const char *const kLastGoodUnknown    = "last good fetch time unknown";
const char *const kNeverFetched       = "never fetched";
const char *const kNoTrayName         = "Slot key ";
const char *const kNoTrayNameEnd      = " (no tray information)";
const char *const kFilamentName       = "Spoolman filament ";
const char *const kSlotName           = "Slot ";

std::string num_text(double v)
{
    std::ostringstream o;
    o.imbue(std::locale::classic());
    o.precision(15);
    o << v;
    return o.str();
}

std::string plural(long long n, const char *unit)
{
    return std::to_string(n) + " " + unit + (n == 1 ? "" : "s");
}

std::string age_text(long long s)
{
    if (s < 0)
        s = 0;
    if (s < 60)
        return plural(s, "second");
    if (s < 3600)
        return plural(s / 60, "minute");
    if (s < 86400)
        return plural(s / 3600, "hour");
    return plural(s / 86400, "day");
}

std::string parent_rule_text(const std::string &rule)
{
    if (rule.empty())
        return {};
    if (rule == "vendor + name + material")
        return kRuleVendorName;
    if (rule == "material family with flags")
        return kRuleFamily;
    if (rule == "material alone")
        return kRuleMaterialAlone;
    if (rule == "setting_id extra")
        return kRuleSettingIdExtra;
    if (rule.rfind(kRuleOfdPrefix, 0) == 0) {
        std::string what = rule.substr(std::string(kRuleOfdPrefix).size());
        if (what == "profile_name")
            what = "profile name";
        else if (what == "generic_id")
            what = "generic id";
        return std::string(kRuleOfdText1) + what + ")";
    }
    return std::string(kRuleMatchedBy) + "rule: " + rule;
}

std::string tier_rule_text(const std::string &rule)
{
    if (rule == "spoolman_id")
        return kTierSpoolmanId;
    if (rule == "external_id")
        return kTierExternalId;
    if (rule == "setting_id")
        return kTierSettingId;
    return std::string(kRuleMatchedBy) + "rule: " + rule;
}

std::string join(const std::vector<std::string> &v, const char *sep)
{
    std::string r;
    for (size_t i = 0; i < v.size(); ++i)
        r += (i ? sep : "") + v[i];
    return r;
}

// Escapes control characters and cuts at a code-point boundary. Invalid UTF-8 bytes are shown as \xNN as well.
std::string cap_value(const std::string &v, bool &truncated)
{
    static const char *hex = "0123456789abcdef";
    std::string out;
    truncated = false;
    size_t i = 0;
    const std::string ellipsis = "\xE2\x80\xA6";
    while (i < v.size()) {
        const unsigned char c = (unsigned char) v[i];
        std::string unit;
        size_t      len = 1;
        if (c < 0x20 || c == 0x7f)
            unit = std::string("\\x") + hex[c >> 4] + hex[c & 15];
        else if (c < 0x80)
            unit = std::string(1, char(c));
        else {
            const size_t want = (c >= 0xF0 && c < 0xF8) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC2) ? 2 : 0;
            bool ok = want > 0 && c < 0xF8 && i + want <= v.size();
            for (size_t k = 1; ok && k < want; ++k)
                ok = ((unsigned char) v[i + k] & 0xC0) == 0x80;
            if (ok) {
                unit = v.substr(i, want);
                len  = want;
            } else
                unit = std::string("\\x") + hex[c >> 4] + hex[c & 15];
        }
        if (out.size() + unit.size() > kMaxValueBytes) {
            truncated = true;
            break;
        }
        out += unit;
        i += len;
    }
    if (truncated)
        out += ellipsis;
    return out;
}

std::string qt(const std::string &s) { return "\"" + s + "\""; }

int source_rank(const std::string &source)
{
    if (source.rfind("native", 0) == 0)
        return 0;
    if (source.rfind("extra", 0) == 0)
        return 1;
    if (source.rfind("ofd", 0) == 0)
        return 2;
    return 3;
}

// Group by source class; within a class the order the upsert gave is kept (it is the mapping's own order, deterministic).
void order_rows(std::vector<SummaryRow> &rows)
{
    std::stable_sort(rows.begin(), rows.end(), [](const SummaryRow &a, const SummaryRow &b) { return source_rank(a.source) < source_rank(b.source); });
}

struct PresetData
{
    std::string             parent, rule;
    bool                    created = false;
    std::vector<SummaryRow> changes;
};

// What the reports say about one Spoolman filament id.
struct IdData
{
    std::map<std::string, PresetData> presets; // by preset NAME
    std::set<std::string>             name_hints;
    std::string                       filament_name;
    bool                              skipped = false, notable = false;
    std::string                       skip_reason;
    std::vector<std::string>          linked_presets;
    std::vector<SummaryIgnored>       ignored;
    std::vector<std::pair<int, std::string>> flags; // (rank, text)
};

// Display order of slots: numbers first (numerically), then labels.
std::pair<std::pair<int, long long>, std::string> display_key(const std::string &d)
{
    const bool numeric = !d.empty() && d.size() < 10 && std::all_of(d.begin(), d.end(), [](char c) { return c >= '0' && c <= '9'; });
    return {{numeric ? 0 : 1, numeric ? std::stoll(d) : 0}, d};
}

} // namespace

SummaryModel build_summary(const SyncOutcome &o)
{
    SummaryModel m;
    const UpsertReport &r = o.report;
    std::map<int, IdData>        data;
    std::map<std::string, std::set<int>> ids_of_preset; // a preset name can appear under several ids
    std::set<int>                in_sync;

    auto add_result = [&](const PresetResult &p, bool created) {
        IdData &d = data[p.spoolman_id];
        PresetData &pd = d.presets[p.preset_name];
        pd.parent  = p.parent_name;
        pd.rule    = parent_rule_text(p.parent_rule);
        pd.created = pd.created || created;
        for (const ValueChange &c : p.changes)
            pd.changes.push_back({c.key, c.old_value, c.new_value, c.source});
        ids_of_preset[p.preset_name].insert(p.spoolman_id);
        in_sync.insert(p.spoolman_id);
        if (created || !p.changes.empty())
            d.notable = true;
    };
    for (const PresetResult &p : r.created)
        add_result(p, true);
    for (const PresetResult &p : r.updated)
        add_result(p, false);
    for (const PresetResult &p : r.unchanged)
        add_result(p, false);

    for (const SkippedFilament &s : r.skipped) {
        IdData &d = data[s.spoolman_id];
        d.skipped = d.notable = true;
        d.skip_reason         = s.reason;
        d.filament_name       = s.filament_name;
        d.linked_presets      = s.linked_presets;
        std::sort(d.linked_presets.begin(), d.linked_presets.end());
    }
    for (const RejectedValue &v : r.rejected) {
        IdData &d = data[v.spoolman_id];
        SummaryIgnored ig;
        ig.key             = v.key_or_field;
        ig.value           = cap_value(v.value, ig.truncated);
        ig.original_length = v.value.size();
        ig.reason          = v.reason;
        ig.warning         = v.warning;
        d.ignored.push_back(std::move(ig));
        if (!v.warning)
            d.notable = true;
        if (!v.preset_name.empty())
            d.name_hints.insert(v.preset_name);
    }
    for (const AmbiguousFilament &a : r.ambiguous) {
        IdData &d = data[a.spoolman_id];
        d.notable = true;
        d.filament_name = a.filament_name;
        std::vector<std::string> c = a.candidates;
        std::sort(c.begin(), c.end());
        d.flags.push_back({0, kFlagAmbiguous + join(c, ", ") + kFlagAmbiguousEnd});
    }
    for (const SupersededPreset &s : r.superseded) {
        IdData &d = data[s.spoolman_id];
        d.notable = true;
        d.name_hints.insert(s.preset_name);
        d.flags.push_back({1, std::string(kFlagSuperseded) + "the preset " + qt(s.preset_name) + " is linked under another parent (" +
                                  qt(s.old_parent) + ", now " + qt(s.new_parent) + "); kept unchanged."});
    }
    for (const DuplicatePreset &s : r.duplicates) {
        IdData &d = data[s.spoolman_id];
        d.notable = true;
        d.name_hints.insert(s.refreshed_preset.empty() ? s.preset_name : s.refreshed_preset);
        d.flags.push_back({2, std::string(kFlagDuplicate) + qt(s.preset_name) + " carries the same Spoolman link" +
                                  (s.refreshed_preset.empty() ? std::string("; kept unchanged.") :
                                                                "; " + qt(s.refreshed_preset) + " was refreshed instead.")});
    }
    for (const std::string &n : r.needs_reselect) {
        const std::string text = std::string(kFlagReselect) + qt(n) + " was refreshed while selected; select it again to see the new values.";
        auto it = ids_of_preset.find(n);
        if (it == ids_of_preset.end()) {
            SummaryNotice info;
            info.text = text;
            m.notices.push_back(std::move(info));
            m.show_dialog = true;
            continue;
        }
        for (int id : it->second) { // every id that reported this preset name gets the flag
            data[id].notable = true;
            data[id].flags.push_back({3, text});
        }
    }
    for (const OrphanedPreset &s : r.orphaned) {
        IdData &d = data[s.spoolman_id];
        d.notable = true;
        d.name_hints.insert(s.preset_name);
        d.flags.push_back({4, std::string(kFlagOrphaned) + qt(s.preset_name) + " is linked to a filament that is no longer in Spoolman; kept, not refreshed."});
    }

    // Tier entries by raw key. Entries sharing a key are sorted so which one is "first" never depends on the input order.
    std::map<int, std::vector<const SpoolmanTierReport::Entry *>> tiers_of_key;
    for (const auto &e : o.tiers.entries)
        tiers_of_key[e.slot].push_back(&e);
    for (auto &kv : tiers_of_key)
        std::stable_sort(kv.second.begin(), kv.second.end(), [](const SpoolmanTierReport::Entry *a, const SpoolmanTierReport::Entry *b) {
            auto ca = a->candidates, cb = b->candidates;
            std::sort(ca.begin(), ca.end());
            std::sort(cb.begin(), cb.end());
            return std::tie(a->rule, a->preset, ca) < std::tie(b->rule, b->preset, cb);
        });

    // Builds a card from an id's data and an optional S6 tier entry. `tier_notable` is set when the tier alone is worth telling.
    auto make_card = [&](int id, IdData d, const SpoolmanTierReport::Entry *tier, bool &notable) -> SlotCard {
        SlotCard c;
        c.spoolman_id = id;
        const std::string *primary_name = nullptr;
        if (tier && tier->rule != "ambiguous" && !tier->preset.empty() && d.presets.count(tier->preset))
            primary_name = &d.presets.find(tier->preset)->first;
        else if (!d.presets.empty())
            primary_name = &d.presets.begin()->first;
        const PresetData *primary = primary_name ? &d.presets.at(*primary_name) : nullptr;
        if (primary) {
            c.preset_name = *primary_name;
            c.rule_text   = primary->rule;
            c.parent_name = primary->parent;
        }
        std::vector<std::string> names;
        for (const auto &kv : d.presets) {
            names.push_back(kv.first);
            c.changes.insert(c.changes.end(), kv.second.changes.begin(), kv.second.changes.end());
            c.created = c.created || kv.second.created;
        }
        c.changes_preset = join(names, ", ");
        order_rows(c.changes);
        if (primary && c.created) {
            const std::string from = c.parent_name.empty() ? std::string("its parent") : c.parent_name;
            c.note = c.changes.empty() ? "Created, no values differ from " + from + "." :
                                         "Created from " + from + "; the preset value column shows " + from + "'s value.";
        }
        if (tier) {
            if (tier->rule == "ambiguous") {
                std::vector<std::string> cand = tier->candidates;
                std::sort(cand.begin(), cand.end());
                d.flags.push_back({0, kFlagAmbiguous + join(cand, ", ") + kFlagAmbiguousEnd});
                notable = true;
            } else if (!tier->preset.empty()) {
                if (!d.presets.empty() && !d.presets.count(tier->preset)) {
                    c.preset_name = tier->preset;
                    d.flags.push_back({5, std::string(kFlagMismatch) + "this slot uses " + qt(tier->preset) + " but the Spoolman preset " +
                                              qt(c.changes_preset) + " was refreshed."});
                    // a mismatch is a note, not a reason to open the dialog by itself
                } else if (d.presets.empty())
                    c.preset_name = tier->preset;
                c.rule_text = tier_rule_text(tier->rule);
            }
        }
        c.skipped        = d.skipped;
        c.skip_reason    = d.skip_reason;
        c.linked_presets = d.linked_presets;
        c.ignored        = d.ignored;
        std::stable_sort(c.ignored.begin(), c.ignored.end(), [](const SummaryIgnored &a, const SummaryIgnored &b) {
            return std::tie(a.key, a.value, a.reason, a.warning, a.original_length) < std::tie(b.key, b.value, b.reason, b.warning, b.original_length);
        });
        std::sort(d.flags.begin(), d.flags.end());
        d.flags.erase(std::unique(d.flags.begin(), d.flags.end()), d.flags.end());
        for (const auto &f : d.flags)
            c.flags.push_back(f.second);
        // fallback name for cards without tray information
        if (!d.filament_name.empty())
            c.spool_name = d.filament_name;
        else if (!d.name_hints.empty())
            c.spool_name = *d.name_hints.begin();
        else if (!names.empty())
            c.spool_name = names.front();
        else if (tier && !tier->preset.empty())
            c.spool_name = tier->preset;
        return c;
    };

    std::set<int>            used_ids;
    std::set<int>            tray_keys;
    std::vector<SlotCard>    cards;
    // Trays that become cards, with their display label: empty -> "Slot <key>", shared labels get " 1", " 2" in raw-key order.
    std::vector<SummaryTray> trays;
    for (const SummaryTray &t : o.trays) {
        tray_keys.insert(t.key);
        if (t.spoolman_filament_id == 0 && !tiers_of_key.count(t.key))
            continue;
        trays.push_back(t);
        if (trays.back().display_slot.empty())
            trays.back().display_slot = kSlotName + std::to_string(t.key);
    }
    std::stable_sort(trays.begin(), trays.end(), [](const SummaryTray &a, const SummaryTray &b) { return a.key < b.key; });
    {
        std::map<std::string, int> uses;
        for (const SummaryTray &t : trays)
            ++uses[t.display_slot];
        std::map<std::string, int> seen;
        for (SummaryTray &t : trays)
            if (uses[t.display_slot] > 1)
                t.display_slot += " " + std::to_string(++seen[t.display_slot]);
    }
    for (const SummaryTray &t : trays) {
        const auto te      = tiers_of_key.find(t.key);
        const auto *tier   = te == tiers_of_key.end() ? nullptr : te->second.front();
        IdData d = data[t.spoolman_filament_id]; // copy: another tray may share the id
        bool   notable = d.notable;
        SlotCard c = make_card(t.spoolman_filament_id, d, tier, notable);
        used_ids.insert(t.spoolman_filament_id);
        c.raw_key      = t.key;
        c.display_slot = t.display_slot;
        c.on_tray      = true;
        c.colour_hex   = t.colour_hex;
        if (!t.spool_name.empty())
            c.spool_name = t.spool_name;
        if (c.spool_name.empty())
            c.spool_name = t.spoolman_filament_id ? kFilamentName + std::to_string(t.spoolman_filament_id) : t.display_slot;
        if (t.remaining_weight_g)
            c.remaining_weight_text = (std::isfinite(*t.remaining_weight_g) && *t.remaining_weight_g >= 0) ?
                                          num_text(*t.remaining_weight_g) + kWeightLeft : std::string(kWeightUnknown);
        m.show_dialog = m.show_dialog || notable;
        cards.push_back(std::move(c));
    }
    std::sort(cards.begin(), cards.end(), [](const SlotCard &a, const SlotCard &b) {
        return std::make_tuple(display_key(a.display_slot), a.raw_key) < std::make_tuple(display_key(b.display_slot), b.raw_key);
    });

    // S6 tier entries whose slot matches no tray, and second and later entries of one key: never dropped.
    std::vector<SlotCard> loose;
    for (const auto &kv : tiers_of_key) {
        for (size_t i = 0; i < kv.second.size(); ++i) {
            if (i == 0 && tray_keys.count(kv.first))
                continue;
            bool notable = true;
            IdData none;
            if (i > 0)
                none.flags.push_back({6, std::string(kFlagDupTier) + "another entry for slot key " + std::to_string(kv.first) +
                                             " was found; the first one is shown with the slot."});
            SlotCard c = make_card(0, none, kv.second[i], notable);
            c.raw_key    = kv.first;
            c.spool_name = kNoTrayName + std::to_string(kv.first) + kNoTrayNameEnd;
            loose.push_back(std::move(c));
            m.show_dialog = true;
        }
    }
    // Filaments on no tray: only when there is something to tell.
    for (auto &kv : data) {
        if (used_ids.count(kv.first) || !kv.second.notable)
            continue;
        bool notable = true;
        SlotCard c = make_card(kv.first, kv.second, nullptr, notable);
        if (c.spool_name.empty())
            c.spool_name = kFilamentName + std::to_string(kv.first);
        loose.push_back(std::move(c));
        m.show_dialog = true;
    }
    m.slots = std::move(cards);
    for (SlotCard &c : loose)
        m.slots.push_back(std::move(c));
    // notes that are visible: warning-only rejections and tier mismatches on the cards that exist
    for (const SlotCard &c : m.slots) {
        for (const SummaryIgnored &i : c.ignored)
            if (i.warning)
                ++m.warning_count;
        for (const std::string &f : c.flags)
            if (f.rfind(kFlagMismatch, 0) == 0)
                ++m.warning_count;
    }

    m.catalog_size   = o.catalog_size;
    m.read_only_note = kReadOnlyNote;
    if (o.catalog_state != CatalogState::fresh) {
        SummaryNotice n;
        n.kind = NoticeKind::stale_spoolman;
        n.text = o.catalog_state == CatalogState::bad_response ? kStaleBadReply : kStaleUnreachable;
        if (o.has_previous_sync) {
            n.text += kStaleLast;
            if (o.last_good_age_seconds)
                n.text += ", " + age_text(*o.last_good_age_seconds) + " ago";
            n.text += ".";
        } else
            n.text += kStaleNoPrevious;
        n.text += kStaleConsequence;
        m.notices.push_back(std::move(n));
        m.show_dialog = true;
        m.fetched_text = !o.has_previous_sync ? kNeverFetched :
                         o.last_good_age_seconds ? kLastGoodPrefix + age_text(*o.last_good_age_seconds) + " ago" : std::string(kLastGoodUnknown);
    } else
        m.fetched_text = (!o.fetched_age_seconds || *o.fetched_age_seconds < 0) ? std::string(kFetchedUnknown) :
                         *o.fetched_age_seconds <= 5                              ? std::string(kFetchedJustNow) :
                                                                                    kFetchedPrefix + age_text(*o.fetched_age_seconds) + " ago";

    for (ProjectCopyReplaced p : o.project_copies) {
        SummaryNotice n;
        n.kind = NoticeKind::project_copy_replaced;
        n.text = kProjectCopy1 + p.preset_name + kProjectCopy2 + (p.changes.empty() ? "" : kProjectCopyRows);
        n.rows = std::move(p.changes);
        order_rows(n.rows);
        m.notices.push_back(std::move(n));
        m.show_dialog = true;
    }
    // severity order: stale, project copies (by preset name), info
    std::stable_sort(m.notices.begin(), m.notices.end(), [](const SummaryNotice &a, const SummaryNotice &b) {
        if (a.kind != b.kind)
            return int(a.kind) < int(b.kind);
        return a.text < b.text;
    });

    if (!m.show_dialog && o.catalog_size > 0) {
        const long long n = (long long) in_sync.size();
        if (n == 0)
            m.toast_text = std::string(kToastPrefix) + plural((long long) o.catalog_size, "filament") + kToastCatalogNone;
        else
            m.toast_text = std::string(kToastPrefix) + plural(n, "filament") + kToastInSync +
                           (m.warning_count > 0 ? plural((long long) m.warning_count, "note") : std::string(kToastNothing));
    }
    return m;
}


std::string sanitize_for_display(const std::string &s)
{
    std::string out = s;
    for (char &c : out)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F)
            c = ' ';
    return out;
}

std::vector<SummaryLine> summary_lines(const SummaryModel &m)
{
    using K = SummaryLine::Kind;
    std::vector<SummaryLine> out;
    auto add = [&out](K k, std::vector<std::string> cells) {
        for (std::string &c : cells)
            c = sanitize_for_display(c);
        out.push_back({k, std::move(cells)});
    };
    const std::vector<std::string> head = {"Setting", "Preset value", "Spoolman value", "Source"};
    for (const SummaryNotice &n : m.notices) {
        add(K::notice, {n.text});
        if (!n.rows.empty()) {
            add(K::notice_head, head);
            for (const SummaryRow &r : n.rows)
                add(K::notice_row, {r.key, r.preset_value, r.spoolman_value, r.source});
        }
    }
    for (const SlotCard &c : m.slots) {
        add(K::slot_header, {c.on_tray ? "Slot " + c.display_slot : std::string("No tray"), c.spool_name, c.remaining_weight_text, c.colour_hex});
        if (c.preset_name)
            add(K::slot_preset, {"Preset: " + *c.preset_name + (c.rule_text.empty() ? "" : " (" + c.rule_text + ")")});
        if (!c.note.empty())
            add(K::slot_note, {c.note});
        if (c.skipped) {
            add(K::slot_note, {"Skipped: " + c.skip_reason});
            for (const std::string &p : c.linked_presets)
                add(K::slot_note, {"Kept: " + p});
        }
        for (const std::string &f : c.flags)
            add(K::slot_flag, {f});
        if (!c.changes.empty()) {
            add(K::table_head, head);
            for (const SummaryRow &r : c.changes)
                add(K::table_row, {r.key, r.preset_value, r.spoolman_value, r.source});
        }
        for (const SummaryIgnored &i : c.ignored)
            add(K::ignored, {"Ignored " + i.key + " = " + i.value + " (" + i.reason + ")"});
    }
    if (!m.fetched_text.empty())
        add(K::footer, {m.fetched_text});
    if (!m.read_only_note.empty())
        add(K::footer, {m.read_only_note});
    return out;
}

}} // namespace Slic3r::Spoolman
