// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// EDIT (Push on Keys): one engine page at a time, its curve filling the
// picture with every parameter named at the point it moves, in its knob's
// colour while it is on the row the pots hold, dim otherwise (the rest of the
// page's parameters listed small at the top right), so nothing hides behind
// a row count. B1 and B3 name the neighbouring pages and go there, B2 MAP
// (its dots the page in view) opens the page map; the wheel moves between
// rows of three; K1-K3 edit the row in view.
#include "melodee/engine/dsp.h"
#include "melodee/engine/midi_engine.h"
#include "melodee/model/mod_matrix.h"
#include "melodee/model/scales.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/shell/midi_track_jobs.h"
#include "melodee/ui/widgets.h"
#include <algorithm>
#include <cmath>
#include <complex>

namespace melodee {
namespace {
using namespace ui;

// The picture's box at device size: the whole picture, a line left under it for the axis.
constexpr float px = zone::pad, py = zone::picture_y + 4, pw = zone::width - 2 * zone::pad, ph = 124;

const EditPage& current_page(const Snapshot& s, unsigned& index) {
    const auto& info = engine_info(s.track().engine);
    index = s.ui.edit_page[s.ui.track] % edit_page_count(info);
    return edit_page_at(info, index);
}

// Which knob shade a parameter wears: its card's when on the row in view.
juce::Colour shade_of(const Snapshot& s, const EditPage& page, int id) {
    const auto& row = page.rows[s.ui.edit_row[s.ui.track] % std::max(page.row_count(), 1u)];
    for (unsigned n = 0; n < 3; ++n) if (row[n] == id) return knob_shade(s.ui.track, n);
    return colour::mid;
}

// Every parameter of the page as a labelled gauge: the default picture.
void bars_picture(Paint& p, const Snapshot& s, const EditPage& page) {
    const auto& info = engine_info(s.track().engine);
    const auto& v = s.track().params;
    unsigned rows = std::max(page.row_count(), 1u);
    float row_h = std::min(34.0f, (ph + 10) / rows), cw = (pw - 16) / 3;
    for (unsigned r = 0; r < rows; ++r)
        for (unsigned n = 0; n < 3; ++n) {
            int id = page.rows[r][n];
            if (id < 0) continue;
            const auto& param = info.params[id];
            float x = px + n * (cw + 8), y = py + r * row_h;
            int value = v[size_t(id)];
            if (s.track().engine == EngineKind::Midi && id == midi_track::PatternProgram0) // the pattern in hand's own (Pattern::program)
                value = s.sketches[s.ui.track % track_count][s.track().pattern % pattern_count].program;
            auto shown = show(param, value);
            juce::String label = param.label;
            if (s.track().engine == EngineKind::Midi) { // a slot by what it sends; this pattern's program (roadmap P4)
                if (midi_track::is_cc(id)) label = midi_track::slot_label(v[size_t(midi_track::slot_address(id - midi_track::Cc1))]);
                else if (midi_track::slot_of_address(id) >= 0) label = "SLOT " + juce::String(midi_track::slot_of_address(id) + 1);
                else if (id == midi_track::PatternProgram0) label = "P" + juce::String(s.track().pattern % pattern_count + 1) + " PROG";
            }
            p.text(label, x, y, cw, 10, caps_text(shade_of(s, page, id)));
            p.text(shown.value + (shown.unit.empty() ? "" : " " + shown.unit), x, y + 10, cw, 13, plain(colour::sec, type::small, Weight::Medium));
            gauge(p, x, y + 25, cw, gauge_of(param, value), shade_of(s, page, id));
        }
}


// A MIDI track's PADS page (roadmap P4, M3): the bank's sixteen pads as on the panel, each its note and its General MIDI
// name on Drum, the last one hit raised; on Keys its keys.
void drum_map_picture(Paint& p, const Snapshot& s) {
    const auto& t = s.track();
    auto c = colour_of(s);
    float cw = (pw - 3 * 4) / 4, ch = (ph - 3 * 4) / 4;
    for (unsigned pad = 0; pad < 16; ++pad) {
        unsigned col = pad % 4, row = 3 - pad / 4; // pad 1 bottom left
        float x = px + col * (cw + 4), y = py + row * (ch + 4);
        int key = pad_note(s.project, t, pad).key;
        bool last = drum_pads(t) && t.last_pad % sampler_pads == 16 * drum_bank(t) + pad;
        p.fill(x, y, cw, ch, last ? c.withAlpha(0.25f) : colour::surf, 3);
        if (last) p.ring(x, y, cw, ch, c, 3, 1.2f);
        const char* gm = drum_pads(t) ? gm_drum_name(unsigned(key)) : nullptr;
        juce::String name = gm ? juce::String(gm) : juce::String(note_name(key));
        auto style = plain(last ? colour::text : colour::sec, type::small, Weight::Medium);
        p.text(name, x + 3, y + 2, cw - 6, ch / 2, p.fit(name, style, cw - 6, type::floor));
        p.text(juce::String(key), x + 3, y + ch / 2, cw - 6, ch / 2 - 2, plain(colour::mid, type::small));
    }
}

// A MIDI track's DEVICE page (docs/MIDI_TRACKS.md; round 2, midi-track A): the route as Stage draws it (common.cpp).
void route_picture(Paint& p, const Snapshot& s) { midi_route(p, s, px, py, pw, ph); }

// MOD MATRIX: eight routes, all on one screen; the wheel picks the route.
void mod_picture(Paint& p, const Snapshot& s) {
    const auto& mods = s.track().mods;
    auto c = colour_of(s);
    unsigned current = s.ui.edit_row[s.ui.track] % mod_routes;
    float y = zone::picture_y + 1, rh = 17;
    for (unsigned r = 0; r < mod_routes; ++r, y += rh + 1) {
        const auto& route = mods[r];
        bool on = r == current;
        float x = zone::pad;
        if (on && route.used()) p.fill(x, y, zone::width - 2 * zone::pad, rh, colour::raised, 4);
        if (on) {
            p.g.saveState();
            p.g.reduceClipRegion(juce::Rectangle<int>(int(x), int(y), 3, int(rh)));
            p.fill(x, y, 8, rh, c, 4);
            p.g.restoreState();
        }
        p.text(juce::String(r + 1), x + 9, y, 10, rh, plain(route.used() ? c : colour::dim, type::small, Weight::Bold));
        if (!route.used()) { p.text("Empty", x + 24, y, 100, rh, plain(colour::dim, type::small, Weight::Medium)); continue; }
        p.text(mod_source_name(ModSource(route.source)), x + 24, y, 75, rh,
               plain(on ? knob_shade(s.ui.track, 0) : colour::sec, type::small, Weight::Medium));
        juce::Path arrow;
        float ax = x + 107, ay = y + rh / 2;
        arrow.startNewSubPath(ax, ay); arrow.lineTo(ax + 10, ay);
        arrow.startNewSubPath(ax + 7, ay - 3); arrow.lineTo(ax + 10, ay); arrow.lineTo(ax + 7, ay + 3);
        p.g.setColour(colour::mid);
        p.g.strokePath(arrow, juce::PathStrokeType(1, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        // A destination this engine has none of (a route kept from another engine) is drawn dim; it does nothing.
        const char* dest = mod_dest_label(engine_info(s.track().engine), ModDest(route.destination));
        p.text(dest ? dest : mod_dest_name(ModDest(route.destination)), x + 123, y, 90, rh,
               plain(!dest ? colour::dim : on ? knob_shade(s.ui.track, 1) : colour::sec, type::small, Weight::Medium));
        float bx = x + 214, bw = 55, amount = route.amount / float(mod_amount_max);
        p.fill(bx, ay - 1, bw, 2, colour::line, 1);
        float from = std::min(0.0f, amount), to = std::max(0.0f, amount);
        p.fill(bx + bw / 2 + from * bw / 2, ay - 1, (to - from) * bw / 2, 2, knob_shade(s.ui.track, 2), 1);
        p.fill(bx + bw / 2 - 0.5f, ay - 2.5f, 1, 5, colour::mid);
        juce::String value = (route.amount > 0 ? "+" : route.amount < 0 ? juce::String::fromUTF8("\xe2\x88\x92") : juce::String())
            + juce::String(std::abs(int(route.amount)));
        p.text(value, x + 270, y, zone::width - 2 * zone::pad - 277, rh, plain(on ? colour::text : colour::sec, type::small, Weight::Medium),
               juce::Justification::centredRight);
    }
}

// PAGE MAP (B2): every page of the engine, two columns; the wheel moves, Push jumps.
void page_map(Paint& p, const Snapshot& s, const EngineInfo& info) {
    auto c = colour_of(s);
    unsigned count = edit_page_count(info), per_column = (count + 1) / 2;
    float cw = (zone::width - 2 * zone::pad - 4) / 2, rh = 19;
    for (unsigned n = 0; n < count; ++n) {
        float x = zone::pad + (n / per_column) * (cw + 4), y = zone::picture_y + 1 + (n % per_column) * (rh + 2);
        bool on = n == s.ui.map_cursor;
        if (on) p.fill(x, y, cw, rh, c, 4);
        p.text(juce::String(n + 1), x + 4, y, 14, rh, plain(on ? colour::bg : colour::dim, type::small, Weight::SemiBold));
        p.text(edit_page_at(info, n).title, x + 23, y, cw - 26, rh, plain(on ? colour::bg : colour::sec, type::small, Weight::Medium));
    }
}

void draw(Paint& p, const Snapshot& s) {
    const auto& info = engine_info(s.track().engine);
    auto c = colour_of(s);
    Header h = header_of(s);
    if (s.track().engine == EngineKind::Plugin) { // the plug-in's own parameters, three at a time; B1 another from the card, B2 its presets,
                                                  // B3 TRANSPORT (it hears Play and Stop: its LED)
        const auto& panel = s.ui.plugin_panel;
        bool ours = panel.entry == s.track().plugin;
        if (ours && panel.status == PluginStatus::Ready) // the share of each audio block it takes
            h.left.push_back(pill(Icon::Chip, juce::String(panel.load / 10) + " %", panel.load > 700 ? colour::amber : colour::mid));
        h.right.push_back(tag_of(s, s.ui.track));
        bool presets = ours && panel.programs > 0, chosen = s.track().plugin < max_plugins;
        SoftKeys keys {{"PLUG-INS", presets ? "PRESETS" : "", chosen ? "TRANSPORT" : ""}, -1, c};
        if (chosen) { keys.toggles = 4; keys.lit = s.project.plugins[s.track().plugin].transport ? 4 : 0; }
        frame(p, s, keys, h);
        if (s.track().plugin >= max_plugins) { // none yet: B1 picks one from the card
            icon(p, Icon::Plug, zone::width / 2.0f - 14, zone::picture_y + 40, 28, colour::dim, 1.4f);
            p.text("NONE", 0, zone::picture_y + 76, zone::width, 14, caps_text(colour::dim), juce::Justification::centred);
            return;
        }
        draw_plugin_view(p, s, s.track().plugin, zone::picture_y);
        return;
    }
    if (!info.page_count) {
        h.right.push_back(tag_of(s, s.ui.track));
        frame(p, s, {{"", "", ""}, -1, c}, h);
        p.text("Nothing to edit yet", zone::pad, zone::picture_y, zone::width - 12, 40, plain(colour::dim, type::small, Weight::Medium),
               juce::Justification::centred);
        return;
    }
    unsigned index = 0;
    const auto& page = current_page(s, index);
    unsigned count = edit_page_count(info);
    const auto& before = edit_page_at(info, index + count - 1);
    const auto& after = edit_page_at(info, index + 1);
    h.left.push_back(pill(juce::String(page.title).toUpperCase(), c)); // the page in view: no title row under it
    if (page.picture == Picture::Mod && !s.ui.map_open) {
        unsigned used = 0;
        for (const auto& r : s.track().mods) used += r.used();
        h.left.push_back(pill(Icon::Modulation, juce::String(used) + " / " + juce::String(mod_routes)));
    }
    h.right.push_back(tag_of(s, s.ui.track));
    // Either side, the pages before and after; with two pages (a MIDI track's) one key, B3, goes to the other.
    SoftKeys keys {{count > 2 ? before.key : "", "MAP", count > 1 ? after.key : ""}, s.ui.map_open ? 1 : -1, c};
    if (count > 2) keys.glyphs[0] = Icon::ChevronLeft;
    if (count > 1) keys.after[2] = Icon::ChevronRight;
    keys.dots[1] = uint8_t(std::min(count, 16u)); keys.dot_at[1] = uint8_t(index);
    frame(p, s, keys, h);
    if (s.ui.map_open) { page_map(p, s, info); return; }
    if (page.picture == Picture::Mod) { mod_picture(p, s); return; }
    switch (page.picture) {
    case Picture::Bars: bars_picture(p, s, page); break;
    case Picture::Route: route_picture(p, s); break;
    case Picture::Pads: drum_map_picture(p, s); break;
    case Picture::Mod: break;
    }
}

unsigned page_count(Context& c) { return std::max(edit_page_count(engine_info(c.track().engine)), 1u); }

void soft(Context& c, int key) {
    if (c.track().engine == EngineKind::Plugin) {
        if (key == 0) c.open_plugin_picker(UiState::PluginTarget::Instrument);
        if (key == 1 && c.ui.plugin_panel.entry == c.track().plugin && c.ui.plugin_panel.programs > 0) c.open_plugin_presets(c.track().plugin);
        if (key == 2) c.toggle_plugin_transport(c.track().plugin);
        return;
    }
    unsigned count = page_count(c), t = c.ui.track;
    if (key == 1) { // B2: the page map, starting on the page in view
        c.ui.map_open = !c.ui.map_open;
        c.ui.map_cursor = uint8_t(c.ui.edit_page[t] % count);
        return;
    }
    if (key == 0 && count <= 2) return; // two pages: B3 alone goes to the other
    c.ui.map_open = false;
    c.ui.edit_page[t] = uint8_t((c.ui.edit_page[t] + (key == 0 ? count - 1 : 1)) % count);
    c.ui.edit_row[t] = 0;
}

} // namespace

// A PLUGIN track's rows of parameters (the panel knows how many).
void plugin_rows(Context& c, int delta) {
    unsigned entry = c.track().plugin;
    if (entry >= max_plugins || c.ui.plugin_panel.entry != entry || !c.ui.plugin_panel.rows) return;
    auto& row = c.ui.plugin_row[entry];
    row = uint16_t(std::clamp<int>(row + delta, 0, c.ui.plugin_panel.rows - 1));
    c.ui.plugin_panel.row = row;
    c.plugin_panel_at = 0; // its cards again at once
}

namespace {

void wheel(Context& c, int delta) {
    if (c.track().engine == EngineKind::Plugin) { plugin_rows(c, delta); return; }
    const auto& info = engine_info(c.track().engine);
    if (!info.page_count) return;
    unsigned t = c.ui.track, count = edit_page_count(info);
    if (c.ui.map_open) {
        c.ui.map_cursor = uint8_t(std::clamp<int>(c.ui.map_cursor + delta, 0, int(count) - 1));
        return;
    }
    unsigned rows = is_mod_page(info, c.ui.edit_page[t]) ? mod_routes : std::max(edit_page_at(info, c.ui.edit_page[t]).row_count(), 1u);
    c.ui.edit_row[t] = uint8_t(std::clamp<int>(c.ui.edit_row[t] + delta, 0, int(rows) - 1));
}

void push(Context& c) {
    const auto& info = engine_info(c.track().engine);
    if (!info.page_count) return;
    unsigned t = c.ui.track;
    if (c.ui.map_open) { // jump there
        c.ui.edit_page[t] = c.ui.map_cursor;
        c.ui.edit_row[t] = 0;
        c.ui.map_open = false;
        return;
    }
    if (c.track().engine == EngineKind::Midi && std::string(edit_page_at(info, c.ui.edit_page[t]).key) == "SYSEX") { // roadmap P4: SEND
        send_sysex(c, t);
        return;
    }
    if (is_mod_page(info, c.ui.edit_page[t])) {
        // Push on an empty row starts a route: the LFO into the cutoff, nothing yet.
        unsigned r = c.ui.edit_row[t] % mod_routes;
        if (!c.track().mods[r].used()) c.set_route(r, {uint8_t(ModSource::Lfo), uint8_t(ModDest::Cutoff), 0});
    }
}

// Minus / Plus: the octave, or on a PLUGIN track its rows as the wheel.
void nudge(Context& c, int way) {
    if (c.track().engine == EngineKind::Plugin) plugin_rows(c, way);
    else octave(c, way);
}
} // namespace

const PageSpec edit_page {draw, play_pad, wheel, push, soft, nudge};

} // namespace melodee
