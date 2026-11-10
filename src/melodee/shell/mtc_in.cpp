// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/mtc_in.h"
#include <cmath>

namespace melodee {

double mtc_frames_a_second(MtcRate rate) {
    switch (rate) {
    case MtcRate::Fps24: return 24;
    case MtcRate::Fps25: return 25;
    case MtcRate::Fps2997Drop: return 30000.0 / 1001.0;
    default: return 30;
    }
}

uint32_t mtc_frame_index(unsigned minutes, unsigned seconds, unsigned frames, MtcRate rate) {
    if (rate == MtcRate::Fps2997Drop) { // labels 0 and 1 skipped each minute, but every tenth
        uint32_t nominal = (minutes * 60 + seconds) * 30 + frames;
        return nominal - 2 * (minutes - minutes / 10);
    }
    unsigned fps = rate == MtcRate::Fps24 ? 24 : rate == MtcRate::Fps25 ? 25 : 30;
    return (minutes * 60 + seconds) * fps + frames;
}

void mtc_code_of(uint32_t index, MtcRate rate, unsigned& minutes, unsigned& seconds, unsigned& frames) {
    unsigned fps = rate == MtcRate::Fps24 ? 24 : rate == MtcRate::Fps25 ? 25 : 30;
    if (rate == MtcRate::Fps2997Drop) { // 17982 frames each ten minutes, 1798 each minute after the tenth's first
        uint32_t tens = index / 17982, rest = index % 17982;
        index += 18 * tens + (rest > 1 ? 2 * ((rest - 2) / 1798) : 0);
    }
    frames = index % fps;
    seconds = index / fps % 60;
    minutes = index / (fps * 60) % 60;
}

void MtcReader::reset() {
    state = State::Idle;
    mask = 0; last_piece = -1; known = false; rejump = false; agree = 0; have_prev = false;
}

MtcReader::Event MtcReader::quarter(uint8_t data, uint64_t ns) {
    Event e; e.ns = ns;
    unsigned piece = (data >> 4) & 7;
    if (last_ns && ns > last_ns + timeout_ns && state != State::Idle) { // came back after a pause idle() did not see
        bool was = running() || rejump;
        reset();
        if (was) { e.kind = Event::Kind::Stop; last_ns = ns; return e; }
    }
    // Backwards (the master rewinding, or playing in reverse): not followed.
    if (last_piece >= 0 && piece == unsigned(last_piece + 7) % 8) {
        bool was = running() || rejump;
        reset();
        last_ns = ns;
        if (was) e.kind = Event::Kind::Stop;
        return e;
    }
    bool in_order = last_piece >= 0 && piece == unsigned(last_piece + 1) % 8;
    last_piece = int(piece); last_ns = ns;
    nibbles[piece] = data & 15;
    if (piece == 0) {
        if (known && in_order) index += 2; // the next round: two frames on
        else known = false;
        mask = 1;
    } else if (in_order && mask) {
        mask |= 1u << piece;
    } else {
        mask = 0; known = false; // a piece lost: wait for the next whole code
    }

    if (piece == 7 && mask == 0xff) {
        unsigned f = nibbles[0] | (nibbles[1] & 1) << 4, s = nibbles[2] | (nibbles[3] & 3) << 4;
        unsigned m = nibbles[4] | (nibbles[5] & 3) << 4;
        auto rate = MtcRate((nibbles[7] >> 1) & 3);
        if (f >= 30 || s > 59 || m > 59 || (rate != MtcRate::Fps30 && rate != MtcRate::Fps2997Drop && f >= unsigned(mtc_frames_a_second(rate)))) {
            mask = 0; known = false; return e; // not a time code
        }
        uint32_t code = mtc_frame_index(m, s, f, rate);
        double at = code + 1.75; // piece 7 comes a quarter frame short of two frames after the code's frame
        bool follows = have_prev && rate == prev_rate &&
                       std::fabs(at - (prev_at + double(ns - prev_ns) * mtc_frames_a_second(rate) / 1e9)) < 1.0;
        prev_at = at; prev_ns = ns; prev_rate = rate; have_prev = true;
        rate_ = rate; index = code; known = true;
        e.seconds = seconds_of(at);
        if (follows) {
            ++agree;
        } else {
            agree = 1;
            if (state == State::Running) { state = State::Settling; rejump = true; ++jump_count; }
        }
        if (agree >= 2 && state != State::Running) {
            e.kind = rejump ? Event::Kind::Jump : Event::Kind::Start;
            state = State::Running; rejump = false;
        } else if (state == State::Running) {
            e.kind = Event::Kind::Time;
        } else {
            state = State::Settling;
        }
        return e;
    }
    if (state == State::Running && known) {
        e.kind = Event::Kind::Time;
        e.seconds = seconds_of(index + piece / 4.0);
    }
    return e;
}

MtcReader::Event MtcReader::full(unsigned, unsigned minutes, unsigned seconds, unsigned frames, MtcRate rate, uint64_t ns) {
    Event e; e.ns = ns;
    bool was = running() || rejump;
    mask = 0; last_piece = -1; known = false; agree = 0; have_prev = false;
    rate_ = rate;
    index = mtc_frame_index(minutes % 60, seconds % 60, frames % 30, rate);
    if (was) { state = State::Settling; rejump = true; return e; } // located while playing: the quarter frames that follow place it
    state = State::Idle;
    e.kind = Event::Kind::Cue;
    e.seconds = seconds_of(index);
    return e;
}

MtcReader::Event MtcReader::idle(uint64_t ns) {
    Event e; e.ns = ns;
    if (state == State::Idle || !last_ns || ns <= last_ns + timeout_ns) return e;
    bool was = running() || rejump;
    reset();
    if (was) e.kind = Event::Kind::Stop;
    return e;
}

} // namespace melodee
