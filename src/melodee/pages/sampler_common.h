// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/engine_info.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/shell/sampler_map.h"
#include "melodee/ui/waveform.h"

// What the sampler's ten pages share (Trim, Mix, Amp Env, Tune, Play,
// Filter, Filt Env as the MPC has them, then Melodee's Kit filter, Follow and
// Cloud): their order and B1 / B2 / B3 groups, the page map (Push), title row, the last pad's pill and its icons (Note
// On, loop, reverse, as the MPC marks them), a row of small pills, and the
// wheel on pages with a second row.
namespace melodee {

void sampler_push(Context& c);                  // Push: the page map, or the page under its cursor
bool sampler_map_wheel(Context& c, int delta);  // the wheel while the map is open
void sampler_wheel(Context& c, int delta);      // a page without rows: only the map's
// The header with `pill_text`, the soft keys, the title with the pad's icons,
// and, with `rows`, ROW n / 2.
void sampler_top(ui::Paint& p, const Snapshot& s, const juce::String& pill_text, bool rows);
juce::String sampler_pad_pill(const Snapshot& s);          // "PAD A01 · KICK"
const PadSound& sampler_pad(const Snapshot& s);             // the last pad hit
juce::Colour pad_colour(const Snapshot& s, unsigned kit);   // kit pad `kit`'s own colour (Stage's COLOUR, an MPC kit's), else the track's
juce::Colour ink_on(juce::Colour fill);                     // type on a pad's colour: dark on a light one, light on a dark one
void sampler_pills(ui::Paint& p, float y, std::initializer_list<juce::String> texts, juce::Colour first);
// Note On (a note), Loop (a loop) and reverse (an arrow) from `x` on; returns where they end.
float pad_icons(ui::Paint& p, float x, float y, const PadSound& pad, juce::Colour colour);
// A filter's response from 20 Hz to 20 kHz, filled under the line: shape 0
// low, 1 band, 2 high pass; poles 2 or 4; cutoff in Hz, q its resonance.
void draw_filter_curve(ui::Paint& p, float x, float y, float w, float h, unsigned shape, unsigned poles, float cutoff, float q,
                       juce::Colour colour, juce::Colour marker, bool labels = true);
// An envelope's line over a stretch of `seconds`: attack, decay to `sustain`,
// held, release (seconds each); with `fade_end`, held at the top and faded
// over `decay` before the end (the MPC's Decay From End).
void draw_envelope(ui::Paint& p, float x, float y, float w, float h, double seconds, double attack, double decay, float sustain,
                   double release, bool fade_end, unsigned track, float soft = 1); // soft < 1: the softer hit's envelope, dashed

// The sample in hand's two sides for ui::waveform, and their zoomed-in peaks when there are some; none for a mono one.
ui::WaveSides wave_sides(const Snapshot& s);
// The newest voice of `pad` playing `sound`'s sample (or its wake, still
// fading) as a head over a file `frames` long (ui/waveform.h); nullptr: none.
// The wake runs from the region's start (reversed: its end; after a loop
// went round, its loop point) to the head.
const PlayHead* pad_head(const Snapshot& s, unsigned pad, const PadSound& sound, uint32_t frames, ui::WaveHead& out);
// Stage (mocks 2 and 3): one head a voice of `pad` (the four newest,
// shell/playheads.h pad_voices): the newest at its envelope with its wake,
// older ones at half and no wake. After a loop went round, the part before
// the loop point keeps half a wake.
struct StageHeads {
    std::array<ui::WaveHead, PadVoices::cap + 1> wave {};
    unsigned count = 0;               // heads in `wave`
    const PlayHead* newest = nullptr; // nullptr: nothing shown
    unsigned sounding = 0;            // voices heard now
};
StageHeads stage_heads(const Snapshot& s, unsigned pad, const PadSound& sound, uint32_t frames);
// Cloud (mock 6): the grains of `pad`'s cloud over a waveform `w` wide
// showing `from`..`to` of a file `frames` long. `wakes` lightens the bars a
// live grain covers (pass them to ui::waveform); draw_grains() then lays each
// grain as a 3 px pill from where it began to where it reads, in K2's shade at
// its window's strength, at its own height across the middle of the waveform.
struct CloudGrains {
    CloudView view;
    std::array<ui::WaveHead, CloudView::cap> wakes {};
    unsigned wake_count = 0;
    double centre = -1; // the frame the grains come from now (Scan moves it); -1: no cloud heard
};
CloudGrains cloud_grains(const Snapshot& s, unsigned pad, const PadSound& sound, uint32_t frames, float from, float to, float w);
void draw_grains(ui::Paint& p, float x, float y, float w, float h, float from, float to, uint32_t frames, const CloudGrains& cloud,
                 juce::Colour shade);
void sampler_rows_wheel(Context& c, int delta);
// A sample page drawn as Stage draws the same page (pages/stage_page.cpp): the pad's pill, the sample keys, the
// kit picture of `kind`; `row` (0 or 1) on pages with a second row of cards, -1 on the others.
void kit_page(ui::Paint& p, const Snapshot& s, StageKind kind, int row);
void sampler_bank(Context& c, int way);

} // namespace melodee
