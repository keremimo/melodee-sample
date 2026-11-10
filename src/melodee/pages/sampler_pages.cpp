// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The sampler's own pages after Trim and Mix, with the same grammar as a
// synth's: B1 and B3 name the neighbouring pages and go there
// (sampler_common.cpp holds the order). The first five follow the MPC's
// Sample Mode; the last three are Melodee's.
// AMP ENV (3 of 10): attack, decay (One shot) or release (Note On), velocity
//   sensitivity; Shift + K2 decay from the start or the end; row 2 sustain
//   and the other of decay and release. This pad only.
// TUNE (4 of 10): semitones, cents, Warp (Off, 50-200 %, Seq); Shift + K3 #
//   Beats. Shift + Pad 15 picks Time stretch or Pitch.
// PLAY (5 of 10): Mono or Poly, mute group 1-16 (pads in a group cut each
//   other), offset; Shift + K2 Pad Link; row 2 Note On, reverse, loop.
// KIT FILTER (8 of 10), for the whole kit: cutoff, resonance, type.
// FOLLOW (9 of 10), for the whole kit: on, pads keep the project's tempo by
//   speed, each from its own tempo worked out from its length as bars.
// CLOUD (10 of 10): pads as clouds of grains while held: K1 where, K2 grain
//   size, K3 grains a second; row 2 scan, spread and Cloud on or off.
#include "melodee/engine/sampler.h"
#include "melodee/pages/sampler_common.h"
#include "melodee/ui/waveform.h"
#include "melodee/ui/widgets.h"
#include <cmath>

namespace melodee {
namespace {
using namespace ui;

juce::String warp_text(const PadSound& pad) {
    if (pad.warp == warp_seq) return "SEQ";
    if (pad.warp >= warp_min) return juce::String(pad.warp) + " %";
    return "OFF";
}

void pad_draw(Paint& p, const Snapshot& s) { // Tune
    sampler_top(p, s, sampler_pad_pill(s), false);
    const auto& pad = sampler_pad(s);
    auto c = colour_of(s);
    float x = zone::pad, w = zone::width - 2 * zone::pad, y = zone::picture_y + 30;
    // Pitch on a line from -12 to +12 (beyond, it sits at the end).
    p.text("PITCH", x, y, 60, 9, caps_text(colour::mid, 7));
    p.text((pad.pitch >= 0 ? "+" : "") + juce::String(pad.pitch) + " ST", x, y, w, 9, caps_text(colour::text, 7), juce::Justification::centredRight);
    p.fill(x, y + 18, w, 1, colour::line);
    for (int st = -12; st <= 12; ++st) p.fill(x + w * (st + 12) / 24.0f, y + (st % 12 ? 15 : 12), 0.5f, st % 12 ? 6 : 12, colour::dim);
    float px = x + w * (std::clamp<int>(pad.pitch, -12, 12) + 12) / 24.0f;
    p.fill(px - 0.5f, y + 10, 1.5f, 16, c);
    p.fill(px - 3, y + 7, 6, 6, c, 1.5f);
    // Fine, and the length as it plays: pitch, warp.
    float fy = y + 44;
    p.text("FINE", x, fy, 40, 9, caps_text(colour::mid, 7));
    p.fill(x, fy + 16, w * 0.32f, 1, colour::line);
    p.fill(x + w * 0.32f * (pad.fine + 50) / 100.0f - 0.5f, fy + 11, 1.5f, 10, knob_shade(s.ui.track, 1));
    bool pitch_mode = s.track().params[sampler::FollowHow] == 2;
    if (s.sample.frames && pad.used()) {
        uint32_t end = pad.end ? pad.end : s.sample.frames, length = end > pad.start ? end - pad.start : 0;
        double seconds = length / 44100.0, played = seconds / std::exp2((pad.pitch + pad.fine / 100.0) / 12.0);
        juce::String warp = "WARP " + warp_text(pad);
        if (pad.warp >= warp_min) played = seconds * pad.warp / 100.0;
        if (pad.warp == warp_seq) {
            double own = pad.beats ? pad.beats * 60.0 / seconds : pad.own_tempo / 10.0, now = s.project.tempo / 10.0;
            if (own > 0) { played = seconds * own / now; warp += " " + juce::String(own, 1) + " BPM"; }
        }
        p.text(warp, x + w * 0.4f, fy, w * 0.3f, 9, caps_text(colour::mid, 7));
        p.text(juce::String(played, 3) + " S AS IT PLAYS", x, fy, w, 9, caps_text(colour::mid, 7), juce::Justification::centredRight);
        float from = float(pad.start) / s.sample.frames, to = float(end) / s.sample.frames;
        waveform(p, x + w * 0.4f, fy + 13, w * 0.6f, 14, s.sample.overview, from, to, from, to, c, 1);
    }
    juce::String beats = pad.beats ? juce::String(pad.beats) + " BEATS" : juce::String("BEATS FROM ITS LENGTH");
    sampler_pills(p, y + 82, {pitch_mode ? "WARP BY PITCH" : "WARP BY TIME STRETCH", beats}, c);
}

void play_draw(Paint& p, const Snapshot& s) {
    sampler_top(p, s, sampler_pad_pill(s), true);
    const auto& t = s.track();
    const auto& pad = sampler_pad(s);
    auto c = colour_of(s);
    float x = zone::pad, y = zone::picture_y + 30;
    p.text("POLYPHONY", x, y, 100, 9, caps_text(colour::mid, 7));
    const char* modes[] {"Poly", "Mono"};
    for (unsigned n = 0; n < 2; ++n) {
        bool on = unsigned(pad.mono) == n;
        if (on) p.circle(x + 2, y + 21 + n * 16, 2, c);
        p.text(modes[n], x + 8, y + 15 + n * 16, 120, 12, plain(on ? colour::text : colour::dim, 11, Weight::Medium));
    }
    auto line = [&](float ly, const juce::String& name, const juce::String& value, bool lit) {
        p.text(name, x, ly, 60, 12, caps_text(colour::mid, 7));
        p.text(value, x + 60, ly, 90, 12, plain(lit ? colour::text : colour::dim, 10, Weight::Medium));
    };
    char label[4];
    pad_label(t.last_pad / 16 * 16 + (pad.link + 15) % 16, label);
    line(y + 52, "OFFSET", pad.offset ? juce::String(pad.offset) + " %" : juce::String("None"), pad.offset != 0);
    line(y + 66, "PAD LINK", pad.link ? juce::String(label) : juce::String("Off"), pad.link != 0);
    line(y + 80, "PLAYS", juce::String(pad.play == PadPlay::Gate ? "Note On" : "One shot") + (pad.loop ? ", loop" : "") + (pad.reverse ? ", reversed" : ""), true);
    // The pads of its mute group, as they sit, with their names.
    float gx = x + 158, gw = 72, cell = (gw - 9) / 4;
    p.text(pad.group ? "GROUP " + juce::String(pad.group) + " CUTS EACH OTHER" : juce::String("IN NO GROUP"), gx, y, 160, 9, caps_text(colour::mid, 7));
    unsigned bank = t.last_pad / 16, listed = 0;
    for (unsigned n = 0; n < 16; ++n) {
        unsigned kit = bank * 16 + n, row = 3 - n / 4, column = n % 4;
        bool same = pad.group && t.pads[kit].group == pad.group;
        bool linked = pad.link && n == unsigned(pad.link - 1) % 16;
        p.fill(gx + column * (cell + 3), y + 14 + row * (cell + 3), cell, cell, same ? (kit == t.last_pad ? c : c.withAlpha(0.6f)) : colour::surf, 2);
        if (linked) p.ring(gx + column * (cell + 3), y + 14 + row * (cell + 3), cell, cell, colour::text, 2, 1);
        if (same && listed < 4) {
            char name[4]; pad_label(kit, name);
            float ly = y + 16 + listed * 14;
            p.fill(gx + gw + 8, ly + 3, 5, 5, c, 1);
            p.text(juce::String(name), gx + gw + 16, ly, 22, 11, caps_text(colour::mid, 6));
            p.text(s.project.samples[t.pads[kit].sample % sample_slots].name, gx + gw + 38, ly, 60, 11, plain(colour::text, 9, Weight::Medium));
            ++listed;
        }
    }
}

void envelope_draw(Paint& p, const Snapshot& s) { // Amp env
    sampler_top(p, s, sampler_pad_pill(s), true);
    const auto& pad = sampler_pad(s);
    float x = zone::pad, w = zone::width - 2 * zone::pad, y = zone::picture_y + 30, h = 78;
    if (!s.sample.frames || !pad.used()) return;
    uint32_t end = pad.end ? pad.end : s.sample.frames;
    float from = float(pad.start) / s.sample.frames, to = float(end) / s.sample.frames;
    waveform(p, x, y, w, h, s.sample.overview, from, to, from, to, colour_of(s).withAlpha(0.6f));
    bool gate = pad.play == PadPlay::Gate, from_end = !gate && !pad.decay_from_start;
    double length = (end - pad.start) / 44100.0;
    double decay = time_seconds(pad.decay) * s.track().params[sampler::Decay] / 100.0;
    bool hold = pad.decay >= 100;
    draw_envelope(p, x, y, w, h, length, time_seconds(pad.attack) * 0.3, hold ? length : decay, hold ? 1.0f : pad.sustain / 100.0f,
                  time_seconds(pad.release) * 0.5, from_end && !hold, s.ui.track);
    juce::String how = gate ? juce::String("NOTE ON: RELEASE") : from_end ? juce::String("DECAY FROM END") : juce::String("DECAY FROM START");
    sampler_pills(p, y + h + 6, {how, "VEL SENS " + juce::String(pad.velocity_sense) + " %", "THIS PAD ONLY"}, colour_of(s));
}

void filter_draw(Paint& p, const Snapshot& s) { // the kit's filter
    sampler_top(p, s, "WHOLE KIT", false);
    const auto& v = s.track().params;
    auto c = colour_of(s);
    float x = zone::pad, w = zone::width - 2 * zone::pad, y = zone::picture_y + 30, h = 70;
    float cutoff = 20.0f * std::exp2(v[sampler::Filter] / 100.0f * 9.97f), q = 1 / std::max(0.05f, 2 - 1.9f * v[sampler::Resonance] / 100.0f);
    unsigned shape = v[sampler::FilterType] == 1 ? 1 : v[sampler::FilterType] == 2 ? 2 : 0;
    draw_filter_curve(p, x, y, w, h, shape, 2, cutoff, q, c, knob_shade(s.ui.track, 0));
    static const char* types[] {"LOW PASS", "BAND PASS", "HIGH PASS"};
    sampler_pills(p, y + h + 16, {juce::String(types[v[sampler::FilterType] % 3]) + juce::String::fromUTF8(" \xc2\xb7 12 DB"), "AFTER EVERY PAD"}, c);
}

void follow_draw(Paint& p, const Snapshot& s) {
    sampler_top(p, s, "WHOLE KIT", false);
    const auto& pad = sampler_pad(s);
    auto c = colour_of(s);
    bool on = s.track().params[sampler::Follow] != 0;
    float x = zone::pad, w = zone::width - 2 * zone::pad, y = zone::picture_y + 28;
    if (!pad.used() || !s.sample.frames) { p.text("Hit a pad with a loop on it", x, y + 20, w, 20, plain(colour::dim, 11, Weight::Medium), juce::Justification::centred); return; }
    uint32_t end = pad.end ? pad.end : s.sample.frames;
    double length = (end - pad.start) / 44100.0, own = pad.own_tempo / 10.0, project = s.project.tempo / 10.0;
    double played = on && own > 0 ? length * own / project : length;
    float from = float(pad.start) / s.sample.frames, to = float(end) / s.sample.frames;
    auto line = [&](float ly, const juce::String& left, double seconds, float width) {
        p.text(left, x, ly, w, 9, caps_text(colour::mid, 7));
        p.text(juce::String(seconds, 3) + " S", x, ly, w, 9, caps_text(colour::text, 7), juce::Justification::centredRight);
        waveform(p, x, ly + 11, w * width, 26, s.sample.overview, from, to, from, to, c, 1);
    };
    double longest = std::max(length, played);
    line(y, own > 0 ? "ITS OWN TEMPO " + juce::String(own, 0) + " BPM" : juce::String("ITS OWN TEMPO: TURN FOLLOW ON"), length, float(length / longest));
    line(y + 44, "IN THE PROJECT " + juce::String(project, 1) + " BPM", played, float(played / longest));
    static const char* hows[] {"FOLLOWING BY BEATS", "FOLLOWING BY TONES", "FOLLOWING BY TAPE"};
    unsigned how = unsigned(s.track().params[sampler::FollowHow]) % 3;
    sampler_pills(p, y + 88, {on ? hows[how] : "NOT FOLLOWING",
                              s.track().params[sampler::PitchHow] ? "PITCH KEEPS LENGTH" : "PITCH MOVES SPEED"}, c);
}

void cloud_draw(Paint& p, const Snapshot& s) {
    const auto& v = s.track().params;
    sampler_top(p, s, v[sampler::Cloud] ? "CLOUD ON" : "CLOUD OFF", true);
    const auto& pad = sampler_pad(s);
    auto c = colour_of(s);
    float x = zone::pad, w = zone::width - 2 * zone::pad, y = zone::picture_y + 30, h = 74;
    if (!pad.used() || !s.sample.frames) return;
    uint32_t end = pad.end ? pad.end : s.sample.frames;
    float from = float(pad.start) / s.sample.frames, to = float(end) / s.sample.frames;
    waveform(p, x, y, w, h, s.sample.overview, from, to, from, to, c.withAlpha(0.5f));
    float at = x + w * v[sampler::Position] / 100.0f, size = std::max(2.0f, w * float(0.01f * std::pow(100.0f, v[sampler::Size] / 100.0f) * 44100 / std::max(1u, end - pad.start)));
    p.fill(at - size / 2, y, size, h, colour::text.withAlpha(0.12f));
    p.fill(at, y - 3, 1, h + 3, knob_shade(s.ui.track, 0));
    // A few grains round the place, as dots in K2's shade.
    for (unsigned g = 0; g < 9; ++g) {
        float gx = at + (std::sin(g * 2.3f) * 0.5f) * size, gy = y + h * (0.25f + 0.5f * std::fmod(g * 0.37f, 1.0f));
        p.fill(gx, gy, 6, 3, knob_shade(s.ui.track, 1), 1.5f);
    }
    double centre = (pad.start + (end - pad.start) * v[sampler::Position] / 100.0) / 44100.0;
    p.text("GRAINS AROUND " + juce::String(centre, 3) + " S", x, y + h + 3, w, 9, caps_text(colour::sec, 7), juce::Justification::centred);
    juce::String scan = v[sampler::Scan] == 50 ? juce::String("SCAN STILL") : "SCAN " + juce::String((v[sampler::Scan] - 50) / 50.0, 2);
    sampler_pills(p, y + h + 16, {scan, "SPREAD " + juce::String(v[sampler::Spread]) + " CT"}, colour::mid);
}
} // namespace

const PageSpec pad_edit_page {pad_draw, play_pad, no_wheel, no_push, sampler_soft, sampler_bank};
const PageSpec play_edit_page {play_draw, play_pad, sampler_rows_wheel, no_push, sampler_soft, sampler_bank};
const PageSpec envelope_page {envelope_draw, play_pad, sampler_rows_wheel, no_push, sampler_soft, sampler_bank};
const PageSpec filter_page {filter_draw, play_pad, no_wheel, no_push, sampler_soft, sampler_bank};
const PageSpec follow_page {follow_draw, play_pad, no_wheel, no_push, sampler_soft, sampler_bank};
const PageSpec cloud_page {cloud_draw, play_pad, sampler_rows_wheel, no_push, sampler_soft, sampler_bank};

} // namespace melodee
