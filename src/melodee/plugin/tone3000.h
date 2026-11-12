// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

// TONE3000 (docs/MELODEE.md, Plug-ins: TONE3000) plays each NAM block at an A2
// size of its own: lite or full. The size is not a parameter; each block keeps
// it in presets and in the plug-in's state as "slimSize" (0 lite, the model's
// smallest; 1 full). On the MPC full costs about three times lite: Matchless
// Crunch's one full block takes 70% of a 48 kHz block and goes over budget in
// a quarter of them; lite, 24% and none. So Melodee makes every block lite
// before TONE3000 reads it: its presets on the card before it opens, a
// project's saved state before it is handed over.
namespace melodee {

inline bool is_tone3000(const std::string& name) { return name == "TONE3000"; }

// Every block's size in TONE3000 data (a preset file, or the state it saves:
// JUCE ValueTrees in binary) set to lite in place. How many it changed.
unsigned tone3000_lite(uint8_t* data, size_t size);

// The presets under `home` (.config/TONE3000/Presets and the folders in it)
// made lite on the card; only the changed sizes are written. A file already
// read and not changed since is skipped. How many files it changed. The rack's
// thread only.
unsigned tone3000_lite_presets(const std::filesystem::path& home);

} // namespace melodee
