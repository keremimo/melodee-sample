// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// PAD EFFECTS (B1 from Track on a sampler track), TRACK EFFECTS (Knob FX),
// BUS (B3 from Sends) and MASTER (B3 from Bus): one page shape for every
// stop along the signal, Stage's FX strip: up to six effects in a row (a
// pad's chain is the last pad hit's, four slots), each its family's glyph
// and name, and under it the one in hand drawn as what it does to sound.
// B1 and B3 name the chains before and after. The wheel picks a device,
// the three pots hold three of its parameters and Minus / Plus the other row
// (two dots by its name). Push on a device opens its sheet on BYPASS (B3),
// B2 MORE for change, presets, move, remove. Push on the plus opens the
// picker: 41 effects in six families, each with its glyph; Push enters a
// family, B3 adds, B1 backs out. A bus or the master has no track, so it is
// drawn in neutral.
#include "melodee/engine/effect_info.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/shell/bindings.h"
#include "melodee/ui/widgets.h"
#include <algorithm>

namespace melodee {
namespace {
using namespace ui;
// A device's sheet: Presets only on a plug-in that has some (its programs, PRESETS).
enum class Action : uint8_t { Change, Presets, Bypass, Left, Right, Remove };
using Actions = std::array<Action, 6>;
unsigned device_actions(const UiState& ui, const EffectSlot& slot, Actions& out) {
    unsigned n = 0;
    out[n++] = Action::Change;
    if (slot.kind == plugin_effect_kind && ui.plugin_panel.entry == unsigned(slot.params[0]) && ui.plugin_panel.programs > 0) out[n++] = Action::Presets;
    for (Action a : {Action::Bypass, Action::Left, Action::Right, Action::Remove}) out[n++] = a;
    return n;
}

juce::String bus_name(unsigned bus) { return juce::String("BUS ") + char('A' + bus % bus_count); }
// The picker's families: every one, but a sampler pad's own chain takes no plug-ins.
unsigned families(PageId page) { return unsigned(Family::Count) - (page == PageId::PadFx ? 1 : 0); }
bool plugin_slot(const EffectSlot& slot) { return slot.kind == plugin_effect_kind; }
unsigned bus_of(const UiState& ui) { return ui.sends_bus % bus_count; } // the bus in view (Sends' K2)

// The picker: the families down the left, each with the glyph the strip uses for it and how many it holds; the
// family in hand's effects in two columns on the right, names in Title Case; Plug-ins the card's folder.
void picker(Paint& p, const Snapshot& s, juce::Colour c) {
    constexpr float x = zone::pad, y = zone::picture_y + 4, w = zone::width - 2 * zone::pad, h = zone::picture_h - 8;
    p.fill(x, y, w, h, colour::panel, 4);
    p.fill(x + 106, y + 6, 1, h - 12, colour::line);
    unsigned shown = families(s.ui.page);
    Family in_hand = Family(s.ui.picker_family % unsigned(Family::Count));
    for (unsigned f = 0; f < shown; ++f) {
        FamilyList list {};
        unsigned count = family_effects(Family(f), list);
        bool on = f == s.ui.picker_family, focus = on && !s.ui.picker_items;
        float fy = y + 4 + f * 19.5f;
        if (on) p.fill(x + 4, fy, 98, 18, focus ? colour::raised : colour::surf, 3);
        icon(p, family_icon(Family(f)), x + 9, fy + 3.5f, 11, on ? c : colour::mid, 1.4f);
        p.text(family_name(Family(f)), x + 26, fy, 60, 18, plain(on ? colour::text : colour::mid, type::small, on ? Weight::SemiBold : Weight::Medium));
        juce::String n = Family(f) == Family::Plugins ? (s.plugins.root.empty() ? juce::String() : juce::String(int(s.plugins.files.size()))) : juce::String(count);
        p.text(n, x + 4, fy, 94, 18, plain(on ? c : colour::dim, type::small, Weight::SemiBold), juce::Justification::centredRight);
    }
    float cx = x + 114, cw = (w - 114 - 8 - 4) / 2;
    if (in_hand == Family::Plugins) { // the card's Plugins folder, one Push away
        bool on = s.ui.picker_items;
        if (on) p.fill(cx, y + 6, cw * 2 + 4, 20, c, 3);
        icon(p, Icon::Folder, cx + 6, y + 10, 12, on ? colour::bg : colour::sec, 1.4f);
        p.text("Plugins", cx + 24, y + 6, cw * 2 - 30, 20, plain(on ? colour::bg : colour::sec, type::body, Weight::Medium));
        icon(p, Icon::ChevronRight, cx + cw * 2 - 12, y + 11, 10, on ? colour::bg : colour::dim, 1.6f);
        return;
    }
    FamilyList list {};
    unsigned count = family_effects(in_hand, list);
    for (unsigned n = 0; n < count; ++n) {
        const auto& info = effect_info(list[n]);
        bool on = s.ui.picker_items && n == s.ui.picker_item;
        float ix = cx + (n / 6) * (cw + 4), iy = y + 6 + (n % 6) * 22;
        if (on) p.fill(ix, iy, cw, 20, info.built ? c : colour::raised, 3);
        juce::String name = name_case(juce::String(info.name).toUpperCase()); // Title Case: "Tape Echo"
        auto style = plain(on && info.built ? colour::bg : info.built ? colour::sec : colour::dim, type::body, on ? Weight::SemiBold : Weight::Regular);
        p.text(name, ix + 6, iy, cw - 8, 20, p.fit(name, style, cw - 8, type::floor));
    }
}

void draw(Paint& p, const Snapshot& s) {
    unsigned n = page_chain(s.project, s.ui), bus = bus_of(s.ui);
    const auto& chain = s.project.chain(n);
    bool pad = s.ui.page == PageId::PadFx, track = s.ui.page == PageId::Rack || pad, kit = s.track().engine == EngineKind::Sampler;
    auto c = track ? colour_of(s) : colour::text;
    unsigned count = chain.count(), cursor = std::min<unsigned>(s.ui.fx_cursor[UiState::cursor_of(n)], count);
    Header h = header_of(s);
    SoftKeys keys {{"", "TRACK", "SENDS"}, 1, c};
    // B1 and B3 the chains before and after, a chevron pointing the way.
    auto ends = [&](juce::String before, juce::String here, juce::String after) {
        keys.labels = {before, here, after};
        keys.glyphs[0] = before.isEmpty() ? Icon::None : Icon::ChevronLeft;
        keys.after[2] = after.isEmpty() ? Icon::None : Icon::ChevronRight;
    };
    if (pad && !kit) { // pad effects belong to a kit's pads
        ends("", "PAD", "TRACK");
        frame(p, s, keys, h);
        icon(p, Icon::Pads, zone::width / 2.0f - 14, zone::picture_y + 44, 28, colour::dim, 1.4f);
        icon(p, Icon::Cross, zone::width / 2.0f + 8, zone::picture_y + 64, 12, colour::dim, 1.8f);
        return;
    }
    if (pad) {
        unsigned last = s.track().last_pad % sampler_pads;
        char label[4]; pad_label(last, label);
        juce::String name = "PAD " + juce::String(label);
        if (s.track().pads[last].used()) name += juce::String::fromUTF8(" \xc2\xb7 ") + name_case(pad_name(s, last, false));
        ends("", "PAD", "TRACK");
        h.left.push_back(pill(name, c));
        h.right.push_back(tag_of(s, s.ui.track));
    } else if (track && s.ui.fx_loop >= 0) { // a loop track's chain (docs/LOOPER.md): B1 back to the Looper
        unsigned l = unsigned(s.ui.fx_loop) % loop_tracks;
        c = loop_colour(l);
        keys.accent = c;
        ends("LOOPER", "L" + juce::String(l + 1), "");
        h.right.push_back(chip("L" + juce::String(l + 1), c));
    } else if (track) { ends(kit ? "PAD" : "", "TRACK", "SENDS"); h.right.push_back(tag_of(s, s.ui.track)); }
    else if (s.ui.page == PageId::Bus) {
        ends("SENDS", bus_name(bus), "MASTER");
        h.left.push_back(pill("RETURN " + juce::String(s.project.buses[bus].level) + " %"));
        h.right.push_back(label(bus_name(bus), colour::mid));
    } else {
        ends(bus_name(bus), "MASTER", "");
        h.right.push_back(label("MASTER", colour::mid));
    }
    if (s.ui.sheet_open && cursor < count) { // the device's sheet: BACK, MORE, BYPASS; on the list BACK, -, DO IT
        keys = s.ui.sheet_more ? SoftKeys {{"BACK", "", "DO IT"}, 2, c, {KeyIcon::Back, KeyIcon::None, KeyIcon::Check}}
                               : SoftKeys {{"BACK", "MORE", "BYPASS"}, 2, c, {KeyIcon::Back, KeyIcon::None, KeyIcon::None}};
        if (!s.ui.sheet_more) keys.glyphs = {Icon::None, Icon::Layers, Icon::Power};
    }
    if (s.ui.picker_open) {
        keys = SoftKeys {{"BACK", "", s.ui.picker_replaces ? "CHANGE" : "ADD"}, 2, colour::text, {KeyIcon::Back, KeyIcon::None, KeyIcon::Check}};
        h.left.clear();
        h.left.push_back(pill(juce::String(effect_count()) + " EFFECTS"));
        h.left.push_back(pill("SLOT " + juce::String(cursor + 1), c));
        keys.glyphs = {}; keys.after = {};
    }
    frame(p, s, keys, h);
    if (s.ui.picker_open) { picker(p, s, c); return; }
    fx_strip(p, chain, cursor, c, zone::picture_y + 6);
    float y = zone::picture_y + 54, bottom = zone::picture_y + zone::picture_h - 4;
    fx_in_hand(p, s, chain, cursor, c, y, bottom);
    if (cursor < count && !plugin_slot(chain.slots[cursor]) && effect_rows(chain.slots[cursor], n) > 1) { // its rows, a KEY last (roadmap P4)
        unsigned rows = effect_rows(chain.slots[cursor], n);
        page_dots(p, zone::width - zone::pad, y + 12, int(rows), int(std::min<unsigned>(s.ui.fx_row[UiState::cursor_of(n)], rows - 1)), c, true);
    }

    if (s.ui.sheet_open && cursor < count) {
        const auto& slot = chain.slots[cursor];
        Actions actions {};
        unsigned rows = device_actions(s.ui, slot, actions);
        if (!s.ui.sheet_more) { // the likely job, Bypass, under B3; it stays to hear the difference
            auto tile = word_sub_tile(Icon::Power, "BYPASS", slot.bypass ? "ON" : "OFF");
            tile.colour = c;
            likely_sheet(p, tile, rows - 1);
            return;
        }
        MenuSheet m;
        m.title = effect_info(EffectKind(slot.kind)).name; m.detail = "SLOT " + juce::String(cursor + 1); m.accent = c;
        for (unsigned r = 0; r < rows; ++r)
            switch (actions[r]) {
            case Action::Change: m.rows[r] = {"Change", "", false, false, false}; break;
            case Action::Presets: m.rows[r] = {"Presets", juce::String(int(s.ui.plugin_panel.programs)), false, false, false}; break;
            case Action::Bypass: m.rows[r] = {"Bypass", slot.bypass ? "On" : "Off", slot.bypass, false, false}; break;
            case Action::Left: m.rows[r] = {"Move left", "", false, false, cursor == 0}; break;
            case Action::Right: m.rows[r] = {"Move right", "", false, false, cursor + 1 >= count}; break;
            case Action::Remove: m.rows[r] = {"Remove", "", false, true, false}; break;
            }
        m.count = rows; m.current = s.ui.sheet_row % rows;
        p.dim_zone(zone::picture_y, zone::picture_h, 0.6f);
        menu_sheet(p, m);
    }
}

unsigned chain_here(Context& c) { return page_chain(c.project, c.ui); }

void add_from_picker(Context& c) {
    if (Family(c.ui.picker_family % unsigned(Family::Count)) == Family::Plugins) { // the card's folder picks it
        unsigned n = chain_here(c), cursor = std::min<unsigned>(c.ui.fx_cursor[UiState::cursor_of(n)], c.project.chain(n).count());
        if (cursor >= c.project.chain(n).size) return;
        c.ui.picker_open = false;
        c.open_plugin_picker(UiState::PluginTarget::Effect, n, cursor);
        return;
    }
    FamilyList list {};
    unsigned count = family_effects(Family(c.ui.picker_family % unsigned(Family::Count)), list);
    if (c.ui.picker_item >= count || !effect_info(list[c.ui.picker_item]).built) return; // not built yet
    unsigned n = chain_here(c), at = UiState::cursor_of(n), cursor = std::min<unsigned>(c.ui.fx_cursor[at], c.project.chain(n).count());
    if (cursor >= c.project.chain(n).size) return;
    c.set_effect(n, cursor, list[c.ui.picker_item]);
    c.ui.fx_row[at] = 0;
    c.ui.picker_open = false;
}

void wheel(Context& c, int delta) {
    auto& ui = c.ui;
    if (ui.picker_open) {
        if (!ui.picker_items) { ui.picker_family = uint8_t(std::clamp<int>(ui.picker_family + delta, 0, int(families(ui.page)) - 1)); return; }
        if (Family(ui.picker_family) == Family::Plugins) return; // one entry
        FamilyList list {};
        unsigned count = family_effects(Family(ui.picker_family), list);
        ui.picker_item = uint8_t(std::clamp<int>(ui.picker_item + delta, 0, int(count) - 1));
        return;
    }
    unsigned n = chain_here(c), at = UiState::cursor_of(n);
    if (ui.sheet_open) {
        if (!ui.sheet_more) return; // the likely tile: a turn opens nothing (B2 MORE lists the rest)
        const auto& chain = c.project.chain(n);
        unsigned k = std::min<unsigned>(ui.fx_cursor[at], chain.size - 1);
        Actions actions {};
        unsigned rows = device_actions(ui, chain.slots[k], actions);
        ui.sheet_row = uint8_t(std::clamp<int>(ui.sheet_row + delta, 0, int(rows) - 1));
        return;
    }
    const auto& chain = c.project.chain(n);
    unsigned last = std::min<unsigned>(chain.count(), chain.size - 1);
    ui.fx_cursor[at] = uint8_t(std::clamp<int>(ui.fx_cursor[at] + delta, 0, int(last)));
    ui.fx_row[at] = 0;
}

void push(Context& c) {
    auto& ui = c.ui;
    unsigned n = chain_here(c), at = UiState::cursor_of(n);
    if (ui.page == PageId::PadFx && c.track().engine != EngineKind::Sampler) return;
    const auto& chain = c.project.chain(n);
    unsigned cursor = std::min<unsigned>(ui.fx_cursor[at], chain.count());
    if (ui.picker_open) {
        if (!ui.picker_items) { ui.picker_items = true; ui.picker_item = 0; }
        else add_from_picker(c);
        return;
    }
    if (ui.sheet_open && cursor < chain.count()) {
        if (!ui.sheet_more) { c.toggle_bypass(n, cursor); return; } // the likely job: Bypass, and it stays open to hear it
        Actions actions {};
        unsigned rows = device_actions(ui, chain.slots[cursor], actions);
        switch (actions[ui.sheet_row % rows]) {
        case Action::Change: ui.picker_open = true; ui.picker_items = false; ui.picker_replaces = true; break;
        case Action::Presets: ui.sheet_open = false; c.open_plugin_presets(unsigned(chain.slots[cursor].params[0])); return;
        case Action::Bypass: c.toggle_bypass(n, cursor); return; // stays open to see it change
        case Action::Left: c.move_effect(n, cursor, -1); if (cursor) ui.fx_cursor[at] = uint8_t(cursor - 1); break;
        case Action::Right: c.move_effect(n, cursor, 1); if (cursor + 1 < chain.count()) ui.fx_cursor[at] = uint8_t(cursor + 1); break;
        case Action::Remove: c.set_effect(n, cursor, EffectKind::None); break;
        }
        ui.sheet_open = false;
        return;
    }
    if (cursor < chain.count()) { ui.sheet_open = true; ui.sheet_more = false; ui.sheet_row = 0; return; }
    if (cursor < chain.size) { ui.picker_open = true; ui.picker_items = false; ui.picker_replaces = false; }
}

void soft(Context& c, int key) {
    auto& ui = c.ui;
    if (ui.picker_open) {
        if (key == 0) { if (ui.picker_items) ui.picker_items = false; else ui.picker_open = false; }
        if (key == 2) { if (ui.picker_items) add_from_picker(c); else { ui.picker_items = true; ui.picker_item = 0; } }
        return;
    }
    if (ui.sheet_open) { // BACK, MORE, BYPASS; on the list BACK, -, DO IT
        if (key == 0) { if (ui.sheet_more) ui.sheet_more = false; else ui.sheet_open = false; }
        if (key == 1 && !ui.sheet_more) { ui.sheet_more = true; ui.sheet_row = 0; }
        if (key == 2) push(c);
        return;
    }
    if (ui.page == PageId::Rack && ui.fx_loop >= 0) { if (key == 0) c.open(PageId::Looper); return; } // a loop track's chain
    bool kit = c.track().engine == EngineKind::Sampler;
    PageId before = ui.page == PageId::PadFx ? PageId::PadFx : ui.page == PageId::Rack ? (kit ? PageId::PadFx : PageId::Rack)
                  : ui.page == PageId::Bus ? PageId::Sends : PageId::Bus;
    PageId after = ui.page == PageId::PadFx ? PageId::Rack : ui.page == PageId::Rack ? PageId::Sends : PageId::Master;
    if (key == 0 && before != ui.page) c.open(before);
    if (key == 2 && after != ui.page) c.open(after);
}

// Minus / Plus: the device's other row of parameters; a plug-in's rows, one at a time.
void nudge(Context& c, int way) {
    unsigned n = chain_here(c), at = UiState::cursor_of(n);
    const auto& chain = c.project.chain(n);
    unsigned cursor = c.ui.fx_cursor[at];
    if (cursor < chain.count() && plugin_slot(chain.slots[cursor])) {
        unsigned entry = unsigned(chain.slots[cursor].params[0]);
        const auto& panel = c.ui.plugin_panel;
        if (entry >= max_plugins || panel.entry != entry || !panel.rows) return;
        auto& row = c.ui.plugin_row[entry];
        row = uint16_t(std::clamp<int>(row + way, 0, panel.rows - 1));
        c.ui.plugin_panel.row = row;
        c.plugin_panel_at = 0;
        return;
    }
    unsigned rows = cursor < chain.count() ? effect_rows(chain.slots[cursor], n) : 2; // a KEY row after (roadmap P4)
    c.ui.fx_row[at] = uint8_t(std::clamp(int(std::min<unsigned>(c.ui.fx_row[at], rows - 1)) + way, 0, int(rows) - 1));
}
} // namespace

const PageSpec rack_page {draw, play_pad, wheel, push, soft, nudge};
const PageSpec bus_page {draw, play_pad, wheel, push, soft, nudge};
const PageSpec master_page {draw, play_pad, wheel, push, soft, nudge};
const PageSpec pad_fx_page {draw, play_pad, wheel, push, soft, nudge};

} // namespace melodee
