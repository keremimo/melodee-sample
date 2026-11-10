// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// RECORD (Sample Record on a sampler track): records from the source picked
// on Input Config (Shift + Sample, or B2 here). Sample Record arms;
// recording starts when the level passes Input Config's threshold, or at
// once on a second press. It ends after the bars on K2 or on Stop (or a
// third press), is saved to Samples/Recorded on the card and lands on the
// first empty pad. K3 gain.
#include "melodee/engine/take_recorder.h"
#include "melodee/pages/common.h"
#include "melodee/pages/input_meter.h"
#include "melodee/pages/pages.h"
#include <cmath>

namespace melodee {
namespace {
using namespace ui;
using State = TakeRecorder::State;

void draw(Paint& p, const Snapshot& s) {
    auto c = colour_of(s);
    auto state = State(s.take.state);
    const auto& t = s.track();
    Header h = header_of(s);
    h.recording = state == State::Recording;
    int empty = -1;
    for (unsigned n = 0; n < sampler_pads && empty < 0; ++n) if (!t.pads[n].used()) empty = int(n);
    if (s.ui.rec_pad >= 0) empty = s.ui.rec_pad;
    if (empty >= 0) { char label[4]; pad_label(unsigned(empty), label); h.left.push_back(pill(juce::String("TO PAD ") + juce::String(label), c)); }
    else h.left.push_back(pill("NO EMPTY PAD"));
    h.left.push_back(pill(juce::String("FROM ") + input_source_name(s.ui.rec_source)));
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, {{"", "INPUT", ""}, -1, c}, h);

    float x = zone::pad, w = zone::width - 2 * zone::pad;
    const char* title = state == State::Armed ? "Waiting" : state == State::Recording ? "Recording" : state == State::Done ? "Saving" : "Ready";
    p.text(title, x, zone::picture_y, 160, 22, plain(colour::text, type::title, Weight::SemiBold));
    // How far: in bars when it has a length, else in seconds.
    double bar_frames = 4 * 60.0 / (s.project.tempo / 10.0) * 44100;
    unsigned bars = s.ui.rec_bars;
    juce::String where;
    if (state == State::Recording || state == State::Done)
        where = bars ? "BAR " + juce::String(std::min<unsigned>(bars, unsigned(s.take.frames / bar_frames) + 1)) + " OF " + juce::String(bars)
                     : juce::String(s.take.frames / 44100.0, 1) + " S";
    else where = bars ? juce::String(bars) + (bars == 1 ? " BAR" : " BARS") : juce::String("UNTIL STOP");
    p.text(where, x + 160, zone::picture_y, w - 160, 22, caps_text(colour::text, 7), juce::Justification::centredRight);

    // The take so far, against its bars.
    float y = zone::picture_y + 26, wh = 60, mid = y + wh / 2;
    unsigned shown = bars ? TakeRecorder::columns : TakeRecorder::columns;
    float cw = w / shown;
    for (unsigned b = 1; bars && b < bars; ++b) p.fill(x + w * b / bars, y, 1, wh, colour::line);
    for (unsigned n = 0; n < shown; ++n) {
        float half = std::max(0.5f, s.take.peaks[n] / 255.0f * wh / 2);
        p.fill(x + n * cw, mid - half, std::max(1.0f, cw * 0.6f), half * 2, c);
    }
    if (state == State::Recording && s.take.length) {
        float at = x + w * float(s.take.frames) / float(s.take.length);
        p.fill(at, y, 1, wh, colour::rec);
    }
    // The input level, with the line it has to pass.
    input_meter(p, s, c, x, y + wh + 12, w);
}

void soft(Context& c, int key) {
    if (key == 1) c.open(PageId::InputConfig);
}
void push(Context& c) { c.record_press(); }
} // namespace

const PageSpec record_page {draw, play_pad, no_wheel, push, soft, [](Context&, int) {}};

} // namespace melodee
