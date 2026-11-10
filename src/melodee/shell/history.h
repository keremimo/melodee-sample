// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
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

struct Edit {
    std::string what;             // shown on undo and redo: "Delete A01"
    uint8_t track = 0;
    int16_t pad = -1;             // a kit pad, or -1
    PadState pad_before, pad_after;
    int16_t pattern = -1;         // a pattern of the track, or -1
    std::unique_ptr<Pattern> pattern_before, pattern_after;
    std::unique_ptr<Song> song_before, song_after;       // a take into the song
    std::unique_ptr<Motion> motion_before, motion_after; // pot moves recorded, or doubled
    std::unique_ptr<TrackState> track_before, track_after; // a track copied onto this one (`track`)
    // One pattern number across tracks (Song -> new pattern): `column`, each changed track's pattern.
    int16_t column = -1;
    std::vector<std::pair<uint8_t, Pattern>> column_before, column_after;
};

class History {
public:
    static constexpr size_t depth = 32;
    void push(Edit&& edit) {
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
