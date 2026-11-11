// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/keygroup.h"
#include "melodee/model/project.h"
#include <map>
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

// A pad of more than one layer plays them as an instrument named "<project>#<track>.<pad>"
// (shell/mpc_program.h): the project's such pads, by "<track>.<pad>", their layers as a drum program's tags.
std::map<std::string, KeygroupInstrument> mpc_project_pads(const std::vector<uint8_t>& file, const std::string& path);

// An MPC 3 program in JSON (an .xty track's, or a gzip .xpm) as the tree .xpm programs read into
// (engine/keygroup.h), drum or keygroup alike. Empty on success.
std::string read_mpc_json_program(const std::vector<uint8_t>& file, const std::string& path, KeygroupProgram& program);

// The sample rate of a WAV file; 0 when it cannot tell.
unsigned mpc_wav_rate(const std::string& path);

} // namespace melodee
