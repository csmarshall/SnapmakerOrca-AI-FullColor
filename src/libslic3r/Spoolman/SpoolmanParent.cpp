#include "SpoolmanParent.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <set>

namespace Slic3r { namespace Spoolman {

namespace {

using Words = std::vector<std::string>;

const std::set<std::string> kColours = {"black", "white", "red", "blue", "green", "purple", "rainbow", "orange", "grey", "gray", "natural",
                                        "transparent", "ivory", "carbon", "lava", "light", "dark", "yellow", "pink", "gold", "silver",
                                        "brown", "beige", "cyan", "magenta", "teal", "navy", "violet", "clear", "tan"};
// Functional words that change how a filament must be printed (esd, fr, aero are in the TSV names): a request for one must not fall back to
// the plain preset, so they are flags that a candidate has or does not have.
const std::set<std::string> kFlags = {"hf", "cf", "gf", "silk", "matte", "basic", "rapid", "glow", "highspeed", "esd", "fr", "aero"};
// Other material words. A leftover word of this kind in a Spoolman name (PC-ABS, "PLA PETG Blend", CoPA) means a blend: the family rules
// do not guess. Hand list (ASSUMED) from the filament_type column of the candidate TSV plus common materials.
const std::set<std::string> kMaterialWords = {"pla", "petg", "abs", "asa", "pc", "pa", "pctg", "pet", "pp", "pe", "pva", "hips", "tpu", "tpe", "bvoh",
                                              "sbs", "pha", "eva", "cope", "copa", "nylon", "ppa", "pps", "peek", "pei"};
// Words (and diameters/weights) that never describe the material family; ignored on both sides in every comparison.
const std::set<std::string> kStopWords = {"filament", "spool", "1.75", "2.85", "3", "1.75mm", "2.85mm", "3mm", "175", "285", "1kg", "2kg",
                                          "0.5kg", "500g", "750g", "1000g", "1000", "750"};
// Vendor spellings that mean the same vendor (canonical form on the right), lowercase. The one place to extend.
const std::vector<std::pair<std::string, std::string>> kVendorAliases = {{"bambu lab", "bambu"}, {"bambulab", "bambu"}};

std::string lower(std::string s)
{
    for (char &c : s)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return s;
}

// ASCII alphanumeric runs, lowercased; a '.' between two digits stays inside the token ("1.75mm", "2.0"); '+' is not part of a token but
// sets *had_plus; every other byte (spaces, '-', '(', UTF-8 symbols such as the trademark sign or a no-break space) separates.
// A name stops at '@' (the printer/nozzle suffix of preset names).
Words split(const std::string &text, bool stop_at_at, bool *had_plus = nullptr)
{
    Words w;
    std::string cur;
    auto flush = [&] { if (!cur.empty()) w.push_back(std::move(cur)); cur.clear(); };
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = text[i];
        if (stop_at_at && c == '@')
            break;
        if (std::isalnum(c))
            cur += char(std::tolower(c));
        else if (c == '.' && !cur.empty() && std::isdigit((unsigned char) cur.back()) && i + 1 < text.size() &&
                 std::isdigit((unsigned char) text[i + 1]))
            cur += '.';
        else {
            if (c == '+' && had_plus)
                *had_plus = true;
            flush();
        }
    }
    flush();
    return w;
}

// cf/gf optionally followed by digits only (CF20, GF30, CF10).
bool glued_flag(const std::string &t)
{
    if (t.size() < 2 || (t.compare(0, 2, "cf") != 0 && t.compare(0, 2, "gf") != 0))
        return false;
    return std::all_of(t.begin() + 2, t.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

// Folds two-word flags into one token: High Flow -> hf, High Speed -> highspeed, Carbon/Glass Fiber/Fibre -> cf/gf.
Words fold(const Words &w)
{
    Words o;
    for (size_t i = 0; i < w.size(); ++i) {
        const std::string next = i + 1 < w.size() ? w[i + 1] : std::string();
        if (w[i] == "high" && next == "flow")                              { o.push_back("hf"); ++i; }
        else if (w[i] == "high" && next == "speed")                        { o.push_back("highspeed"); ++i; }
        else if (w[i] == "carbon" && (next == "fiber" || next == "fibre")) { o.push_back("cf"); ++i; }
        else if (w[i] == "glass" && (next == "fiber" || next == "fibre"))  { o.push_back("gf"); ++i; }
        else if (glued_flag(w[i])) o.push_back(w[i].substr(0, 2));  // CF20, GF30
        else if (w[i] == "carbon") {
            // "Carbon" attached to a colour name stays a colour ("Carbon Black"); on its own it means carbon fibre.
            const std::string prev = i > 0 ? w[i - 1] : std::string();
            const bool colour_next = kColours.count(next) && next != "carbon", colour_prev = kColours.count(prev) && prev != "carbon";
            o.push_back(colour_next || colour_prev ? "carbon" : "cf");
        }
        else o.push_back(w[i]);
    }
    return o;
}

struct Classified
{
    std::set<std::string> flags;
    std::set<std::string> colours;
    std::set<std::string> rest; // descriptive words
    bool                  plus = false;
};

// A material / filament_type string: base word (first non-flag word), flags carried as suffix or extra words ("PLA-CF", "Silk PLA",
// "PETG HF"), other words, and a '+' marker.
struct Material
{
    std::string           base;
    std::set<std::string> flags;
    std::set<std::string> rest;
    bool                  plus = false;
    Words                 all; // every folded word, dropped from names
};

Material parse_material(const std::string &text)
{
    Material m;
    m.all = fold(split(text, false, &m.plus));
    for (const std::string &t : m.all) {
        if (kFlags.count(t))
            m.flags.insert(t);
        else if (m.base.empty())
            m.base = t;
        else if (!kStopWords.count(t))
            m.rest.insert(t);
    }
    return m;
}

// Removes `drop` words (vendor, material, "generic"), stop-words, colours; separates flags from descriptive words.
Classified classify(const std::string &name, const std::set<std::string> &drop, bool stop_at_at)
{
    Classified c;
    for (const std::string &t : fold(split(name, stop_at_at, &c.plus))) {
        if (t == "generic" || drop.count(t) || kStopWords.count(t)) {}
        else if (kFlags.count(t))   c.flags.insert(t);
        else if (kColours.count(t)) c.colours.insert(t);
        else                        c.rest.insert(t);
    }
    return c;
}

std::string join(const std::set<std::string> &s)
{
    std::string r;
    for (auto &x : s)
        r += (r.empty() ? "" : " ") + x;
    return r;
}

std::string canon_vendor(const std::string &v)
{
    std::string s;
    for (const std::string &w : split(v, false))
        s += (s.empty() ? "" : " ") + w;
    for (auto &a : kVendorAliases)
        if (s == a.first)
            return a.second;
    return s;
}

// The caller's token must be the WHOLE '@' suffix ("@U1 0.4 nozzle" does not match "@U1 0.4 nozzle HF"; a leading '@' in the token is
// optional).
bool has_token(const std::string &name, const std::string &token)
{
    const size_t at = name.find('@');
    if (at == std::string::npos || token.empty())
        return false;
    const std::string t = token[0] == '@' ? token.substr(1) : token;
    return name.compare(at + 1, t.size(), t) == 0 && (at + 1 + t.size() == name.size());
}

std::string base_name(const std::string &name)
{
    std::string b = name.substr(0, name.find('@'));
    while (!b.empty() && b.back() == ' ')
        b.pop_back();
    return b;
}

ParentChoice found(const std::vector<ParentCandidate> &cands, size_t i, std::string rule, std::string reason)
{
    ParentChoice r;
    r.chosen = &cands[i];
    r.index  = int(i);
    r.rule   = std::move(rule);
    r.reason = std::move(reason);
    return r;
}

// Zero hits: returns false. One hit: fills `out`, true. Several hits: if they are all the same family (same name before '@') and exactly
// one carries the caller's preferred printer token, that twin wins (noted); otherwise no choice with an "ambiguous" reason, true.
// List order never decides. Different families never get a tie-break.
bool resolve(const std::vector<ParentCandidate> &cands, std::vector<size_t> hits, const std::string &rule, const std::string &why,
             const std::string &token, ParentChoice &out)
{
    if (hits.empty())
        return false;
    std::string twin_note;
    if (hits.size() > 1 && !token.empty() &&
        std::all_of(hits.begin(), hits.end(), [&](size_t i) { return base_name(cands[i].name) == base_name(cands[hits[0]].name); })) {
        std::vector<size_t> pref;
        for (size_t i : hits)
            if (has_token(cands[i].name, token))
                pref.push_back(i);
        if (pref.size() == 1) {
            twin_note = "preferred '" + cands[pref[0]].name + "' (target printer) over its twin(s) of the same family";
            hits      = pref;
        }
    }
    if (hits.size() == 1) {
        auto notes = std::move(out.notes);
        out        = found(cands, hits[0], rule, why);
        out.notes  = std::move(notes);
        if (!twin_note.empty())
            out.notes.push_back(twin_note);
        return true;
    }
    out.rule = rule;
    out.reason = "ambiguous: ";
    for (size_t k = 0; k < hits.size(); ++k)
        out.reason += (k ? ", " : "") + cands[hits[k]].name;
    return true;
}

// Note-only colour check: colour words of external_id that the name lacks. Whole-word comparison; slugs glue words together
// ("darkgrey"), so a token is also stripped of trailing colour words of 4+ letters ("titan"/"tanned" never match "tan").
std::set<std::string> external_colours(const std::string &ext)
{
    std::set<std::string> found_set;
    for (std::string t : split(ext, false)) {
        if (kColours.count(t))
            found_set.insert(t);
        for (bool again = true; again;) {
            again = false;
            for (const std::string &w : kColours)
                if (w.size() >= 4 && t.size() > w.size() && t.compare(t.size() - w.size(), w.size(), w) == 0) {
                    found_set.insert(w);
                    t.resize(t.size() - w.size());
                    again = true;
                    break;
                }
        }
    }
    return found_set;
}

} // namespace

std::vector<std::string> colour_words() { return {kColours.begin(), kColours.end()}; }
std::vector<std::string> flag_words() { return {kFlags.begin(), kFlags.end()}; }
std::vector<std::string> stop_words() { return {kStopWords.begin(), kStopWords.end()}; }

std::string preferred_token_for_printer(const std::string &printer_name, const std::vector<ParentCandidate> &candidates)
{
    auto squash = [](const std::string &s) {
        std::string r;
        for (char ch : s)
            if (std::isalnum((unsigned char) ch) || ch == '.')
                r += char(std::tolower((unsigned char) ch));
        return r;
    };
    const std::string printer = squash(printer_name);
    std::string       best, best_squashed;
    for (const ParentCandidate &c : candidates) {
        const size_t at = c.name.find('@');
        if (at == std::string::npos)
            continue;
        std::string suffix = c.name.substr(at + 1);
        const std::string sq = squash(suffix);
        if (sq.size() > best_squashed.size() && printer.find(sq) != std::string::npos) {
            best          = suffix;
            best_squashed = sq;
        }
    }
    return best.empty() ? std::string() : "@" + best;
}

ParentChoice choose_parent(const Filament &f, const std::vector<ParentCandidate> &cands, const ParentOptions &opts)
{
    ParentChoice out;
    const std::string &token = opts.preferred_token;
    auto extra = [&](const char *k) -> const std::string * {
        auto it = f.extras.find(k);
        return it == f.extras.end() || f.extras_undecoded.count(k) ? nullptr : &it->second;
    };
    auto collect = [&](auto pred) {
        std::vector<size_t> h;
        for (size_t i = 0; i < cands.size(); ++i)
            if (pred(cands[i], i))
                h.push_back(i);
        return h;
    };

    // Rules 1 and 2. A value that matches NOTHING falls through to the next rule (with a note); a value that matches SEVERAL candidates is
    // ambiguous and returns no choice at once: it must not fall through to a fuzzier rule that would then silently pick one of them.
    if (const std::string *sid = extra("setting_id"); sid && !sid->empty()) {
        auto hits = collect([&](const ParentCandidate &c, size_t) { return c.setting_id == *sid; });
        if (resolve(cands, hits, "setting_id extra", "setting_id " + *sid, token, out))
            return out;
        out.notes.push_back("setting_id extra '" + *sid + "' matches no candidate");
    }

    if (const std::string *ofd = extra("slicer_settings_orcaslicer"); ofd && !ofd->empty()) {
        if (ofd->size() > kMaxExtraBytes)
            out.notes.push_back("slicer_settings_orcaslicer ignored: longer than 4096 bytes");
        else if (max_nesting(*ofd) > kMaxExtraDepth)
            out.notes.push_back("slicer_settings_orcaslicer ignored: nesting too deep");
        else {
            nlohmann::json j = nlohmann::json::parse(*ofd, nullptr, false);
            if (j.is_discarded() || !j.is_object())
                out.notes.push_back("slicer_settings_orcaslicer ignored: not a JSON object");
            else {
                auto str = [&](const char *k) -> std::string {
                    auto it = j.find(k);
                    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
                };
                const std::string id = str("id"), pn = str("profile_name"), gid = str("generic_id");
                if (!id.empty()) {
                    auto hits = collect([&](const ParentCandidate &c, size_t) { return c.setting_id == id || c.filament_id == id; });
                    if (resolve(cands, hits, "OFD slicer settings: id", "id " + id, token, out))
                        return out;
                }
                if (!pn.empty()) {
                    auto hits = collect([&](const ParentCandidate &c, size_t) { return c.name == pn || (!c.alias.empty() && c.alias == pn); });
                    if (resolve(cands, hits, "OFD slicer settings: profile_name", "profile_name " + pn, token, out))
                        return out;
                }
                if (!gid.empty()) {
                    auto hits = collect([&](const ParentCandidate &c, size_t) { return c.filament_id == gid; });
                    if (resolve(cands, hits, "OFD slicer settings: generic_id", "generic_id " + gid, token, out))
                        return out;
                }
                out.notes.push_back("slicer_settings_orcaslicer names no candidate");
            }
        }
    }

    // Name-based rules.
    const std::string vendor = f.vendor ? f.vendor->name : std::string();
    const Material    mat    = parse_material(f.material);
    std::set<std::string> drop(mat.all.begin(), mat.all.end());
    for (const std::string &w : split(vendor, false))
        drop.insert(w);
    Classified want = classify(f.name, drop, false);
    for (const std::string &fl : mat.flags)
        want.flags.insert(fl);
    want.plus = want.plus || mat.plus;
    if (f.external_id) {
        std::set<std::string> differ;
        for (const std::string &w : external_colours(*f.external_id))
            if (!want.colours.count(w))
                differ.insert(w);
        if (!differ.empty() && !want.colours.empty())
            out.notes.push_back("name colour (" + join(want.colours) + ") and external_id colour words (" + join(differ) +
                                ") disagree; the name was used, external_id never drives the choice");
    }

    if (mat.base.empty()) {
        out.reason = "no material: vendor/name/material and family rules need one";
        return out;
    }
    if (!want.rest.empty() || !mat.rest.empty()) {
        std::set<std::string> r = want.rest;
        r.insert(mat.rest.begin(), mat.rest.end());
        want.rest = r;
    }

    const std::string sv       = canon_vendor(vendor);
    const Words       sv_words = split(vendor, false);
    struct Cls { Classified c; bool type_ok; bool vendor_ok; bool generic; };
    std::vector<Cls> cc;
    for (const ParentCandidate &c : cands) {
        const Material ct   = parse_material(c.filament_type);
        const Words    nw   = split(c.name, true);
        const std::string cv = canon_vendor(c.filament_vendor);
        const bool generic = cv == "generic" || (!nw.empty() && nw[0] == "generic");
        // A candidate whose inherited vendor is "Generic" but whose NAME starts with the Spoolman vendor (e.g. "Snapmaker PLA Glow") counts
        // as that vendor's. Heuristic, ASSUMED: first name word == the Spoolman vendor (or its first word, or via the alias table).
        const bool vendor_ok = !sv.empty() && (cv == sv || (cv == "generic" && !nw.empty() &&
                                                             (canon_vendor(nw[0]) == sv || (!sv_words.empty() && nw[0] == sv_words[0]))));
        std::set<std::string> d(ct.all.begin(), ct.all.end());
        for (const std::string &w : split(c.filament_vendor, false))
            d.insert(w);
        if (vendor_ok)
            d.insert(sv_words.begin(), sv_words.end());
        Classified k = classify(c.name, d, true);
        k.flags.insert(ct.flags.begin(), ct.flags.end()); // "PLA-CF" is type PLA + flag cf
        k.rest.insert(ct.rest.begin(), ct.rest.end());
        k.plus = k.plus || ct.plus;
        cc.push_back({std::move(k), lower(ct.base) == mat.base, vendor_ok, generic});
    }

    // Rules 3-5 with the '+' marker compared exactly first; only if that finds nothing, ignoring it (noted).
    const bool by_material_only = want.flags.empty();
    std::set<std::string> blend;
    for (const std::string &w : want.rest)
        if (kMaterialWords.count(w) && w != mat.base)
            blend.insert(w);
    auto run = [&](bool strict) -> bool {
        auto plus_ok = [&](const Cls &k) { return !strict || k.c.plus == want.plus; };
        const std::string plus_note = "'+' marker differs between the Spoolman filament and the candidate; matched without it";
        {
            auto hits = collect([&](const ParentCandidate &, size_t i) {
                const Cls &k = cc[i];
                return k.vendor_ok && k.type_ok && plus_ok(k) && k.c.flags == want.flags && k.c.rest == want.rest;
            });
            if (resolve(cands, hits, "vendor + name + material",
                        "vendor " + vendor + ", name words '" + join(want.rest) + "', material " + f.material +
                            (want.flags.empty() ? "" : ", flags " + join(want.flags)),
                        token, out)) {
                if (!strict && out.chosen && cc[out.index].c.plus != want.plus)
                    out.notes.push_back(plus_note);
                return true;
            }
        }
        // Tier 2 (the printer vendor's own preset, listed for the printer) is only an alternative to a Generic library preset that is NOT
        // listed for the printer: it never turns a "no candidate" into a choice (a Glow PLA from another vendor stays unmatched).
        auto family = [&](size_t i, bool generic_unlisted) {
            const Cls &k = cc[i];
            return k.type_ok && plus_ok(k) && k.c.flags == want.flags && k.c.rest.empty() &&
                   (generic_unlisted ? (k.generic && !k.vendor_ok && !cands[i].printer_listed) :
                                       (!k.generic && !k.vendor_ok && cands[i].printer_listed));
        };
        bool has_unlisted_generic = false;
        for (size_t i = 0; i < cands.size(); ++i)
            has_unlisted_generic = has_unlisted_generic || family(i, true);
        for (int tier = 0; tier < 4 && blend.empty(); ++tier) {
            if (tier == 2 && !has_unlisted_generic)
                continue;
            auto hits = collect([&](const ParentCandidate &, size_t i) {
                const Cls &k = cc[i];
                return k.type_ok && plus_ok(k) && k.c.flags == want.flags && k.c.rest.empty() &&
                       (tier == 0 ? k.vendor_ok :
                        tier == 1 ? (k.generic && !k.vendor_ok && cands[i].printer_listed) :
                        tier == 2 ? (!k.generic && !k.vendor_ok && cands[i].printer_listed) :
                                    (k.generic && !k.vendor_ok && !cands[i].printer_listed));
            });
            const std::string rule = by_material_only ? "material alone" : "material family with flags";
            const std::string why  = "material " + f.material + (want.flags.empty() ? " (no flag requested)" : ", flags " + join(want.flags)) +
                                    (tier == 0 ? ", vendor-specific" :
                                     tier == 1 ? ", Generic, listed for the printer" :
                                     tier == 2 ? ", the printer vendor's, listed for the printer" : ", Generic");
            if (resolve(cands, hits, rule, why, token, out)) {
                if (out.chosen && by_material_only)
                    out.notes.push_back("choice by material only");
                if (!strict && out.chosen && cc[out.index].c.plus != want.plus)
                    out.notes.push_back(plus_note);
                return true;
            }
        }
        return false;
    };
    if (!want.rest.empty())
        out.notes.push_back("words ignored in the family match: " + join(want.rest));
    if (run(true) || run(false))
        return out;
    if (!blend.empty()) {
        out.reason = "blend: " + join(blend);
        return out;
    }
    out.reason = "no compatible candidate for material " + f.material + (want.flags.empty() ? "" : " with flags " + join(want.flags));
    return out;
}

}} // namespace Slic3r::Spoolman
