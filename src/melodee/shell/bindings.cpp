// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/bindings.h"
#include "melodee/shell/free_length.h"
#include "melodee/shell/step_sounds.h"
#include "melodee/shell/levels_keys.h"
#include "melodee/shell/features.h"
#include "melodee/shell/mix_hub.h"
#include "melodee/shell/mixer_channels.h"
#include "melodee/engine/command.h"
#include "melodee/model/note_edits.h"
#include "melodee/shell/midi_ports.h"
#include "melodee/shell/midi_config_rows.h"
#include "melodee/shell/input_choices.h"
#include "melodee/shell/loop_sheet.h"
#include "melodee/shell/pad_colour.h"
#include "melodee/engine/arp.h"
#include "melodee/engine/effect_info.h"
#include "melodee/engine/pad_fx_info.h"
#include "melodee/shell/name_suggest.h"
#include "melodee/engine/engine_info.h"
#include "melodee/engine/macros.h"
#include "melodee/engine/sampler.h"
#include "melodee/model/mod_matrix.h"
#include "melodee/model/pad_params.h"
#include "melodee/model/scales.h"
#include "melodee/engine/midi_engine.h"
#include "melodee/shell/sound_list.h"
#include "melodee/shell/sample_mode.h"
#include "melodee/shell/stage_map.h"
#include "melodee/shell/zoom_view.h"
#include "melodee/shell/browse_trim.h"
#include "melodee/shell/step_edit.h"
#include "melodee/shell/step_lane.h"
#include "melodee/model/generators.h"
#include "melodee/shell/roll.h"
#include <algorithm>

namespace melodee {
namespace {
using Kind = Binding::Kind;

Binding param(const Track& track, int id) {
    const auto& info = engine_info(track.engine);
    if (id < 0 || unsigned(id) >= info.param_count) return {};
    const auto& p = track_param_info(track.engine, track.params, unsigned(id));
    return {Kind::Param, int16_t(id), p.min, p.max, track.params[size_t(id)]};
}
Binding engine_param(const Track& track, int id) { return param(track, id); }
} // namespace

std::array<Binding, 3> favourite_bindings(const Track& track) {
    const auto& info = engine_info(track.engine);
    return {param(track, info.favourites[0]), param(track, info.favourites[1]), param(track, info.favourites[2])};
}

unsigned page_chain(const Project& project, const UiState& ui) {
    if (ui.page == PageId::Rack && ui.fx_loop >= 0) return loop_chain(unsigned(ui.fx_loop) % loop_tracks); // the Looper's track
    if (ui.page == PageId::Rack && ui.fx_sub >= 0) return sub_chain(unsigned(ui.fx_sub) % sub_count);    // a mixer submix's
    if (mixer_fx_lit(ui) && channel_chain(mixer_strip(ui)) >= 0) return unsigned(channel_chain(mixer_strip(ui))); // the mixer's FX block
    if (ui.page == PageId::Bus) return bus_chain(ui.sends_bus % bus_count); // the bus in view (Sends' K2)
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
    {"CUTOFF", 0, 100, 100}, {"RESO", 0, 100, 0}, {"SEND A", 0, 100, 0}, {"SEND B", 0, 100, 0}, {"SEND C", 0, 100, 0}, {"SEND D", 0, 100, 0},
    {"POSITION", 0, 100, 38}, {"SIZE", 0, 100, 50}, {"DENSITY", 0, 100, 24, Format::Number}, {"SCAN", 0, 100, 50},
    {"SPREAD", 0, 100, 12, Format::Cents},
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
    case PadParam::SendA: case PadParam::SendB: case PadParam::SendC: case PadParam::SendD: return K::PadSend; // id: the bus
    case PadParam::CloudPosition: case PadParam::CloudSize: case PadParam::CloudDensity: case PadParam::CloudScan:
    case PadParam::CloudSpread: return K::PadCloud; // id: cloud_id()
    default: return K::None;
    }
}
// A PadCloud card's id for a cloud lane (0 is Cloud on or off, which has none), and the card of one.
int cloud_id(PadParam p) { return int(p) - int(PadParam::CloudPosition) + 1; }
Binding cloud_card(const PadSound& pad, int id) {
    if (!id) return {Kind::PadCloud, 0, 0, 1, pad.cloud};
    auto p = PadParam(int(PadParam::CloudPosition) + id - 1);
    return {Kind::PadCloud, id, 0, 100, pad_param_value(pad, p)};
}
// The Sample Mode cards of the last pad hit (parity phase 4).
Binding trim_zoom(const UiState& ui, int handle) { return {Kind::TrimZoom, handle, 0, zoom_top, ui.trim_zoom}; }
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

// The mod wheel's lane, as Motion shows it (parity phase 8), and a plug-in track's or a kit's pitch bend.
const ParamInfo wheel_info {"MOD WHEEL", 0, 127, 0, Format::Number};
const ParamInfo bend_info {"PITCH BEND", -64, 63, 0, Format::Number};
const ParamInfo pad_mute_info {"MUTE", 0, 1, 0, Format::Switch}; // issue 32: a kit pad's mute lane

// Roadmap P4: the mix's lanes as Motion shows them.
const ParamInfo level_info {"LEVEL", 0, 127, 100, Format::Level};
const ParamInfo pan_info {"PAN", -50, 50, 0, Format::Number};
const std::array<ParamInfo, bus_count> send_infos {{{"SEND A", 0, 100, 0, Format::Percent}, {"SEND B", 0, 100, 0, Format::Percent},
                                                     {"SEND C", 0, 100, 0, Format::Percent}, {"SEND D", 0, 100, 0, Format::Percent}}};
const std::array<ParamInfo, bus_count> return_infos {{{"RETURN A", 0, 100, 70, Format::Percent}, {"RETURN B", 0, 100, 70, Format::Percent},
                                                       {"RETURN C", 0, 100, 70, Format::Percent}, {"RETURN D", 0, 100, 70, Format::Percent}}};

// The mixer's strip lanes (model/strip.h strip_lane_parts) as Motion names them.
const char* const strip_speed_choices[] {"FAST", "MED", "SLOW", "AUTO"};
const std::array<ParamInfo, motion_strip_count> strip_infos {{
    {"TRIM", -trim_range, trim_range, 0, Format::Tenths}, {"WIDTH", 0, int16_t(width_max), 100, Format::Percent},
    {"63 Hz", -eq_range, eq_range, 0, Format::Tenths}, {"125 Hz", -eq_range, eq_range, 0, Format::Tenths},
    {"250 Hz", -eq_range, eq_range, 0, Format::Tenths}, {"500 Hz", -eq_range, eq_range, 0, Format::Tenths},
    {"1 kHz", -eq_range, eq_range, 0, Format::Tenths}, {"2 kHz", -eq_range, eq_range, 0, Format::Tenths},
    {"4 kHz", -eq_range, eq_range, 0, Format::Tenths}, {"8 kHz", -eq_range, eq_range, 0, Format::Tenths},
    {"16 kHz", -eq_range, eq_range, 0, Format::Tenths}, {"AMOUNT", 0, int16_t(comp_amount_max), 0, Format::Number},
    {"RATIO", int16_t(comp_ratio_min), int16_t(comp_ratio_max), 40, Format::Tenths},
    {"SPEED", 0, int16_t(int(CompSpeed::Count) - 1), 1, Format::Choice, strip_speed_choices}, {"MIX", 0, 100, 100, Format::Percent},
    {"GAIN", int16_t(comp_gain_min), int16_t(comp_gain_max), 0, Format::Tenths}}};

MotionParam motion_param(const Track& track, int lane, const Project* project) {
    if (lane < 0) return {};
    int param = int(lane_param(lane));
    if (is_strip_lane(unsigned(param))) // the mixer: the track's strip
        return {&strip_infos[unsigned(param) - motion_strip_base], strip_get(track.strip, strip_lane_part(unsigned(param)))};
    if (unsigned(param) == motion_wheel) return {&wheel_info, track.wheel};
    if (unsigned(param) == motion_bend) return track.engine == EngineKind::Plugin || track.sampled() ? MotionParam {&bend_info, track.bend} : MotionParam {};
    if (is_mix_lane(unsigned(param))) { // roadmap P4
        unsigned p = unsigned(param);
        if (p == motion_level) return {&level_info, track.mix.level};
        if (p == motion_pan) return {&pan_info, track.mix.pan};
        if (p < motion_master_fx_base) return {&send_infos[(p - motion_send(0)) % bus_count], track.mix.sends[(p - motion_send(0)) % bus_count]};
        if (!project) return {};
        if (p >= motion_return_base) { unsigned b = (p - motion_return_base) % bus_count; return {&return_infos[b], project->buses[b].level}; }
        unsigned slot = (p - motion_master_fx_base) / effect_params, q = (p - motion_master_fx_base) % effect_params;
        if (slot >= project->master.count()) return {};
        const auto& s = project->master.slots[slot];
        const auto& pi = effect_info(EffectKind(s.kind)).params[q];
        if (!pi.label || s.kind == plugin_effect_kind) return {};
        return {&pi, s.params[q]};
    }
    if (unsigned(param) == motion_pad_mute) return track.sampled() ? MotionParam {&pad_mute_info, int(track.pad_muted(lane_pad(lane)))} : MotionParam {};
    if (int which = pad_param_of(unsigned(param)); which >= 0) { // a kit pad's own value
        auto p = PadParam(which);
        if (!track.sampled()) return {};
        return {&pad_infos[which], pad_param_value(track.pads[lane_pad(lane) % sampler_pads], p)};
    }
    if (unsigned(param) < motion_fx_base) {
        const auto& info = engine_info(track.engine);
        if (unsigned(param) >= info.param_count) return {};
        return {&track_param_info(track.engine, track.params, unsigned(param)), track.params[size_t(param)]};
    }
    unsigned slot = (unsigned(param) - motion_fx_base) / effect_params, q = (unsigned(param) - motion_fx_base) % effect_params;
    if (slot >= track.fx.count()) return {};
    const auto& s = track.fx.slots[slot];
    const auto& pi = effect_info(EffectKind(s.kind)).params[q];
    if (!pi.label) return {};
    return {&pi, s.params[q]};
}

std::string lane_name(const Track& track, int lane, const Project* project) {
    auto m = motion_param(track, lane, project);
    if (!m.info) return "lane";
    if (is_master_lane(lane_param(lane)) && lane_param(lane) < motion_return_base) return std::string("MASTER ") + m.info->label; // roadmap P4
    if (!is_pad_lane(lane_param(lane))) return m.info->label;
    char label[4];
    pad_label(lane_pad(lane), label);
    return std::string(label) + " " + m.info->label;
}

Binding lane_binding(const Track& track, unsigned t, int lane, const Project* project) {
    int param = int(lane_param(lane));
    if (unsigned(param) == motion_wheel) return {Kind::ModWheel, 0, 0, 127, track.wheel};
    if (unsigned(param) == motion_bend) return track.engine == EngineKind::Plugin || track.sampled() ? Binding {Kind::PitchBend, 0, -64, 63, track.bend} : Binding {};
    if (unsigned(param) == motion_pad_mute) // issue 32: the pad's mute, as Mute + pad sets it
        return track.sampled() ? Binding {Kind::PadMute, int32_t(lane_pad(lane) % sampler_pads), 0, 1, int(track.pad_muted(lane_pad(lane)))} : Binding {};
    if (is_strip_lane(unsigned(param))) return strip_card(t, strip_lane_part(unsigned(param)), track.strip); // the mixer
    if (is_mix_lane(unsigned(param))) { // roadmap P4: the card that turns it (the selected track's strip, the master's, a return)
        auto m = motion_param(track, lane, project);
        if (!m.info) return {};
        unsigned p = unsigned(param);
        if (p == motion_level) return {Kind::Level, 0, 0, 127, m.value};
        if (p == motion_pan) return {Kind::Pan, 0, -50, 50, m.value};
        if (p < motion_master_fx_base) return {Kind::Send, int32_t((p - motion_send(0)) % bus_count), 0, 100, m.value};
        if (p >= motion_return_base) return {Kind::BusReturn, int32_t((p - motion_return_base) % bus_count), 0, 100, m.value};
        unsigned slot = (p - motion_master_fx_base) / effect_params, q = (p - motion_master_fx_base) % effect_params;
        return {Kind::FxParam, int32_t(master_chain * 64 + slot * 8 + q), m.info->min, m.info->max, m.value};
    }
    if (int which = pad_param_of(unsigned(param)); which >= 0) { // the pad's card, as on the sampler pages (they edit the last pad hit)
        auto m = motion_param(track, lane);
        if (!m.info || lane_pad(lane) != track.last_pad % sampler_pads) return {};
        int id = which >= int(PadParam::CloudPosition) ? cloud_id(PadParam(which)) // a cloud card names its value
               : which >= int(PadParam::SendA) ? which - int(PadParam::SendA) : 0; // a send's its bus
        return {pad_kind(PadParam(which)), id, m.info->min, m.info->max, m.value};
    }
    if (param >= int(motion_fx_base)) {
        auto m = motion_param(track, param);
        if (!m.info) return {};
        unsigned slot = (unsigned(param) - motion_fx_base) / effect_params, q = (unsigned(param) - motion_fx_base) % effect_params;
        return {Kind::FxParam, int32_t(t * 64 + slot * 8 + q), m.info->min, m.info->max, m.value};
    }
    return engine_param(track, param);
}

std::array<Binding, 3> lock_cards(const Project& project, const UiState& ui, const Pattern& pattern, uint32_t tick) {
    unsigned t = ui.track % track_count;
    const auto& track = project.tracks[t];
    auto lanes = motion_lanes(project, ui, pattern);
    std::array<Binding, 3> out;
    for (unsigned k = 0; k < 3; ++k) {
        if (lanes[k] < 0) continue;
        Binding b = lane_binding(track, t, lanes[k], &project);
        if (!b.bound()) continue;
        const MotionPoint* lock = pattern.lock_at(tick, lane_param(lanes[k]), lane_pad(lanes[k]));
        out[k] = {Kind::Lock, lanes[k], b.min, b.max, lock ? int(lock->value) : b.value};
    }
    return out;
}

std::array<Binding, 3> feel_cards(const Pattern& pattern, unsigned row) {
    const auto& f = pattern.feel;
    if (row % 2 == 0)
        return {Binding {Kind::FeelStrength, 0, 0, 100, f.strength}, Binding {Kind::FeelSwing, 0, 50, 75, f.swing},
                Binding {Kind::FeelHumanize, 0, 0, 100, f.humanize}};
    return {Binding {Kind::FeelVelocity, 0, 0, 100, f.humanize_velocity},
            Binding {Kind::GrooveFrom, 0, 0, int(track_count * pattern_count), f.groove_from},
            Binding {Kind::GrooveAmount, 0, 0, 100, f.groove_amount}};
}

bool lock_place(const Project& project, const UiState& ui, const Pattern& pattern, uint32_t& tick, uint32_t& hold) {
    const auto& track = project.tracks[ui.track % track_count];
    unsigned q = pattern.q();
    // Step Edit: a step held (the pads on the steps), or the caret's on LOCK; as long as its longest note on its lane
    // (a step at least)
    if (ui.page == PageId::StepEdit && (ui.held_step >= 0 || step_set(track, ui.step_cards) == cards_lock)) {
        unsigned st = unsigned(ui.held_step >= 0 ? ui.held_step : std::max<int>(ui.step_cursor, 0)) % pattern.steps();
        tick = st * q; hold = q;
        int lane = steps_lane(track);
        for (size_t n = pattern.lower(tick); n < pattern.events.size() && pattern.events[n].tick < tick + q; ++n)
            if (lane < 0 || pattern.events[n].key == lane) hold = std::max<uint32_t>(hold, pattern.events[n].length);
        return true;
    }
    if (ui.page == PageId::Roll && ui.held_note >= 0) { // the note in focus, as long as it is
        int at = roll_focus(pattern, ui, roll_lane(track));
        if (at < 0) return false;
        tick = pattern.events[size_t(at)].tick; hold = std::max<uint32_t>(pattern.events[size_t(at)].length, 1);
        return true;
    }
    return false;
}

Binding nudge_card(const Pattern& pattern, const Event& e) {
    int q = int(pattern.q());
    return {Kind::EventNudge, 0, -(q / 2), (q + 1) / 2 - 1, int(e.tick) - int(nudge_base(pattern, e))};
}

// The kit pad value a card turns (model/pad_params.h), -1 for any other card.
static int card_pad_param(const Binding& b) {
    for (unsigned p = 0; p < pad_param_count; ++p)
        if (pad_kind(PadParam(p)) == b.kind && (b.kind != Kind::PadSend || p == unsigned(PadParam::SendA) + unsigned(b.id) % bus_count)
            && (b.kind != Kind::PadCloud || cloud_id(PadParam(p)) == b.id))
            return int(p);
    return -1;
}

int binding_lane(const Track& track, const Binding& b) {
    if (b.kind == Kind::Param) return motion_lane(unsigned(b.id));
    if (b.kind == Kind::ModWheel) return motion_lane(motion_wheel);
    if (b.kind == Kind::PitchBend) return motion_lane(motion_bend);
    if (b.kind == Kind::Strip && (unsigned(b.id) >> 8) < track_count) { const int lane = motion_strip(StripPart(b.id & 0xff)); return lane >= 0 ? motion_lane(unsigned(lane)) : -1; }
    if (!track.sampled()) return -1;
    if (int p = card_pad_param(b); p >= 0) return motion_lane(motion_pad_param(PadParam(p)), track.last_pad % sampler_pads); // on the last pad hit
    return -1;
}

bool pad_reset_value(const Binding& b, int& value) {
    static const PadSound fresh {};
    if (b.kind == Kind::PadWarp) { value = warp_card(fresh.warp); return true; }
    if (b.kind == Kind::PadLoopXfade) { value = fresh.loop_xfade; return true; }
    if (int p = card_pad_param(b); p >= 0) { value = pad_param_value(fresh, PadParam(p)); return true; }
    return false;
}

bool erase_resets(const Binding& b) {
    int value = 0;
    return b.kind == Kind::Param || b.kind == Kind::KnobParam || b.kind == Kind::PluginParam || b.kind == Kind::Lock
        || pad_reset_value(b, value);
}

namespace {
std::array<Binding, 3> effect_row(unsigned id, const EffectSlot& s, unsigned row);
std::array<Binding, 3> plugin_cards(const UiState& ui, unsigned entry);
} // namespace

std::array<Binding, 3> mixer_cards(const Project& project, const UiState& ui, bool shift_row) {
    const unsigned n = mixer_strip(ui);
    const ChannelKind kind = channel_kind(n);
    if (kind == ChannelKind::Track && !project.tracks[n].used()) return {};
    auto strip = [&](StripPart part) { return strip_card(n, part, project.strip(n)); };
    // SENDS: a track's or a submix's sends, pan and (a track's) output.
    Binding pan, out;
    std::array<Binding, bus_count> sends;
    if (kind == ChannelKind::Track) {
        const auto& mix = project.tracks[n].mix;
        pan = {Kind::Pan, 0, -50, 50, mix.pan};
        out = {Kind::TrackOut, int32_t(n), 0, int(sub_count), mix.out};
        for (unsigned b = 0; b < bus_count; ++b) sends[b] = {Kind::Send, int32_t(b), 0, 100, mix.sends[b]};
    } else if (kind == ChannelKind::Submix) {
        const unsigned sub = channel_sub(n);
        const auto& mix = project.subs[sub].mix;
        auto part = [&](unsigned p) { return int32_t(sub << 8 | p); };
        pan = {Kind::SubMix, part(Command::SubPan), -50, 50, mix.pan};
        for (unsigned b = 0; b < bus_count; ++b) sends[b] = {Kind::SubMix, part(Command::SubSend0 + b), 0, 100, mix.sends[b]};
    }
    const unsigned block = mixer_block_of(ui, kind);
    const bool second = shift_row != (ui.mixer_row != 0); // − / + left SENDS or COMP on its second row: Shift shows the first
    if (!full_features(ui)) return second ? std::array<Binding, 3> {pan, Binding {}, Binding {}} : std::array<Binding, 3> {sends[0], Binding {}, Binding {}};
    if (block == block_input) { // the input: its strip's trim, Input Config's monitor, and dry or wet takes
        if (shift_row) return {};
        return {strip(StripPart::Trim), Binding {Kind::InputMonitor, 0, 0, int(Monitor::Count) - 1, int(ui.settings.input.monitor)},
                Binding {Kind::InputWet, 0, 0, 1, project.input_wet ? 1 : 0}};
    }
    if (block == block_comp && kind == ChannelKind::Master) { // the printed Compressor, as its page: Shift + K3 In Boost
        const auto& c = project.compressor;
        if (second) return {Binding {}, Binding {}, Binding {Kind::CompBoost, 0, 0, 24, c.boost}};
        return {Binding {Kind::CompAttack, 0, 0, 100, c.attack}, Binding {Kind::CompRelease, 0, 0, 100, c.release},
                Binding {Kind::CompAmount, 0, 0, 100, c.amount}};
    }
    if (block == block_fx) { // the lit effect of the channel's chain, as its FX page has it (the same cursor and row)
        const int chain = channel_chain(n);
        if (chain < 0) return {};
        const auto& c = project.chain(unsigned(chain));
        const unsigned at = UiState::cursor_of(unsigned(chain)), slot = ui.fx_cursor[at];
        if (slot >= c.count()) return {}; // the plus
        if (c.slots[slot].kind == plugin_effect_kind) return shift_row ? std::array<Binding, 3> {} : plugin_cards(ui, unsigned(c.slots[slot].params[0]));
        return effect_row(unsigned(chain) * 64 + slot * 8, c.slots[slot], shift_row ? 1 - ui.fx_row[at] % 2 : ui.fx_row[at]);
    }
    shift_row = second;
    switch (block) {
    case 1: { // EQ: the group's three bands
        if (shift_row) return {};
        const unsigned g = ui.mixer_eq_group % 3;
        return {strip(eq_part(g * 3)), strip(eq_part(g * 3 + 1)), strip(eq_part(g * 3 + 2))};
    }
    case 2:
        if (shift_row) return {strip(StripPart::Key), strip(StripPart::Mix), strip(StripPart::Gain)};
        return {strip(StripPart::Amount), strip(StripPart::Ratio), strip(StripPart::Speed)};
    default:
        if (shift_row) return {pan, out, sends[3]};
        return {sends[0], sends[1], sends[2]};
    }
}

std::array<int, 3> motion_lanes(const Project& project, const UiState& ui, const Pattern& pattern) {
    const auto& track = project.tracks[ui.track % track_count];
    const auto& info = engine_info(track.engine);
    std::array<int, 3> lanes {-1, -1, -1};
    unsigned n = 0;
    auto add = [&](int param) {
        if (!motion_param(track, param, &project).info || n == lanes.size()) return;
        for (unsigned k = 0; k < n; ++k) if (lanes[k] == param) return;
        lanes[n++] = param;
    };
    bool kit = track.engine == EngineKind::Sampler;
    unsigned pad = track.last_pad % sampler_pads;
    for (const auto& m : pattern.motion) {
        bool pad_lane = is_pad_lane(m.param);
        if (pad_lane && m.pad != pad) continue; // another pad's lane: shown when that pad is the last hit
        add(motion_lane(m.param, pad_lane ? m.pad : 0));
    }
    if (kit && track.pads[pad].used()) // a kit: the last pad's tune, level and pan, as on Pads
        for (auto p : {PadParam::Pitch, PadParam::Level, PadParam::Pan}) add(motion_lane(motion_pad_param(p), pad));
    for (auto f : info.favourites) add(f);
    return lanes;
}

unsigned effect_rows(const EffectSlot& s, unsigned chain) {
    const auto& info = effect_info(EffectKind(s.kind));
    unsigned own = info.params[3].label ? 2 : 1;
    return own + (fx_keyable(s.kind) && chain < mix_chain_count ? 1 : 0);
}

namespace {
// One row of an effect's parameters on K1-K3 (row 1 its first three); `id` the binding's chain and slot.
std::array<Binding, 3> effect_row(unsigned id, const EffectSlot& s, unsigned row) {
    const auto& info = effect_info(EffectKind(s.kind));
    std::array<Binding, 3> out;
    if (s.kind == plugin_effect_kind) return out; // its entry is no card: the plug-in's own are (plugin_cards())
    unsigned rows = effect_rows(s, id / 64);
    row = std::min(row, rows - 1);
    if (fx_keyable(s.kind) && id / 64 < mix_chain_count && row == rows - 1) { // roadmap P4: KEY, a row of its own on K1
        out[0] = {Kind::FxKey, int32_t(id), 0, int(fx_keys) - 1, s.key};
        return out;
    }
    for (unsigned k = 0; k < 3; ++k) {
        unsigned q = row * 3 + k;
        const auto& pi = info.params[q];
        if (pi.label) out[k] = {Kind::FxParam, int32_t(id + q), pi.min, pi.max, s.params[q]};
    }
    return out;
}
// A plug-in's row of three, as the shell last read it into the panel (plugin/plugin_panel.h); none while another
// plug-in's panel is up or it is still loading.
std::array<Binding, 3> plugin_cards(const UiState& ui, unsigned entry) {
    std::array<Binding, 3> out;
    const auto& panel = ui.plugin_panel;
    if (panel.entry != entry || panel.status != PluginStatus::Ready) return out;
    for (unsigned k = 0; k < 3; ++k)
        if (panel.params[k].used) {
            unsigned shown = panel.screen ? plugin_screen_slot + k : panel.row * 3u + k; // its own screen's three
            out[k] = {Kind::PluginParam, plugin_binding(entry, shown), 0, plugin_param_max, panel.params[k].value};
        }
    return out;
}
// Knob FX's row (Shift: its second), while the mode is on.
std::array<Binding, 3> knob_bindings(const Project& project, bool shift) {
    const auto& k = project.knob_fx;
    const auto& info = effect_info(EffectKind(k.kind));
    std::array<Binding, 3> out;
    for (unsigned n = 0; n < 3; ++n) {
        unsigned q = (shift ? 3 : 0) + n;
        const auto& pi = info.params[q];
        if (pi.label) out[n] = {Kind::KnobParam, int32_t(q), pi.min, pi.max, q < 3 ? k.setting()[q] : k.params[q]}; // K1-K3: A or B in hand
    }
    return out;
}
bool knob_owns(const Project& project, const UiState& ui) { return project.knob_fx.on && project.knob_fx.kind && !ui.name.open(); }

// A kit's Sample Mode cards on the last pad hit, as `page` (a sampler page) has them; row 2 where it has one.
// The sampler pages and Stage's kit pages share them.
std::array<Binding, 3> pad_page_bindings(const Project& project, const Track& track, const UiState& ui, PageId page, bool row2) {
    const auto& pad = track.pads[track.last_pad % sampler_pads];
    if (page == PageId::Trim) { // K1 start, K2 end, K3 the loop point (Shift: zoom at each)
        const auto& ref = project.samples[pad.sample % sample_slots];
        if (pad.used() && ref.instrument()) { // round 3 "instrument C": ZONES, K1 the zone in hand, K2 / K3 its trim
            if (!ref.zones) return {};
            unsigned zone = std::min<unsigned>(ui.zone, ref.zones - 1u);
            const ZoneTrim* trim = find_trim(track.zone_trims, track.last_pad % sampler_pads, zone, pad.sample);
            return {Binding {Kind::ZonePick, 0, 0, int(ref.zones) - 1, int(zone)},
                    Binding {Kind::ZoneTune, int32_t(zone), -zone_trim_cents, zone_trim_cents, trim ? trim->tune : 0},
                    Binding {Kind::ZoneLevel, int32_t(zone), zone_trim_low, zone_trim_high, trim ? trim->level : 0}};
        }
        auto h = trim_handles(project, ui); // in frames; zoomed in, each pot turns across the view on screen
        if (!h.frames) return {};
        FrameView view = ui.trim_zoom ? trim_view(project, ui) : FrameView {};
        uint32_t last = h.end > h.start ? h.end - 1 : h.start;
        return {handle_card(Kind::PadStart, 0, h.start, 0, last, view), handle_card(Kind::PadEnd, 0, h.end, h.start + 1, h.frames, view),
                handle_card(Kind::PadLoopPoint, 0, std::min(h.loop, last), h.start, std::max(h.start + 1, last), view)};
    }
    if (!track.sampled()) return {};
    switch (page) { // Melodee's kit-wide pages, with or without a pad
    case PageId::FilterPage: return {param(track, sampler::Filter), param(track, sampler::Resonance), param(track, sampler::FilterType)};
    case PageId::FollowPage: return {param(track, sampler::Follow), param(track, sampler::FollowHow), param(track, sampler::PitchHow)};
    default: break;
    }
    if (!pad.used()) return {};
    switch (page) {
    case PageId::CloudPage: // the pad's cloud: where, grain size, grains a second; row 2 scan, spread and Cloud on or off
        if (row2) return {cloud_card(pad, 4), cloud_card(pad, 5), cloud_card(pad, 0)};
        return {cloud_card(pad, 1), cloud_card(pad, 2), cloud_card(pad, 3)};
    case PageId::LayersPage: { // roadmap P3: K1 the layer in hand, K2 / K3 its velocities (Velocity alone)
        unsigned n = ui.pad_layer % pad_layers;
        const PadLayer l = pad.layer(n);
        Binding pick {Kind::LayerPick, 0, 0, int(pad_layers) - 1, int(n)};
        if (!l.used() || pad.layer_play != LayerPlay::Velocity) return {pick, Binding {}, Binding {}};
        return {pick, Binding {Kind::LayerLow, int32_t(n), 1, 127, l.low}, Binding {Kind::LayerHigh, int32_t(n), 1, 127, l.high}};
    }
    case PageId::PadMix: // K1 volume, K3 pan (Shift + K2 the kit's volume)
        return {Binding {Kind::PadLevel, 0, 0, 127, pad.level}, Binding {}, Binding {Kind::PadPan, 0, -50, 50, pad.pan}};
    case PageId::PadPage: { // Tune: semitones, cents, Warp (Shift + K3 # Beats); Warp on by Pitch has no Semi or Fine
        bool tuned = !(pad.warped() && track.params[sampler::FollowHow] == 2);
        return {tuned ? Binding {Kind::PadPitch, 0, -24, 24, pad.pitch} : Binding {}, tuned ? Binding {Kind::PadFine, 0, -50, 50, pad.fine} : Binding {},
                Binding {Kind::PadWarp, 0, 0, warp_card_seq, warp_card(pad.warp)}};
    }
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
    case PageId::FilterEnvPage: // Filt Env: attack, decay or release, depth; row 2 sustain and the other
        if (row2) return {Binding {Kind::PadFSustain, 0, 0, 100, pad.filter_sustain}, filter_second(pad, true), Binding {}};
        return {Binding {Kind::PadFAttack, 0, 0, 100, pad.filter_attack}, filter_second(pad, false),
                Binding {Kind::PadFAmount, 0, 0, 100, pad.filter_amount}};
    default: return {};
    }
}

// Their Shift + K cards, as on the MPC: zoom on Trim, the kit's volume on Mix, Decay From on the envelopes,
// # Beats on Tune, Pad Link on Play; on Stage's CLOUD, Cloud's row 2 (scan, spread, on or off).
std::array<Binding, 3> pad_page_shift(const Project& project, const Track& track, const UiState& ui, PageId page) {
    const auto& pad = track.pads[track.last_pad % sampler_pads];
    if (page == PageId::Trim) {
        if (pad.used() && project.samples[pad.sample % sample_slots].instrument()) return {}; // ZONES: nothing to zoom
        return {trim_zoom(ui, 0), trim_zoom(ui, 1), trim_zoom(ui, 2)};
    }
    if (track.engine != EngineKind::Sampler) return {};
    switch (page) {
    case PageId::PadMix: return {Binding {}, param(track, sampler::Level), Binding {}};
    case PageId::EnvelopePage: case PageId::FilterEnvPage:
        if (!pad.used()) return {};
        return {Binding {}, Binding {Kind::PadDecayFrom, 0, 0, 1, pad.decay_from_start ? 0 : 1}, Binding {}};
    case PageId::PadPage:
        if (!pad.used()) return {};
        return {Binding {}, Binding {}, Binding {Kind::PadBeats, 0, 0, int(max_pad_beats), pad.beats}};
    case PageId::PlayPage: // Shift + K2 Pad Link (row 1), Shift + K3 the loop's seam crossfade (roadmap P3)
        if (!pad.used()) return {};
        return {Binding {}, ui.sampler_row ? Binding {} : Binding {Kind::PadLink, 0, 0, 16, pad.link},
                Binding {Kind::PadLoopXfade, 0, 0, int(max_loop_xfade), pad.loop_xfade}};
    case PageId::LayersPage: { // Shift + K1 how a hit picks; Shift + K2 / K3 the layer's tune and level (layer 1's are the pad's)
        if (!pad.used()) return {};
        unsigned n = ui.pad_layer % pad_layers;
        Binding play {Kind::LayerPlayMode, 0, 0, int(LayerPlay::Count) - 1, int(pad.layer_play)};
        if (!n || !pad.layers[n - 1].used()) return {play, Binding {}, Binding {}};
        const auto& l = pad.layers[n - 1];
        return {play, Binding {Kind::LayerPitch, int32_t(n), -24, 24, l.pitch}, Binding {Kind::LayerGain, int32_t(n), -480, 120, l.gain / 10}};
    }
    case PageId::CloudPage:
        if (!pad.used()) return {};
        return {cloud_card(pad, 4), cloud_card(pad, 5), cloud_card(pad, 0)};
    default: return {};
    }
}

// The sound PRESET's K1 sits on: where the cursor is while a turn waits to load, else the loaded sound.
int preset_position(const Track& track, const UiState& ui, unsigned& listed) {
    std::array<uint16_t, max_listed> list {};
    listed = sound_list(track, ui, list);
    if (ui.preset_due) return std::min<int>(ui.preset_cursor, int(listed) - 1);
    for (unsigned n = 0; n < listed; ++n) if (list[n] == track.preset) return int(n);
    return 0;
}
} // namespace

unsigned new_engines(std::array<EngineKind, size_t(EngineKind::Count)>& out) {
    std::array<EngineKind, size_t(EngineKind::Count)> all {};
    unsigned count = available_engines(all), n = 0;
    for (unsigned k = 0; k < count; ++k) // PLUGIN (Melodee's synths are plug-ins): a kit and MIDI have buttons of their own
        if (all[k] != EngineKind::Sampler && all[k] != EngineKind::Midi) out[n++] = all[k];
    return n;
}

std::array<Binding, 3> stage_bindings(const Project& project, const UiState& ui, const Pattern& pattern, bool shift_row) {
    const auto& track = project.tracks[ui.track % track_count];
    const auto& page = stage_page_now(ui, track, stage_skip(project, ui, track));
    using SK = StageKind;
    if (PageId kit = stage_kit_page(page.kind); kit != PageId::Count) {
        if (shift_row) return page.kind == SK::PadMix ? std::array<Binding, 3> {} : pad_page_shift(project, track, ui, kit);
        auto cards = pad_page_bindings(project, track, ui, kit, false);
        if (page.kind == SK::PadMix && track.pads[track.last_pad % sampler_pads].used()) cards[1] = param(track, sampler::Level); // K2 the kit's volume
        return cards;
    }
    unsigned t = ui.track % track_count;
    if (page.kind == SK::Colour) { // K1 (and the wheel) the last pad's colour; Shift nothing
        const auto& pad = track.pads[track.last_pad % sampler_pads];
        if (shift_row || track.engine != EngineKind::Sampler || !pad.used()) return {};
        return {Binding {Kind::PadColour, 0, colour_track, colour_last(pad), colour_choice(pad.colour)}, Binding {}, Binding {}};
    }
    switch (page.kind) {
    case SK::Params: {
        const auto& ids = shift_row ? page.shift : page.params;
        return {param(track, ids[0]), param(track, ids[1]), param(track, ids[2])};
    }
    case SK::Preset: {
        if (shift_row) return {};
        unsigned listed = 0;
        int at = preset_position(track, ui, listed);
        std::array<EngineKind, size_t(EngineKind::Count)> engines {};
        unsigned count = available_engines(engines), here = 0;
        for (unsigned n = 0; n < count; ++n) if (engines[n] == track.engine) here = n;
        return {Binding {Kind::StageSound, 0, 0, int(std::max(listed, 2u)) - 1, at}, Binding {Kind::SoundList, 0, 0, 2, ui.sounds_filter},
                Binding {Kind::Engine, 0, 0, int(std::max(count, 2u)) - 1, int(here)}};
    }
    case SK::Clip: case SK::Fades: case SK::Warp: { // freeze plan F3: the take of the pattern in hand
        const auto& r = pattern.audio;
        if (!r.used()) return {};
        if (page.kind == SK::Warp) {
            if (shift_row) return {Binding {}, Binding {}, Binding {Kind::RegionFine, 0, -50, 50, r.fine}};
            const int how = r.warp ? (r.mode == WarpMode::Repitch ? 2 : 1) : 0;
            return {Binding {Kind::RegionWarp, 0, 0, 2, how}, Binding {Kind::RegionBpm, 0, 300, 3000, r.tempo}, Binding {Kind::RegionSemi, 0, -24, 24, r.semi}};
        }
        if (shift_row) return {};
        auto ms = [](uint32_t frames) { return int((uint64_t(frames) * 10 + 220) / 441); };
        const int take_ms = ms(r.take_frames), start = ms(r.offset), end = ms(r.offset + r.frames), length = end - start;
        if (page.kind == SK::Clip)
            return {Binding {Kind::RegionGain, 0, region_gain_min, region_gain_max, r.gain}, Binding {Kind::RegionStart, 0, 0, std::max(end - 1, 1), start},
                    Binding {Kind::RegionEnd, 0, std::min(start + 1, take_ms - 1), std::max(take_ms, 1), end}};
        return {Binding {Kind::RegionFadeIn, 0, 0, std::max(length, 1), ms(r.fade_in)}, Binding {Kind::RegionFadeOut, 0, 0, std::max(length, 1), ms(r.fade_out)},
                Binding {Kind::RegionReverse, 0, 0, 1, int(r.reverse)}};
    }
    case SK::AudioInput: { // freeze plan F3: what the track records from, whether it is heard, the input's gain
        if (shift_row) return {};
        return {Binding {Kind::AudioSource, 0, 0, int(audio_source_count) - 1, int(audio_source_index(track.audio.source))},
                Binding {Kind::AudioMonitorMode, 0, 0, int(TrackMonitor::Count) - 1, int(track.audio.monitor)},
                Binding {Kind::RecGain, 0, -12, 24, ui.settings.input.gain}};
    }
    case SK::Sends: { // a kit's only, since the mixer (step 6): K1 the last pad's own send, K2 the bus (A-D), K3 its return
        if (track.engine != EngineKind::Sampler) return {};
        unsigned bus = ui.sends_bus % bus_count;
        const auto& pad = track.pads[track.last_pad % sampler_pads];
        if (shift_row) // Shift + K2: the last pad's output, the kit or a submix (MPC 3's pad output)
            return {Binding {}, pad.used() ? Binding {Kind::PadOut, 0, 0, int(sub_count), int(pad.out)} : Binding {}, Binding {}};
        return {pad.used() ? Binding {Kind::PadSend, int(bus), 0, 100, pad.sends[bus]} : Binding {},
                Binding {Kind::SendBus, 0, 0, int(bus_count) - 1, int(bus)},
                Binding {Kind::BusReturn, int16_t(bus), 0, 100, project.buses[bus].level}};
    }
    case SK::Arp:
        if (shift_row) return {Binding {Kind::ArpGate, 0, 5, 100, track.arp_gate}, Binding {Kind::ArpOn, 0, 0, 1, track.arp}, Binding {Kind::ArpHold, 0, 0, 1, track.arp_latch}};
        return {Binding {Kind::ArpMode, 0, 0, 4, track.arp_mode}, Binding {Kind::ArpRate, 0, 0, int(arp_rates) - 1, track.arp_rate},
                Binding {Kind::ArpOctaves, 0, 1, 4, track.arp_octaves}};
    case SK::Chord:
        if (shift_row) return {Binding {Kind::ChordOn, 0, 0, 1, track.chord}, Binding {}, Binding {Kind::ArpHold, 0, 0, 1, track.arp_latch}};
        return {Binding {Kind::ChordType, 0, 0, 5, track.chord_type}, Binding {Kind::ChordVoicing, 0, 0, 3, track.chord_voicing},
                Binding {Kind::ChordInvert, 0, 0, 3, track.chord_invert}};
    case SK::Fx: {
        unsigned slot = ui.fx_cursor[UiState::cursor_of(t)];
        if (slot >= track.fx.count()) return {};
        return effect_row(t * 64 + slot * 8, track.fx.slots[slot], shift_row ? 1 : 0);
    }
    case SK::PluginSound: // the three performance knobs, the same on every plug-in (engine/macros.h)
        if (shift_row) return {};
        return {param(track, plugin_track::Macro1), param(track, plugin_track::Macro2), param(track, plugin_track::Macro3)};
    case SK::PluginEdit:   // the plug-in's own row of three
    case SK::PluginScreen: // or the three its own screen names
        if (shift_row) return {};
        return plugin_cards(ui, track.plugin);
    case SK::NewSynth: case SK::NewKit: case SK::NewMidi: return {}; // an empty track: the wheel and Push (shell/new_track.h)
    default: return {};
    }
}

namespace {
// The project's key: K1 and K2 wherever the pads can play in it.
std::array<Binding, 3> project_key(const Project& project, Binding third = {}) {
    return {Binding {Binding::Kind::Root, 0, 0, 11, project.root},
            Binding {Binding::Kind::Scale, 0, 0, int(scales().size()) - 1, project.scale}, third};
}
} // namespace

Binding length_card(const Project& project, const UiState& ui, const Pattern& pattern) {
    uint32_t shown = free_shown(pattern, ui.column_ticks), bar = std::max(project.bar_ticks(), 1u);
    int low = free_offered(pattern, ui.column_ticks) ? -1 : 0, top = int(pattern_length_count) - 1;
    if (!shown) return Binding {Kind::PatternLength, 0, low, top, -1};
    unsigned bars = std::max(1u, unsigned((shown + bar - 1) / bar)), index = 0;
    while (index + 1 < pattern_length_count && pattern_lengths[index + 1] <= bars) ++index;
    return Binding {Kind::PatternLength, 0, low, top, int(index)};
}

namespace {
// The TRIG row of Steps (`step`: its step in hand's notes) and Roll (the note in focus), roadmap P1: K1 the ratchet's
// hits, K2 its velocity ramp, K3 its trig condition (beside the chance, K3 of the main row).
// 16 Levels held, on any page (sample_mode.h): K1 the type, K2 and K3 the two ends (pad 1's and pad 16's), or of
// type Scale the key's root and scale; Chords the progression's key and voicing; Zones only the type (− / + its pages).
std::array<Binding, 3> levels_cards(const Project& project, const Track& track) {
    std::array<LevelsKind, 6> types {};
    unsigned count = levels_types(project, track, types), at = 0;
    auto kind = LevelsKind(track.levels_kind % unsigned(LevelsKind::Count));
    for (unsigned n = 0; n < count; ++n) if (types[n] == kind) at = n;
    if (kind == LevelsKind::Filter && track.engine != EngineKind::Sampler) return {};
    if (kind == LevelsKind::Chords) // the progression's key and voicing, as Keys' Prog layout has them
        return {Binding {Kind::LevelsType, 0, 0, int(count) - 1, int(at)}, Binding {Kind::ProgKey, 0, 0, 11, int(prog_key(track))},
                Binding {Kind::ProgVoicing, 0, 0, int(ProgVoicing::Count) - 1, track.prog_voicing}};
    if (kind == LevelsKind::Zones) return {Binding {Kind::LevelsType, 0, 0, int(count) - 1, int(at)}};
    if (kind == LevelsKind::Scale) { // pad 1 the root, up the scale from there
        auto key = project_key(project);
        return {Binding {Kind::LevelsType, 0, 0, int(count) - 1, int(at)}, key[0], key[1]};
    }
    auto range = levels_range(kind);
    const auto& ends = track.levels_ends[unsigned(kind) % 4];
    return {Binding {Kind::LevelsType, 0, 0, int(count) - 1, int(at)}, Binding {Kind::LevelsLow, 0, range.low, range.high, ends[0]},
            Binding {Kind::LevelsHigh, 0, range.low, range.high, ends[1]}};
}

// Step Edit's SOUND row on step `at` (on a kit its lane's note): its sample, slice and way.
std::array<Binding, 3> sound_cards(const Project& project, const Track& track, const Pattern& pattern, unsigned at) {
    int n = lane_event(pattern, at, steps_lane(track));
    if (n < 0) return {};
    const auto& e = pattern.events[size_t(n)];
    const auto choices = step_sound_choices(project);
    const unsigned slices = step_sound_slices(project, track, e.key, e.sample);
    const int slice = e.slice == slice_random ? int(slices) + 1 : std::min<int>(e.slice, int(slices));
    return {Binding {Kind::StepSound, int(e.sample), 0, int(choices.size()), std::max(step_sound_index(choices, e.sample), 0)},
            Binding {Kind::StepSlice, int(slices), 0, slices ? int(slices) + 1 : 0, slices ? slice : 0},
            Binding {Kind::StepPlay, 0, 0, int(StepPlay::Count) - 1, int(step_play(e.flags))}};
}

std::array<Binding, 3> trig_cards(const Event& e, bool step) {
    return {Binding {step ? Kind::StepRatchet : Kind::EventRatchet, 0, 1, int(max_ratchet), int(e.hits())},
            Binding {step ? Kind::StepRamp : Kind::EventRamp, 0, -100, 100, e.ramp},
            Binding {step ? Kind::StepCondition : Kind::EventCondition, 0, 0, int(trig_count) - 1, e.condition}};
}
// Patterns' cards, and Seq held's on any page (shell/seq_layer.h): the selected track's pattern,
// K1 length by the bar, K2 Q, K3 swing; Shift: the length in sixteenths (issue 27), the time signature, the metronome's
// level (as on the MPC).
std::array<Binding, 3> pattern_cards(const Project& project, const UiState& ui, const Pattern& pattern) {
    return {length_card(project, ui, pattern),
            Binding {Kind::Division, 0, 0, int(Division::Count) - 1, int(pattern.division)},
            Binding {Kind::Swing, 0, 50, 75, project.swing}};
}
std::array<Binding, 3> pattern_shift_cards(const Project& project, const UiState& ui, const Pattern& pattern) {
    uint32_t length = free_shown(pattern, ui.column_ticks);
    if (!length) length = pattern.length;
    unsigned steps = std::clamp<unsigned>((length + sixteenth_ticks - 1) / sixteenth_ticks, 1, pattern_step_max);
    const auto& track = project.tracks[ui.track % track_count];
    const Binding speed = track.used() ? Binding {Kind::TrackSpeed, int(ui.track % track_count), 0, int(speed_count) - 1, track.speed % int(speed_count)} : Binding {};
    return {Binding {Kind::PatternSteps, 0, 1, int(pattern_step_max), int(steps)},
            Binding {Kind::TimeSignature, 0, 0, int(time_signature_count) - 1, project.signature},
            speed}; // the track's playback speed (roadmap P1; since the mixer, here: the click level is on Tempo)
}
} // namespace

std::array<Binding, 3> shift_bindings(const Project& project, const UiState& ui, const Pattern& pattern) {
    const auto& track = project.tracks[ui.track % track_count];
    if (ui.name.open()) return {};
    if (ui.seq_held) return pattern_shift_cards(project, ui, pattern); // Seq held: the pattern's, on any page
    if (ui.pad_bank || ui.copying == UiState::Copying::Tracks) return {}; // Pad Bank held (or a track move): the track's strip has one row
    if (ui.levels_held || ui.levels_sheet) return {}; // 16 Levels held, or its FILE or SET: no Shift row
    if (auto pads = sample_held_page(project, ui); pads != PageId::Count) { // Sample held: its pads' (Pads, Keys)
        UiState on = ui; on.page = pads; on.sample_held = false;
        return shift_bindings(project, on, pattern);
    }
    if (knob_owns(project, ui)) return knob_bindings(project, true);
    switch (ui.page) {
    case PageId::Mixer: return mixer_cards(project, ui, true);
    case PageId::Looper: { // Shift: feedback, Quantize, After take (the loop's sheet too); on MIX Shift + K3 the bus
        unsigned l = ui.loop % loop_tracks;
        if (!project.looper.on) return {};
        if (ui.looper_view == 1 && !ui.loop_sheet) return {Binding {}, Binding {}, Binding {Kind::LoopBus, int(l), 0, int(bus_count) - 1, project.loop_mix[l].bus}};
        return {Binding {Kind::LoopFeedback, int(l), 0, 100, project.looper.loops[l].feedback},
                Binding {Kind::LoopQuantize, 0, 0, int(looper::Quantize::Count) - 1, project.looper.quantize},
                Binding {Kind::LoopThen, 0, 0, int(looper::Then::Count) - 1, project.looper.then}};
    }
    case PageId::Record: case PageId::InputConfig: { // Shift + K1: a track's tap (before or after its effects); Shift + K3: Melodee's input gain
        const auto& in = ui.settings.input;
        Binding tap = in.source == InputSource::Track ? Binding {Kind::RecTap, 0, 0, 1, int(in.post)} : Binding {};
        return {tap, Binding {}, Binding {Kind::RecGain, 0, -12, 24, in.gain}};
    }
    case PageId::Patterns: // FOLLOW: Shift + K1 the follow action's second, on the odds the first leaves; SCENE none
        if (ui.sheet_open || ui.patterns_cards == 1) return {};
        if (ui.patterns_cards == 2 && follow_shown(ui))
            return track.used() ? std::array<Binding, 3> {Binding {Kind::FollowOther, 0, 0, int(FollowAction::Count) - 1, pattern.follow.other}, Binding {}, Binding {}}
                                : std::array<Binding, 3> {};
        return pattern_shift_cards(project, ui, pattern);
    case PageId::Samples: { // Shift + K2 / K3: the trim sheet's zoom at START or END, as on Trim
        if (ui.sheet_open || !(ui.browse.up() || ui.files_audio)) return {};
        const auto zoom = browse_zoom_cards(project, ui.browse);
        return {Binding {}, zoom[0], zoom[1]};
    }
    case PageId::Scenes: // roadmap P5: Shift + K1 the follow action's second, on the odds the first leaves
        if (!follow_shown(ui)) return pattern_shift_cards(project, ui, pattern); // Basic: as Patterns' Shift row
        if (!track.used()) return {};
        return {Binding {Kind::FollowOther, 0, 0, int(FollowAction::Count) - 1, pattern.follow.other}, Binding {}, Binding {}};
    case PageId::Song: { // roadmap P5: the tempo lane at the cursor's bar: its tempo, and (a point there) its shape
        if (ui.song_export || ui.jam_sheet) return {};
        const auto& lane = project.song.tempo;
        int at = lane.at_bar(ui.song_cursor);
        int tempo = at >= 0 ? (lane.points[size_t(at)].tempo + 5) / 10 : int(std::lround(lane.at(ui.song_cursor, project.tempo) / 10));
        Binding shape = at >= 0 ? Binding {Kind::SongTempoShape, 0, 0, 1, int(lane.points[size_t(at)].ramp)} : Binding {};
        return {Binding {Kind::SongTempo, 0, 29, 300, tempo}, shape, Binding {}};
    }
    case PageId::StepEdit: { // Shift + K1 the pattern's length, Shift + K2 Q (the caret's step), Shift + K3 the time signature;
        // on a kit lane (EUCLID, or the pads on the steps) Shift + K1 the lane's own length, on TRIG Shift + K2 its pitch ramp
        int lane = steps_lane(track);
        if (ui.held_step >= 0) return {};
        if (lane >= 0) {
            unsigned q = pattern.q(), steps = pattern.steps(), set = step_set(track, ui.step_cards);
            unsigned at = unsigned(std::max<int>(ui.step_cursor, 0)) % steps;
            if (set == cards_trig) {
                int n = lane_event(pattern, at, lane);
                if (n < 0) return {};
                return {Binding {}, Binding {Kind::StepPitchRamp, 0, -max_pitch_ramp, max_pitch_ramp, pattern.events[size_t(n)].pitch_ramp}, Binding {}};
            }
            if (set == cards_euclid || ui.step_pads) {
                unsigned own = std::clamp<unsigned>((pattern.lane_length(unsigned(lane)) + q - 1) / q, 1, steps);
                return {Binding {Kind::LaneLength, lane, 1, int(std::max(steps, 2u)), int(own)}, Binding {}, Binding {}};
            }
        }
        return {length_card(project, ui, pattern),
                Binding {Kind::Division, 0, 0, int(Division::Count) - 1, int(pattern.division)},
                Binding {Kind::TimeSignature, 0, 0, int(time_signature_count) - 1, project.signature}};
    }
    case PageId::Roll: { // Shift + K1 NUDGE (round 2): the note in focus off its grid line; the TRIG row: Shift + K2 a kit pad's pitch ramp
        int at = roll_focus(pattern, ui, roll_lane(track));
        Binding ramp; // marked notes: Shift + K2 their velocity ramp
        if (!ui.trig_cards && roll_selected(ui, track.pattern)) {
            std::vector<Event> notes;
            for (auto n : roll_marked_events(pattern, ui)) notes.push_back(pattern.events[n]);
            if (notes.size() >= 2) ramp = Binding {Kind::MarksRamp, 0, -100, 100, ramp_of(notes)};
        }
        if (at < 0) return {Binding {}, ramp, Binding {}};
        const auto& e = pattern.events[size_t(at)];
        if (!ui.trig_cards) return {nudge_card(pattern, e), ramp, Binding {}};
        if (track.engine != EngineKind::Sampler) return {};
        return {Binding {}, Binding {Kind::EventPitchRamp, 0, -max_pitch_ramp, max_pitch_ramp, e.pitch_ramp}, Binding {}};
    }
    case PageId::Trim: case PageId::PadMix: case PageId::EnvelopePage: case PageId::FilterEnvPage: case PageId::PadPage: case PageId::PlayPage:
    case PageId::LayersPage:
        return pad_page_shift(project, track, ui, ui.page);
    case PageId::ModPage: { // roadmap P4: a modulator's kind, then an LFO's shape and clock or a follower's source and fall; a macro target's low
        if (ui.mod_view == 1) {
            const auto& g = project.macros[ui.macro_index % macro_count].targets[ui.macro_target % macro_targets];
            if (!g.used()) return {};
            auto m = motion_param(project.tracks[g.track % track_count], motion_lane(g.param, g.pad), &project);
            if (!m.info) return {};
            return {Binding {}, Binding {}, Binding {Kind::MacroLow, 0, m.info->min, m.info->max, g.low}};
        }
        const auto& m = project.modulators[ui.mod_index % modulator_count];
        Binding kind {Kind::ModKind, 0, 0, int(ModKind::Count) - 1, int(m.kind)};
        if (m.kind == ModKind::Follower)
            return {kind, Binding {Kind::ModInput, 0, 0, int(track_count + bus_count) - 1, m.source}, Binding {Kind::ModRelease, 0, 0, 100, m.release}};
        if (m.kind == ModKind::Off) return {kind, Binding {}, Binding {}};
        return {kind, Binding {Kind::ModShape, 0, 0, int(LfoShape::Count) - 1, int(m.shape)}, Binding {Kind::ModSync, 0, 0, 1, int(m.sync)}};
    }
    case PageId::Chop: { // Shift + K1 / K2 zoom at the slice's start or end; Shift + K3 the type's amount (Threshold as on the MPC; Beats' division)
        static const int lows[] {0, 2, 0, 0, 0}, highs[] {100, int(max_slices), int(beat_divisions) - 1, 0, int(auto_counts) - 1};
        unsigned mode = unsigned(ui.chop_mode);
        Binding amount {};
        if (mode < unsigned(ChopMode::Count) && ChopMode(mode) != ChopMode::Manual)
            amount = {Kind::ChopAmount, int32_t(mode), lows[mode], highs[mode], ui.chop_amount[mode]};
        return {Binding {Kind::ChopZoom, 0, 0, zoom_top, ui.chop_zoom}, Binding {Kind::ChopZoom, 1, 0, zoom_top, ui.chop_zoom}, amount};
    }
    case PageId::Rack: case PageId::Bus: case PageId::Master: case PageId::PadFx: { // Shift + K: the device's other row (as Knob FX's Shift + K on the MPC)
        unsigned chain = page_chain(project, ui), slot = ui.fx_cursor[UiState::cursor_of(chain)];
        const auto& c = project.chain(chain);
        if (slot >= c.count() || ui.picker_open) return {};
        return effect_row(chain * 64 + slot * 8, c.slots[slot], 1 - ui.fx_row[UiState::cursor_of(chain)] % 2);
    }
    case PageId::Punch: // an MPC Pad FX in hand: Shift + K1-K3 are Melodee's depth, snap and tail
        if (ui.punch_selected < 0 || !punch_is_mpc(PunchKind(ui.punch_selected))) return {};
        return {Binding {Kind::PunchDepth, 0, 0, 100, project.punch_depth}, Binding {Kind::PunchSnap, 0, 0, 3, project.punch_snap},
                Binding {Kind::PunchTail, 0, 0, 2, project.punch_tail}};
    case PageId::Compressor: // Shift + K3: In Boost
        return {Binding {}, Binding {}, Binding {Kind::CompBoost, 0, 0, 24, project.compressor.boost}};
    case PageId::Stage: return stage_bindings(project, ui, pattern, true);
    case PageId::Pads: case PageId::Tempo: return project_key(project); // Shift + K1 / K2: the project's key, no synth track needed
    case PageId::TimeCorrect: { // Shift: humanize, its velocity, the groove's amount (a pad picks the groove)
        auto feel = feel_cards(pattern, 0), more = feel_cards(pattern, 1);
        return {feel[2], more[0], more[2]};
    }
    case PageId::Keys: // Shift + K3: the pads by semitones
        return {Binding {}, Binding {}, Binding {Kind::Transpose, 0, -12, 12, track.transpose}};
    default: return {};
    }
}

std::array<Binding, 3> page_bindings(const Project& project, const UiState& ui, const Pattern& pattern, bool shift) {
    // SD card access: the card is away, every mode waits (shell/card_access.h).
    if (ui.page == PageId::CardAccess || ui.card.state != 0) return {};
    // Knob FX on: the pots are its, on every page (as on the MPC), but for the Name sheet and Seq held.
    // A held key's cards win as well: Seq held (the pattern's), Pad Bank held (the track's strip), Sample held (the pad's).
    bool held_key = ui.seq_held || ui.pad_bank || ui.copying == UiState::Copying::Tracks || sample_held_page(project, ui) != PageId::Count
                 || ui.levels_sheet; // 16 Levels' SET: the chord's cards
    if (knob_owns(project, ui) && !(held_key && !ui.name.open())) return knob_bindings(project, shift);
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
    if (ui.seq_held) return pattern_cards(project, ui, pattern); // Seq held: the pattern's, on any page
    if (ui.pad_bank || ui.copying == UiState::Copying::Tracks) { // Pad Bank held, or a track move or copy picking its tracks:
        if (!track.used()) return {};                             // the selected track's strip, as the mixer's Tracks and Stage MIX
        return {Binding {Kind::Level, 0, 0, 127, track.mix.level}, Binding {Kind::Pan, 0, -50, 50, track.mix.pan},
                Binding {Kind::Send, 0, 0, 100, track.mix.sends[0]}};
    }
    if (auto pads = sample_held_page(project, ui); pads != PageId::Count) { // Sample held: its pads' (Pads, Keys)
        UiState on = ui; on.page = pads; on.sample_held = false;
        return base_bindings(project, on, pattern);
    }
    if (ui.levels_sheet == levels_sheet_chord) // 16 Levels' SET: the chord on the pad picked
        return {Binding {Kind::BuildRoot, 0, 0, 11, ui.chord_root}, Binding {Kind::BuildChord, 0, 0, int(built_chord_count) - 1, ui.chord_type},
                Binding {Kind::BuildInvert, 0, 0, int(built_inversions) - 1, ui.chord_invert}};
    if (ui.levels_sheet) return {}; // FILE's list, or SET waiting for its pad: no cards
    if (levels_held_cards(project, ui)) return levels_cards(project, track); // 16 Levels held: its settings, on any page
    if (ui.page == PageId::Looper) { // the Looper (docs/LOOPER.md): LOOPS, the loop tracks' MIX, or the loop's sheet
        unsigned l = ui.loop % loop_tracks;
        const auto& m = project.loop_mix[l];
        if (!project.looper.on) return {};
        if (ui.looper_view == 1 && !ui.loop_sheet)
            return {Binding {Kind::LoopLevel, int(l), 0, 127, m.level}, Binding {Kind::LoopPan, int(l), -50, 50, m.pan},
                    Binding {Kind::LoopSend, int(l), 0, 100, m.send}};
        unsigned at = 0;
        for (unsigned n = 0; n < looper::lengths.size(); ++n) if (looper::lengths[n] == project.looper.loops[l].bars) at = n;
        Binding length {Kind::LoopLength, int(l), 0, int(looper::lengths.size()) - 1, int(at)}, level {Kind::LoopLevel, int(l), 0, 127, m.level};
        if (ui.loop_sheet) { // the sheet: K1 the loop's source in place of the loop, which the wheel picks there
            std::array<uint8_t, looper::source_count> sources {};
            unsigned own = 0, count = loop_sources(project, l, sources, own);
            return {Binding {Kind::LoopSource, int(l), 0, int(count) - 1, int(own)}, length, level};
        }
        return {Binding {Kind::LoopPick, 0, 0, int(loop_tracks) - 1, int(l)}, length, level};
    }
    switch (ui.page) {
    case PageId::Keys:
        if (prog_pads(track)) // the progression's key and voicing (the wheel picks the progression)
            return {Binding {Kind::ProgKey, 0, 0, 11, int(prog_key(track))}, Binding {Kind::ProgVoicing, 0, 0, int(ProgVoicing::Count) - 1, track.prog_voicing},
                    Binding {Kind::Layout, 0, 0, int(PadLayout::Count) - 1, int(track.layout)}};
        return project_key(project, Binding {Kind::Layout, 0, 0, int(PadLayout::Count) - 1, int(track.layout)});
    case PageId::Sounds: { // every list: K1 jumps, the other pots draw nothing (the engine is Stage PRESET's K3)
        std::array<uint16_t, max_listed> list {};
        unsigned listed = sound_list(track, ui, list);
        return {Binding {Kind::Jump, 0, 0, int(std::max(listed, 1u)) - 1, std::min<int>(ui.sounds_cursor, int(listed) - 1)}, Binding {}, Binding {}};
    }
    case PageId::PluginPresets: return plugin_cards(ui, ui.presets_entry); // its row on K1-K3 while presets are heard
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
        if (track.engine == EngineKind::Midi) { // roadmap P4 (docs/MIDI_TRACKS.md M3): this pattern's program; the pads
            if (std::string(page.key) == "PADS") {
                Binding note {};
                if (drum_pads(track)) { unsigned pad = track.last_pad % sampler_pads; note = {Kind::MidiDrumNote, int32_t(pad), 0, 127, track.midi.drum_note[pad]}; }
                return {Binding {Kind::Layout, 0, 0, int(PadLayout::Count) - 1, int(track.layout)}, note, Binding {}};
            }
            if (std::string(page.key) == "SYSEX") // K1 the file (the card's SysEx folder), K2 on load; Push sends it
                return {Binding {Kind::MidiSysexFile, 0, 0, std::max<int>(ui.sysex_count, 1), midi_sysex_choice(ui, track)},
                        param(track, midi_track::SysexOnLoad), Binding {}};
            std::array<Binding, 3> out {param(track, row[0]), param(track, row[1]), param(track, row[2])};
            for (auto& b : out)
                if (b.kind == Kind::Param && b.id == midi_track::Port) // the devices by name
                    b = {Kind::MidiPort, 0, 0, int(midi_port_count(ui, track)) - 1, midi_port_index(ui, track)};
            for (auto& b : out) // the pattern in hand's own program (Pattern::program)
                if (b.kind == Kind::Param && b.id == midi_track::PatternProgram0) b = {Kind::PatternProgram, 0, 0, 128, pattern.program};
            return out;
        }
        return {param(track, row[0]), param(track, row[1]), param(track, row[2])};
    }
    case PageId::Motion: {
        auto lanes = motion_lanes(project, ui, pattern);
        unsigned t = ui.track % track_count;
        return {lane_binding(track, t, lanes[0]), lane_binding(track, t, lanes[1]), lane_binding(track, t, lanes[2])};
    }
    case PageId::Patterns: { // the pattern's cards; SEQUENCE: the sequence in hand's tempo and colour; FOLLOW: the pattern's follow action
        if (ui.sheet_open) return {Binding {Kind::VaryAmount, 0, 0, 100, ui.vary_amount}, Binding {}, Binding {}}; // the VARY sheet
        if (ui.patterns_cards == 1) {
            unsigned scene = project.scene % pattern_count; // the sequence in hand
            unsigned tempo = project.scenes[scene].tempo;
            return {Binding {Kind::SceneTempo, int32_t(scene), 29, 300, tempo ? int((tempo + 5) / 10) : 29},
                    Binding {Kind::SceneColour, int32_t(scene), 0, 16, project.scenes[scene].colour}, Binding {}};
        }
        if (ui.patterns_cards == 2) { // roadmap P5: the selected track's pattern's follow action, after how long, on what odds
            if (!track.used()) return {};
            const auto& f = pattern.follow;
            return {Binding {Kind::FollowAction, 0, 0, int(FollowAction::Count) - 1, f.action}, Binding {Kind::FollowAfter, 0, 0, 127, follow_after_card(f)},
                    Binding {Kind::FollowChance, 0, 0, 100, f.chance}};
        }
        return pattern_cards(project, ui, pattern);
    }
    case PageId::Scenes: { // roadmap P5: the selected track's pattern's follow action, after how long, on what odds
        if (!follow_shown(ui)) return pattern_cards(project, ui, pattern); // Basic: the pattern's length, Q and swing, as on Patterns
        if (!track.used()) return {};
        const auto& f = pattern.follow;
        return {Binding {Kind::FollowAction, 0, 0, int(FollowAction::Count) - 1, f.action}, Binding {Kind::FollowAfter, 0, 0, 127, follow_after_card(f)},
                Binding {Kind::FollowChance, 0, 0, 100, f.chance}};
    }
    case PageId::ModPage: { // roadmap P4: the modulator in hand, its rate (or a follower's rise), the target in hand's depth; a macro, its value, a target's high
        if (ui.mod_view == 1) {
            const auto& macro = project.macros[ui.macro_index % macro_count];
            const auto& g = macro.targets[ui.macro_target % macro_targets];
            Binding high {};
            if (g.used())
                if (auto m = motion_param(project.tracks[g.track % track_count], motion_lane(g.param, g.pad), &project); m.info)
                    high = {Kind::MacroHigh, 0, m.info->min, m.info->max, g.high};
            return {Binding {Kind::MacroPick, 0, 0, int(macro_count) - 1, ui.macro_index % int(macro_count)}, Binding {Kind::MacroValue, 0, 0, 127, macro.value}, high};
        }
        const auto& m = project.modulators[ui.mod_index % modulator_count];
        Binding pick {Kind::ModPick, 0, 0, int(modulator_count) - 1, ui.mod_index % int(modulator_count)};
        Binding rate {};
        if (m.kind == ModKind::Follower) rate = {Kind::ModAttack, 0, 0, 100, m.attack};
        else if (m.kind == ModKind::Lfo) rate = m.sync ? Binding {Kind::ModRate, 0, 0, int(lfo_divisions) - 1, m.division} : Binding {Kind::ModRate, 0, 0, 100, m.rate};
        const auto& g = m.targets[ui.mod_target % modulator_targets];
        Binding depth = g.used() ? Binding {Kind::ModDepth, 0, -100, 100, g.depth} : Binding {};
        return {pick, rate, depth};
    }
    case PageId::StepEdit: { // B1's card sets on the caret's step; on a kit (or Drums) its lane's note, the last pad's
        unsigned at = unsigned(std::max<int>(ui.step_cursor, 0)) % pattern.steps();
        const unsigned set = step_set(track, ui.step_cards);
        if (sounds_open(ui)) return sound_cards(project, track, pattern, sound_step(ui, pattern)); // the sounds on the pads: the step's
        if (ui.held_step >= 0) { // STEPS, held: its SOUND row on SOUND, else locks
            if (set == cards_sound) return sound_cards(project, track, pattern, unsigned(ui.held_step) % pattern.steps());
            return lock_cards(project, ui, pattern, unsigned(ui.held_step) % pattern.steps() * pattern.q());
        }
        int lane = steps_lane(track);
        bool spare = in_spare(ui, pattern); // past the end (shell/spare_bar.h): no step's cards, only what a pad writes
        switch (set) {
        case cards_lock: if (spare) return {}; return lock_cards(project, ui, pattern, at * pattern.q()); // LOCK: the Motion lanes locked on the caret's step
        case cards_sound: if (spare) return {}; return sound_cards(project, track, pattern, at); // SOUND: its sample, slice, way
        case cards_trig: { // TRIG: its ratchet, ramp and condition
            if (spare) return {};
            int n = lane_event(pattern, at, lane);
            if (n < 0) return {};
            return trig_cards(pattern.events[size_t(n)], true);
        }
        case cards_euclid: { // EUCLID: the last pad's lane's rhythm (roadmap P1)
            unsigned key = unsigned(lane);
            const EuclidLane* e = euclid_of(pattern, key);
            int steps = e ? e->steps : int(std::min(pattern.steps(), euclid_max_steps));
            return {Binding {Kind::EuclidHits, int(key), 0, steps, e ? e->hits : 0}, Binding {Kind::EuclidSteps, int(key), 1, int(euclid_max_steps), steps},
                    Binding {Kind::EuclidRotate, int(key), 0, std::max(steps - 1, 1), e ? e->rotate : 0}};
        }
        default: break;
        }
        if (ui.step_pads) { // NOTE with the pads on the steps: the caret's step, as the pads show it
            if (spare) return {};
            int n = lane_event(pattern, at, lane);
            if (n < 0) return {};
            const auto& e = pattern.events[size_t(n)];
            unsigned q = pattern.q(), length = std::clamp<unsigned>((e.length + q / 2) / q, 1, 16);
            return {Binding {Kind::StepLength, 0, 1, 16, int(length)}, Binding {Kind::StepVelocity, 0, 1, 127, e.velocity},
                    Binding {Kind::StepChance, 0, 0, 100, e.chance}};
        }
        // NOTE, step entry: what a pad writes, its length in steps of Q, velocity (0 as played), chance
        return {Binding {Kind::EntryLength, 0, 1, 16, ui.entry_length}, Binding {Kind::EntryVelocity, 0, 0, 127, ui.entry_velocity},
                Binding {Kind::EntryChance, 0, 0, 100, ui.entry_chance}};
    }
    case PageId::TimeCorrect: { // time-correct C: Q (the pattern's), and the strength and swing heard live (its feel)
        auto feel = feel_cards(pattern, 0);
        return {Binding {Kind::Division, 0, 0, int(Division::Count) - 1, int(pattern.division)}, feel[0], feel[1]};
    }
    case PageId::AutoSample: { // the AUTO SAMPLE sheet: its row of three; nothing while it runs
        const auto& sheet = ui.auto_sheet;
        const auto& a = sheet.settings;
        if (sheet.sampling || sheet.checking) return {};
        switch (sheet.row % 3) {
        case 0: // drums: every key that sounds, no STEP
            if (a.found.kind == AutoKind::Drums) return {Binding {Kind::AutoLow, 0, 0, 127, a.low}, Binding {Kind::AutoHigh, 0, 0, 127, a.high}, Binding {}};
            return {Binding {Kind::AutoLow, 0, 0, 127, a.low}, Binding {Kind::AutoHigh, 0, 0, 127, a.high}, Binding {Kind::AutoStep, 0, 1, 24, a.step}};
        case 1: return {Binding {Kind::AutoLayers, 0, 0, 8, a.layers}, Binding {Kind::AutoHold, 0, 0, 200, a.hold_auto ? 0 : int(std::lround(a.hold * 10))},
                        Binding {Kind::AutoTail, 0, 0, 200, int(std::lround(a.tail * 10))}};
        default: { // a MIDI track: its INPUT (the Audio page's) where a plug-in's TAP is
            const auto& from = project.tracks[sheet.track % track_count];
            const Binding first = from.engine == EngineKind::Midi
                ? Binding {Kind::Param, int16_t(midi_track::AudioIn), 0, midi_track::AudioCount - 1, from.params[midi_track::AudioIn]}
                : Binding {Kind::AutoTap, 0, 0, 1, int(a.post_fx)};
            return {first, Binding {Kind::AutoLoop, 0, 0, 2, a.loop_auto ? 0 : a.loop ? 1 : 2},
                    Binding {Kind::AutoLevel, 0, 0, 1, int(a.normalize)}};
        }
        }
    }
    case PageId::Stems: return {}; // the STEMS sheet: no cards
    case PageId::Print: { // freeze plan F2c: K1 TAP, K2 TAIL; K3 empty (no Song lane print yet); still while printing
        const auto& sheet = ui.print_sheet;
        if (sheet.printing || project.tracks[sheet.track % track_count].engine == EngineKind::Midi) return {};
        return {Binding {Kind::PrintTapPick, 0, 0, 1, int(sheet.tap)}, Binding {Kind::PrintTailPick, 0, 0, 2, int(sheet.tail)}, Binding {}};
    }
    case PageId::Song: {
        if (ui.jam_sheet) // JAMS: the part of the picked jam, FROM and TO (bars from 1)
            return {Binding {Kind::JamFrom, 0, 1, std::max<int>(ui.jam_pick_bars, 2), ui.jam_from + 1},
                    Binding {Kind::JamTo, 0, 1, std::max<int>(ui.jam_pick_bars, 2), ui.jam_to}, Binding {}};
        int clip = project.song.clip_at(ui.track, ui.song_cursor);
        int pattern_value = clip >= 0 ? project.song.clips[size_t(clip)].pattern : ui.song_pattern;
        int length_value = clip >= 0 ? project.song.clips[size_t(clip)].bars : ui.song_length;
        return {Binding {Kind::SongZoom, 0, 0, int(sizeof(song_zooms) / sizeof(song_zooms[0])) - 1, ui.song_zoom},
                Binding {Kind::SongPattern, int16_t(clip), 0, int(pattern_count) - 1, pattern_value},
                Binding {Kind::SongLength, int16_t(clip), 1, int(max_bars), length_value}};
    }
    case PageId::Rack:
    case PageId::Bus:
    case PageId::Master:
    case PageId::PadFx: {
        unsigned chain = page_chain(project, ui), slot = ui.fx_cursor[UiState::cursor_of(chain)];
        const auto& c = project.chain(chain);
        if (slot >= c.count() || ui.picker_open) return {};
        if (c.slots[slot].kind == plugin_effect_kind) return plugin_cards(ui, unsigned(c.slots[slot].params[0]));
        return effect_row(chain * 64 + slot * 8, c.slots[slot], ui.fx_row[UiState::cursor_of(chain)]); // a keyed kind's KEY last (roadmap P4)
    }
    case PageId::Pads: {
        if (track.engine != EngineKind::Sampler) return favourite_bindings(track); // a synth: its own three
        const auto& pad = track.pads[track.last_pad % sampler_pads];
        if (!pad.used()) return {};
        return {Binding {Kind::PadPitch, 0, -24, 24, pad.pitch}, Binding {Kind::PadLevel, 0, 0, 127, pad.level},
                Binding {Kind::PadPan, 0, -50, 50, pad.pan}};
    }
    case PageId::Trim: // K1 start, K2 end, K3 the loop point (Shift: zoom at each)
        return pad_page_bindings(project, track, ui, ui.page, false);
    case PageId::Chop: { // K1 the slice's start, K2 its end (the neighbours' end and start with them), K3 the type
        uint32_t frames = ui.chop_sample < sample_slots ? project.samples[ui.chop_sample].frames : 0;
        Binding type {Kind::ChopMode, 0, 0, int(ChopMode::Count) - 1, int(ui.chop_mode)};
        unsigned n = ui.chop_slice;
        if (!frames || n >= ui.slices.count) return {Binding {}, Binding {}, type};
        const auto& sl = ui.slices; // in frames; zoomed in, each pot turns across the view on screen
        uint32_t start = sl.start[n], end = sl.end(n, frames);
        // A neighbour touching the edge moves with it (keeping 20 ms); across a gap the edge stops at the neighbour.
        uint32_t start_lo = !n ? 0 : sl.touches(n - 1, frames) ? sl.start[n - 1] + min_slice : sl.end(n - 1, frames);
        uint32_t start_hi = end > min_slice ? end - min_slice : 0;
        uint32_t end_hi = sl.touches(n, frames) ? sl.end(n + 1, frames) - min_slice : n + 1 < sl.count ? sl.start[n + 1] : frames;
        FrameView view = ui.chop_zoom ? chop_view(project, ui) : FrameView {};
        return {handle_card(Kind::ChopStart, int(n), start, start_lo, start_hi, view),
                handle_card(Kind::ChopEnd, int(n), end, start + min_slice, end_hi, view), type};
    }
    case PageId::Record: case PageId::InputConfig: { // as on the MPC's Sample Record: K1 source, K2 rec length, K3 threshold
        const auto& in = ui.settings.input;
        auto choices = input_choices(project, ui.track, in);
        return {Binding {Kind::RecSource, 0, 0, int(choices.count) - 1, int(choices.index_of(input_code(in)))},
                Binding {Kind::RecLength, 0, 0, int(rec_lengths) - 1, in.length},
                Binding {Kind::RecThreshold, 0, InputConfig::threshold_min, 0, in.threshold}};
    }
    case PageId::Compressor: { // as on the MPC: K1 attack, K2 release, K3 amount
        const auto& c = project.compressor;
        return {Binding {Kind::CompAttack, 0, 0, 100, c.attack}, Binding {Kind::CompRelease, 0, 0, 100, c.release},
                Binding {Kind::CompAmount, 0, 0, 100, c.amount}};
    }
    case PageId::Project: // the list: K1 jumps, K2 sorts; the tiles and the sheets have no pots
        if (!ui.project_list || ui.sheet_open || ui.move_open) return {};
        return {Binding {Kind::ProjectJump, 0, 0, std::max(int(ui.project_rows) - 1, 1), ui.project_cursor},
                Binding {Kind::ProjectSort, 0, 0, 1, ui.project_sort}, Binding {}};
    case PageId::Tempo:
        return {Binding {Kind::Tempo, 0, 30, 300, int(project.tempo_of(ui.lead_pattern) / 10)}, Binding {Kind::Swing, 0, 50, 75, project.swing},
                Binding {Kind::ClickLevel, 0, 0, 100, ui.settings.click_level}};
    case PageId::PadMix:
    case PageId::LayersPage:
    case PageId::PadPage:
    case PageId::PlayPage:
    case PageId::EnvelopePage:
    case PageId::PadFilterPage:
    case PageId::FilterEnvPage:
    case PageId::FilterPage:
    case PageId::FollowPage:
    case PageId::CloudPage:
        return pad_page_bindings(project, track, ui, ui.page, ui.sampler_row != 0);
    case PageId::Punch: { // the MPC's Pad FX pressed last: its K1-K3 (as on the MPC); else depth, snap, tail
        if (ui.punch_selected >= 0 && punch_is_mpc(PunchKind(ui.punch_selected))) {
            unsigned mpc = unsigned(ui.punch_selected) - punch_mpc_first;
            std::array<Binding, 3> out;
            for (unsigned n = 0; n < punch_params; ++n) {
                const auto& pi = mpc_pad_fx(mpc).params[n];
                if (pi.label) out[n] = {Kind::PadFxParam, int32_t(mpc * 4 + n), pi.min, pi.max, project.pad_fx_params[mpc][n]};
            }
            return out;
        }
        return {Binding {Kind::PunchDepth, 0, 0, 100, project.punch_depth}, Binding {Kind::PunchSnap, 0, 0, 3, project.punch_snap},
                Binding {Kind::PunchTail, 0, 0, 2, project.punch_tail}};
    }
    case PageId::FlexBeat: // K1 One Shot / Loop, K3 mix (as on the MPC)
        return {Binding {Kind::FlexMode, 0, 0, 1, project.flex.mode}, Binding {}, Binding {Kind::FlexMix, 0, 0, 100, project.flex.mix}};
    case PageId::Samples: { // every list: K1 jumps; on an audio file K2 START, K3 END (the trim sheet, shell/browse_trim.h)
        const Binding jump {Kind::FileJump, 0, 0, std::max(int(ui.files_count) - 1, 1), ui.files_cursor};
        if (ui.sheet_open || !(ui.browse.up() || ui.files_audio)) return {jump, Binding {}, Binding {}};
        const auto trim = browse_cards(project, ui.browse);
        return {jump, trim[0], trim[1]};
    }
    case PageId::Plugins:
        return {Binding {Kind::PluginJump, 0, 0, std::max(int(ui.plugins_count) - 1, 1), ui.plugins_cursor}, Binding {}, Binding {}};
    case PageId::Roll: { // the note in focus: K1 its length in steps of Q, K2 velocity, K3 chance; none, no cards
        int at = roll_focus(pattern, ui, roll_lane(track));
        const bool marks = roll_selected(ui, track.pattern) && ui.held_note < 0;
        if (at < 0 && marks) { // marked notes: K1 and K3 set them all, read off the first
            auto marked = roll_marked_events(pattern, ui);
            if (!marked.empty()) at = int(marked.front());
        }
        if (at < 0) return {};
        const auto& e = pattern.events[size_t(at)];
        if (ui.held_note >= 0) return lock_cards(project, ui, pattern, e.tick); // its pad held: locks there (roadmap P1)
        if (ui.trig_cards) return trig_cards(e, false); // B2 again: its ratchet and ramp
        unsigned q = pattern.q(), steps = std::max(1u, (e.length + q / 2) / q);
        Binding velocity {Kind::EventVelocity, 0, 1, 127, e.velocity};
        int low = 0, high = 0; // marked notes: K2 moves every velocity by as much, the card their lowest
        if (marks && roll_marks_span(pattern, ui, low, high)) velocity = Binding {Kind::MarksVelocity, 0, 1, 127 - (high - low), low};
        return {Binding {Kind::EventLength, 0, 1, 64, int(std::min(steps, 64u))}, velocity, Binding {Kind::EventChance, 0, 0, 100, e.chance}};
    }
    case PageId::Stage: return stage_bindings(project, ui, pattern, false);
    // System pages: their rows take the wheel; no pots, so no cards (never the synth's favourites there). The Fader
    // page keeps the favourites (fader C): a pot turned there is what the fader learns.
    case PageId::SettingsPage: { // on the doors K1 is FEATURES, Basic or Full (shell/features.h); behind a door the row under the cursor
        if (!ui.settings_in)
            return {Binding {Kind::Features, 0, 0, 1, int(ui.settings.full_features)}, Binding {Kind::UsageLog, 0, 0, 1, int(ui.settings.usage_log)}, Binding {}};
        if (ui.settings_look) return {}; // Palette or Font's cards: the wheel
        unsigned row = ui.settings_cursor, at = ui.settings_wait_row == int(row) ? ui.settings_wait_value : ui.settings.choice(row);
        return {Binding {Kind::SettingRow, int16_t(row), 0, int(ui.settings.choices(row)) - 1, int(at)}, Binding {}, Binding {}};
    }
    case PageId::MidiConfig: { // K1 the row under the cursor; MIDI Panic and Reset have none (Push)
        unsigned row = ui.midi_cursor, count = midi_row_choices(row);
        if (!count) return {};
        return {Binding {Kind::MidiRow, int16_t(row), 0, int(count) - 1, int(midi_row_choice(ui.settings, track, row))}, Binding {}, Binding {}};
    }
    case PageId::System: return {};
    case PageId::Tracks: // the strip in hand, as Stage's MIX: K1 its level, K2 its pan, K3 its send into bus A (pads and the wheel pick the track)
        return {Binding {Kind::Level, 0, 0, 127, track.mix.level}, Binding {Kind::Pan, 0, -50, 50, track.mix.pan},
                Binding {Kind::Send, 0, 0, 100, track.mix.sends[0]}};
    case PageId::Mixer: return mixer_cards(project, ui, false);
    default: return favourite_bindings(track);
    }
}

} // namespace melodee
