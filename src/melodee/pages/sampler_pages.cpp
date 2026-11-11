// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The sampler's own pages after Trim and Mix, with the same grammar as a
// synth's: B1 and B3 name the neighbouring pages and go there
// (sampler_common.cpp holds the order). The first five follow the MPC's
// Sample Mode; the last three are Melodee's. Each draws as Stage draws the
// same page (kit_page): the pad's region with what the page shapes over it,
// so a page looks the same whichever way it was reached.
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
#include "melodee/pages/sampler_common.h"

namespace melodee {
namespace {
using namespace ui;

int row(const Snapshot& s) { return s.ui.sampler_row & 1; }
void tune_draw(Paint& p, const Snapshot& s) { kit_page(p, s, StageKind::Tune, -1); }
void play_draw(Paint& p, const Snapshot& s) { kit_page(p, s, StageKind::Play, row(s)); }
void envelope_draw(Paint& p, const Snapshot& s) { kit_page(p, s, StageKind::AmpEnv, row(s)); }
void filter_draw(Paint& p, const Snapshot& s) { kit_page(p, s, StageKind::KitFilter, -1); }
void follow_draw(Paint& p, const Snapshot& s) { kit_page(p, s, StageKind::Follow, -1); }
void cloud_draw(Paint& p, const Snapshot& s) { kit_page(p, s, StageKind::Cloud, row(s)); }
} // namespace

const PageSpec pad_edit_page {tune_draw, play_pad, sampler_wheel, sampler_push, sampler_soft, sampler_bank};
const PageSpec play_edit_page {play_draw, play_pad, sampler_rows_wheel, sampler_push, sampler_soft, sampler_bank};
const PageSpec envelope_page {envelope_draw, play_pad, sampler_rows_wheel, sampler_push, sampler_soft, sampler_bank};
const PageSpec filter_page {filter_draw, play_pad, sampler_wheel, sampler_push, sampler_soft, sampler_bank};
const PageSpec follow_page {follow_draw, play_pad, sampler_wheel, sampler_push, sampler_soft, sampler_bank};
const PageSpec cloud_page {cloud_draw, play_pad, sampler_rows_wheel, sampler_push, sampler_soft, sampler_bank};

} // namespace melodee
