// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/fader_job.h"
#include "melodee/engine/effect_info.h"
#include "melodee/engine/engine_info.h"
#include "melodee/engine/sampler.h"
#include "melodee/model/pad_params.h"
#include <algorithm>
#include <cmath>

namespace melodee {
namespace {
using Kind = Binding::Kind;

} // namespace

Binding fader_target_binding(const Track& track, FaderTarget target) {
    if (target == FaderTarget::ModWheel) return {Kind::ModWheel, 0, 0, 127, track.wheel};
    if (track.engine != EngineKind::Sampler) return {};
    if (target == FaderTarget::KitVolume) {
        const auto& p = engine_info(track.engine).params[sampler::Level];
        return {Kind::Param, sampler::Level, p.min, p.max, track.params[sampler::Level]};
    }
    const auto& pad = track.pads[track.last_pad % sampler_pads];
    if (!pad.used()) return {};
    switch (target) {
    case FaderTarget::PadVolume: return {Kind::PadLevel, 0, 0, 127, pad.level};
    case FaderTarget::PadPan: return {Kind::PadPan, 0, -50, 50, pad.pan};
    case FaderTarget::PadTune: return {Kind::PadPitch, 0, -24, 24, pad.pitch};
    case FaderTarget::PadAttack: return {Kind::PadAttack, 0, 0, 100, pad.attack};
    case FaderTarget::PadDecay: // Decay on a one shot, Release on Note On, as the MPC swaps them
        return pad.play == PadPlay::OneShot ? Binding {Kind::PadDecay, 0, 0, 100, pad.decay} : Binding {Kind::PadRelease, 0, 0, 100, pad.release};
    case FaderTarget::PadCutoff: return {Kind::PadCutoff, 0, 0, 100, pad.cutoff};
    default: return {};
    }
}

bool fader_learned_pad(const FaderLearned& learned) {
    return learned.what == FaderLearn::Lane && pad_param_of(lane_param(learned.lane)) >= 0;
}

FaderLearned fader_learnable(const Track& track, unsigned t, const Binding& b) {
    FaderLearned l;
    l.engine = uint8_t(track.engine);
    switch (b.kind) {
    case Kind::Level: l.what = FaderLearn::Level; return l;
    case Kind::Pan: l.what = FaderLearn::Pan; return l;
    case Kind::Send: l.what = FaderLearn::Send; l.lane = uint16_t(unsigned(b.id) % bus_count); return l;
    case Kind::FxParam: { // an effect on the track's own chain (its bus's, the master's or a pad's are not the track's)
        unsigned chain = unsigned(b.id) / 64, slot = unsigned(b.id) % 64 / 8, q = unsigned(b.id) % 8;
        if (chain != t || slot >= track_slots || q >= effect_params) return {};
        l.what = FaderLearn::Lane; l.lane = uint16_t(motion_fx_param(slot, q));
        return l;
    }
    default: break;
    }
    int lane = binding_lane(track, b); // an engine parameter, a kit pad's own value on the last pad, the mod wheel
    if (lane < 0) return {};
    l.what = FaderLearn::Lane;
    l.lane = uint16_t(pad_param_of(lane_param(lane)) >= 0 ? lane_param(lane) : unsigned(lane)); // a pad's: whichever is hit last
    return l;
}

Binding fader_learned_binding(const Track& track, unsigned t) {
    const auto& l = track.fader_learned;
    switch (l.what) {
    case FaderLearn::Level: return {Kind::Level, 0, 0, 127, track.mix.level};
    case FaderLearn::Pan: return {Kind::Pan, 0, -50, 50, track.mix.pan};
    case FaderLearn::Send: { unsigned bus = l.lane % bus_count; return {Kind::Send, int(bus), 0, 100, track.mix.sends[bus]}; }
    case FaderLearn::Lane: {
        unsigned param = lane_param(l.lane);
        if (param < motion_fx_base && l.engine != uint8_t(track.engine)) return {}; // another engine's parameter
        if (pad_param_of(param) < 0) return lane_binding(track, t, int(l.lane));
        unsigned pad = track.last_pad % sampler_pads;
        if (track.engine != EngineKind::Sampler || !track.pads[pad].used()) return {};
        return lane_binding(track, t, motion_lane(param, pad));
    }
    case FaderLearn::None: break;
    }
    return {};
}

FaderJob fader_job(const Project& project, const UiState& ui) {
    const auto& track = project.tracks[ui.track % track_count];
    FaderJob job;
    auto page = [&](Binding b) { job.source = FaderJob::Source::Page; job.binding = b; };
    switch (ui.page) {
    case PageId::StepEdit: job.source = FaderJob::Source::Nudge; return job;
    case PageId::Tracks: page({Kind::Level, 0, 0, 127, track.mix.level}); return job;
    case PageId::Sends: page({Kind::Send, int(ui.sends_bus % bus_count), 0, 100, track.mix.sends[ui.sends_bus % bus_count]}); return job;
    case PageId::Compressor: page({Kind::CompAmount, 0, 0, 100, project.compressor.amount}); return job;
    case PageId::FlexBeat: page({Kind::FlexMix, 0, 0, 100, project.flex.mix}); return job;
    case PageId::KnobFx: // Knob FX C: A to B (no effect loaded: the track's target)
        if (project.knob_fx.kind) { page({Kind::KnobParam, int(knob_morph), 0, 127, project.knob_fx.morph}); return job; }
        break;
    default: break;
    }
    if (!track.used() || !track.fader_on) return job;
    job.source = FaderJob::Source::Target;
    if (track.fader_learned.learned()) { // fader C: the pot it learned, while that is still one of the track's values
        auto learned = fader_learned_binding(track, ui.track % track_count);
        bool pad = fader_learned_pad(track.fader_learned) && track.engine == EngineKind::Sampler; // an empty last pad: nothing to drive
        if (learned.bound() || pad) { job.binding = learned; job.learned = true; job.pad = pad; }
    }
    if (!job.learned) {
        auto target = fader_target(track);
        job.binding = fader_target_binding(track, target);
        job.pad = target != FaderTarget::KitVolume && target != FaderTarget::ModWheel;
    }
    job.centred = job.binding.bound() && job.binding.min < 0 && job.binding.max > 0; // pan, tune: either side of a middle
    return job;
}

float fader_light_level(const FaderJob& job) {
    if (job.source == FaderJob::Source::None) return 0;
    if (job.source == FaderJob::Source::Nudge || !job.binding.bound()) return 0.35f; // a job with no value to show
    float f = std::clamp(job.binding.fraction(), 0.0f, 1.0f);
    if (job.centred) f = 1 - std::fabs(f - 0.5f) * 2;
    return 0.06f + 0.94f * f; // never quite dark while the fader has a job
}

} // namespace melodee
