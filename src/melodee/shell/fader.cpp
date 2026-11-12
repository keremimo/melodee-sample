// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/fader.h"
#include "melodee/engine/midi_engine.h"
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

// The takeover the job goes by: Settings' Fader takeover, but a pitch bend always picks up (from its middle).
Takeover takeover_of(const Context& c, const FaderJob& job) { return job.bend ? Takeover::Pickup : c.ui.settings.fader_takeover; }

// A new value to drive, or the value moved by something else (a knob, an
// undo, a bend gone back to the middle) since the fader last drove it: the
// fader takes it over afresh. `before`: where it reaches (fader_reach()).
void check_takeover(Context& c, const FaderJob& job, int before) {
    auto& fader = c.ui.fader;
    uint32_t key = job_key(c, job);
    const auto& b = job.binding;
    if (fader.bound != key) {
        fader.bound = key;
        fader.scaled = -1;
        fader.engaged = takeover_of(c, job) == Takeover::Instant || takeover_meets(before, b);
        return;
    }
    if (fader.engaged && b.bound() && before >= 0) { // where the fader sits still names the value, within a step of the card
        int slack = std::max(2, 127 / std::max(1, b.max - b.min));
        if (std::abs(before - int(b.fraction() * 127 + 0.5f)) > slack) { fader.engaged = false; fader.scaled = -1; }
    }
}

// The Motion lane the fader's value lands in: binding_lane()'s, or the track's mix lanes (its level, pan, sends),
// which Context::apply records too.
int fader_lane(const Track& track, const Binding& b) {
    switch (b.kind) {
    case Binding::Kind::Level: return motion_lane(motion_level);
    case Binding::Kind::Pan: return motion_lane(motion_pan);
    case Binding::Kind::Send: return motion_lane(motion_send(unsigned(b.id) % bus_count));
    default: return binding_lane(track, b);
    }
}

// Erase + fader (as on the MPC): the lane of what it drives, once B3 says so.
void erase_lane(Context& c, const FaderJob& job) {
    const auto& track = c.track();
    int lane = fader_lane(track, job.binding);
    if (lane < 0) { c.notice("No motion lane for the fader here"); return; }
    if (c.pattern().has_lane(lane_param(lane), lane_pad(lane) % sampler_pads))
        ask(c, UiState::Ask::EraseLane, lane);
    else c.notice("No motion on " + lane_name(track, lane, &c.project));
}
} // namespace

void fader_move(Context& c, int value, bool syncing) {
    auto& ui = c.ui;
    int before = ui.fader.position;
    ui.fader.position = int16_t(value);
    auto job = fader_job(c.project, ui);
    before = fader_reach(job, before); // a bend's wide middle: where the fader reaches, not where it sits
    int reach = fader_reach(job, value);
    check_takeover(c, job, before);
    if (syncing) return; // where it sits at start: nothing moves
    ui.fader_moved_at = c.now;
    if (ui.erase) ui.erase_used = true; // Erase + fader: its release deletes nothing
    if (ui.ask != UiState::Ask::None) return; // a question open: the fader waits, as the pots do
    if (!job.binding.bound()) return;
    if (ui.erase) { erase_lane(c, job); return; }
    int to = 0;
    if (takeover_move(takeover_of(c, job), ui.fader, before, reach, job.binding, to)) {
        c.apply(job.binding, to);
        if (job.bend) c.fader_bent = uint16_t(c.fader_bent | 1u << (ui.track % track_count));
    }
}

void fader_check(Context& c) {
    auto job = fader_job(c.project, c.ui);
    check_takeover(c, job, fader_reach(job, c.ui.fader.position));
}

void fader_rest(Context& c) {
    if (!c.fader_bent || c.now - c.ui.fader_moved_at < bend_rest_ms) return;
    for (unsigned t = 0; t < track_count; ++t) {
        if (!((c.fader_bent >> t) & 1) || c.holds_notes(t)) continue;
        c.fader_bent = uint16_t(c.fader_bent & ~(1u << t));
        auto& track = c.project.tracks[t];
        bool here = t == c.ui.track % track_count; // Seq Record keeps the return on the selected track's lane
        if (track.engine == EngineKind::Plugin && track.bend) {
            track.bend = 0;
            c.send({Command::Type::Bend, uint8_t(t), 0, 0, 0});
            if (here) c.record_motion(int(motion_bend), 0);
        } else if (track.engine == EngineKind::Midi && track.params[midi_track::Bend]) {
            c.set_param(t, midi_track::Bend, 0);
            if (here) c.record_motion(midi_track::Bend, 0);
        }
    }
}

void set_fader_target(Context& c, unsigned target) {
    auto& t = c.track();
    if (!t.used() || target >= fader_targets) return;
    if (!fader_takes(t.engine, FaderTarget(target))) {
        c.notice(std::string(fader_target_name(FaderTarget(target))) + (FaderTarget(target) == FaderTarget::PitchBend ? ": plug-in and MIDI tracks" : ": kit tracks"));
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

void step_fader_target(Context& c, int delta) { // over the targets the track can take, in fader_order, from the one it has
    auto& t = c.track();
    if (!t.used() || delta == 0) return;
    int at = int(fader_place(fader_target(t))), step = delta > 0 ? 1 : -1;
    for (int left = std::abs(delta); left > 0; --left)
        for (int n = at + step; n >= 0 && n < int(fader_targets); n += step)
            if (fader_takes(t.engine, fader_order[n])) { at = n; break; }
    set_fader_target(c, unsigned(fader_order[at]));
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
