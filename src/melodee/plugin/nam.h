// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

// Neural Amp Modeler (docs/MELODEE.md, Plug-ins: Neural Amp Modeler), the
// official plug-in built for the MPC without its editor. Its Model and Cab
// parameters pick files from two folders under its home; Slim is the size an
// A2 capture plays at, 0 the smallest (lite). Full costs about four times lite
// on the MPC, so Melodee keeps Slim at 0 and off the page, as it keeps
// TONE3000's blocks lite (plugin/tone3000.h).
namespace melodee {

inline bool is_nam(const std::string& name) { return name == "NeuralAmpModeler"; }

// The parameter Melodee holds at 0.
inline const char* nam_slim_name() { return "Slim"; }

// The page's order for its parameters, given their names as the plug-in lists
// them: positions in `names`. Model, Cab and Output first, the tone stack
// next, the rest after in the plug-in's order; Slim left out.
std::vector<size_t> nam_page_order(const std::vector<std::string>& names);

// A shorter name for a card, where the plug-in's own is long
// ("NoiseGateActive" is "Gate"); others as they are.
std::string nam_card_name(const std::string& name);

// The folders it lists, under the plug-in home (Melodee/Plugin Data):
// "Neural Amp Modeler/Models" and ".../IRs", made if missing. The rack's
// thread only.
void nam_folders(const std::filesystem::path& home);

} // namespace melodee
