// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// PATTERNS (the SEQUENCE hub's B1, SEQ's home): the selected track's sixteen patterns of the bank in view, drawn
// as the pads sit (pad 1 bottom left, 13 top left), so a cell is the pad that launches it. The pads launch them
// as Seq held does (shell/seq_layer.h): at the next bar (stopped, at once), hold one and tap another to copy,
// Erase + pad clears (asked first); they light as the cells. Pad Bank steps the bank (A-H), as Seq held + Pad Bank.
// In each cell: the number, ▶ playing, NEXT queued, a tinted cell for notes, a dot a track in view for the other
// tracks (a ring: notes there; filled: playing it), an arrow for a follow action, an amber dot for a scene tempo.
// Right: the eight tracks in view, each with the pattern it plays and how far along it is.
// The wheel moves the cursor (amber) over the cells in pad order: cell n is scene n too, and Push launches it on
// every track; Shift + wheel moves the playhead by Q (stopped: where Shift + Play carries on from). Minus / Plus move the track. K1-K3: the pattern's length, Q, swing (Shift: any bars, time signature,
// the metronome's level). B1-B3 PATTERNS / ROLL / MOTION; Shift + B1 SCENE (K1 the cursor's scene tempo, Push
// names it), Shift + B2 FOLLOW (the pattern's follow action, Shift + K1 OR, Push puts it on its column), Shift + B3
// VARY (a variation of the pattern into the next empty one; the wheel picks, Push or B3 makes it).
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/model/generators.h"
#include "melodee/shell/step_lane.h"
#include "melodee/ui/widgets.h"
#include <algorithm>

namespace melodee {
namespace {
using namespace ui;

constexpr float cell_w = 47, cell_h = 32.5f, cell_gap = 3, grid_x = zone::pad, grid_y = zone::picture_y + 3;
constexpr float list_x = 214, list_w = zone::width - zone::pad - list_x;

// The cursor: a cell of the bank in view (UiState::scene keeps its place in the bank).
unsigned cursor_of(const UiState& ui) { return ui.pattern_bank % pattern_banks * pattern_pads + ui.scene % pattern_pads; }

void draw_cell(Paint& p, const Snapshot& s, unsigned k, unsigned first) {
    unsigned t = s.ui.track % track_count, n = s.ui.pad_pattern(k);
    float x = grid_x + (k % 4) * (cell_w + cell_gap), y = grid_y + (3 - k / 4) * (cell_h + cell_gap);
    auto c = track_colour(t);
    const auto& track = s.project.tracks[t];
    const auto& sketch = s.sketches[t][n];
    bool notes = track.used() && sketch.notes;
    unsigned live = s.playing ? s.playing_patterns[t] : track.pattern;
    bool playing = notes && n == live && (!s.playing || s.positions[t] >= 0);
    bool queued = track.used() && s.playing && n == track.pattern && n != live;
    juce::Colour ink = notes ? colour::text : colour::dim;
    if (playing) {
        p.fill(x, y, cell_w, cell_h, c, 4);
        ink = colour::bg;
    } else if (queued) {
        bool blink = (s.now / 150) % 2;
        p.fill(x, y, cell_w, cell_h, c.withAlpha(blink ? 0.35f : 0.15f), 4);
        p.ring(x + 0.75f, y + 0.75f, cell_w - 1.5f, cell_h - 1.5f, c, 4, 1.5f);
    } else if (notes) {
        p.fill(x, y, cell_w, cell_h, c.withAlpha(0.22f), 4);
    } else {
        p.fill(x, y, cell_w, cell_h, colour::quiet, 4);
    }
    auto number = juce::String(n + 1);
    auto style = plain(ink, type::small, Weight::Bold);
    p.text(number, x + 5, y + 3, 24, 13, style);
    float after = x + 5 + p.width(number, style) + 3;
    if (s.project.scenes[n].tempo) p.circle(after + 1.5f, y + 9.5f, 1.6f, playing ? colour::bg : colour::amber); // the scene's tempo
    if (playing) icon(p, Icon::Play, x + cell_w - 12, y + 4, 8, colour::bg, 1.4f);
    else if (queued) p.text("NEXT", x + cell_w - 34, y + 3, 30, 13, plain(c, type::small, Weight::Bold), juce::Justification::centredRight);
    else if (notes && sketch.follows) icon(p, Icon::ArrowRight, x + cell_w - 12, y + 4, 8, colour::sec, 1.4f); // a follow action
    if (notes && sketch.follows && playing) icon(p, Icon::ArrowRight, x + cell_w - 22, y + 4, 8, colour::bg, 1.4f);
    // The other tracks in view: a dot each, a ring for notes in this pattern, filled while it plays it.
    for (unsigned r = 0; r < 8; ++r) {
        unsigned o = first + r;
        if (o == t) continue;
        const auto& other = s.project.tracks[o];
        if (!other.used() || !s.sketches[o][n].notes) continue;
        bool on = (s.playing ? s.playing_patterns[o] == n && s.positions[o] >= 0 : other.pattern == n);
        float dx = x + 6 + r * 5.3f, dy = y + cell_h - 6.5f;
        auto dc = playing ? colour::bg : track_colour(o);
        if (on) p.circle(dx, dy, 1.9f, dc);
        else p.ring(dx - 1.6f, dy - 1.6f, 3.2f, 3.2f, dc, 1.6f, 0.9f);
    }
    if (n == cursor_of(s.ui)) p.ring(x - 1.5f, y - 1.5f, cell_w + 3, cell_h + 3, colour::amber, 5, 1.5f);
    if (s.ui.pattern_held == int(n)) p.ring(x - 1.5f, y - 1.5f, cell_w + 3, cell_h + 3, colour::text, 5, 2);
}

// The eight tracks in view: number, ▶ the pattern it plays (– none), how far along.
void draw_tracks(Paint& p, const Snapshot& s, unsigned first) {
    float rh = (zone::picture_h - 4) / 8.0f, y0 = grid_y - 1;
    for (unsigned r = 0; r < 8; ++r) {
        unsigned t = first + r;
        const auto& track = s.project.tracks[t];
        auto c = track_colour(t);
        float y = y0 + r * rh, cy = y + rh / 2;
        bool selected = t == s.ui.track;
        if (selected) { p.fill(list_x, y + 1, list_w, rh - 2, colour::surf, 3); p.fill(list_x + 2, y + 2.5f, 15, rh - 5, c, 3); }
        p.text(juce::String(t + 1), list_x + 2, y, 15, rh, plain(selected ? colour::bg : c, type::small, Weight::Bold), juce::Justification::centred);
        if (!track.used()) continue;
        unsigned live = s.playing ? s.playing_patterns[t] : track.pattern;
        bool stopped = s.playing && s.positions[t] < 0;
        if (!s.sketches[t][live].notes || stopped) {
            p.text(juce::String::fromUTF8("\xe2\x80\x93"), list_x + 22, y, 20, rh, plain(colour::dim, type::small));
            continue;
        }
        icon(p, Icon::Play, list_x + 21, cy - 3.5f, 7, c, 1.2f);
        p.text(juce::String(live + 1), list_x + 31, y, 22, rh, plain(colour::text, type::small, Weight::SemiBold));
        float bx = list_x + 54, bw = list_w - 58;
        p.fill(bx, cy - 1, bw, 2, colour::line, 1);
        uint32_t length = std::max<uint32_t>(s.sketches[t][live].length, 1);
        float done = s.playing && s.positions[t] >= 0 ? float(uint32_t(s.positions[t]) % length) / length : 0;
        if (done > 0) p.fill(bx, cy - 1, bw * done, 2, c, 1);
    }
}

void draw(Paint& p, const Snapshot& s) {
    unsigned first = s.ui.track / 8 * 8, cursor = cursor_of(s.ui);
    auto c = colour_of(s);
    Header h = header_of(s);
    h.left.push_back(bank_pill(s));
    const auto& scene = s.project.scenes[cursor];
    if (s.ui.patterns_cards == 2) // FOLLOW: the pattern in hand's
        h.left.push_back(chip("T" + juce::String(s.ui.track + 1) + " P" + juce::String(s.track().pattern % pattern_count + 1), c));
    else
        {
        juce::String id = "S" + juce::String(cursor + 1), name(scene.name);
        h.left.push_back(chip(name.isEmpty() || name == id ? id : id + " " + name, colour::amber)); // a name of its own beside its number
    }
    // Rec Quantize's grid (Shift + Pad 10), after the scene: the header leaves it out when there is no room.
    h.left.push_back(pill(Icon::Magnet, s.ui.settings.rec_quantize ? juce::String(division_name(grid_division(s.ui.settings.rec_grid, s.pattern.division)))
                                                                 : juce::String("OFF"),
                          s.ui.settings.rec_quantize ? colour::mid : colour::dim));
    if (s.track().speed != speed_normal) // the track plays its patterns at a speed (Tracks, Shift + K1)
        h.right.push_back(pill(juce::String::fromUTF8(speed_name(s.track().speed)), c));
    h.right.push_back(tag_of(s, s.ui.track));
    SoftKeys keys {{"PATTERNS", "ROLL", "MOTION"}, 0, c};
    if (s.ui.sheet_open) keys = SoftKeys {{"BACK", "", "DO IT"}, 2, c, {KeyIcon::Back, KeyIcon::None, KeyIcon::Check}};
    else if (s.ui.shift) {
        keys = SoftKeys {{"SCENE", "FOLLOW", "VARY"}, -1, c};
        if (s.ui.patterns_cards == 1) keys.tints[0] = colour::amber;
        if (s.ui.patterns_cards == 2) keys.tints[1] = colour::amber;
    }
    frame(p, s, keys, h);

    for (unsigned k = 0; k < pattern_pads; ++k) draw_cell(p, s, k, first);
    draw_tracks(p, s, first);

    if (s.ui.sheet_open) { // Shift + B3: VARY, a variation of the pattern into the next empty one (roadmap P1)
        p.dim_zone(zone::picture_y, zone::picture_h, 0.7f);
        MenuSheet m;
        m.title = "INTO THE NEXT EMPTY PATTERN"; m.accent = c;
        bool kit = s.track().engine == EngineKind::Sampler || drum_pads(s.track());
        for (unsigned v = 0; v < unsigned(Variation::Count); ++v) {
            bool here = variation_on_kit(Variation(v)) || !kit;
            m.rows[v] = {variation_name(Variation(v)), v < 3 ? juce::String(s.ui.vary_amount) + " %" : juce::String(), false, false, !here};
        }
        m.count = unsigned(Variation::Count); m.current = s.ui.sheet_row % m.count;
        m.detail = {};
        wide_sheet(p, m, "VARY", c); // two columns: the cards (K1 the amount) stay in view
    }
}

void do_vary(Context& c) {
    c.ui.sheet_open = false;
    c.vary(Variation(c.ui.sheet_row % unsigned(Variation::Count)));
}

// Push: the cursor's scene on every track; on SCENE its name, on FOLLOW the follow action onto its column; on VARY
// the variation.
void push(Context& c) {
    auto& ui = c.ui;
    if (ui.sheet_open) { do_vary(c); return; }
    ui.scene = uint8_t(cursor_of(ui));
    if (ui.patterns_cards == 1) { c.name_scene(ui.scene); return; }
    if (ui.patterns_cards == 2) { c.follow_to_column(); return; }
    c.launch_scene(ui.scene);
}

// The wheel: the cursor through the cells in pad order (the VARY sheet: its rows).
void wheel(Context& c, int delta) {
    auto& ui = c.ui;
    if (ui.sheet_open) { ui.sheet_row = uint8_t(std::clamp<int>(ui.sheet_row + delta, 0, int(Variation::Count) - 1)); return; }
    if (ui.shift) { ui.shift_used = true; c.locate(delta); return; } // Shift: the playhead by Q (stopped: where Shift + Play carries on)
    int k = std::clamp<int>(int(ui.scene % pattern_pads) + delta, 0, int(pattern_pads) - 1);
    ui.scene = uint8_t(ui.pattern_bank % pattern_banks * pattern_pads + unsigned(k));
}

// The pads launch patterns before the page sees them (shell/pad_context.h SeqLayer); nothing is left for it.
void pad(Context&, unsigned, uint8_t, bool) {}

void nudge(Context& c, int way) { c.select_track(unsigned(std::clamp<int>(c.ui.track + way, 0, int(track_count) - 1))); }

void soft(Context& c, int key) {
    auto& ui = c.ui;
    if (ui.sheet_open) { // BACK, -, DO IT
        if (key == 0) ui.sheet_open = false;
        if (key == 2) do_vary(c);
        return;
    }
    if (ui.shift) { // SCENE, FOLLOW: their cards (again: the pattern's); VARY: its sheet
        ui.shift_used = true;
        if (key == 0) ui.patterns_cards = ui.patterns_cards == 1 ? 0 : 1;
        if (key == 1) ui.patterns_cards = ui.patterns_cards == 2 ? 0 : 2;
        if (key == 2) { ui.patterns_cards = 0; ui.sheet_open = true; }
        return;
    }
    if (key == 0) { ui.patterns_cards = 0; return; } // PATTERNS again: the pattern's cards
    seq_hub_soft(c, key);
}
} // namespace

const PageSpec patterns_page {draw, pad, wheel, push, soft, nudge};

} // namespace melodee
