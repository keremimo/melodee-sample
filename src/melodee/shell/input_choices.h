// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/input_config.h"
#include "melodee/model/project.h"
#include <algorithm>
#include <array>
#include <cstdint>

// K1 Source on Record and Input Config: the eight inputs, then each track
// that makes a sound of its own (a synth, the 909, another kit), as the old
// plugins listed their synths after the MPC's inputs. The kit a take goes
// onto is left out (it cannot record itself), and so are empty and MIDI
// tracks. A code is an InputSource below Track, else Track + the track.
namespace melodee {

constexpr unsigned input_track_code = unsigned(InputSource::Track);

inline unsigned input_code(const InputConfig& in) {
    return in.source == InputSource::Track ? input_track_code + in.track % track_count : unsigned(in.source);
}
inline bool sounding_track(const Track& t) { return t.engine != EngineKind::None && t.engine != EngineKind::Midi; }

struct InputChoices {
    std::array<uint8_t, input_track_code + track_count> codes {};
    unsigned count = 0;
    unsigned index_of(unsigned code) const {
        for (unsigned n = 0; n < count; ++n) if (codes[n] == code) return n;
        return 0;
    }
    unsigned code_at(int index) const { return codes[unsigned(std::clamp(index, 0, int(count) - 1))]; }
};

// `selected`: the track in view (a kit there is the take's, so not a source). The code in use stays
// listed even when its track no longer sounds, so the knob shows where it is.
inline InputChoices input_choices(const Project& project, unsigned selected, const InputConfig& in) {
    InputChoices out;
    for (unsigned s = 0; s < input_track_code; ++s) out.codes[out.count++] = uint8_t(s);
    unsigned now = input_code(in);
    bool kit = project.tracks[selected % track_count].engine == EngineKind::Sampler;
    for (unsigned t = 0; t < track_count; ++t) {
        bool usable = sounding_track(project.tracks[t]) && !(kit && t == selected % track_count);
        if (usable || input_track_code + t == now) out.codes[out.count++] = uint8_t(input_track_code + t);
    }
    return out;
}

} // namespace melodee
