// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// MIX (2 of 10 sampler pages, the MPC's Sample Mode Mix): the last pad's
// volume (K1, in dB: -inf, -74 to +6) with its meter on the left, its pan
// (K3) on the right, and Shift + K2 the kit's volume, the same scale.
// Drawn as Stage draws the same page (kit_page): the pad's region, its volume on the left and pan on the right.
#include "melodee/pages/sampler_common.h"

namespace melodee {
namespace {
void draw(ui::Paint& p, const Snapshot& s) { kit_page(p, s, StageKind::PadMix, -1); }
} // namespace

const PageSpec pad_mix_page {draw, play_pad, sampler_wheel, sampler_push, sampler_soft, sampler_bank};

} // namespace melodee
