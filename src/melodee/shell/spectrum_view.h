// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/scope_ring.h"
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

// The mixer's EQ picture (docs/MIXER_PLAN.md step 5): what the channel in hand plays, as a spectrum behind the curve.
// The engine copies that channel's sound after its strip into a ring (EngineHost::set_spectrum_tap); the screen thread
// takes the newest frames each frame it draws, windows them and runs an FFT (engine/fft.h), and reads off bands a
// sixth of an octave wide over 40 Hz - 20 kHz, as the EQ picture's axis. A band rises at once and falls back slowly,
// so the picture reads as a level, not a flicker. A full-scale sine reads 0 dB in its band.
namespace melodee {

constexpr unsigned spectrum_points = 2048; // the FFT: about 43 ms at 48 kHz, bins about 23 Hz apart
constexpr unsigned spectrum_bands = 54;    // 40 Hz - 20 kHz, nine octaves in sixths
constexpr float spectrum_low_hz = 40, spectrum_high_hz = 20000;
constexpr float spectrum_floor_db = -90;
constexpr float spectrum_fall_db_per_s = 36;

// The band `b`'s edges, Hz.
inline float spectrum_band_hz(float b) {
    return spectrum_low_hz * std::pow(spectrum_high_hz / spectrum_low_hz, b / float(spectrum_bands));
}

class SpectrumView {
public:
    SpectrumView();
    // The ring's newest frames at `rate` Hz at `now` ms; nothing new since the last look, the bands fall.
    void update(const ScopeRing& ring, float rate, uint32_t now);
    // Every band to the floor (the picture left).
    void clear();
    const std::array<float, spectrum_bands>& bands() const { return db; }

private:
    std::array<float, spectrum_bands> db {};
    std::vector<float> frames, window;
    std::vector<std::complex<float>> bins, twiddle;
    uint32_t last_written = 0, last_at = 0;
    bool seen = false;
};

} // namespace melodee
