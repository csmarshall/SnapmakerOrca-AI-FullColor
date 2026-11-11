#ifndef slic3r_SpoolmanMapping_hpp_
#define slic3r_SpoolmanMapping_hpp_

// Pure mapping of one parsed Spoolman Filament onto validated filament-preset overrides. No network, no GUI, no preset mutation
// (applying the overrides to a preset is a later step). No printer-specific code.
//
// VERIFIED (against v2.5.0 sources and by tests): every option key used exists in print_config_def; the filament-option check uses
// Preset::filament_options() only; values are checked against the real
// ConfigOptionDef (type, min/max, enum values); absent Spoolman fields give no override; precedence parent < native < extra < ofd.
// ASSUMED, NOT verified (the units/meaning are not checked against Spoolman or OFD documentation or a real printer):
//  - flow_ratio equals Orca's filament_flow_ratio multiplier; fan speeds are percent (0..100), not 0..255;
//  - dry_temperature (deg C) / dry_time (unit unknown, Orca key is declared without a unit) mean the same on a non-Bambu printer
//    as on the Bambu AMS keys they are written to; chamber_temperature is deg C;
//  - Spoolman's filament price is per spool of `weight` grams, so filament_cost (money per kg) = price * 1000 / weight;
//  - extra field names follow the OFD schema as given in the design (decision D3, reconcile later);
//  - the plate-temperature key list below is every plate key in v2.5.0's filament options, and Spoolman's single bed temperature
//    is meant for every plate the parent already enables (decision D1).
// Only keys on the allowlist (spoolman_allowlist()) can be set; everything else is rejected ('not a filament option' or
// 'not on the Spoolman allowlist').
// Never silently clamps and never partially applies: an invalid value is reported in `rejected`, its key gets no override.
// A pressure_advance override from any source also yields enable_pressure_advance (1 when > 0, else 0; same source). color_hex sets
// default_filament_colour only (the slot colour is set at assignment, later). Extra and ofd data can never set G-code, inheritance,
// compatibility, identity or material-type keys (see the denylist in the .cpp); strings are capped at 256 bytes, no control characters.
// Per-key tightened limits live in kKeyLimits; zero or negative Spoolman temperatures mean 'unset'. Values are the single-element string form of
// the option (e.g. "0.04", "250", "#FF1523", "Snapmaker"), without the vector quoting of the serialized preset form.

#include "SpoolmanCatalog.hpp"

#include <string>
#include <vector>

namespace Slic3r {
class DynamicPrintConfig;
namespace Spoolman {

struct Override
{
    std::string key;
    std::string value;
    std::string source;          // native:<spoolman field> | extra:<field> | ofd:slicer_settings_orcaslicer
    std::string replaced_source; // non-empty when this replaced an earlier (lower precedence) override of the same key
    std::string replaced_value;
};

struct Rejected
{
    std::string key_or_field;
    std::string reason;
    bool        warning = false; // informational, not an error (e.g. min/max_bed_temperature)
};

struct MappingResult
{
    std::vector<Override>    overrides;
    std::vector<Rejected>    rejected;
    std::vector<std::string> display_only; // "weight=1000", "spool_weight=140"; no override
};

enum class AllowedKind { Float, Int, Bool, Text, Colour };

// One entry of the allowlist of keys Spoolman data may set (see the table in SpoolmanMapping.cpp). Number kinds carry explicit finite limits.
struct AllowedKey
{
    const char *key;
    AllowedKind kind;
    double      lo;
    bool        lo_exclusive;
    double      hi;
    bool        native_only; // set by the native field mapping only, refused from extra/ofd
};

const std::vector<AllowedKey> &spoolman_allowlist();

// Plate temperature keys that receive Spoolman's bed temperature when the parent's value is non-zero.
const std::vector<std::string>& plate_temperature_keys();

// True when `key` is a filament preset option and exists in print_config_def.
bool is_filament_option(const std::string &key);

MappingResult map_filament(const Filament &filament, const DynamicPrintConfig &parent);

}} // namespace Slic3r::Spoolman

#endif
