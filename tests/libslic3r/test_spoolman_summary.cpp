// Tests for the Spoolman sync summary model (SpoolmanSummary). Pure data, no GUI. Expected values are written by hand from the approved S8
// mockup (filament 16 "PETG HF Red"), NOT derived from the builder's constants.
// VERIFIED: card/notice/toast/header content for the four mockup states and the known-bad cases listed in SpoolmanSummary.hpp.
// NOT shown: how the dialog renders it (not built), translation, real sync data (the reports here are hand-built).
#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Spoolman/SpoolmanSummary.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

using namespace Slic3r;
using namespace Slic3r::Spoolman;

namespace {

PresetResult red_result()
{
    PresetResult p;
    p.spoolman_id = 16;
    p.preset_name = "Snapmaker PETG HF @U1 0.4 nozzle";
    p.parent_name = "Snapmaker PETG HF @U1 0.4 nozzle";
    p.parent_rule = "vendor + name + material";
    // the order the upsert gives (the mapping's own order), as in the mockup
    p.changes = {
        {"nozzle_temperature", "255", "250", "native:settings_extruder_temp"},
        {"nozzle_temperature_initial_layer", "255", "250", "native:settings_extruder_temp"},
        {"hot_plate_temp", "80", "70", "native:settings_bed_temp"},
        {"hot_plate_temp_initial_layer", "80", "70", "native:settings_bed_temp"},
        {"filament_density", "1.27", "1.3", "native:density"},
        {"default_filament_colour", "#E0E0E0", "#FF1523", "native:color_hex"},
    };
    return p;
}

// raw key 1 is shown as slot "2": the two differ on purpose
SummaryTray red_tray()
{
    SummaryTray t;
    t.key = 1;
    t.display_slot = "2";
    t.colour_hex = "#FF1523";
    t.spool_name = "Snapmaker PETG HF Red";
    t.remaining_weight_g = 612;
    t.spoolman_filament_id = 16;
    return t;
}

SummaryTray tray(int key, const std::string &disp, int id, const std::string &name = "n", std::optional<double> w = std::nullopt)
{
    SummaryTray t;
    t.key = key; t.display_slot = disp; t.spoolman_filament_id = id; t.spool_name = name; t.remaining_weight_g = w;
    return t;
}

SyncOutcome normal()
{
    SyncOutcome o;
    o.report.updated.push_back(red_result());
    o.trays.push_back(red_tray());
    o.catalog_size = 22;
    o.fetched_age_seconds = 0;
    return o;
}

PresetResult unchanged_result(int id, const std::string &name)
{
    PresetResult u;
    u.spoolman_id = id; u.preset_name = name;
    return u;
}

void check_red_card(const SlotCard &c)
{
    CHECK(c.raw_key == 1);
    CHECK(c.display_slot == "2");
    CHECK(c.on_tray);
    CHECK(c.spoolman_id == 16);
    CHECK(c.colour_hex == "#FF1523");
    CHECK(c.spool_name == "Snapmaker PETG HF Red");
    CHECK(c.remaining_weight_text == "612 g left");
    REQUIRE(c.preset_name.has_value());
    CHECK(*c.preset_name == "Snapmaker PETG HF @U1 0.4 nozzle");
    CHECK(c.rule_text == "matched by vendor + name + material");
    CHECK_FALSE(c.skipped);
    CHECK_FALSE(c.created);
    REQUIRE(c.changes.size() == 6);
    // the mockup's order: native fields in mapping order
    const char *exp[6][4] = {
        {"nozzle_temperature", "255", "250", "native:settings_extruder_temp"},
        {"nozzle_temperature_initial_layer", "255", "250", "native:settings_extruder_temp"},
        {"hot_plate_temp", "80", "70", "native:settings_bed_temp"},
        {"hot_plate_temp_initial_layer", "80", "70", "native:settings_bed_temp"},
        {"filament_density", "1.27", "1.3", "native:density"},
        {"default_filament_colour", "#E0E0E0", "#FF1523", "native:color_hex"},
    };
    for (int i = 0; i < 6; ++i) {
        CHECK(c.changes[i].key == exp[i][0]);
        CHECK(c.changes[i].preset_value == exp[i][1]);
        CHECK(c.changes[i].spoolman_value == exp[i][2]);
        CHECK(c.changes[i].source == exp[i][3]);
    }
    CHECK(c.ignored.empty());
    CHECK(c.flags.empty());
}

// every field of a model, so two models can be compared whole
std::string dump(const SummaryModel &m)
{
    std::ostringstream o;
    o << m.show_dialog << "|" << m.toast_text << "|" << m.catalog_size << "|" << m.fetched_text << "|" << m.warning_count << "\n";
    for (const SummaryNotice &n : m.notices) {
        o << "N" << int(n.kind) << n.text;
        for (const auto &r : n.rows) o << "[" << r.key << r.preset_value << r.spoolman_value << r.source << "]";
        o << "\n";
    }
    for (const SlotCard &c : m.slots) {
        o << "C" << c.raw_key << "/" << c.display_slot << "/" << c.on_tray << "/" << c.spoolman_id << "/" << c.spool_name << "/" << c.remaining_weight_text << "/"
          << (c.preset_name ? *c.preset_name : "-") << "/" << c.rule_text << "/" << c.skipped << c.skip_reason << "/" << c.created << c.parent_name << c.note
          << "/" << c.changes_preset << "\n";
        for (const auto &r : c.changes) o << " R" << r.key << r.preset_value << r.spoolman_value << r.source << "\n";
        for (const auto &i : c.ignored) o << " I" << i.key << "=" << i.value << i.reason << i.warning << "\n";
        for (const auto &l : c.linked_presets) o << " L" << l << "\n";
        for (const auto &f : c.flags) o << " F" << f << "\n";
    }
    return o.str();
}

} // namespace

TEST_CASE("Spoolman summary: normal sync (mockup state 1)", "[Spoolman]")
{
    SyncOutcome o = normal();
    // second card from the mockup: a rejected value, two changes; raw key 5 shown as slot "4"
    PresetResult b;
    b.spoolman_id = 21;
    b.preset_name = "Generic PETG @U1 0.4 nozzle";
    b.parent_rule = "material family with flags";
    b.changes = {{"filament_flow_ratio", "0.95", "0.98", "extra:flow_ratio"}, {"pressure_advance", "0.04", "0.05", "extra:pressure_advance"}};
    o.report.updated.push_back(b);
    RejectedValue rv;
    rv.spoolman_id = 21;
    rv.preset_name = b.preset_name;
    rv.key_or_field = "max_volumetric_speed";
    rv.value = "abc";
    rv.reason = "not a number";
    o.report.rejected.push_back(rv);
    o.trays.push_back(tray(5, "4", 21, "Overture PETG Blue", 880));
    o.trays.back().colour_hex = "#2B6FD6";
    // third card: nothing differs; raw key 0 shown as slot "3"
    PresetResult u = unchanged_result(30, "Generic TPU @U1 0.4 nozzle");
    u.parent_rule = "material alone";
    o.report.unchanged.push_back(u);
    o.trays.push_back(tray(0, "3", 30, "Sunlu TPU 95A", 310));
    o.trays.back().colour_hex = "#E8E8E8";
    // external spool, raw key 255
    o.trays.push_back(tray(255, "Ext", 0));

    SummaryModel m = build_summary(o);
    CHECK(m.show_dialog);
    CHECK(m.toast_text.empty());
    CHECK(m.notices.empty());
    CHECK(m.catalog_size == 22);
    CHECK(m.fetched_text == "fetched just now");
    CHECK(m.read_only_note == "Nothing is written to Spoolman. Edit a Spoolman-backed preset for this session only; \"Save as\" under a new name removes the link.");
    REQUIRE(m.slots.size() == 3); // the external tray has no Spoolman filament and no tier entry: no card
    CHECK(m.slots[0].display_slot == "2");
    CHECK(m.slots[1].display_slot == "3");
    CHECK(m.slots[2].display_slot == "4");
    CHECK(m.slots[1].raw_key == 0);
    CHECK(m.slots[2].raw_key == 5);
    check_red_card(m.slots[0]);

    const SlotCard &tpu = m.slots[1];
    CHECK(tpu.spool_name == "Sunlu TPU 95A");
    CHECK(tpu.colour_hex == "#E8E8E8");
    CHECK(tpu.remaining_weight_text == "310 g left");
    CHECK(*tpu.preset_name == "Generic TPU @U1 0.4 nozzle");
    CHECK(tpu.rule_text == "matched by material only (flags not checked)");
    CHECK(tpu.changes.empty());

    const SlotCard &blue = m.slots[2];
    CHECK(blue.rule_text == "matched by material family");
    CHECK(blue.remaining_weight_text == "880 g left");
    REQUIRE(blue.changes.size() == 2);
    CHECK(blue.changes[0].key == "filament_flow_ratio");
    CHECK(blue.changes[0].preset_value == "0.95");
    CHECK(blue.changes[0].spoolman_value == "0.98");
    CHECK(blue.changes[0].source == "extra:flow_ratio");
    CHECK(blue.changes[1].key == "pressure_advance");
    REQUIRE(blue.ignored.size() == 1);
    CHECK(blue.ignored[0].key == "max_volumetric_speed");
    CHECK(blue.ignored[0].value == "abc");
    CHECK(blue.ignored[0].reason == "not a number");
}

TEST_CASE("Spoolman summary: rows group by source class, keeping the given order inside", "[Spoolman]")
{
    SyncOutcome o = normal();
    o.report.updated[0].changes = {{"z_ofd", "1", "2", "ofd:slicer_settings_orcaslicer"}, {"b_extra", "1", "2", "extra:flow"}, {"parent_reset", "1", "2", "parent"},
                                   {"z_native", "1", "2", "native:density"},  {"a_extra", "1", "2", "extra:pa"},   {"a_native", "1", "2", "native:color_hex"}};
    SummaryModel m = build_summary(o);
    REQUIRE(m.slots[0].changes.size() == 6);
    const char *want[6] = {"z_native", "a_native", "b_extra", "a_extra", "z_ofd", "parent_reset"};
    for (int i = 0; i < 6; ++i)
        CHECK(m.slots[0].changes[i].key == want[i]);
}

TEST_CASE("Spoolman summary: Spoolman unreachable (mockup state 2)", "[Spoolman]")
{
    SyncOutcome o = normal();
    o.catalog_state = CatalogState::unreachable;
    o.has_previous_sync = true;
    o.last_good_age_seconds = 2 * 86400;
    SummaryModel m = build_summary(o);
    CHECK(m.show_dialog);
    CHECK(m.toast_text.empty());
    REQUIRE(m.notices.size() == 1);
    CHECK(m.notices[0].kind == NoticeKind::stale_spoolman);
    CHECK(m.notices[0].text == "Spoolman could not be reached. Showing the values from the last sync, 2 days ago. "
                               "Slots were matched by the printer's RFID type, the way Orca did before.");
    CHECK(m.fetched_text == "last good fetch 2 days ago");
    REQUIRE(m.slots.size() == 1);
    check_red_card(m.slots[0]);

    o.last_good_age_seconds.reset();
    CHECK(build_summary(o).notices[0].text == "Spoolman could not be reached. Showing the values from the last sync. "
                                              "Slots were matched by the printer's RFID type, the way Orca did before.");
    CHECK(build_summary(o).fetched_text == "last good fetch time unknown");
    o.last_good_age_seconds = 3600;
    CHECK(build_summary(o).notices[0].text == "Spoolman could not be reached. Showing the values from the last sync, 1 hour ago. "
                                              "Slots were matched by the printer's RFID type, the way Orca did before.");
}

TEST_CASE("Spoolman summary: unusable reply and no earlier sync say what is true", "[Spoolman]")
{
    SyncOutcome o;
    o.catalog_state = CatalogState::bad_response;
    o.has_previous_sync = false;
    SummaryModel m = build_summary(o);
    CHECK(m.show_dialog); // stale alone opens it
    REQUIRE(m.notices.size() == 1);
    CHECK(m.notices[0].text == "Spoolman sent an unusable reply. There are no earlier values to show. "
                               "Slots were matched by the printer's RFID type, the way Orca did before.");
    CHECK(m.notices[0].text.find("last sync") == std::string::npos);
    CHECK(m.fetched_text == "never fetched");

    o.catalog_state = CatalogState::unreachable;
    CHECK(build_summary(o).notices[0].text == "Spoolman could not be reached. There are no earlier values to show. "
                                              "Slots were matched by the printer's RFID type, the way Orca did before.");
    o.catalog_state = CatalogState::bad_response;
    o.has_previous_sync = true;
    o.last_good_age_seconds = 90;
    CHECK(build_summary(o).notices[0].text == "Spoolman sent an unusable reply. Showing the values from the last sync, 1 minute ago. "
                                              "Slots were matched by the printer's RFID type, the way Orca did before.");
}

TEST_CASE("Spoolman summary: fresh fetch header text", "[Spoolman]")
{
    SyncOutcome o;
    o.catalog_size = 3;
    CHECK(build_summary(o).fetched_text == "fetched at an unknown time"); // no age: never claim "just now"
    o.fetched_age_seconds = -4;
    CHECK(build_summary(o).fetched_text == "fetched at an unknown time");
    o.fetched_age_seconds = 0;
    CHECK(build_summary(o).fetched_text == "fetched just now");
    o.fetched_age_seconds = 5;
    CHECK(build_summary(o).fetched_text == "fetched just now");
    o.fetched_age_seconds = 6;
    CHECK(build_summary(o).fetched_text == "fetched 6 seconds ago");
    o.fetched_age_seconds = 7200;
    CHECK(build_summary(o).fetched_text == "fetched 2 hours ago");
}

TEST_CASE("Spoolman summary: slot with no preset match (mockup state 3)", "[Spoolman]")
{
    SyncOutcome o = normal();
    SkippedFilament s;
    s.spoolman_id = 40;
    s.filament_name = "YOUSU Glow PLA Purple";
    s.reason = "no preset with the Glow flag exists";
    s.linked_presets = {"YOUSU Glow PLA Purple (Spoolman) @old", "A kept preset"};
    o.report.skipped.push_back(s);
    o.trays.push_back(tray(0, "1", 40, "YOUSU Glow PLA Purple", 450));
    o.trays.back().colour_hex = "#7A3BA6";

    SummaryModel m = build_summary(o);
    CHECK(m.show_dialog);
    REQUIRE(m.slots.size() == 2);
    const SlotCard &c = m.slots[0];
    CHECK(c.display_slot == "1");
    CHECK(c.colour_hex == "#7A3BA6");
    CHECK(c.spool_name == "YOUSU Glow PLA Purple");
    CHECK(c.remaining_weight_text == "450 g left");
    CHECK_FALSE(c.preset_name.has_value());
    CHECK(c.skipped);
    CHECK(c.skip_reason == "no preset with the Glow flag exists");
    CHECK(c.changes.empty());
    REQUIRE(c.linked_presets.size() == 2);
    CHECK(c.linked_presets[0] == "A kept preset");
    check_red_card(m.slots[1]);
}

TEST_CASE("Spoolman summary: older project copy replaced (mockup state 4)", "[Spoolman]")
{
    SyncOutcome o = normal();
    ProjectCopyReplaced p;
    p.preset_name = "Snapmaker PETG HF @U1 0.4 nozzle";
    p.changes = {{"nozzle_temperature", "240", "250", "native:settings_extruder_temp"}, {"filament_density", "1.25", "1.3", "native:density"}};
    o.project_copies.push_back(p);
    SummaryModel m = build_summary(o);
    CHECK(m.show_dialog);
    REQUIRE(m.notices.size() == 1);
    CHECK(m.notices[0].kind == NoticeKind::project_copy_replaced);
    CHECK(m.notices[0].text ==
          "This project had an older copy of \"Snapmaker PETG HF @U1 0.4 nozzle\". Spoolman is the source for this filament, so the project "
          "now uses the current library preset. The changed values are listed below.");
    REQUIRE(m.notices[0].rows.size() == 2);
    CHECK(m.notices[0].rows[0].key == "nozzle_temperature");
    CHECK(m.notices[0].rows[0].preset_value == "240");
    CHECK(m.notices[0].rows[0].spoolman_value == "250");
    CHECK(m.notices[0].rows[1].key == "filament_density");
    check_red_card(m.slots[0]);

    // no changed values: the text must not point at a list that is not there
    o.project_copies[0].changes.clear();
    m = build_summary(o);
    REQUIRE(m.notices.size() == 1);
    CHECK(m.notices[0].text.find("listed below") == std::string::npos);
    CHECK(m.notices[0].text.find("current library preset.") != std::string::npos);
    // no project copies at all: no notice
    o.project_copies.clear();
    CHECK(build_summary(o).notices.empty());
}

TEST_CASE("Spoolman summary: notices are ordered stale, project copy, info", "[Spoolman]")
{
    SyncOutcome o;
    o.catalog_size = 1;
    o.catalog_state = CatalogState::unreachable;
    o.project_copies.push_back({"B preset", {}});
    o.project_copies.push_back({"A preset", {}});
    o.report.needs_reselect.push_back("unknown preset");
    SummaryModel m = build_summary(o);
    REQUIRE(m.notices.size() == 4);
    CHECK(m.notices[0].kind == NoticeKind::stale_spoolman);
    CHECK(m.notices[1].kind == NoticeKind::project_copy_replaced);
    CHECK(m.notices[1].text.find("\"A preset\"") != std::string::npos);
    CHECK(m.notices[2].text.find("\"B preset\"") != std::string::npos);
    CHECK(m.notices[3].kind == NoticeKind::info);
    CHECK(m.notices[3].text == "Needs re-select: \"unknown preset\" was refreshed while selected; select it again to see the new values.");
}

TEST_CASE("Spoolman summary: nothing changed gives a toast, rejected values still open the dialog", "[Spoolman]")
{
    SyncOutcome o;
    o.catalog_size = 5;
    o.report.unchanged.push_back(unchanged_result(16, "A"));
    o.report.unchanged.push_back(unchanged_result(17, "B"));
    o.trays.push_back(tray(0, "1", 16, "a"));
    SummaryModel m = build_summary(o);
    CHECK_FALSE(m.show_dialog);
    CHECK(m.toast_text == "Spoolman: 2 filaments in sync, nothing changed");
    CHECK(m.slots.size() == 1); // the caller just does not show the dialog

    SyncOutcome one;
    one.catalog_size = 1;
    one.report.unchanged.push_back(unchanged_result(18, "C"));
    CHECK(build_summary(one).toast_text == "Spoolman: 1 filament in sync, nothing changed");

    RejectedValue rv;
    rv.spoolman_id = 16; rv.key_or_field = "nozzle_temperature"; rv.value = "450"; rv.reason = "exceeds printer limit";
    o.report.rejected.push_back(rv);
    m = build_summary(o);
    CHECK(m.show_dialog);
    CHECK(m.toast_text.empty());
    REQUIRE(m.slots[0].ignored.size() == 1);
    CHECK(m.slots[0].ignored[0].key == "nozzle_temperature");
    CHECK(m.slots[0].ignored[0].value == "450");
    CHECK(m.slots[0].ignored[0].reason == "exceeds printer limit");
    CHECK_FALSE(m.slots[0].ignored[0].warning);
}

TEST_CASE("Spoolman summary: warning-only rejections do not open the dialog but are kept and counted", "[Spoolman]")
{
    SyncOutcome o;
    o.catalog_size = 5;
    o.report.unchanged.push_back(unchanged_result(16, "A"));
    o.report.unchanged.push_back(unchanged_result(17, "B"));
    o.trays.push_back(tray(0, "1", 16, "a"));
    RejectedValue w;
    w.spoolman_id = 16; w.preset_name = "A"; w.key_or_field = "nozzle_temperature";
    w.reason = "parent's own value 350 exceeds printer limit (kept)"; w.warning = true;
    o.report.rejected.push_back(w);
    w.spoolman_id = 17; w.key_or_field = "min_bed_temperature"; w.reason = "informational only, never applied";
    o.report.rejected.push_back(w);
    SummaryModel m = build_summary(o);
    CHECK_FALSE(m.show_dialog);
    // the warning on id 17 (no tray, nothing else notable) is on no visible card: it is not counted
    CHECK(m.warning_count == 1);
    CHECK(m.toast_text == "Spoolman: 2 filaments in sync, 1 note");
    CHECK(m.slots.size() == 1);
    REQUIRE(m.slots.size() == 1);
    REQUIRE(m.slots[0].ignored.size() == 1);
    CHECK(m.slots[0].ignored[0].warning);
    CHECK(m.slots[0].ignored[0].reason == "parent's own value 350 exceeds printer limit (kept)");

    o.report.rejected.pop_back();
    CHECK(build_summary(o).toast_text == "Spoolman: 2 filaments in sync, 1 note");
    // a second tray makes the second warning visible
    o.report.rejected.push_back(w);
    o.trays.push_back(tray(1, "2", 17, "b"));
    m = build_summary(o);
    CHECK(m.warning_count == 2);
    CHECK(m.toast_text == "Spoolman: 2 filaments in sync, 2 notes");
    o.trays.pop_back();
    o.report.rejected.pop_back();
    // a real rejection next to the warnings opens it
    RejectedValue real;
    real.spoolman_id = 17; real.key_or_field = "x"; real.reason = "bad";
    o.report.rejected.push_back(real);
    CHECK(build_summary(o).show_dialog);
}

TEST_CASE("Spoolman summary: empty input and an empty catalog give no in-sync toast", "[Spoolman]")
{
    SummaryModel m = build_summary(SyncOutcome{});
    CHECK(m.slots.empty());
    CHECK(m.notices.empty());
    CHECK_FALSE(m.show_dialog);
    CHECK(m.toast_text.empty());
    CHECK(m.catalog_size == 0);

    SyncOutcome o; // catalog_size 0 with a (pathological) unchanged list: still nothing to be "in sync" with
    o.report.unchanged.push_back(unchanged_result(1, "A"));
    m = build_summary(o);
    CHECK_FALSE(m.show_dialog);
    CHECK(m.toast_text.empty());
}

TEST_CASE("Spoolman summary: skipped filament without a tray is still a card", "[Spoolman]")
{
    SyncOutcome o;
    o.catalog_size = 3;
    SkippedFilament s;
    s.spoolman_id = 9; s.filament_name = "Lonely"; s.reason = "no parent";
    o.report.skipped.push_back(s);
    SummaryModel m = build_summary(o);
    CHECK(m.show_dialog);
    REQUIRE(m.slots.size() == 1);
    CHECK_FALSE(m.slots[0].on_tray);
    CHECK(m.slots[0].raw_key == -1);
    CHECK(m.slots[0].display_slot.empty());
    CHECK(m.slots[0].skipped);
    CHECK(m.slots[0].spool_name == "Lonely");
    CHECK(m.slots[0].skip_reason == "no parent");
}

TEST_CASE("Spoolman summary: each flag alone opens the dialog", "[Spoolman]")
{
    auto base = [] {
        SyncOutcome o;
        o.catalog_size = 3;
        o.report.unchanged.push_back(unchanged_result(1, "U1"));
        o.trays.push_back(tray(0, "1", 1, "t"));
        return o;
    };
    CHECK_FALSE(build_summary(base()).show_dialog);
    {   SyncOutcome o = base(); SupersededPreset s; s.spoolman_id = 1; s.preset_name = "U1"; o.report.superseded.push_back(s);
        CHECK(build_summary(o).show_dialog); }
    {   SyncOutcome o = base(); DuplicatePreset d; d.spoolman_id = 1; d.preset_name = "U1 copy"; o.report.duplicates.push_back(d);
        CHECK(build_summary(o).show_dialog); }
    {   SyncOutcome o = base(); o.report.needs_reselect.push_back("U1");
        CHECK(build_summary(o).show_dialog); }
    {   SyncOutcome o = base(); OrphanedPreset p; p.spoolman_id = 1; p.preset_name = "U1"; o.report.orphaned.push_back(p);
        CHECK(build_summary(o).show_dialog); }
    {   SyncOutcome o = base(); AmbiguousFilament a; a.spoolman_id = 1; a.candidates = {"x"}; o.report.ambiguous.push_back(a);
        CHECK(build_summary(o).show_dialog); }
}

TEST_CASE("Spoolman summary: flags land on the right card (ids differ from slots) with their text", "[Spoolman]")
{
    SyncOutcome o;
    o.catalog_size = 9;
    // id 10..14 on raw keys 4,3,2,1,0 shown as slots 5..1
    for (int i = 0; i < 5; ++i) {
        const int id = 10 + i;
        o.report.unchanged.push_back(unchanged_result(id, "U" + std::to_string(id)));
        o.trays.push_back(tray(4 - i, std::to_string(5 - i), id));
    }
    AmbiguousFilament a; a.spoolman_id = 10; a.filament_name = "f"; a.candidates = {"Z copy", "A copy"};
    o.report.ambiguous.push_back(a);
    SupersededPreset s; s.spoolman_id = 11; s.preset_name = "U11"; s.old_parent = "Old @x"; s.new_parent = "New @x";
    o.report.superseded.push_back(s);
    DuplicatePreset d; d.spoolman_id = 12; d.preset_name = "U12 copy"; d.refreshed_preset = "U12";
    o.report.duplicates.push_back(d);
    o.report.needs_reselect.push_back("U13");
    OrphanedPreset op; op.spoolman_id = 14; op.preset_name = "U14";
    o.report.orphaned.push_back(op);

    SummaryModel m = build_summary(o);
    CHECK(m.show_dialog);
    REQUIRE(m.slots.size() == 5);
    // cards run in display-slot order 1..5 = ids 14,13,12,11,10
    REQUIRE(m.slots[0].flags.size() == 1);
    CHECK(m.slots[0].spoolman_id == 14);
    CHECK(m.slots[0].flags[0] == "Orphaned: \"U14\" is linked to a filament that is no longer in Spoolman; kept, not refreshed.");
    CHECK(m.slots[1].spoolman_id == 13);
    REQUIRE(m.slots[1].flags.size() == 1);
    CHECK(m.slots[1].flags[0] == "Needs re-select: \"U13\" was refreshed while selected; select it again to see the new values.");
    CHECK(m.slots[2].spoolman_id == 12);
    REQUIRE(m.slots[2].flags.size() == 1);
    CHECK(m.slots[2].flags[0] == "Duplicate: \"U12 copy\" carries the same Spoolman link; \"U12\" was refreshed instead.");
    CHECK(m.slots[3].spoolman_id == 11);
    REQUIRE(m.slots[3].flags.size() == 1);
    CHECK(m.slots[3].flags[0] == "Superseded: the preset \"U11\" is linked under another parent (\"Old @x\", now \"New @x\"); kept unchanged.");
    CHECK(m.slots[4].spoolman_id == 10);
    REQUIRE(m.slots[4].flags.size() == 1);
    CHECK(m.slots[4].flags[0] == "Ambiguous: several presets match this filament (A copy, Z copy); nothing was changed.");
}

TEST_CASE("Spoolman summary: S6 tier entries, matched by RAW key", "[Spoolman]")
{
    SyncOutcome o = normal(); // red tray: raw key 1, display "2", id 16
    SpoolmanTierReport::Entry e;
    e.slot = 1; e.rule = "spoolman_id"; e.preset = "Snapmaker PETG HF @U1 0.4 nozzle";
    o.tiers.entries.push_back(e);
    SummaryModel m = build_summary(o);
    REQUIRE(m.slots.size() == 1);
    CHECK(*m.slots[0].preset_name == "Snapmaker PETG HF @U1 0.4 nozzle");
    CHECK(m.slots[0].rule_text == "linked Spoolman filament id");
    CHECK(m.slots[0].flags.empty());
    CHECK(m.slots[0].changes.size() == 6);

    // an entry keyed by the DISPLAY number (2) matches no tray: it is a separate card, not silently joined
    o.tiers.entries[0].slot = 2;
    m = build_summary(o);
    CHECK(m.show_dialog);
    REQUIRE(m.slots.size() == 2);
    CHECK(m.slots[0].on_tray);
    CHECK(m.slots[0].rule_text == "matched by vendor + name + material");
    CHECK_FALSE(m.slots[1].on_tray);
    CHECK(m.slots[1].raw_key == 2);

    // external-spool style key
    SyncOutcome x;
    x.catalog_size = 1;
    x.trays.push_back(tray(255, "Ext", 0, "spool"));
    SpoolmanTierReport::Entry ee;
    ee.slot = 255; ee.rule = "external_id"; ee.preset = "P";
    x.tiers.entries.push_back(ee);
    m = build_summary(x);
    REQUIRE(m.slots.size() == 1);
    CHECK(m.slots[0].display_slot == "Ext");
    CHECK(m.slots[0].rule_text == "matched by Spoolman external id");
    CHECK(*m.slots[0].preset_name == "P");
}

TEST_CASE("Spoolman summary: a tier entry with no tray is never dropped", "[Spoolman]")
{
    SyncOutcome q;
    q.catalog_size = 3;
    SpoolmanTierReport::Entry amb;
    amb.slot = 7; amb.rule = "ambiguous"; amb.candidates = {"B", "A"};
    q.tiers.entries.push_back(amb);
    SummaryModel m = build_summary(q);
    CHECK(m.show_dialog);
    REQUIRE(m.slots.size() == 1);
    CHECK_FALSE(m.slots[0].on_tray);
    CHECK(m.slots[0].raw_key == 7);
    CHECK_FALSE(m.slots[0].spool_name.empty());
    REQUIRE(m.slots[0].flags.size() == 1);
    CHECK(m.slots[0].flags[0] == "Ambiguous: several presets match this filament (A, B); nothing was changed.");

    SyncOutcome n;
    n.catalog_size = 3;
    SpoolmanTierReport::Entry ok;
    ok.slot = 8; ok.rule = "setting_id"; ok.preset = "Chosen";
    n.tiers.entries.push_back(ok);
    m = build_summary(n);
    CHECK(m.show_dialog);
    REQUIRE(m.slots.size() == 1);
    CHECK(*m.slots[0].preset_name == "Chosen");
    CHECK(m.slots[0].rule_text == "matched by setting_id");
}

TEST_CASE("Spoolman summary: a tier preset that is not the refreshed one is flagged, both names shown", "[Spoolman]")
{
    SyncOutcome o = normal();
    SpoolmanTierReport::Entry e;
    e.slot = 1; e.rule = "setting_id"; e.preset = "Other preset";
    o.tiers.entries.push_back(e);
    SummaryModel m = build_summary(o);
    CHECK(m.show_dialog);
    REQUIRE(m.slots.size() == 1);
    CHECK(*m.slots[0].preset_name == "Other preset");
    CHECK(m.slots[0].changes_preset == "Snapmaker PETG HF @U1 0.4 nozzle");
    REQUIRE(m.slots[0].flags.size() == 1);
    CHECK(m.slots[0].flags[0] == "Mismatch: this slot uses \"Other preset\" but the Spoolman preset \"Snapmaker PETG HF @U1 0.4 nozzle\" was refreshed.");
}

TEST_CASE("Spoolman summary: a tier mismatch alone is a note, not a reason to open the dialog", "[Spoolman]")
{
    SyncOutcome o;
    o.catalog_size = 2;
    o.report.unchanged.push_back(unchanged_result(1, "Refreshed"));
    o.trays.push_back(tray(0, "1", 1, "t"));
    SpoolmanTierReport::Entry e;
    e.slot = 0; e.rule = "setting_id"; e.preset = "Other";
    o.tiers.entries.push_back(e);
    SummaryModel m = build_summary(o);
    CHECK_FALSE(m.show_dialog);
    CHECK(m.warning_count == 1);
    CHECK(m.toast_text == "Spoolman: 1 filament in sync, 1 note");
    REQUIRE(m.slots.size() == 1);
    REQUIRE(m.slots[0].flags.size() == 1);
    CHECK(m.slots[0].flags[0].rfind("Mismatch:", 0) == 0);
    // together with another notable item the dialog opens
    o.report.skipped.push_back({2, "x", "no parent", {}});
    CHECK(build_summary(o).show_dialog);
}

TEST_CASE("Spoolman summary: tier entries sharing a raw key all stay visible", "[Spoolman]")
{
    auto make = [](bool rev) {
        SyncOutcome o;
        o.catalog_size = 2;
        o.report.unchanged.push_back(unchanged_result(1, "P"));
        o.trays.push_back(tray(3, "1", 1, "t"));
        SpoolmanTierReport::Entry a, b;
        a.slot = 3; a.rule = "external_id"; a.preset = "P";
        b.slot = 3; b.rule = "spoolman_id"; b.preset = "P";
        o.tiers.entries = {a, b};
        if (rev)
            std::reverse(o.tiers.entries.begin(), o.tiers.entries.end());
        return o;
    };
    SummaryModel m = build_summary(make(false));
    CHECK(m.show_dialog);
    REQUIRE(m.slots.size() == 2);
    CHECK(m.slots[0].on_tray);
    CHECK(m.slots[0].rule_text == "matched by Spoolman external id"); // sorted by rule: external_id first
    CHECK_FALSE(m.slots[1].on_tray);
    CHECK(m.slots[1].raw_key == 3);
    CHECK(m.slots[1].rule_text == "linked Spoolman filament id");
    REQUIRE(m.slots[1].flags.size() == 1);
    CHECK(m.slots[1].flags[0] == "Duplicate tier entry: another entry for slot key 3 was found; the first one is shown with the slot.");
    CHECK(dump(m) == dump(build_summary(make(true)))); // which one is first never depends on input order
}

TEST_CASE("Spoolman summary: slot labels are never blank and never shared", "[Spoolman]")
{
    SyncOutcome o;
    o.catalog_size = 3;
    o.report.unchanged = {unchanged_result(1, "A"), unchanged_result(2, "B"), unchanged_result(3, "C")};
    o.trays.push_back(tray(255, "Ext", 1, "e2"));
    o.trays.push_back(tray(254, "Ext", 2, "e1"));
    o.trays.push_back(tray(7, "", 3, "blank"));
    SummaryModel m = build_summary(o);
    REQUIRE(m.slots.size() == 3);
    CHECK(m.slots[0].display_slot == "Ext 1"); // labels sort by text
    CHECK(m.slots[0].raw_key == 254);
    CHECK(m.slots[1].display_slot == "Ext 2");
    CHECK(m.slots[1].raw_key == 255);
    CHECK(m.slots[2].display_slot == "Slot 7");
    CHECK(m.slots[2].raw_key == 7);
}

TEST_CASE("Spoolman summary: an update that only rewrote the link is not notable; a catalog nobody matched says so", "[Spoolman]")
{
    SyncOutcome o;
    o.catalog_size = 4;
    PresetResult p = unchanged_result(1, "P"); // changes empty: only the link / filament_settings_id was rewritten
    o.report.updated.push_back(p);
    SummaryModel m = build_summary(o);
    CHECK_FALSE(m.show_dialog);
    CHECK(m.toast_text == "Spoolman: 1 filament in sync, nothing changed");

    SyncOutcome n;
    n.catalog_size = 4;
    m = build_summary(n);
    CHECK_FALSE(m.show_dialog);
    CHECK(m.toast_text == "Spoolman: 4 filaments in the catalog, none matched a preset");
    CHECK(m.toast_text.find("in sync") == std::string::npos);
}

TEST_CASE("Spoolman summary: needs_reselect reaches every id that reported the preset name", "[Spoolman]")
{
    SyncOutcome o;
    o.catalog_size = 2;
    o.report.unchanged = {unchanged_result(2, "Shared"), unchanged_result(1, "Shared")};
    o.trays.push_back(tray(0, "1", 1, "a"));
    o.trays.push_back(tray(1, "2", 2, "b"));
    o.report.needs_reselect.push_back("Shared");
    SummaryModel m = build_summary(o);
    CHECK(m.show_dialog);
    REQUIRE(m.slots.size() == 2);
    CHECK(m.slots[0].flags.size() == 1);
    CHECK(m.slots[1].flags.size() == 1);
    CHECK(m.slots[0].flags[0] == m.slots[1].flags[0]);
}

TEST_CASE("Spoolman summary: created presets say what the preset value column is", "[Spoolman]")
{
    SyncOutcome o;
    o.catalog_size = 1;
    PresetResult p = red_result();
    p.parent_name = "Snapmaker PETG HF @U1 0.4 nozzle";
    p.preset_name = "Snapmaker PETG HF Red (Spoolman) @U1 0.4 nozzle";
    o.report.created.push_back(p);
    o.trays.push_back(red_tray());
    SummaryModel m = build_summary(o);
    CHECK(m.show_dialog);
    REQUIRE(m.slots.size() == 1);
    CHECK(m.slots[0].created);
    CHECK(m.slots[0].parent_name == "Snapmaker PETG HF @U1 0.4 nozzle");
    CHECK(m.slots[0].note == "Created from Snapmaker PETG HF @U1 0.4 nozzle; the preset value column shows Snapmaker PETG HF @U1 0.4 nozzle's value.");

    o.report.created[0].changes.clear();
    m = build_summary(o);
    CHECK(m.show_dialog); // a new preset is worth a line
    CHECK(m.slots[0].note == "Created, no values differ from Snapmaker PETG HF @U1 0.4 nozzle.");
}

TEST_CASE("Spoolman summary: results are kept per preset name, toast counts distinct ids", "[Spoolman]")
{
    SyncOutcome o;
    o.catalog_size = 4;
    PresetResult a;
    a.spoolman_id = 7; a.preset_name = "Preset A"; a.parent_rule = "material alone"; a.parent_name = "PA";
    a.changes = {{"k1", "1", "2", "native:density"}};
    PresetResult b;
    b.spoolman_id = 7; b.preset_name = "Preset B"; b.parent_rule = "vendor + name + material"; b.parent_name = "PB";
    b.changes = {{"k2", "3", "4", "native:density"}};
    o.report.updated = {b, a};
    o.trays.push_back(tray(0, "1", 7, "x"));
    SummaryModel m = build_summary(o);
    REQUIRE(m.slots.size() == 1);
    // the first preset by name owns preset/rule; neither overwrites the other; both sets of rows are there
    CHECK(*m.slots[0].preset_name == "Preset A");
    CHECK(m.slots[0].rule_text == "matched by material only (flags not checked)");
    CHECK(m.slots[0].parent_name == "PA");
    CHECK(m.slots[0].changes_preset == "Preset A, Preset B");
    REQUIRE(m.slots[0].changes.size() == 2);
    CHECK(m.slots[0].changes[0].key == "k1");
    CHECK(m.slots[0].changes[1].key == "k2");

    // two results of one id count once
    SyncOutcome u;
    u.catalog_size = 4;
    u.report.unchanged = {unchanged_result(7, "Preset A"), unchanged_result(7, "Preset B"), unchanged_result(8, "C")};
    CHECK(build_summary(u).toast_text == "Spoolman: 2 filaments in sync, nothing changed");
}

TEST_CASE("Spoolman summary: output is identical when every list arrives shuffled", "[Spoolman]")
{
    auto make = [](bool rev) {
        SyncOutcome o;
        o.catalog_size = 9;
        auto fix = [&](auto &v) { if (rev) std::reverse(v.begin(), v.end()); };
        for (int id : {5, 3, 8, 3}) { // id 3 twice: two presets of one id
            PresetResult p;
            p.spoolman_id = id;
            p.preset_name = "P" + std::to_string(id) + (id == 3 && o.report.updated.size() == 3 ? "b" : "");
            p.parent_rule = "material alone";
            p.changes = {{"a", "1", "2", "native:x"}, {"b", "1", "2", "extra:y"}};
            o.report.updated.push_back(p);
        }
        for (int i = 0; i < 4; ++i)
            o.report.rejected.push_back({3, "P3", "key" + std::to_string(i), "reason" + std::to_string(i), i % 2 == 0, "v" + std::to_string(i)});
        AmbiguousFilament a; a.spoolman_id = 8; a.filament_name = "f"; a.candidates = {"c", "a", "b"};
        o.report.ambiguous.push_back(a);
        o.report.superseded.push_back({3, "P3", "o", "n"});
        o.report.superseded.push_back({3, "P3b", "o2", "n2"});
        o.report.duplicates.push_back({5, "d1", "P5"});
        o.report.duplicates.push_back({5, "d2", "P5"});
        o.report.orphaned.push_back({5, "or1"});
        o.report.orphaned.push_back({5, "or2"});
        o.report.needs_reselect = {"P5", "P8"};
        for (int k : {0, 1, 2})
            o.trays.push_back(tray(k, std::to_string(k + 1), k == 0 ? 5 : k == 1 ? 3 : 8, "t" + std::to_string(k), 10.5));
        o.project_copies = {{"A", {{"k", "1", "2", "native:x"}}}, {"B", {}}};
        fix(o.report.updated); fix(o.report.rejected); fix(o.report.superseded); fix(o.report.duplicates);
        fix(o.report.orphaned); fix(o.report.needs_reselect); fix(o.trays); fix(o.project_copies);
        std::reverse(o.report.ambiguous[0].candidates.begin(), o.report.ambiguous[0].candidates.end());
        return o;
    };
    // the per-preset change order is the upsert's and is NOT shuffled; everything else is
    const std::string a = dump(build_summary(make(false))), b = dump(build_summary(make(true)));
    CHECK(a == b);
    CHECK(a.find("Cslot") == std::string::npos); // dump sanity: it does contain cards
    CHECK(a.find("C0/1/") != std::string::npos);
}

TEST_CASE("Spoolman summary: a rejected value is capped, escaped and keeps its length; the reason stays whole", "[Spoolman]")
{
    SyncOutcome o = normal();
    RejectedValue rv;
    rv.spoolman_id = 16;
    rv.key_or_field = "filament_notes";
    rv.reason = std::string(5000, 'x') + " end \"quoted\" 1.2300";
    auto shown = [&](const std::string &value) {
        o.report.rejected.clear();
        rv.value = value;
        o.report.rejected.push_back(rv);
        SummaryModel m = build_summary(o);
        REQUIRE(m.slots[0].ignored.size() == 1);
        return m.slots[0].ignored[0];
    };
    // short: untouched
    SummaryIgnored i = shown("abc");
    CHECK(i.value == "abc");
    CHECK(i.original_length == 3);
    CHECK_FALSE(i.truncated);
    CHECK(i.reason == rv.reason); // reasons are our own text and are never cut
    // exactly at the limit: untouched
    i = shown(std::string(120, 'y'));
    CHECK(i.value == std::string(120, 'y'));
    CHECK_FALSE(i.truncated);
    // long: 120 bytes + the ellipsis
    i = shown(std::string(5000, 'y'));
    CHECK(i.value == std::string(120, 'y') + "\xE2\x80\xA6");
    CHECK(i.original_length == 5000);
    CHECK(i.truncated);
    // a 2-byte character straddling the limit is dropped whole, never split
    i = shown(std::string(119, 'y') + "\xC3\xA9" + "zzz");
    CHECK(i.value == std::string(119, 'y') + "\xE2\x80\xA6");
    CHECK(i.original_length == 119 + 2 + 3);
    // a 4-byte character that still fits is kept
    i = shown(std::string(116, 'y') + "\xF0\x9F\x98\x80" + "zzz");
    CHECK(i.value == std::string(116, 'y') + "\xF0\x9F\x98\x80" + "\xE2\x80\xA6");
    // control characters are shown as \xNN, NUL and ESC included; DEL too
    i = shown(std::string("a\0b\x1b[31m\n\x7f", 10));
    CHECK(i.value == "a\\x00b\\x1b[31m\\x0a\\x7f");
    CHECK(i.original_length == 10);
    CHECK_FALSE(i.truncated);
    // the escape is one unit: never cut in the middle of "\x1b"
    i = shown(std::string(118, 'y') + "\x1b");
    CHECK(i.value == std::string(118, 'y') + "\xE2\x80\xA6");
    // invalid UTF-8 is escaped, not passed on
    i = shown(std::string("\xFF\xC3"));
    CHECK(i.value == "\\xff\\xc3");
}

TEST_CASE("Spoolman summary: UTF-8 names and odd weights", "[Spoolman]")
{
    SyncOutcome o = normal();
    o.trays[0].spool_name = "PolyLite\xE2\x84\xA2 ABS Gr\xC3\xBCn";
    CHECK(build_summary(o).slots[0].spool_name == "PolyLite\xE2\x84\xA2 ABS Gr\xC3\xBCn");

    o.trays[0].remaining_weight_g = 612.5;
    CHECK(build_summary(o).slots[0].remaining_weight_text == "612.5 g left");
    o.trays[0].remaining_weight_g = 0;
    CHECK(build_summary(o).slots[0].remaining_weight_text == "0 g left");
    o.trays[0].remaining_weight_g = -3;
    CHECK(build_summary(o).slots[0].remaining_weight_text == "weight unknown");
    o.trays[0].remaining_weight_g = std::nan("");
    CHECK(build_summary(o).slots[0].remaining_weight_text == "weight unknown");
    o.trays[0].remaining_weight_g.reset();
    CHECK(build_summary(o).slots[0].remaining_weight_text.empty());
}

TEST_CASE("Spoolman summary: cards without tray information are never nameless", "[Spoolman]")
{
    SyncOutcome o;
    o.catalog_size = 3;
    o.report.orphaned.push_back({1, "Orphan preset"});
    o.report.superseded.push_back({2, "Old preset", "a", "b"});
    RejectedValue rv;
    rv.spoolman_id = 3; rv.preset_name = "Rejected preset"; rv.key_or_field = "k"; rv.reason = "r";
    o.report.rejected.push_back(rv);
    o.report.rejected.push_back({4, "", "k", "r", false, ""});
    SummaryModel m = build_summary(o);
    REQUIRE(m.slots.size() == 4);
    for (const SlotCard &c : m.slots)
        CHECK_FALSE(c.spool_name.empty());
    CHECK(m.slots[0].spool_name == "Orphan preset");
    CHECK(m.slots[1].spool_name == "Old preset");
    CHECK(m.slots[2].spool_name == "Rejected preset");
    CHECK(m.slots[3].spool_name == "Spoolman filament 4");
}

TEST_CASE("Spoolman summary lines: normal sync renders header, preset, table, footer in order", "[Spoolman]")
{
    SyncOutcome o = normal();
    o.catalog_size = 20;
    o.fetched_age_seconds = 0;
    std::vector<SummaryLine> lines = summary_lines(build_summary(o));
    using K = SummaryLine::Kind;
    REQUIRE(lines.size() >= 5);
    CHECK(lines.front().kind == K::slot_header);
    CHECK(lines.front().cells == std::vector<std::string>{"Slot 2", "Snapmaker PETG HF Red", "612 g left", "#FF1523"});
    CHECK(lines[1].kind == K::slot_preset);
    CHECK(lines[1].cells[0] == "Preset: Snapmaker PETG HF @U1 0.4 nozzle (matched by vendor + name + material)");
    size_t head = 0, rows = 0;
    for (const SummaryLine &l : lines) {
        head += l.kind == K::table_head;
        if (l.kind == K::table_row) {
            ++rows;
            CHECK(l.cells.size() == 4);
        }
    }
    CHECK(head == 1);
    CHECK(rows == 6);
    CHECK(lines.back().kind == K::footer);
    CHECK(lines.back().cells[0].find("Nothing is written to Spoolman") == 0);
}

TEST_CASE("Spoolman summary lines: control characters from Spoolman cannot add lines", "[Spoolman]")
{
    CHECK(sanitize_for_display(std::string("a\nb\tc\x1b[31md\x7f") + "e") == "a b c [31md e");
    CHECK(sanitize_for_display("caf\xc3\xa9 \xe2\x80\xa6") == "caf\xc3\xa9 \xe2\x80\xa6"); // UTF-8 kept

    SyncOutcome o = normal();
    o.trays[0].spool_name = "Evil\nSlot 9\r\nName";
    std::vector<SummaryLine> lines = summary_lines(build_summary(o));
    REQUIRE_FALSE(lines.empty());
    CHECK(lines.front().cells[1] == "Evil Slot 9  Name");
    for (const SummaryLine &l : lines)
        for (const std::string &c : l.cells)
            CHECK(c.find_first_of("\n\r\t") == std::string::npos);
}

TEST_CASE("Spoolman summary lines: ignored value stays capped and is one line; markup is plain text", "[Spoolman]")
{
    SyncOutcome o = normal();
    RejectedValue rv;
    rv.spoolman_id = 16; rv.preset_name = red_result().preset_name; rv.key_or_field = "filament_start_gcode";
    rv.reason = "not allowed"; rv.value = "<b>M104</b>\n" + std::string(500, 'x');
    o.report.rejected.push_back(rv);
    std::vector<SummaryLine> lines = summary_lines(build_summary(o));
    size_t n = 0;
    for (const SummaryLine &l : lines)
        if (l.kind == SummaryLine::Kind::ignored) {
            ++n;
            const std::string &t = l.cells[0];
            CHECK(t.find("Ignored filament_start_gcode = <b>M104</b>\\x0a") == 0); // markup literal, the model already shows the newline as \\x0a
            CHECK(t.size() < 120 + 80);
            CHECK(t.rfind(" (not allowed)") == t.size() - std::string(" (not allowed)").size());
        }
    CHECK(n == 1);
}

TEST_CASE("Spoolman summary lines: unreachable state leads with the stale notice", "[Spoolman]")
{
    SyncOutcome o;
    o.catalog_state = CatalogState::unreachable;
    o.has_previous_sync = true;
    o.last_good_age_seconds = 2 * 86400;
    std::vector<SummaryLine> lines = summary_lines(build_summary(o));
    REQUIRE_FALSE(lines.empty());
    CHECK(lines.front().kind == SummaryLine::Kind::notice);
    CHECK(lines.front().cells[0].find("Spoolman") != std::string::npos);
}
