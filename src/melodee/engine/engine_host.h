// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/arp.h"
#include "melodee/engine/command.h"
#include "melodee/engine/cz.h"
#include "melodee/engine/fm6.h"
#include "melodee/engine/kit909.h"
#include "melodee/engine/sid.h"
#include "melodee/engine/effects.h"
#include "melodee/engine/prophet.h"
#include "melodee/engine/punch.h"
#include "melodee/engine/sampler.h"
#include "melodee/engine/take_recorder.h"
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

// A whole project for the audio thread to switch to at once (a load or a new
// project). The MIDI thread makes it, the audio thread hands it back.
struct ProjectPack {
    Project project;
    PatternBank patterns;
};

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

    // Audio thread: `frames` interleaved stereo S32 frames, after `volume`;
    // `input` the capture block (silent when there is none), for Record.
    void render(int32_t* out, unsigned frames, float volume, const int32_t* input = nullptr);
    TakeRecorder& recorder() { return take; }
    const TakeRecorder& recorder() const { return take; }
    unsigned record_source() const { return source_now.load(std::memory_order_relaxed); }

    // Feedback, any thread.
    bool playing() const { return is_playing.load(std::memory_order_relaxed); }
    uint32_t ticks() const { return tick_count.load(std::memory_order_relaxed); } // clock ticks since Play, 24 a quarter
    int step_of(unsigned track) const { return steps_now[track % track_count].load(std::memory_order_relaxed); } // -1: none
    unsigned pattern_of(unsigned track) const { return patterns_now[track % track_count].load(std::memory_order_relaxed); }
    bool punching() const { return punch_on.load(std::memory_order_relaxed); }
    bool song_playing() const { return in_song.load(std::memory_order_relaxed); }
    unsigned song_bar() const { return bar_now.load(std::memory_order_relaxed); }
    float meter(unsigned track) const { return meters[track % track_count].load(std::memory_order_relaxed); }
    float peak() const { return output_peak.load(std::memory_order_relaxed); }
    unsigned dropped() const { return lost.load(std::memory_order_relaxed); }

    // Service thread: a loaded sample for slot `slot` (nullptr empties it). The
    // audio thread swaps it in at its next block and hands the one it replaced
    // back through take_retired(), to be freed there.
    bool offer_sample(unsigned slot, const SampleData* sample) { return offers.push({uint16_t(slot), sample}); }
    bool take_retired(const SampleData*& sample) { return retired.pop(sample); }
    // MIDI thread: a ProjectPack the audio thread has finished with, to free.
    bool take_done_pack(const ProjectPack*& pack) { return done_packs.pop(pack); }

    static constexpr unsigned block = 128;

private:
    struct PendingOff { int16_t key = -1; int32_t samples = 0; };
    struct Slot {
        EngineKind kind = EngineKind::None;
        ProphetEngine prophet;
        SamplerEngine sampler;
        Kit909Engine kit;
        Fm6Engine fm;
        SidEngine sid;
        CzEngine cz;
        Engine* engine = nullptr;
        float gain = 1, pan_left = 0.7071f, pan_right = 0.7071f, send = 0;
        bool mute = false, solo = false;
        uint8_t pattern = 0, queued = 255; // queued: launches on the next bar
        uint32_t start = 0;                // the tick its pattern started on
        int clip = -1;                     // in the song: the clip playing
        bool silent = false;               // in the song: no clip here
        std::array<PendingOff, 32> offs {};
        std::array<int16_t, max_params> base {}; // the track's own values; motion only borrows them
        std::array<std::array<int16_t, effect_params>, max_slots> fx_base {}; // and its effects'
        bool fx_moved = false;
        bool moved = false;                       // motion changed a value since Play
        float buffer[block] {};
        float left[block] {}, right[block] {};
        uint8_t bus = 0;
        Arp arp;
    };
    void apply(const Command& command);
    void select_engine(Slot& slot, EngineKind kind);
    void tick();       // plays whatever falls on this clock tick, then moves on
    void arp_tick();   // the arps' own clock, running whether the song plays or not
    void bar_start();  // launches queued patterns, or follows the song
    double tick_samples(uint32_t at) const;
    void play_motion(unsigned track, Slot& slot, unsigned at);
    void restore(Slot& slot);
    void schedule_off(Slot& slot, int key, int32_t samples);
    void run_offs(Slot& slot, unsigned frames);
    double quarter_tick() const { return 44100.0 * 60.0 / (tempo / 10.0) / ticks_per_quarter; }

    MidiQueue<4096, Command> queue;
    struct Offer { uint16_t slot = 0; const SampleData* sample = nullptr; };
    MidiQueue<64, Offer> offers;
    MidiQueue<128, const SampleData*> retired;
    MidiQueue<8, const ProjectPack*> done_packs;
    SamplerEngine::Table samples {};
    TakeRecorder take;
    std::atomic<unsigned> source_now {0};
    float record_gain = 0;
    double until_arp = 0;
    uint32_t arp_ticks = 0;
    bool click = false;
    float click_level = 0.4f, click_env = 0, click_phase = 0, click_step = 0;
    float record_l[block] {}, record_r[block] {};
    // Punch: one on the master or a track, one on a single sampler pad. A
    // press while playing waits for the next step of its snap grid.
    struct PendingPunch { bool waiting = false; uint8_t target = 255; PunchKind kind {}; float depth = 1; unsigned snap = 2, tail = 0; };
    Punch punch, pad_punch;
    uint8_t punch_target = 255;
    int pad_punch_track = -1, pad_punch_pad = -1;
    PendingPunch pending;
    bool song_recording = false, punch_from_song = false, live_punch = false;
    uint32_t song_punch_until = 0; // the tick a song punch lets go
    void play_song_punches();      // on each sixteenth of the song: punches written there
    void start_punch(const PendingPunch& p);
    double bar_length() const { return quarter_tick() * ticks_per_bar; }
    std::array<Slot, track_count> slots;
    DelayPool pool;
    std::array<std::array<Effect, max_slots>, chain_count> fx;
    std::unique_ptr<std::array<SamplerEngine::PadEffects, track_count>> pad_fx; // each sampler pad's chain
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
    std::atomic<bool> punch_on {false};
};

} // namespace melodee
