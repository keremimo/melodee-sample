// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/sfz.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// A SoundFont 2 file (.sf2) read (roadmap P3), after melodee-ios's reader: its
// presets, and one of them as the sampler's zones, read as an SFZ's are. The
// file is one blob holding every sound of every preset: nothing here copies it,
// the caller keeps its bytes (mapped) while this reads them, and only the
// sounds of the preset chosen are taken out, one at a time, when its zones land.
// Two sounds panned hard apart, alike in all else, become one stereo sound.
// JUCE and inMusic's open-source packages hold no SoundFont reader, so it is
// written here.
namespace melodee {

struct Sf2Preset {
    unsigned index = 0;        // its place in the file
    int bank = 0, program = 0;
    std::string name;
};

class Sf2 {
public:
    // The file's lists, from its whole bytes (kept by the caller). Empty on success, else why: not an SF2, or its
    // sounds packed (an .sf3), which nothing here unpacks.
    std::string open(const uint8_t* bytes, size_t size);
    // In the order they are played on: by bank, then by program.
    const std::vector<Sf2Preset>& presets() const { return list; }
    // Whether a preset has a sound to play at all.
    bool plays(const Sf2Preset& preset) const;
    // One preset's zones; each region's sample names its sound ("12") or a stereo pair ("12+13").
    SfzRead read(const Sf2Preset& preset) const;
    // A region's sound, its frames at their own rate (two sides for a pair, else one, as `right` empty); its loop
    // from the sound's header, frames from its start (end past the last), -1 none. False when it names none.
    bool sound(const std::string& name, std::vector<float>& left, std::vector<float>& right, unsigned& rate,
               int64_t& loop_start, int64_t& loop_end) const;

private:
    struct Range { size_t from = 0, size = 0; };
    struct Header { std::string name; uint32_t start = 0, end = 0, loop_start = 0, loop_end = 0, rate = 0; int pitch = 60, correction = 0;
                    unsigned link = 0, type = 0; };
    bool header(int index, Header& out) const;
    unsigned u16(size_t at) const { return unsigned(data[at]) | unsigned(data[at + 1]) << 8; }
    uint32_t u32(size_t at) const { return uint32_t(u16(at)) | uint32_t(u16(at + 2)) << 16; }
    int s16(size_t at) const { return int16_t(uint16_t(u16(at))); }
    std::string text(size_t at, size_t count) const;
    const uint8_t* data = nullptr;
    size_t size = 0;
    Range sounds, low, phdr, pbag, pmod, pgen, inst, ibag, imod, igen, shdr;
    std::vector<Sf2Preset> list;
};

// "<file>.sf2#<preset>": an SF2's preset as an instrument path; false when the path names none.
bool split_sf2_preset(const std::string& path, std::string& file, unsigned& preset);
bool is_sf2(const std::string& path);

} // namespace melodee
