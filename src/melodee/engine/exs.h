// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/sfz.h"
#include <functional>
#include <string>

// A Logic EXS24 instrument (.exs, Logic's Sampler too) as the sampler's zones:
// the same read an .sfz makes, so its samples are laid out on the card and
// streamed as an SFZ's are. The file is a run of blocks, little- or
// big-endian alike: the instrument, its zones, its groups, its samples and
// its parameters. A zone is a key and velocity range playing one sample; a
// group gives its zones a level, a pan, a choke, limits on their ranges and
// how they are picked (round robin, keyswitch). Nothing here reads a file:
// where a sample is on this card is the shell's to find (ExsFind).
namespace melodee {

// A sample the instrument names: its file name and the folder it was in on the
// machine that saved it. The path to play, relative to the instrument's
// folder with forward slashes, or empty where it is not found.
using ExsFind = std::function<std::string(const std::string& name, const std::string& folder)>;

// The file's bytes as zones. `error` names why where it is not an EXS file at all.
SfzRead read_exs(const std::string& bytes, const ExsFind& find = {}, std::string* error = nullptr);

// Parts of the reader, for tests.
bool exs_magic(const std::string& bytes);  // its first block's "TBOS"/"SOBT" (or "JBOS"/"SOBJ")
double exs_seconds(int value);             // an envelope stage's 0..127 as seconds

} // namespace melodee
