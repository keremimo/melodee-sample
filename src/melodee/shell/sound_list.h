// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/engine_info.h"
#include "melodee/shell/ui_state.h"
#include <array>

// The sounds the Sounds page lists for the selected track: every factory
// sound of its engine, the starred ones (Shift + Sample Select stars) or the
// recently loaded ones.
namespace melodee {

constexpr unsigned max_listed = 256;
inline unsigned sound_list(const Track& track, const UiState& ui, std::array<uint16_t, max_listed>& out) {
    const auto& info = engine_info(track.engine);
    unsigned n = 0;
    if (ui.sounds_filter == 0)
        for (unsigned s = 0; s < info.sound_count && n < max_listed; ++s) out[n++] = uint16_t(s);
    else if (ui.sounds_filter == 1)
        for (unsigned s = 0; s < info.sound_count && n < max_listed; ++s) { if (ui.settings.favourite(unsigned(track.engine), s)) out[n++] = uint16_t(s); }
    else if (ui.sounds_filter == 2)
        for (unsigned r = 0; r < ui.recent_count; ++r) if (ui.recent[r] < info.sound_count) out[n++] = ui.recent[r];
    return n;
}

} // namespace melodee
