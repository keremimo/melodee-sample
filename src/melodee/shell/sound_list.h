// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/engine_info.h"
#include "melodee/shell/ui_state.h"
#include <array>
#include <string>
#include <vector>

// The sounds the Sounds page lists for the selected track: every factory
// sound of its engine and then the sounds of one's own (numbered after them),
// the starred ones (Shift + Sample Select stars) or the recently loaded ones.
namespace melodee {

constexpr unsigned max_listed = 256;
inline unsigned sound_list(const Track& track, const UiState& ui, std::array<uint16_t, max_listed>& out) {
    const auto& info = engine_info(track.engine);
    unsigned n = 0;
    unsigned all = info.sound_count + ui.user_sound_count; // a sound of one's own follows the factory ones
    if (ui.sounds_filter == 0)
        for (unsigned s = 0; s < all && n < max_listed; ++s) out[n++] = uint16_t(s);
    else if (ui.sounds_filter == 1)
        for (unsigned s = 0; s < all && n < max_listed; ++s) { if (ui.settings.favourite(unsigned(track.engine), s)) out[n++] = uint16_t(s); }
    else if (ui.sounds_filter == 2)
        for (unsigned r = 0; r < ui.recent_count; ++r) if (ui.recent[r] < all) out[n++] = ui.recent[r];
    return n;
}

// The name of a listed sound: a factory one's from the engine, one of one's own from `own` (the names of the
// engine's sounds of one's own, in Context::user_sound()'s order). nullptr: `own` does not hold it (yet). The
// engine's table ends at sound_count: a sound of one's own read from it reads past its end.
inline const char* sound_name(const EngineInfo& info, unsigned sound, const std::vector<std::string>& own) {
    if (sound < info.sound_count) return info.sounds[sound].name;
    unsigned k = sound - info.sound_count;
    return k < own.size() ? own[k].c_str() : nullptr;
}

} // namespace melodee
