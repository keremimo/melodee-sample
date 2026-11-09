// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/command.h"
#include "melodee/engine/prophet.h"
#include "melodee/model/project.h"
#include "surface/midi.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

// The audio side of Melodee: sixteen track slots, each with its engine and
// mix strip, a mirror of every pattern, and the sequencer clock. The shell
// sends Commands from the MIDI thread; render() runs on the audio thread and
// never allocates, locks or waits. What the screen needs back (playhead,
// meters) is published as atomics.
namespace melodee {

class EngineHost {
public:
    EngineHost();
    // Before the audio thread starts: the whole project and its patterns.
    void load(const Project& project, const PatternBank& patterns);

    // MIDI thread only. False when the queue is full (the change is lost).
    bool send(const Command& command) {
        if (queue.push(command)) return true;
        lost.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Audio thread: `frames` interleaved stereo S32 frames, after `volume`.
    void render(int32_t* out, unsigned frames, float volume);

    // Feedback, any thread.
    bool playing() const { return is_playing.load(std::memory_order_relaxed); }
    uint32_t position() const { return step_position.load(std::memory_order_relaxed); } // steps since Play
    float meter(unsigned track) const { return meters[track % track_count].load(std::memory_order_relaxed); }
    float peak() const { return output_peak.load(std::memory_order_relaxed); }
    unsigned dropped() const { return lost.load(std::memory_order_relaxed); }

    static constexpr unsigned block = 128;

private:
    struct PendingOff { int16_t key = -1; int32_t samples = 0; };
    struct Slot {
        EngineKind kind = EngineKind::None;
        ProphetEngine prophet;
        Engine* engine = nullptr;
        float gain = 1, pan_left = 0.7071f, pan_right = 0.7071f, send = 0;
        bool mute = false, solo = false;
        uint8_t pattern = 0;
        std::array<PendingOff, 32> offs {};
        std::array<int16_t, max_params> base {}; // the track's own values; motion only borrows them
        bool moved = false;                       // motion changed a value since Play
        float buffer[block] {};
    };
    void apply(const Command& command);
    void select_engine(Slot& slot, EngineKind kind);
    void step(); // plays the step at `position`, then moves on
    void play_motion(unsigned track, Slot& slot, unsigned at);
    void restore(Slot& slot);
    void schedule_off(Slot& slot, int key, int32_t samples);
    void run_offs(Slot& slot, unsigned frames);
    double samples_per_step() const { return 44100.0 * 60.0 / (tempo / 10.0) / 4.0; }

    MidiQueue<4096, Command> queue;
    std::array<Slot, track_count> slots;
    std::unique_ptr<PatternBank> patterns;
    Motion motion;
    uint16_t tempo = 1200;
    uint8_t swing = 50;
    bool running = false;
    uint32_t position_now = 0;
    double until_step = 0; // samples until the next step plays
    uint32_t random = 0x2545F491u;

    std::atomic<bool> is_playing {false};
    std::atomic<uint32_t> step_position {0};
    std::array<std::atomic<float>, track_count> meters {};
    std::atomic<float> output_peak {0};
    std::atomic<unsigned> lost {0};
};

} // namespace melodee
