// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Drawn over any page: the tracks while Pad Bank is held (after a second), the Sequence
// sheet while Seq is held (after a moment), Note Repeat's
// rates, the Shift + K cards while Shift is held, and the Exit sheet after a
// short press of Power; and over all of them the place bar.
#include "melodee/engine/arp.h"
#include "melodee/pages/common.h"
#include "melodee/shell/mute_mode.h"
#include "melodee/shell/note_repeat.h"
#include "melodee/shell/pad_bank.h"
#include "melodee/shell/seq_layer.h"
#include "melodee/ui/widgets.h"

namespace melodee {
using namespace ui;

void draw_overlays(Paint& p, const Snapshot& s) {
    bool tracks = track_overlay_shown(s.ui, s.now); // Pad Bank held a second, or a track copy
    bool sheet = s.ui.name.open() || s.ui.exit_open;
    bool seq = !tracks && seq_sheet_shown(s.ui, s.now); // Seq held a moment: the Sequence sheet
    if (!sheet && !tracks && !seq) draw_sampler_map(p, s); // under the notice and the cards
    if (tracks && !sheet) draw_track_overlay(p, s);
    if (seq && !sheet) draw_seq_sheet(p, s);
    if (!sheet && s.ui.ask != UiState::Ask::None) draw_ask_sheet(p, s); // B1 Cancel / B3 Do it before an erase
    if (!sheet && s.ui.ask == UiState::Ask::None && !tracks && !seq) draw_fader_card(p, s);
    draw_notice(p, s);
    if (s.ui.name.open()) draw_name_sheet(p, s);
    if (s.ui.exit_open) draw_exit_sheet(p, s);
    else if (s.ui.shift && !s.ui.shift_used && !s.ui.name.open()) draw_shift_layer(p, s);
    if (!sheet && !tracks && !seq && repeat_keys(s.project, s.ui)) draw_repeat_keys(p, s);
    draw_place_bar(p, s); // over everything: where the music is stays in view
}

// Note Repeat on: B1-B3 are its rate, 1/8, 1/16, 1/32, and with Shift held their triplets; B1 and B3 again 1/4 and
// 1/64 (dots under them, roadmap P1).
void draw_repeat_keys(Paint& p, const Snapshot& s) {
    bool shift = s.ui.shift;
    int active = -1;
    unsigned now = s.track().arp_rate;
    SoftKeys keys {{}, -1, colour_of(s)};
    for (int k = 0; k < 3; ++k) {
        unsigned rate = repeat_shown(now, k, shift);
        keys.labels[size_t(k)] = arp_rate_name(rate);
        if (now == rate) active = k;
        if (repeat_other(k, shift) != repeat_rate(k, shift)) { // B1 and B3 again: 1/4 and 1/64 (roadmap P1)
            keys.dots[size_t(k)] = 2; keys.dot_at[size_t(k)] = uint8_t(now == repeat_other(k, shift) ? 1 : 0);
        }
    }
    keys.active = active;
    p.fill(0, zone::soft_y, zone::width, zone::soft_h, colour::bg);
    soft_keys(p, keys);
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
    if (s.ui.page == PageId::FaderPage) return;
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
    if (!copying && s.ui.captured_at && s.now - s.ui.captured_at < 3000) { // Capture (capture C): what landed, and the undo keys
        p.fill(0, zone::header_y, zone::width, zone::header_h, colour::bg);
        Header h = header_of(s);
        h.left.push_back(chip(Icon::Check, "P" + juce::String(s.ui.captured_pattern + 1), colour_of(s)));
        if (s.ui.captured_tracks > 1) h.left.push_back(pill("+" + juce::String(s.ui.captured_tracks - 1), colour::text)); // roadmap P5: other tracks too
        h.left.push_back(pill(Icon::Undo, juce::String::fromUTF8("SHIFT \xe2\x88\x92")));
        h.right.push_back(tag_of(s, s.ui.track));
        header(p, h);
        return;
    }
    if (s.ui.copying == UiState::Copying::Pads) {
        char label[4]; pad_label(unsigned(std::max<int>(s.ui.copy_from, 0)), label);
        text = s.ui.copy_from < 0 ? juce::String("Copy: pick a pad") : "Copy " + juce::String(label) + ": pick where";
    } else if (s.ui.copying == UiState::Copying::Patterns) {
        text = s.ui.copy_from < 0 ? juce::String("Copy: pick a pattern") : "Copy P" + juce::String(s.ui.copy_from + 1) + ": pick where";
    } else if (s.ui.copying == UiState::Copying::Tracks) {
        juce::String verb = s.ui.moving ? "Move" : "Copy";
        text = s.ui.copy_from < 0 ? verb + ": pick a track" : verb + " track " + juce::String(s.ui.copy_from + 1) + ": pick where";
    } else if (s.ui.notice[0] && s.now - s.ui.notice_at < 1500) {
        text = juce::String::fromUTF8(s.ui.notice); // notices may hold an en dash or a middle dot
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
    // Soft keys: STAY under B1, POWER OFF under B2, the MPC app under B3 (Push never quits); a tile under each says it again.
    SoftKeys keys {{"STAY", "POWER OFF", "MPC APP"}, s.ui.exit_off ? 1 : -1, colour::text, {KeyIcon::Back, KeyIcon::None, KeyIcon::None}};
    keys.glyphs = {Icon::None, Icon::Power, Icon::Mpc};
    soft_keys(p, keys);

    // The header says what happens to the project: saved (a check), or no card to save it on.
    bool card = !s.projects.root.empty();
    Header h = header_of(s);
    h.playing = false; h.recording = false;
    if (!card) h.left.push_back(pill(Icon::Card, "NO CARD", colour::amber));
    else if (s.ui.exit_off && !s.ui.exit_saved) h.left.push_back(pill(Icon::Rec, "SAVING", colour::amber));
    else if (!s.ui.exit_off && s.dirty) h.left.push_back(pill(Icon::Rec, "UNSAVED", colour::amber)); // as Project says it: OFF and MPC APP save it first
    else h.left.push_back(pill(Icon::Check, s.project_path.empty() ? "SAVED AS NEW" : "SAVED", colour::play));
    h.right.push_back(label(s.project.name));
    header(p, h);
    if (s.ui.exit_off) { // POWER OFF chosen: the save on its way, then the MPC goes dark
        key_tiles(p, {KeyTile {}, word_tile(Icon::Power, s.ui.exit_saved || !card ? "TURNING OFF" : "SAVING", true), KeyTile {}});
        return;
    }
    key_tiles(p, {word_tile(Icon::Back, "STAY", false, colour::sec), word_tile(Icon::Power, "POWER OFF"), word_tile(Icon::Mpc, "MPC APP", true)});
}

} // namespace melodee
