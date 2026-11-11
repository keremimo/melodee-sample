// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/fader_job.h"
#include "melodee/engine/effect_info.h"
#include "melodee/engine/engine_info.h"
#include "melodee/engine/sampler.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace melodee {
namespace {
using Kind = Binding::Kind;

// Knob FX's mix: the effect's parameter named MIX; an effect without one
// (Filter, EQ, Gate, Auto-Pan ...) its first, K1's (a filter's cutoff).
Binding knob_fx_mix(const Project& project) {
    const auto& k = project.knob_fx;
    if (!k.kind) return {};
    const auto& info = effect_info(EffectKind(k.kind));
    int chosen = info.params[0].label ? 0 : -1;
    for (unsigned q = 0; q < effect_params; ++q)
        if (info.params[q].label && std::strcmp(info.params[q].label, "MIX") == 0) { chosen = int(q); break; }
    if (chosen < 0) return {};
    const auto& pi = info.params[size_t(chosen)];
    return {Kind::KnobParam, chosen, pi.min, pi.max, k.params[size_t(chosen)]};
}
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

FaderJob fader_job(const Project& project, const UiState& ui) {
    const auto& track = project.tracks[ui.track % track_count];
    FaderJob job;
    auto page = [&](Binding b) { job.source = FaderJob::Source::Page; job.binding = b; };
    switch (ui.page) {
    case PageId::StepEdit: job.source = FaderJob::Source::Nudge; return job;
    case PageId::Tracks: page({Kind::Level, 0, 0, 127, track.mix.level}); return job;
    case PageId::Sends: page({Kind::Send, 0, 0, 100, track.mix.send}); return job;
    case PageId::Compressor: page({Kind::CompAmount, 0, 0, 100, project.compressor.amount}); return job;
    case PageId::FlexBeat: page({Kind::FlexMix, 0, 0, 100, project.flex.mix}); return job;
    case PageId::KnobFx:
        if (auto mix = knob_fx_mix(project); mix.bound()) { page(mix); return job; }
        break; // an effect without a mix: the track's target
    default: break;
    }
    if (!track.used() || !track.fader_on) return job;
    job.source = FaderJob::Source::Target;
    job.binding = fader_target_binding(track, fader_target(track));
    job.centred = job.binding.kind == Kind::PadPan || job.binding.kind == Kind::PadPitch;
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
