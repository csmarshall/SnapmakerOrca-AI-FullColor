# Spoolman sync — High Level Design

## Purpose and scope

Spoolman sync turns the filament catalog of a Spoolman server into Orca user filament presets, and lets the AMS/filament sync pick the preset that belongs to the spool in a slot instead of the one the RFID type suggests. Each Spoolman filament becomes a user preset that inherits the closest system preset; Spoolman's values are written over it, and the source of every written value is recorded. Spoolman is the authoritative, read-only source: nothing is ever written to Spoolman.

This is the generic core. It has no printer-specific code, and the app preference `spoolman_preset_sync` is off by default. With it off, no request of any kind is made to Moonraker or Spoolman. The dialog that shows the result of a sync and the adapter that tells which Spoolman spool sits in a slot of a particular printer are not part of this change (see "What is verified" below).

## Data flow

1. **Catalog fetch.** The Moonraker printer agent asks `GET /server/spoolman/status` and, only if Spoolman is connected, fetches `/v1/vendor`, `/v1/filament` and `/v1/spool?allow_archived=false` through Moonraker's Spoolman proxy. The proxy call is a POST to Moonraker that carries `request_method: "GET"` and never a body. `SpoolmanCatalog` parses the replies (a `{"result": ...}` envelope or a bare array); it tells a malformed reply, an HTTP failure and an empty list apart. Optional Spoolman fields stay optional: a filament without an extruder temperature does not become 0.
2. **Mapping.** `SpoolmanMapping` turns one filament into validated overrides of option keys, each checked against the real `ConfigOptionDef` (type, minimum, maximum) and against an allowlist.
3. **Parent choice.** `SpoolmanParent` picks the existing system preset the new preset inherits from, among the candidates compatible with the target printer. Nothing found means the filament is skipped and reported, never an arbitrary preset.
4. **User preset upsert.** `upsert_catalog` creates or refreshes one user preset per filament per parent, named `<Vendor> <Spoolman name> (Spoolman) @<parent suffix>`, and returns a report of everything it did.
5. **Sync tier.** `PresetBundle::sync_ams_list` has one extra tier before upstream's `filament_id` match. A tray that carries a Spoolman join selects its linked preset; a tray without one takes upstream's path unchanged.
6. **Summary.** `build_summary` turns the reports into a plain data model (one card per slot, notices, dialog-or-toast decision). It has no GUI dependency.

### Mapping and precedence

The precedence is parent < native fields < extra fields < the OFD `slicer_settings_orcaslicer` overrides. Later sources replace earlier ones for the same key, and the replaced value and source are kept in the override record.

| Source | Spoolman field | Orca option |
|--------|----------------|-------------|
| native | `settings_extruder_temp` | `nozzle_temperature`, `nozzle_temperature_initial_layer` |
| native | `settings_bed_temp` | each plate temperature key (and its initial-layer key) the parent has non-zero |
| native | `density`, `diameter` | `filament_density`, `filament_diameter` |
| native | `price` | `filament_cost` (`price * 1000 / weight`) |
| native | `vendor.name` | `filament_vendor` |
| native | `color_hex` | `default_filament_colour` |
| extra | `pressure_advance` | `pressure_advance`, plus `enable_pressure_advance` |
| extra | `flow_ratio`, `max_volumetric_speed` | `filament_flow_ratio`, `filament_max_volumetric_speed` |
| extra | `min/max_print_temperature` | `nozzle_temperature_range_low/high` |
| extra | `min/max_fan_speed`, `chamber_temperature` | `fan_min/max_speed`, `chamber_temperature` |
| extra | `dry_temperature`, `dry_time` | `filament_dev_ams_drying_temperature`, `filament_dev_ams_drying_time` |
| extra | `slicer_settings_orcaslicer` | any allowlisted key, applied last |

`weight`, `spool_weight` and `remaining_weight` are display only. `min/max_bed_temperature` only produce a warning. `setting_id` is used for matching only. `filament_type` is never written from Spoolman's `material`. Slot colour (`filament_colour`) is a project key and is not written into a preset.

Spoolman stores every extra field as `json.dumps(value)` in a string, so a float arrives as `"0.98"`. A decoded number or boolean keeps its exact text; arrays, objects and null are kept raw and flagged, and an undecodable value is reported as rejected.

### Parent choice

First match wins, and the rule that fired is reported. Rules 1 and 2 use data written for the purpose, rules 3 to 5 are heuristics.

1. The extra `setting_id` equals a candidate's `setting_id`.
2. The `slicer_settings_orcaslicer` object's `id`, then `profile_name`, then `generic_id`.
3. Vendor (by name), name and material, with colour words and stop-words removed and the flags (HF, CF, GF, Silk, Matte, ...) equal.
4. Material family with flags: a requested flag must match exactly, a missing or an unrequested flag loses.
5. Material alone, only when no flag was requested.

Two equally good candidates in one rule give no choice ("ambiguous"); list order never decides. `external_id` never drives the choice. The flag vocabulary, the colour-word and stop-word lists are guesses from preset names, not from a specification.

### The link

`filament_spoolman_link` is a filament option holding JSON text: `{"id": N, "external_id": "...", "sources": {"key": "source", ...}}`. It is a development-mode option, one string per filament preset, empty meaning not linked. Parsing is strict: at most 4096 bytes, nesting depth 8, no NUL byte, `id` required and in 1..INT_MAX, and anything else is treated as "no link". Serializing refuses text over the cap, so a written link cannot silently stop loading.

### Sync tier

A tray carries its Spoolman join in three plain config keys: `spoolman_filament_id`, `spoolman_external_id` and `spoolman_setting_id`. The core only reads them; it does not fill them. Rules in order, the first rule with any candidate wins:

1. The tray's Spoolman id equals a user preset's link id.
2. The link's `external_id` equals the tray's (non-empty, exact, case-sensitive).
3. The preset's `setting_id` equals the tray's (non-empty; system presets are allowed here only).

If more than one preset matches under the winning rule, nothing is guessed: the slot is reported as ambiguous and upstream's path runs. Placeholder trays never use the tier. A linked preset inherits its parent's `filament_id`, so upstream's own comparison could never select it; the tier records its picks so that the later `synced_preset_name` step does not override them.

## Safety properties

- **Read only.** Only `GET` requests are made, through the proxy, and the code has no write path to Spoolman.
- **Size cap.** A body larger than 4 MiB is rejected, enforced both while downloading (`Http::size_limit`) and before parsing.
- **Depth scan.** An iterative, string-aware scan rejects bodies nested deeper than 16 levels before they reach the JSON library. Extra-field values and the link have their own, smaller depth and size limits.
- **Allowlist, not denylist.** Values from extra fields and the OFD overrides can only set the roughly 30 keys on `kAllowedKeys`, each with explicit finite limits (for example `filament_flow_ratio` 0.5..1.5, fan speeds 0..100, nozzle temperatures 150..500). A value out of range is rejected, never clamped, and a rejected value is never partially applied. A denylist (G-code, `inherits`, `compatible_*`, `*_condition`, `filament_type`, identity keys) stays as a second layer. Optional printer limits for the nozzle and bed temperatures can be supplied by the caller, as v2.5.0 has no such maximum of its own.
- **Untrusted strings.** Strings from Spoolman have no control characters and at most 256 bytes; the only text options that can be set are `filament_vendor` and `default_filament_colour`, and only from the native fields. Generated preset names are length-capped, stripped of unsafe characters and checked against reserved Windows names.
- **Link kept out of G-code and cloud sync.** `filament_spoolman_link` is in `banned_keys` in `GCode.cpp`, so it is not in the G-code config block; it is in the ignore list for the differences to the system preset; and it is excluded from the cloud preset-sync payload because the id only means something on this machine. It still travels in the preset JSON and the project 3MF.
- **Nothing overwritten.** The upsert refreshes only presets that carry the same Spoolman id, inherit the chosen parent, and are not system, default, embedded, bundle or external presets. A name collision with an unlinked user preset gets a counter and never overwrites it. Several candidates with no exact name match are reported as ambiguous and left alone.
- **No bulk deletion.** An empty catalog never deletes or orphans presets, and nothing is deleted automatically. A preset whose Spoolman filament is gone is reported as orphaned and kept.

## Failure behaviour

If Moonraker is not connected to Spoolman, the status is unknown, a request fails or times out (3 s to connect, 6 s in total per request), or a reply is malformed, too large or too deep, the catalog sync is skipped. Linked presets keep their last values, and the slot sync falls back to upstream's path, which is the RFID type of the tray. The per-session cache keeps the last good fetch per printer; a failed fetch never replaces it and is not cached as fresh. The summary model has a stale notice that tells an unreachable Spoolman from an unusable reply and whether an earlier sync exists.

The fetch is blocking. A caller must run it off the GUI thread; the worst case for the three requests plus the status call is about 18 s.

## Constraints

- G-code is meant to be identical with the preference off or with no linked presets. The link option is in the `steps_ignore` list of `Print.cpp`, so a change of the link alone invalidates no slicing step; as with `filament_notes`, the G-code export is still re-run once.
- A bed temperature is applied all-or-nothing across the plate keys: a value above one key's maximum rejects the whole value.
- Spoolman has one nozzle and one bed temperature, while Orca has initial-layer and per-plate keys. The nozzle temperature goes to both the normal and the initial-layer key; the bed temperature goes to every plate key the parent has non-zero.
- The extra field names follow the Open Filament Database schema as described in the design, and may need to be reconciled with what a Spoolman actually stores.

## What is verified / not verified

Verified, as far as unit tests can show: catalog parsing against 22 captured filaments (trimmed fixtures in `tests/data/spoolman`), the mapping and its limits, the parent choice on those filaments and about 20 invented names, the link round trip, the transport limits and the Moonraker proxy encoding, the upsert (idempotence, orphans, collisions), the sync tier (a linked tray beats an RFID "PETG Basic"; with no link the results are unchanged), the exclusion of the link from the G-code and the cloud payload, and the summary model.

What these tests cannot catch: the expected values come from the same fixtures the parser reads, so they show that the code agrees with itself, not that the mapping is right.

Not verified, hopefully clear enough to check before relying on any of it:

- **Never exercised against a live Spoolman or Moonraker from this code in a build.** The proxy request shape and the version 2 reply envelope were read in Moonraker's documentation and source, not run. The `allow_archived` query parameter is assumed (archived spools are also dropped locally).
- **Units are assumptions.** `price` is taken as per spool of `weight` grams (`filament_cost = price * 1000 / weight`), and the fixtures contain no price. `flow_ratio` is taken as Orca's multiplier. Fan speeds are taken as percent (0..100), not 0..255. `dry_time` has an unknown unit (hours assumed), and `dry_temperature`/`dry_time` are written to the Bambu AMS drying keys, whose meaning on other printers is unknown. `chamber_temperature` is taken as degrees C. The Spoolman fixtures have no values in any extra field except `variant`, so the extra and OFD paths have only been exercised with invented data.
- **The sync dialog is not on this branch.** Only its data model (`build_summary`) exists, with tests; the strings have never been seen in a real dialog, and are not translated.
- **The printer adapter is not on this branch.** Nothing fills `spoolman_filament_id`, `spoolman_external_id` or `spoolman_setting_id` on a tray, so the tier is dormant until a printer agent does. `Plater.cpp` `build_tray_config` and `DevAmsTray` are unchanged. Whether AFC or Happy Hare publish a per-slot Spoolman spool id is unverified.
- **No GUI caller.** As far as this branch goes, nothing in the GUI calls the catalog fetch or `upsert_catalog`; the preference has no settings page entry, and the filament tab does not yet treat linked presets as read-only (saving a linked preset under a new name does not yet drop the link).
- Other Spoolman versions, whether the chosen parent is physically the best one, and the behaviour with many presets beyond the 22 fixtures have not been checked.
