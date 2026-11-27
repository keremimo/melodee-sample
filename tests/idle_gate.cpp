// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// When a plug-in may sleep (plugin/idle_gate.h): held keys, the sustain pedal and the controllers that release
// them (a stranded pedal must not keep a silent plug-in awake for ever), heardPeak(), and an effect's reported
// tail holding it awake past the plain quiet span. Built and run by tests/test_plugin_hosts.py; no JUCE, no objects.
#include "melodee/plugin/idle_gate.h"

#include <cstdio>

using namespace melodee;

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

namespace {
constexpr double rate = 48000;
float zeros[128] {};

// `seconds` of silence heard, in blocks.
void quiet(IdleGate& gate, double seconds) {
    for (double done = 0; done < seconds; done += 128.0 / rate) gate.heard(zeros, zeros, 128);
}
void cc(IdleGate& gate, uint8_t channel, uint8_t controller, uint8_t value) {
    gate.midi(uint8_t(0xb0 | channel), controller, value);
}
} // namespace

int main() {
    // The pedal down keeps an instrument awake; All Sound Off or Reset All Controllers
    // lets it sleep once it has been quiet long enough.
    for (uint8_t release : {uint8_t(120), uint8_t(121)}) {
        IdleGate gate;
        gate.reset(true, rate);
        gate.midi(0x90, 60, 100);
        gate.midi(0x80, 60, 0);
        cc(gate, 0, 64, 127);
        quiet(gate, 1.0);
        CHECK(!gate.asleep(false, false, nullptr, nullptr, 0)); // the pedal is still down
        cc(gate, 0, release, 0);
        CHECK(gate.keys_held() == 0);
        CHECK(gate.asleep(false, false, nullptr, nullptr, 0));
    }
    { // Ordinary All Notes Off releases held keys, while sustain remains down until CC64 rises.
        IdleGate gate;
        gate.reset(true, rate);
        gate.midi(0x90, 60, 100);
        cc(gate, 0, 64, 127);
        gate.midi(0x80, 60, 0);
        gate.midi(0x90, 62, 100);
        gate.set_tail(rate, 2.0);
        CHECK(gate.keys_held() == 1);
        cc(gate, 0, 123, 0);
        quiet(gate, 1.0);
        CHECK(gate.keys_held() == 0);
        CHECK(!gate.asleep(false, false, nullptr, nullptr, 0));
        cc(gate, 0, 64, 0);
        CHECK(gate.asleep(false, false, nullptr, nullptr, 0));
    }
    { // The pedal on another channel is not this one's release.
        IdleGate gate;
        gate.reset(true, rate);
        cc(gate, 1, 64, 127);
        cc(gate, 0, 121, 0);
        quiet(gate, 1.0);
        CHECK(!gate.asleep(false, false, nullptr, nullptr, 0));
    }
    { // A key still held, the song playing, or work waiting: awake however quiet.
        IdleGate gate;
        gate.reset(true, rate);
        gate.midi(0x90, 60, 100);
        quiet(gate, 1.0);
        CHECK(!gate.asleep(false, false, nullptr, nullptr, 0));
        gate.midi(0x80, 60, 0);
        CHECK(gate.asleep(false, false, nullptr, nullptr, 0));
        CHECK(!gate.asleep(false, true, nullptr, nullptr, 0));
        CHECK(!gate.asleep(true, false, nullptr, nullptr, 0));
    }
    { // heardPeak() matches heard(): resets on signal above quiet_level, accumulates below.
        IdleGate gate;
        gate.reset(true, rate);
        for (double done = 0; done < 1.0; done += 128.0 / rate) gate.heardPeak(0.0f, 128);
        CHECK(gate.asleep(false, false, nullptr, nullptr, 0));
        gate.heardPeak(IdleGate::quiet_level * 2.0f, 128);
        CHECK(!gate.asleep(false, false, nullptr, nullptr, 0));
        for (double done = 0; done < 1.0; done += 128.0 / rate) gate.heardPeak(IdleGate::quiet_level * 0.5f, 128);
        CHECK(gate.asleep(false, false, nullptr, nullptr, 0));
    }
    { // An effect with a two-second tail stays awake for it after its last sound, then sleeps.
        IdleGate gate;
        gate.reset(false, rate, 2.0);
        float loud[128];
        for (auto& s : loud) s = 0.5f;
        gate.heard(loud, loud, 128);
        quiet(gate, 1.0);
        CHECK(!gate.asleep(false, false, zeros, zeros, 128)); // past the plain half second, inside the tail
        quiet(gate, 1.5);
        CHECK(gate.asleep(false, false, zeros, zeros, 128));
        CHECK(!gate.asleep(false, false, loud, loud, 128)); // sound at its input wakes it
    }
    { // Grow the tail of a quiet effect, then shorten it without losing the elapsed quiet time.
        IdleGate gate;
        gate.reset(false, rate);
        quiet(gate, 0.5);
        CHECK(gate.asleep(false, false, zeros, zeros, 128));
        gate.set_tail(rate, 2.0);
        CHECK(!gate.asleep(false, false, zeros, zeros, 128));
        quiet(gate, 0.75);
        CHECK(!gate.asleep(false, false, zeros, zeros, 128));
        gate.set_tail(rate, 1.0);
        CHECK(gate.asleep(false, false, zeros, zeros, 128));
    }
    { // The same effect told no tail sleeps after the plain quiet span, as before.
        IdleGate gate;
        gate.reset(false, rate);
        quiet(gate, 1.0);
        CHECK(gate.asleep(false, false, zeros, zeros, 128));
    }
    if (failures) return 1;
    std::puts("idle ok");
    return 0;
}
