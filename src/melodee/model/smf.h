// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include <cstdint>
#include <string>
#include <vector>

// Roadmap P4: Standard MIDI Files, in and out, at Melodee's 96 ticks a quarter. Plain data, no I/O.
namespace melodee {

struct SmfNote {
    uint32_t tick = 0, length = 1; // ticks at 96 a quarter
    uint8_t key = 60, velocity = 100;
};
struct SmfTrack {
    std::string name;
    uint8_t channel = 1; // 1..16
    std::vector<SmfNote> notes;
};
constexpr unsigned smf_ppq = 96;

// Format 1: a first track with the tempo (tenths of a BPM) and time signature (beats over unit), then one track a
// SmfTrack, each its name and notes on its channel.
std::vector<uint8_t> write_smf(const std::vector<SmfTrack>& tracks, unsigned tempo_tenths, unsigned beats, unsigned unit);
// Format 0 or 1, any division (ticks a quarter; SMPTE time is refused): every note, at 96 ticks a quarter, a track
// each MTrk with notes (format 0: one a channel); the first tempo found (tenths of a BPM; 0 none). False and `why`
// when it is no MIDI file.
bool read_smf(const std::vector<uint8_t>& data, std::vector<SmfTrack>& out, std::string& why, unsigned* tempo_tenths = nullptr);

// What a project gives a MIDI file: the song's tracks laid out as it plays them (its clips, muted ones out), or with no
// song each track's pattern playing, once; a kit's pads as notes 36 on (A01, as MIDI in hears them), a MIDI track on its
// own channel, a kit on 10, the rest on their track's number.
std::vector<SmfTrack> smf_tracks(const Project& project, const PatternBank& patterns);
// The notes a MIDI file's tracks lay on `track` (as above), and the pattern's length; false when none fit.
bool smf_pattern(const std::vector<SmfTrack>& tracks, const Track& track, uint32_t bar_ticks, Pattern& out);

} // namespace melodee
