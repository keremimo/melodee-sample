// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/model/sound_file.h"
#include <algorithm>
#include <cstring>

namespace melodee {
namespace {
constexpr char magic[8] {'M', 'L', 'D', 'S', 'N', 'D', '1', 0};
}

std::vector<uint8_t> save_sound(const UserSound& s) {
    std::vector<uint8_t> out(magic, magic + 8);
    auto u8 = [&](unsigned v) { out.push_back(uint8_t(v)); };
    auto i16 = [&](int v) { u8(unsigned(v) & 0xff); u8((unsigned(v) >> 8) & 0xff); };
    u8(unsigned(s.engine));
    unsigned length = unsigned(strnlen(s.name, name_length - 1));
    u8(length);
    out.insert(out.end(), s.name, s.name + length);
    u8(max_params);
    for (auto v : s.params) i16(v);
    u8(mod_routes);
    for (const auto& m : s.mods) { u8(m.source); u8(m.destination); i16(m.amount); }
    return out;
}

bool load_sound_file(const std::vector<uint8_t>& data, UserSound& s) {
    if (data.size() < 10 || std::memcmp(data.data(), magic, 8)) return false;
    size_t at = 8;
    bool ok = true;
    auto u8 = [&]() -> unsigned { if (at >= data.size()) { ok = false; return 0; } return data[at++]; };
    auto i16 = [&]() { unsigned a = u8(); return int(int16_t(uint16_t(a | u8() << 8))); };
    s = UserSound {};
    unsigned engine = u8();
    if (engine == 0 || engine >= unsigned(EngineKind::Count)) return false;
    s.engine = EngineKind(engine);
    unsigned length = u8();
    for (unsigned k = 0; k < length; ++k) { char c = char(u8()); if (k + 1 < name_length) s.name[k] = c; }
    unsigned params = u8();
    for (unsigned k = 0; k < params; ++k) { int v = i16(); if (k < max_params) s.params[k] = int16_t(v); }
    unsigned routes = u8();
    for (unsigned k = 0; k < routes; ++k) {
        ModRoute m {uint8_t(u8()), uint8_t(u8()), int8_t(std::clamp(i16(), -100, 100))};
        if (k < mod_routes) s.mods[k] = m;
    }
    return ok;
}

UserSound sound_of(const Track& track, const char* name) {
    UserSound s;
    s.engine = track.engine;
    std::strncpy(s.name, name, name_length - 1);
    s.params = track.params;
    s.mods = track.mods;
    return s;
}

} // namespace melodee
