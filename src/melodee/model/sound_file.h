// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include <cstdint>
#include <string>
#include <vector>

// A sound of one's own: an engine's values and the track's mod routes under
// a name, one small file each in Melodee/Sounds on the card (".mlds").
namespace melodee {

struct UserSound {
    EngineKind engine = EngineKind::None;
    char name[name_length] {};
    std::array<int16_t, max_params> params {};
    std::array<ModRoute, mod_routes> mods {};
};

std::vector<uint8_t> save_sound(const UserSound& sound);
bool load_sound_file(const std::vector<uint8_t>& data, UserSound& out); // false: not a sound file
UserSound sound_of(const Track& track, const char* name);               // what Save sound keeps

} // namespace melodee
