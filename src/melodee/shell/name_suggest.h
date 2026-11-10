// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include "melodee/shell/name_entry.h"
#include <array>

// K1 on the Name sheet: ready names for what is being named, the one made
// for it first (a free project name, the sound's own name, the marker's).
namespace melodee {

constexpr unsigned max_suggestions = 10;
inline unsigned name_suggestions(const NameEntry& e, std::array<const char*, max_suggestions>& out) {
    unsigned n = 0;
    if (e.first_suggestion[0]) out[n++] = e.first_suggestion;
    static const char* const projects[] {"Sketch", "Demo", "Beat", "Jam", "Live set", "Idea"};
    static const char* const sounds[] {"BASS", "LEAD", "PAD", "KEYS", "PLUCK", "BRASS", "FX"};
    auto add = [&](const char* const* list, unsigned count) { for (unsigned k = 0; k < count && n < max_suggestions; ++k) out[n++] = list[k]; };
    switch (e.target) {
    case NameTarget::Marker: add(Song::marker_names, Song::marker_name_count); break;
    case NameTarget::Sound: add(sounds, 7); break;
    case NameTarget::None: break;
    default: add(projects, 6); break;
    }
    return n;
}

} // namespace melodee
