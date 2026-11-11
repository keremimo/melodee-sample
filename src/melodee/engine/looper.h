// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/dsp.h"
#include "melodee/model/looper.h"
#include "surface/midi.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

// The Looper (docs/LOOPER.md): four tempo-synced audio loops, each played
// into its own track (L1-L4). The audio thread owns every loop; the shell
// asks through Command::Looper, the service thread hands memory in and takes
// it back through queues and fits loops to a tempo, the screen reads the
// published views. Audio is kept at 44.1 kHz (dsp::model_rate) whatever the
// device runs, so a loop sounds and saves the same at any rate.
//
// Time is the sequencer's: ticks (96 a quarter) from bar 1, as a double. A
// loop's place is (tick - start) mod its ticks, so it stays on the beat
// whatever the tempo does; at the tempo it was recorded at it reads its
// frames, at another it is stretched in pitch (LoopStretch).
namespace melodee {

namespace looper {
constexpr uint32_t capacity = 64 * uint32_t(dsp::model_rate); // frames of a loop, and of its undo
constexpr unsigned fade = 32;          // seam and punch fades, frames
constexpr unsigned ramp = 64;          // play, stop and level ramps, frames
constexpr unsigned peak_columns = 256; // the waveform the page draws
constexpr unsigned swap_chunk = 8192;  // undo frames swapped a block
constexpr unsigned block = 128;        // the engine host's block
enum class State : uint8_t { Empty, Armed, Recording, Playing, Overdub, Stopped };
enum class Pending : uint8_t { None, Start, Close, Play, Stop };
enum class Undo : uint8_t { None, Undo, Redo };
enum class Error : uint8_t { None, NoMemory, Locked, TooLong, Tempo, Busy };
// Command::Looper's index: what the shell asks of loop `track`.
enum class Do : uint8_t {
    Enable,    // value: on (memory is asked for) or off (every loop cleared, memory handed back)
    Primary,   // record, close, overdub, play: the one-button cycle
    Toggle,    // play or stop on the next boundary; again cancels
    Halt,      // stop now
    Now,       // play or stop at once, whatever the quantize
    Undo,      // undo, then redo, the last overdub pass
    Clear,
    Length,    // value: bars (0 free); empty or armed loops only
    Feedback,  // value: percent
    Quantize,  // value: looper::Quantize
    Then,      // value: looper::Then
    Source,    // value: looper::source_*; not while it records or overdubs
    Latency,   // position: the input's round trip, device frames
    Transport, // value: looper::Transport
};
inline const char* state_word(State s) {
    static const char* const w[] {"", "ARM", "REC", "PLAY", "DUB", "STOP"};
    return w[unsigned(s) % 6];
}
} // namespace looper

// Loop memory: one stereo buffer of looper::capacity frames. The service
// thread makes it (zeroed, so its pages are there before the audio thread
// writes) and frees it; the audio thread holds two a loop, audio and undo.
struct LoopBuffer {
    LoopBuffer() : left(new float[looper::capacity]()), right(new float[looper::capacity]()) {}
    std::unique_ptr<float[]> left, right;
};

// A saved loop, read by the service thread into a buffer of its own; the
// audio thread swaps it in for the loop's audio and hands the old one back.
struct LoopLoad {
    LoopBuffer* buffer = nullptr;
    uint8_t loop = 0, layers = 1;
    uint32_t frames = 0;
    double ticks = 0;
    uint16_t tempo = 0; // tenths of a BPM it was recorded at
    bool stopped = false;
};

// Plays a loop at another tempo, keeping its pitch: two taps each read a
// window of `window` frames at the pitch's step, a new one every half window,
// started where the loop should be now and pulled onto the place that best
// continues the tap it takes over from (WSOLA), each faded in and out with a
// squared sine so the two always add to one. Real-time safe; the service
// thread uses the same to fit a loop.
class LoopStretch {
public:
    static constexpr unsigned window = 2048, hop = window / 2, search = 384, match = 64;
    void reset() { on[0] = on[1] = false; since = hop; first = true; }
    // One frame: `at` the source frame it should be at now, `step` source frames a frame (the pitch).
    void frame(const float* l, const float* r, uint32_t length, double at, double step, float& out_l, float& out_r);
private:
    double tap[2] {};
    unsigned age[2] {};
    bool on[2] {};
    unsigned next = 0, since = hop;
    bool first = true;
    double best(const float* l, const float* r, uint32_t length, double from, double around, double step) const;
};

class Looper {
public:
    using State = looper::State;
    using Pending = looper::Pending;
    // The sequencer's time at the block's first frame: `running` it plays (the
    // tick moves), `restart` it started there (Play: every loop lines up, armed
    // takes start); `counting` a count-in plays (loops wait it out).
    struct Clock {
        double tick = 0, per_frame = 0;  // ticks at frame 0, ticks a device frame (from the tempo even while stopped)
        unsigned bar = 384, beat = 96;   // ticks
        bool running = false, restart = false, counting = false;
        float rate = dsp::model_rate;    // the device's frames a second
        unsigned input_lag = 0;          // the input's round trip, device frames, unless `looper latency` set one
    };
    // Where each source was in the block before this one (looper::source_*): left and right, `frames` long.
    struct Sources {
        std::array<std::array<const float*, 2>, looper::source_count> at {};
        unsigned frames = 0;
    };

    Looper();
    ~Looper();
    Looper(const Looper&) = delete;
    Looper& operator=(const Looper&) = delete;

    // Audio thread.
    void ask(looper::Do what, unsigned loop, int value, uint32_t position = 0);
    // The project's settings (a load or a new project): every loop cleared unless `keep` (the same project again
    // at a new rate), the Looper on or off as it says, and a load waiting for each loop it keeps.
    void configure(const LooperSettings& settings, bool keep);
    // One block: each loop's output into out_l[n] / out_r[n] (replacing them), and what the sources played the
    // block before written where it belongs.
    void run(const Clock& clock, const Sources& sources, unsigned frames, float (*out_l)[looper::block], float (*out_r)[looper::block]);
    bool on() const { return enabled; }
    uint8_t source(unsigned loop) const { return loops[loop % loop_tracks].source; }
    // It waits for or records a take, or overdubs: a hardware input it records is heard meanwhile (Monitor Auto).
    bool listening(unsigned loop) const {
        auto s = loops[loop % loop_tracks].state;
        return enabled && (s == State::Armed || s == State::Recording || s == State::Overdub);
    }
    uint32_t sources_used() const; // a bit for each source some loop may record (the host copies only those)

    // Service thread.
    void service();                                   // memory in and out, fits
    bool give_load(LoopLoad item);                    // a saved loop's audio (the buffer is the Looper's from here)
    bool memory_wanted() const { return want_memory.load(std::memory_order_acquire); }
    // Hold LOOP to read its audio (save, commit): true once the audio thread no longer writes it, and until
    // release() it starts no overdub, undo, fit or clear (they wait). Its frames from the start, `frames()` long.
    bool hold(unsigned loop);
    void release(unsigned loop);
    const float* held_left(unsigned loop) const { return held_buffer[loop % loop_tracks].load(std::memory_order_acquire) ? held_buffer[loop % loop_tracks].load()->left.get() : nullptr; }
    const float* held_right(unsigned loop) const { return held_buffer[loop % loop_tracks].load(std::memory_order_acquire) ? held_buffer[loop % loop_tracks].load()->right.get() : nullptr; }
    unsigned buffers_out() const { return owned.load(std::memory_order_acquire); } // made and not freed yet

    // Any thread: what each loop is doing, for the page, the lights and the remote.
    struct View {
        State state = State::Empty;
        Pending pending = Pending::None;
        uint8_t bars = 0, layers = 0, source = 0, feedback = 100;
        looper::Undo undo = looper::Undo::None;
        looper::Error error = looper::Error::None;
        uint32_t length = 0, position = 0, span = 0; // frames: the loop, where it plays, what the waveform spans
        double ticks = 0;                            // the ticks it spans; 0 while its first take runs
        uint16_t tempo = 0;                          // tenths of a BPM it was recorded at (0: none)
        bool stretched = false, fitting = false, sounding = false, held = false, ready = false;
        unsigned errors = 0;                         // counts up with each refusal (the page says why once)
    };
    View view(unsigned loop) const;
    uint8_t peak(unsigned loop, unsigned column) const { return loops_view[loop % loop_tracks].peaks[column % looper::peak_columns].load(std::memory_order_relaxed); }
    bool armed() const { return any_armed.load(std::memory_order_relaxed); }       // a loop waits for its take
    bool recording() const { return any_recording.load(std::memory_order_relaxed); } // a take or an overdub runs
    unsigned quantize() const { return quantize_now.load(std::memory_order_relaxed); }
    unsigned then() const { return then_now.load(std::memory_order_relaxed); }
    unsigned transport() const { return transport_now.load(std::memory_order_relaxed); }
    int latency() const { return latency_now.load(std::memory_order_relaxed); }
    bool enabled_now() const { return enabled_shown.load(std::memory_order_relaxed); }
    double beat_tick() const { return tick_shown.load(std::memory_order_relaxed); } // the looper's clock, ticks

private:
    enum class Write : uint8_t { Off, Take, DubIn, Dub, DubOut };
    enum class FitState : uint8_t { None, Wanted, Running, Done, Failed };
    struct Loop {
        State state = State::Empty;
        Pending pending = Pending::None;
        double target = -1;          // the tick its pending action falls on; < 0: when the transport starts
        double start = 0, ticks = 0; // where it began (ticks), how many it spans (0 during the first take)
        uint32_t length = 0;         // frames
        double take_at = 0;          // the first take: frames from its start the play side has reached
        uint16_t tempo = 0;          // tenths, recorded at
        uint8_t bars = 0, layers = 0, source = 0;
        float feedback = 1;
        LoopBuffer* audio = nullptr;
        LoopBuffer* undo = nullptr;
        // The writer, `lag` behind the play side: its mode, the next one and the frames until it applies.
        Write write = Write::Off, write_next = Write::Off;
        int write_in = -1;
        double wx = 0;               // its place, frames (Take: from the take's start; else in the loop)
        bool wx_known = false;
        float in_l = 0, in_r = 0;    // the input frame before, for the frames between
        float dub = 0;               // the punch's fade, 0..1
        uint32_t dub_start = 0, dub_count = 0; // the frames the pass has written (their old layers kept for undo)
        looper::Undo undo_state = looper::Undo::None;
        bool undo_wanted = false;    // undo once the pass ends
        bool swapping = false;       // undo running: swap_at of dub_count frames swapped so far
        uint32_t swap_at = 0;
        bool clearing = false;       // cleared once its output has faded
        float gain = 0;              // the play ramp, 0..1
        bool was_stretched = false;
        unsigned xfade = 0;          // frames of a crossfade from the stretch back to plain reading
        LoopStretch stretch;
        float lp[4][2] {};           // the 96 kHz low pass's state: [stage * 2 + side][z1, z2]
        uint32_t span = 0;           // frames the waveform spans: the loop, the set take or the free take so far
        unsigned peaks_next = looper::peak_columns; // the next column worked out again from the audio
        std::array<uint8_t, looper::peak_columns> peaks {};
        Write after_take = Write::Off; // what the writer does once the first take is all written
        double halt_tick = 0;        // the place it fades out from when the transport stops
        looper::Error error = looper::Error::None;
        unsigned errors = 0;
        // Fit to tempo: the service thread renders `audio` (length frames) into `undo` as fit_length frames.
        std::atomic<FitState> fit {FitState::None};
        std::atomic<bool> fit_cancel {false};
        uint32_t fit_length = 0;
        uint16_t fit_tempo = 0;
        bool fit_then_dub = false;
        // Held for the service thread to read.
        std::atomic<bool> hold_asked {false};
        bool held = false;
        bool waiting_load = false;   // a saved loop is coming: no take meanwhile
    };
    struct Published {
        std::atomic<uint8_t> state {0}, pending {0}, bars {0}, layers {0}, source {0}, feedback {100}, undo {0}, error {0};
        std::atomic<uint32_t> length {0}, position {0}, span {0};
        std::atomic<double> ticks {0};
        std::atomic<uint16_t> tempo {0};
        std::atomic<bool> stretched {false}, fitting {false}, sounding {false}, held {false}, ready {false};
        std::atomic<unsigned> errors {0};
        std::array<std::atomic<uint8_t>, looper::peak_columns> peaks {};
    };

    struct Coefficients { float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };
    static float biquad(const Coefficients& c, float* z, float x) {
        float y = c.b0 * x + z[0];
        z[0] = c.b1 * x - c.a1 * y + z[1];
        z[1] = c.b2 * x - c.a2 * y;
        return y;
    }
    void refuse(Loop& l, looper::Error e) { l.error = e; ++l.errors; }
    void reset(Loop& l, bool keep_memory);
    double boundary(double tick) const;                 // the first quantize boundary at or after `tick`
    void queue(Loop& l, Pending p, double now);         // a pending action for the next boundary (or the transport's start)
    double take_start(const Loop& self, double now) const; // where an armed take starts: the longest loop playing's next start
    void set_write(Loop& l, Write mode);
    void schedule_write(Loop& l, Write mode, unsigned lag);
    void end_pass(Loop& l);                             // an overdub pass over: its undo is there
    void begin_take(Loop& l, double at, unsigned lag);
    void close_take(Loop& l, double at, double past = 0); // `past`: frames the take ran past `at`, dropped
    void close_on_bar(Loop& l, double now, bool nearest); // on whole bars from its start (a press, or a stop)
    void take_written(Loop& l);
    void punch(Loop& l, bool in, unsigned lag);
    void act(Loop& l, Pending p, double at, unsigned lag); // a pending action, on its tick
    void write_frame(Loop& l, unsigned n, double x, float in_l, float in_r);
    void write_at(Loop& l, unsigned n, uint32_t k, float in_l, float in_r);
    void play_frame(Loop& l, double tick, float& out_l, float& out_r);
    double index_at(const Loop& l, double tick) const;
    bool stretched(const Loop& l) const;                // it plays at a tempo other than its own
    void peak_column(Loop& l, unsigned column);         // worked out again from the audio
    void seam(Loop& l);                                 // a take's ends faded, once it is all written
    void swap_step(Loop& l);
    void fit_start(Loop& l, bool then_dub);
    void fit_finish(Loop& l);
    void publish(unsigned n);
    unsigned lag_of(const Loop& l, unsigned block_frames) const;
    void primary(Loop& l, unsigned n, double now);
    void toggle(Loop& l, double now);
    void halt(Loop& l, double now);
    void fit_render(Loop& l); // service thread

    std::array<Loop, loop_tracks> loops;
    std::array<Published, loop_tracks> loops_view;
    bool enabled = false, disable_wanted = false;
    uint8_t quantize_mode = 0, then_mode = 0, transport_mode = 0;
    double last_bar = 384, last_beat = 96, last_per_frame = 0; // the clock's last time signature and pace
    unsigned default_lag = 0;
    std::array<Coefficients, 2> lp_coef {};
    int input_latency = -1;   // frames; -1: none set (the input arrives in the next block, nothing more)
    double own_tick = 0;      // the looper's clock (the sequencer's while it runs)
    bool was_running = false, halted = true;
    float rate = dsp::model_rate;
    unsigned block_frames = looper::block;
    // Memory: made by the service thread, handed in (`pool`) and back (`retired`).
    MidiQueue<16, LoopBuffer*> pool, retired;
    MidiQueue<8, LoopLoad> loads;
    std::array<LoopBuffer*, 2 * loop_tracks> spare {};   // handed in, not in a loop yet
    std::atomic<bool> want_memory {false};
    std::atomic<unsigned> owned {0};
    std::array<std::atomic<LoopBuffer*>, loop_tracks> held_buffer {};
    std::array<LoopLoad, loop_tracks> waiting {};          // loads that came before the memory to swap with
    std::atomic<bool> any_armed {false}, any_recording {false}, enabled_shown {false};
    std::atomic<unsigned> quantize_now {0}, then_now {0}, transport_now {0};
    std::atomic<int> latency_now {-1};
    std::atomic<double> tick_shown {0};
    // Deferred while a loop is held (save, commit).
    struct Asked { looper::Do what; uint8_t loop; int value; uint32_t position; };
    std::array<Asked, 16> deferred {};
    unsigned deferred_count = 0;
    bool retire(LoopBuffer*& b) { if (!b) return true; if (!retired.push(b)) return false; b = nullptr; return true; }
};

} // namespace melodee
