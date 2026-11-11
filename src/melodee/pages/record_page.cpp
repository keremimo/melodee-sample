// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// RECORD (Sample Record on a sampler track), the MPC's Sample Record Mode:
// tap the pad to record onto; the take starts when the level passes the
// threshold (at once at -96 dB; with rec length Seq while playing, where the
// pattern comes round). During the take the other pads tapped cut it there,
// each getting the part from its cut on. The pad again, Stop or Push ends
// it and Record stays; Sample Record ends it and goes back to the pad's
// Sample page. K1 source, K2 rec length, K3 threshold, B1 monitor, as on the
// MPC and in step with Input Config (B2 opens it); Shift + K3 gain. Push on
// its own records onto the first empty pad. Takes are saved to
// Samples/Recorded on the card. With Melodee's own sound as the source (a
// track, Resample) the pads play the instrument from the tap until the take
// ends (Context::record_keys(); the shell routes them before this page).
#include "melodee/engine/take_recorder.h"
#include "melodee/pages/common.h"
#include "melodee/pages/input_meter.h"
#include "melodee/pages/pages.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace melodee {
namespace {
using namespace ui;
using State = TakeRecorder::State;

void draw(Paint& p, const Snapshot& s) {
    auto c = colour_of(s);
    auto state = State(s.take.state);
    const auto& in = s.ui.settings.input;
    Header h = header_of(s);
    h.recording = state == State::Recording;
    bool taking = s.ui.rec_pad >= 0 && s.ui.rec_track == s.ui.track;
    char label[4];
    if (taking) { pad_label(unsigned(s.ui.rec_pad), label); h.left.push_back(pill(Icon::Pads, juce::String(label), c)); }
    h.left.push_back(pill(source_icon(in.source), juce::String(in.source == InputSource::Track ? "T" + juce::String(in.track % track_count + 1)
                                                                                             : juce::String(input_source_name(in.source))).toUpperCase()));
    if (s.speaker_quieted) h.left.push_back(pill(Icon::SpeakerOff, "", colour::amber));
    h.right.push_back(tag_of(s, s.ui.track));
    SoftKeys keys {{"MONITOR", "INPUT", ""}, -1, c};
    keys.toggles = 1;
    keys.lit = in.monitor != Monitor::Off ? 1 : 0;
    keys.leds[0] = in.monitor == Monitor::Auto ? colour::amber : colour::play;
    keys.glyphs[1] = source_icon(in.source);
    frame(p, s, keys, h);

    float x = zone::pad, w = zone::width - 2 * zone::pad;
    // At the corner the state as a glyph (ready, waiting for the level or the loop, recording, saving) and the time so far.
    Icon glyph = state == State::Armed ? Icon::Ring : state == State::Cued ? Icon::Loop : state == State::Recording ? Icon::Rec
               : state == State::Done || (taking && state == State::Idle) ? Icon::Check : Icon::Pads;
    juce::Colour tone = state == State::Recording ? colour::rec : state == State::Armed || state == State::Cued ? colour::amber : colour::mid;
    double bar_frames = double(s.project.bar_ticks()) / ticks_per_quarter * 60.0 / (s.tempo / 10.0) * 44100;
    unsigned bars = rec_bars(in.length);
    bool going = state == State::Recording || state == State::Done;
    juce::String elapsed = going ? juce::String(s.take.frames / 44100.0, 1) + " s" : in.length == rec_seq ? juce::String("SEQ") : juce::String();
    auto time_style = plain(colour::text, type::body, Weight::SemiBold);
    float tw = p.width(elapsed, time_style);
    p.text(elapsed, x + w - tw, zone::picture_y + 1, tw + 1, 16, time_style);
    icon(p, glyph, x + w - tw - (tw > 0 ? 18 : 12), zone::picture_y + 3, 12, tone, 1.6f);

    // The take in its frame: with a length in bars, that many bars numbered (the one being taken lit), so what is
    // left shows; a take without one (Free, up to 60 s) grows across the frame, an eighth of it at least.
    float y = zone::picture_y + 18, wh = 62, mid = y + wh / 2;
    p.fill(x, y, w, wh, colour::panel, 3);
    uint32_t length = std::max<uint32_t>(s.take.length, 1);
    unsigned shown = TakeRecorder::columns;
    if (length >= TakeRecorder::max_frames)
        shown = std::clamp<unsigned>(unsigned((uint64_t(s.take.frames) * TakeRecorder::columns + length - 1) / length), TakeRecorder::columns / 8,
                                     TakeRecorder::columns);
    float span = float(length) * shown / TakeRecorder::columns; // the frames across the picture
    unsigned frame_bars = going && length < TakeRecorder::max_frames && bar_frames > 0 ? std::max(1u, unsigned(length / bar_frames + 0.5)) : bars;
    unsigned now_bar = going && bar_frames > 0 ? unsigned(s.take.frames / bar_frames) : 0;
    if (frame_bars > 1 && frame_bars <= 16)
        for (unsigned b = 0; b < frame_bars; ++b) {
            float bx = x + w * b / frame_bars;
            if (b) p.fill(bx, y, 1, wh, colour::line);
            bool lit = going && b == now_bar;
            if (bx + 20 > x + w - tw - 22) continue; // clear of the time at the corner
            p.text(juce::String(b + 1), bx + 3, zone::picture_y + 1, 24, 16, plain(lit ? colour::text : colour::dim, type::small, Weight::SemiBold));
        }
    float cw = w / shown;
    for (unsigned n = 0; n < shown; ++n) {
        float half = std::max(0.5f, s.take.peaks[n] / 255.0f * (wh / 2 - 4));
        p.fill(x + n * cw, mid - half, std::max(1.0f, cw * 0.6f), half * 2, c);
    }
    if (taking)
        for (unsigned n = 0; n < s.ui.rec_chops; ++n) { // the cuts the pads made, each with its pad as a chip
            float at = x + w * std::min(1.0f, float(s.ui.rec_chop_at[n]) / span);
            p.fill(at, y, 1.2f, wh, colour::text.withAlpha(0.8f));
            pad_label(s.ui.rec_chop_pad[n], label);
            auto style = caps_text(colour::text);
            float lw = p.width(juce::String(label), style) + 6;
            p.fill(at + 3, y + 4, lw, 13, colour::raised, 3);
            p.text(juce::String(label), at + 3, y + 4, lw, 13, style, juce::Justification::centred);
        }
    if (state == State::Recording) { // where it has come to, and what is left of the frame
        float at = x + w * std::min(1.0f, float(s.take.frames) / span);
        p.fill(at - 1, y - 1, 2, wh + 2, colour::rec);
        for (float d = at + 4; d < x + w - 2; d += 6) p.fill(d, mid - 0.6f, 2, 1.2f, colour::dim);
    }
    // The input level, with the marker it has to pass.
    input_meter(p, s, c, x, y + wh + 4, w);
    // What the pads do now, as glyph pairs: a pad (the take's outlined, any other filled) and what it does.
    struct Pair { int pad; Icon does; }; // pad: 1 the take's pad, 2 another pad, 3 the sixteen pads, 0 none (Stop)
    std::array<Pair, 2> pairs {{{2, Icon::Rec}, {0, Icon::None}}};
    size_t count = 1;
    if (record_keys_shown(s)) pairs[0] = {3, Icon::Note};
    else if (state == State::Recording) { pairs = {{{1, Icon::Stop}, {2, Icon::Scissors}}}; count = 2; }
    else if (state == State::Armed || state == State::Cued) { pairs = {{{1, Icon::Play}, {0, Icon::Cross}}}; count = 2; }
    float total = 0;
    for (size_t n = 0; n < count; ++n) total += 12 + 6 + 12 + (n ? 20 : 0);
    float lx = zone::width / 2.0f - total / 2, ly = zone::picture_y + 130;
    for (size_t n = 0; n < count; ++n) {
        if (n) { p.circle(lx + 10, ly + 6, 1.3f, colour::dim); lx += 20; }
        const auto& pr = pairs[n];
        if (pr.pad == 1) p.ring(lx, ly, 12, 12, c, 2.5f, 1.6f);
        else if (pr.pad == 2) p.fill(lx, ly, 12, 12, colour::raised, 2.5f);
        else if (pr.pad == 3) icon(p, Icon::Pads, lx, ly, 12, colour::sec, 1.5f);
        else icon(p, Icon::Stop, lx, ly, 12, colour::sec);
        icon(p, pr.does, lx + 18, ly, 12, pr.does == Icon::Rec ? colour::rec : colour::sec, 1.5f);
        lx += 30;
    }
}

void pad(Context& c, unsigned pad, uint8_t, bool down) {
    if (!down || c.track().engine != EngineKind::Sampler) return;
    if (c.ui.erase) { c.notice("Erase a pad on Pads or Trim"); return; } // here the pads record; nothing is erased by accident
    c.record_pad(c.track().bank % sampler_banks * 16 + pad);
}
void soft(Context& c, int key) {
    if (key == 0) c.cycle_monitor();
    if (key == 1) c.open(PageId::InputConfig);
}
void push(Context& c) { c.record_press(); }
} // namespace

const PageSpec record_page {draw, pad, no_wheel, push, soft, [](Context&, int) {}};

} // namespace melodee
