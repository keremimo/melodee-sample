// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// PROJECT (Shift + Pad 16): projects are files on the SD card, as many as it
// holds, in folders. The wheel moves; Push on New project starts one, Push
// on a folder goes in (Shift + Push comes back out), Push on a project
// opens its sheet: on the open one Save, Save as new, Duplicate, New folder
// here, Open at start, Delete; on any other Load heads the list. Delete asks
// once more. Save (with no file yet), Save as new and Rename open the Name
// sheet; Move to folder opens a picker of every folder (the wheel picks,
// Push or B3 moves, B1 backs out). The amber dot marks unsaved changes.
// Parity phase 10, as the MPC's project page: New project asks first (B3 DO
// IT, B1 CANCEL); DEMOS, at the top, holds the projects made in code
// (shell/demo_projects.h), opened unsaved; Load kit on another project's
// sheet takes only its kit (pads and samples, no patterns) onto the
// selected track. Parity phase 10b: SD card access, under DEMOS, lends the
// card to a computer (pages/card_access_page.cpp).
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/shell/demo_projects.h"
#include "melodee/shell/project_store.h"
#include "melodee/ui/widgets.h"
#include <cstdio>
#include <filesystem>
#include <vector>

namespace melodee {
namespace {
using namespace ui;
constexpr unsigned visible = 7;
const juce::Colour amber = colour::amber;

enum Action { Load, Save, SaveAsNew, Rename, Duplicate, MoveTo, NewFolder, OpenAtStart, Delete, LoadKit };

// The sheet's rows for an entry: the open project's, or another's with Load first.
unsigned actions(bool open, std::array<Action, 8>& out) {
    if (open) { out = {Save, SaveAsNew, Rename, Duplicate, MoveTo, NewFolder, OpenAtStart, Delete}; return 8; }
    out = {Load, LoadKit, Duplicate, Rename, MoveTo, NewFolder, OpenAtStart, Delete};
    return 8;
}
const char* action_name(Action a) {
    static const char* names[] {"Load", "Save", "Save as new", "Rename", "Duplicate", "Move to folder", "New folder here", "Open at start", "Delete",
                                "Load kit (no patterns)"};
    return names[a];
}

// The rows: New project, the open project when it has no file yet, DEMOS (at the top folder), then the
// folder's entries. In DEMOS the rows are the demos.
bool unsaved_row(const std::string& path) { return path.empty(); }
unsigned demos_row(const std::string& path) { return unsaved_row(path) ? 2 : 1; }
bool has_demos_row(const std::string& folder) { return folder.empty(); }
unsigned first_entry(const std::string& path, const std::string& folder) { return demos_row(path) + (has_demos_row(folder) ? 2 : 0); }
const ProjectEntry* entry_at(const ProjectStore::View& v, unsigned row, const std::string& path, const std::string& folder) {
    unsigned first = first_entry(path, folder);
    return row >= first && row - first < v.entries.size() ? &v.entries[row - first] : nullptr;
}
bool on_unsaved(unsigned row, const std::string& path) { return unsaved_row(path) && row == 1; }
bool on_demos(unsigned row, const std::string& path, const std::string& folder) { return has_demos_row(folder) && row == demos_row(path); }
// SD card access (parity phase 10b), under DEMOS at the top, as the MPC's Project menu ends with it.
bool on_card(unsigned row, const std::string& path, const std::string& folder) { return has_demos_row(folder) && row == demos_row(path) + 1; }
// A demo's tracks and tempo, made once.
struct DemoMeta { unsigned tracks = 0, tempo = 0; };
const DemoMeta& demo_meta(unsigned n) {
    static const auto metas = [] {
        std::array<DemoMeta, demo_count> out {};
        for (unsigned k = 0; k < demo_count; ++k) {
            auto project = std::make_unique<Project>();
            auto bank = std::make_unique<PatternBank>();
            make_demo(k, *project, *bank);
            out[k] = {project->used_tracks(), project->tempo};
        }
        return out;
    }();
    return metas[n % demo_count];
}

// Move to folder: where a project can go (the top, then every folder by
// name), all but the folder it is in.
std::vector<std::string> destinations(const ProjectStore::View& v, const std::string& path) {
    std::string here = std::filesystem::path(path).parent_path().string(), top = v.root;
    std::string rel = here.size() > top.size() ? here.substr(top.size() + 1) : std::string();
    std::vector<std::string> out;
    if (!rel.empty()) out.push_back("");
    for (const auto& f : v.folders) if (f != rel) out.push_back(f);
    return out;
}

void folder_icon(Paint& p, float x, float cy) {
    juce::Path icon;
    icon.addRoundedRectangle(x, cy - 4, 12, 9, 1.5f);
    icon.addRoundedRectangle(x, cy - 5.5f, 5, 3, 1);
    p.g.setColour(colour::mid); p.g.fillPath(icon);
}

// A microSD card: the cut corner and its contacts.
void card_icon(Paint& p, float x, float cy, juce::Colour c) {
    juce::Path card;
    card.startNewSubPath(x + 3, cy - 6);
    card.lineTo(x + 10, cy - 6); card.lineTo(x + 10, cy + 6); card.lineTo(x, cy + 6); card.lineTo(x, cy - 3);
    card.closeSubPath();
    p.g.setColour(c); p.g.strokePath(card, juce::PathStrokeType(1.2f));
    for (int k = 0; k < 3; ++k) p.fill(x + 3 + k * 2.2f, cy - 4, 1.1f, 3, c);
}

juce::String free_text(uint64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f GB FREE", bytes / 1e9);
    return text;
}

// Folders first, then projects newest first or by name, as K3 says.
void sort_view(ProjectStore::View& v, unsigned by_name) {
    std::stable_sort(v.entries.begin(), v.entries.end(), [&](const ProjectEntry& a, const ProjectEntry& b) {
        if (a.kind != b.kind) return a.kind == ProjectEntry::Kind::Folder;
        if (by_name || a.kind == ProjectEntry::Kind::Folder) return a.name < b.name;
        return a.modified > b.modified;
    });
}
// DEMOS: the projects made in code; Push opens one's sheet (Load), Shift + Push or B1 comes back out.
void draw_demos(Paint& p, const Snapshot& s) {
    Header h = header_of(s);
    h.left.push_back(pill("DEMOS"));
    bool sheet = s.ui.sheet_open;
    frame(p, s, sheet ? SoftKeys {{"BACK", "", "LOAD"}, 2, colour::text, {KeyIcon::Back, KeyIcon::None, KeyIcon::Check}}
                      : SoftKeys {{"BACK", "SETTINGS", "ABOUT"}, -1, colour::text, {KeyIcon::Back, KeyIcon::None, KeyIcon::None}}, h);
    unsigned cursor = std::min<unsigned>(s.ui.project_cursor, demo_count - 1);
    float x = zone::pad, w = zone::width - 2 * zone::pad;
    for (unsigned n = 0; n < demo_count; ++n) {
        float y = zone::picture_y + 0.5f + n * (list_row_h + list_row_gap), cy = y + list_row_h / 2;
        bool current = n == cursor;
        if (current) {
            p.fill(x, y, w, list_row_h, colour::raised, 4);
            p.g.saveState(); p.g.reduceClipRegion(juce::Rectangle<int>(int(x), int(y), 3, int(list_row_h)));
            p.fill(x, y, 8, list_row_h, colour::text, 4); p.g.restoreState();
        }
        const auto& meta = demo_meta(n);
        juce::String text = juce::String(meta.tracks) + " TRACKS" + juce::String::fromUTF8(" \xc2\xb7 ") + juce::String(meta.tempo / 10) + " BPM";
        auto meta_style = caps_text(colour::mid, 7);
        float mw = p.width(text, meta_style), right = x + w - 7 - mw;
        p.text(text, right, y, mw + 1, list_row_h, meta_style);
        p.circle(x + 15, cy, 2.5f, current ? colour::text : colour::mid);
        p.text(demo_name(n), x + 27, y, right - x - 31, list_row_h, plain(current ? colour::text : colour::sec, 12, Weight::Medium));
    }
    p.text("Made in Melodee: they open unsaved, Save gives them a name", x, zone::picture_y + 132, w, 12, caps_text(colour::dim, 7),
           juce::Justification::centredRight);
    if (sheet) {
        MenuSheet m;
        m.title = demo_name(cursor); m.accent = colour::text;
        m.detail = s.dirty ? "UNSAVED CHANGES CLOSE" : "DEMO"; m.detail_colour = s.dirty ? amber : colour::mid;
        m.rows[0] = {"Load", "", false, false, false};
        m.count = 1; m.current = 0;
        p.dim_zone(zone::picture_y, zone::picture_h, 0.7f);
        p.dim_zone(zone::knobs_y, zone::knobs_h);
        wide_sheet(p, m, "DEMO", colour::text);
    }
}

void draw(Paint& p, const Snapshot& s) {
    if (s.ui.project_demos) { draw_demos(p, s); return; }
    auto v = s.projects;
    sort_view(v, s.ui.project_sort);
    Header h = header_of(s);
    juce::String where = v.root.empty() ? juce::String("NO CARD") : "SD / PROJECTS" + (s.project_folder.empty() ? juce::String() : " / " + juce::String(s.project_folder).toUpperCase());
    h.left.push_back(pill(where));
    if (v.free_bytes) h.right.push_back(pill(free_text(v.free_bytes)));
    bool sheet = s.ui.sheet_open;
    if (s.ui.move_open) frame(p, s, {{"BACK", "", "MOVE"}, 2, colour::text, {KeyIcon::Back, KeyIcon::None, KeyIcon::Check}}, h);
    else if (sheet) frame(p, s, {{"BACK", "", "DO IT"}, 2, colour::text, {KeyIcon::Back, KeyIcon::None, KeyIcon::Check}}, h);
    else frame(p, s, {{"PROJECT", "SETTINGS", "ABOUT"}, 0, colour::text}, h);

    unsigned count = unsigned(v.entries.size()) + first_entry(s.project_path, s.project_folder), cursor = std::min<unsigned>(s.ui.project_cursor, count - 1);
    unsigned first = cursor < 3 ? 0 : std::min(cursor - 3, count > visible ? count - visible : 0);
    float x = zone::pad, w = zone::width - 2 * zone::pad;
    for (unsigned row = 0; row < visible && first + row < count; ++row) {
        unsigned n = first + row;
        float y = zone::picture_y + 0.5f + row * (list_row_h + list_row_gap);
        const auto* e = entry_at(v, n, s.project_path, s.project_folder);
        bool current = n == cursor, open = e && e->kind == ProjectEntry::Kind::Project && e->path == s.project_path;
        if (current) {
            p.fill(x, y, w, list_row_h, colour::raised, 4);
            p.g.saveState(); p.g.reduceClipRegion(juce::Rectangle<int>(int(x), int(y), 3, int(list_row_h)));
            p.fill(x, y, 8, list_row_h, colour::text, 4); p.g.restoreState();
        }
        float tx = x + 27, cy = y + list_row_h / 2;
        if (on_demos(n, s.project_path, s.project_folder)) { // the demos, as a folder
            folder_icon(p, x + 10, cy);
            juce::String meta = juce::String(demo_count) + " PROJECTS" + juce::String::fromUTF8(" \xc2\xb7 ") + "BUILT IN";
            auto meta_style = caps_text(colour::mid, 7);
            float mw = p.width(meta, meta_style);
            p.text(meta, x + w - 7 - mw, y, mw + 1, list_row_h, meta_style);
            p.text("Demos", tx, y, w - mw - 40, list_row_h, plain(colour::sec, 12, Weight::Medium));
            continue;
        }
        if (on_card(n, s.project_path, s.project_folder)) { // the card, to a computer over USB-C
            card_icon(p, x + 10, cy, colour::mid);
            auto meta_style = caps_text(colour::mid, 7);
            float mw = p.width("USB-C", meta_style);
            p.text("USB-C", x + w - 7 - mw, y, mw + 1, list_row_h, meta_style);
            p.text("SD card access", tx, y, w - mw - 40, list_row_h, plain(colour::sec, 12, Weight::Medium));
            continue;
        }
        if (on_unsaved(n, s.project_path)) { // the open project, not on the card yet
            auto chip_style = caps_text(colour::bg, 7);
            float cw = p.width("OPEN", chip_style) + 8, right = x + w - 7 - cw;
            p.fill(right, cy - 5.5f, cw, 11, colour::text, 3);
            p.text("OPEN", right, cy - 5.5f, cw, 11, chip_style, juce::Justification::centred);
            auto style = caps_text(amber, 7);
            float uw = p.width("NOT SAVED", style);
            p.text("NOT SAVED", right - uw - 6, y, uw + 1, list_row_h, style);
            p.circle(right - uw - 11, cy, 2, amber);
            p.text(s.project.name, tx, y, right - uw - 20 - tx, list_row_h, plain(colour::text, 12, Weight::Medium));
            continue;
        }
        if (!e) { // New project
            p.fill(x + 11, cy - 0.75f, 9, 1.5f, colour::mid); p.fill(x + 14.75f, cy - 4.5f, 1.5f, 9, colour::mid);
            p.text("New project", tx, y, w - 40, list_row_h, plain(colour::mid, 12, Weight::Medium));
            continue;
        }
        juce::String meta;
        if (e->kind == ProjectEntry::Kind::Folder) {
            folder_icon(p, x + 10, cy);
            meta = juce::String(e->projects) + (e->projects == 1 ? " PROJECT" : " PROJECTS");
        } else {
            meta = juce::String(e->tracks) + (e->tracks == 1 ? " TRACK" : " TRACKS") + juce::String::fromUTF8(" \xc2\xb7 ") + juce::String(e->tempo / 10) + " BPM";
        }
        auto meta_style = caps_text(open ? colour::text : colour::mid, 7);
        float mw = p.width(meta, meta_style);
        float right = x + w - 7 - mw;
        p.text(meta, right, y, mw + 1, list_row_h, meta_style);
        if (open) {
            auto chip_style = caps_text(colour::bg, 7);
            float cw = p.width("OPEN", chip_style) + 8;
            right -= cw + 6;
            p.fill(right, cy - 5.5f, cw, 11, colour::text, 3);
            p.text("OPEN", right, cy - 5.5f, cw, 11, chip_style, juce::Justification::centred);
            if (s.dirty) {
                auto style = caps_text(amber, 7);
                float uw = p.width("UNSAVED", style);
                right -= uw + 6;
                p.text("UNSAVED", right, y, uw + 1, list_row_h, style);
                right -= 7;
                p.circle(right + 2, cy, 2, amber);
            }
        }
        p.text(e->name, tx, y, right - tx - 4, list_row_h, p.fit(e->name, plain(open ? colour::text : colour::sec, 12, Weight::Medium), right - tx - 4, 8));
    }
    if (s.ui.move_open) { // the folder picker over the list
        const auto* e = entry_at(v, cursor, s.project_path, s.project_folder);
        if (!e) return;
        auto list = destinations(v, e->path);
        p.dim_zone(zone::picture_y, zone::picture_h, 0.7f);
        p.dim_zone(zone::knobs_y, zone::knobs_h);
        float px = zone::pad + 6, pw = zone::width - 2 * zone::pad - 12, py = zone::picture_y + 3, ph = zone::picture_h - 6;
        p.fill(px, py, pw, ph, colour::surf, 6);
        p.ring(px, py, pw, ph, colour::line, 6);
        p.text("MOVE " + juce::String(e->name).toUpperCase() + " TO", px + 9, py + 5, pw - 18, 10, p.fit("MOVE " + juce::String(e->name).toUpperCase() + " TO", caps_text(colour::mid, 7), pw - 18, 5));
        if (list.empty()) {
            p.text("No other folder: New folder here makes one", px, py + 50, pw, 14, plain(colour::dim, 10), juce::Justification::centred);
            return;
        }
        constexpr unsigned shown = 6;
        unsigned at = std::min<unsigned>(s.ui.move_cursor, unsigned(list.size()) - 1);
        unsigned from = at < shown / 2 ? 0 : std::min<unsigned>(at - shown / 2, list.size() > shown ? unsigned(list.size()) - shown : 0);
        for (unsigned r = 0; r < shown && from + r < list.size(); ++r) {
            unsigned n = from + r;
            float ry = py + 19 + r * 20, rx = px + 5, rw = pw - 10;
            bool on = n == at;
            if (on) {
                p.fill(rx, ry, rw, 19, colour::raised.brighter(0.25f), 4);
                p.g.saveState(); p.g.reduceClipRegion(juce::Rectangle<int>(int(rx), int(ry), 3, 19));
                p.fill(rx, ry, 8, 19, colour::text, 4); p.g.restoreState();
            }
            folder_icon(p, rx + 6, ry + 9.5f);
            juce::String name = list[n].empty() ? juce::String("Projects (the top)") : juce::String(list[n]).replace("/", juce::String::fromUTF8(" \xe2\x80\xba "));
            p.text(name, rx + 22, ry, rw - 28, 19, p.fit(name, plain(on ? colour::text : colour::sec, 11, Weight::Medium), rw - 28, 7));
        }
        return;
    }
    if (!v.message.empty() && !sheet)
        p.text(v.message, x, zone::picture_y + 132, w, 12, caps_text(colour::rec, 7), juce::Justification::centredRight);

    if (sheet) {
        const auto* e = entry_at(v, cursor, s.project_path, s.project_folder);
        bool unsaved = on_unsaved(cursor, s.project_path);
        if (!unsaved && (!e || e->kind != ProjectEntry::Kind::Project)) return;
        bool open = unsaved || e->path == s.project_path;
        juce::String title = unsaved ? juce::String(s.project.name) : juce::String(e->name);
        std::array<Action, 8> list {};
        unsigned n = actions(open, list);
        MenuSheet m;
        m.title = title; m.accent = colour::text;
        if (unsaved) { m.detail = "NOT SAVED"; m.detail_colour = amber; }
        else if (open && s.dirty) { m.detail = "UNSAVED CHANGES"; m.detail_colour = amber; }
        for (unsigned k = 0; k < n; ++k) {
            bool off = false;
            juce::String name = action_name(list[k]);
            if (list[k] == Delete && s.ui.delete_armed && k == s.ui.sheet_row) name = "Delete: press again";
            if (list[k] == OpenAtStart && !unsaved && v.start == e->path) name = "Opens at start";
            if (unsaved && (list[k] == Duplicate || list[k] == OpenAtStart || list[k] == Delete || list[k] == MoveTo)) off = true; // no file yet
            m.rows[k] = {name, "", false, list[k] == Delete, off};
        }
        m.count = n; m.current = s.ui.sheet_row % n;
        p.dim_zone(zone::picture_y, zone::picture_h, 0.7f);
        p.dim_zone(zone::knobs_y, zone::knobs_h);
        wide_sheet(p, m, open ? "OPEN" : "PROJECT", colour::text);
    }
}

ProjectStore::View view_of(Context& c) {
    ProjectStore::View v;
    if (c.store) c.store->view(v);
    sort_view(v, c.ui.project_sort);
    c.ui.project_rows = uint16_t(v.entries.size() + first_entry(c.project_path, c.project_folder));
    return v;
}

void act(Context& c) {
    auto v = view_of(c);
    const auto* e = entry_at(v, c.ui.project_cursor, c.project_path, c.project_folder);
    bool unsaved = on_unsaved(c.ui.project_cursor, c.project_path);
    if ((!e && !unsaved) || !c.store) return;
    bool open = unsaved || e->path == c.project_path;
    std::array<Action, 8> list {};
    unsigned n = actions(open, list);
    auto action = list[c.ui.sheet_row % n];
    if (action != Delete) c.ui.delete_armed = false;
    if (unsaved && (action == Duplicate || action == OpenAtStart || action == Delete || action == MoveTo)) return;
    switch (action) {
    case Load: c.store->load(e->path); break;
    case LoadKit: c.ui.sheet_open = false; c.load_kit_file(e->path, e->name); return;
    case Save:
        if (unsaved) { c.name_project(false); return; } // a name first
        c.save_project_to(e->path);
        break;
    case SaveAsNew: c.name_project(true); return;
    case Duplicate: c.store->duplicate(e->path); break;
    case NewFolder: c.store->new_folder(); break;
    case OpenAtStart: c.store->open_at_start(e->path); break;
    case Delete:
        if (!c.ui.delete_armed) { c.ui.delete_armed = true; return; } // asks once more
        c.store->remove(e->path);
        if (open) c.project_path.clear(); // still open, but no longer on the card
        c.ui.delete_armed = false;
        break;
    case Rename: c.rename_project(unsaved ? std::string() : e->path, unsaved ? std::string(c.project.name) : e->name); return;
    case MoveTo: c.ui.sheet_open = false; c.ui.move_open = true; c.ui.move_cursor = 0; return;
    }
    c.ui.sheet_open = false;
    c.store->list(c.project_folder);
}

// The picker: Push or B3 moves the project there, B1 backs out.
void move_here(Context& c) {
    auto v = view_of(c);
    const auto* e = entry_at(v, c.ui.project_cursor, c.project_path, c.project_folder);
    c.ui.move_open = false;
    if (!e || !c.store) return;
    auto list = destinations(v, e->path);
    if (list.empty()) return;
    c.store->move(e->path, list[std::min<size_t>(c.ui.move_cursor, list.size() - 1)]);
    c.store->list(c.project_folder);
}

void wheel(Context& c, int delta) {
    if (c.ui.project_demos) {
        if (!c.ui.sheet_open) c.ui.project_cursor = uint16_t(std::clamp<int>(c.ui.project_cursor + delta, 0, int(demo_count) - 1));
        return;
    }
    if (c.ui.move_open) {
        auto v = view_of(c);
        const auto* e = entry_at(v, c.ui.project_cursor, c.project_path, c.project_folder);
        size_t n = e ? destinations(v, e->path).size() : 0;
        c.ui.move_cursor = uint16_t(std::clamp<int>(c.ui.move_cursor + delta, 0, std::max(int(n) - 1, 0)));
        return;
    }
    if (c.ui.sheet_open) {
        auto v = view_of(c);
        const auto* e = entry_at(v, c.ui.project_cursor, c.project_path, c.project_folder);
        std::array<Action, 8> list {};
        unsigned n = actions(on_unsaved(c.ui.project_cursor, c.project_path) || (e && e->path == c.project_path), list);
        c.ui.sheet_row = uint8_t(std::clamp<int>(c.ui.sheet_row + delta, 0, int(n) - 1));
        c.ui.delete_armed = false;
        return;
    }
    auto v = view_of(c);
    c.ui.project_cursor = uint16_t(std::clamp<int>(c.ui.project_cursor + delta, 0, int(v.entries.size() + first_entry(c.project_path, c.project_folder)) - 1));
}

void push(Context& c) {
    if (c.ui.project_demos) { // a demo's sheet, then Load
        if (!c.ui.sheet_open) { c.ui.sheet_open = true; c.ui.sheet_row = 0; return; }
        c.ui.sheet_open = false; c.ui.project_demos = false;
        c.open_demo(c.ui.project_cursor % demo_count);
        return;
    }
    if (c.ui.move_open) { move_here(c); return; }
    if (c.ui.sheet_open) { act(c); return; }
    auto v = view_of(c);
    if (on_unsaved(c.ui.project_cursor, c.project_path)) { c.ui.sheet_open = true; c.ui.sheet_row = 0; c.ui.delete_armed = false; return; }
    if (on_demos(c.ui.project_cursor, c.project_path, c.project_folder)) { c.ui.project_demos = true; c.ui.project_cursor = 0; return; }
    if (on_card(c.ui.project_cursor, c.project_path, c.project_folder)) { c.card_access(); return; }
    const auto* e = entry_at(v, c.ui.project_cursor, c.project_path, c.project_folder);
    if (!e) { // New project: asks first, as the MPC does (B3 DO IT, B1 CANCEL); the open one is saved first if it has a file
        c.ask_new_project();
        return;
    }
    if (e->kind == ProjectEntry::Kind::Folder) {
        c.project_folder = c.project_folder.empty() ? e->name : c.project_folder + "/" + e->name;
        c.ui.project_cursor = 0;
        if (c.store) c.store->list(c.project_folder);
        return;
    }
    c.ui.sheet_open = true; c.ui.sheet_row = 0; c.ui.delete_armed = false;
}

void soft(Context& c, int key) {
    if (c.ui.project_demos) {
        if (c.ui.sheet_open) { if (key == 0) c.ui.sheet_open = false; if (key == 2) push(c); return; }
        if (key == 0) { c.ui.project_demos = false; c.ui.project_cursor = uint16_t(demos_row(c.project_path)); return; }
    }
    if (c.ui.move_open) {
        if (key == 0) c.ui.move_open = false;
        if (key == 2) move_here(c);
        return;
    }
    if (c.ui.sheet_open) {
        if (key == 0) { c.ui.sheet_open = false; c.ui.delete_armed = false; }
        if (key == 2) act(c);
        return;
    }
    if (key == 1) c.open(PageId::SettingsPage);
    if (key == 2) c.open(PageId::About);
}
} // namespace

const PageSpec project_page {draw, play_pad, wheel, push, soft, wheel}; // Minus / Plus move the list

} // namespace melodee
