// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/limits.h"
#include "melodee/model/pattern.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

// Settings > SEQ > Note preview (Context::preview_notes(), shell/note_preview.cpp): a note written or moved on Step
// Edit or Roll without a pad or key playing it is heard once on the selected track. Step Edit: a step set on the pads
// in STEPS, a held step's pitch moved with the wheel. Roll: the marked notes moved up or down, or pasted. Pads and MIDI
// keys are heard as they play, with the setting on or off. A move with the wheel is heard once the wheel rests, so a
// fast turn plays one note, not one a detent; nothing repeats by itself.
namespace melodee {

constexpr unsigned preview_notes_max = notes_per_step; // a chord's worth
constexpr uint32_t preview_settle_ms = 150;            // the wheel's moves: heard once it rests this long
constexpr uint32_t preview_shortest_ms = 60, preview_longest_ms = 1000;

// The notes a preview plays and the ones sounding (Context::note_preview).
struct NotePreview {
    std::array<Event, preview_notes_max> waiting {}; // to play at `due`
    uint8_t count = 0, track = 0;
    uint16_t q = sixteenth_ticks; // their pattern's
    uint32_t due = 0;
    std::array<int32_t, preview_notes_max> sounding = [] {
        std::array<int32_t, preview_notes_max> keys {};
        keys.fill(-1);
        return keys;
    }();
    uint8_t sounding_track = 0;
    uint32_t until = 0; // when the sounding ones end
};

// How long a preview of `e` sounds: as long as the sequencer holds it (a touch short of its length, by a tenth of a
// step of `q`) at `tempo` tenths of a BPM, between preview_shortest_ms and preview_longest_ms.
inline uint32_t preview_ms(const Event& e, unsigned q, unsigned tempo) {
    double each = double(std::max<uint32_t>(e.length, 1)) / e.hits(); // a ratchet: its first hit
    double ticks = each - std::min<double>(std::max(q, 1u), each) * 0.1;
    double ms = ticks * 60000.0 / (std::max(tempo, 10u) / 10.0) / ticks_per_quarter;
    return uint32_t(std::clamp(ms, double(preview_shortest_ms), double(preview_longest_ms)));
}

// What a preview of `events` plays: the notes starting with the earliest (a chord, a run's first), muted ones left out,
// up to preview_notes_max of them; how many went into `out`.
inline unsigned preview_chord(const std::vector<Event>& events, std::array<Event, preview_notes_max>& out) {
    uint32_t first = UINT32_MAX;
    for (const auto& e : events) if (!(e.flags & step_mute)) first = std::min(first, e.tick);
    unsigned n = 0;
    for (const auto& e : events)
        if (!(e.flags & step_mute) && e.tick == first && n < out.size()) out[n++] = e;
    return n;
}

} // namespace melodee
