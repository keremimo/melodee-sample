// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// RECORD (Sample Record on a sampler track): B1-B3 pick the source: the
// built-in mic, the line inputs, or the mix itself (resample). Sample
// Record arms; recording starts when the level passes the line on K1, or
// at once on a second press. It ends after the bars on K2 or on Stop (or a
// third press), is saved to Samples/Recorded on the card and lands on the
// first empty pad. K3 gain.
#include "melodee/engine/take_recorder.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include <cmath>

namespace melodee {
namespace {
using namespace ui;
using State = TakeRecorder::State;

float db_of(float level) { return level > 0 ? 20 * std::log10(level) : -90; }
juce::String db_text(float db) {
    int v = int(std::lround(db));
    return (v < 0 ? juce::String::fromUTF8("\xe2\x88\x92") : juce::String()) + juce::String(std::abs(v)) + " DB";
}

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
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, {{"MIC", "LINE", "RESAMPLE"}, s.ui.rec_source, c}, h);

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
    float ly = y + wh + 12;
    p.text("INPUT LEVEL", x, ly, 120, 9, caps_text(colour::mid, 7));
    p.text("PEAK " + db_text(std::max(-60.0f, db_of(s.take.level))), x, ly, w, 9, caps_text(colour::mid, 7), juce::Justification::centredRight);
    auto fraction = [](float db) { return std::clamp((db + 60) / 60, 0.0f, 1.0f); };
    p.fill(x, ly + 12, w, 6, colour::line, 3);
    p.fill(x, ly + 12, w * fraction(db_of(s.take.level)), 6, c, 3);
    if (s.ui.rec_threshold > -60) {
        float tx = x + w * fraction(float(s.ui.rec_threshold));
        p.fill(tx, ly + 9, 1, 12, colour::text);
        p.text("STARTS AT " + db_text(float(s.ui.rec_threshold)), tx - 60, ly + 22, 120, 9, caps_text(c, 7), juce::Justification::centred);
    } else {
        p.text("STARTS AT ONCE", x, ly + 22, w, 9, caps_text(c, 7), juce::Justification::centred);
    }
}

void soft(Context& c, int key) {
    if (c.host_recording()) return; // the source stays while a take runs
    c.ui.rec_source = uint8_t(key % 3);
}
void push(Context& c) { c.record_press(); }
} // namespace

const PageSpec record_page {draw, play_pad, no_wheel, push, soft, [](Context&, int) {}};

} // namespace melodee
