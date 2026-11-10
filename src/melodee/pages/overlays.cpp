// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Drawn over any page: the tracks while Pad Bank is held, Note Repeat's
// rates, the Shift layer while Shift is held, and the Exit sheet after a
// short press of Power.
#include "melodee/engine/arp.h"
#include "melodee/pages/common.h"
#include "melodee/shell/mute_mode.h"
#include "melodee/shell/note_repeat.h"
#include "melodee/shell/pad_bank.h"
#include "melodee/shell/shift_pad_jobs.h"
#include "melodee/ui/widgets.h"

namespace melodee {
using namespace ui;

void draw_overlays(Paint& p, const Snapshot& s) {
    bool sheet = s.ui.name.open() || s.ui.exit_open;
    if (tracks_on_pads(s.ui) && !sheet) draw_track_overlay(p, s);
    if (!sheet && s.ui.ask != UiState::Ask::None) draw_ask_sheet(p, s); // B1 Cancel / B3 Do it before an erase
    draw_notice(p, s);
    if (s.ui.name.open()) draw_name_sheet(p, s);
    if (s.ui.exit_open) draw_exit_sheet(p, s);
    else if (s.ui.shift && !s.ui.shift_used && !s.ui.name.open()) draw_shift_layer(p, s);
    if (!sheet && !tracks_on_pads(s.ui) && repeat_keys(s.project, s.ui)) draw_repeat_keys(p, s);
}

// Note Repeat on: B1-B3 are its rate, 1/8, 1/16, 1/32, and with Shift held their triplets.
void draw_repeat_keys(Paint& p, const Snapshot& s) {
    bool shift = s.ui.shift;
    std::array<juce::String, 3> labels;
    int active = -1;
    for (int k = 0; k < 3; ++k) {
        unsigned rate = repeat_rate(k, shift);
        labels[size_t(k)] = arp_rate_name(rate);
        if (s.track().arp_rate == rate) active = k;
    }
    p.fill(0, zone::soft_y, zone::width, zone::soft_h, colour::bg);
    soft_keys(p, {labels, active, colour_of(s)});
}

void draw_shift_layer(Paint& p, const Snapshot& s) {
    // Second jobs as printed (MELODEE_PARITY.md decisions); some follow the
    // page or the track's engine. Left: Shift + pad as the pads sit (pad 1
    // bottom left), lit where the pad does something; right: the buttons.
    // The knob strip shows the cards Shift turns (Shift + K) and nothing else.
    const auto& t = s.track();
    bool kit = t.engine == EngineKind::Sampler;
    bool kit_pads = kit && !t.levels && page_spec(s.ui.page).pad == play_pad;
    bool song = s.ui.page == PageId::Song && !s.ui.sheet_open; // there Push names a marker instead
    const char* copy = s.ui.pad_bank ? "Copy track" : kit_pads ? "Copy pad" : s.ui.page == PageId::Patterns ? "Copy pattern" : "Copy pad, pattern, track";
    const char* select = s.ui.page == PageId::Sounds ? "Star sound" : kit ? "Save sample" : "Save sound";
    static const char* const clicks[] {"Metronome: off", "Metronome: on", "Metronome: rec"};
    const char* levels = !t.used() ? "Type (a sound first)" : !kit ? "Type: velocity, tune, scale" : "Type: velocity, filter, tune";
    // The page's own Shift + B1-B3, last: Trim's and Chop's (as on the MPC).
    bool trim = s.ui.page == PageId::Trim && kit, chop = s.ui.page == PageId::Chop && kit, browser = s.ui.page == PageId::Samples && kit;
    const char* const jobs[][2] {
        {"SAMPLE", "Input config"}, {"SEQ", "Step edit"}, {"KNOB FX", "FX select: chains"}, {"PAD BANK", "Previous bank"}, {"ERASE", copy},
        {"LOOP", kit ? "Reverse last pad" : "Reverse (kit pads)"}, {"CHOP", kit ? "Note On / One shot" : "Note On (kit pads)"},
        {"16 LEVELS", levels}, {"SAMPLE SELECT", select},
        {"MUTE", "Unmute all; mute + pad: solo"}, {"TAP TEMPO", clicks[s.project.click % 3]},
        {"SAMPLE RECORD", kit ? "Audio recall (25 s)" : "Audio recall (kit tracks)"}, {"SEQ RECORD", "Recall last bars"},
        {"PLAY", "Continue"}, {"STOP", "All notes off"}, {"\xe2\x88\x92 / +", "Undo / redo"},
        {"PUSH", song ? "Marker at cursor" : "Back"},
        {trim ? "B1 / B2" : chop ? "B1 / B2 / B3" : browser ? "PADS" : "",
         trim ? "Loop lock / Normalize" : chop ? "Extract / Split / Merge" : browser ? "Load to next empty pad" : ""},
    };
    p.dim_zone(zone::soft_y, zone::soft_h);
    p.fill(0, zone::header_y, zone::width, zone::header_h + zone::picture_h, colour::bg);
    Header h = header_of(s);
    h.left.push_back(chip("SHIFT", colour::text));
    h.right.push_back(tag_of(s, s.ui.track));
    header(p, h);
    auto c = colour_of(s);

    // Shift + pad: the sixteen printed jobs in a 4 x 4 grid.
    constexpr float gap = 3, top = zone::picture_y + 2, grid_w = 162, grid_h = 144;
    constexpr float pw = (grid_w - 3 * gap) / 4, ph = (grid_h - 3 * gap) / 4;
    for (unsigned pad = 0; pad < 16; ++pad) {
        unsigned row = 3 - pad / 4, column = pad % 4, level = shift_pad_level(s.ui, pad);
        const auto& job = shift_pad_job(pad);
        float x = zone::pad + column * (pw + gap), y = top + row * (ph + gap);
        p.fill(x, y, pw, ph, level == 2 ? c : level == 1 ? colour::surf : colour::quiet, 4);
        auto ink = level == 2 ? colour::bg : level == 1 ? colour::text : colour::dim;
        p.text(juce::String(pad + 1), x + 3.5f, y + 2.5f, 12, 8, plain(level == 2 ? colour::bg : colour::dim, 6, Weight::SemiBold));
        juce::String corner = job.kind == ShiftPadJob::Kind::Later ? "P" + juce::String(job.phase)
                            : job.kind == ShiftPadJob::Kind::Toggle ? juce::String(level == 2 ? "ON" : "OFF") : juce::String();
        if (corner.isNotEmpty())
            p.text(corner, x + pw - 21, y + 2.5f, 18, 8, caps_text(level == 2 ? colour::bg : level ? c : colour::dim, 5.5f),
                   juce::Justification::right);
        juce::StringArray words; // the name on two lines, as printed under the pad
        words.addTokens(job.name, " ", "");
        juce::String first = words[0], second = words.size() > 1 ? words[1] : juce::String();
        for (int k = 2; k < words.size(); ++k) second += " " + words[k];
        float ty = second.isEmpty() ? y + ph - 13 : y + ph - 20;
        auto style = plain(ink, 7.5f, Weight::Medium);
        p.text(first, x + 3, ty, pw - 5, 9, p.fit(first, style, pw - 5, 5));
        if (second.isNotEmpty()) p.text(second, x + 3, ty + 8, pw - 5, 9, p.fit(second, style, pw - 5, 5));
    }

    // The buttons' second jobs, one line each.
    float x = zone::pad + grid_w + 5, w = zone::width - zone::pad - x, name_w = 46;
    p.fill(x, top, w, grid_h, colour::surf, 6);
    const unsigned count = sizeof(jobs) / sizeof(jobs[0]) - (trim || chop || browser ? 0 : 1); // the page's own last
    float rh = (grid_h - 4) / count;
    for (unsigned n = 0; n < count; ++n) {
        float y = top + 2 + n * rh;
        juce::String name = juce::String::fromUTF8(jobs[n][0]), job = juce::String::fromUTF8(jobs[n][1]);
        p.text(name, x + 5, y, name_w, rh, p.fit(name, caps_text(c, 5.5f), name_w, 4.5f));
        auto style = plain(colour::text, 7.5f, Weight::Medium);
        p.text(job, x + 5 + name_w, y, w - name_w - 9, rh, p.fit(job, style, w - name_w - 9, 6));
    }

    // Shift + K: only the cards Shift turns; none, the strip stays dim.
    auto cards = cards_of(s);
    bool any = false;
    for (auto& card : cards) { if (card.shift == 2) any = true; else card.state = KnobCard::State::Idle; }
    if (any) knob_strip(p, cards);
    else p.dim_zone(zone::knobs_y, zone::knobs_h);
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
    } else if (s.ui.copying == UiState::Copying::Tracks) {
        text = s.ui.copy_from < 0 ? juce::String("Copy: pick a track") : "Copy track " + juce::String(s.ui.copy_from + 1) + ": pick where";
    } else if (s.ui.notice[0] && s.now - s.ui.notice_at < 1500) {
        text = juce::String(s.ui.notice);
    } else if (s.ui.mute_mode() && !tracks_on_pads(s.ui)) { // mute mode: what the pads mute here
        const auto& t = s.track();
        bool kit = mute_on_kit(s.ui, t.engine == EngineKind::Sampler && !t.levels && page_spec(s.ui.page).pad == play_pad);
        text = kit ? "Mute: pads" : "Mute: tracks";
        copying = true; // in the track's colour, as a mode
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
