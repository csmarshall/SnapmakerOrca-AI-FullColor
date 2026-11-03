# OrcaSlicer Remote API

A token-authenticated HTTP + WebSocket API embedded in OrcaSlicer. It exposes the
**running** application for automation: read status, read/write config live, kick
off slicing, and subscribe to a live event stream.

- **Off by default.** Enable it explicitly in Preferences.
- **Single API version:** `v1`, all paths under `/api/v1`.
- **Machine-readable spec:** [`openapi.yaml`](./openapi.yaml).

## Enabling

Preferences → **Remote API**:

| Setting | Meaning |
|---|---|
| **Enable** | Start the server (applied immediately on closing Preferences). |
| **Bind LAN** | Off = listen on `127.0.0.1` only. On = listen on `0.0.0.0` (reachable from other machines on your network). |
| **Port** | Default `13130`. |
| **Token** | 32 hex chars, auto-generated on first run. **Regenerate** invalidates existing clients. |

> Binding to the LAN exposes live control of your slicer to every machine on the
> network. Keep the token secret and prefer localhost unless you need remote access.

## Authentication

- **HTTP:** send the header `X-Api-Token: <token>` on every request. Missing/wrong
  token → `401 {"error":"unauthorized"}`.
- **WebSocket:** pass `?token=<token>` in the URL (browsers can't set WS headers).

## Endpoints

Base URL: `http://<host>:13130/api/v1`

| Method | Path | Purpose |
|---|---|---|
| GET | `/status` | App/project/preset status + slice validity |
| GET | `/config[?keys=a,b]` | Read merged config (secrets redacted) |
| PUT | `/config` | Apply config edits live (atomic) |
| PUT | `/preset` | Select a preset (409 `unsaved_changes` unless `discard:true`) |
| POST | `/preset/discard` | Drop a collection's unsaved edits |
| POST | `/preset/save` | Save the current edits as a user preset |
| POST | `/slice` | Start slicing the current plate |
| GET | `/slice/status` | Current/last slice state + stats + warnings |
| WS | `/events?token=…` | Live event stream |
| GET | `/plates` | List the plates: index, name, current, locked, objects on each |
| POST | `/plates` | Add a plate (`{"select": true}` by default) |
| POST | `/plates/{i}/select` | Make plate `i` the current one |
| DELETE | `/plates/{i}` | Delete plate `i` (its objects are parked beside the bed, not deleted) |
| POST | `/objects/{id}/plate` | Move an object onto plate `i` |
| GET | `/objects/{id}` | One object with its volumes (parts, modifiers, blockers, enforcers) |
| PUT | `/objects/{id}/volumes/{v}/config` | Per-volume setting overrides (atomic, same guards as the object endpoint) |
| POST | `/objects/{id}/modifiers` | Add a box or cylinder modifier / support blocker / enforcer |
| DELETE | `/objects/{id}/volumes/{v}` | Delete a non-part volume |

### GET /status

```bash
curl -H "X-Api-Token: $TOK" http://localhost:13130/api/v1/status
```
```json
{
  "app": "OrcaSlicer", "app_version": "2.3.2", "api_version": "1.0",
  "capabilities": ["status","config","slice","events"],
  "project": "",
  "objects": [{"name":"cube","size_mm":[20,20,20]}],
  "presets": {"printer":"...","print":"...","filaments":["..."]},
  "modified": {"print":["layer_height"],"filament":[],"printer":[]},
  "slice_result_valid": false,
  "slicing": false
}
```

### GET /config

```bash
curl -H "X-Api-Token: $TOK" "http://localhost:13130/api/v1/config?keys=layer_height,wall_loops"
```
```json
{ "config": { "layer_height": "0.2", "wall_loops": "2" } }
```
Omit `?keys=` to get the full merged config. Values are canonical strings (same
form as `.ini`/`.3mf`).

### PUT /config  (atomic)

```bash
curl -X PUT -H "X-Api-Token: $TOK" -H "Content-Type: application/json" \
  -d '{"layer_height":0.28,"wall_loops":3}' \
  http://localhost:13130/api/v1/config
```
```json
{ "applied": ["layer_height","wall_loops"], "errors": {} }
```
Applied through the GUI's own edit path (dirty markers + live refresh + slice
invalidation). **All-or-nothing:** if any key is unknown or invalid, nothing is
applied and you get `422` with per-key reasons:
```json
{ "applied": [], "errors": { "layer_height": "value out of range", "nope": "unknown_key" } }
```
Other errors: `400 body_must_be_object`, `400 invalid_json`, `504 ui_timeout`.

#### Per-filament keys

`GET /config` returns the **merged** config, so a vector-valued filament key holds
one run of values per loaded filament slot:

```json
{ "config": { "hot_plate_temp": "70,100,70,70,35" } }
```

`PUT /config`, by contrast, writes the single filament preset open in the Filament
tab. Those two shapes are not interchangeable, so **writing a merged value back is
rejected**:

```json
{ "applied": [], "errors": { "hot_plate_temp": "per_filament_length_mismatch: the filament preset open in the Filament tab holds 1 value(s), got 5. ..." } }
```

Pass only that preset's own value (`{"hot_plate_temp": 70}`), and pick the slot with
`filament_slot` (0-based, the same numbering as `PUT /preset`'s `extruder`):

```json
{ "filament_slot": 2, "nozzle_temperature": "240,240,240,240,240" }
```

That opens slot 2's preset in the Filament tab (as the slot's edit button does) and
writes it. The edit is to the **preset**, so every slot using that preset changes; the
response lists them:

```json
{ "applied": ["nozzle_temperature"], "errors": {},
  "filament_slot": { "filament_slot": 2, "preset": "Generic PETG @U1 0.4 nozzle", "slots_sharing_preset": [1, 2] } }
```

Errors: `400 bad_filament_slot` (not an integer 0-255), `422 bad_filament_slot` (no
such slot), `409 unsaved_filament_edits` (another preset is open in the Filament tab
with unsaved changes; switching would drop them, so nothing is done). The switch
happens before the keys are validated, so a batch refused with `422` may still leave
the Filament tab showing the requested slot's preset. Written values stay unsaved
edits, like a change typed in the GUI, so writing a second slot that uses a different
preset gets `409` until the first is saved or discarded. Without `filament_slot`, the
preset already open in the Filament tab is written. Writing a key back unchanged
still works on a single-filament setup, where merged and per-preset shapes coincide.

Earlier builds accepted the merged string and stored all of it in the one preset.
The next merge then spliced that whole list in at *every* slot using that preset,
so the value grew on each write (5 entries → 13 → …) and the per-slot settings were
lost. If you hit that, re-select the filament preset to restore it.

#### Project-scope keys (multi-material / CFS)

Most keys resolve against the edited print, filament or printer preset. A few
belong to the *project* instead, and only these seven are writable:

| Key | Shape | Constraint |
|---|---|---|
| `flush_volumes_matrix` | `,`-separated floats | exactly `filaments² × nozzles` values, each in `[0, 20000]` |
| `flush_multiplier` | `,`-separated floats | one per nozzle, each in `[0, 3]` |
| `filament_colour` | `;`-separated `#RRGGBB` | recolour only — the count is fixed by the filament presets |
| `curr_bed_type` | enum name | `"Default Plate"` is rejected |
| `prime_volume_mode` | enum name | — |
| `wipe_tower_x`, `wipe_tower_y` | float | applies to the current plate only |

```bash
curl -X PUT -H "X-Api-Token: $TOK" -H "Content-Type: application/json" \
  -d '{"flush_multiplier":"0.3","flush_volumes_matrix":"0,280,280,280,280,0,280,280,280,280,0,280,280,280,280,0"}' \
  http://localhost:13130/api/v1/config
```

The size and range checks are not decoration: these vectors are indexed by the
slicer with arithmetic bounded by a *different* vector, so a wrong-length write
is an out-of-bounds access rather than a validation error. Values are also vetted
as text before parsing, because the vector deserializers turn an unparsable token
into `0` and report success — which would silently mean "no purge at all".

The remaining project keys stay read-only (they appear in `GET /config` but not
in `PUT`) and report why: `values_are_unchecked_extruder_indices` (`filament_map`,
`filament_volume_map`, `filament_nozzle_map`), `tied_to_filament_map`
(`filament_map_mode`, `nozzle_volume_type`), `indexed_unchecked_by_the_colour_picker`
(`filament_colour_type`, `filament_multi_colour`), `derived_from_flush_volumes_matrix`
(`flush_volumes_vector`), `live_printer_state_not_a_setting`
(`has_filament_switcher`, `enable_filament_dynamic_map`).

### PUT /preset

`{"type":"print|filament|printer","name":"...","extruder":0?,"discard":false?}` selects a
preset. Unsaved edits are never dropped silently: if any settings group is modified the
call fails with 409 `unsaved_changes` and `dirty` lists the groups. `discard:true` drops
the unsaved edits in EVERY dirty group (print, filament and printer), not just the named
type, and the response lists them in `discarded_changes`. A non-boolean `discard` is 400
`bad_discard`. To drop one collection's edits without selecting anything use
`POST /preset/discard`.

### POST /preset/discard

`{"type":"print|filament|printer"}` reverts that collection to its saved preset through the
tab's own "back to saved values" path (`Tab::on_roll_back_value`), opens no dialog and
refreshes the tab and sidebar. It can only return values to what is saved on disk.

    {"type":"print","preset":"0.20mm Standard","discarded":true,"dropped_keys":["wall_loops"]}

When nothing was modified it answers 200 with `discarded:false` and empty `dropped_keys`.
Errors: 400 `missing_fields`/`unknown_type`, 409 `filament_editor_open` (the sidebar
filament editor is open, so the refresh could rewrite the slot being edited; close it
first), 500 `discard_failed`. Discarding printer edits can change the number of filament
slots, as it does in the GUI.

### POST /preset/save

`{"type":"...","name":"...","detach":false?}` saves the collection's current edits as a
user preset (create or update, no dialog). System presets cannot be overwritten (409).

For `type=filament` the response also reports what happened to the slots:
`previous_preset` (the preset that was being edited), `repointed_slots` (0-based slots
whose preset changed) and `slots_using_saved_preset` (every slot on the saved name
afterwards). The GUI's save-as moves exactly the slots that held the old preset (for
example a system preset opened with `filament_slot`) to the new user preset and leaves
the others alone; saving under the same name moves none. This is read from the source and
reported, not re-implemented here; if a slot ever moved to a different name the call fails
with 500 `slot_repoint_unexpected` listing the slots. Caveat: saving over a DIFFERENT
existing user preset changes the settings of any slot already using that name, which
`slots_using_saved_preset` makes visible.

### POST /slice

```bash
curl -X POST -H "X-Api-Token: $TOK" http://localhost:13130/api/v1/slice
```
- `202 {"started":true}` — slicing began (watch events or poll `/slice/status`).
- `200 {"started":false,"already_valid":true}` — plate already sliced.
- `409 {"error":"already_slicing"}` — a slice is in progress.
- `422 {"error":"nothing_to_slice"}` — no objects on the plate.

### GET /slice/status

```json
{
  "state": "done", "percent": 100, "message": "",
  "stats": { "estimated_time": "27m 23s", "estimated_time_seconds": 1643,
             "filament_used_mm": 1582, "filament_used_g": 4.72, "total_cost": 0.094 },
  "warnings": [ { "level": 3, "message": "bed temp too high…", "code": "1000C001" } ]
}
```
`state` is one of `idle | slicing | done | error`. `stats` appears once done.

### WS /events

```
ws://<host>:13130/api/v1/events?token=<token>
```
Each message is a JSON object with an `event` field:

| event | payload |
|---|---|
| `slice.started` | `state,percent,message` |
| `slice.progress` | `state:"slicing",percent,message` (stage text) |
| `slice.done` | `state:"done",percent:100,stats{…}` |
| `slice.error` | `state:"error",message` |
| `slice.cancelled` | `state:"idle",message:"cancelled"` |
| `slice.plate_changed` | the current plate's state after a plate route: `done` + `stats`, or `idle` + `message` |
| `config.changed` | `tabs:[print\|filament\|printer\|sla_print\|sla_material\|other]` |
| `project.opened` | `project:"<filename>"` |

> `slice.*` events carry `stats` (on done) but not the slice `warnings` array —
> call `GET /slice/status` for warnings.

## Plates

`slice`, `gcode`, `plate/render` and `jobs/status` all act on the **current** plate.
`arrange` is the toolbar's "Arrange all": it re-packs the objects of every unlocked
plate together and deletes plates left empty at the end, so it undoes a
one-part-per-plate layout (locked plates are left alone). The plate
routes let a client build a multi-plate project the way the toolbar does: every
mutation goes through the same `Plater` calls as a click, so undo, the sidebar plate
list, the background process and the preview follow along. `GET /status` reports
`plates: {count, current}` and lists `plates` in `capabilities`; `GET /objects` adds
a `plate` field per object (0-based, `-1` when it sits off every plate).

`GET /slice/status` follows the current plate. After a plate is selected, added
with `select`, deleted, or an object moves, it reports the current plate's own
result (`done` with its stats when that plate is still sliced, otherwise `idle`
with the message `plate N is not sliced`), and `POST /slice` on a plate that is
already sliced (`already_valid`) refreshes it the same way. Without that, the
status would keep describing whichever plate was sliced last.

### GET /plates

```bash
curl -H "X-Api-Token: $TOK" http://localhost:13130/api/v1/plates
```
```json
{
  "plates": [
    { "index": 0, "name": "", "current": true, "locked": false, "empty": false,
      "slice_result_valid": false,
      "origin": [0,0,0], "bbox_min": [0,0,0], "bbox_max": [256,256,250],
      "objects": [ { "id": 794, "instance": 0, "name": "box_base.stl" } ] },
    { "index": 1, "name": "", "current": false, "locked": false, "empty": true,
      "slice_result_valid": false,
      "origin": [281,0,0], "bbox_min": [281,0,0], "bbox_max": [537,256,250],
      "objects": [] }
  ],
  "count": 2, "current": 0, "max": 36, "can_add": true, "can_delete": true
}
```
Indices are 0-based and shift down when an earlier plate is deleted, exactly as in
the GUI (plate "2" on screen is index 1).

### POST /plates

```bash
curl -X POST -H "X-Api-Token: $TOK" -H "Content-Type: application/json" \
  -d '{"select": true}' http://localhost:13130/api/v1/plates
```
```json
{ "created": true, "index": 1, "count": 2, "current": 1, "plate": { "...": "as in GET /plates" } }
```
The body is optional; `select` defaults to `true` (the new plate becomes current,
as with the toolbar button). Errors: `422 max_plates` (the limit is reported in
`max`), `409 slicing`.

### POST /plates/{i}/select

```json
{ "selected": true, "current": 1, "plate": { "...": "as in GET /plates" } }
```
Errors: `404 unknown_plate`, `409 slicing`, `422 select_failed`.

### DELETE /plates/{i}

```json
{ "deleted": true, "index": 1, "count": 1, "current": 0, "objects_moved_off_plate": 2 }
```
The objects the plate held are **not** deleted: `Plater::delete_plate` parks them
beside the bed, as the toolbar does, and `objects_moved_off_plate` says how many.
Errors: `404 unknown_plate`, `422 last_plate` (the only plate cannot be deleted),
`409 slicing`.

### POST /objects/{id}/plate

```bash
curl -X POST -H "X-Api-Token: $TOK" -H "Content-Type: application/json" \
  -d '{"plate": 1}' http://localhost:13130/api/v1/objects/832/plate
```
```json
{ "moved": true, "id": 832, "instance": 0, "plate": 1, "from_plate": 0, "offset": [409,128,6] }
```
`instance` (default 0) picks the copy when the object has several. An object coming
from another plate keeps its position relative to that plate (front-left of plate 1
lands front-left of plate 2); one coming from off-plate goes to the target's centre.
Already on that plate: `200 {"moved": false, "already_on_plate": true}`. The plate
it left is marked unsliced, as a drag in the window does. If keeping the relative
position would put it outside the target (it straddled the old plate's edge), it is
centred instead. `POST /objects/{id}/transform` and `/duplicate` also re-check which
plate the object (or the new copy) sits on, so an object moved onto another plate by
coordinates is sliced with that plate. A plate's deleted objects are parked where the
next plate appears, so `POST /plates` straight after a delete picks them back up. Errors:
`404 unknown_object | unknown_plate | unknown_instance`, `422 plate_locked` (the
target or the plate it sits on is locked; `plate` names which) `| move_failed`,
`409 slicing`, `400 missing_plate`.

### Object transform semantics

`POST /objects/{id}/transform` takes `translate`, `rotate` and `scale` (each `[x,y,z]`, at least one) and an optional `instance` (default 0).

| field | meaning | units | limit |
|---|---|---|---|
| `translate` | RELATIVE, added to the instance offset (world frame) | mm | +-10000 per component |
| `rotate` | RELATIVE, added per component to the instance's stored Euler XYZ angles | degrees in | +-3600 per component |
| `scale` | ABSOLUTE per-axis factor (sending `[2,2,2]` twice leaves 2); negative refused, so no mirroring | factor | 0.0001 to 1000 |

- With more than one non-zero rotation axis, adding Euler angles is not the same as an extra rotation about the world axes, and angles are not normalised (they can pass 360 degrees).
- Order inside one request does not matter. Rotation and scale act about the instance origin (mesh origin), not the bounding-box centre.
- Afterwards the object is dropped to the bed if it was floating or touching (lowest point at or above -0.001 mm) or lay wholly below it; one sunk part-way stays sunk. Read `bbox_min[2]` from `GET /objects`.
- The response `transform` and `GET /objects` (`transform` for instance 0, every entry of `instances_detail`) report `offset` in mm, `rotation` in RADIANS and `rotation_deg` in degrees (same angles), `scale` as factors. `rotation` stays in radians for existing clients.
- Errors: `400 bad_transform` (a vector is not exactly 3 finite numbers, or out of range; `detail` says which), `400 no_transform`, `400 instance_must_be_integer`, `400 negative_index`, `404 unknown_object | no_instance | unknown_instance` (every failure to find the target is a 404).

## Notes & limits

- **Single-client oriented.** The server uses one I/O thread and marshals most
  calls onto the GUI thread with a 10s timeout (`504 ui_timeout`). Heavy or
  concurrent calls serialize; a stuck GUI stalls the API for up to 10s.
- **Token strength.** 32 hex chars (128 bits) from `std::random_device` — fine for
  a LAN token, not intended as a cryptographic secret store.
- **Survives GUI recreate.** Changing language/skin rebuilds the GUI; the server
  keeps running and slice-event subscriptions rebind onto the new window, so WS
  clients stay connected.
- **No SO_REUSEADDR pitfalls.** The listener sets `SO_REUSEADDR`, so restarting
  the app rebinds the port cleanly even if a prior socket is in `TIME_WAIT`.

### Part-level settings and modifiers

`GET /objects/{id}` returns `{id, index, name, config, volumes}`; each volume is
`{index, id, name, type, extruder, config}` with `type` one of `part`, `negative`,
`modifier`, `support_blocker`, `support_enforcer`.

```bash
curl -X POST -H "X-Api-Token: $TOK" -H "Content-Type: application/json" \
  -d '{"shape":"box","size":[20,20,10],"offset":[0,0,5],"config":{"sparse_infill_density":"40%"}}' \
  http://localhost:13130/api/v1/objects/832/modifiers
curl -X PUT -H "X-Api-Token: $TOK" -H "Content-Type: application/json" \
  -d '{"wall_loops":4}' http://localhost:13130/api/v1/objects/832/volumes/1/config
curl -X DELETE -H "X-Api-Token: $TOK" http://localhost:13130/api/v1/objects/832/volumes/1
```

- `PUT .../volumes/{v}/config` follows `PUT /objects/{id}/config` exactly: a forbidden key
  is `forbidden_key`, an unknown one `unknown_key`, a policy-gated one
  `blocked_by_remote_api_policy`, and nothing is applied if any key fails (`422`). Errors:
  `400 bad_volume_index`, `404 unknown_object | unknown_volume`. It takes an undo snapshot.
- `POST .../modifiers`: `shape` is `box` (`size` = [x,y,z]) or `cylinder` (`size` =
  [diameter,height]), in mm, each 0.1 to 1000. `offset` (mm, within +-10000) is relative to the
  centre of the object's bounding box along the bed axes. `type` defaults to `modifier`. A
  bad number gives `400 bad_modifier`; an invalid or forbidden `config` key gives `422` and adds
  nothing. The new volume is not selected and no tips dialog is shown.
- `DELETE .../volumes/{v}` removes modifiers, blockers, enforcers and negative volumes
  only: a part gives `422 cannot_delete_part`, a cut object's negative volume `422 cut_object`.
  When one volume is left the slicer folds its overrides into the object config, as the
  window does.
- Volume indices are re-sorted after an add or delete (parts first). Always re-read
  `GET /objects/{id}` before addressing a volume; `id` is the volume's stable id.
- Any print key is accepted, as for objects; some (for example `layer_height` on a modifier) have no effect.
