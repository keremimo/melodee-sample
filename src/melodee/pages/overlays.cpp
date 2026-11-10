// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Drawn over any page: the Shift layer while Shift is held, and the Exit
// sheet after a short press of Power.
#include "melodee/pages/common.h"
#include "melodee/ui/widgets.h"

namespace melodee {
using namespace ui;

void draw_shift_layer(Paint& p, const Snapshot& s) {
    // Second jobs as printed (MELODEE_PARITY.md decisions); some follow the
    // page or the track's engine. Shift + pad has only 3, 12 and 16 until
    // phase 1b draws all sixteen.
    const auto& t = s.track();
    bool kit = t.engine == EngineKind::Sampler;
    bool kit_pads = kit && !t.levels && page_spec(s.ui.page).pad == play_pad;
    bool song = s.ui.page == PageId::Song && !s.ui.sheet_open; // there Push names a marker instead
    const char* copy = kit_pads ? "Copy pad" : s.ui.page == PageId::Patterns ? "Copy pattern" : "Copy pads, patterns";
    const char* select = s.ui.page == PageId::Sounds ? "Star sound" : kit ? "Save sample (later)" : "Save sound";
    static const char* const clicks[] {"Metronome: off", "Metronome: on", "Metronome: rec"};
    const char* const jobs[][2] {
        {"SAMPLE", "Input config"}, {"SEQ", "Step edit"}, {"PAD BANK", "Previous pad bank"}, {"ERASE", copy},
        {"LOOP", kit ? "Reverse last pad" : "Reverse (kit pads)"}, {"SAMPLE SELECT", select},
        {"MUTE", "+ pad: solo track"}, {"TAP TEMPO", clicks[s.project.click % 3]},
        {"SAMPLE RECORD", "Audio recall (later)"}, {"SEQ RECORD", "Recall last bars"},
        {"PLAY", "Continue"}, {"STOP", "All notes off"}, {"\xe2\x88\x92 / +", "Undo / redo"},
        {"PUSH", song ? "Marker at cursor" : "Back"},
        {"PAD 3 \xc2\xb7 12 \xc2\xb7 16", "Double seq \xc2\xb7 Song \xc2\xb7 Project"},
    };
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
    constexpr unsigned count = sizeof(jobs) / sizeof(jobs[0]), rows = 8;
    for (unsigned n = 0; n < count; ++n) {
        unsigned column = n / rows, row = n % rows;
        float cx = x + 2 + column * (cw + 2) + 6, cy = y + 3 + row * 17.5f;
        p.text(juce::String::fromUTF8(jobs[n][0]), cx, cy, cw - 12, 7, caps_text(c, 6));
        p.text(juce::String::fromUTF8(jobs[n][1]), cx, cy + 7, cw - 12, 11, plain(colour::text, 9.5f, Weight::Medium));
    }
}

// Over the header: copying a pad (until it is done), or for a moment what
// an undo, redo, delete or copy did.
void draw_notice(Paint& p, const Snapshot& s) {
    juce::String text;
    bool copying = s.ui.copying != UiState::Copying::Off;
    if (s.ui.copying == UiState::Copying::Pads) {
        char label[4]; pad_label(unsigned(std::max<int>(s.ui.copy_from, 0)), label);
        text = s.ui.copy_from < 0 ? juce::String("Copy: pick a pad") : "Copy " + juce::String(label) + ": pick where";
    } else if (s.ui.copying == UiState::Copying::Patterns) {
        text = s.ui.copy_from < 0 ? juce::String("Copy: pick a pattern") : "Copy P" + juce::String(s.ui.copy_from + 1) + ": pick where";
    } else if (s.ui.notice[0] && s.now - s.ui.notice_at < 1500) {
        text = juce::String(s.ui.notice);
    } else {
        return;
    }
    p.fill(0, zone::header_y, zone::width, zone::header_h, colour::bg);
    Header h = header_of(s);
    h.left.push_back(chip(text.toUpperCase(), copying ? colour_of(s) : colour::text));
    h.right.push_back(tag_of(s, s.ui.track));
    header(p, h);
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
    juce::String body = s.projects.root.empty()
        ? juce::String("Notes stop and the MPC app starts again. There is no card to save this project on.")
        : juce::String(s.project_path.empty() ? "It is saved as a new project." : (juce::String(s.project.name) + " is saved."))
              + " Notes stop and the MPC app starts again.";
    sheet(p, {"Leave Melodee?", body, true});
}

} // namespace melodee
