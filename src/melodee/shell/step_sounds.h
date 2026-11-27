// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include "melodee/shell/ui_state.h"
#include <algorithm>
#include <cstdint>
#include <vector>

// Step SOUND (Elektron's sample lock; Step Edit's SOUND cards, and SAMPLE SELECT with a step held): the sounds a
// kit or keygroup step may play in place of its pad's own sample. Every sample on a kit or keygroup pad of the
// project, by track and then pad, each once. The step keeps the sample (Event::sample, its slot + 1), not the pad:
// a pad loaded with another sample later leaves the step as it was.
namespace melodee {

struct StepSoundChoice {
    uint16_t slot = no_sample; // Project::samples
    uint8_t track = 0, pad = 0; // the first pad found holding it
};

inline std::vector<StepSoundChoice> step_sound_choices(const Project& project) {
    std::vector<StepSoundChoice> out;
    std::vector<bool> seen(sample_slots, false);
    for (unsigned t = 0; t < track_count; ++t) {
        const auto& track = project.tracks[t];
        if (track.engine != EngineKind::Sampler && track.engine != EngineKind::Keygroup) continue;
        for (unsigned pad = 0; pad < sampler_pads; ++pad) {
            const auto& sound = track.pads[pad];
            if (!sound.used() || sound.sample >= sample_slots || seen[sound.sample]) continue;
            seen[sound.sample] = true;
            out.push_back({sound.sample, uint8_t(t), uint8_t(pad)});
        }
    }
    return out;
}

// A step's sample (Event::sample) among the choices: 1 and on; 0 its pad's own; -1 a sample no pad holds now.
inline int step_sound_index(const std::vector<StepSoundChoice>& choices, uint16_t sample) {
    if (!sample) return 0;
    for (size_t k = 0; k < choices.size(); ++k)
        if (choices[k].slot + 1u == sample) return int(k) + 1;
    return -1;
}

// The slices of the sample a kit step plays (its own, else its pad's); 0 none (no chop, or not a kit: a keygroup
// plays whole samples at a key).
inline unsigned step_sound_slices(const Project& project, const Track& track, unsigned key, uint16_t sample) {
    if (track.engine != EngineKind::Sampler) return 0;
    unsigned slot = sample ? sample - 1u : unsigned(no_sample);
    if (!sample && key >= sampler_first_note && key - sampler_first_note < sampler_pads) slot = track.pads[key - sampler_first_note].sample;
    if (slot >= sample_slots) return 0;
    return project.chops[slot].slices.count;
}

// Whether the sounds are on the pads (Step Edit, for a step of the selected track).
inline bool sounds_open(const UiState& ui) { return ui.page == PageId::StepEdit && ui.sounds_step >= 0 && ui.sounds_for == ui.track; }
// The step the SOUND cards and the sounds on the pads edit: the one the pads were opened for, else the held step,
// else the caret's.
inline unsigned sound_step(const UiState& ui, const Pattern& pattern) {
    const int step = sounds_open(ui) ? ui.sounds_step : ui.held_step >= 0 ? ui.held_step : std::max<int>(ui.step_cursor, 0);
    return unsigned(step) % std::max(1u, pattern.steps());
}

// The screen's copy of the project leaves the chop table out (Shell::snapshot). Step Edit draws from chops: the
// SOUND row's SLICE card, the panel's slice edges, the SLICES on the pads. This copies into `to` the chops of the
// samples on `track`'s pads and of the samples `pattern`'s steps play.
inline void copy_step_chops(Project& to, const Project& from, const Track& track, const Pattern& pattern) {
    auto take = [&](unsigned slot) {
        if (slot < sample_slots && from.chops[slot].used() && !to.chops[slot].used()) to.chops.edit(slot) = from.chops[slot];
    };
    for (const auto& sound : track.pads) take(sound.sample);
    for (const auto& e : pattern.events)
        if (e.sample) take(e.sample - 1u);
}

} // namespace melodee
