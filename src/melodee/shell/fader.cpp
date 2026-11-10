// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/fader.h"
#include "melodee/shell/ask.h"
#include "melodee/shell/context.h"
#include "melodee/shell/fader_job.h"
#include "melodee/shell/step_edit.h"
#include "melodee/shell/takeover.h"
#include <algorithm>
#include <cstdlib>

namespace melodee {
namespace {
constexpr uint32_t nudge_gesture_key = 0xFADE0000u; // no card's key: the fader's nudge is its own control

// Which value the fader drives, for takeover: the card, and for a pad's
// own value which pad (the last hit), so another pad is picked up afresh.
uint32_t job_key(const Context& c, const FaderJob& job) {
    if (!job.binding.bound()) return 0;
    uint32_t key = job.binding.key(c.ui.track);
    int lane = binding_lane(c.project.tracks[c.ui.track % track_count], job.binding);
    if (lane >= 0) key ^= uint32_t(lane_pad(lane)) << 8; // a pad's own value: which pad (0 for the rest)
    return key;
}

// A new value to drive, or the value moved by something else (a knob, an
// undo) since the fader last drove it: the fader takes it over afresh.
void check_takeover(Context& c, const FaderJob& job, int before) {
    auto& fader = c.ui.fader;
    uint32_t key = job_key(c, job);
    const auto& b = job.binding;
    if (fader.bound != key) {
        fader.bound = key;
        fader.scaled = -1;
        fader.engaged = c.ui.settings.fader_takeover == Takeover::Instant || takeover_meets(before, b);
        return;
    }
    if (fader.engaged && b.bound() && before >= 0) { // where the fader sits still names the value, within a step of the card
        int slack = std::max(2, 127 / std::max(1, b.max - b.min));
        if (std::abs(before - int(b.fraction() * 127 + 0.5f)) > slack) { fader.engaged = false; fader.scaled = -1; }
    }
}

// Erase + fader (as on the MPC): the lane of what it drives, once B3 says so.
void erase_lane(Context& c, const FaderJob& job) {
    const auto& track = c.track();
    int lane = binding_lane(track, job.binding);
    if (lane < 0) { c.notice("No motion lane for the fader here"); return; }
    if (c.project.motion.has(c.ui.track, track.pattern, uint8_t(lane_param(lane)), uint8_t(lane_pad(lane) % sampler_pads)))
        ask(c, UiState::Ask::EraseLane, lane);
    else c.notice("No motion on " + lane_name(track, lane));
}
} // namespace

void fader_move(Context& c, int value, bool syncing) {
    auto& ui = c.ui;
    int before = ui.fader.position;
    ui.fader.position = int16_t(value);
    auto job = fader_job(c.project, ui);
    check_takeover(c, job, before);
    if (syncing) return; // where it sits at start: nothing moves
    ui.fader_moved_at = c.now;
    if (ui.ask != UiState::Ask::None) return; // a question open: the fader waits, as the pots do
    if (job.source == FaderJob::Source::Nudge) { // Step Edit: by how far it went (its place means nothing here)
        if (before < 0) return;
        c.touch_scope(pattern_scope(c.project, ui), nudge_gesture_key); // a run of nudges is one undo step
        nudge_event(c, value - before);
        return;
    }
    if (!job.binding.bound()) return;
    if (ui.erase) { erase_lane(c, job); return; }
    int to = 0;
    if (takeover_move(ui.settings.fader_takeover, ui.fader, before, value, job.binding, to)) c.apply(job.binding, to);
}

void fader_check(Context& c) { check_takeover(c, fader_job(c.project, c.ui), c.ui.fader.position); }

void set_fader_target(Context& c, unsigned target) {
    auto& t = c.track();
    if (!t.used() || target >= fader_targets) return;
    if (fader_needs_kit(FaderTarget(target)) && t.engine != EngineKind::Sampler) {
        c.notice(std::string(fader_target_name(FaderTarget(target))) + ": kit tracks");
        return;
    }
    t.fader_target = uint8_t(target);
    t.fader_on = true;
    c.notice("Fader: " + std::string(fader_target_name(FaderTarget(target))));
}

void toggle_fader(Context& c) {
    auto& t = c.track();
    if (!t.used()) return;
    t.fader_on = !t.fader_on;
    c.notice(t.fader_on ? "Fader on" : "Fader off");
}

} // namespace melodee
