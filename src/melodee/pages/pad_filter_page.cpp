// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// FILTER (6 of 10 sampler pages, the MPC's Sample Mode Filter): the last
// pad's own filter. K1 cutoff, K2 resonance, K3 type: Off, Classic (a
// low pass that saturates as it rings, after the MPC3000), LPF2 / LPF4,
// HPF2 / HPF4, BPF2 / BPF4 (2: 12 dB an octave, 4: 24). The kit's filter
// is the Kit filter page's, after every pad.
// Drawn as Stage draws the same page (kit_page): the filter over the pad's region.
#include "melodee/pages/sampler_common.h"

namespace melodee {
namespace {
void draw(ui::Paint& p, const Snapshot& s) { kit_page(p, s, StageKind::Filter, -1); }
} // namespace

const PageSpec pad_filter_page {draw, play_pad, sampler_wheel, sampler_push, sampler_soft, sampler_bank};

} // namespace melodee
