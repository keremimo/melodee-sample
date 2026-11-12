// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/midi_config.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

// MIDI and the sync jack, timed on the audio clock (parity phase 9).
//
// The audio thread decides *when* (a frame of its own count) and *what* (a
// MidiOutEvent, no bytes yet); the MIDI thread turns each into a message with
// juce::MidiMessage and sends it when its frame is heard. AudioClock maps
// frames to CLOCK_MONOTONIC for that, and back for what comes in (the
// kernel's receive stamps). Everything here is allocation-free.
namespace melodee {

struct MidiOutEvent {
    enum class Kind : uint8_t {
        NoteOn, NoteOff,     // channel 1..16, a = key, b = velocity
        Clock, Start, Stop,  // MIDI clock (24 a quarter) and transport
        Continue,            // value = Song Position, in sixteenths (sent first)
        MtcQuarter,          // a = piece 0..7, b = its nibble
        MtcFull,             // value = frames at 25 fps since 0:00:00:00 (a full-frame SysEx)
        CvHigh, CvLow,       // SYNC OUT (CV_CLK_OUT)
        Control, Program,    // a MIDI track's (Stage's CC and PROGRAM pages): a = controller, b = value; a = program
        // Roadmap P4 (docs/MIDI_TRACKS.md M2): a slot's 14-bit CC (a = its number 0-31, value 0-16383: MSB, then LSB at
        // a + 32), NRPN (value = number << 14 | data, as CC 99, 98, 6, 38), pitch bend (value 0-16383), channel
        // pressure (b), poly pressure (a = key, b).
        Cc14, Nrpn, PitchBend, ChannelPressure, PolyPressure,
    };
    Kind kind = Kind::NoteOn;
    uint8_t channel = 1, a = 0, b = 0;
    uint32_t value = 0;
    uint64_t frame = 0; // the audio thread's frame it belongs to
    MidiDest dest = MidiDest::Config; // where it goes out (a MIDI track's own port, docs/MIDI_TRACKS.md)
    uint8_t track = 255;              // the MIDI track it is from (255: none)
};

// The audio clock against CLOCK_MONOTONIC. The audio thread stamps each
// block as it starts rendering; a block comes when the device has room, so
// a late wake-up only ever makes a stamp late, never early. The estimate
// therefore follows the earliest stamps: an early one moves it at once, a
// late one only by a hair, which is still enough to follow the two crystals'
// drift (tens of ppm). What the device plays was rendered `latency` frames
// before (the frames the PCM holds queued). Rate and latency are the device's,
// set by the app (any rate: nothing here assumes 44.1 kHz).
class AudioClock {
public:
    void set_format(double frames_a_second, uint64_t latency_frames) {
        rate.store(frames_a_second, std::memory_order_relaxed);
        latency.store(latency_frames, std::memory_order_relaxed);
        anchored = false;
    }
    double frames_a_second() const { return rate.load(std::memory_order_relaxed); }
    uint64_t latency_frames() const { return latency.load(std::memory_order_relaxed); }
    // Audio thread, at each block's start.
    void stamp(int64_t now_ns, uint64_t frame) {
        double ns_a_frame = 1e9 / rate.load(std::memory_order_relaxed);
        double expected = anchor + double(frame) * ns_a_frame;
        double error = double(now_ns) - expected;
        if (!anchored || std::fabs(error) > 20e6) { anchor = double(now_ns) - double(frame) * ns_a_frame; anchored = true; }
        else if (error < 0) anchor += error * 0.5; // earlier than thought: the schedule is earlier
        else anchor += error * 0.002;              // later: a late wake-up, or a little drift
        published.store(int64_t(anchor), std::memory_order_relaxed);
    }
    // Any thread: when `frame` is heard, ns of CLOCK_MONOTONIC.
    int64_t heard_at(uint64_t frame) const {
        double ns_a_frame = 1e9 / rate.load(std::memory_order_relaxed);
        return published.load(std::memory_order_relaxed) + int64_t(double(frame + latency.load(std::memory_order_relaxed)) * ns_a_frame);
    }
    // Any thread: the frame being rendered at `ns` (for what came in then).
    uint64_t frame_at(int64_t ns) const {
        double f = double(ns - published.load(std::memory_order_relaxed)) * rate.load(std::memory_order_relaxed) / 1e9;
        return f > 0 ? uint64_t(f) : 0;
    }
    // Any thread: the frame heard at `ns` (rendered `latency` frames before; MTC in lines the song up with it).
    uint64_t heard_frame(int64_t ns) const {
        uint64_t f = frame_at(ns), l = latency.load(std::memory_order_relaxed);
        return f > l ? f - l : 0;
    }
    bool known() const { return published.load(std::memory_order_relaxed) != 0; }

private:
    double anchor = 0;
    bool anchored = false;
    std::atomic<double> rate {44100.0};
    std::atomic<uint64_t> latency {384};
    std::atomic<int64_t> published {0};
};

// MIDI clock, MTC and the CV clock, from the tempo in samples a quarter. Not
// from the sequencer's ticks: those swing, the clocks must not. Each block
// says where the next pulses fall in it, to the frame. The audio thread only.
class SyncOut {
public:
    // MTC's quarter frames (25 fps, four a frame) and the CV pulse's width follow the device's rate.
    void set_rate(double frames_a_second) { rate = frames_a_second; }
    void set(bool clock, bool mtc, bool cv, unsigned ppqn) {
        if (going && clock && !clock_on) next_clock = std::ceil(beats * 24 - 1e-9);
        if (going && cv && !cv_on) next_cv = std::ceil(beats * cv_ppqn(ppqn) - 1e-9);
        clock_on = clock; mtc_on = mtc; cv_on = cv; ppqn_ = std::max(1u, ppqn);
    }
    bool going_now() const { return going; }
    // Following MIDI clock in: the clock out stays smooth on the tempo measured, but never runs ahead of the pulses
    // let through (`allow`: how many since Start) and catches up at once when a pulse behind; the count always matches.
    void set_external(bool on) { if (on && !external) first_external = next_clock; external = on; }
    void allow(uint64_t pulses) { allowed = pulses; }
    // The song starts at `frame`, `quarters` from its start, `seconds` into it.
    // From the start: Start; elsewhere Song Position (the next sixteenth) and Continue.
    template <class Emit> void start(uint64_t frame, double quarters, double seconds, double quarter_samples, Emit&& emit) {
        going = true;
        double sixteenth = std::ceil(quarters * 4 - 1e-9);
        double wait = (sixteenth / 4 - quarters) * quarter_samples; // the clock resumes on that sixteenth
        beats = quarters;
        next_clock = sixteenth * 6;
        first_external = next_clock; allowed = 0; last_clock = 0;
        next_cv = std::ceil(quarters * ppqn_ - 1e-9);
        if (clock_on) {
            MidiOutEvent e; e.frame = frame;
            if (quarters <= 0) e.kind = MidiOutEvent::Kind::Start;
            else { e.kind = MidiOutEvent::Kind::Continue; e.value = uint32_t(sixteenth); e.frame = at_frame(frame, wait); }
            emit(e);
        }
        mtc_count = 0; mtc_left = 0;
        mtc_base = uint32_t(std::max(0.0, seconds) * 25 + 0.5);
        if (mtc_on) { MidiOutEvent e; e.kind = MidiOutEvent::Kind::MtcFull; e.value = mtc_base; e.frame = frame; emit(e); }
    }
    template <class Emit> void stop(uint64_t frame, Emit&& emit) {
        if (!going) return;
        going = false;
        if (clock_on) { MidiOutEvent e; e.kind = MidiOutEvent::Kind::Stop; e.frame = frame; emit(e); }
        if (cv_on) { MidiOutEvent e; e.kind = MidiOutEvent::Kind::CvLow; e.frame = frame; emit(e); }
    }
    // The pulses inside the block of `frames` starting at `frame`.
    template <class Emit> void run(uint64_t frame, unsigned frames, double quarter_samples, Emit&& emit) {
        if (!going || quarter_samples <= 0) return;
        if (clock_on)
            for (;;) {
                double at = (next_clock / 24 - beats) * quarter_samples;
                if (external) {
                    double through = first_external + double(allowed); // pulses received so far, on this count
                    if (next_clock >= through) break;                  // wait for the clock in
                    // Late (the pulses came in a bunch, or the tempo was not known yet): catch up at up to twice
                    // the pace, never closer than half a pulse, so what follows downstream never sees a burst.
                    double soonest = double(last_clock) + quarter_samples / 48 - double(frame);
                    at = std::max(at, soonest);
                }
                if (at >= frames) break;
                MidiOutEvent e; e.kind = MidiOutEvent::Kind::Clock; e.frame = at_frame(frame, at); emit(e);
                last_clock = e.frame;
                next_clock += 1;
            }
        if (cv_on)
            for (;;) {
                double at = (next_cv / ppqn_ - beats) * quarter_samples;
                if (at >= frames) break;
                // High for 5 ms, or half the pulse's period when they come faster.
                double width = std::min(0.005 * rate, quarter_samples / ppqn_ / 2);
                MidiOutEvent e; e.kind = MidiOutEvent::Kind::CvHigh; e.frame = at_frame(frame, at); emit(e);
                e.kind = MidiOutEvent::Kind::CvLow; e.frame = at_frame(frame, at + std::max(1.0, width)); emit(e);
                next_cv += 1;
            }
        if (mtc_on) {
            while (mtc_left < frames) {
                // Eight quarter frames carry one time code, two frames' worth: the one they began on.
                unsigned piece = mtc_count % 8;
                uint32_t code = mtc_base + (mtc_count / 8) * 2;
                MidiOutEvent e; e.kind = MidiOutEvent::Kind::MtcQuarter; e.a = uint8_t(piece); e.b = mtc_nibble(code, piece);
                e.frame = at_frame(frame, mtc_left); emit(e);
                ++mtc_count; mtc_left += rate / 100;
            }
            mtc_left -= frames;
        }
        beats += frames / quarter_samples;
    }
    // A time code (frames at 25 fps) as MTC's piece n: frames, seconds, minutes, hours (with the rate, 25 fps = 1).
    static uint8_t mtc_nibble(uint32_t code, unsigned piece) {
        unsigned f = code % 25, s = code / 25 % 60, m = code / 1500 % 60, h = code / 90000 % 24;
        switch (piece) {
        case 0: return uint8_t(f & 15);
        case 1: return uint8_t(f >> 4);
        case 2: return uint8_t(s & 15);
        case 3: return uint8_t(s >> 4);
        case 4: return uint8_t(m & 15);
        case 5: return uint8_t(m >> 4);
        case 6: return uint8_t(h & 15);
        default: return uint8_t((h >> 4) | (1 << 1));
        }
    }

private:
    static uint64_t at_frame(uint64_t frame, double offset) { return frame + uint64_t(std::max(0.0, std::round(offset))); }
    static unsigned cv_ppqn(unsigned p) { return std::max(1u, p); }
    bool clock_on = false, mtc_on = false, cv_on = false, going = false, external = false;
    double first_external = 0; // the clock out's pulse number when Start or Continue came in
    uint64_t last_clock = 0;   // the frame of the last pulse out
    uint64_t allowed = 0;
    unsigned ppqn_ = 2;
    double rate = 44100.0;
    double beats = 0, next_clock = 0, next_cv = 0; // quarters since the song's start; the next pulses' numbers
    double mtc_left = 0;                           // samples to the next quarter frame
    uint32_t mtc_count = 0, mtc_base = 0;
};

} // namespace melodee
