// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The helpers around playing:
// TEMPO (hold Tap Tempo): K1 tempo, K2 swing for the whole project, K3 click
//   level; B1 halves, B3 doubles, B2 CLICK (or Shift + Tap Tempo anywhere)
//   the metronome Off -> On -> Rec (only while Seq Record), its LED green on,
//   amber on Rec; the wheel moves the tempo by 0.1; Push switches BPM per
//   pattern and one for the project, named in the header. Tapping works from
//   anywhere. The picture is a ring that beats: sixteen marks for the
//   sixteenths, the swung ones off their places by the swing, an arc sweeping
//   the bar as it plays, the tempo in the middle.
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include <cmath>

namespace melodee {
namespace {
using namespace ui;

// ---------- Tempo ----------
void tempo_draw(Paint& p, const Snapshot& s) {
    Header h = header_of(s);
    // Push: BPM per pattern (each pattern number its own tempo, the one launched last leading) or one for the project.
    h.left.push_back(pill(s.project.seq_tempo ? "P" + juce::String(s.ui.lead_pattern % pattern_count + 1) : juce::String("PROJECT"),
                          s.project.seq_tempo ? colour::text : colour::mid));
    h.right.push_back(label(s.project.name));
    SoftKeys keys {{"HALF", "CLICK", "DOUBLE"}, -1, colour::text};
    keys.toggles = 2; keys.lit = s.ui.settings.click ? 2 : 0;
    keys.leds[1] = s.ui.settings.click == 2 ? colour::amber : colour::play;
    frame(p, s, keys, h);
    // The ring: the bar swept as it plays, its sixteenths marked (beats longer), the swung ones moved by the swing.
    constexpr float cx = zone::width / 2.0f, cy = zone::picture_y + 72, r = 56;
    juce::Path ring;
    ring.addCentredArc(cx, cy, r, r, 0, 0, juce::MathConstants<float>::twoPi, true);
    p.g.setColour(colour::line);
    p.g.strokePath(ring, juce::PathStrokeType(6));
    unsigned bar = std::max(1u, s.project.bar_ticks());
    if (s.playing) {
        float phase = float(s.ticks % bar) / bar;
        juce::Path sweep;
        sweep.addCentredArc(cx, cy, r, r, 0, 0, phase * juce::MathConstants<float>::twoPi, true);
        p.g.setColour(colour::play);
        p.g.strokePath(sweep, juce::PathStrokeType(6, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
    }
    float swing = std::clamp((s.project.swing - 50) / 25.0f, 0.0f, 1.0f) * 0.5f; // of a sixteenth, at 75 % half of one
    for (unsigned n = 0; n < 16; ++n) {
        float at = (float(n) + (n % 2 ? swing : 0)) / 16 * juce::MathConstants<float>::twoPi;
        float r0 = n % 4 == 0 ? 44.0f : 47.5f, sx = std::sin(at), sy = -std::cos(at); // clear of the number in the middle
        juce::Path tick;
        tick.startNewSubPath(cx + r0 * sx, cy + r0 * sy); tick.lineTo(cx + 51 * sx, cy + 51 * sy);
        p.g.setColour(n % 2 ? knob_shade(s.ui.track, 1) : n % 4 == 0 ? colour::text : colour::mid);
        p.g.strokePath(tick, juce::PathStrokeType(n % 4 == 0 ? 2.0f : 1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    char text[16];
    std::snprintf(text, sizeof(text), "%u.%u", s.tempo / 10, s.tempo % 10);
    p.text(text, cx - 40, cy - 16, 80, 30, plain(colour::text, 24, Weight::Medium), juce::Justification::centred);
    p.text("BPM", cx - 30, cy + 12, 60, 14, caps_text(colour::mid), juce::Justification::centred);
}
void tempo_soft(Context& c, int key) {
    if (key == 0) c.set_tempo(int(c.tempo_now()) / 2);
    if (key == 2) c.set_tempo(int(c.tempo_now()) * 2);
    if (key == 1) c.set_click(c.ui.settings.click + 1u);
}
void tempo_wheel(Context& c, int delta) { c.set_tempo(int(c.tempo_now()) + delta); }

} // namespace

const PageSpec tempo_page {tempo_draw, play_pad, tempo_wheel, [](Context& c) {
                               c.set_seq_tempo(!c.project.seq_tempo); // Push: per pattern or global
                               c.notice(c.project.seq_tempo ? "BPM per pattern" : "One BPM for the project");
                           }, tempo_soft,
                           [](Context& c, int way) { c.set_tempo(int(c.tempo_now()) + way * 10); }}; // Minus / Plus: a whole BPM

} // namespace melodee
