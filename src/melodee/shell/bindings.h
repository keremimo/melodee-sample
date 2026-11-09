// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include "melodee/shell/ui_state.h"
#include <array>
#include <cstdint>

// What K1-K3 hold on the page in view. Every page states its three cards as
// Bindings; the shell turns pot moves into values through them and the
// pages draw the cards from them, so both always agree.
namespace melodee {

struct Binding {
    enum class Kind : uint8_t {
        None, Param, Level, Pan, Send, Root, Scale, Layout, Engine, Jump, Audition, StepLength, StepVelocity, StepChance,
        ModSource, ModDest, ModAmount // id: the route
    };
    Kind kind = Kind::None;
    int16_t id = 0;      // Param: the parameter
    int min = 0, max = 0, value = 0;
    uint32_t key(unsigned track) const { return uint32_t(kind) << 24 | uint32_t(uint16_t(id)) << 8 | track; }
    bool bound() const { return kind != Kind::None && max > min; }
    float fraction() const { return max > min ? float(value - min) / float(max - min) : 0; }
    // The value a pot at `position` (0..127) asks for.
    int at(int position) const { return min + (position * (max - min) + 63) / 127; }
};

// The cards for a page; `pattern` is the selected track's playing pattern.
std::array<Binding, 3> page_bindings(const Project& project, const UiState& ui, const Pattern& pattern);
// The three parameters the Motion page shows as lanes: those holding motion
// on the track's playing pattern first, then the track's favourites.
std::array<int, 3> motion_lanes(const Project& project, const UiState& ui);
// The track's three favourite parameters, for every page without its own.
std::array<Binding, 3> favourite_bindings(const Track& track);

} // namespace melodee
