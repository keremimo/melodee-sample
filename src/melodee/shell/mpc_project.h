// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include <cstdint>
#include <string>
#include <vector>

// The MPC Sample's own projects (.xpj: gzip around five header lines and a
// JSON document, the samples in "<name>_[ProjectData]" beside it) read into a
// Melodee project. The file is never written: Save makes a .mldp. Each Drum
// track becomes a kit track, each sequence that numbered pattern on all of them.
namespace melodee {

struct MpcImport {
    unsigned tracks = 0, sequences = 0, pads = 0, notes = 0;
    unsigned over_limit = 0; // tracks, sequences or pads Melodee has no room for
    unsigned missing = 0;    // pads whose file is not in the project's data folder
};

bool is_mpc_project(const std::string& path);

// Empty on success. A file of 10 MB of JSON takes a while: never on the MIDI or audio thread.
std::string import_mpc_project(const std::vector<uint8_t>& file, const std::string& path, Project& project, PatternBank& patterns, MpcImport& report);

} // namespace melodee
