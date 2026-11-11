// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// FILTER ENV (7 of 10 sampler pages, the MPC's Filt Env): how the last
// pad's filter opens on each hit. K1 attack, K2 decay (One shot) or
// release (Note On), K3 depth (up to seven octaves over the cutoff);
// Shift + K2 decay from the start or the end; row 2 sustain and the other
// of decay and release.
// Drawn as Stage draws the same page (kit_page): the filter envelope over the pad's region.
#include "melodee/pages/sampler_common.h"

namespace melodee {
namespace {
void draw(ui::Paint& p, const Snapshot& s) { kit_page(p, s, StageKind::FiltEnv, s.ui.sampler_row & 1); }
} // namespace

const PageSpec filter_env_page {draw, play_pad, sampler_rows_wheel, sampler_push, sampler_soft, sampler_bank};

} // namespace melodee
