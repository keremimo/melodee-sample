// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/jam_log.h"
#include "melodee/model/project.h"
#include "melodee/shell/chop.h"
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Undo and redo (Shift + Minus / Shift + Plus), the one history for every
// edit: what a press (or a whole Seq Record take) changed, before and after.
// A kit pad (its sound and its own effect chain), a pattern, the song, the
// motion, a whole track; only the parts that changed are kept. Plain data on the MIDI
// thread; Context sends what an undo or redo changes to the audio thread.
namespace melodee {

struct PadState {
    PadSound sound;
    Chain chain;
    std::vector<ZoneTrim> trims; // round 3 "instrument C": its instrument's zones trimmed (their pad as kept, not re-read)
};
inline PadState pad_state(const Track& t, unsigned pad) {
    PadState s {t.pads[pad % sampler_pads], t.pad_fx.at[pad % sampler_pads], {}};
    for (const auto& z : t.zone_trims) if (z.used() && z.pad == pad % sampler_pads) s.trims.push_back(z);
    return s;
}
// A pad's state onto kit pad `pad` (its own, or another's when copied or moved): its trims go with it, a free
// track's trims allowing.
inline void set_pad_state(Track& t, unsigned pad, const PadState& s) {
    t.pads[pad % sampler_pads] = s.sound;
    t.pad_fx.at[pad % sampler_pads] = s.chain;
    clear_trims(t.zone_trims, pad % sampler_pads);
    for (const auto& z : s.trims) set_trim(t.zone_trims, pad % sampler_pads, z.zone, z.slot, z.tune, z.level);
}
inline bool same_trims(const PadState& a, const PadState& b) {
    if (a.trims.size() != b.trims.size()) return false;
    for (const auto& z : a.trims) {
        bool found = false;
        for (const auto& o : b.trims) if (o.zone == z.zone && o.slot == z.slot && o.tune == z.tune && o.level == z.level) { found = true; break; }
        if (!found) return false;
    }
    return true;
}

// A whole track and its sixteen patterns, for a track copy (Shift + Erase with Pad Bank held).
struct TrackState {
    Track track;
    std::array<Pattern, pattern_count> patterns;
};

// Parity phase 10: the rest a pot gesture or a page's button may change.
// Every track's mix strip (level, pan, send and its bus; mute and solo are
// performance, left out) and the buses' returns.
// Every strip (the mixer, by strip number), each track's output (Mix::out) and the submixes' mix too.
inline bool same_strip(const Strip& a, const Strip& b) {
    for (unsigned p = 0; p < unsigned(StripPart::Count); ++p) if (strip_get(a, StripPart(p)) != strip_get(b, StripPart(p))) return false;
    return true;
}
struct MixState {
    std::array<uint8_t, track_count> level {};
    std::array<std::array<uint8_t, bus_count>, track_count> sends {}; // each track's send into each bus
    std::array<int8_t, track_count> pan {};
    std::array<uint8_t, bus_count> returns {};
    std::array<Strip, strip_channels> strips {};
    std::array<uint8_t, track_count> outs {};
    // The submixes' level, pan and sends (their mute and solo are performance, as a track's).
    std::array<uint8_t, sub_count> sub_level {};
    std::array<int8_t, sub_count> sub_pan {};
    std::array<std::array<uint8_t, bus_count>, sub_count> sub_sends {};
    bool input_wet = false; // the mixer's input: takes after its strip
    bool operator==(const MixState& o) const {
        if (input_wet != o.input_wet) return false;
        if (!(level == o.level && sends == o.sends && pan == o.pan && returns == o.returns && outs == o.outs)) return false;
        if (!(sub_level == o.sub_level && sub_pan == o.sub_pan && sub_sends == o.sub_sends)) return false;
        for (unsigned n = 0; n < strip_channels; ++n) if (!same_strip(strips[n], o.strips[n])) return false;
        return true;
    }
};
// A track's name and whether it is one of its own (Tracks, Shift + Push): nothing else of the track changes with it.
struct TrackName {
    char text[name_length] {};
    bool named = false;
};
// A track's sound: its engine's values and its mod routes.
struct SoundState {
    std::array<int16_t, max_params> params {};
    std::array<ModRoute, mod_routes> mods {};
    bool operator==(const SoundState& o) const {
        if (params != o.params) return false;
        for (unsigned r = 0; r < mod_routes; ++r)
            if (mods[r].source != o.mods[r].source || mods[r].destination != o.mods[r].destination || mods[r].amount != o.mods[r].amount) return false;
        return true;
    }
};
// The master's performance effects: Knob FX's effect (its targets and the mode
// are performance, left out), the compressor, the MPC Pad FX's K1-K3, Flex
// Beat and the punches' depth, snap and tail.
// One track's patterns' Motion (roadmap P1: each pattern's own), for an edit that clears lanes across them.
struct TrackMotion {
    uint8_t track = 0;
    std::array<std::vector<MotionPoint>, pattern_count> lanes;
    bool operator==(const TrackMotion& o) const { return track == o.track && lanes == o.lanes; }
};

struct MasterState {
    uint8_t knob_choice = 0, knob_kind = 0;
    bool knob_bypass = false;
    std::array<int16_t, 6> knob_params {};
    std::array<int16_t, 3> knob_a {}, knob_b {}; // Knob FX C: its two settings and the fader's place between them
    uint8_t knob_morph = 0;
    MasterComp compressor {};
    PadFxParams pad_fx {};
    FlexSettings flex {};
    uint8_t punch_depth = 0, punch_snap = 0, punch_tail = 0;
    bool punch_quantize = true;
    std::array<Modulator, modulator_count> modulators {}; // roadmap P4: the MOD page's
    std::array<Macro, macro_count> macros {};
    std::array<std::array<uint64_t, 2>, track_count> knob_pads {}; // Knob FX's kit pads, per track
    bool operator==(const MasterState& o) const { // field by field: padding is never compared
        const auto& c = compressor; const auto& d = o.compressor;
        return knob_choice == o.knob_choice && knob_kind == o.knob_kind && knob_bypass == o.knob_bypass && knob_params == o.knob_params
            && knob_a == o.knob_a && knob_b == o.knob_b && knob_morph == o.knob_morph
            && c.on == d.on && c.color == d.color && c.attack == d.attack && c.release == d.release && c.amount == d.amount && c.boost == d.boost
            && c.sidechain == d.sidechain && c.key == d.key && c.key_pad == d.key_pad
            && pad_fx == o.pad_fx && flex.mode == o.flex.mode && flex.mix == o.flex.mix && flex.quantize == o.flex.quantize
            && punch_depth == o.punch_depth && punch_snap == o.punch_snap && punch_tail == o.punch_tail && punch_quantize == o.punch_quantize
            && modulators == o.modulators && macros == o.macros && knob_pads == o.knob_pads;
    }
};
// The tempo: the project's, each pattern's own and whether patterns keep their own (BPM per pattern).
struct TempoState {
    uint16_t tempo = 0;
    std::array<uint16_t, pattern_count> patterns {};
    bool per_pattern = false;
    bool operator==(const TempoState& o) const { return tempo == o.tempo && patterns == o.patterns && per_pattern == o.per_pattern; }
};
// Chop's slices of the region in hand (UI state, not saved).
struct ChopState {
    Slices slices {};
    HandCuts hand {};
    uint8_t mode = 0, slice = 0;
    std::array<int, 5> amount {};
    uint16_t sample = 0;
    uint32_t from = 0, to = 0;
    bool operator==(const ChopState& o) const {
        return slices.same(o.slices) && hand.same(o.hand) && mode == o.mode
            && slice == o.slice && amount == o.amount && sample == o.sample && from == o.from && to == o.to;
    }
};

// A plug-in picked over one in use (shell/plugin_jobs.cpp): the entry's plug-in on either side and its state. The
// side an undo or redo leaves takes the plug-in's state as it is then (Context::swap_plugin), so each comes back as
// it was last heard.
struct PluginSwap {
    uint8_t entry = 0;
    PluginRef before, after;
    std::vector<uint8_t> state_before, state_after;
};

// A sequence: its name, tempo and colour, and what it keeps for each track (model/scene_links.h): CAPTURE and ERASE
// on Sequences.
struct SceneState {
    uint8_t index = 0;
    Scene scene;
    std::array<uint8_t, track_count> kept {};
    uint16_t muted = 0; // the tracks it keeps muted, a bit each (Track::scene_mutes)
};

struct Edit {
    std::string what;             // shown on undo and redo: "Delete A01"
    uint32_t at = 0;              // when it was kept (History::clock, ms): the undo list's "2 min"
    uint8_t track = 0;
    int16_t pad = -1;             // a kit pad, or -1
    PadState pad_before, pad_after;
    // Several kit pads of `track` at once: a Sample Record take's pad and the pads cut during it.
    std::vector<std::pair<uint8_t, PadState>> pads_before, pads_after;
    int16_t pattern = -1;         // a pattern of the track, or -1
    std::unique_ptr<Pattern> pattern_before, pattern_after;
    int16_t pattern_was = -1;     // the track's pattern before the edit made `pattern` the one it plays (Chop's TO PADS), or -1
    std::unique_ptr<Song> song_before, song_after;       // a take into the song
    std::unique_ptr<TrackMotion> motion_before, motion_after; // a track's patterns' motion (an effect moved or removed)
    std::unique_ptr<TrackState> track_before, track_after; // a track copied onto this one (`track`)
    int8_t swap_a = -1, swap_b = -1; // a track move: the two that traded places; undo and redo trade them again
    // One pattern number across tracks (Song -> new pattern): `column`, each changed track's pattern.
    int16_t column = -1;
    std::vector<std::pair<uint8_t, Pattern>> column_before, column_after;
    // Roadmap P5: patterns of several tracks, each its own number (Capture into each track's next empty pattern).
    struct PatternAt { uint8_t track = 0, pattern = 0; Pattern value; };
    std::vector<PatternAt> patterns_before, patterns_after;
    // Parity phase 10: an effect chain (`chain`, numbered as in model/project.h), the mix, a track's sound
    // (`track`), the master's effects, Chop's slices.
    int16_t chain = -1;
    std::unique_ptr<Chain> chain_before, chain_after;
    std::unique_ptr<MixState> mix_before, mix_after;
    std::unique_ptr<SoundState> sound_before, sound_after;
    std::unique_ptr<TrackName> name_before, name_after; // a track named (`track`)
    int16_t colour_before = -1, colour_after = -1;        // a track's colour picked (`track`, issue 69): Track::colour
    std::unique_ptr<MasterState> master_before, master_after;
    std::unique_ptr<ChopState> chop_before, chop_after;
    std::unique_ptr<TempoState> tempo_before, tempo_after; // the wheel, K1, B1 / B3, Minus / Plus and Tap Tempo
    std::unique_ptr<PluginSwap> plugin; // a plug-in replaced (its track's name and pads in track_before / after)
    std::unique_ptr<SceneState> scene_before, scene_after; // a sequence kept or cleared
    std::unique_ptr<JamList> jams_before, jams_after;      // a jam deleted (JAMS' ERASE)
    size_t bytes = 0;                                      // what it holds, as History counted it when kept (edit_bytes)
};

// The memory an edit holds: itself and the copies it keeps, their notes, Motion and plug-in states included. Counted
// once, when it is kept (a take that grows its edit afterwards is not counted again).
inline size_t pattern_bytes(const Pattern& p) {
    return sizeof(Pattern) + p.events.size() * sizeof(Event) + p.motion.size() * sizeof(MotionPoint) + p.lanes.size() * sizeof(LaneLength)
         + p.euclids.size() * sizeof(EuclidLane) + p.punches.size() * sizeof(PatternPunch)
         + p.feel.groove_offsets.size() * sizeof(int16_t) + p.feel.groove_levels.size();
}
inline size_t edit_bytes(const Edit& e) {
    size_t n = sizeof(Edit) + e.what.size() + (e.pads_before.size() + e.pads_after.size()) * sizeof(PadState);
    for (const auto* p : {e.pattern_before.get(), e.pattern_after.get()}) if (p) n += pattern_bytes(*p);
    for (const auto* t : {e.track_before.get(), e.track_after.get()})
        if (t) { n += sizeof(TrackState); for (const auto& p : t->patterns) n += pattern_bytes(p) - sizeof(Pattern); }
    for (const auto* list : {&e.column_before, &e.column_after}) for (const auto& c : *list) n += sizeof(c) + pattern_bytes(c.second);
    for (const auto* list : {&e.patterns_before, &e.patterns_after}) for (const auto& c : *list) n += sizeof(c) + pattern_bytes(c.value);
    if (e.song_before) n += sizeof(Song);
    if (e.song_after) n += sizeof(Song);
    for (const auto* m : {e.motion_before.get(), e.motion_after.get()})
        if (m) { n += sizeof(TrackMotion); for (const auto& lane : m->lanes) n += lane.size() * sizeof(MotionPoint); }
    n += (e.chain_before ? sizeof(Chain) : 0) + (e.chain_after ? sizeof(Chain) : 0) + (e.mix_before ? sizeof(MixState) : 0)
       + (e.mix_after ? sizeof(MixState) : 0) + (e.sound_before ? sizeof(SoundState) : 0) + (e.sound_after ? sizeof(SoundState) : 0)
       + (e.master_before ? sizeof(MasterState) : 0) + (e.master_after ? sizeof(MasterState) : 0)
       + (e.chop_before ? sizeof(ChopState) : 0) + (e.chop_after ? sizeof(ChopState) : 0);
    if (e.plugin) n += sizeof(PluginSwap) + e.plugin->state_before.size() + e.plugin->state_after.size();
    for (const auto* j : {e.jams_before.get(), e.jams_after.get()})
        if (j) for (const auto& log : j->jams) n += sizeof(log) + log.bars.size() * sizeof(JamBar);
    return n;
}

class History {
public:
    // The last 512 steps, as long as what they hold stays under `budget` (the oldest go first; the newest always stays).
    static constexpr size_t depth = 512;
    static constexpr size_t budget = size_t(64) << 20;
    // Freeze plan F2c: asked before an edit is kept; false: it is not (the owner put back what it changed).
    bool (*gate)(void* owner, Edit& edit) = nullptr;
    void* gate_owner = nullptr;
    void push(Edit&& edit) {
        ++pushes;
        if (gate && !gate(gate_owner, edit)) return;
        edit.at = clock ? *clock : 0;
        edit.bytes = edit_bytes(edit);
        for (const auto& e : redo_) bytes_ -= e.bytes;
        redo_.clear();
        bytes_ += edit.bytes;
        undo_.push_back(std::move(edit));
        while (undo_.size() > depth || (bytes_ > budget && undo_.size() > 1)) { bytes_ -= undo_.front().bytes; undo_.pop_front(); }
    }
    // The edit to take back (or do again), moved to the other list; nullptr when there is none.
    Edit* undo() { return step(undo_, redo_); }
    Edit* redo() { return step(redo_, undo_); }
    bool can_undo() const { return !undo_.empty(); }
    bool can_redo() const { return !redo_.empty(); }
    void clear() { undo_.clear(); redo_.clear(); bytes_ = 0; }
    size_t bytes() const { return bytes_; } // what the steps hold, undo's and redo's
    size_t size() const { return undo_.size(); }
    const Edit* last() const { return undo_.empty() ? nullptr : &undo_.back(); }
    Edit* latest() { return undo_.empty() ? nullptr : &undo_.back(); }
    const Edit* next() const { return redo_.empty() ? nullptr : &redo_.back(); } // what a redo would do
    Edit* upcoming() { return redo_.empty() ? nullptr : &redo_.back(); }
    // Every edit kept, to undo and to redo (roadmap P3: which sample slots they could bring back).
    template <typename F> void each(F&& f) const { for (const auto& e : undo_) f(e); for (const auto& e : redo_) f(e); }
    unsigned pushes = 0; // edits kept so far: a press that kept its own edit is not kept twice (shell/undo_scope.h)
    const uint32_t* clock = nullptr; // the shell's clock (ms, Context::now): each edit's `at`
    // The undo list (shell/history_list.h): the steps undone, the next redo last, and the steps done, the newest last.
    size_t redos() const { return redo_.size(); }
    const Edit& undone(size_t n) const { return redo_[n]; }
    const Edit& kept(size_t n) const { return undo_[n]; }

private:
    std::deque<Edit> undo_, redo_;
    size_t bytes_ = 0;
    Edit* step(std::deque<Edit>& from, std::deque<Edit>& to) {
        if (from.empty()) return nullptr;
        to.push_back(std::move(from.back()));
        from.pop_back();
        if (to.size() > depth) { bytes_ -= to.front().bytes; to.pop_front(); }
        return &to.back();
    }
};

// Roadmap P3: the sample slots in use: every kit pad's layers, and every pad an undo or redo could bring back (and
// Chop's region one could), so a slot freed on save is never one an edit would play again.
inline std::array<bool, sample_slots> samples_in_use(const Project& project, const History& edits) {
    std::array<bool, sample_slots> used {};
    auto mark = [&](const PadSound& p) {
        if (p.used()) used[p.sample % sample_slots] = true;
        for (const auto& l : p.layers) if (l.used()) used[l.sample % sample_slots] = true;
    };
    for (const auto& t : project.tracks) for (const auto& p : t.pads) mark(p);
    edits.each([&](const Edit& e) {
        mark(e.pad_before.sound); mark(e.pad_after.sound);
        for (const auto& kept : e.pads_before) mark(kept.second.sound);
        for (const auto& kept : e.pads_after) mark(kept.second.sound);
        for (const auto* t : {e.track_before.get(), e.track_after.get()}) if (t) for (const auto& p : t->track.pads) mark(p);
        for (const auto* c : {e.chop_before.get(), e.chop_after.get()}) if (c && c->sample < sample_slots) used[c->sample] = true;
    });
    return used;
}

inline std::string pad_text(const char* verb, unsigned pad) {
    char label[4]; pad_label(pad, label);
    return std::string(verb) + " " + label;
}

} // namespace melodee
