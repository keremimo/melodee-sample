// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/bindings.h"
#include "melodee/engine/effect_info.h"
#include "melodee/engine/engine_info.h"
#include "melodee/model/mod_matrix.h"
#include "melodee/model/scales.h"
#include "melodee/shell/sound_list.h"
#include <algorithm>

namespace melodee {
namespace {
using Kind = Binding::Kind;

Binding param(const Track& track, int id) {
    const auto& info = engine_info(track.engine);
    if (id < 0 || unsigned(id) >= info.param_count) return {};
    const auto& p = info.params[id];
    return {Kind::Param, int16_t(id), p.min, p.max, track.params[size_t(id)]};
}
} // namespace

std::array<Binding, 3> favourite_bindings(const Track& track) {
    const auto& info = engine_info(track.engine);
    return {param(track, info.favourites[0]), param(track, info.favourites[1]), param(track, info.favourites[2])};
}

unsigned page_chain(const Project& project, const UiState& ui) {
    if (ui.page == PageId::Bus) return bus_chain(project.tracks[ui.track % track_count].mix.bus % bus_count);
    if (ui.page == PageId::Master) return master_chain;
    return ui.track % track_count;
}

std::array<int, 3> motion_lanes(const Project& project, const UiState& ui) {
    const auto& track = project.tracks[ui.track % track_count];
    const auto& info = engine_info(track.engine);
    std::array<int, 3> lanes {-1, -1, -1};
    unsigned n = 0;
    auto add = [&](int param) {
        if (param < 0 || unsigned(param) >= info.param_count || n == lanes.size()) return;
        for (unsigned k = 0; k < n; ++k) if (lanes[k] == param) return;
        lanes[n++] = param;
    };
    const auto& motion = project.motion;
    for (unsigned e = 0; e < motion.count; ++e)
        if (motion.events[e].track == ui.track && motion.events[e].pattern == track.pattern) add(motion.events[e].param);
    for (auto f : info.favourites) add(f);
    return lanes;
}

std::array<Binding, 3> page_bindings(const Project& project, const UiState& ui, const Pattern& pattern) {
    const auto& track = project.tracks[ui.track % track_count];
    switch (ui.page) {
    case PageId::Keys:
        return {Binding {Kind::Root, 0, 0, 11, project.root},
                Binding {Kind::Scale, 0, 0, int(scales().size()) - 1, project.scale},
                Binding {Kind::Layout, 0, 0, int(PadLayout::Count) - 1, int(track.layout)}};
    case PageId::Mix:
        return {Binding {Kind::Level, 0, 0, 127, track.mix.level}, Binding {Kind::Pan, 0, -50, 50, track.mix.pan},
                Binding {Kind::Send, 0, 0, 100, track.mix.send}};
    case PageId::Sounds: {
        std::array<EngineKind, size_t(EngineKind::Count)> engines {};
        unsigned count = available_engines(engines), at = 0;
        for (unsigned n = 0; n < count; ++n) if (engines[n] == track.engine) at = n;
        std::array<uint16_t, max_listed> list {};
        unsigned listed = sound_list(track, ui, list);
        // An engine choice needs at least two positions to turn through.
        return {Binding {Kind::Engine, 0, 0, int(std::max(count, 2u)) - 1, int(at)},
                Binding {Kind::Jump, 0, 0, int(std::max(listed, 1u)) - 1, std::min<int>(ui.sounds_cursor, int(listed) - 1)},
                Binding {Kind::Audition, 0, 1, 127, ui.audition_velocity}};
    }
    case PageId::Edit: {
        const auto& info = engine_info(track.engine);
        if (!info.page_count) return {};
        if (is_mod_page(info, ui.edit_page[ui.track])) {
            unsigned r = ui.edit_row[ui.track] % mod_routes;
            const auto& route = track.mods[r];
            if (!route.used()) return {}; // Push on an empty row starts a route
            return {Binding {Kind::ModSource, int16_t(r), 1, int(ModSource::Count) - 1, route.source},
                    Binding {Kind::ModDest, int16_t(r), 1, int(ModDest::Count) - 1, route.destination},
                    Binding {Kind::ModAmount, int16_t(r), -mod_amount_max, mod_amount_max, route.amount}};
        }
        const auto& page = edit_page_at(info, ui.edit_page[ui.track]);
        const auto& row = page.rows[ui.edit_row[ui.track] % std::max(page.row_count(), 1u)];
        return {param(track, row[0]), param(track, row[1]), param(track, row[2])};
    }
    case PageId::Motion: {
        auto lanes = motion_lanes(project, ui);
        return {param(track, lanes[0]), param(track, lanes[1]), param(track, lanes[2])};
    }
    case PageId::Length:
        return {Binding {Kind::PatternBars, 0, 1, int(max_steps / bar_steps), int((pattern.length + bar_steps - 1) / bar_steps)},
                Binding {Kind::Division, 0, 0, int(Division::Count) - 1, int(pattern.division)},
                Binding {Kind::Swing, 0, 50, 75, project.swing}};
    case PageId::Song: {
        int clip = project.song.clip_at(ui.track, ui.song_cursor);
        int pattern_value = clip >= 0 ? project.song.clips[size_t(clip)].pattern : ui.song_pattern;
        int length_value = clip >= 0 ? project.song.clips[size_t(clip)].bars : ui.song_length;
        return {Binding {Kind::SongZoom, 0, 0, int(sizeof(song_zooms) / sizeof(song_zooms[0])) - 1, ui.song_zoom},
                Binding {Kind::SongPattern, int16_t(clip), 0, int(pattern_count) - 1, pattern_value},
                Binding {Kind::SongLength, int16_t(clip), 1, 16, length_value}};
    }
    case PageId::Rack:
    case PageId::Bus:
    case PageId::Master: {
        unsigned chain = page_chain(project, ui), slot = ui.fx_cursor[chain];
        const auto& c = project.chain(chain);
        if (slot >= c.count() || ui.picker_open) return {};
        const auto& s = c.slots[slot];
        const auto& info = effect_info(EffectKind(s.kind));
        std::array<Binding, 3> out;
        for (unsigned k = 0; k < 3; ++k) {
            unsigned q = (ui.fx_row[chain] % 2) * 3 + k;
            const auto& pi = info.params[q];
            if (pi.label) out[k] = {Kind::FxParam, int16_t(chain * 64 + slot * 8 + q), pi.min, pi.max, s.params[q]};
        }
        return out;
    }
    case PageId::Sends: {
        unsigned bus = track.mix.bus % bus_count;
        return {Binding {Kind::Send, 0, 0, 100, track.mix.send}, Binding {Kind::SendBus, 0, 0, int(bus_count) - 1, int(bus)},
                Binding {Kind::BusReturn, int16_t(bus), 0, 100, project.buses[bus].level}};
    }
    case PageId::Levels:
    case PageId::Pads: {
        const auto& pad = track.pads[(ui.page == PageId::Levels ? track.levels_pad : track.last_pad) % sampler_pads];
        if (!pad.used()) return {};
        return {Binding {Kind::PadPitch, 0, -24, 24, pad.pitch}, Binding {Kind::PadLevel, 0, 0, 127, pad.level},
                Binding {Kind::PadPan, 0, -50, 50, pad.pan}};
    }
    case PageId::Trim: {
        const auto& pad = track.pads[track.last_pad % sampler_pads];
        uint32_t frames = pad.used() ? project.samples[pad.sample % sample_slots].frames : 0;
        if (!frames) return {Binding {}, Binding {}, Binding {Kind::TrimZoom, 0, 0, 4, ui.trim_zoom}};
        int length = int(uint64_t(frames) * 1000 / 44100), window = std::max(4, length >> ui.trim_zoom);
        int start = int(uint64_t(pad.start) * 1000 / 44100), end = pad.end ? int(uint64_t(pad.end) * 1000 / 44100) : length;
        // Closer in, each pot turns across a window of the sample that stays put while the handle is inside it.
        auto range = [&](int value) { int lo = std::min(value / window * window, std::max(0, length - window)); return std::pair<int, int>(lo, std::min(length, lo + window)); };
        auto [s0, s1] = range(start);
        auto [e0, e1] = range(end);
        return {Binding {Kind::PadStart, 0, s0, std::min(s1, end - 1), start}, Binding {Kind::PadEnd, 0, std::max(e0, start + 1), e1, end},
                Binding {Kind::TrimZoom, 0, 0, 4, ui.trim_zoom}};
    }
    case PageId::Chop: {
        int mode = int(ui.chop_mode);
        static const int lows[] {0, 2, 1}, highs[] {100, int(max_slices), 8};
        Binding start {};
        uint32_t frames = ui.chop_sample < sample_slots ? project.samples[ui.chop_sample].frames : 0;
        unsigned n = ui.chop_slice;
        if (frames && n > 0 && n < ui.slices.count) {
            int lo = int(uint64_t(ui.slices.start[n - 1]) * 1000 / 44100) + 1, hi = int(uint64_t(ui.slices.end(n, frames)) * 1000 / 44100) - 1;
            start = {Kind::ChopStart, int16_t(n), lo, hi, int(uint64_t(ui.slices.start[n]) * 1000 / 44100)};
        }
        return {Binding {Kind::ChopMode, 0, 0, int(ChopMode::Count) - 1, mode},
                Binding {Kind::ChopAmount, int16_t(mode), lows[mode], highs[mode], ui.chop_amount[size_t(mode)]}, start};
    }
    case PageId::Record:
        return {Binding {Kind::RecThreshold, 0, -60, 0, ui.rec_threshold}, Binding {Kind::RecBars, 0, 0, 16, ui.rec_bars},
                Binding {Kind::RecGain, 0, -12, 24, ui.rec_gain}};
    case PageId::Samples:
        return {Binding {}, Binding {Kind::FileJump, 0, 0, std::max(int(ui.files_count) - 1, 1), ui.files_cursor}, Binding {}};
    case PageId::Roll:
    case PageId::Steps: {
        unsigned at = unsigned(std::max<int>(ui.step_cursor, 0)) % max_steps;
        const auto& step = pattern.steps[at];
        if (step.empty()) return {};
        const Note* note = step.first();
        return {Binding {Kind::StepLength, 0, 1, 16, step.length}, Binding {Kind::StepVelocity, 0, 1, 127, note->velocity},
                Binding {Kind::StepChance, 0, 0, 100, step.chance}};
    }
    case PageId::Stage:
    default: return favourite_bindings(track);
    }
}

} // namespace melodee
