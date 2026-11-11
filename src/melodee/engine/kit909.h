// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/dsp.h"
#include "melodee/engine/engine.h"
#include "melodee/engine/engine_info.h"
#include <array>

// 909: a drum machine in the shape of the classic analogue kit, written for
// sample-base from simple parts: pitched sines with a falling pitch (kick,
// toms), tone plus filtered noise (snare, clap), resonators (rim) and six
// detuned square waves through filters (hats and cymbals). Pads 1-11 (notes
// 36-46): kick, snare, clap, rim, low, mid and high tom, closed hat, open
// hat, crash, ride. The closed hat chokes the open one. Audio thread only.
namespace melodee {

namespace kit909 {
enum Drum : uint8_t { Kick, Snare, Clap, Rim, LowTom, MidTom, HighTom, ClosedHat, OpenHat, Crash, Ride, Drums };
enum Param : int8_t {
    KickTune, KickDecay, KickAttack, SnareTune, SnareTone, SnareSnappy, ClapDecay, RimLevel,
    LowTune, MidTune, HighTune, TomDecay, ClosedDecay, OpenDecay, HatTone, CrashTune, CrashDecay, RideTune, RideDecay,
    KickLevel, SnareLevel, ClapLevel, TomLevel, HatLevel, CymbalLevel, Drive,
    Count
};
const char* drum_name(unsigned drum);
const EngineInfo& info();
}

class Kit909Engine final : public Engine {
public:
    Kit909Engine();
    void note_on(int key, int velocity) override;
    void set_accent(float gain) override { accent = gain; }
    void note_off(int) override {}
    void all_notes_off() override {}
    void silence() override;
    void set_param(int id, int value) override;
    void render(float* out, unsigned frames) override;
    bool sounding() const override;

private:
    struct Voice {
        bool on = false;
        float level = 0, velocity = 0, phase = 0, phase2 = 0, pitch_env = 0, amp = 0, noise_amp = 0, age = 0;
        std::array<float, 6> phases {};
        float lp = 0, bp = 0, hp_last = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0;
    };
    float drum(unsigned d, Voice& v);
    float metal(Voice& v, float base);
    std::array<Voice, kit909::Drums> voices {};
    std::array<int, kit909::Count> p {};
    dsp::Noise noise;
    float accent = 1; // the sequencer's, for the drums started next (set_accent)
};

} // namespace melodee
