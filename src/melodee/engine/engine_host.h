// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/arp.h"
#include "melodee/engine/command.h"
#include "melodee/engine/cz.h"
#include "melodee/engine/fm6.h"
#include "melodee/engine/kit909.h"
#include "melodee/engine/master_comp.h"
#include "melodee/engine/sid.h"
#include "melodee/engine/effects.h"
#include "melodee/engine/prophet.h"
#include "melodee/engine/punch.h"
#include "melodee/engine/sampler.h"
#include "melodee/engine/take_recorder.h"
#include "melodee/engine/bounce.h"
#include "melodee/engine/recall_ring.h"
#include "melodee/model/input_config.h"
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

// One track's sixteen patterns as the audio thread plays them: copies made
// on the MIDI thread, swapped in by pointer and handed back to be freed
// there, so the audio thread never allocates or frees.
using PlayPatterns = std::array<Pattern*, pattern_count>;
PlayPatterns copy_patterns(const std::array<Pattern, pattern_count>& patterns);
void free_patterns(PlayPatterns& patterns);

// A whole project for the audio thread to switch to at once (a load or a new
// project). The MIDI thread makes it, the audio thread swaps its patterns
// for the ones it played and hands it back, to be freed with those.
struct ProjectPack {
    ProjectPack(const Project& project, const PatternBank& bank);
    ~ProjectPack();
    ProjectPack(const ProjectPack&) = delete;
    ProjectPack& operator=(const ProjectPack&) = delete;
    Project project;
    std::array<PlayPatterns, track_count> patterns {};
};

// One track for the audio thread to take whole, without stopping (a track
// copy, or its undo): the track, its sixteen patterns and the project's
// motion after the change. Made and freed like a ProjectPack.
struct TrackPack {
    TrackPack(unsigned track, const Track& value, const std::array<Pattern, pattern_count>& patterns, const Motion& motion);
    ~TrackPack();
    TrackPack(const TrackPack&) = delete;
    TrackPack& operator=(const TrackPack&) = delete;
    uint8_t track = 0;
    Track value;
    PlayPatterns patterns {};
    Motion motion;
};

class EngineHost {
public:
    EngineHost();
    ~EngineHost();
    EngineHost(const EngineHost&) = delete;
    EngineHost& operator=(const EngineHost&) = delete;
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
    // `stems`, when given, gets each track's stem: `track_count` rows of
    // `block` floats, the track after its chain and fader as you hear it,
    // summed to mono with its pan law taken back out, before the buses, the
    // master chain and main volume. Muted (or soloed-out) tracks are silent.
    // `usb`, when given, is what a computer plays over USB-C: `frames`
    // interleaved stereo floats, for the USB sources.
    void render(int32_t* out, unsigned frames, float volume, const int32_t* input = nullptr, float* stems = nullptr,
                const float* usb = nullptr);
    TakeRecorder& recorder() { return take; }
    const TakeRecorder& recorder() const { return take; }
    RecallRing& recall() { return recall_ring; }
    const RecallRing& recall() const { return recall_ring; }
    Bounce& bounce() { return render_out; }
    const Bounce& bounce() const { return render_out; }
    unsigned record_source() const { return source_now.load(std::memory_order_relaxed); } // InputSource
    bool monitoring() const { return monitor_on.load(std::memory_order_relaxed); }

    // Feedback, any thread.
    bool playing() const { return is_playing.load(std::memory_order_relaxed); }
    uint32_t ticks() const { return tick_count.load(std::memory_order_relaxed); } // clock ticks since Play, 96 a quarter
    // Where a track is in its pattern, in ticks; -1 while it plays none.
    int position_of(unsigned track) const { return positions_now[track % track_count].load(std::memory_order_relaxed); }
    unsigned count_in() const { return count_beat.load(std::memory_order_relaxed); } // the count-in's beat now, 1..; 0 none
    unsigned tempo_now() const { return tempo_shown.load(std::memory_order_relaxed); } // tenths of a BPM, as it plays
    unsigned pattern_of(unsigned track) const { return patterns_now[track % track_count].load(std::memory_order_relaxed); }
    bool punching() const { return punch_on.load(std::memory_order_relaxed); }
    float compression() const { return comp.reduction_db(); } // dB the master compressor takes off now
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
    bool take_done_track(const TrackPack*& pack) { return done_tracks.pop(pack); }
    // MIDI thread: a pattern copy the audio thread played until a newer one came (PatternData), to free.
    bool take_retired_pattern(Pattern*& pattern) { return retired_patterns.pop(pattern); }

    static constexpr unsigned block = 128;

private:
    struct PendingOff { int16_t key = -1; int32_t samples = 0; };
    struct PendingOn { int16_t key = -1, velocity = 0, from = -1; int32_t samples = 0, length = 0; }; // an arp note swung late
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
        PendingOn arp_late;
        int arp_played = -1, arp_from = -1; // the arp's last note and the held key it came from
        // Notes Seq Record just wrote ahead of the playhead (Rec Quantize), already heard live: skipped once.
        struct Skip { int16_t key = -1; uint8_t pattern = 0; uint32_t tick = 0, until = 0; };
        std::array<Skip, 8> skips {};
    };
    void load_track(unsigned t, const Track& track); // a whole track's sound, kit, mix and effects
    void load_state(const Project& project);         // all but the patterns
    void run_late(Slot& slot, unsigned frames);       // a swung arp note, when its time comes
    void arp_note(Slot& slot, int key, int velocity, int from, int32_t length); // with its pad's pressure
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
    double tick_length() const { return 44100.0 * 60.0 / (tempo / 10.0) / ticks_per_quarter; } // samples a clock tick
    double arp_tick_length() const { return tick_length() * (ticks_per_quarter / arp_ticks_per_quarter); }
    void follow_tempo();  // the tempo now: the global one, or the lead pattern's own (BPM per pattern)
    bool skipped(Slot& slot, unsigned key, uint32_t tick);

    MidiQueue<4096, Command> queue;
    struct Offer { uint16_t slot = 0; const SampleData* sample = nullptr; };
    MidiQueue<64, Offer> offers;
    MidiQueue<128, const SampleData*> retired;
    MidiQueue<8, const ProjectPack*> done_packs;
    MidiQueue<8, const TrackPack*> done_tracks;
    MidiQueue<1024, Pattern*> retired_patterns;
    std::array<Pattern*, 64> limbo {}; // retired ones the queue had no room for yet
    void retire(Pattern* pattern);
    SamplerEngine::Table samples {};
    TakeRecorder take;
    RecallRing recall_ring; // Audio Recall: the source, always
    Bounce render_out;      // Resample and the song's mixdown
    std::atomic<unsigned> source_now {1};
    std::atomic<bool> monitor_on {false};
    float record_gain = 1;  // linear
    int cue_track = -1;     // a take cued for Rec length Seq starts where this track's pattern comes round
    uint32_t bounce_ticks = 0; // a pattern render ends after this many ticks; 0 with bounce_song
    bool bounce_song = false;  // the song's render: to its end, then its tail
    unsigned block_at = 0;  // frames of this block rendered when tick() runs
    void select_input(const int32_t* input, const float* usb, unsigned frames); // into record_l / record_r
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
    double bar_length() const { return tick_length() * bar; }
    std::array<Slot, track_count> slots;
    DelayPool pool;
    std::array<std::array<Effect, max_slots>, chain_count> fx;
    std::unique_ptr<std::array<SamplerEngine::PadEffects, track_count>> pad_fx; // each sampler pad's chain
    std::array<float, bus_count> returns {0.7f, 0.7f, 0.7f, 0.7f};
    float bus_l[bus_count][block] {}, bus_r[bus_count][block] {}, master_l[block] {}, master_r[block] {};
    void run_chain(unsigned chain, float* left, float* right, unsigned frames);
    // Knob FX (parity phase 7): one effect the chosen tracks and kit pads go
    // through on their way to the master (`knob_in`, its twin, on the input
    // during a take with Rec Input FX); it rings on a while after it stops.
    Effect knob, knob_in;
    bool knob_heard = false, knob_rec = false;
    uint16_t knob_tracks = 0;
    std::array<std::array<uint64_t, 2>, track_count> knob_pads {};
    uint32_t knob_tail = 0;
    float knob_l[block] {}, knob_r[block] {};
    void knob_targets(); // hands each kit its pads (none while Knob FX is not heard)
    void knob_command(const Command& c);
    MasterCompressor comp;
    MasterComp comp_settings;
    std::array<PlayPatterns, track_count> patterns {};
    Motion motion;
    Song song;
    bool song_mode = false;
    unsigned song_bar_now = 0;
    uint16_t tempo = 1200, global_tempo = 1200;
    // BPM per pattern: the tempo of the pattern launched last (`lead`), when set.
    bool seq_tempo = false;
    std::array<uint16_t, pattern_count> pattern_tempo {};
    uint8_t lead = 0;
    unsigned bar = 4 * ticks_per_quarter, beat = ticks_per_quarter; // the time signature, in ticks
    uint32_t count_left = 0; // Count-In: ticks of the bar counted before the song starts
    uint8_t swing = 50;
    bool running = false;
    uint32_t tick_now = 0;
    double until_tick = 0; // samples until the next tick
    uint32_t random = 0x2545F491u;

    std::atomic<bool> is_playing {false};
    std::atomic<uint32_t> tick_count {0};
    std::array<std::atomic<int>, track_count> positions_now {};
    std::atomic<unsigned> count_beat {0};
    std::atomic<unsigned> tempo_shown {1200};
    std::array<std::atomic<unsigned>, track_count> patterns_now {};
    std::atomic<bool> in_song {false};
    std::atomic<unsigned> bar_now {0};
    std::array<std::atomic<float>, track_count> meters {};
    std::atomic<float> output_peak {0};
    std::atomic<unsigned> lost {0};
    std::atomic<bool> punch_on {false};
};

} // namespace melodee
