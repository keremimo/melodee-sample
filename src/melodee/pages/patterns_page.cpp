// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// PATTERNS (Seq, the MPC's Sequence mode): tracks down, sixteen patterns
// across, eight tracks in view following the selected one. K1 the selected
// pattern's length (1, 2, 4 .. 128 bars; Shift + K1 any bar count), K2 its
// Q (Shift + K2 the project's time signature), K3 swing (Shift + K3 the
// metronome's level). The wheel moves the playhead by a beat (Shift: by Q);
// stopped, it sets where Shift + Play carries on from. Hold B1 and turn the
// wheel for the BPM (Push: whole BPM or tenths); B2 switches BPM per pattern
// (SEQ) and global (GBL). The header shows where the playhead is, the time
// signature and Rec Quantize (Shift + Pad 10). The pads are that track's
// sixteen patterns: tap to queue (it starts on the next bar), Erase + pad
// clears, hold a pad and tap another to copy. Shift + Erase copies too: a
// pad, then (on this or another track) where it goes. B3 COLUMN latches
// column launch: a pad then launches that column on every track. Minus /
// Plus move the track. Song is Shift + Pad 12 (B2 moved to the BPM in
// parity phase 2, as on the MPC).
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/ui/widgets.h"

namespace melodee {
namespace {
using namespace ui;

// Bar.beat.tick of a position in the pattern, as the MPC counts (from 1).
juce::String where(const Snapshot& s, uint32_t ticks) {
    unsigned bar = s.project.bar_ticks(), beat = s.project.beat_ticks();
    char text[24];
    std::snprintf(text, sizeof(text), "%u.%u.%02u", ticks / bar + 1, ticks % bar / beat + 1, ticks % beat);
    return text;
}

void draw(Paint& p, const Snapshot& s) {
    unsigned first = s.ui.track / 8 * 8;
    Header h = header_of(s);
    // B3 COLUMN on: a tap launches the whole column. Counting in or armed, the header's chip takes its room.
    bool chip = !h.left.empty();
    if (!chip) h.left.push_back(s.ui.seq_sample ? pill(s.track().engine == EngineKind::Sampler ? "PADS PLAY KIT" : "PADS PLAY", colour_of(s))
                                : s.ui.launch_column ? pill("LAUNCH COLUMN", colour_of(s)) : pill("LAUNCH BAR"));
    uint32_t length = std::max<uint32_t>(s.pattern.length, 1);
    uint32_t at = s.playing && s.positions[s.ui.track] >= 0 ? uint32_t(s.positions[s.ui.track]) : s.ui.locate % length;
    h.left.push_back(pill(where(s, at), s.playing ? colour::text : colour::mid));
    auto sig = s.project.time_signature();
    h.left.push_back(pill(juce::String(sig.beats) + "/" + juce::String(sig.unit)));
    h.left.push_back(pill(s.ui.settings.rec_quantize ? "REC Q " + juce::String(division_name(s.pattern.division)) : juce::String("REC Q OFF"),
                          s.ui.settings.rec_quantize ? colour::mid : colour::dim));
    // No track tag: the grid marks the selected track, and the header needs the room.
    frame(p, s, {{"BPM", s.project.seq_tempo ? "SEQ BPM" : "GBL BPM", "COLUMN"}, s.ui.launch_column ? 2 : 0, colour_of(s)}, h);

    constexpr float gap = 2, label_w = 16, head_h = 9, top = zone::picture_y + 1.5f;
    constexpr float cw = (zone::width - 2 * zone::pad - label_w - 16 * gap) / 16;
    constexpr float rh = (143.5f - 1.5f - head_h - 8 * gap) / 8;
    unsigned shown = s.playing ? s.playing_patterns[s.ui.track] : s.track().pattern;
    for (unsigned n = 0; n < pattern_count; ++n)
        p.text(juce::String(n + 1), zone::pad + label_w + gap + n * (cw + gap), top, cw, head_h,
               plain(n == shown ? colour::text : colour::dim, 7, Weight::SemiBold), juce::Justification::centred);
    for (unsigned row = 0; row < 8; ++row) {
        unsigned t = first + row;
        const auto& track = s.project.tracks[t];
        auto c = track_colour(t);
        float y = top + head_h + gap + row * (rh + gap);
        bool selected = t == s.ui.track;
        if (selected) p.fill(zone::pad, y, label_w, rh, c, 3);
        p.text(juce::String(t + 1), zone::pad, y, label_w, rh, plain(selected ? colour::bg : c, 9, Weight::Bold),
               juce::Justification::centred);
        unsigned live = s.playing ? s.playing_patterns[t] : track.pattern;
        for (unsigned n = 0; n < pattern_count; ++n) {
            float x = zone::pad + label_w + gap + n * (cw + gap);
            const auto& pattern = s.sketches[t][n];
            bool notes = track.used() && pattern.notes;
            bool playing = track.used() && n == live && notes;
            bool queued = track.used() && s.playing && n == track.pattern && n != live;
            if (playing) {
                p.fill(x, y, cw, rh, c, 3);
                int position = s.positions[t];
                bool here = s.playing ? position >= 0 : selected; // the playhead, or where Shift + Play carries on from
                if (here) {
                    float done = float((s.playing ? uint32_t(position) : s.ui.locate) % std::max<uint32_t>(pattern.length, 1) + 1) / pattern.length;
                    p.g.saveState();
                    p.g.reduceClipRegion(juce::Rectangle<int>(int(x), int(y), int(cw + 1), int(rh)));
                    p.fill(x, y + rh - 2.5f, cw * done, 2.5f, colour::bg.withAlpha(0.5f));
                    p.g.restoreState();
                }
            } else if (queued) {
                p.fill(x, y, cw, rh, c.withAlpha(0.2f), 3);
                p.ring(x, y, cw, rh, c, 3);
            } else if (notes) {
                p.fill(x, y, cw, rh, c.withAlpha(0.2f), 3);
                const float widths[] {8, 5, 6.5f};
                for (unsigned k = 0; k < 3; ++k) p.fill(x + 3, y + rh / 2 - 4 + k * 3, std::min(widths[k], cw - 5), 1.5f, c, 0.75f);
            } else {
                p.fill(x, y, cw, rh, colour::quiet, 3);
            }
            // BPM per pattern: a pattern with a tempo of its own carries a dot.
            if (s.project.seq_tempo && s.project.pattern_tempo[n] && row == 0)
                p.fill(x + cw / 2 - 1, top + head_h - 1.5f, 2, 2, colour::text, 1);
        }
    }
    if (s.ui.b1_held) draw_bpm_window(p, s); // B1 held: the wheel sets the BPM, Push whole / tenths
}

void pad(Context& c, unsigned pad, uint8_t, bool down) {
    auto& ui = c.ui;
    if (down && ui.copying == UiState::Copying::Patterns) { // Shift + Erase: the pattern to copy, then where
        if (ui.copy_from < 0) {
            if (c.patterns.at[ui.track][pad].empty()) { c.notice("P" + std::to_string(pad + 1) + " is empty"); return; }
            ui.copy_from = int8_t(pad); ui.copy_track = ui.track;
            return;
        }
        if (ui.copy_track == ui.track && unsigned(ui.copy_from) == pad) return;
        std::string what = "Copy P" + std::to_string(ui.copy_from + 1) + " to P" + std::to_string(pad + 1);
        c.edit_pattern(ui.track, pad, what.c_str(), [&] { c.copy_pattern(ui.track, unsigned(ui.copy_from), pad, ui.copy_track); });
        c.notice(what);
        ui.stop_copying();
        return;
    }
    if (down) {
        if (ui.erase) { c.edit_pattern(ui.track, pad, "Clear pattern", [&] { c.clear_pattern(ui.track, pad); }); return; }
        if (ui.pattern_held >= 0 && ui.pattern_held != int(pad)) { // hold one, tap another: copy
            c.edit_pattern(ui.track, pad, "Copy pattern", [&] { c.copy_pattern(ui.track, unsigned(ui.pattern_held), pad); });
            ui.pattern_copied = true;
            return;
        }
        ui.pattern_held = int8_t(pad);
        ui.pattern_copied = false;
        return;
    }
    if (ui.pattern_held != int(pad)) return;
    if (!ui.pattern_copied) { // a tap queues it; with COLUMN on, that column on every track
        if (!ui.launch_column) c.launch(ui.track, pad);
        else for (unsigned t = 0; t < track_count; ++t) if (c.project.tracks[t].used()) c.launch(t, pad);
    }
    ui.pattern_held = -1;
}

// B1 held: the BPM, by a whole one or a tenth; else the playhead.
void wheel(Context& c, int delta) {
    if (c.ui.b1_held) { c.set_tempo(int(c.tempo_now()) + delta * (c.ui.bpm_tenths ? 1 : 10)); return; }
    c.locate(delta);
}

void push(Context& c) { if (c.ui.b1_held) c.ui.bpm_tenths = !c.ui.bpm_tenths; }

void nudge(Context& c, int way) { c.select_track(unsigned(std::clamp<int>(c.ui.track + way, 0, int(track_count) - 1))); }

void soft(Context& c, int key) {
    if (key == 1) c.set_seq_tempo(!c.project.seq_tempo);
    if (key == 2) c.ui.launch_column = !c.ui.launch_column;
}
} // namespace

const PageSpec patterns_page {draw, pad, wheel, push, soft, nudge};

} // namespace melodee
