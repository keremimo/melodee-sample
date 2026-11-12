// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/plugin/nam.h"
#include <algorithm>
#include <array>
#include <system_error>
#include <utility>

namespace melodee {
namespace fs = std::filesystem;

std::vector<size_t> nam_page_order(const std::vector<std::string>& names) {
    static const std::array<const char*, 12> first {"Model", "Cab", "Output", "Bass", "Middle", "Treble",
                                                     "Input", "Threshold", "NoiseGateActive", "ToneStack", "IRToggle", "OutputMode"};
    std::vector<size_t> out;
    std::vector<bool> taken(names.size(), false);
    for (const char* name : first)
        for (size_t i = 0; i < names.size(); ++i)
            if (!taken[i] && names[i] == name) { out.push_back(i); taken[i] = true; break; }
    for (size_t i = 0; i < names.size(); ++i)
        if (!taken[i] && names[i] != nam_slim_name()) out.push_back(i);
    return out;
}

std::string nam_card_name(const std::string& name) {
    static const std::array<std::pair<const char*, const char*>, 6> shorter {{
        {"NoiseGateActive", "Gate"}, {"ToneStack", "Tone"}, {"IRToggle", "Cab on"}, {"CalibrateInput", "Calibrate"},
        {"InputCalibrationLevel", "Cal level"}, {"OutputMode", "Out mode"}}};
    for (const auto& [own, card] : shorter)
        if (name == own) return card;
    return name;
}

void nam_folders(const fs::path& home) {
    if (home.empty()) return;
    std::error_code error;
    fs::create_directories(home / "Neural Amp Modeler" / "Models", error);
    fs::create_directories(home / "Neural Amp Modeler" / "IRs", error);
}

} // namespace melodee
