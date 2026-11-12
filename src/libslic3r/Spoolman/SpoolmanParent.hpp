#ifndef slic3r_SpoolmanParent_hpp_
#define slic3r_SpoolmanParent_hpp_

// Pure choice of the existing filament preset a Spoolman filament's user preset should inherit from. No network, no GUI, no wx, no
// PresetBundle access. The caller supplies candidates already filtered to the presets compatible with the target printer; a helper that
// builds candidates from a PresetBundle is deliberately left for S5 (it needs a bundle fixture).
//
// Rules, first match wins (the rule that fired is returned for the UI):
//  1. extra "setting_id" equals a candidate's setting_id (exact).
//  2. extra "slicer_settings_orcaslicer" (JSON object, same limits as the mapping: 4096 bytes, depth 8): "id" (candidate setting_id or
//     filament_id), then "profile_name" (candidate name or alias), then "generic_id" (candidate filament_id).
//     In rules 1 and 2 a value matching NOTHING falls through (noted); a value matching SEVERAL candidates returns none at once.
//  3. vendor (matched BY NAME, ASCII case-insensitive, via a tiny alias table: "Bambu Lab" == "Bambu") + name + material: the Spoolman name
//     with colour words, stop-words (filament, spool, 1.75mm ...), vendor and material words removed must equal the candidate name
//     (same removals, "@..." suffix cut) as a set of words, and the flags must be equal. A candidate whose inherited vendor is "Generic"
//     but whose name starts with the Spoolman vendor ("Snapmaker PLA Glow") counts as that vendor's (heuristic, ASSUMED).
//  4. material family with flags (a flag was requested): candidate type == material, flags EXACTLY equal (a missing flag loses, an
//     unrequested flag loses), no other descriptive words; vendor-specific candidates before Generic ones.
//  5. material alone, ONLY when no flag was requested; same exactness and the same vendor-before-Generic order; noted as such.
// Rules 3-5 first compare the '+' marker exactly ("PLA+" vs "PLA"); only if nothing matches is it ignored, with a note, so eSUN "PLA" never
// silently becomes "eSUN PLA+" while a Generic PLA exists. Flags come from the name AND from the material / filament_type suffix
// ("PLA-CF" = PLA + cf, "Silk PLA" = PLA + silk), on both sides.
// Two equally good candidates in one rule/tier give no choice ("ambiguous: ..."); list order never decides. Only a same-family twin
// ("Generic PETG @U1 0.4 nozzle" vs "Generic PETG @System") is separated, by ParentOptions::preferred_token. Nothing found gives no
// choice and NEVER an arbitrary preset. Spoolman `material` is used for matching only. `external_id` never drives the choice; a colour
// word in it (whole word, or the trailing colour words of a glued slug) that the name does not carry only adds a note.
//
// VERIFIED: behaviour on the 22 real filaments in tests/data/spoolman and on ~20 invented names, against candidates_u1_0.4.tsv, generated
// from the real profiles by tests/data/spoolman/make_candidates.py (Snapmaker '@U1 0.4 nozzle' + every OrcaFilamentLibrary preset visible
// to all printers, types/vendors resolved through inherits).
// ASSUMED, a guess from those names and profile names, not from any specification:
//  - the flag vocabulary: HF (also "High Flow"), CF (also "Carbon Fiber"), GF ("Glass Fiber"), Silk, Matte, Basic, Rapid, Glow,
//    HighSpeed ("High Speed"), ESD, FR, Aero (functional: a request must never fall back to the plain preset). Digit-glued CF20/GF30 are
//    cf/gf; "Carbon" alone is cf, "Carbon Black" a colour. Any other word left after removing colours, flags, vendor, material is a "descriptive word"
//    (e.g. Translucent, PolyLite, Pro): on a candidate it disqualifies it from rules 4/5, on a Spoolman name it is ignored in rule 4
//    (noted) and must match in rule 3;
//  - a leftover word that is ANOTHER material (PC-ABS, "PLA PETG Blend", CoPA; kMaterialWords) makes rules 4/5 return none, "blend: <words>";
//  - the colour words (see colour_words() in the .cpp) and the stop-word list (stop_words()); "Carbon" followed by Fiber/Fibre is CF;
//  - a candidate is "Generic" when its filament_vendor or its first word is "generic" (data convention, not printer specific);
//  - the Spoolman material is compared to candidate filament_type case-insensitively on its first non-flag word.

#include "SpoolmanCatalog.hpp"

#include <string>
#include <vector>

namespace Slic3r { namespace Spoolman {

struct ParentCandidate
{
    std::string name;          // preset name e.g. "Generic PETG HF @U1 0.4 nozzle"
    std::string alias;
    std::string filament_type; // PETG, PLA ...
    std::string filament_vendor;
    std::string setting_id;
    std::string filament_id;
};

struct ParentChoice
{
    const ParentCandidate   *chosen = nullptr; // points into the caller's vector; nullptr = no choice
    int                      index  = -1;      // index into the caller's vector, -1 = no choice
    std::string              rule;             // which rule fired, shown in the UI; empty when none
    std::string              reason;           // why; for no choice: why nothing was chosen ("ambiguous: ...", "no candidate ...")
    std::vector<std::string> notes;
};

struct ParentOptions
{
    // Substring of the target printer's preset names (e.g. "@U1 0.4 nozzle"), supplied by the caller. When two or more candidates tie and
    // are the SAME family (same name before '@', e.g. "Generic PETG @U1 0.4 nozzle" and "Generic PETG @System"), exactly one whose "@" suffix IS this token
    // (whole suffix, not a prefix) wins. Empty = no preference. Never separates different families.
    std::string preferred_token;
};

ParentChoice choose_parent(const Filament &filament, const std::vector<ParentCandidate> &candidates, const ParentOptions &options = {});

// The vocabularies, exposed so tests and docs can list them (sorted).
std::vector<std::string> colour_words();
std::vector<std::string> flag_words();
std::vector<std::string> stop_words();

}} // namespace Slic3r::Spoolman

#endif
