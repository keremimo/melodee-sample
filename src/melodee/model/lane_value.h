// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/pad_params.h"
#include "melodee/model/project.h"

// Roadmap P4: where a Motion lane of track `t` stands as set (Motion aside), from the project alone: an engine
// parameter, an effect of the track's chain, a kit pad's own value, the mod wheel, the track's level, pan and sends,
// the master's chain and the returns. Modulators move a lane around it; macros set it.
namespace melodee {

inline int lane_own(const Project& project, unsigned t, unsigned param, unsigned pad) {
    const auto& track = project.tracks[t % track_count];
    if (param == motion_wheel) return track.wheel;
    if (is_mix_lane(param)) {
        if (param == motion_level) return track.mix.level;
        if (param == motion_pan) return track.mix.pan;
        if (param < motion_master_fx_base) return track.mix.sends[(param - motion_send(0)) % bus_count];
        if (param >= motion_return_base) return project.buses[(param - motion_return_base) % bus_count].level;
        unsigned slot = (param - motion_master_fx_base) / 6, q = (param - motion_master_fx_base) % 6;
        return slot < max_slots ? project.master.slots[slot].params[q] : 0;
    }
    if (int which = pad_param_of(param); which >= 0) return pad_param_value(track.pads[pad % sampler_pads], PadParam(which));
    if (param >= motion_fx_base) {
        unsigned slot = (param - motion_fx_base) / 6, q = (param - motion_fx_base) % 6;
        return slot < max_slots ? track.fx.slots[slot].params[q] : 0;
    }
    return param < max_params ? track.params[param] : 0;
}

} // namespace melodee
