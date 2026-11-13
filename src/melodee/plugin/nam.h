// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

// Neural Amp Modeler (docs/MELODEE.md, Plug-ins: Neural Amp Modeler), the
// official plug-in built for the MPC without its editor, an optional plug-in
// beside Melodee's own Amp sim effect. Its Model and Cab parameters pick files
// from two folders under its home. Its parameters are left as the user sets
// them, Slim (an A2 capture's size) too; Melodee only orders its page.
namespace melodee {

inline bool is_nam(const std::string& name) { return name == "NeuralAmpModeler"; }

// The page's order for its parameters, given their names as the plug-in lists
// them: positions in `names`. Model, Cab and Output first, the tone stack
// next, the rest after in the plug-in's order.
std::vector<size_t> nam_page_order(const std::vector<std::string>& names);

// A shorter name for a card, where the plug-in's own is long
// ("NoiseGateActive" is "Gate"); others as they are.
std::string nam_card_name(const std::string& name);

// The folders it lists, under the plug-in home (Melodee/Plugin Data):
// "Neural Amp Modeler/Models" and ".../IRs", made if missing. The rack's
// thread only.
void nam_folders(const std::filesystem::path& home);

} // namespace melodee
