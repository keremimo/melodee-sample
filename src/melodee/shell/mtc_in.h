// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <array>
#include <cstdint>

// MTC in (parity phase 9b): MIDI Time Code's quarter frames and full frames
// as the song's time, on the MIDI thread. Pure, so the host test feeds it
// generated time code.
//
// Eight quarter frames carry one time code (frames, seconds, minutes, hours
// and the rate), two frames' worth: the code names the frame their first
// piece was sent on. Once one code is read whole, every quarter frame after
// it says where the master is (that code, two frames on each round, plus a
// quarter frame a piece), so each one is a time sample with its kernel stamp.
//
// Settling: the master is followed only after two whole codes in a row agree
// (the second where the first and the time between say, within a frame, at
// the same rate): four to six frames from the first quarter frame, 130-250
// ms. A code that does not follow on from the last one
// is a jump (the master located while playing): it settles again and then
// places the song there. Quarter frames stopping for 120 ms, or running
// backwards, is Stop. A full frame is where the master stands (located while
// stopped): a cue, not a start.
//
// Time: seconds since 0:00:00:00, the hours left out (a DAW's session often
// starts at 1:00:00:00, which is the song's start here too). 29.97 drop-frame
// codes are counted as the frames they are (two labels skipped each minute
// but every tenth), so its seconds run with the clock on the wall.
namespace melodee {

enum class MtcRate : uint8_t { Fps24, Fps25, Fps2997Drop, Fps30 };

double mtc_frames_a_second(MtcRate rate);
// The frames since 0:00:00 a time code names (hours left out), drop-frame counted as real frames.
uint32_t mtc_frame_index(unsigned minutes, unsigned seconds, unsigned frames, MtcRate rate);
// And back: the time code of a frame index (for the status line and the test's generator).
void mtc_code_of(uint32_t index, MtcRate rate, unsigned& minutes, unsigned& seconds, unsigned& frames);

class MtcReader {
public:
    struct Event {
        enum class Kind : uint8_t { None, Start, Jump, Time, Stop, Cue } kind = Kind::None;
        double seconds = 0; // where the master was at `ns`
        uint64_t ns = 0;    // the message's receive stamp
    };
    static constexpr uint64_t timeout_ns = 120000000; // no quarter frame for this long: stopped

    Event quarter(uint8_t data, uint64_t ns); // F1 dd
    Event full(unsigned hours, unsigned minutes, unsigned seconds, unsigned frames, MtcRate rate, uint64_t ns);
    Event idle(uint64_t ns); // the MIDI loop's pass: Stop once when the quarter frames stopped
    void reset();

    bool running() const { return state == State::Running; }
    bool settling() const { return state == State::Settling; }
    MtcRate rate() const { return rate_; }
    uint32_t frame() const { return index; }      // the last code read whole, as a frame index
    unsigned jumps() const { return jump_count; } // codes that did not follow on, while running

private:
    enum class State : uint8_t { Idle, Settling, Running };
    double seconds_of(double frames) const { return frames / mtc_frames_a_second(rate_); }
    State state = State::Idle;
    std::array<uint8_t, 8> nibbles {};
    unsigned mask = 0;         // pieces read in order since piece 0
    int last_piece = -1;
    bool known = false;        // `index` is the code the pieces now running belong to
    bool rejump = false;       // settling again after a jump (Jump, not Start, when settled)
    uint32_t index = 0;        // frame index of the current round's piece 0
    unsigned agree = 0;        // whole codes in a row that followed on from the one before
    bool have_prev = false;    // the last whole code: where it put the master (frames) and when
    double prev_at = 0;
    uint64_t prev_ns = 0;
    MtcRate prev_rate = MtcRate::Fps25;
    uint64_t last_ns = 0;
    MtcRate rate_ = MtcRate::Fps25;
    unsigned jump_count = 0;
};

} // namespace melodee
