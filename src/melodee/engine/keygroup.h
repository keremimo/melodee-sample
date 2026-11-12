// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/sfz.h"
#include "melodee/model/sampler.h"
#include <array>
#include <map>
#include <string>
#include <vector>

// An MPC keygroup program (.xpm, the XML kind) as the sampler's zones: the same
// read an .sfz makes, so its samples are laid out on the card and streamed as
// an SFZ's are. Each keygroup is a key range; each of its layers, a sample over
// a velocity range. The file's XML is read elsewhere (shell, with JUCE) into
// the plain tree below; nothing here reads a file.
namespace melodee {

using KeygroupTags = std::map<std::string, std::string>; // a child element's name and its text, untrimmed

struct KeygroupInstrument {
    int number = 0;                    // its number="" attribute, 1 up
    KeygroupTags tags;
    std::vector<KeygroupTags> layers;  // each <Layer>'s
};

struct KeygroupProgram {
    std::string type;                  // <Program type="">: "Keygroup" is the one played
    KeygroupTags tags;
    std::vector<KeygroupInstrument> instruments;
};

SfzRead read_keygroup(const KeygroupProgram& program);

// One pad of a drum program (or of an MPC Sample project's kit) with more than
// one layer: its layers as zones over every key, the pad's note at their root,
// so the pad's own Semi moves them. The pad's settings are keygroup_pad()'s.
SfzRead read_drum_pad(const KeygroupInstrument& pad);

// The layers of an instrument that sound (active, unmuted, with a sample).
std::vector<const KeygroupTags*> keygroup_layers(const KeygroupInstrument& instrument);
// The file a layer plays, relative to the program's folder: its SampleName and ".WAV".
std::string keygroup_sample(const KeygroupTags& layer);

// A drum program's pad as a kit pad: level, pan, tune, play, mono, mute group,
// envelope, velocity and filter from the instrument; with one layer, that
// layer's tune, region (frames of the source times `to_model`), loop and
// direction too, the pad then playing its file alone.
void keygroup_pad(const KeygroupInstrument& instrument, double to_model, PadSound& pad);

// Roadmap P3: a drum pad of two to four layers as the kit pad's own layers,
// over what keygroup_pad() (or an .xpj's pad) set: layer 1 is the pad's
// sample, region, loop and direction, its tune, volume and pan folded into the
// pad's; layers 2-4 play `slots[n]` with their tune, volume and pan as
// offsets from layer 1's and their velocities; ZonePlay picks Velocity, Cycle
// or Random. `to_model[n]` scales a layer's frames to 44.1 kHz. False (the pad
// untouched) when the pad does not fit: another layer count, or a note-off pad.
bool keygroup_pad_layers(const KeygroupInstrument& instrument, const std::array<uint16_t, pad_layers>& slots,
                         const std::array<double, pad_layers>& to_model, PadSound& pad);

// An envelope stage's 0..1 as seconds: 1 ms to 100 s, logarithmically.
double keygroup_seconds(double value);
// A layer's Volume as dB: its gain coefficient (1 is 0 dB).
double keygroup_decibels(double value);

// The MPC's volume, pan and filter numbers, in .xpm and .xpj alike.
uint8_t mpc_level(double value);     // a pad's or program's Volume (0.7079 is 0 dB, 1 is +6 dB) as Melodee's 0..127
int8_t mpc_pan(double value);        // 0..1 as -50..50
PadFilter mpc_filter(int type);      // its filter table as the pad's types; Off where it has none

} // namespace melodee
