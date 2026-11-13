// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/ui/paint.h"
#include <array>
#include <cstdint>

// A sample drawn as the boards draw it: thin bars from the centre line,
// one per column of its overview (512 peaks). `from`..`to` is the part of
// the sample in view (0..1); `keep_from`..`keep_to` is drawn in `colour`,
// the rest dim.
namespace melodee::ui {
void waveform(Paint& p, float x, float y, float w, float h, const std::array<uint8_t, 512>& overview,
              float from, float to, float keep_from, float keep_to, juce::Colour colour, float bar = 1.5f);

// Where a voice is heard in the sample, and the wake behind it: the part it
// has played since `wake_from`, both as a share of the file (0..1).
struct WaveHead {
    float at = 0;
    float wake_from = 0;  // the wake runs from here to `at`, either way round (reversed: from the region's end)
    float strength = 0;   // the head's line: its envelope, 0..1 (0: no line, the wake may still show)
    float wake = 0;       // 0..1: 1 while it sounds, fading once it is silent
    bool ground = true;   // the wake's faint fill under the bars (a cloud's grains light their bars only)
};

// The waveform with heads over it (mock 1, "a head and its wake"): each
// wake a faint fill and its bars lighter (the colour mixed 0.45 toward the
// text), the bars within 2 px of a head in the text colour. Then draw_heads().
void waveform(Paint& p, float x, float y, float w, float h, const std::array<uint8_t, 512>& overview,
              float from, float to, float keep_from, float keep_to, juce::Colour colour,
              const WaveHead* heads, unsigned count, float bar = 1.5f);
// Zoomed in (Trim, Chop): finer peaks of part of the sample, `from`..`to` of it (0..1), read from the audio; the
// columns inside it take theirs from these instead of the overview.
struct WaveDetail {
    const std::array<uint8_t, 512>* peaks = nullptr;
    float from = 0, to = 0;
};
void waveform(Paint& p, float x, float y, float w, float h, const std::array<uint8_t, 512>& overview,
              float from, float to, float keep_from, float keep_to, juce::Colour colour,
              const WaveHead* heads, unsigned count, const WaveDetail& detail, float bar = 1.5f);
// A stereo sample's sides: each side's overview, and its zoomed-in peaks (as WaveDetail's, over the same part).
// Both overviews given: the waveform above is drawn twice, the left side in the top half, the right in the
// bottom, 2 px apart; else once, from `overview`.
struct WaveSides {
    const std::array<uint8_t, 512>* overview[2] = {nullptr, nullptr};
    const std::array<uint8_t, 512>* detail[2] = {nullptr, nullptr};
    bool stereo() const { return overview[0] && overview[1]; }
};
void waveform(Paint& p, float x, float y, float w, float h, const std::array<uint8_t, 512>& overview, const WaveSides& sides,
              float from, float to, float keep_from, float keep_to, juce::Colour colour,
              const WaveHead* heads, unsigned count, const WaveDetail& detail, float bar = 1.5f);
// Each head a 1.5 px line in the text colour down the waveform's height at
// its envelope's strength; with `cap`, a small triangle sits on it above `y`
// (4 px). Heads outside `from`..`to` are not drawn.
void draw_heads(Paint& p, float x, float y, float w, float h, float from, float to, const WaveHead* heads, unsigned count,
                bool cap = true);
}
