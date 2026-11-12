// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The pads' lights, a function a context (shell/pad_lights.h). Each says what
// a press does there, in the grammar of shell/pad_light.h: the track's colour
// for its own pads, white for the panel's jobs and the one in your hand, red
// for a loss or a recording; Faint, Dim, Mid and Full for how much; a blink
// for a wait, a flare for a sound, a pulse for the count.
#include "melodee/shell/pad_lights.h"
#include "melodee/engine/engine_host.h"
#include "melodee/engine/pad_fx_info.h"
#include "melodee/engine/voice_tap.h"
#include "melodee/model/chords.h"
#include "melodee/model/pad_params.h"
#include "melodee/model/scales.h"
#include "melodee/shell/looper_jobs.h"
#include "melodee/shell/mute_mode.h"
#include "melodee/shell/roll.h"
#include "melodee/shell/track_copy.h"
#include "melodee/shell/shift_pad_jobs.h"
#include "melodee/shell/step_edit.h"
#include "melodee/shell/step_lane.h"
#include <algorithm>
#include <cmath>

namespace melodee {
using namespace pad_light;

namespace {

const Track& selected(const PadScene& s) { return s.project.tracks[s.ui.track % track_count]; }
unsigned selected_index(const PadScene& s) { return s.ui.track % track_count; }
// The pattern the pages edit (Context::pattern()).
const Pattern& shown(const PadScene& s) { return s.patterns.at[selected_index(s)][selected(s).pattern % pattern_count]; }
unsigned kit_of(const Track& t, unsigned pad) { return t.bank % sampler_banks * 16 + pad % 16; }
bool held(const PadScene& s, unsigned pad) { return s.ui.held_pads & (1u << pad); }
// Notes go into the pattern now: Seq Record, past its count-in.
bool writing(const PadScene& s) { return s.ui.recording && s.host.playing() && !s.host.count_in(); }
// A track's meter, 0 at -48 dB up to 1 at 0 dB, as the Tracks page draws it.
float meter(const PadScene& s, unsigned t) {
    float m = s.host.meter(t);
    return m > 0 ? std::clamp((20 * std::log10(m) + 48) / 48, 0.0f, 1.0f) : 0;
}
// 16 Levels plays the pads (Context::play_pad()).
bool levels_on(const Track& t) {
    return t.levels || (t.engine != EngineKind::Sampler && t.layout == PadLayout::Levels);
}
bool any_solo(const Project& p) {
    for (const auto& t : p.tracks) if (t.used() && t.mix.solo) return true;
    return false;
}
// The keys holding notes in a pattern.
std::array<bool, 192> keys_in(const Pattern& p) {
    std::array<bool, 192> out {};
    for (const auto& e : p.events) if (e.tick < p.length) if (e.key < 192) out[e.key] = true;
    return out;
}

// What sounds now, pad by pad: a voice's envelope (a kit), or the pattern at
// the playhead (plug-in and MIDI tracks have no voice taps); and a pad let
// go, fading (hand_ms).
struct Heard {
    std::array<float, 16> level {};
    std::array<float, 16> hand {};
};
// A pattern's note at `pos`: its velocity while held, fading over a sixteenth after (round the loop).
float note_now(const Event& e, uint32_t pos, uint32_t length) {
    uint32_t since = pos >= e.tick ? pos - e.tick : pos + length - e.tick;
    uint32_t gate = std::max<uint32_t>(e.length, 1);
    float v = e.velocity / 127.0f;
    if (since < gate) return v;
    uint32_t after = since - gate;
    return after < sixteenth_ticks ? v * (1 - float(after) / sixteenth_ticks) : 0;
}
Heard heard_now(const PadScene& s) {
    Heard out;
    const auto& t = selected(s);
    unsigned at = selected_index(s);
    if (t.engine == EngineKind::Sampler) {
        unsigned first = t.bank % sampler_banks * 16;
        if (s.taps.track == at)
            for (unsigned n = 0; n < s.taps.count && n < TrackTaps::voices; ++n) {
                const auto& v = s.taps.voice[n];
                if (v.pad < int(first) || v.pad >= int(first + 16) || (v.flags & tap::Choked)) continue;
                auto& level = out.level[unsigned(v.pad) - first];
                level = std::max(level, v.level);
            }
    } else if (t.used() && s.host.playing() && s.host.position_of(at) >= 0) {
        const auto& p = s.patterns.at[at][s.host.pattern_of(at) % pattern_count];
        if (p.length) {
            std::array<int, 16> keys {};
            for (unsigned pad = 0; pad < 16; ++pad) keys[pad] = pad_key(s.project, t, pad);
            uint32_t pos = uint32_t(s.host.position_of(at)) % p.length;
            for (const auto& e : p.events) {
                if (e.tick >= p.length) break;
                float v = note_now(e, pos, p.length);
                if (v <= 0) continue;
                for (unsigned pad = 0; pad < 16; ++pad)
                    if (keys[pad] == int(e.key)) out.level[pad] = std::max(out.level[pad], v);
            }
        }
    }
    if (s.let_go)
        for (unsigned pad = 0; pad < 16; ++pad) {
            uint32_t up = (*s.let_go)[pad];
            if (!up || held(s, pad) || s.now + 1 < up) continue;
            uint32_t since = s.now + 1 - up;
            if (since < 4 * hand_ms) out.hand[pad] = std::exp(-float(since) / hand_ms);
        }
    return out;
}
// A pad that plays: held, white (red while it writes into the pattern); else its look, flaring as it sounds.
Look playing(const PadScene& s, const Heard& snd, const Look& base, unsigned pad) {
    if (held(s, pad)) return writing(s) ? red(full) : white(full);
    if (writing(s)) return red_flare(flare(base, snd.level[pad]), snd.hand[pad]); // a hit just written
    return flare(base, std::max(snd.level[pad], snd.hand[pad]));
}
// Count-In: every pad pulses white on its beats, the bar's first brighter.
Look counting(const PadScene& s, const Look& look) {
    unsigned beat = s.host.count_in();
    if (!beat || !s.count_beat_at || s.now < s.count_beat_at) return look;
    auto sig = s.project.time_signature();
    float bpm = std::max(s.host.tempo_now() / 10.0f, 20.0f);
    float beat_ms = 60000.0f / bpm * 4.0f / float(std::max<unsigned>(sig.unit, 1));
    float since = float(s.now - s.count_beat_at) / beat_ms;
    if (since >= 1) return look;
    bool first = (beat - 1) % std::max<unsigned>(sig.beats, 1) == 0;
    float level = (first ? mid : faint) * std::pow(1 - since, 3.0f);
    return level > 0.01f ? brighter(look, white(level)) : look;
}

// ---- the track's own pads: a kit, a synth or MIDI track, 16 Levels
Look kit_base(const PadScene& s, unsigned pad) {
    const auto& t = selected(s);
    unsigned kit = kit_of(t, pad);
    bool used = t.pads[kit].used();
    if (used && t.pad_muted(kit)) return red(faint); // a muted pad says so on every page
    if (kit == t.last_pad) return track(selected_index(s), full); // the pad selected, empty too: what the pages edit and load into
    return used ? track(selected_index(s), dim) : Look {};
}
// Where `key` sits in the track's key, 0 its root: the project's root moved by the track's transpose, as its Scale pads are.
unsigned key_degree(const Project& project, const Track& t, int key) {
    return unsigned(((key - int(project.root) - t.transpose) % 12 + 12) % 12);
}
bool in_key(const Project& project, unsigned degree) { return (scales()[project.scale % scales().size()].mask >> degree) & 1; }
// A pad's note by its place in the key (Keys and 16 Levels alike): the key's roots in track `at`'s colour, its other
// notes white, out of the key white Faint.
Look note_look(const Project& project, unsigned at, unsigned degree) {
    if (!degree) return track(at, mid);
    return white(in_key(project, degree) ? dim : faint);
}
Look keys_base(const PadScene& s, unsigned pad) {
    const auto& t = selected(s);
    if (!t.used()) return {};
    int key = pad_note(s.project, t, pad).key;
    if (drum_pads(t)) { // Drums pads: lit as a kit's, the drum hit last Full; a pad the plug-in has no voice on, dark
        if (s.ui.drum_voices.any() && !s.ui.drum_voices.at(key)) return {};
        return track(selected_index(s), key == int(t.last_note) ? full : dim);
    }
    if (t.layout == PadLayout::Chords) // a held pad's chord: its other notes white Full, as the pad held
        for (unsigned h = 0; h < 16; ++h) {
            if (h == pad || !held(s, h)) continue;
            std::array<int, notes_per_step> keys {};
            unsigned count = chord_notes(s.project, t, h, keys);
            for (unsigned n = 0; n < count; ++n) if (keys[n] == key) return white(full);
        }
    return note_look(s.project, selected_index(s), key_degree(s.project, t, key));
}
// 16 Levels: what each pad plays of the one sound, from the lowest pad's to the highest's.
Look levels_base(const PadScene& s, unsigned pad) {
    const auto& t = selected(s);
    if (!t.used()) return {};
    unsigned at = selected_index(s);
    auto kind = LevelsKind(t.levels_kind % unsigned(LevelsKind::Count));
    if (!t.levels || (kind == LevelsKind::Filter && t.engine != EngineKind::Sampler)) kind = LevelsKind::Velocity;
    auto lv = levels_of(s.project, t, pad);
    if (kind == LevelsKind::Tune || kind == LevelsKind::Scale) { // notes: the key's roots in the track's colour, its other notes white
        // Scale goes up the key from its root on pad 1. Tune: a synth's pad plays its last note moved; a kit's sample
        // stands for the key's root as recorded.
        unsigned degree = kind == LevelsKind::Scale ? unsigned(level_scale_step(s.project, pad) % 12)
                        : t.engine == EngineKind::Sampler ? unsigned((lv.semitones % 12 + 12) % 12)
                                                          : key_degree(s.project, t, std::clamp(int(t.last_note) + lv.semitones, 0, 127));
        return note_look(s.project, at, degree);
    }
    int low = lv.shown, high = lv.shown; // Velocity, Filter: soft to loud, closed to open
    for (unsigned n = 0; n < 16; ++n) { int v = levels_of(s.project, t, n).shown; low = std::min(low, v); high = std::max(high, v); }
    return track(at, high > low ? between(faint, full, float(lv.shown - low) / float(high - low)) : dim);
}
// 16 Levels on a kit: how strongly the one pad's voice sounds.
float levels_voice(const PadScene& s) {
    const auto& t = selected(s);
    if (t.engine != EngineKind::Sampler || s.taps.track != selected_index(s)) return 0;
    float level = 0;
    for (unsigned n = 0; n < s.taps.count && n < TrackTaps::voices; ++n)
        if (s.taps.voice[n].pad == int(t.levels_pad) && !(s.taps.voice[n].flags & tap::Choked)) level = std::max(level, s.taps.voice[n].level);
    return level;
}
Look levels_look(const PadScene& s, const Heard& snd, unsigned pad) {
    Look base = levels_base(s, pad);
    if (held(s, pad)) return writing(s) ? red(full) : white(full);
    if (int(pad) != s.ui.levels_hit) return base;
    float sound = std::max(levels_voice(s), snd.hand[pad]); // the pad hit last, white as its sound fades
    return writing(s) ? red_flare(base, sound) : blend(base, white(full), sound);
}
Look play_look(const PadScene& s, const Heard& snd, unsigned pad) {
    const auto& t = selected(s);
    if (levels_on(t)) return counting(s, levels_look(s, snd, pad));
    Look base = t.engine == EngineKind::Sampler ? kit_base(s, pad) : keys_base(s, pad);
    return counting(s, playing(s, snd, base, pad));
}

// ---- pads as the sixteen tracks
// A track's next pattern waits for the bar.
bool queued(const PadScene& s, unsigned t) { return s.host.playing() && s.project.tracks[t].pattern != s.host.pattern_of(t); }
Look tracks_look(const PadScene& s, unsigned pad) {
    const auto& ui = s.ui;
    const auto& t = s.project.tracks[pad];
    if (ui.copying == UiState::Copying::Tracks && ui.copy_from >= 0) { // where the copy goes, or the move
        if (ui.copy_from == int(pad)) return white(full);
        if (ui.moving) return track(pad, t.used() ? dim : faint); // a track there trades places: nothing lost, no red
        return t.used() ? red(faint) : track(pad, faint);
    }
    if (ui.erase) return track_clearable(t, s.patterns.at[pad]) ? red(faint) : Look {}; // Erase held: the tracks Erase + pad would clear
    if (!t.used()) return {};
    if (t.mix.mute) return red(faint);
    if (queued(s, pad)) return blink(s.now, blink_ms) ? track(pad, full) : track(pad, faint);
    if (pad == selected_index(s)) return track(pad, full);
    if (any_solo(s.project) && !t.mix.solo) return track(pad, faint); // silenced by a solo
    float m = meter(s, pad);
    return t.mix.solo ? white(between(dim, mid, m)) : track(pad, between(dim, mid, m));
}
Look song_look(const PadScene& s, unsigned pad) {
    const auto& t = s.project.tracks[pad];
    if (!t.used()) return {};
    if (t.mix.mute) return red(faint);
    if (pad == selected_index(s)) return track(pad, full);
    const auto& song = s.project.song;
    if (song.clip_at(pad, s.ui.song_cursor) >= 0) return flare(track(pad, mid), s.host.playing() ? 0.6f * meter(s, pad) : 0);
    for (unsigned n = 0; n < song.count; ++n)
        if (song.clips[n].track == pad) return track(pad, dim);
    return track(pad, faint);
}

// ---- SCENES (roadmap P5): a pad a scene, the column of patterns it launches. The one playing (every track with notes
// there playing it) bright, one waiting for its bar blinking, the rest as many tracks as hold notes there; Erase held:
// the scenes with a name or tempo to clear.
Look scenes_look(const PadScene& s, unsigned pad) {
    unsigned n = s.ui.pad_pattern(pad), with = 0, on = 0, waiting = 0; // the bank in view's
    bool rolling = s.host.playing();
    for (unsigned t = 0; t < track_count; ++t) {
        const auto& tr = s.project.tracks[t];
        if (!tr.used() || s.patterns.at[t][n].empty()) continue;
        ++with;
        bool live = rolling ? s.host.pattern_of(t) == n && !(s.host.stopped_tracks() >> t & 1) : tr.pattern == n;
        on += live;
        waiting += rolling && !live && tr.pattern == n;
    }
    if (s.ui.erase) return s.project.scenes[n].plain() ? Look {} : red(dim);
    if (with && on == with) return white(full);
    if (waiting) return (s.now / 150) % 2 ? white(mid) : white(faint);
    if (!with) return s.project.scenes[n].plain() ? Look {} : white(faint);
    return with == 1 ? white(faint) : with < 4 ? white(dim) : white(mid);
}

// ---- the Looper's grid (docs/LOOPER.md): a column a loop, pad 1 bottom left
Look loop_look(const PadScene& s, unsigned pad, bool muting) {
    using S = Looper::State;
    const auto& lp = s.host.looper();
    unsigned loop = pad % 4, row = pad / 4 % 4;
    if (!lp.enabled_now()) return {};
    auto v = lp.view(loop);
    const auto& m = s.project.loop_mix[loop];
    if (muting) return m.mute ? red(dim) : m.solo ? white(mid) : pad_light::loop(loop, mid); // each column its loop track
    bool on = blink(s.now, 200), audio = v.length > 0;
    switch (row) {
    case 3: // record / overdub / play
        if (v.state == S::Recording) return red(full);
        if (v.state == S::Armed) return on ? red(full) : red(faint);
        if (v.state == S::Overdub) return orange(full);
        if (v.fitting) return pad_light::loop(loop, on ? full : faint);
        return pad_light::loop(loop, audio ? dim : faint);
    case 2: { // play or stop on the next boundary
        if (!audio) return {};
        if (v.pending != Looper::Pending::None) return pad_light::loop(loop, on ? full : faint);
        if (v.state == S::Playing || v.state == S::Overdub) {
            double tick = std::max(lp.beat_tick(), 0.0);
            unsigned bar = s.project.bar_ticks(), beat = s.project.beat_ticks();
            bool rolling = s.host.playing();
            if (rolling && std::fmod(tick, double(bar)) < beat / 4.0) return white(full); // the bar
            return pad_light::loop(loop, rolling && std::fmod(tick, double(beat)) < beat / 4.0 ? full : mid);
        }
        return pad_light::loop(loop, faint);
    }
    case 1: // undo, then redo
        return v.undo == looper::Undo::Undo ? white(mid) : v.undo == looper::Undo::Redo ? white(faint) : Look {};
    default: // hold to clear: fills red
        if (!audio && v.state == S::Empty) return {};
        if (s.ui.loop_clear == int(loop)) return red(between(faint, full, float(s.now - s.ui.loop_clear_at) / loop_clear_ms));
        return red(faint);
    }
}

// ---- steps, notes and patterns
Look seq_look(const PadScene& s, unsigned pad) {
    const auto& ui = s.ui;
    unsigned at = selected_index(s), n = ui.pad_pattern(pad); // the pattern under the pad, in the bank in view
    const auto& t = s.project.tracks[at];
    bool notes = !s.patterns.at[at][n].empty();
    bool rolling = s.host.playing(), live = s.host.pattern_of(at) == n;
    bool on = rolling ? live : t.pattern == n;
    if (ui.copying == UiState::Copying::Patterns) {
        if (ui.copy_from < 0) return notes ? track(at, dim) : Look {}; // the pattern to copy
        if (ui.copy_track == at && ui.copy_from == int(n)) return white(full);
        return notes ? red(faint) : track(at, faint);                // where: replaced, or landing clean
    }
    if (ui.erase) return !notes ? Look {} : on ? red(mid) : red(dim);   // clears, asked first
    if (ui.shift) {                                                     // a column: the tracks holding notes there
        if (on) return white(full);
        unsigned with = 0;
        for (unsigned k = 0; k < track_count; ++k) with += s.project.tracks[k].used() && !s.patterns.at[k][n].empty();
        return with == 0 ? Look {} : with == 1 ? white(faint) : with < 4 ? white(dim) : white(mid);
    }
    if (ui.pattern_held == int(n)) return white(full); // tap another to copy it there
    if (on) return track(at, full);
    if (rolling && t.pattern == n) return blink(s.now, blink_ms) ? track(at, full) : track(at, faint); // waits for the bar
    return notes ? track(at, dim) : Look {};
}
Look velocity_look(unsigned t, unsigned velocity) {
    return !velocity ? track(t, faint) : track(t, velocity >= 120 ? full : velocity >= 90 ? mid : dim);
}
Look steps_look(const PadScene& s, unsigned pad) {
    const auto& ui = s.ui;
    const auto& p = shown(s);
    unsigned step = unsigned(ui.steps_bar) * bar_steps + pad;
    int lane = steps_lane(selected(s));
    uint32_t loop = lane >= 0 ? p.lane_length(unsigned(lane)) : p.length; // a kit lane of its own length (roadmap P1)
    if (step >= p.steps() || step * p.q() >= loop) return {}; // past the pattern's (or the lane's) end
    if (int(step) == ui.held_step) return white(full);
    int position = s.host.position_of(selected_index(s));
    uint64_t elapsed = uint64_t(s.host.pass_of(selected_index(s))) * std::max<uint32_t>(p.length, 1) + uint32_t(std::max(position, 0));
    if (s.host.playing() && position >= 0 && elapsed % std::max<uint32_t>(loop, 1) / p.q() == step) return writing(s) ? red(mid) : white(mid);
    return velocity_look(selected_index(s), step_velocity(p, step, steps_lane(selected(s))));
}
Look step_edit_look(const PadScene& s, unsigned pad) {
    const auto& t = selected(s);
    const auto& p = shown(s);
    int key = pad_key(s.project, t, pad);
    if (key < 0) return {};
    int chosen = selected_event(p, s.ui);
    if (chosen >= 0 && p.events[size_t(chosen)].key == key) return white(full);
    unsigned velocity = step_velocity(p, unsigned(std::max<int>(s.ui.step_cursor, 0)) % p.steps(), key);
    if (velocity) return velocity_look(selected_index(s), velocity);
    bool empty = t.engine == EngineKind::Sampler && !t.pads[unsigned(key - int(sampler_first_note)) % sampler_pads].used();
    return empty ? Look {} : track(selected_index(s), faint);
}
// Roll: the key in focus, and the keys of the cursor's step, as the page draws them.
struct RollAt {
    int focus = -1;
    std::array<bool, 192> here {};
};
RollAt roll_at(const PadScene& s) {
    const auto& p = shown(s);
    RollAt out;
    int focus = roll_focus(p, s.ui);
    if (focus >= 0) out.focus = p.events[size_t(focus)].key;
    unsigned step = roll_cursor(p, s.ui);
    uint32_t start = step * p.q(), end = std::min<uint32_t>(start + p.q(), p.length);
    for (size_t n = p.lower(start); n < p.events.size() && p.events[n].tick < end; ++n) if (p.events[n].key < 192) out.here[p.events[n].key] = true;
    return out;
}
Look roll_look(const PadScene& s, const Heard& snd, const RollAt& at, const std::array<bool, 192>& keys, unsigned pad) {
    const auto& t = selected(s);
    int key = pad_key(s.project, t, pad);
    if (key < 0 || !t.used()) return {};
    if (key == at.focus) return white(full);
    Look base = track(selected_index(s), at.here[unsigned(key) % 192] ? full : keys[unsigned(key) % 192] ? dim : faint);
    return playing(s, snd, base, pad);
}
Look time_correct_look(const PadScene& s, const std::array<bool, 192>& keys, unsigned pad) {
    int key = pad_key(s.project, selected(s), pad);
    if (key < 0) return {};
    bool notes = keys[unsigned(key) % 192];
    if (s.ui.tc_all) return notes ? track(selected_index(s), full) : Look {}; // every note moves
    if (s.ui.tc_key(unsigned(key))) return track(selected_index(s), full);
    return notes ? track(selected_index(s), dim) : Look {};
}
// Motion on a kit: the pads with recorded motion in this pattern.
std::array<bool, 192> pads_moving(const PadScene& s) {
    std::array<bool, 192> out {};
    for (const auto& m : shown(s).motion)
        if (pad_param_of(m.param) >= 0) out[m.pad % 192] = true;
    return out;
}
Look motion_look(const PadScene& s, const Heard& snd, const std::array<bool, 192>& moving, unsigned pad) {
    const auto& t = selected(s);
    if (t.engine != EngineKind::Sampler || levels_on(t)) return play_look(s, snd, pad);
    unsigned kit = kit_of(t, pad);
    Look base = kit_base(s, pad);
    if (base.hue == Hue::Track && kit != t.last_pad && moving[kit % 192]) base = track(selected_index(s), mid);
    return counting(s, playing(s, snd, base, pad));
}

// ---- slices, takes and files
// Chop: how strongly each slice sounds (the audition voice's place in the file).
std::array<float, 16> slices_sounding(const PadScene& s) {
    std::array<float, 16> out {};
    const auto& t = selected(s);
    if (t.engine != EngineKind::Sampler || s.taps.track != selected_index(s)) return out;
    const auto& sound = t.pads[t.last_pad % sampler_pads];
    uint32_t frames = sound.used() ? s.project.samples[sound.sample % sample_slots].frames : 0;
    const auto& slices = s.ui.slices;
    for (unsigned n = 0; n < s.taps.count && n < TrackTaps::voices; ++n) {
        const auto& v = s.taps.voice[n];
        if (v.pad != int(sampler_pads) || (v.flags & tap::Choked)) continue;
        for (unsigned k = 0; k < slices.count && k < 16; ++k)
            if (v.at >= slices.start[k] && v.at < slices.end(k, frames)) out[k] = std::max(out[k], v.level);
    }
    return out;
}
Look chop_look(const PadScene& s, const std::array<float, 16>& sounding, unsigned pad) {
    const auto& ui = s.ui;
    unsigned at = selected_index(s), count = ui.slices.count;
    bool manual = ui.chop_mode == ChopMode::Manual;
    if (manual && ui.chop_armed) return pad == 0 ? (blink(s.now, flash_ms) ? red(full) : red(faint)) : Look {}; // pad 1 starts the take
    bool taking = ui.chop_take_at && s.now - ui.chop_take_at < 60000;
    if (manual && taking) return pad == 0 ? white(full) : pad < count ? track(at, dim) : white(faint); // tap to cut here
    if (ui.erase) return pad == 0 ? (count ? track(at, dim) : Look {}) : pad < count ? red(faint) : Look {}; // takes the slice out
    if (held(s, pad)) return white(full);
    if (pad >= count) return {};
    return flare(track(at, pad == ui.chop_slice ? full : dim), sounding[pad]);
}
Look record_look(const PadScene& s, unsigned pad) {
    using S = TakeRecorder::State;
    const auto& ui = s.ui;
    const auto& t = selected(s);
    if (t.engine != EngineKind::Sampler) return {}; // the pads record onto a kit only
    unsigned kit = kit_of(t, pad);
    bool here = ui.rec_track == ui.track;
    auto state = s.host.recorder().state();
    if (here && ui.rec_pad == int(kit)) return state == S::Recording || blink(s.now, flash_ms) ? red(full) : red(faint);
    for (unsigned n = 0; n < ui.rec_chops && here; ++n)
        if (ui.rec_chop_pad[n] == kit) return white(mid); // cut: takes the next part
    if (here && state == S::Recording) return white(faint); // tap to cut here
    return track(selected_index(s), t.pads[kit].used() ? dim : faint);
}
Look resample_look(const PadScene& s, unsigned pad) {
    const auto& t = selected(s);
    if (t.engine != EngineKind::Sampler) return {};
    if (t.pads[kit_of(t, pad)].used()) return track(selected_index(s), dim);
    float pulse = 0.5f + 0.5f * std::sin(float(s.now % 1000) / 1000.0f * 6.2831853f); // empty: pick one
    return track(selected_index(s), between(faint, dim, pulse));
}
// A take of Melodee's own sound: the instrument's pads, the take's pad red.
Look record_keys_look(const PadScene& s, unsigned pad) {
    const auto& ui = s.ui;
    const auto& in = ui.settings.input;
    bool kit_source = in.source != InputSource::Track;
    unsigned from = kit_source ? selected_index(s) : in.track % track_count;
    if (held(s, pad)) return white(full);
    if (kit_source) {
        const auto& t = selected(s);
        unsigned kit = kit_of(t, pad);
        if (ui.rec_pad == int(kit)) return red(full);
        return track(from, t.pads[kit].used() ? dim : faint);
    }
    const auto& source = s.project.tracks[from];
    if (levels_on(source) || !source.used() || drum_pads(source)) return track(from, dim); // its levels: the one note; its drums
    int key = pad_note(s.project, source, pad).key; // its keys, as on its own pages
    return note_look(s.project, from, key_degree(s.project, source, key));
}

// ---- effects
Look compressor_look(const PadScene& s, const Heard& snd, unsigned pad) {
    const auto& c = s.project.compressor;
    if (!c.sidechain) return play_look(s, snd, pad);
    unsigned key = c.key % track_count;
    const auto& keyed = s.project.tracks[key];
    if (s.ui.comp_pads && keyed.engine == EngineKind::Sampler) { // the key kit's pads
        unsigned kit = s.ui.comp_bank % sampler_banks * 16 + pad;
        return c.key_pad == kit ? track(key, full) : keyed.pads[kit].used() ? track(key, faint) : Look {};
    }
    return pad == key ? track(pad, full) : s.project.tracks[pad].used() ? track(pad, faint) : Look {};
}
Look knob_fx_look(const PadScene& s, unsigned pad) {
    const auto& k = s.project.knob_fx;
    const auto& t = selected(s);
    unsigned at = selected_index(s);
    if (t.engine == EngineKind::Sampler) {
        unsigned kit = kit_of(t, pad);
        return k.pad_on(at, kit) ? track(at, full) : t.pads[kit].used() ? track(at, dim) : Look {};
    }
    return k.track_on(pad) ? track(pad, full) : s.project.tracks[pad].used() ? track(pad, dim) : Look {};
}
Look punch_look(const PadScene& s, unsigned pad) {
    const auto& ui = s.ui;
    int kind = punch_on_pad(s.project.punch_bank, pad);
    if (kind < 0) return {};
    unsigned at = selected_index(s);
    if (ui.erase) { // Erase held (pad-fx C): the pads whose punch the pattern keeps red, as Erase + pad takes them out
        const auto& kept = s.patterns.at[at % track_count][selected(s).pattern % pattern_count].punches;
        bool any = std::any_of(kept.begin(), kept.end(), [&](const PatternPunch& e) { return e.kind == uint8_t(kind); });
        return any ? red(dim) : Look {};
    }
    auto colour = [&](float level) { return s.project.punch_target ? track(at, level) : white(level); }; // white: on the master
    int engaged = ui.engaged_at(kind);
    if (engaged >= 0 && ui.engaged[unsigned(engaged)].pad >= 0) // held: as bright as it is pressed
        return white(between(mid, full, ui.engaged[unsigned(engaged)].depth / 100.0f));
    if (ui.punch_sounding(kind)) return colour(full);                                    // latched, sounding
    if (engaged >= 0) return blink(s.now, flash_ms) ? colour(mid) : colour(faint);        // engaged, waiting for a slot
    return colour(dim);
}
Look flex_look(const PadScene& s, unsigned pad) {
    unsigned on = s.host.flex_playing();
    return pad == on ? white(full) : white(pad == 0 ? faint : dim); // pad 1 is Empty
}
Look pad_punch_look(const PadScene& s, const Heard& snd, unsigned pad) {
    const auto& t = selected(s);
    if (t.engine != EngineKind::Sampler) return play_look(s, snd, pad);
    unsigned kit = kit_of(t, pad);
    const auto& sound = t.pads[kit];
    if (!sound.used()) return {};
    return track(selected_index(s), s.ui.pad_punch_held == int(kit) ? full : sound.punch ? mid : faint);
}

// ---- held buttons and open sheets
bool any_notes(const PadScene& s) {
    for (unsigned t = 0; t < track_count; ++t)
        if (s.project.tracks[t].used() && !s.patterns.at[t][s.project.tracks[t].pattern % pattern_count].empty()) return true;
    return false;
}
// Whether a printed Shift + pad job would act, by the checks the job makes first (shell/shift_pads.cpp).
bool shift_can_act(const PadScene& s, unsigned pad) {
    const auto& t = selected(s);
    const auto& p = shown(s);
    bool notes = !p.empty();
    switch (pad) {
    case 1: return notes && std::max<uint32_t>(p.length / 2, 1) != p.length; // Half Seq
    case 2: return notes && p.length * 2 <= s.project.max_length();         // Double Seq
    case 5: case 6: case 13: return notes;                                  // Half Speed, Double Speed, Time Correct
    case 10: return s.ui.resample_pick || (!s.ui.rendering && t.engine == EngineKind::Sampler && any_notes(s)); // Resample
    case 12: case 14: { // Trim Sample, Warp: a kit's own sample, not an instrument's
        if (t.engine != EngineKind::Sampler) return false;
        const auto& sound = t.pads[t.last_pad % sampler_pads];
        if (!sound.used()) return pad == 14;
        const auto& ref = s.project.samples[sound.sample % sample_slots];
        if (ref.instrument()) return false;
        if (pad == 14) return true;
        uint32_t frames = ref.frames, end = sound.end && sound.end < frames ? sound.end : frames;
        return frames && !(sound.start == 0 && end >= frames); // something to cut
    }
    default: return true;
    }
}
Look shift_look(const PadScene& s, unsigned pad) {
    unsigned level = shift_pad_level(s.ui, pad); // 0 not built, 1 a job, 2 a toggle that is on
    if (pad == 10 && s.ui.resample_pick) level = 2;
    if (!level || !shift_can_act(s, pad)) return {};
    return white(level == 2 ? full : dim);
}
Look erase_look(const PadScene& s, const Heard& snd, const std::array<bool, 192>& keys, unsigned pad) {
    const auto& t = selected(s);
    unsigned kit = kit_of(t, pad);
    if (!s.host.playing() && !s.ui.recording) return t.pads[kit].used() ? red(faint) : Look {}; // the pad empties
    if (!keys[(sampler_first_note + kit) % 192]) return {};
    return red_flare(red(dim), snd.level[pad]); // its notes go, each flaring as it plays
}
Look copy_look(const PadScene& s, unsigned pad) {
    const auto& ui = s.ui;
    const auto& t = selected(s);
    unsigned kit = kit_of(t, pad);
    bool used = t.pads[kit].used();
    if (ui.copy_from < 0) return used ? track(selected_index(s), dim) : Look {}; // the pad to copy
    if (ui.copy_from == int(kit)) return white(full);
    return used ? red(faint) : track(selected_index(s), faint); // where: replaced, or landing clean
}
// Shift + Erase on a synth's keys, Roll or Step Edit: the keys with notes in the track's colour; then the key
// copied white, where its notes go Faint (red where they replace notes).
Look key_copy_look(const PadScene& s, const std::array<bool, 192>& keys, unsigned pad) {
    int key = pad_key(s.project, selected(s), pad);
    if (key < 0) return {};
    bool notes = keys[unsigned(key) % 192];
    if (s.ui.copy_from < 0) return notes ? track(selected_index(s), dim) : Look {};
    if (s.ui.copy_from == key) return white(full);
    return notes ? red(faint) : track(selected_index(s), faint);
}
// Shift + Erase on Steps: the steps with notes (on a kit, the lane's); then the step copied white, where it goes Faint.
Look step_copy_look(const PadScene& s, unsigned pad) {
    const auto& p = shown(s);
    unsigned step = unsigned(s.ui.steps_bar) * bar_steps + pad;
    if (step >= p.steps()) return {};
    bool notes = step_velocity(p, step, steps_lane(selected(s))) > 0;
    if (s.ui.copy_from < 0) return notes ? track(selected_index(s), dim) : Look {};
    if (s.ui.copy_from == int(step)) return white(full);
    return notes ? red(faint) : track(selected_index(s), faint);
}
// Erase held where the pads play a synth's keys: the keys with notes Dim red, flaring as they play.
Look key_erase_look(const PadScene& s, const Heard& snd, const std::array<bool, 192>& keys, unsigned pad) {
    int key = pad_key(s.project, selected(s), pad);
    if (key < 0 || !keys[unsigned(key) % 192]) return {};
    return red_flare(red(dim), snd.level[pad]);
}
Look name_look(const PadScene& s, unsigned pad) {
    const auto& n = s.ui.name;
    Look base = pad == 15 ? red(dim) : pad == 14 && n.digits ? white(full) : white(dim); // 16 deletes; 15 the digits
    if (n.last_pad == int(pad) && s.now - n.last_at < NameEntry::retap_ms) // just typed: again for its other letter
        base = blend(base, white(full), 1 - float(s.now - n.last_at) / NameEntry::retap_ms);
    return base;
}
Look sheet_look(const PadScene& s, unsigned pad) {
    using A = UiState::Ask;
    const auto& ui = s.ui;
    if (ui.exit_open) return {};
    bool about = false; // the pad the question is about
    switch (ui.ask) {
    case A::ClearPattern: case A::ClearTrack: about = int(pad) == ui.ask_target; break;
    case A::ClearLoop: about = int(pad % 4) == ui.ask_target; break;
    case A::EraseKeyEvents: about = pad_key(s.project, selected(s), pad) == ui.ask_target; break;
    default: break;
    }
    return about ? (blink(s.now, flash_ms) ? red(full) : red(faint)) : Look {};
}

} // namespace

void pad_looks(const PadScene& s, std::array<Look, 16>& out) {
    const auto& ui = s.ui;
    const auto& t = selected(s);
    Heard snd = heard_now(s);
    auto keys = keys_in(shown(s));
    RollAt roll;
    if (ui.page == PageId::Roll) roll = roll_at(s);
    std::array<bool, 192> moving {};
    if (ui.page == PageId::Motion) moving = pads_moving(s);
    std::array<float, 16> slices {};
    if (ui.page == PageId::Chop) slices = slices_sounding(s);
    for (unsigned pad = 0; pad < 16; ++pad) {
        Look look;
        switch (pad_context(ui, t, s.facts, pad)) {
        case PadContext::Sheet: look = sheet_look(s, pad); break;
        case PadContext::Tracks: look = tracks_look(s, pad); break;
        case PadContext::Name: look = name_look(s, pad); break;
        case PadContext::LoopHeld:
        case PadContext::LoopNow: look = loop_look(s, pad, false); break;
        case PadContext::PatternCopy:
        case PadContext::SeqLayer: look = seq_look(s, pad); break;
        case PadContext::RecordKeys: look = record_keys_look(s, pad); break;
        case PadContext::Mute:
            if (ui.page == PageId::Looper) { look = loop_look(s, pad, true); break; }
            if (mute_on_kit(ui, s.facts.kit_pads)) {
                unsigned kit = kit_of(t, pad);
                if (!t.pads[kit].used()) break;
                look = t.pad_muted(kit) ? red(dim) : flare(pad_soloed(t, kit) ? white(mid) : track(selected_index(s), mid), snd.level[pad]);
                break;
            }
            if (const auto& tr = s.project.tracks[pad]; tr.used()) {
                float m = meter(s, pad);
                look = tr.mix.mute ? red(dim) : tr.mix.solo ? white(between(mid, full, m))
                     : any_solo(s.project) ? track(pad, faint) : track(pad, between(mid, full, m));
            }
            break;
        case PadContext::SamplePick: look = play_look(s, snd, pad); break; // the pad hit becomes the last pad
        case PadContext::Shift: look = shift_look(s, pad); break;
        case PadContext::ResamplePick: look = resample_look(s, pad); break;
        case PadContext::PadCopy: look = copy_look(s, pad); break;
        case PadContext::PadErase: look = erase_look(s, snd, keys, pad); break;
        case PadContext::KeyCopy: look = key_copy_look(s, keys, pad); break;
        case PadContext::StepCopy: look = step_copy_look(s, pad); break;
        case PadContext::KeyErase: look = key_erase_look(s, snd, keys, pad); break;
        case PadContext::Page:
            switch (ui.page) {
            case PageId::Looper: look = loop_look(s, pad, false); break;
            case PageId::Tracks: look = tracks_look(s, pad); break;
            case PageId::Song: look = song_look(s, pad); break;
            case PageId::Scenes: look = scenes_look(s, pad); break; // roadmap P5
            case PageId::Steps: look = steps_look(s, pad); break;
            case PageId::StepEdit: look = step_edit_look(s, pad); break;
            case PageId::Roll: look = roll_look(s, snd, roll, keys, pad); break;
            case PageId::TimeCorrect: look = time_correct_look(s, keys, pad); break;
            case PageId::Motion: look = motion_look(s, snd, moving, pad); break;
            case PageId::Chop: look = chop_look(s, slices, pad); break;
            case PageId::Record: look = record_look(s, pad); break;
            case PageId::Compressor: look = compressor_look(s, snd, pad); break;
            case PageId::KnobFx: look = knob_fx_look(s, pad); break;
            case PageId::Punch: look = punch_look(s, pad); break;
            case PageId::FlexBeat: look = flex_look(s, pad); break;
            case PageId::PadPunch: look = pad_punch_look(s, snd, pad); break;
            default: look = play_look(s, snd, pad); break; // the track's own pads
            }
            break;
        }
        // The Pad lights setting, last: white as bright as the colour was, or off.
        if (ui.settings.pad_lights == 1 && look.lit()) {
            float top = std::max<float>(led_colour::white(1).r, 1);
            look.rgb = led_colour::white(std::max({look.rgb.r, look.rgb.g, look.rgb.b}) / top);
            look.colour = 0xFFFFFF; look.hue = Hue::White;
        }
        if (ui.settings.pad_lights == 2) look = {};
        out[pad] = look;
    }
}

} // namespace melodee
