// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/keygroup.h"
#include "melodee/model/project.h"
#include "melodee/shell/mpc_project.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// MPC programs (.xpm, and the MPC 3's .xty tracks): a keygroup program is one instrument on a
// pad (engine/keygroup.h); a drum program is a kit, loaded with Load Kit onto
// the selected track as an MPC Sample project's is. A drum pad of one layer
// plays its WAV; one of more plays them as an instrument named
// "<program>#<pad>" (an .xpj's "<project>#<track>.<pad>"), read again from the
// program when the pad's sample loads, so its layers stream from the card.
namespace melodee {

// An .xpm's XML read into the keygroup reader's tree. Empty on success.
std::string parse_mpc_program(const std::string& text, KeygroupProgram& program);
// Any MPC program's bytes, read into that tree: an .xpm's XML, or the MPC 3's
// gzip JSON (an .xty track, or an .xpm saved by MPC 3). Empty on success.
std::string read_program(const std::string& path, const std::string& bytes, KeygroupProgram& program);

// An .xpm or an .xty: an MPC program, by its name.
bool is_mpc_program(const std::string& path);
// A drum program (a kit), by its first few kilobytes, or the whole of an MPC 3 one.
bool is_drum_program(const std::string& path);
// The program's preview, as MPC expansions keep one: "[Previews]/<program file>.mp3" beside it; empty when none.
std::string program_preview(const std::string& path);

// A drum program as a project of one kit track. Empty on success.
std::string import_mpc_program(const std::vector<uint8_t>& file, const std::string& path, Project& project, MpcImport& report);

// "<file>#<which>": the file and which pad, when the path names a pad of an MPC program or project.
bool split_program_pad(const std::string& path, std::string& file, std::string& which);

// Roadmap P3: each pad playing a program pad's layers as an instrument ("<file>#<which>") whose two to four layers'
// files are all found, and fit in the project's free slots (the instrument's own slot given back), takes them as its
// own layers (engine/keygroup.h keygroup_pad_layers()); the rest stream as before. Returns how many pads moved.
unsigned layers_from_programs(Project& project);

// Every layered pad of an .xpm or .xpj, by its "which". Empty and `error` set when it cannot be read.
std::map<std::string, KeygroupInstrument> program_pads(const std::string& file, std::string& error);

} // namespace melodee
