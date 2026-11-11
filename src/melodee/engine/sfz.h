// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/multisample.h"
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

// The SFZ format's text read, after melodee-ios's reader: its headers and
// opcodes as the format's own reference has them, a region's over its
// group's, its master's and its global's. What the sampler has no place for
// is not played and is named. Nothing here reads a sample.
namespace melodee {

struct SfzRegion {
    std::string sample;          // relative to the file's folder, with forward slashes
    SampleZone zone;
    int64_t offset = -1, end = -1; // frames of its file it plays from and to; -1: its start, its end
    bool loop_asked = false;     // it asks for a loop, so a file's own may give it
    bool loop_named = false;     // a mode was named, so a file's own loop is not taken over no_loop
};

// The amp envelope most regions have, in seconds and percent, for the pad.
struct SfzEnvelope {
    std::optional<double> attack, decay, sustain, release;
};

struct SfzRead {
    std::vector<SfzRegion> regions;
    std::set<std::string> ignored; // opcodes asked for and not played
    unsigned left = 0;             // regions left out
    std::set<std::string> left_by; // the opcodes that kept them from a press
    SfzEnvelope envelope;
    double velocity = 100;         // percent: its first region's amp_veltrack
};

// Reads an #include's text; nullopt leaves it out.
using SfzInclude = std::function<std::optional<std::string>(const std::string&)>;
SfzRead read_sfz(const std::string& text, const SfzInclude& include = {});

// Parts of the reader, for tests.
std::optional<int> sfz_note(const std::string& text); // a number or a name: c4 is 60, c#4, db4
std::string sfz_uncommented(const std::string& text);
std::vector<std::pair<std::string, std::string>> sfz_opcodes(const std::string& text);

// A stage's seconds as the pad's 0..100 steps (the sampler's own curves).
int sfz_attack_step(double seconds);
int sfz_decay_step(double seconds);
int sfz_release_step(double seconds);
int sfz_percent_step(double percent);

} // namespace melodee
