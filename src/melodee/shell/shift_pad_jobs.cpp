// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/shift_pad_jobs.h"

namespace melodee {
namespace {
using K = ShiftPadJob::Kind;
// Pads 1-16 in order; phases from MELODEE_PARITY.md.
constexpr ShiftPadJob jobs[16] {
    {"Full Level", K::Toggle, 0},   {"Half Seq", K::Action, 0},     {"Double Seq", K::Action, 0},  {"Count-In", K::Toggle, 2},
    {"Compressor", K::Later, 7},    {"Half Speed", K::Action, 0},  {"Double Speed", K::Action, 0}, {"MIDI Config", K::Later, 9},
    {"Fader", K::Later, 8},         {"Rec Quantize", K::Toggle, 2}, {"Resample", K::Later, 6},    {"Song", K::Open, 0},
    {"Trim Sample", K::Action, 0},   {"Time Correct", K::Open, 0}, {"Warp", K::Action, 0},         {"Project", K::Open, 0},
};
} // namespace

const ShiftPadJob& shift_pad_job(unsigned pad) { return jobs[pad % 16]; }

bool shift_pad_on(const UiState& ui, unsigned pad) {
    switch (pad % 16) {
    case 0: return ui.settings.full_level();
    case 3: return ui.settings.count_in;
    case 9: return ui.settings.rec_quantize;
    default: return false;
    }
}

unsigned shift_pad_level(const UiState& ui, unsigned pad) {
    const auto& job = shift_pad_job(pad);
    if (job.kind == K::Later) return 0;
    return job.kind == K::Toggle && shift_pad_on(ui, pad) ? 2 : 1;
}

} // namespace melodee
