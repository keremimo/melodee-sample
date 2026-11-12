// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// PROJECT (Shift + Pad 16): the open project's three jobs as tiles under
// the keys: B1 SAVE (to its file in one press; with no file yet the Name
// sheet first), B2 OPEN (the list), B3 NEW (asks first, as the MPC does).
// The wheel moves the highlight across the three, Push presses it (SAVE
// first). Settings and About are Shift + Pad 8's.
// The list behind OPEN: projects are files on the SD card, as many as it
// holds, in folders; SD card access at the top lends the card to a computer
// (pages/card_access_page.cpp). Each row a glyph first and its facts last.
// The wheel moves; Push or B3 opens: a folder goes in (B1 or Shift + Push
// comes back out, B1 at the top back to the tiles), a project's sheet opens
// its jobs as cards, the likely one first and lit (Save on the open one, Load
// on another; the wheel moves, Push does it, B1 back): on the open one Save, Save as new, Rename, Duplicate,
// Move to folder, New folder here, Open at start, Delete; on another Load,
// Load kit (its pads and samples onto the selected track, no patterns) and
// the same. Delete asks once more. Save as new and Rename open the Name
// sheet; Move to folder opens a picker of every folder (the wheel picks,
// Push or B3 moves, B1 backs out). K1 jumps, K2 sorts.
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/shell/key_pick.h"
#include "melodee/shell/project_store.h"
#include "melodee/ui/widgets.h"
#include <cstdio>
#include <filesystem>
#include <vector>

namespace melodee {
namespace {
using namespace ui;
constexpr unsigned visible = list_rows;
const juce::Colour amber = colour::amber;

enum Action { Load, Save, SaveAsNew, Rename, Duplicate, MoveTo, NewFolder, OpenAtStart, Delete, LoadKit };

// The sheet's rows for an entry: the open project's, or another's with Load first.
unsigned actions(bool open, std::array<Action, 8>& out, bool mpc = false) {
    if (mpc) { out[0] = Load; out[1] = LoadKit; return 2; } // the MPC's own file is only ever read
    if (open) { out = {Save, SaveAsNew, Rename, Duplicate, MoveTo, NewFolder, OpenAtStart, Delete}; return 8; }
    out = {Load, LoadKit, Duplicate, Rename, MoveTo, NewFolder, OpenAtStart, Delete};
    return 8;
}

// The list's rows: SD card access (at the top folder), then the folder's entries.
bool has_card_row(const std::string& folder) { return folder.empty(); }
unsigned first_entry(const std::string& folder) { return has_card_row(folder) ? 1 : 0; }
const ProjectEntry* entry_at(const ProjectStore::View& v, unsigned row, const std::string& folder) {
    unsigned first = first_entry(folder);
    return row >= first && row - first < v.entries.size() ? &v.entries[row - first] : nullptr;
}
// SD card access (parity phase 10b), at the top, as the MPC's Project menu ends with it.
bool on_card(unsigned row, const std::string& folder) { return has_card_row(folder) && row == 0; }

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

juce::String free_text(uint64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f GB", bytes / 1e9);
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
juce::String count_text(unsigned n, const char* one, const char* many) { return juce::String(n) + " " + (n == 1 ? one : many); }

// The three jobs as tiles under the keys.
void draw_tiles(Paint& p, const Snapshot& s, const ProjectStore::View& v) {
    Header h = header_of(s);
    if (v.root.empty()) h.left.push_back(pill(Icon::Card, "NO CARD", amber));
    else if (s.dirty) h.left.push_back(pill(Icon::Rec, "UNSAVED", amber));
    h.right.push_back(label(s.project.name));
    unsigned pick = s.ui.project_pick % 3; // the wheel's tile, raised; Push presses its key
    SoftKeys keys {{"SAVE", "OPEN", "NEW"}, int(pick), colour::text};
    keys.glyphs = {Icon::Save, Icon::Folder, Icon::Plus};
    frame(p, s, keys, h);
    unsigned projects = 0;
    for (const auto& e : v.entries) projects += e.kind == ProjectEntry::Kind::Project;
    juce::String fresh = !s.dirty ? juce::String() : s.project_path.empty() ? juce::String("CHANGES LOST") : juce::String("SAVES FIRST");
    key_tiles(p, {word_sub_tile(Icon::Save, "SAVE", juce::String(s.project.name), pick == 0),
                  word_sub_tile(Icon::Folder, "OPEN", v.root.empty() ? juce::String("NO CARD") : count_text(projects, "PROJECT", "PROJECTS"), pick == 1),
                  word_sub_tile(Icon::Plus, "NEW", fresh, pick == 2)});
}

// One row of the list: a glyph, the name, its facts at the right edge.
void draw_row(Paint& p, const Snapshot& s, const ProjectStore::View& v, unsigned n, float y, bool current) {
    float x = zone::pad, w = zone::width - 2 * zone::pad, cy = y + list_row_h / 2, right = x + w - 8;
    if (current) { p.fill(x, y, w, list_row_h, colour::raised, 3); p.fill(x, y, 2.5f, list_row_h, colour::text, 1); }
    auto glyph_c = current ? colour::text : colour::mid;
    auto facts = plain(colour::mid, type::small, Weight::SemiBold);
    float tx = x + 28;
    if (on_card(n, s.project_folder)) { // the card, to a computer over USB-C
        icon(p, Icon::Card, x + 10, cy - 6, 12, glyph_c, 1.4f);
        float fw = p.width("USB-C", caps_text(colour::mid));
        p.text("USB-C", right - fw, y, fw + 1, list_row_h, caps_text(colour::mid));
        icon(p, Icon::Usb, right - fw - 14, cy - 5, 10, colour::mid, 1.5f);
        p.text("SD card access", tx, y, right - fw - 20 - tx, list_row_h, plain(current ? colour::text : colour::sec, type::list, Weight::Regular));
        return;
    }
    const auto* e = entry_at(v, n, s.project_folder);
    if (!e) return;
    bool open = e->kind == ProjectEntry::Kind::Project && e->path == s.project_path;
    if (e->kind == ProjectEntry::Kind::Folder) {
        icon(p, Icon::Folder, x + 10, cy - 6, 12, glyph_c, 1.4f);
        icon(p, Icon::ChevronRight, right - 10, cy - 5, 10, colour::dim, 1.6f);
        juce::String count(e->projects);
        float cw = p.width(count, facts);
        p.text(count, right - 14 - cw, y, cw + 1, list_row_h, facts);
        right -= 20 + cw;
    } else {
        icon(p, Icon::File, x + 10, cy - 6, 12, glyph_c, 1.4f);
        if (open) { // the one open: OPEN, after a dot while it has changes the card has not
            auto chip_style = caps_text(colour::bg);
            float cw = p.width("OPEN", chip_style) + 8;
            right -= cw;
            p.fill(right, cy - 7.5f, cw, 15, colour::text, 3);
            p.text("OPEN", right, cy - 7.5f, cw, 15, chip_style, juce::Justification::centred);
            if (s.dirty) { right -= 9; p.circle(right + 2, cy, 2.5f, amber); }
            right -= 6;
        } else {
            juce::String meta = e->mpc() ? juce::String("MPC")
                              : count_text(e->tracks, "track", "tracks") + juce::String::fromUTF8(" \xc2\xb7 ") + juce::String(e->tempo / 10) + " BPM";
            float mw = p.width(meta, facts);
            p.text(meta, right - mw, y, mw + 1, list_row_h, facts);
            right -= mw + 8;
        }
    }
    auto name = plain(current || open ? colour::text : colour::sec, type::list, current ? Weight::Medium : Weight::Regular);
    p.text(e->name, tx, y, right - tx, list_row_h, p.fit(e->name, name, right - tx, type::floor));
}

void draw(Paint& p, const Snapshot& s) {
    auto v = s.projects;
    sort_view(v, s.ui.project_sort);
    if (!s.ui.project_list) { draw_tiles(p, s, v); return; }
    Header h = header_of(s);
    juce::String where = v.root.empty() ? juce::String("NO CARD") : "SD / PROJECTS" + (s.project_folder.empty() ? juce::String() : " / " + juce::String(s.project_folder));
    h.left.push_back(pill(where));
    if (v.free_bytes) h.right.push_back(pill(Icon::Card, free_text(v.free_bytes)));
    bool sheet = s.ui.sheet_open;
    unsigned count = project_rows(v.entries.size(), s.project_folder), cursor = std::min<unsigned>(s.ui.project_cursor, std::max(count, 1u) - 1);
    const auto* in_hand = entry_at(v, cursor, s.project_folder);
    bool open_one = in_hand && in_hand->path == s.project_path, mpc = in_hand && in_hand->mpc();
    if (s.ui.move_open) frame(p, s, {{"BACK", "", "MOVE"}, 2, colour::text, {KeyIcon::Back, KeyIcon::None, KeyIcon::Check}}, h);
    else if (sheet) frame(p, s, {{"BACK", "", ""}, -1, colour::text, {KeyIcon::Back, KeyIcon::None, KeyIcon::None}}, h); // the cards
    else frame(p, s, {{"BACK", "", "OPEN"}, 2, colour::text, {KeyIcon::Back, KeyIcon::None, KeyIcon::Check}}, h);

    unsigned first = cursor < 3 ? 0 : std::min(cursor - 3, count > visible ? count - visible : 0);
    for (unsigned row = 0; row < visible && first + row < count; ++row)
        draw_row(p, s, v, first + row, zone::picture_y + 1 + row * (list_row_h + list_row_gap), first + row == cursor);
    if (!count) { // an empty folder, or no card
        icon(p, v.root.empty() ? Icon::Card : Icon::Folder, zone::width / 2.0f - 14, zone::picture_y + 50, 28, colour::dim, 1.4f);
        return;
    }
    if (s.ui.move_open) { // the folder picker over the list
        const auto* e = in_hand;
        if (!e) return;
        auto list = destinations(v, e->path);
        p.fill(0, zone::picture_y, zone::width, zone::picture_h, colour::bg); // nothing of the list shows beside the sheet
        p.dim_zone(zone::knobs_y, zone::knobs_h);
        float px = zone::pad + 6, pw = zone::width - 2 * zone::pad - 12, py = zone::picture_y + 3, ph = zone::picture_h - 6;
        p.fill(px, py, pw, ph, colour::surf, 6);
        juce::String move = "MOVE " + juce::String(e->name) + " TO";
        p.text(move, px + 9, py + 5, pw - 18, 12, p.fit(move, caps_text(colour::mid), pw - 18, type::floor));
        if (list.empty()) {
            icon(p, Icon::Folder, px + pw / 2 - 12, py + 40, 24, colour::dim, 1.4f); // nowhere else to go (New folder here makes one)
            return;
        }
        constexpr unsigned shown = 6;
        unsigned at = std::min<unsigned>(s.ui.move_cursor, unsigned(list.size()) - 1);
        unsigned from = at < shown / 2 ? 0 : std::min<unsigned>(at - shown / 2, list.size() > shown ? unsigned(list.size()) - shown : 0);
        for (unsigned r = 0; r < shown && from + r < list.size(); ++r) {
            unsigned n = from + r;
            float ry = py + 19 + r * 20, rx = px + 5, rw = pw - 10;
            bool on = n == at;
            if (on) { p.fill(rx, ry, rw, 19, colour::raised, 3); p.fill(rx, ry, 2.5f, 19, colour::text, 1); }
            icon(p, Icon::Folder, rx + 6, ry + 4, 11, on ? colour::text : colour::mid, 1.4f);
            juce::String name = list[n].empty() ? juce::String("Projects") : juce::String(list[n]).replace("/", juce::String::fromUTF8(" \xe2\x80\xba "));
            p.text(name, rx + 24, ry, rw - 30, 19, p.fit(name, plain(on ? colour::text : colour::sec, type::small, Weight::Medium), rw - 30, type::floor));
        }
        return;
    }
    if (!v.message.empty() && !sheet)
        p.text(v.message, zone::pad, zone::picture_y + 132, zone::width - 2 * zone::pad, 12, caps_text(colour::rec), juce::Justification::centredRight);

    if (sheet) {
        const auto* e = in_hand;
        if (!e || e->kind != ProjectEntry::Kind::Project) return;
        std::array<Action, 8> list {};
        unsigned n = actions(open_one, list, mpc);
        CardSheet m;
        m.chip = open_one ? "OPEN" : "PROJECT"; m.title = e->name;
        if (open_one && s.dirty) { m.detail = "UNSAVED"; m.detail_colour = amber; }
        else if (mpc) { m.detail = "OPENS AS A COPY"; m.detail_colour = colour::mid; }
        for (unsigned k = 0; k < n; ++k) {
            bool start = v.start == e->path;
            switch (list[k]) {
            case Load: m.items[k] = card_item(Icon::Folder, "LOAD"); break;
            case LoadKit: m.items[k] = card_item(Icon::Pads, "LOAD KIT", "NO PATTERNS"); break;
            case Save: m.items[k] = card_item(Icon::Save, "SAVE"); break;
            case SaveAsNew: m.items[k] = card_item(Icon::File, "SAVE AS NEW"); break;
            case Rename: m.items[k] = card_item(Icon::Keyboard, "RENAME"); break;
            case Duplicate: m.items[k] = card_item(Icon::Copy, "DUPLICATE"); break;
            case MoveTo: m.items[k] = card_item(Icon::ArrowRight, "MOVE TO FOLDER"); break;
            case NewFolder: m.items[k] = card_item(Icon::Plus, "NEW FOLDER HERE"); break;
            case OpenAtStart: m.items[k] = card_item(Icon::Flag, "OPEN AT START", start ? "ON" : "OFF", start); break;
            case Delete: m.items[k] = card_item(Icon::Trash, "DELETE", s.ui.delete_armed && k == s.ui.sheet_row ? "PRESS AGAIN" : "", false, true); break;
            }
        }
        m.count = n; m.current = s.ui.sheet_row % n;
        card_sheet(p, m);
    }
}

ProjectStore::View view_of(Context& c) {
    ProjectStore::View v;
    if (c.store) c.store->view(v);
    sort_view(v, c.ui.project_sort);
    c.ui.project_rows = uint16_t(project_rows(v.entries.size(), c.project_folder));
    return v;
}

void close_sheet(Context& c) { c.ui.sheet_open = false; c.ui.delete_armed = false; }

void act(Context& c, Action action) {
    auto v = view_of(c);
    const auto* e = entry_at(v, c.ui.project_cursor, c.project_folder);
    if (!e || e->kind != ProjectEntry::Kind::Project || !c.store) return;
    bool open = e->path == c.project_path;
    if (action != Delete) c.ui.delete_armed = false;
    switch (action) {
    case Load:
        if (e->mpc()) c.notice("Opening " + e->name);
        c.store->load(e->path);
        break;
    case LoadKit: close_sheet(c); c.load_kit_file(e->path, e->name); return;
    case Save: c.save_project_to(e->path); break;
    case SaveAsNew: close_sheet(c); c.name_project(true); return;
    case Duplicate: c.store->duplicate(e->path); break;
    case NewFolder: c.store->new_folder(); break;
    case OpenAtStart: c.store->open_at_start(e->path); break;
    case Delete:
        if (!c.ui.delete_armed) { c.ui.delete_armed = true; return; } // asks once more
        c.store->remove(e->path);
        if (open) c.project_path.clear(); // still open, but no longer on the card
        c.ui.delete_armed = false;
        break;
    case Rename: close_sheet(c); c.rename_project(e->path, e->name); return;
    case MoveTo: close_sheet(c); c.ui.move_open = true; c.ui.move_cursor = 0; return;
    }
    close_sheet(c);
    c.store->list(c.project_folder);
}

// The sheet's job: the card lit (the likely one, first, as it opens).
void act(Context& c) {
    auto v = view_of(c);
    const auto* e = entry_at(v, c.ui.project_cursor, c.project_folder);
    if (!e) return;
    bool open = e->path == c.project_path;
    std::array<Action, 8> list {};
    unsigned n = actions(open, list, e->mpc());
    act(c, list[c.ui.sheet_row % n]);
}

// The picker: Push or B3 moves the project there, B1 backs out.
void move_here(Context& c) {
    auto v = view_of(c);
    const auto* e = entry_at(v, c.ui.project_cursor, c.project_folder);
    c.ui.move_open = false;
    if (!e || !c.store) return;
    auto list = destinations(v, e->path);
    if (list.empty()) return;
    c.store->move(e->path, list[std::min<size_t>(c.ui.move_cursor, list.size() - 1)]);
    c.store->list(c.project_folder);
}

void open_list(Context& c) {
    c.ui.project_list = true;
    c.ui.project_cursor = 0;
    if (c.store) c.store->list(c.project_folder);
}

void wheel(Context& c, int delta) {
    if (!c.ui.project_list) { c.ui.project_pick = key_pick_turn(c.ui.project_pick, delta); return; } // the tiles: SAVE, OPEN, NEW
    if (c.ui.move_open) {
        auto v = view_of(c);
        const auto* e = entry_at(v, c.ui.project_cursor, c.project_folder);
        size_t n = e ? destinations(v, e->path).size() : 0;
        c.ui.move_cursor = uint16_t(std::clamp<int>(c.ui.move_cursor + delta, 0, std::max(int(n) - 1, 0)));
        return;
    }
    if (c.ui.sheet_open) {
        auto v = view_of(c);
        const auto* e = entry_at(v, c.ui.project_cursor, c.project_folder);
        std::array<Action, 8> list {};
        unsigned n = actions(e && e->path == c.project_path, list, e && e->mpc());
        c.ui.sheet_row = uint8_t(std::clamp<int>(c.ui.sheet_row + delta, 0, int(n) - 1));
        c.ui.delete_armed = false;
        return;
    }
    auto v = view_of(c);
    c.ui.project_cursor = uint16_t(std::clamp<int>(c.ui.project_cursor + delta, 0, std::max(int(project_rows(v.entries.size(), c.project_folder)) - 1, 0)));
}

void open_row(Context& c) {
    auto v = view_of(c);
    if (on_card(c.ui.project_cursor, c.project_folder)) { c.card_access(); return; }
    const auto* e = entry_at(v, c.ui.project_cursor, c.project_folder);
    if (!e) return;
    if (e->kind == ProjectEntry::Kind::Folder) {
        c.project_folder = c.project_folder.empty() ? e->name : c.project_folder + "/" + e->name;
        c.ui.project_cursor = 0;
        if (c.store) c.store->list(c.project_folder);
        return;
    }
    c.ui.sheet_open = true; c.ui.sheet_row = 0; c.ui.delete_armed = false; // the likely job (Save or Load) is the first card
}

void soft(Context& c, int key);

void push(Context& c) {
    if (!c.ui.project_list) { soft(c, c.ui.project_pick % 3); return; } // the tiles: Push presses the one the wheel lit
    if (c.ui.move_open) { move_here(c); return; }
    if (c.ui.sheet_open) { act(c); return; }
    open_row(c);
}

void soft(Context& c, int key) {
    if (!c.ui.project_list) { // SAVE, OPEN, NEW
        if (key == 0) c.save_project();
        if (key == 1) open_list(c);
        if (key == 2) c.ask_new_project(); // asks first, as the MPC does; the open one is saved first if it has a file
        return;
    }
    if (c.ui.move_open) {
        if (key == 0) c.ui.move_open = false;
        if (key == 2) move_here(c);
        return;
    }
    if (c.ui.sheet_open) { // the cards: B1 BACK; the wheel and Push do the rest
        if (key == 0) close_sheet(c);
        return;
    }
    if (key == 2) { open_row(c); return; }
    if (key != 0) return;
    if (!c.project_folder.empty()) { // BACK: up a folder, or from the top back to the tiles
        auto cut = c.project_folder.find_last_of('/');
        c.project_folder = cut == std::string::npos ? std::string() : c.project_folder.substr(0, cut);
        c.ui.project_cursor = 0;
        if (c.store) c.store->list(c.project_folder);
        return;
    }
    c.ui.project_list = false;
}
} // namespace

// Erase tapped on the list (shell/erase_copy.h): the project under the cursor's sheet open on DELETE, armed; tapped
// again, deleted (as Push on DELETE twice). False on a folder, SD card access or the MPC's own file.
bool project_erase(Context& c) {
    if (!c.ui.project_list || c.ui.move_open) return false;
    auto v = view_of(c);
    const auto* e = entry_at(v, c.ui.project_cursor, c.project_folder);
    if (!e || e->kind != ProjectEntry::Kind::Project || e->mpc()) return false;
    std::array<Action, 8> list {};
    unsigned n = actions(e->path == c.project_path, list, e->mpc()), row = n;
    for (unsigned k = 0; k < n; ++k) if (list[k] == Delete) row = k;
    if (row == n) return false;
    if (!c.ui.sheet_open || c.ui.sheet_row != row) { c.ui.sheet_open = true; c.ui.sheet_row = uint8_t(row); c.ui.delete_armed = false; }
    act(c, Delete);
    return true;
}

// Shift + Erase on the list: the project under the cursor duplicated (DUPLICATE). False where there is none.
bool project_copy(Context& c) {
    if (!c.ui.project_list || c.ui.move_open) return false;
    auto v = view_of(c);
    const auto* e = entry_at(v, c.ui.project_cursor, c.project_folder);
    if (!e || e->kind != ProjectEntry::Kind::Project || e->mpc()) return false;
    act(c, Duplicate);
    c.notice("Duplicated " + e->name);
    return true;
}

const PageSpec project_page {draw, play_pad, wheel, push, soft, wheel}; // Minus / Plus move the list

} // namespace melodee
