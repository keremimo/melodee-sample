// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
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
};
inline PadState pad_state(const Track& t, unsigned pad) { return {t.pads[pad % sampler_pads], t.pad_fx.at[pad % sampler_pads]}; }
inline void set_pad_state(Track& t, unsigned pad, const PadState& s) {
    t.pads[pad % sampler_pads] = s.sound;
    t.pad_fx.at[pad % sampler_pads] = s.chain;
}

// A whole track and its sixteen patterns, for a track copy (Shift + Erase with Pad Bank held).
struct TrackState {
    Track track;
    std::array<Pattern, pattern_count> patterns;
};

// Parity phase 10: the rest a pot gesture or a page's button may change.
// Every track's mix strip (level, pan, send and its bus; mute and solo are
// performance, left out) and the buses' returns.
struct MixState {
    std::array<uint8_t, track_count> level {}, send {}, bus {};
    std::array<int8_t, track_count> pan {};
    std::array<uint8_t, bus_count> returns {};
    bool operator==(const MixState& o) const { return level == o.level && send == o.send && bus == o.bus && pan == o.pan && returns == o.returns; }
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
struct MasterState {
    uint8_t knob_choice = 0, knob_kind = 0;
    bool knob_bypass = false;
    std::array<int16_t, 6> knob_params {};
    MasterComp compressor {};
    PadFxParams pad_fx {};
    FlexSettings flex {};
    uint8_t punch_depth = 0, punch_snap = 0, punch_tail = 0;
    bool operator==(const MasterState& o) const { // field by field: padding is never compared
        const auto& c = compressor; const auto& d = o.compressor;
        return knob_choice == o.knob_choice && knob_kind == o.knob_kind && knob_bypass == o.knob_bypass && knob_params == o.knob_params
            && c.on == d.on && c.color == d.color && c.attack == d.attack && c.release == d.release && c.amount == d.amount && c.boost == d.boost
            && pad_fx == o.pad_fx && flex.mode == o.flex.mode && flex.mix == o.flex.mix && flex.quantize == o.flex.quantize
            && punch_depth == o.punch_depth && punch_snap == o.punch_snap && punch_tail == o.punch_tail;
    }
};
// Chop's slices of the region in hand (UI state, not saved).
struct ChopState {
    Slices slices {};
    uint8_t mode = 0, slice = 0;
    std::array<int, 4> amount {};
    uint16_t sample = 0;
    uint32_t from = 0, to = 0;
    bool operator==(const ChopState& o) const {
        return slices.start == o.slices.start && slices.count == o.slices.count && slices.last == o.slices.last && mode == o.mode
            && slice == o.slice && amount == o.amount && sample == o.sample && from == o.from && to == o.to;
    }
};

struct Edit {
    std::string what;             // shown on undo and redo: "Delete A01"
    uint8_t track = 0;
    int16_t pad = -1;             // a kit pad, or -1
    PadState pad_before, pad_after;
    // Several kit pads of `track` at once: a Sample Record take's pad and the pads cut during it.
    std::vector<std::pair<uint8_t, PadState>> pads_before, pads_after;
    int16_t pattern = -1;         // a pattern of the track, or -1
    std::unique_ptr<Pattern> pattern_before, pattern_after;
    std::unique_ptr<Song> song_before, song_after;       // a take into the song
    std::unique_ptr<Motion> motion_before, motion_after; // pot moves recorded, or doubled
    std::unique_ptr<TrackState> track_before, track_after; // a track copied onto this one (`track`)
    // One pattern number across tracks (Song -> new pattern): `column`, each changed track's pattern.
    int16_t column = -1;
    std::vector<std::pair<uint8_t, Pattern>> column_before, column_after;
    // Parity phase 10: an effect chain (`chain`, numbered as in model/project.h), the mix, a track's sound
    // (`track`), the master's effects, Chop's slices.
    int16_t chain = -1;
    std::unique_ptr<Chain> chain_before, chain_after;
    std::unique_ptr<MixState> mix_before, mix_after;
    std::unique_ptr<SoundState> sound_before, sound_after;
    std::unique_ptr<MasterState> master_before, master_after;
    std::unique_ptr<ChopState> chop_before, chop_after;
};

class History {
public:
    static constexpr size_t depth = 32;
    void push(Edit&& edit) {
        ++pushes;
        redo_.clear();
        undo_.push_back(std::move(edit));
        if (undo_.size() > depth) undo_.pop_front();
    }
    // The edit to take back (or do again), moved to the other list; nullptr when there is none.
    Edit* undo() { return step(undo_, redo_); }
    Edit* redo() { return step(redo_, undo_); }
    bool can_undo() const { return !undo_.empty(); }
    bool can_redo() const { return !redo_.empty(); }
    void clear() { undo_.clear(); redo_.clear(); }
    size_t size() const { return undo_.size(); }
    const Edit* last() const { return undo_.empty() ? nullptr : &undo_.back(); }
    unsigned pushes = 0; // edits kept so far: a press that kept its own edit is not kept twice (shell/undo_scope.h)

private:
    std::deque<Edit> undo_, redo_;
    static Edit* step(std::deque<Edit>& from, std::deque<Edit>& to) {
        if (from.empty()) return nullptr;
        to.push_back(std::move(from.back()));
        from.pop_back();
        if (to.size() > depth) to.pop_front();
        return &to.back();
    }
};

inline std::string pad_text(const char* verb, unsigned pad) {
    char label[4]; pad_label(pad, label);
    return std::string(verb) + " " + label;
}

} // namespace melodee
