// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/fader.h"
#include "melodee/shell/ask.h"
#include "melodee/shell/context.h"
#include "melodee/shell/fader_job.h"
#include "melodee/shell/takeover.h"
#include <algorithm>
#include <cstdlib>

namespace melodee {
namespace {

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
    if (c.pattern().has_lane(lane_param(lane), lane_pad(lane) % sampler_pads))
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
    if (ui.erase) ui.erase_used = true; // Erase + fader: its release deletes nothing
    if (ui.ask != UiState::Ask::None) return; // a question open: the fader waits, as the pots do
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
    t.fader_learned = {}; // the pot it followed gives way
    t.fader_on = true;
    c.notice("Fader: " + std::string(fader_target_name(FaderTarget(target))));
}

void learn_fader(Context& c, const Binding& b, unsigned pot) {
    auto& t = c.track();
    if (!t.used() || t.fader_pin) return;
    auto learned = fader_learnable(t, c.ui.track, b);
    if (!learned.learned()) return; // not one of the track's values (a tempo, a pattern's length, a bus's effect): it keeps what it has
    learned.pot = uint8_t(pot % 3);
    t.fader_learned = learned;
}

void step_fader_target(Context& c, int delta) { // over the targets the track can take, from the one it has
    auto& t = c.track();
    if (!t.used() || delta == 0) return;
    bool kit = t.engine == EngineKind::Sampler;
    int at = int(fader_target(t)), step = delta > 0 ? 1 : -1;
    for (int left = std::abs(delta); left > 0; --left)
        for (int n = at + step; n >= 0 && n < int(fader_targets); n += step)
            if (kit || !fader_needs_kit(FaderTarget(n))) { at = n; break; }
    set_fader_target(c, unsigned(at));
}

void toggle_fader_pin(Context& c) {
    auto& t = c.track();
    if (!t.used()) return;
    t.fader_pin = !t.fader_pin;
    c.notice(t.fader_pin ? "Fader pinned" : "Fader follows the pots");
}

void toggle_fader(Context& c) {
    auto& t = c.track();
    if (!t.used()) return;
    t.fader_on = !t.fader_on;
    c.notice(t.fader_on ? "Fader on" : "Fader off");
}

} // namespace melodee
