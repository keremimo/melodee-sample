// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// PAD EFFECTS (B1 from Track on a sampler track), TRACK EFFECTS (Knob FX),
// BUS (B3 from Sends) and MASTER (B3 from Bus): one page shape for every
// stop along the signal. A pad's chain is the last pad hit's, four slots. Up to six effects in a row;
// the wheel picks a device, the three pots hold three of its parameters and
// Minus / Plus the other row. Push on a device: change, bypass, move,
// remove. Push on + ADD opens the picker: 41 effects in six families, Push
// enters a family, B3 adds, B1 backs out. A bus or the master has no track,
// so it is drawn in neutral.
#include "melodee/engine/effect_info.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/pages/signal_flow.h"
#include "melodee/shell/bindings.h"
#include "melodee/ui/widgets.h"
#include <algorithm>

namespace melodee {
namespace {
using namespace ui;
constexpr unsigned device_actions = 5;

juce::String bus_name(unsigned bus) { return juce::String("BUS ") + char('A' + bus % bus_count); }
unsigned bus_of(const Project& project, unsigned track) { return project.tracks[track % track_count].mix.bus % bus_count; }

void picker(Paint& p, const Snapshot& s, juce::Colour c) {
    constexpr float x = zone::pad, y = zone::picture_y + 1.5f, w = zone::width - 2 * zone::pad, h = 143;
    for (int n = 1; n <= 4; ++n) p.fill(x - n, y + n * 2, w + 2 * n, h, juce::Colours::black.withAlpha(0.06f), 6 + n);
    p.fill(x, y, w, h, colour::surf, 6);
    p.ring(x, y, w, h, colour::line, 6);
    for (unsigned f = 0; f < unsigned(Family::Count); ++f) {
        FamilyList list {};
        unsigned count = family_effects(Family(f), list);
        bool on = f == s.ui.picker_family;
        float fy = y + 6 + f * 22;
        if (on) p.fill(x + 6, fy, 100, 20, colour::raised, 4);
        p.text(family_name(Family(f)), x + 12, fy, 70, 20, plain(on ? colour::text : colour::mid, 10, on ? Weight::SemiBold : Weight::Medium));
        p.text(juce::String(count), x + 12, fy, 88, 20, caps_text(on ? c : colour::dim, 7), juce::Justification::centredRight);
    }
    p.fill(x + 112, y + 6, 1, h - 12, colour::line);
    FamilyList list {};
    unsigned count = family_effects(Family(s.ui.picker_family % unsigned(Family::Count)), list);
    float cw = (w - 124 - 9) / 2;
    for (unsigned n = 0; n < count; ++n) {
        const auto& info = effect_info(list[n]);
        bool on = s.ui.picker_items && n == s.ui.picker_item;
        float ix = x + 118 + (n / 6) * (cw + 3), iy = y + 6 + (n % 6) * 22;
        if (on) p.fill(ix, iy, cw, 20, info.built ? c : colour::raised, 4);
        p.text(info.name, ix + 7, iy, cw - 10, 20,
               plain(on && info.built ? colour::bg : info.built ? colour::sec : colour::dim, 10.5f, Weight::Medium));
    }
}

void device_card(Paint& p, const Snapshot& s, const EffectSlot& slot, unsigned index, float x, float y, float w, float h, bool on, juce::Colour c) {
    p.fill(x, y, w, h, on ? colour::raised : colour::quiet, 4);
    const auto& info = effect_info(EffectKind(slot.kind));
    juce::String head = juce::String(index + 1) + juce::String::fromUTF8(" \xc2\xb7 ") + juce::String(family_name(info.family)).toUpperCase();
    p.text(head, x + 6, y + 5, w - 12, 9, caps_text(on ? c : colour::mid, 6.5f));
    auto name = p.fit(info.name, plain(slot.bypass ? colour::dim : on ? colour::text : colour::sec, 12, Weight::SemiBold), w - 12, 8);
    p.text(info.name, x + 6, y + 15, w - 12, 16, name);
    if (slot.bypass) p.text("BYPASS", x + 6, y + 31, w - 12, 9, caps_text(colour::dim, 6.5f));
    float bx = x + 6, bottom = y + h - 6, bw = (w - 12 - 10 * 2.5f) / 11;
    for (unsigned n = 0; n < 11; ++n) {
        float bh = std::max(2.0f, info.picture[n] * 0.31f);
        p.fill(bx + n * (bw + 2.5f), bottom - bh, bw, bh, info.picture[n] ? c.withAlpha(on && !slot.bypass ? 1.0f : 0.45f) : colour::line, 1.5f);
    }
    (void)s;
}

void draw(Paint& p, const Snapshot& s) {
    unsigned n = page_chain(s.project, s.ui), bus = bus_of(s.project, s.ui.track);
    const auto& chain = s.project.chain(n);
    bool pad = s.ui.page == PageId::PadFx, track = s.ui.page == PageId::Rack || pad, kit = s.track().engine == EngineKind::Sampler;
    auto c = track ? colour_of(s) : colour::text;
    unsigned count = chain.count(), cursor = std::min<unsigned>(s.ui.fx_cursor[UiState::cursor_of(n)], count);
    Header h = header_of(s);
    SoftKeys keys {{"", "TRACK", "SENDS"}, 1, c};
    juce::String back = juce::String::fromUTF8("\xe2\x80\xb9 "), on = juce::String::fromUTF8(" \xe2\x80\xba");
    if (pad && !kit) {
        frame(p, s, {{"", "PAD", "TRACK" + on}, 1, c}, h);
        p.text("Pad effects are for a sampler track", zone::pad, zone::picture_y + 60, zone::width - 2 * zone::pad, 16,
               plain(colour::dim, 11), juce::Justification::centred);
        return;
    }
    if (pad) {
        unsigned last = s.track().last_pad % sampler_pads;
        char label[4]; pad_label(last, label);
        juce::String name = "PAD " + juce::String(label);
        if (s.track().pads[last].used()) name += juce::String::fromUTF8(" \xc2\xb7 ") + pad_name(s, last, false).toUpperCase();
        keys.labels = {"", "PAD", "TRACK" + on};
        h.left.push_back(pill(name, c));
        h.right.push_back(tag_of(s, s.ui.track));
    } else if (track) { keys.labels = {kit ? back + "PAD" : juce::String(), "TRACK", "SENDS" + on}; h.right.push_back(tag_of(s, s.ui.track)); }
    else if (s.ui.page == PageId::Bus) {
        keys.labels = {back + "SENDS", bus_name(bus), "MASTER" + on};
        h.left.push_back(pill("RETURN " + juce::String(s.project.buses[bus].level) + " %"));
        h.right.push_back(label(bus_name(bus), colour::mid));
        if (count) h.right.push_back(label(effect_info(EffectKind(chain.slots[0].kind)).name, colour::text));
    } else {
        keys.labels = {back + bus_name(bus), "MASTER", ""};
        h.right.push_back(label("MASTER", colour::mid));
    }
    if (s.ui.picker_open) {
        keys = SoftKeys {{"BACK", "", s.ui.picker_replaces ? "CHANGE" : "ADD"}, 2, colour::text, {KeyIcon::Back, KeyIcon::None, KeyIcon::Check}};
        h.left.clear();
        h.left.push_back(pill(juce::String(effect_count()) + " EFFECTS"));
        h.left.push_back(pill("SLOT " + juce::String(cursor + 1), c));
    }
    frame(p, s, keys, h);
    if (s.ui.picker_open) { picker(p, s, c); return; }
    signal_flow(p, pad ? Stop::Pad : track ? Stop::Track : s.ui.page == PageId::Bus ? Stop::Bus : Stop::Master, c, kit);

    // Four cards across; the window follows the cursor through six slots and + ADD.
    unsigned shown = std::min<unsigned>(chain.size, count + 1), first = cursor > 3 ? cursor - 3 : 0;
    float y = zone::picture_y + 18, ch = 100, gap = 4, cw = (zone::width - 2 * zone::pad - 3 * gap) / 4;
    for (unsigned k = 0; k < 4; ++k) {
        unsigned slot = first + k;
        float x = zone::pad + k * (cw + gap);
        if (slot < count) { device_card(p, s, chain.slots[slot], slot, x, y, cw, ch, slot == cursor, c); continue; }
        p.fill(x, y, cw, ch, slot == cursor ? colour::raised : colour::quiet, 4);
        if (slot < shown) { // + ADD
            auto style = plain(slot == cursor ? colour::text : colour::dim, 22);
            p.text("+", x, y + 30, cw, 24, style, juce::Justification::centred);
            p.text("ADD", x, y + 55, cw, 9, caps_text(slot == cursor ? colour::text : colour::dim, 6.5f), juce::Justification::centred);
        }
    }
    // The device in view, its row of parameters, and how full the chain is.
    float fy = y + ch + 5;
    juce::String name = cursor < count ? juce::String(effect_info(EffectKind(chain.slots[cursor].kind)).name)
                                       : count < chain.size ? juce::String("Add an effect") : juce::String("Chain full");
    float nw = p.text(name, zone::pad, fy, 160, 20, plain(colour::text, 14, Weight::SemiBold));
    if (cursor < count) {
        unsigned row = s.ui.fx_row[UiState::cursor_of(n)] % 2;
        const auto& info = effect_info(EffectKind(chain.slots[cursor].kind));
        bool second = info.params[3].label != nullptr;
        if (second) {
            float lw = p.text("ROW " + juce::String(row + 1) + " / 2", zone::pad + nw + 8, fy, 70, 20, caps_text(colour::mid, 7));
            page_dots(p, zone::pad + nw + 8 + lw + 6, fy + 10, 2, int(row), c);
        }
    }
    juce::String slots = juce::String(count) + " OF " + juce::String(chain.size) + " SLOTS";
    auto style = caps_text(colour::mid);
    float pw = p.width(slots, style) + 9;
    p.fill(zone::width - zone::pad - pw, fy + 3.5f, pw, 12.5f, colour::surf, 3);
    p.text(slots, zone::width - zone::pad - pw, fy + 3.5f, pw, 12.5f, style, juce::Justification::centred);

    if (s.ui.sheet_open && cursor < count) {
        const auto& slot = chain.slots[cursor];
        MenuSheet m;
        m.title = effect_info(EffectKind(slot.kind)).name; m.detail = "SLOT " + juce::String(cursor + 1); m.accent = c;
        m.rows[0] = {"Change", "", false, false, false};
        m.rows[1] = {"Bypass", slot.bypass ? "On" : "Off", slot.bypass, false, false};
        m.rows[2] = {"Move left", "", false, false, cursor == 0};
        m.rows[3] = {"Move right", "", false, false, cursor + 1 >= count};
        m.rows[4] = {"Remove", "", false, true, false};
        m.count = device_actions; m.current = s.ui.sheet_row % device_actions;
        p.dim_zone(zone::picture_y, zone::picture_h, 0.6f);
        menu_sheet(p, m);
    }
}

unsigned chain_here(Context& c) { return page_chain(c.project, c.ui); }

void add_from_picker(Context& c) {
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
        if (!ui.picker_items) { ui.picker_family = uint8_t(std::clamp<int>(ui.picker_family + delta, 0, int(Family::Count) - 1)); return; }
        FamilyList list {};
        unsigned count = family_effects(Family(ui.picker_family), list);
        ui.picker_item = uint8_t(std::clamp<int>(ui.picker_item + delta, 0, int(count) - 1));
        return;
    }
    if (ui.sheet_open) { ui.sheet_row = uint8_t(std::clamp<int>(ui.sheet_row + delta, 0, int(device_actions) - 1)); return; }
    unsigned n = chain_here(c), at = UiState::cursor_of(n);
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
    if (ui.sheet_open) {
        switch (ui.sheet_row % device_actions) {
        case 0: ui.picker_open = true; ui.picker_items = false; ui.picker_replaces = true; break;
        case 1: c.toggle_bypass(n, cursor); return; // stays open to see it change
        case 2: c.move_effect(n, cursor, -1); if (cursor) ui.fx_cursor[at] = uint8_t(cursor - 1); break;
        case 3: c.move_effect(n, cursor, 1); if (cursor + 1 < chain.count()) ui.fx_cursor[at] = uint8_t(cursor + 1); break;
        case 4: c.set_effect(n, cursor, EffectKind::None); break;
        }
        ui.sheet_open = false;
        return;
    }
    if (cursor < chain.count()) { ui.sheet_open = true; ui.sheet_row = 0; return; }
    if (cursor < chain.size) { ui.picker_open = true; ui.picker_items = false; ui.picker_replaces = false; }
}

void soft(Context& c, int key) {
    auto& ui = c.ui;
    if (ui.picker_open) {
        if (key == 0) { if (ui.picker_items) ui.picker_items = false; else ui.picker_open = false; }
        if (key == 2) { if (ui.picker_items) add_from_picker(c); else { ui.picker_items = true; ui.picker_item = 0; } }
        return;
    }
    if (ui.sheet_open) { if (key == 0) ui.sheet_open = false; return; }
    bool kit = c.track().engine == EngineKind::Sampler;
    PageId before = ui.page == PageId::PadFx ? PageId::PadFx : ui.page == PageId::Rack ? (kit ? PageId::PadFx : PageId::Rack)
                  : ui.page == PageId::Bus ? PageId::Sends : PageId::Bus;
    PageId after = ui.page == PageId::PadFx ? PageId::Rack : ui.page == PageId::Rack ? PageId::Sends : PageId::Master;
    if (key == 0 && before != ui.page) c.open(before);
    if (key == 2 && after != ui.page) c.open(after);
}

// Minus / Plus: the device's other row of parameters.
void nudge(Context& c, int way) {
    unsigned at = UiState::cursor_of(chain_here(c));
    c.ui.fx_row[at] = uint8_t(std::clamp(c.ui.fx_row[at] + way, 0, 1));
}
} // namespace

const PageSpec rack_page {draw, play_pad, wheel, push, soft, nudge};
const PageSpec bus_page {draw, play_pad, wheel, push, soft, nudge};
const PageSpec master_page {draw, play_pad, wheel, push, soft, nudge};
const PageSpec pad_fx_page {draw, play_pad, wheel, push, soft, nudge};

} // namespace melodee
