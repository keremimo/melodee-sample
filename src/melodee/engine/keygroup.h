// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/sfz.h"
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

// An envelope stage's 0..1 as seconds: 1 ms to 100 s, logarithmically.
double keygroup_seconds(double value);
// A layer's Volume (0..1) as dB: 0.353 is -12, 1 is +6.
double keygroup_decibels(double value);

} // namespace melodee
