// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/command.h"
#include "melodee/engine/effects.h"
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
    uint32_t ticks() const { return tick_count.load(std::memory_order_relaxed); } // clock ticks since Play, 24 a quarter
    int step_of(unsigned track) const { return steps_now[track % track_count].load(std::memory_order_relaxed); } // -1: none
    unsigned pattern_of(unsigned track) const { return patterns_now[track % track_count].load(std::memory_order_relaxed); }
    bool song_playing() const { return in_song.load(std::memory_order_relaxed); }
    unsigned song_bar() const { return bar_now.load(std::memory_order_relaxed); }
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
        uint8_t pattern = 0, queued = 255; // queued: launches on the next bar
        uint32_t start = 0;                // the tick its pattern started on
        int clip = -1;                     // in the song: the clip playing
        bool silent = false;               // in the song: no clip here
        std::array<PendingOff, 32> offs {};
        std::array<int16_t, max_params> base {}; // the track's own values; motion only borrows them
        bool moved = false;                       // motion changed a value since Play
        float buffer[block] {};
        float left[block] {}, right[block] {};
        uint8_t bus = 0;
    };
    void apply(const Command& command);
    void select_engine(Slot& slot, EngineKind kind);
    void tick();       // plays whatever falls on this clock tick, then moves on
    void bar_start();  // launches queued patterns, or follows the song
    double tick_samples(uint32_t at) const;
    void play_motion(unsigned track, Slot& slot, unsigned at);
    void restore(Slot& slot);
    void schedule_off(Slot& slot, int key, int32_t samples);
    void run_offs(Slot& slot, unsigned frames);
    double quarter_tick() const { return 44100.0 * 60.0 / (tempo / 10.0) / ticks_per_quarter; }

    MidiQueue<4096, Command> queue;
    std::array<Slot, track_count> slots;
    DelayPool pool;
    std::array<std::array<Effect, max_slots>, chain_count> fx;
    std::array<float, bus_count> returns {0.7f, 0.7f, 0.7f, 0.7f};
    float bus_l[bus_count][block] {}, bus_r[bus_count][block] {}, master_l[block] {}, master_r[block] {};
    void run_chain(unsigned chain, float* left, float* right, unsigned frames);
    std::unique_ptr<PatternBank> patterns;
    Motion motion;
    Song song;
    bool song_mode = false;
    unsigned song_bar_now = 0;
    uint16_t tempo = 1200;
    uint8_t swing = 50;
    bool running = false;
    uint32_t tick_now = 0;
    double until_tick = 0; // samples until the next tick
    uint32_t random = 0x2545F491u;

    std::atomic<bool> is_playing {false};
    std::atomic<uint32_t> tick_count {0};
    std::array<std::atomic<int>, track_count> steps_now {};
    std::array<std::atomic<unsigned>, track_count> patterns_now {};
    std::atomic<bool> in_song {false};
    std::atomic<unsigned> bar_now {0};
    std::array<std::atomic<float>, track_count> meters {};
    std::atomic<float> output_peak {0};
    std::atomic<unsigned> lost {0};
};

} // namespace melodee
