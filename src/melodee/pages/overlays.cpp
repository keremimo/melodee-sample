// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Drawn over any page: the tracks while Pad Bank is held, Note Repeat's
// rates, the Shift + K cards while Shift is held, and the Exit sheet after a
// short press of Power.
#include "melodee/engine/arp.h"
#include "melodee/pages/common.h"
#include "melodee/shell/mute_mode.h"
#include "melodee/shell/note_repeat.h"
#include "melodee/shell/pad_bank.h"
#include "melodee/ui/widgets.h"

namespace melodee {
using namespace ui;

void draw_overlays(Paint& p, const Snapshot& s) {
    bool sheet = s.ui.name.open() || s.ui.exit_open;
    if (!sheet && !tracks_on_pads(s.ui)) draw_sampler_map(p, s); // under the notice and the cards
    if (tracks_on_pads(s.ui) && !sheet) draw_track_overlay(p, s);
    if (!sheet && s.ui.ask != UiState::Ask::None) draw_ask_sheet(p, s); // B1 Cancel / B3 Do it before an erase
    if (!sheet && s.ui.ask == UiState::Ask::None && !tracks_on_pads(s.ui)) draw_fader_card(p, s);
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
    // The Shift jobs are printed on the MPC: the page stays as it is, and
    // the knob strip shows the cards Shift turns (Shift + K), or dims when
    // there are none.
    auto cards = cards_of(s);
    bool any = false;
    for (auto& card : cards) { if (card.shift == 2) any = true; else card.state = KnobCard::State::Idle; }
    if (any) knob_strip(p, cards);
    else p.dim_zone(zone::knobs_y, zone::knobs_h);
}

// The fader moved (parity phase 8): for a moment, over the picture's bottom
// right, what it drives on this page and the value, hollow where the fader
// has not reached it yet (takeover). The Fader page and Step Edit (FADER:
// NUDGE) say it on the page.
void draw_fader_card(Paint& p, const Snapshot& s) {
    if (!s.ui.fader_moved_at || s.now - s.ui.fader_moved_at > 1500) return;
    if (s.ui.page == PageId::FaderPage || s.ui.page == PageId::StepEdit) return;
    auto shown = fader_shown(s);
    constexpr float w = 176, h = 50;
    float x = zone::width - zone::pad - w, y = zone::knobs_y - h - 3;
    auto c = colour_of(s);
    p.fill(x - 1, y - 1, w + 2, h + 2, colour::bg, 6);
    p.fill(x, y, w, h, colour::raised, 5);
    auto label = caps_text(colour::mid);
    float lw = p.text("FADER", x + 7, y + 3, 50, 14, label) + 6;
    juce::String what = shown.off ? juce::String::fromUTF8("OFF \xc2\xb7 SHIFT + PAD 9") : shown.what;
    p.text(what, x + 7 + lw, y + 3, w - 14 - lw, 14, p.fit(what, caps_text(shown.off ? colour::mid : c), w - 14 - lw, type::floor));
    if (!shown.value) return;
    juce::String value = shown.card.value + (shown.card.unit.isNotEmpty() ? " " + shown.card.unit : juce::String());
    p.text(value, x + 7, y + 17, w - 14, 20, p.fit(value, plain(shown.waiting ? colour::mid : colour::text, 17, Weight::Medium), w - 14, type::floor));
    float gx = x + 7, gw = w - 14, gy = y + h - 8, f = std::clamp(shown.card.fill, 0.0f, 1.0f);
    p.fill(gx, gy, gw, 3, colour::quiet, 1.5f);
    auto ink = shown.waiting ? colour::mid : c;
    if (shown.card.bipolar) p.fill(gx + gw * std::min(f, 0.5f), gy, std::max(1.0f, gw * std::abs(f - 0.5f)), 3, ink, 1.5f);
    else p.fill(gx, gy, gw * f, 3, ink, 1.5f);
    if (shown.waiting) p.ring(gx + gw * shown.card.pot - 2, gy - 2, 4, 7, colour::text, 1.5f, 1); // where the fader sits
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
        bool kit = mute_on_kit(s.ui, t.engine == EngineKind::Sampler && !t.levels && pad_job(s.ui) == play_pad);
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
        p.fill(zone::pad, zone::picture_y + 1.5f + n * 36, zone::width - 2 * zone::pad, 33, colour::quiet.interpolatedWith(colour::bg, 0.5f), 4);
    juce::String body = s.projects.root.empty()
        ? juce::String("Notes stop and the MPC app starts again. There is no card to save this project on.")
        : juce::String(s.project_path.empty() ? "It is saved as a new project." : (juce::String(s.project.name) + " is saved."))
              + " Notes stop and the MPC app starts again.";
    sheet(p, {"Leave Melodee?", body, true});
}

} // namespace melodee
