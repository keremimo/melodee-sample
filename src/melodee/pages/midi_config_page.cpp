// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// MIDI CONFIG (Shift + Pad 8, parity phase 9), as on the MPC: the wheel runs
// down the settings, Push opens one and the wheel changes it, Push keeps it;
// Minus / Plus change the one under the cursor straight away. MIDI Port
// (External jacks or USB), In channel (All or 1-16: what plays the selected
// track), Out channel, Melodee's Track Channel (the selected track's own:
// notes on it play that track wherever you are, and its notes go out on it),
// Pad MIDI In, Pad MIDI Out (Never, Always, Empty), Sync In and Out (Off,
// MIDI Clock, MTC), Thru, Receive Program Change (patterns), CV/Sync Out with
// its Base and Division (pulses a quarter: base x division), the three
// takeover modes (Settings has them too), MIDI Panic (Push: every note off,
// everywhere) and Reset Factory Settings (Push, then B3 YES or B1 NO). B1 goes back. The pads play, so what they send can
// be tried; IN and OUT in the header light as MIDI passes.
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/shell/ask.h"
#include "melodee/shell/midi_config_rows.h"
#include <algorithm>

namespace melodee {
namespace {
using namespace ui;

constexpr unsigned shown_rows = 8;

void draw(Paint& p, const Snapshot& s) {
    auto c = colour_of(s);
    Header h = header_of(s);
    h.left.push_back(pill("MIDI CONFIG"));
    bool in = s.ui.midi_in_at && s.now - s.ui.midi_in_at < 150, out = s.ui.midi_out_at && s.now - s.ui.midi_out_at < 150;
    h.right.push_back(pill("IN", in ? colour::text : colour::dim));
    h.right.push_back(pill("OUT", out ? colour::text : colour::dim));
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, {{"BACK", "", ""}, -1, c, {KeyIcon::Back, KeyIcon::None, KeyIcon::None}}, h);

    constexpr float rh = 18; // eight rows in the picture's 148 px
    unsigned cursor = s.ui.midi_cursor % MidiRows;
    unsigned first = cursor < shown_rows / 2 ? 0 : std::min(cursor - shown_rows / 2 + 1, unsigned(MidiRows) - shown_rows);
    float x = zone::pad, w = zone::width - 2 * zone::pad - 6, y = zone::picture_y + 0.5f;
    const auto& track = s.track();
    for (unsigned row = first; row < first + shown_rows && row < MidiRows; ++row, y += rh) {
        bool here = row == cursor, editing = here && s.ui.midi_editing;
        if (here) { p.fill(x, y, w, rh - 1, colour::raised, 4); p.fill(x, y, 3, rh - 1, row == ResetRow ? colour::rec : c); }
        auto ink = here ? colour::text : colour::sec;
        p.text(midi_row_name(row), x + 9, y, w * 0.62f, rh - 1, plain(row == ResetRow && here ? colour::rec : ink, 11.5f, Weight::Medium));
        juce::String value = midi_row_value(s.ui.settings, track, row);
        if (row == TrackChannelRow) value = "T" + juce::String(s.ui.track + 1) + "  " + value;
        if (row == CvDivisionRow && s.ui.settings.midi.cv_out) value += "  (" + juce::String(s.ui.settings.midi.cv_ppqn()) + " PPQN)";
        if (row == ResetRow || row == PanicRow) value = here ? "PUSH" : "";
        auto style = plain(here ? colour::text : colour::mid, 11, Weight::Medium);
        float vw = p.width(value, style), vx = x + w - 7 - vw;
        if (editing) { // the open value: framed
            p.fill(vx - 6, y + 2, vw + 12, rh - 5, colour::surf, 3);
            p.ring(vx - 6, y + 2, vw + 12, rh - 5, colour::text, 3, 1);
        }
        p.text(value, vx, y, vw, rh - 1, style);
    }
    // Where the eight rows sit in the eighteen.
    float bar_x = zone::width - zone::pad - 3, top = zone::picture_y + 1, height = shown_rows * rh - 2;
    p.fill(bar_x, top, 2, height, colour::quiet, 1);
    p.fill(bar_x, top + height * first / MidiRows, 2, height * shown_rows / MidiRows, colour::mid, 1);
}

void wheel(Context& c, int delta) {
    if (c.ui.midi_editing) { c.change_midi(c.ui.midi_cursor, delta > 0 ? 1 : -1); return; }
    c.ui.midi_cursor = uint8_t(std::clamp(int(c.ui.midi_cursor) + delta, 0, int(MidiRows) - 1));
}
void push(Context& c) {
    if (c.ui.midi_cursor == ResetRow) { c.ui.midi_editing = false; ask(c, UiState::Ask::ResetSettings, 0); return; }
    if (c.ui.midi_cursor == PanicRow) { c.ui.midi_editing = false; c.midi_panic(); return; }
    c.ui.midi_editing = !c.ui.midi_editing;
}
void nudge(Context& c, int way) { if (c.ui.midi_cursor < PanicRow) c.change_midi(c.ui.midi_cursor, way); }
void soft(Context& c, int key) {
    c.ui.midi_editing = false;
    if (key == 0) c.back();
}
} // namespace

const PageSpec midi_config_page {draw, play_pad, wheel, push, soft, nudge};

} // namespace melodee
