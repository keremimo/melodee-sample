// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// When a plug-in may sleep: its processBlock skipped, at no cost, until
// something could make it sound again. An instrument sleeps once it holds no
// key (sustain pedal included), has had nothing in its output above -100 dB
// for half a second, and does not hear the song playing (a plug-in with a
// sequencer of its own plays from Play alone; TRANSPORT off, it never hears
// it). An effect sleeps once its input and output have been as quiet for as
// long, or for the tail it reports when that is longer (so echoes are not
// frozen mid-gap); the song playing does not matter. Any MIDI for it, sound
// in an effect's input, or anything else waiting for it (a CLAP plug-in's
// request_process or request_flush, a parameter value set for it) wakes it
// for that block and counts it quiet again from there, as a note does. All
// Notes Off releases the keys while a held sustain pedal keeps the gate awake. Audio thread only; no allocation.
namespace melodee {

class IdleGate {
public:
    static constexpr float quiet_level = 1e-5f; // -100 dB
    static constexpr double quiet_seconds = 0.5;

    // `rate`: the frames the plug-in's output is counted in (its own rate). `tail_seconds`: an effect's reported
    // tail, held awake for after its last sound (at most ten seconds; an instrument's ringing is heard in its
    // output already, so only an effect's counts).
    void reset(bool is_instrument, double rate, double tail_seconds = 0) {
        instrument = is_instrument;
        set_tail(rate, tail_seconds);
        quiet = 0;
        held.fill(0);
        held_count = 0;
        sustain = 0;
    }
    // Update only the silence threshold. Elapsed quiet time and MIDI state survive a tail change.
    void set_tail(double rate, double seconds) {
        double span = quiet_seconds;
        if (!instrument && seconds > span) span = std::min(seconds, 10.0);
        quiet_needed = uint32_t(std::lround(span * rate));
    }
    // Every MIDI message sent to the plug-in, as sent: keys held and the sustain pedal followed.
    void midi(uint8_t status, uint8_t data1, uint8_t data2) {
        const uint8_t kind = status & 0xf0, channel = status & 0x0f;
        const unsigned key = data1 & 0x7f;
        const uint8_t bit = uint8_t(1u << (key & 7));
        uint8_t& byte = held[size_t(channel) * 16 + key / 8];
        if (kind == 0x90 && data2 > 0) {
            if (!(byte & bit)) { byte |= bit; ++held_count; }
        } else if (kind == 0x80 || kind == 0x90) {
            if (byte & bit) { byte &= uint8_t(~bit); --held_count; }
        } else if (kind == 0xb0 && data1 == 64) {
            sustain = data2 >= 64 ? uint16_t(sustain | (1u << channel)) : uint16_t(sustain & ~(1u << channel));
        } else if (kind == 0xb0 && data1 == 121) { // Reset All Controllers: lift the sustain pedal
            sustain = uint16_t(sustain & ~(1u << channel));
        } else if (kind == 0xb0 && (data1 == 120 || data1 == 123)) {
            if (data1 == 120) sustain = uint16_t(sustain & ~(1u << channel));
            for (unsigned k = 0; k < 16; ++k) {
                held_count -= unsigned(__builtin_popcount(held[size_t(channel) * 16 + k]));
                held[size_t(channel) * 16 + k] = 0;
            }
        }
    }
    // What it played: `frames` of its output, at its own rate.
    void heard(const float* left, const float* right, unsigned frames) {
        if (loud(left, right, frames)) quiet = 0;
        else quiet = std::min<uint32_t>(quiet + frames, quiet_needed);
    }
    // The same by the peak the caller took while passing the samples by, avoiding a second walk over the block.
    void heardPeak(float peak, unsigned frames) {
        if (peak > quiet_level) quiet = 0;
        else quiet = std::min<uint32_t>(quiet + frames, quiet_needed);
    }
    // Before a render: whether to skip it. `waiting`: messages for it since its last render, or anything else it must
    // be run for (its own request to be, a parameter value set); `playing`: the song as it hears it; `input`: an
    // effect's, for this render (device frames).
    bool asleep(bool waiting, bool playing, const float* left, const float* right, unsigned frames) {
        if (waiting) { quiet = 0; return false; }
        if (instrument) {
            if (held_count || sustain || playing) return false;
        } else if (left && right && loud(left, right, frames)) {
            quiet = 0;
            return false;
        }
        return quiet >= quiet_needed;
    }
    unsigned keys_held() const { return held_count; }

private:
    static bool loud(const float* left, const float* right, unsigned frames) {
        for (unsigned f = 0; f < frames; ++f)
            if (std::fabs(left[f]) > quiet_level || std::fabs(right[f]) > quiet_level) return true;
        return false;
    }
    bool instrument = true;
    uint32_t quiet_needed = 22050, quiet = 0;
    std::array<uint8_t, 16 * 16> held {}; // a bit a channel's key
    unsigned held_count = 0;
    uint16_t sustain = 0;                 // a bit a channel: its pedal down
};

} // namespace melodee
