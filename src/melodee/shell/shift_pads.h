// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/shell/context.h"

// Shift + Pad 1-16: the jobs printed under the pads (shift_pad_jobs.h), on
// every page. Song, Project, Double Seq and the toggles Full Level,
// Count-In and Rec Quantize work; the rest say which phase brings them.
namespace melodee {

// A pad struck with Shift held (not over the Name or Exit sheet, nor with
// Pad Bank or Mute held: those come first).
void shift_pad(Context& c, unsigned pad);

} // namespace melodee
