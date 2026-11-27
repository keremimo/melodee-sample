// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// STEP EDIT (Shift + Seq, as printed): the one step editor, over Patterns, Roll or Motion. A caret stands on a
// step of Q (UiState::step_cursor, shared with Roll). Two pad modes, Shift + B1 PADS:
//  PLAY (step entry, the default): play a pad or a chord and it is written at the caret, heard as it goes in;
//   when the pads let go the caret moves on. K1-K3 are what a pad writes: its length in steps, velocity (Played:
//   as hit), chance. Erase + pad erases all of that pad's events once B3 says so.
//  STEPS: the pads are the sixteen steps of the caret's bar, for one sound (a kit's or Drums track's last pad,
//   Sample held + pad picks another; a synth's last note). Tap a step on or off; hold one and K1-K3 lock the
//   Motion lanes there and the wheel moves its pitch (a kit: the lane to the next pad). K1-K3: the step's
//   length, velocity, chance; on a kit Shift + K1 the lane's own length. Erase + pad clears the step.
// B1 the cards: NOTE (as above), LOCK (the Motion lanes locked on the caret's step), TRIG (its ratchet, ramp and
// condition; Shift + K2 a kit pad's pitch ramp), EUCLID on a kit or Drums lane (hits, steps, rotate; Shift + K1
// the lane's length). B2 REST moves the caret on without writing, B3 ERASE steps back and takes that step out, the
// wheel moves it. Shift + B2 TIE ties the caret's notes into the next step, Shift + B3 INSERT puts an empty step at
// the caret and moves the rest on (one undo each). Shift + K1-K3 otherwise: the pattern's length, Q, the time
// signature. Minus / Plus the octave (a kit's bank). Push, Seq or Shift + Seq go back to the page under it.
// Picture (Steps' drawing round the caret's bar): each step's notes at their pitch, taller for harder, a ratchet as
// its hits, an accent's dot, a tie's arc, a slide's sign, a lock's square, a condition's ring, where it plays now;
// the caret's column amber with its mark; the pattern's bars at the foot; and a panel with the pads held (else
// what the caret's step holds) and the step the caret goes to. Past the last step the caret walks into the spare
// steps (shell/spare_bar.h), drawn as outlines; a step written there makes the pattern longer. A + and an arrow at
// the right edge show it can grow, amber while the caret is in them.
#include "melodee/model/scales.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/model/generators.h"
#include "melodee/shell/ask.h"
#include "melodee/shell/page_stack.h"
#include "melodee/shell/step_edit.h"
#include "melodee/shell/step_lane.h"
#include "melodee/shell/step_sounds.h"
#include "melodee/ui/widgets.h"
#include "melodee/ui/waveform.h"
#include "melodee/pages/sampler_common.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace melodee {
namespace {
using namespace ui;
constexpr uint32_t tap_ms = 300;

// What a key is called on the track: on a kit its pad and the pad's name ("A01 Kick"), else its note.
juce::String key_name(const Snapshot& s, int key, bool with_name) {
    const auto& t = s.track();
    juce::String label = key_label(t, key);
    if (with_name && t.engine == EngineKind::Sampler && key >= int(sampler_first_note) && key <= int(sampler_last_key)) {
        unsigned kit = unsigned(key) - sampler_first_note;
        if (t.pads[kit % sampler_pads].used()) label += " " + pad_name(s, kit % sampler_pads, true);
    }
    return label;
}

// A pad held, as it was written: its key, and under 16 Levels the level it plays ("A01 +7", "A01 V96", "C3 V40").
juce::String held_name(const Snapshot& s, unsigned pad) {
    const auto& t = s.track();
    if (t.engine == EngineKind::Sampler && t.levels) {
        auto lv = levels_of(s.project, t, pad);
        juce::String name = key_name(s, int(sampler_first_note + t.levels_pad % sampler_pads), false);
        if (lv.none) return name;
        if (lv.cutoff >= 0) return name + " F" + juce::String(lv.cutoff);
        if (lv.velocity) return name + " V" + juce::String(lv.velocity);
        return name + " " + (lv.semitones > 0 ? "+" : "") + juce::String(lv.semitones);
    }
    int key = s.held_keys[pad];
    if (key < 0) return {};
    juce::String name = key_name(s, key, true);
    if (t.engine != EngineKind::Sampler && t.levels && levels_of(s.project, t, pad).velocity) name += " V" + juce::String(levels_of(s.project, t, pad).velocity);
    else if (t.engine != EngineKind::Sampler && !t.levels && t.layout == PadLayout::Levels) name += " V" + juce::String(level_velocity(pad));
    return name;
}

// A note's bar, its ends showing step PLAY: pointed back (REV), forward (FWD), both (RND); square as the pad plays.
void note_bar(Paint& p, float x, float y, float w, float h, juce::Colour ink, StepPlay play) {
    if (play == StepPlay::Pad) { p.fill(x, y, w, h, ink, 2); return; }
    const float tip = std::min(3.5f, w / 3);
    const bool back = play != StepPlay::Forward, on = play != StepPlay::Reverse;
    juce::Path bar;
    bar.startNewSubPath(x + (back ? tip : 0), y);
    bar.lineTo(x + w - (on ? tip : 0), y);
    if (on) bar.lineTo(x + w, y + h / 2); else { bar.lineTo(x + w, y); bar.lineTo(x + w, y + h); }
    bar.lineTo(x + w - (on ? tip : 0), y + h);
    bar.lineTo(x + (back ? tip : 0), y + h);
    if (back) bar.lineTo(x, y + h / 2); else { bar.lineTo(x, y + h); bar.lineTo(x, y); }
    bar.closeSubPath();
    p.g.setColour(ink);
    p.g.fillPath(bar);
}

unsigned bars_of(const Pattern& p) { return std::max(1u, (p.steps() + bar_steps - 1) / bar_steps); }

void draw_bar(Paint& p, const Snapshot& s, unsigned caret, juce::Colour c) {
    const auto& pattern = s.pattern;
    unsigned steps = pattern.steps(), first = caret / bar_steps * bar_steps;
    int lane = steps_lane(s.track());
    bool lane_only = s.ui.step_pads && lane >= 0; // STEPS on a kit: the lane's notes, the others quiet
    // Pitch as rows of the project's scale, five of them centred on the bar's notes.
    std::array<uint8_t, 12> degrees {};
    unsigned count = std::max(1u, scale_degrees(s.project.scale, degrees));
    auto row_key = [&](int index) {
        int octave = index >= 0 ? index / int(count) : -((-index + int(count) - 1) / int(count));
        return 12 * octave + s.project.root + degrees[size_t(index - octave * int(count))];
    };
    auto index_of = [&](int key) {
        int best = 0;
        for (int i = 0; i < 80; ++i) if (row_key(i) <= key) best = i;
        return best;
    };
    auto pitch_of = [](const Event& e) { return int(e.key) + e.pitch; }; // 16 Levels on a kit: its pad at the level's pitch
    int lo = 1000, hi = -1;
    for (unsigned st = first; st < first + bar_steps && st < steps; ++st) {
        auto in = step_events(pattern, st);
        for (unsigned n = 0; n < in.count && n < notes_per_step; ++n) {
            int at = index_of(pitch_of(pattern.events[in.first + n]));
            lo = std::min(lo, at); hi = std::max(hi, at);
        }
    }
    if (hi < 0) lo = hi = index_of(s.track().step_note());
    constexpr int rows = 5;
    int base = (lo + hi) / 2 - rows / 2;
    constexpr float top = zone::picture_y + 8, ch = 64, colw = 15.6f, stepw = 18.0f, beat_gap = 2.2f; // room at the right for +▸
    unsigned spare = spare_end(s.project, pattern); // the steps the caret may walk to, the spare ones past `steps`
    auto x_of = [&](unsigned k) { return zone::pad + k * stepw + float(k / 4) * beat_gap; };
    uint32_t loop = lane >= 0 ? pattern.lane_length(unsigned(lane)) : pattern.length;
    unsigned lane_steps = std::max(1u, (loop + pattern.q() - 1) / pattern.q());
    uint64_t elapsed = uint64_t(s.pass) * std::max<uint32_t>(pattern.length, 1) + s.position % std::max<uint32_t>(pattern.length, 1);
    int playing_step = s.playing ? int((elapsed % std::max<uint32_t>(loop, 1)) / pattern.q()) : -1;
    uint32_t now = uint32_t(elapsed % std::max<uint32_t>(loop, 1)); // where it plays, in ticks
    auto sounding = [&](const Event& e) { return s.playing && now >= e.tick && now < e.tick + std::max<uint32_t>(e.length, pattern.q() / 2); };
    for (unsigned k = 0; k < bar_steps; ++k) {
        unsigned st = first + k;
        float x = x_of(k);
        bool inside = st < steps, here = st == caret, held = int(st) == s.ui.held_step, past = st >= lane_steps;
        p.fill(x, top, colw, ch, here ? colour::amber.withAlpha(0.22f) : inside && !past ? colour::quiet : colour::bg, 2);
        if (!inside && st < spare) p.ring(x + 0.5f, top + 0.5f, colw - 1, ch - 1, here ? colour::amber : colour::line, 2); // a spare step
        if (int(st) == playing_step) p.fill(x, top - 3, colw, 2, colour::text, 1); // where it plays now
        auto ink = here ? colour::amber : inside && !pattern.step(st).empty() ? colour::sec : colour::dim;
        p.text(juce::String(st + 1), x - 4, top + ch + 2, colw + 8, 13, plain(ink, type::small, here || k % 4 == 0 ? Weight::Bold : Weight::Medium),
               juce::Justification::centred);
        if (here) { // the caret's mark under its number
            juce::Path mark;
            mark.addTriangle(x + 3.2f, top + ch + 21, x + 8.2f, top + ch + 16, x + 13.2f, top + ch + 21);
            p.g.setColour(colour::amber);
            p.g.fillPath(mark);
        }
        if (!inside) continue;
        const auto& step = pattern.step(st);
        auto in = step_events(pattern, st);
        for (unsigned n = 0; n < in.count && n < notes_per_step; ++n) { // each note at its pitch's row, as tall as it is hard
            const auto& e = pattern.events[in.first + n];
            int row = std::clamp(index_of(pitch_of(e)) - base, 0, rows - 1);
            float cy = top + ch - (row + 0.5f) * (ch / rows);
            bool other = lane_only && int(e.key) != lane;
            auto ink_note = held || (sounding(e) && !past) ? colour::text : past || other ? colour::line : c; // white: held, or sounding now
            unsigned hits = e.hits();
            if (hits == 1) {
                float bh = 4 + 6 * e.velocity / 127.0f;
                note_bar(p, x + 1.5f, cy - bh / 2, colw - 3, bh, ink_note, step_play(e.flags)); // its ends: step PLAY
                if (e.sample) p.ring(x + 0.5f, cy - bh / 2 - 1, colw - 1, bh + 2, colour::text.withAlpha(0.85f), 2.5f, 1.2f); // a sample of its own
                continue;
            }
            float gap = 0.75f, hw = (colw - 3 - gap * (hits - 1)) / hits; // a ratchet: its hits side by side
            for (unsigned h = 0; h < hits; ++h) {
                float bh = 4 + 6 * e.hit_velocity(e.velocity, h) / 127.0f;
                p.fill(x + 1.5f + h * (hw + gap), cy - bh / 2, hw, bh, ink_note, 1);
            }
        }
        if (step.empty()) continue;
        int lane_note = lane_event(pattern, st, lane);
        int kit_pad = s.track().engine == EngineKind::Sampler && lane >= 0 ? lane - int(sampler_first_note) : -1;
        if (lock_in(pattern, st * pattern.q(), (st + 1) * pattern.q(), kit_pad)) // a Motion lock: a square at its top
            p.fill(x + 2, top + 2, 4, 4, held ? colour::text : colour::sec, 1);
        if (lane_note >= 0 && pattern.events[size_t(lane_note)].condition != trig_always) // a trig condition: a ring at its foot
            p.ring(x + colw / 2 - 3, top + ch - 8, 6, 6, colour::sec, 3, 1.2f);
        if (step.flags & step_accent) p.circle(x + colw / 2, top - 4.5f, 1.8f, colour::text); // an accent: a dot over it
        if (step.flags & step_tie && k + 1 < bar_steps) { // a tie: an arc under it to the next
            juce::Path arc;
            float ax = x + colw / 2, bx = x_of(k + 1) + colw / 2, ay = top + ch - 1;
            arc.startNewSubPath(ax, ay); arc.cubicTo(ax + 4, ay + 5, bx - 4, ay + 5, bx, ay);
            p.g.setColour(colour::sec); p.g.strokePath(arc, juce::PathStrokeType(1.3f));
        }
        if (step.flags & step_slide) icon(p, Icon::Slide, x + colw - 9, top + 2, 8, colour::sec, 1.6f); // a slide: its sign
    }
    // The +▸ mark at the right edge: the pattern can grow past its last step (amber: the caret is there).
    if (spare_mark(s.project, s.ui, pattern)) {
        bool there = in_spare(s.ui, pattern);
        auto ink = there ? colour::amber : colour::sec;
        float tx = zone::width - zone::pad - 12;
        p.fill(tx, top, 12, ch, there ? colour::amber.withAlpha(0.22f) : colour::quiet, 3);
        icon(p, Icon::Plus, tx + 2, top + ch / 2 - 11, 8, ink, 1.6f);
        icon(p, Icon::ArrowRight, tx + 1, top + ch / 2 + 2, 10, ink, 1.6f);
    }
    // The pattern's bars at the foot, the caret's lit (eight at a time); the spare page after them as an outline.
    unsigned bars = bars_of(pattern), bar = caret / bar_steps, total = std::max(bars, bar + 1);
    unsigned shown = std::min(total, 8u), window = bar / shown * shown;
    float bw = (zone::width - 2 * zone::pad - (shown - 1) * 3.0f) / shown, by = top + ch + 24;
    for (unsigned b = 0; b < shown && window + b < total; ++b) {
        auto ink = window + b == bar ? (window + b < bars ? c : colour::amber) : colour::line;
        if (window + b < bars) p.fill(zone::pad + b * (bw + 3), by, bw, 3, ink, 1.5f);
        else p.ring(zone::pad + b * (bw + 3), by - 0.5f, bw, 4, ink, 1.5f);
    }
}

// Step Edit's SOUND panel: where the step's sound comes from (its track's tag, the pad and the sample), its way, and
// the whole sample with the step's slice lit.
void draw_sound_panel(Paint& p, const Snapshot& s, juce::Colour c, float px, float py, float pw, float ph) {
    const auto& t = s.track();
    const int n = lane_event(s.pattern, sound_step(s.ui, s.pattern), steps_lane(t));
    const float cy = py + ph / 2;
    if (n < 0) { p.text("Rest", px + 10, cy - 9, 80, 18, plain(colour::dim, type::body)); return; }
    const auto& e = s.pattern.events[size_t(n)];
    const auto choices = step_sound_choices(s.project);
    const int k = step_sound_index(choices, e.sample);
    juce::String name, tag = juce::String(s.ui.track + 1);
    juce::Colour from = c;
    unsigned slot = no_sample;
    if (k > 0) {
        const auto& choice = choices[size_t(k - 1)];
        char label[4];
        pad_label(choice.pad, label);
        name = juce::String(label) + " " + juce::String::fromUTF8(s.project.samples[choice.slot].name);
        tag = juce::String(choice.track + 1); from = track_colour(choice.track); slot = choice.slot;
    } else if (e.sample) { // a sample no pad holds now
        slot = e.sample - 1u;
        name = juce::String::fromUTF8(s.project.samples[slot % sample_slots].name);
    } else {
        name = key_name(s, e.key, true);
        if (t.engine == EngineKind::Keygroup) slot = t.pads[0].sample;
        else if (e.key >= sampler_first_note) slot = t.pads[(e.key - sampler_first_note) % sampler_pads].sample;
    }
    float x = px + 8;
    p.fill(x, cy - 9, 16, 18, from, 3);
    p.text(tag, x, cy - 9, 16, 18, plain(colour::bg, type::small, Weight::Bold), juce::Justification::centred);
    x += 22;
    auto style = plain(colour::text, type::body, Weight::SemiBold);
    const float most = pw * 0.46f - 44;
    const float nw = std::min(p.width(name, style), most);
    p.text(name, x, cy - 9, nw, 18, p.fit(name, style, most, type::floor));
    x += nw + 6;
    switch (step_play(e.flags)) { // its way, when not the pad's
    case StepPlay::Reverse: icon(p, Icon::ArrowLeft, x, cy - 6, 12, colour::sec, 1.6f); break;
    case StepPlay::Forward: icon(p, Icon::ArrowRight, x, cy - 6, 12, colour::sec, 1.6f); break;
    case StepPlay::Random: icon(p, Icon::Dice, x, cy - 6, 12, colour::sec, 1.4f); break;
    default: break;
    }
    // The sample, whole: its slices' edges, the step's slice lit (RND: all of them).
    const float wx = px + pw * 0.5f, ww = px + pw - 8 - wx, wy = py + 5, wh = ph - 10;
    p.fill(wx, wy, ww, wh, colour::bg, 3);
    if (!s.step_wave_shown || slot >= sample_slots) return;
    const uint32_t frames = std::max<uint32_t>(s.project.samples[slot].frames, 1);
    const auto& chop = s.project.chops[slot].slices;
    float lit_from = 0, lit_to = 1;
    if (e.slice && e.slice != slice_random && e.slice <= chop.count) {
        lit_from = float(chop.start[e.slice - 1u]) / frames;
        lit_to = float(chop.end(e.slice - 1u, frames)) / frames;
    }
    const auto& wave = s.step_wave;
    const float bw = ww / float(wave.size());
    for (unsigned k2 = 0; k2 < wave.size(); ++k2) {
        const float at = (k2 + 0.5f) / float(wave.size()), bh = std::max(1.0f, wave[k2] / 255.0f * (wh - 4));
        p.fill(wx + k2 * bw, wy + (wh - bh) / 2, std::max(1.0f, bw - 0.4f), bh, at >= lit_from && at < lit_to ? c : colour::line, 0.5f);
    }
    for (unsigned k2 = 1; k2 < chop.count; ++k2) p.fill(wx + ww * float(chop.start[k2]) / frames, wy, 1, wh, colour::dim, 0);
}

// Step SOUND's sounds on the pads (SAMPLE SELECT with a step held): the shown track's bank as the pads sit, each its
// waveform and name, the one the step plays lit; on SLICES the step's sample with its slices numbered.
void draw_sounds(Paint& p, const Snapshot& s, juce::Colour c) {
    const auto& ui = s.ui;
    const auto& t = s.track();
    const unsigned st = sound_step(ui, s.pattern);
    const int n = lane_event(s.pattern, st, steps_lane(t));
    const auto& source = s.project.tracks[ui.sounds_track % track_count];
    Header h = header_of(s);
    h.left.push_back(chip("STEP " + juce::String(st + 1), colour::amber));
    const unsigned bank = ui.sounds_bank % sampler_banks;
    h.left.push_back(pill("BANK " + juce::String::charToString(juce::juce_wchar('A' + bank))));
    h.right.push_back(tag_of(s, ui.sounds_slices ? ui.track : ui.sounds_track % track_count));
    SoftKeys keys {{"CLEAR", "SLICES", ""}, -1, c};
    keys.toggles = 2; keys.lit = ui.sounds_slices ? 2 : 0;
    frame(p, s, keys, h);
    if (n < 0) return;
    const auto& e = s.pattern.events[size_t(n)];
    uint16_t plays = e.sample;
    if (!plays && t.engine == EngineKind::Sampler && e.key >= sampler_first_note) plays = uint16_t(t.pads[(e.key - sampler_first_note) % sampler_pads].sample + 1);
    if (!plays && t.engine == EngineKind::Keygroup) plays = uint16_t(t.pads[0].sample + 1);
    if (ui.sounds_slices) { // the step's sample, its slices numbered (sixteen at a time: Pad Bank)
        const unsigned slot = plays ? plays - 1u : no_sample;
        if (slot >= sample_slots) return;
        const uint32_t frames = std::max<uint32_t>(s.project.samples[slot].frames, 1);
        const auto& chop = s.project.chops[slot].slices;
        const float x = zone::pad, y = zone::picture_y + 8, w = zone::width - 2 * zone::pad, hgt = zone::picture_h - 34;
        p.fill(x, y, w, hgt, colour::panel, 4);
        float from = 0, to = 0;
        if (e.slice && e.slice != slice_random && e.slice <= chop.count) { from = float(chop.start[e.slice - 1u]) / frames; to = float(chop.end(e.slice - 1u, frames)) / frames; }
        if (s.sample.frames) waveform(p, x + 2, y + 2, w - 4, hgt - 4, s.sample.overview, 0, 1, from, to, c);
        for (unsigned k = 0; k < chop.count; ++k) {
            const float sx = x + w * float(chop.start[k]) / frames, ex = x + w * float(chop.end(k, frames)) / frames;
            if (k) p.fill(sx, y, 1, hgt, colour::dim, 0);
            if (k / 16 != bank) continue;
            const bool lit = e.slice == k + 1;
            p.text(juce::String(k + 1), sx, y + hgt + 2, std::max(ex - sx, 12.0f), 13, plain(lit ? colour::amber : colour::mid, type::small, lit ? Weight::Bold : Weight::Medium),
                   juce::Justification::centred);
        }
        return;
    }
    constexpr float gap = 4, top = zone::picture_y + 4, grid_w = zone::width - 2 * zone::pad;
    constexpr float w = (grid_w - 3 * gap) / 4, ch = (zone::picture_h - 8 - 3 * gap) / 4;
    for (unsigned k = 0; k < 16; ++k) {
        const unsigned kit = bank * 16 + k, row = 3 - k / 4, column = k % 4;
        const float x = zone::pad + column * (w + gap), y = top + row * (ch + gap);
        const bool keygroup = source.engine == EngineKind::Keygroup;
        const auto& sound = source.pads[keygroup ? 0 : kit % sampler_pads];
        const bool used = sound.used() && (!keygroup || kit == 0);
        const bool picked = used && sound.sample + 1u == plays;
        const auto pc = sound.colour ? juce::Colour(0xff000000u | full_colour(sound.colour)) : track_colour(ui.sounds_track % track_count);
        p.fill(x, y, w, ch, picked ? pc : used ? colour::surf : colour::quiet, 3);
        if (!used) continue;
        const float wx = x + 5, wy = y + 4, ww = w - 10, wh = ch - 19;
        if (s.pad_waves_shown & (1u << k)) {
            const auto& wave = s.pad_waves[k];
            const float bw = ww / float(wave.size());
            const auto ink = picked ? ink_on(pc).withAlpha(0.9f) : pc.withAlpha(0.75f);
            for (unsigned b = 0; b < wave.size(); ++b) {
                const float bh = std::max(1.0f, wave[b] / 255.0f * wh);
                p.fill(wx + b * bw, wy + (wh - bh) / 2, std::max(1.0f, bw - 0.6f), bh, ink, 0.5f);
            }
        }
        const auto name = juce::String::fromUTF8(s.project.samples[sound.sample % sample_slots].name);
        auto style = plain(picked ? ink_on(pc) : colour::text, type::small, Weight::SemiBold);
        p.text(name, x + 5, y + ch - 15, w - 10, 13, p.fit(name, style, w - 10, type::floor));
        if (s.project.chops[sound.sample % sample_slots].slices.count > 1) // chopped: its slices (B2 SLICES)
            icon(p, Icon::Scissors, x + w - 13, y + 3, 9, picked ? ink_on(pc) : colour::sec, 1.3f);
    }
}

void draw(Paint& p, const Snapshot& s) {
    const auto& pattern = s.pattern;
    auto c = colour_of(s);
    if (sounds_open(s.ui)) { draw_sounds(p, s, c); return; }
    const unsigned set = step_set(s.track(), s.ui.step_cards);
    unsigned q = pattern.q(), caret = edit_caret(s.ui, pattern);
    bool spare = in_spare(s.ui, pattern);
    int lane = steps_lane(s.track());
    Header h = header_of(s);
    // Past the last step: NEW BAR, a step here makes the pattern longer.
    juce::String bar_name = spare ? juce::String("NEW BAR") : "BAR " + juce::String(caret / bar_steps + 1);
    juce::String where = "STEP " + juce::String(caret + 1) + juce::String::fromUTF8(" \xc2\xb7 ") + bar_name;
    if (s.ui.step_pads) { // STEPS: the sound the pads step
        int key = lane >= 0 ? lane : int(s.track().step_note());
        where = (spare ? bar_name : juce::String("STEPS")) + juce::String::fromUTF8(" \xc2\xb7 ") + key_name(s, key, true);
    }
    if (s.ui.held_step >= 0) where += juce::String::fromUTF8(set == cards_sound ? " \xc2\xb7 SOUND" : " \xc2\xb7 LOCK"); // held: the pots lock it (SOUND: set its sound)
    h.left.push_back(edit_pattern_chip(s));
    h.left.push_back(chip(where, colour::amber));
    if (set == cards_euclid && lane >= 0) // the EUCLID row: the lane's rhythm, E(hits, steps)
        if (const EuclidLane* e = euclid_of(pattern, unsigned(lane)))
            h.left.push_back(pill("E(" + juce::String(e->hits) + "," + juce::String(e->steps) + ")"));
    if (lane >= 0 && pattern.own_lane(unsigned(lane))) // its lane loops on its own
        h.left.push_back(pill("LANE " + juce::String((pattern.lane_length(unsigned(lane)) + q - 1) / q)));
    h.right.push_back(tag_of(s, s.ui.track));
    std::array<uint8_t, cards_count> card_sets {};
    const unsigned sets = step_card_sets(s.track(), card_sets);
    SoftKeys keys {{step_cards_name(set), "REST", "BACKSPACE"}, -1, c}; // Shift + Seq (or Seq, or Push) closes (T-a)
    keys.glyphs = {Icon::None, Icon::ArrowRight, Icon::Trash};
    keys.tints[0] = c; keys.tints[2] = colour::rec;
    keys.dots[0] = uint8_t(sets); keys.dot_at[0] = uint8_t(step_set_place(s.track(), s.ui.step_cards));
    if (s.ui.shift) { // PADS (STEPS lit), TIE, INSERT
        keys = SoftKeys {{"PADS", "TIE", "INSERT"}, -1, c};
        keys.toggles = 1; keys.lit = s.ui.step_pads ? 1 : 0;
        keys.glyphs = {Icon::Pads, Icon::Tie, Icon::Plus};
    }
    frame(p, s, keys, h);
    draw_bar(p, s, caret, c);

    // The panel: the pads held (bright), else what the caret's step holds (quiet); and where the caret goes next.
    float py = zone::picture_y + 103, ph = zone::knobs_y - 4 - py, px = zone::pad, pw = zone::width - 2 * zone::pad;
    p.fill(px, py, pw, ph, colour::panel, 4);
    if (set == cards_sound) { draw_sound_panel(p, s, c, px, py, pw, ph); return; } // SOUND: the step's sound
    std::vector<juce::String> keys_shown;
    bool held = s.ui.entry_held != 0;
    auto add = [&](const juce::String& name) {
        if (name.isNotEmpty() && std::find(keys_shown.begin(), keys_shown.end(), name) == keys_shown.end()) keys_shown.push_back(name);
    };
    if (held) {
        for (unsigned pad = 0; pad < 16; ++pad)
            if (s.ui.entry_held & (1u << pad)) add(held_name(s, pad));
    } else {
        for (size_t n = pattern.lower(caret * q); n < pattern.events.size() && pattern.events[n].tick < (caret + 1) * q; ++n) {
            const auto& e = pattern.events[n];
            juce::String name = key_name(s, e.key, true);
            if (e.pitch) name = key_name(s, e.key, false) + " " + (e.pitch > 0 ? "+" : "") + juce::String(int(e.pitch));
            if (e.slice) { // a slice hit (issue 24): its pad and slice, the pad's name left out where both do not fit
                const juce::String slice = juce::String::fromUTF8(" \xc2\xb7 ") + juce::String(int(e.slice));
                name += slice;
                if (p.width(name, plain(colour::sec, type::body, Weight::SemiBold)) > 82) name = key_name(s, e.key, false) + slice;
            }
            add(name);
        }
    }
    float ix = px + 10, cy = py + ph / 2;
    for (unsigned k = 0; k < keys_shown.size() && k < 3; ++k) {
        const auto& name = keys_shown[k];
        auto style = plain(held ? colour::text : colour::sec, type::body, Weight::SemiBold);
        float tw = std::min(p.width(name, style), 82.0f);
        p.fill(ix - 4, cy - 9, tw + 8, 18, held ? c.withAlpha(0.25f) : colour::raised, 9);
        p.text(name, ix, cy - 9, tw, 18, p.fit(name, style, 82, type::floor));
        ix += tw + 14;
    }
    if (keys_shown.size() > 3) p.text("+" + juce::String(int(keys_shown.size()) - 3), ix, cy - 9, 30, 18, plain(colour::mid, type::body));
    if (keys_shown.empty()) p.text("Rest", ix, cy - 9, 80, 18, plain(colour::dim, type::body));
    if (s.ui.step_pads) return; // STEPS: the caret goes where a pad is held, not on
    unsigned end = spare_end(s.project, pattern); // on into the spare steps while it can grow, else round
    juce::String next = juce::String((caret + 1 >= end ? 0 : caret + 1) + 1);
    auto next_style = plain(colour::amber, type::body, Weight::Bold);
    float nw = p.width(next, next_style) + 14;
    p.fill(px + pw - 8 - nw, cy - 9, nw, 18, colour::raised, 9);
    p.text(next, px + pw - 8 - nw, cy - 9, nw, 18, next_style, juce::Justification::centred);
    icon(p, Icon::ArrowRight, px + pw - 8 - nw - 18, cy - 6, 12, colour::amber, 1.6f);
}

// ---- STEPS: the pads are the caret's bar (Steps' job)
// Settings > SEQ > Note preview: step `st`'s notes (on a kit lane, the lane's) heard once; `settle`: once the wheel rests.
void hear_step(Context& c, unsigned st, int lane, bool settle) {
    const auto& p = c.pattern();
    auto in = step_events(p, st);
    std::vector<Event> notes;
    for (unsigned n = 0; n < in.count; ++n)
        if (lane < 0 || int(p.events[in.first + n].key) == lane) notes.push_back(p.events[in.first + n]);
    c.preview_notes(notes, settle);
}

void clear_step(Context& c, unsigned st, int lane) {
    if (lane < 0) { c.set_step(c.ui.track, st, Step {}); return; }
    if (clear_lane(c.pattern(), st, lane)) c.sync_pattern(c.ui.track, c.track().pattern);
}

void steps_pad(Context& c, unsigned pad, uint8_t velocity, bool down) {
    auto& ui = c.ui;
    auto& pattern = c.pattern();
    unsigned bar = edit_caret(ui, pattern) / bar_steps;
    unsigned st = bar * bar_steps + pad % 16;
    int lane = steps_lane(c.track()); // a kit: the last pad's lane, the other pads' notes left alone
    if (st >= pattern.steps()) { // a spare step: set, the pattern grows to hold it (shell/spare_bar.h)
        if (!down || ui.erase || !grow_to_step(c, st)) return;
    }
    if (down) {
        if (ui.erase) { c.edit_pattern(ui.track, c.track().pattern, "Erase step", [&] { clear_step(c, st, lane); }); return; }
        ui.step_cursor = int16_t(st);
        ui.held_step = int16_t(st);
        ui.held_since = c.now;
        ui.held_edited = false;
        ui.held_created = !step_velocity(pattern, st, lane);
        if (ui.held_created) {
            uint8_t v = uint8_t(std::max<int>(velocity, 64));
            if (lane >= 0) {
                if (add_lane_note(pattern, st, uint8_t(lane), v)) c.sync_pattern(ui.track, c.track().pattern);
                else c.notice("The pattern is full");
            } else {
                Step step;
                step.notes[0] = {c.track().step_note(), v};
                c.set_step(ui.track, st, step);
            }
            hear_step(c, st, lane, false); // the step set, heard
        }
        return;
    }
    if (ui.held_step != int(st)) return;
    bool tap = c.now - ui.held_since < tap_ms;
    if (!ui.held_created && !ui.held_edited && tap) clear_step(c, st, lane);
    ui.held_step = -1;
}

// A step held: its pitch (a kit: the lane's note to the next pad, not onto one already there). False when none is.
bool held_wheel(Context& c, int delta) {
    auto& ui = c.ui;
    if (ui.held_step < 0) return false;
    auto& pattern = c.pattern();
    auto& t = c.track();
    unsigned st = unsigned(ui.held_step);
    if (steps_lane(t) >= 0) {
        bool kit = t.engine == EngineKind::Sampler;
        int lane = steps_lane(t), to = std::clamp(lane + delta, int(kit ? sampler_first_note : drum_first_key), int(kit ? sampler_last_key : drum_last_key));
        if (to != lane && !step_velocity(pattern, st, to) && move_lane(pattern, st, lane, uint8_t(to))) {
            t.last_note = uint8_t(to);
            if (kit) t.last_pad = uint8_t(to - int(sampler_first_note));
            c.sync_pattern(ui.track, t.pattern);
            hear_step(c, st, to, true); // on the pad it moved to, once the wheel rests
        }
        ui.held_edited = true;
        return true;
    }
    Step step = pattern.step(st); // the held step's notes move together, one semitone a detent
    for (auto& n : step.notes) if (n.velocity) n.key = uint8_t(std::clamp(n.key + delta, 0, 127));
    if (const Note* n = step.first()) t.last_note = n->key;
    c.set_step(ui.track, st, step);
    hear_step(c, st, -1, true); // at its new pitch, once the wheel rests
    ui.held_edited = true;
    return true;
}

// ---- Shift + B2 TIE, Shift + B3 INSERT
void tie(Context& c) {
    auto& ui = c.ui;
    unsigned caret = edit_caret(ui, c.pattern());
    int lane = steps_lane(c.track());
    if (in_spare(ui, c.pattern()) || c.pattern().step(caret).empty()) { c.notice("Nothing to tie on step " + std::to_string(caret + 1)); return; }
    bool on = false;
    c.edit_pattern(ui.track, c.track().pattern, "Tie", [&] {
        auto& p = c.pattern();
        auto in = step_events(p, caret);
        bool any_off = false;
        for (unsigned n = 0; n < in.count; ++n) {
            const auto& e = p.events[in.first + n];
            if ((lane < 0 || int(e.key) == lane) && !(e.flags & step_tie)) any_off = true;
        }
        on = any_off;
        for (unsigned n = 0; n < in.count; ++n) {
            auto& e = p.events[in.first + n];
            if (lane >= 0 && int(e.key) != lane) continue;
            e.flags = uint8_t(on ? e.flags | step_tie : e.flags & ~step_tie);
        }
    });
    c.sync_pattern(ui.track, c.track().pattern);
    c.notice(on ? "Tied into the next step" : "Tie off");
}

void insert(Context& c) {
    auto& ui = c.ui;
    unsigned q = c.pattern().q(), caret = edit_caret(ui, c.pattern());
    if (in_spare(ui, c.pattern())) { c.notice("Step " + std::to_string(caret + 1) + " is past the end"); return; }
    uint32_t at = caret * q;
    c.edit_pattern(ui.track, c.track().pattern, "Insert step", [&] {
        auto& p = c.pattern();
        std::vector<Event> kept;
        for (auto e : p.events) {
            if (e.tick >= at) e.tick += q;
            if (e.tick < p.length) kept.push_back(e); // the last step's notes fall off the end
        }
        p.events = std::move(kept);
        for (auto& m : p.motion) if (m.tick >= at) m.tick = std::min<uint32_t>(m.tick + q, p.length - 1);
    });
    c.sync_pattern(ui.track, c.track().pattern);
    c.notice("Step " + std::to_string(caret + 1) + " inserted");
}

void pad(Context& c, unsigned pad, uint8_t velocity, bool down) {
    if (sounds_open(c.ui)) { if (down) sounds_pad(c, pad); return; } // the sounds on the pads
    if (c.ui.step_pads) { steps_pad(c, pad, velocity, down); return; }
    int key = pad_key(c.project, c.track(), pad);
    if (down && c.ui.erase) { // Erase + pad: all of that pad's events, once B3 says so
        if (key >= 0) ask(c, UiState::Ask::EraseKeyEvents, key);
        return;
    }
    entry_pad(c, pad, velocity, down);
}

void wheel(Context& c, int delta) {
    if (sounds_open(c.ui)) { sounds_walk(c, delta); return; } // the next sound of them all
    if (c.ui.step_pads && held_wheel(c, delta)) return;
    if (seq_following(c)) return; // Settings > SEQ > Follow has the caret while it plays
    entry_move(c, delta);
}

// Back to the page Step Edit sits over (Patterns, Roll or Motion: the track's last).
void push(Context& c) {
    if (sounds_open(c.ui)) { sounds_close(c); return; } // the sounds on the pads close
    c.open(seq_editor_page(c.ui));
}

void soft(Context& c, int key) {
    auto& ui = c.ui;
    if (ui.shift) {
        ui.shift_used = true;
        if (key == 0) { ui.step_pads = !ui.step_pads; ui.held_step = -1; c.notice(ui.step_pads ? "Pads: the bar's steps" : "Pads: play to write"); }
        if (key == 1) tie(c);
        if (key == 2) insert(c);
        return;
    }
    if (sounds_open(ui)) { // the sounds on the pads: B1 CLEAR, B2 SLICES
        if (key == 0) sounds_clear(c);
        if (key == 1) sounds_slices(c);
        return;
    }
    if (key == 0) { // the next card set: NOTE, LOCK, TRIG, SOUND (a kit or keygroup), EUCLID (a kit or Drums lane)
        ui.step_cards = uint8_t(next_step_set(c.track(), ui.step_cards));
        return;
    }
    if (key == 1) entry_rest(c);
    else if (key == 2) entry_back(c);
}

// Minus / Plus: the octave the pads play (a kit's bank).
void nudge(Context& c, int way) {
    if (sounds_open(c.ui)) { sounds_track_step(c, way); return; } // the sounds of the next track
    auto& t = c.track();
    if (t.engine == EngineKind::Sampler) t.bank = uint8_t((t.bank + sampler_banks + way) % sampler_banks);
    else octave(c, way);
}
} // namespace

const PageSpec step_edit_page {draw, pad, wheel, push, soft, nudge};

} // namespace melodee
