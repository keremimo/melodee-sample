// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <array>
#include <cstdint>
#include <filesystem>

namespace melodee {

struct CustomGui {
    struct Parameter {
        int index = 0;                  // native plug-in index, before Q-Link ordering
        std::array<char, 48> name {};   // verify the index against the loaded plug-in
    };
    static constexpr unsigned max_parameters = 3;
    bool enabled = false;
    uint32_t accent = 0;
    unsigned count = 0;
    std::array<Parameter, max_parameters> parameters {};
};

CustomGui read_custom_gui(const std::filesystem::path& package);

} // namespace melodee
