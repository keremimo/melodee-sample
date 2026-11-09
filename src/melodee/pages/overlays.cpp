// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Drawn over any page: the Shift layer while Shift is held, and the Exit
// sheet after a short press of Power.
#include "melodee/pages/common.h"
#include "melodee/ui/widgets.h"

namespace melodee {
using namespace ui;

void draw_shift_layer(Paint& p, const Snapshot& s) {
    // Second jobs, as on the Shift board; the ones not built yet stay listed
    // so the layer reads the same as it will.
    static const char* const jobs[][2] {
        {"SAMPLE", "Project, settings"}, {"PAD BANK", "Previous pad bank"}, {"SAMPLE SELECT", "Save sound"},
        {"MUTE", "+ pad: solo track"}, {"ERASE", "Clear pattern"}, {"LOOP", "Double length"}, {"SEQ", "Quantise take"},
        {"PLAY", "Continue"}, {"STOP", "All notes off"}, {"SEQ RECORD", "Undo last take"},
        {"SAMPLE RECORD", "Capture last bars"}, {"TAP TEMPO", "Metronome"}, {"\xe2\x88\x92 / +", "Transpose pads"}, {"PUSH", "Back"},
    };
    bool song = s.ui.page == PageId::Song && !s.ui.sheet_open; // there Push names a marker instead
    p.dim_zone(zone::soft_y, zone::soft_h);
    p.dim_zone(zone::knobs_y, zone::knobs_h);
    p.fill(0, zone::header_y, zone::width, zone::header_h + zone::picture_h, colour::bg);
    Header h = header_of(s);
    h.left.push_back(chip("SHIFT", colour::text));
    h.right.push_back(tag_of(s, s.ui.track));
    header(p, h);
    float x = zone::pad, y = zone::picture_y + 2, w = zone::width - 2 * zone::pad, ph = 144;
    p.fill(x, y, w, ph, colour::surf, 6);
    auto c = colour_of(s);
    float cw = (w - 4 - 2) / 2;
    for (unsigned n = 0; n < 14; ++n) {
        unsigned column = n / 7, row = n % 7;
        float cx = x + 2 + column * (cw + 2) + 6, cy = y + 5 + row * 19;
        p.text(juce::String::fromUTF8(jobs[n][0]), cx, cy, cw - 12, 7, caps_text(c, 6));
        const char* job = song && n == 13 ? "Marker at cursor" : jobs[n][1];
        p.text(job, cx, cy + 7, cw - 12, 11, plain(colour::text, 9.5f, Weight::Medium));
    }
}

void draw_exit_sheet(Paint& p, const Snapshot& s) {
    p.fill(0, 0, zone::width, zone::knobs_y, colour::bg);
    p.dim_zone(zone::knobs_y, zone::knobs_h);
    // Soft keys: STAY under B1, LEAVE under B3 (Push never quits).
    soft_keys(p, {{"STAY", "", "LEAVE"}, 2, colour::text, {KeyIcon::Back, KeyIcon::None, KeyIcon::Check}});

    Header h = header_of(s);
    h.playing = false; h.recording = false;
    h.right.push_back(label(s.project.name));
    header(p, h);
    for (unsigned n = 0; n < 4; ++n)
        p.fill(zone::pad, zone::picture_y + 1.5f + n * 37, zone::width - 2 * zone::pad, 34, colour::quiet.interpolatedWith(colour::bg, 0.5f), 4);
    sheet(p, {"Leave Melodee?", "Notes stop and the MPC app starts again. This project is not saved yet.", true});
}

} // namespace melodee
