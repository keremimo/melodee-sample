// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/shell/sampler_map.h"

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
void sampler_pills(ui::Paint& p, float y, std::initializer_list<juce::String> texts, juce::Colour first);
// Note On (a note), Loop (a loop) and reverse (an arrow) from `x` on; returns where they end.
float pad_icons(ui::Paint& p, float x, float y, const PadSound& pad, juce::Colour colour);
// A filter's response from 20 Hz to 20 kHz, filled under the line: shape 0
// low, 1 band, 2 high pass; poles 2 or 4; cutoff in Hz, q its resonance.
void draw_filter_curve(ui::Paint& p, float x, float y, float w, float h, unsigned shape, unsigned poles, float cutoff, float q,
                       juce::Colour colour, juce::Colour marker);
// An envelope's line over a stretch of `seconds`: attack, decay to `sustain`,
// held, release (seconds each); with `fade_end`, held at the top and faded
// over `decay` before the end (the MPC's Decay From End).
void draw_envelope(ui::Paint& p, float x, float y, float w, float h, double seconds, double attack, double decay, float sustain,
                   double release, bool fade_end, unsigned track);
void sampler_rows_wheel(Context& c, int delta);
void sampler_bank(Context& c, int way);

} // namespace melodee
