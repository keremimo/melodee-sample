// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// A pattern's feel (roadmap P1, model/pattern.h Feel): Time Correct heard
// live. The notes stay as written; the audio thread plays feel_pattern() of
// the pattern, sent again whenever a value changes, so turning one back takes
// it back. A groove is taken from another pattern's notes when picked (a copy:
// that pattern may change after).
#include "melodee/shell/context.h"
#include <algorithm>

namespace melodee {

void Context::set_feel(Binding::Kind kind, int value) {
    using Kind = Binding::Kind;
    auto& f = pattern().feel;
    Feel was = f;
    switch (kind) {
    case Kind::FeelStrength: f.strength = uint8_t(std::clamp(value, 0, 100)); break;
    case Kind::FeelSwing: f.swing = uint8_t(std::clamp(value, 50, 75)); break;
    case Kind::FeelHumanize: f.humanize = uint8_t(std::clamp(value, 0, 100)); break;
    case Kind::FeelVelocity: f.humanize_velocity = uint8_t(std::clamp(value, 0, 100)); break;
    case Kind::GrooveAmount: f.groove_amount = uint8_t(std::clamp(value, 0, 100)); break;
    case Kind::GrooveFrom: {
        int from = std::clamp(value, 0, int(track_count * pattern_count));
        f.groove_from = uint16_t(from);
        if (!from) { f.groove_offsets.clear(); f.groove_levels.clear(); break; }
        const auto& source = patterns.at[unsigned(from - 1) / pattern_count][unsigned(from - 1) % pattern_count];
        groove_of(source, f);
        if (f.groove_offsets.empty()) notice("Groove: no notes in T" + std::to_string((from - 1) / int(pattern_count) + 1) + " P" +
                                             std::to_string((from - 1) % int(pattern_count) + 1));
        break;
    }
    default: return;
    }
    if (f != was) sync_pattern(ui.track, track().pattern);
}

} // namespace melodee
