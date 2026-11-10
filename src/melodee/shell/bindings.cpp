// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/bindings.h"
#include "melodee/engine/effect_info.h"
#include "melodee/shell/name_suggest.h"
#include "melodee/engine/engine_info.h"
#include "melodee/engine/sampler.h"
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
Binding engine_param(const Track& track, int id) { return param(track, id); }
} // namespace

std::array<Binding, 3> favourite_bindings(const Track& track) {
    const auto& info = engine_info(track.engine);
    return {param(track, info.favourites[0]), param(track, info.favourites[1]), param(track, info.favourites[2])};
}

unsigned page_chain(const Project& project, const UiState& ui) {
    if (ui.page == PageId::Bus) return bus_chain(project.tracks[ui.track % track_count].mix.bus % bus_count);
    if (ui.page == PageId::Master) return master_chain;
    if (ui.page == PageId::PadFx) { const auto& t = project.tracks[ui.track % track_count]; return pad_chain(ui.track % track_count, t.last_pad % sampler_pads); }
    return ui.track % track_count;
}

MotionParam motion_param(const Track& track, int param) {
    if (param < 0) return {};
    if (unsigned(param) < motion_fx_base) {
        const auto& info = engine_info(track.engine);
        if (unsigned(param) >= info.param_count) return {};
        return {&info.params[param], track.params[size_t(param)]};
    }
    unsigned slot = (unsigned(param) - motion_fx_base) / effect_params, q = (unsigned(param) - motion_fx_base) % effect_params;
    if (slot >= track.fx.count()) return {};
    const auto& s = track.fx.slots[slot];
    const auto& pi = effect_info(EffectKind(s.kind)).params[q];
    if (!pi.label) return {};
    return {&pi, s.params[q]};
}

Binding lane_binding(const Track& track, unsigned t, int param) {
    if (param >= int(motion_fx_base)) {
        auto m = motion_param(track, param);
        if (!m.info) return {};
        unsigned slot = (unsigned(param) - motion_fx_base) / effect_params, q = (unsigned(param) - motion_fx_base) % effect_params;
        return {Kind::FxParam, int32_t(t * 64 + slot * 8 + q), m.info->min, m.info->max, m.value};
    }
    return engine_param(track, param);
}

std::array<int, 3> motion_lanes(const Project& project, const UiState& ui) {
    const auto& track = project.tracks[ui.track % track_count];
    const auto& info = engine_info(track.engine);
    std::array<int, 3> lanes {-1, -1, -1};
    unsigned n = 0;
    auto add = [&](int param) {
        if (!motion_param(track, param).info || n == lanes.size()) return;
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
    if (ui.name.open()) { // the Name sheet over the page: ready names, the character at the cursor
        std::array<const char*, max_suggestions> names {};
        unsigned count = name_suggestions(ui.name, names);
        return {Binding {Kind::NameSuggest, 0, 0, int(std::max(count, 2u)) - 1, std::min<int>(ui.name.suggestion, int(count) - 1)},
                Binding {Kind::NameChar, 0, 0, int(NameEntry::charset_size()) - 1, int(ui.name.char_at())}, Binding {}};
    }
    switch (ui.page) {
    case PageId::Keys:
        if (ui.keys_row) return {Binding {Kind::Transpose, 0, -12, 12, track.transpose}, Binding {}, Binding {}};
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
        unsigned t = ui.track % track_count;
        return {lane_binding(track, t, lanes[0]), lane_binding(track, t, lanes[1]), lane_binding(track, t, lanes[2])};
    }
    case PageId::Patterns: // the selected track's pattern: K1 length by the bar, K2 Q, K3 swing
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
    case PageId::Master:
    case PageId::PadFx: {
        unsigned chain = page_chain(project, ui), slot = ui.fx_cursor[UiState::cursor_of(chain)];
        const auto& c = project.chain(chain);
        if (slot >= c.count() || ui.picker_open) return {};
        const auto& s = c.slots[slot];
        const auto& info = effect_info(EffectKind(s.kind));
        std::array<Binding, 3> out;
        for (unsigned k = 0; k < 3; ++k) {
            unsigned q = (ui.fx_row[UiState::cursor_of(chain)] % 2) * 3 + k;
            const auto& pi = info.params[q];
            if (pi.label) out[k] = {Kind::FxParam, int32_t(chain * 64 + slot * 8 + q), pi.min, pi.max, s.params[q]};
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
        return {Binding {}, Binding {Kind::RecBars, 0, 0, 16, ui.rec_bars}, Binding {Kind::RecGain, 0, -12, 24, ui.rec_gain}};
    case PageId::InputConfig: // the source is on B1-B3
        return {Binding {Kind::RecThreshold, 0, -60, 0, ui.rec_threshold}, Binding {}, Binding {}};
    case PageId::Project:
        if (ui.sheet_open || ui.move_open) return {};
        return {Binding {}, Binding {Kind::ProjectJump, 0, 0, std::max(int(ui.project_rows) - 1, 1), ui.project_cursor},
                Binding {Kind::ProjectSort, 0, 0, 1, ui.project_sort}};
    case PageId::Tempo:
        return {Binding {Kind::Tempo, 0, 30, 300, project.tempo / 10}, Binding {Kind::Swing, 0, 50, 75, project.swing},
                Binding {Kind::ClickLevel, 0, 0, 100, project.click_level}};
    case PageId::Arp:
        return {Binding {Kind::ArpMode, 0, 0, 4, track.arp_mode}, Binding {Kind::ArpRate, 0, 0, 5, track.arp_rate},
                Binding {Kind::ArpOctaves, 0, 1, 4, track.arp_octaves}};
    case PageId::Chord:
        return {Binding {Kind::ChordType, 0, 0, 5, track.chord_type}, Binding {Kind::ChordVoicing, 0, 0, 3, track.chord_voicing},
                Binding {Kind::ChordInvert, 0, 0, 3, track.chord_invert}};
    case PageId::Capture:
        return {Binding {Kind::CaptureBars, 0, 1, 4, ui.capture_bars}, Binding {Kind::CaptureQuantise, 0, 0, 2, ui.capture_quantise},
                Binding {Kind::CaptureInto, 0, 0, int(pattern_count) - 1, ui.capture_into}};
    case PageId::PadPage:
    case PageId::PlayPage:
    case PageId::EnvelopePage: {
        const auto& pad = track.pads[track.last_pad % sampler_pads];
        if (!pad.used()) return {};
        if (ui.page == PageId::PadPage)
            return {Binding {Kind::PadPitch, 0, -24, 24, pad.pitch}, Binding {Kind::PadFine, 0, -50, 50, pad.fine},
                    Binding {Kind::PadLevel, 0, 0, 127, pad.level}};
        if (ui.page == PageId::PlayPage)
            return {Binding {Kind::PadPlayMode, 0, 0, 1, int(pad.play)}, Binding {Kind::PadGroup, 0, 0, 8, pad.group},
                    Binding {Kind::PadReverse, 0, 0, 1, pad.reverse}};
        if (ui.sampler_row) return {Binding {Kind::PadSustain, 0, 0, 100, pad.sustain}, Binding {}, Binding {}};
        return {Binding {Kind::PadAttack, 0, 0, 100, pad.attack}, Binding {Kind::PadDecay, 0, 0, 100, pad.decay},
                Binding {Kind::PadRelease, 0, 0, 100, pad.release}};
    }
    case PageId::FilterPage: return {param(track, sampler::Filter), param(track, sampler::Resonance), param(track, sampler::FilterType)};
    case PageId::FollowPage: return {param(track, sampler::Follow), param(track, sampler::FollowHow), param(track, sampler::PitchHow)};
    case PageId::CloudPage:
        if (ui.sampler_row) return {param(track, sampler::Scan), param(track, sampler::Spread), param(track, sampler::Cloud)};
        return {param(track, sampler::Position), param(track, sampler::Size), param(track, sampler::Density)};
    case PageId::Punch:
        return {Binding {Kind::PunchDepth, 0, 0, 100, project.punch_depth}, Binding {Kind::PunchSnap, 0, 0, 3, project.punch_snap},
                Binding {Kind::PunchTail, 0, 0, 2, project.punch_tail}};
    case PageId::PadPunch: {
        const auto& pad = track.pads[track.last_pad % sampler_pads];
        if (!pad.used()) return {};
        return {Binding {Kind::PadPunchKind, 0, 0, int(punch_kinds), pad.punch}, Binding {Kind::PadPunchRate, 0, 0, 3, pad.punch_rate},
                Binding {Kind::PadPunchDepth, 0, 0, 100, pad.punch_depth}};
    }
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
