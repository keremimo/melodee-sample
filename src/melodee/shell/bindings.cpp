// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/bindings.h"
#include "melodee/engine/arp.h"
#include "melodee/engine/effect_info.h"
#include "melodee/shell/name_suggest.h"
#include "melodee/engine/engine_info.h"
#include "melodee/engine/sampler.h"
#include "melodee/model/mod_matrix.h"
#include "melodee/model/pad_params.h"
#include "melodee/model/scales.h"
#include "melodee/shell/sound_list.h"
#include "melodee/shell/step_edit.h"
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

namespace {
// A kit pad's values as Motion lanes describe them (model/pad_params.h order).
const ParamInfo pad_infos[pad_param_count] {
    {"LEVEL", 0, 127, 100, Format::Number}, {"PITCH", -24, 24, 0, Format::Semitones}, {"FINE", -50, 50, 0, Format::Cents},
    {"PAN", -50, 50, 0, Format::Number}, {"ATTACK", 0, 100, 0}, {"DECAY", 0, 100, 100}, {"SUSTAIN", 0, 100, 100},
    {"RELEASE", 0, 100, 20}, {"VEL SENS", 0, 100, 100}, {"OFFSET", 0, 100, 0}, {"F ATTACK", 0, 100, 0},
    {"F DECAY", 0, 100, 50}, {"F SUSTAIN", 0, 100, 0}, {"F RELEASE", 0, 100, 20}, {"F DEPTH", 0, 100, 0},
    {"CUTOFF", 0, 100, 100}, {"RESO", 0, 100, 0},
};
Binding::Kind pad_kind(PadParam p) {
    using K = Binding::Kind;
    switch (p) {
    case PadParam::Level: return K::PadLevel;
    case PadParam::Pitch: return K::PadPitch;
    case PadParam::Fine: return K::PadFine;
    case PadParam::Pan: return K::PadPan;
    case PadParam::Attack: return K::PadAttack;
    case PadParam::Decay: return K::PadDecay;
    case PadParam::Sustain: return K::PadSustain;
    case PadParam::Release: return K::PadRelease;
    case PadParam::VelocitySense: return K::PadVelSense;
    case PadParam::Offset: return K::PadOffset;
    case PadParam::FilterAttack: return K::PadFAttack;
    case PadParam::FilterDecay: return K::PadFDecay;
    case PadParam::FilterSustain: return K::PadFSustain;
    case PadParam::FilterRelease: return K::PadFRelease;
    case PadParam::FilterAmount: return K::PadFAmount;
    case PadParam::FilterCutoff: return K::PadCutoff;
    case PadParam::FilterResonance: return K::PadResonance;
    default: return K::None;
    }
}
// The Sample Mode cards of the last pad hit (parity phase 4).
int ms(uint32_t frames) { return int(uint64_t(frames) * 1000 / 44100); }
Binding trim_zoom(const UiState& ui, int handle) { return {Kind::TrimZoom, handle, 0, 4, ui.trim_zoom}; }
// Decay on One shot, Release on Note On (Gate), as the MPC swaps them; the other on row 2.
Binding amp_second(const PadSound& pad, bool other) {
    bool decay = (pad.play == PadPlay::OneShot) != other;
    return decay ? Binding {Kind::PadDecay, 0, 0, 100, pad.decay} : Binding {Kind::PadRelease, 0, 0, 100, pad.release};
}
Binding filter_second(const PadSound& pad, bool other) {
    bool decay = (pad.play == PadPlay::OneShot) != other;
    return decay ? Binding {Kind::PadFDecay, 0, 0, 100, pad.filter_decay} : Binding {Kind::PadFRelease, 0, 0, 100, pad.filter_release};
}
} // namespace

MotionParam motion_param(const Track& track, int lane) {
    if (lane < 0) return {};
    int param = int(lane_param(lane));
    if (int which = pad_param_of(unsigned(param)); which >= 0) { // a kit pad's own value
        auto p = PadParam(which);
        if (track.engine != EngineKind::Sampler) return {};
        return {&pad_infos[which], pad_param_value(track.pads[lane_pad(lane) % sampler_pads], p)};
    }
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

std::string lane_name(const Track& track, int lane) {
    auto m = motion_param(track, lane);
    if (!m.info) return "lane";
    if (pad_param_of(lane_param(lane)) < 0) return m.info->label;
    char label[4];
    pad_label(lane_pad(lane), label);
    return std::string(label) + " " + m.info->label;
}

Binding lane_binding(const Track& track, unsigned t, int lane) {
    int param = int(lane_param(lane));
    if (int which = pad_param_of(unsigned(param)); which >= 0) { // the pad's card, as on the sampler pages (they edit the last pad hit)
        auto m = motion_param(track, lane);
        if (!m.info || lane_pad(lane) != track.last_pad % sampler_pads) return {};
        return {pad_kind(PadParam(which)), 0, m.info->min, m.info->max, m.value};
    }
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
    bool kit = track.engine == EngineKind::Sampler;
    unsigned pad = track.last_pad % sampler_pads;
    for (unsigned e = 0; e < motion.count; ++e) {
        const auto& ev = motion.events[e];
        if (ev.track != ui.track || ev.pattern != track.pattern) continue;
        bool pad_lane = pad_param_of(ev.param) >= 0;
        if (pad_lane && ev.pad != pad) continue; // another pad's lane: shown when that pad is the last hit
        add(motion_lane(ev.param, pad_lane ? ev.pad : 0));
    }
    if (kit && track.pads[pad].used()) // a kit: the last pad's tune, level and pan, as on Pads
        for (auto p : {PadParam::Pitch, PadParam::Level, PadParam::Pan}) add(motion_lane(motion_pad_param(p), pad));
    for (auto f : info.favourites) add(f);
    return lanes;
}

std::array<Binding, 3> shift_bindings(const Project& project, const UiState& ui, const Pattern& pattern) {
    const auto& track = project.tracks[ui.track % track_count];
    if (ui.name.open()) return {};
    switch (ui.page) {
    case PageId::Patterns: // Shift: any bar count, the time signature, the metronome's level (as on the MPC)
        return {Binding {Kind::PatternBars, 0, 1, int(max_bars), int(pattern_bars(project, pattern))},
                Binding {Kind::TimeSignature, 0, 0, int(time_signature_count) - 1, project.signature},
                Binding {Kind::ClickLevel, 0, 0, 100, project.click_level}};
    case PageId::StepEdit: { // Shift + K1 the selected event's length (in steps of Q), Shift + K2 the time signature (as on the MPC)
        int at = selected_event(pattern, ui);
        Binding length {};
        if (at >= 0) {
            unsigned q = pattern.q(), steps = std::max(1u, (pattern.events[size_t(at)].length + q / 2) / q);
            length = {Kind::EventLength, 0, 1, 64, int(std::min(steps, 64u))};
        }
        return {length, Binding {Kind::TimeSignature, 0, 0, int(time_signature_count) - 1, project.signature}, Binding {}};
    }
    case PageId::Trim: // Shift + K: zoom in at the start, the end or the loop point (as on the MPC)
        return {trim_zoom(ui, 0), trim_zoom(ui, 1), trim_zoom(ui, 2)};
    case PageId::PadMix: // Shift + K2: the kit's volume
        if (track.engine != EngineKind::Sampler) return {};
        return {Binding {}, param(track, sampler::Level), Binding {}};
    case PageId::EnvelopePage:
    case PageId::FilterEnvPage: { // Shift + K2: decay from the start or the end
        const auto& pad = track.pads[track.last_pad % sampler_pads];
        if (track.engine != EngineKind::Sampler || !pad.used()) return {};
        return {Binding {}, Binding {Kind::PadDecayFrom, 0, 0, 1, pad.decay_from_start ? 0 : 1}, Binding {}};
    }
    case PageId::PadPage: { // Tune: Shift + K3 # Beats
        const auto& pad = track.pads[track.last_pad % sampler_pads];
        if (track.engine != EngineKind::Sampler || !pad.used()) return {};
        return {Binding {}, Binding {}, Binding {Kind::PadBeats, 0, 0, int(max_pad_beats), pad.beats}};
    }
    case PageId::PlayPage: { // Shift + K2: Pad Link
        const auto& pad = track.pads[track.last_pad % sampler_pads];
        if (track.engine != EngineKind::Sampler || !pad.used() || ui.sampler_row) return {};
        return {Binding {}, Binding {Kind::PadLink, 0, 0, 16, pad.link}, Binding {}};
    }
    case PageId::Keys: // Shift + K3: the pads by semitones
        if (track.engine == EngineKind::Kit909) return {};
        return {Binding {}, Binding {}, Binding {Kind::Transpose, 0, -12, 12, track.transpose}};
    default: return {};
    }
}

std::array<Binding, 3> page_bindings(const Project& project, const UiState& ui, const Pattern& pattern, bool shift) {
    auto cards = base_bindings(project, ui, pattern);
    if (!shift) return cards;
    auto second = shift_bindings(project, ui, pattern);
    for (unsigned n = 0; n < 3; ++n) if (second[n].bound()) cards[n] = second[n];
    return cards;
}

std::array<Binding, 3> page_bindings(const Project& project, const UiState& ui, const Pattern& pattern) {
    return page_bindings(project, ui, pattern, ui.shift);
}

std::array<Binding, 3> base_bindings(const Project& project, const UiState& ui, const Pattern& pattern) {
    const auto& track = project.tracks[ui.track % track_count];
    if (ui.name.open()) { // the Name sheet over the page: ready names, the character at the cursor
        std::array<const char*, max_suggestions> names {};
        unsigned count = name_suggestions(ui.name, names);
        return {Binding {Kind::NameSuggest, 0, 0, int(std::max(count, 2u)) - 1, std::min<int>(ui.name.suggestion, int(count) - 1)},
                Binding {Kind::NameChar, 0, 0, int(NameEntry::charset_size()) - 1, int(ui.name.char_at())}, Binding {}};
    }
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
        unsigned t = ui.track % track_count;
        return {lane_binding(track, t, lanes[0]), lane_binding(track, t, lanes[1]), lane_binding(track, t, lanes[2])};
    }
    case PageId::Patterns: // the selected track's pattern: K1 length by the bar, K2 Q, K3 swing
    {
        unsigned bars = pattern_bars(project, pattern), index = 0;
        while (index + 1 < pattern_length_count && pattern_lengths[index + 1] <= bars) ++index;
        return {Binding {Kind::PatternLength, 0, 0, int(pattern_length_count) - 1, int(index)},
                Binding {Kind::Division, 0, 0, int(Division::Count) - 1, int(pattern.division)},
                Binding {Kind::Swing, 0, 50, 75, project.swing}};
    }
    case PageId::StepEdit: { // as on the MPC: K1 the pattern's length (as on Patterns), K2 Q, K3 the selected event's velocity
        unsigned bars = pattern_bars(project, pattern), index = 0;
        while (index + 1 < pattern_length_count && pattern_lengths[index + 1] <= bars) ++index;
        int at = selected_event(pattern, ui);
        Binding velocity {};
        if (at >= 0) velocity = {Kind::EventVelocity, 0, 1, 127, pattern.events[size_t(at)].velocity};
        return {Binding {Kind::PatternLength, 0, 0, int(pattern_length_count) - 1, int(index)},
                Binding {Kind::Division, 0, 0, int(Division::Count) - 1, int(pattern.division)}, velocity};
    }
    case PageId::TimeCorrect: // Q (the pattern's, as on Patterns), shift and swing for the notes moved
        return {Binding {Kind::Division, 0, 0, int(Division::Count) - 1, int(pattern.division)},
                Binding {Kind::TcShift, 0, -int(ticks_per_quarter / 2), int(ticks_per_quarter / 2), ui.tc_shift},
                Binding {Kind::TcSwing, 0, 50, 75, ui.tc_swing}};
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
        if (track.engine != EngineKind::Sampler) return favourite_bindings(track); // 16 Levels on a synth: its own three
        const auto& pad = track.pads[(ui.page == PageId::Levels ? track.levels_pad : track.last_pad) % sampler_pads];
        if (!pad.used()) return {};
        return {Binding {Kind::PadPitch, 0, -24, 24, pad.pitch}, Binding {Kind::PadLevel, 0, 0, 127, pad.level},
                Binding {Kind::PadPan, 0, -50, 50, pad.pan}};
    }
    case PageId::Trim: { // K1 start, K2 end, K3 the loop point (Shift: zoom at each)
        const auto& pad = track.pads[track.last_pad % sampler_pads];
        uint32_t frames = pad.used() ? project.samples[pad.sample % sample_slots].frames : 0;
        if (!frames) return {};
        int length = ms(frames), window = std::max(4, length >> ui.trim_zoom);
        int start = ms(pad.start), end = pad.end ? ms(pad.end) : length;
        // Closer in, each pot turns across a window of the sample that stays put while the handle is inside it.
        auto range = [&](int value) { int lo = std::min(value / window * window, std::max(0, length - window)); return std::pair<int, int>(lo, std::min(length, lo + window)); };
        auto [s0, s1] = range(start);
        auto [e0, e1] = range(end);
        int loop = std::clamp(ms(pad.loop_point()), start, std::max(start, end - 1));
        auto [l0, l1] = range(loop);
        return {Binding {Kind::PadStart, 0, s0, std::min(s1, end - 1), start}, Binding {Kind::PadEnd, 0, std::max(e0, start + 1), e1, end},
                Binding {Kind::PadLoopPoint, 0, std::max(l0, start), std::min(l1, std::max(start + 1, end - 1)), loop}};
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
        return {Binding {Kind::Tempo, 0, 30, 300, int(project.tempo_of(ui.lead_pattern) / 10)}, Binding {Kind::Swing, 0, 50, 75, project.swing},
                Binding {Kind::ClickLevel, 0, 0, 100, project.click_level}};
    case PageId::Arp:
        return {Binding {Kind::ArpMode, 0, 0, 4, track.arp_mode}, Binding {Kind::ArpRate, 0, 0, int(arp_rates) - 1, track.arp_rate},
                Binding {Kind::ArpOctaves, 0, 1, 4, track.arp_octaves}};
    case PageId::Chord:
        return {Binding {Kind::ChordType, 0, 0, 5, track.chord_type}, Binding {Kind::ChordVoicing, 0, 0, 3, track.chord_voicing},
                Binding {Kind::ChordInvert, 0, 0, 3, track.chord_invert}};
    case PageId::Capture:
        return {Binding {Kind::CaptureBars, 0, 1, 4, ui.capture_bars}, Binding {Kind::CaptureQuantise, 0, 0, 2, ui.capture_quantise},
                Binding {Kind::CaptureInto, 0, 0, int(pattern_count) - 1, ui.capture_into}};
    case PageId::PadMix:
    case PageId::PadPage:
    case PageId::PlayPage:
    case PageId::EnvelopePage:
    case PageId::PadFilterPage:
    case PageId::FilterEnvPage: {
        const auto& pad = track.pads[track.last_pad % sampler_pads];
        if (track.engine != EngineKind::Sampler || !pad.used()) return {};
        bool row2 = ui.sampler_row != 0;
        switch (ui.page) {
        case PageId::PadMix: // K1 volume, K3 pan (Shift + K2 the kit's volume)
            return {Binding {Kind::PadLevel, 0, 0, 127, pad.level}, Binding {}, Binding {Kind::PadPan, 0, -50, 50, pad.pan}};
        case PageId::PadPage: // Tune: semitones, cents, Warp (Shift + K3 # Beats)
            return {Binding {Kind::PadPitch, 0, -24, 24, pad.pitch}, Binding {Kind::PadFine, 0, -50, 50, pad.fine},
                    Binding {Kind::PadWarp, 0, 0, warp_card_seq, warp_card(pad.warp)}};
        case PageId::PlayPage: // Mono / Poly, mute group, offset (Shift + K2 Pad Link); row 2 Note On, reverse, loop
            if (row2) return {Binding {Kind::PadPlayMode, 0, 0, 1, int(pad.play)}, Binding {Kind::PadReverse, 0, 0, 1, pad.reverse},
                              Binding {Kind::PadLoop, 0, 0, 1, pad.loop}};
            return {Binding {Kind::PadMono, 0, 0, 1, pad.mono}, Binding {Kind::PadGroup, 0, 0, 16, pad.group},
                    Binding {Kind::PadOffset, 0, 0, 100, pad.offset}};
        case PageId::EnvelopePage: // Amp Env: attack, decay (One shot) or release (Note On), velocity sensitivity; row 2 sustain and the other
            if (row2) return {Binding {Kind::PadSustain, 0, 0, 100, pad.sustain}, amp_second(pad, true), Binding {}};
            return {Binding {Kind::PadAttack, 0, 0, 100, pad.attack}, amp_second(pad, false), Binding {Kind::PadVelSense, 0, 0, 100, pad.velocity_sense}};
        case PageId::PadFilterPage: // the pad's filter: cutoff, resonance, type
            return {Binding {Kind::PadCutoff, 0, 0, 100, pad.cutoff}, Binding {Kind::PadResonance, 0, 0, 100, pad.resonance},
                    Binding {Kind::PadFilterType, 0, 0, int(pad_filter_count) - 1, int(pad.filter)}};
        default: // Filt Env: attack, decay or release, depth; row 2 sustain and the other
            if (row2) return {Binding {Kind::PadFSustain, 0, 0, 100, pad.filter_sustain}, filter_second(pad, true), Binding {}};
            return {Binding {Kind::PadFAttack, 0, 0, 100, pad.filter_attack}, filter_second(pad, false),
                    Binding {Kind::PadFAmount, 0, 0, 100, pad.filter_amount}};
        }
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
        unsigned at = unsigned(std::max<int>(ui.step_cursor, 0)) % pattern.steps();
        const auto step = pattern.step(at);
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
