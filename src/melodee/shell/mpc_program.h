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

// MPC programs (.xpm, the XML kind): a keygroup program is one instrument on a
// pad (engine/keygroup.h); a drum program is a kit, loaded with Load Kit onto
// the selected track as an MPC Sample project's is. A drum pad of one layer
// plays its WAV; one of more plays them as an instrument named
// "<program>#<pad>" (an .xpj's "<project>#<track>.<pad>"), read again from the
// program when the pad's sample loads, so its layers stream from the card.
namespace melodee {

// The XML read into the keygroup reader's tree. Empty on success; the MPC 3
// kind (gzip around JSON) is named, not read.
std::string parse_mpc_program(const std::string& text, KeygroupProgram& program);

// A drum program, by its first few kilobytes.
bool is_drum_program(const std::string& path);

// A drum program as a project of one kit track. Empty on success.
std::string import_mpc_program(const std::vector<uint8_t>& file, const std::string& path, Project& project, MpcImport& report);

// "<file>#<which>": the file and which pad, when the path names a pad of an MPC program or project.
bool split_program_pad(const std::string& path, std::string& file, std::string& which);

// Every layered pad of an .xpm or .xpj, by its "which". Empty and `error` set when it cannot be read.
std::map<std::string, KeygroupInstrument> program_pads(const std::string& file, std::string& error);

} // namespace melodee
