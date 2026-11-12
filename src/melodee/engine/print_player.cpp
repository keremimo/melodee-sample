// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/print_player.h"
#include <algorithm>
#include <cmath>

namespace melodee {

namespace {
// The pager's look-ahead is two chunks times its speed: a speed of rate / chunk holds about a second of stereo ahead.
float ahead_speed(float rate) { return std::max(1.0f, rate / float(stream_chunk)); }
} // namespace

uint32_t PrintClip::safe_from(uint32_t frame) const {
    if (resident) return frame < frames ? 2 * (frames - frame) : 0;
    const uint64_t at = uint64_t(frame) * 2;
    for (const auto& h : held)
        if (at >= h.from && at < h.to) return uint32_t(h.to - at);
    return 0;
}

void PrintClip::hold_whole(std::vector<float> interleaved, uint32_t at_rate, uint32_t pass_frames, PrintTail how) {
    owned = std::move(interleaved);
    frames = uint32_t(owned.size() / 2);
    part.left = part.right = owned.data();
    part.total = 2 * frames;
    part.frames.store(part.total);
    rate = at_rate; pass = pass_frames; tail = how;
    resident = true;
}

void PrintPlayer::close(Voice& v) {
    if (v.stream >= 0 && pager) pager->close(v.stream);
    v = Voice {};
}

void PrintPlayer::cut_all() { for (auto& v : voices) close(v); }

void PrintPlayer::fade_all(float rate) {
    for (auto& v : voices) {
        if (!v.clip) continue;
        v.leaving = true;
        v.step = std::min(v.step, -1.0f / (fade_seconds * rate));
    }
}

bool PrintPlayer::leaving() const {
    for (const auto& v : voices) if (v.clip && v.leaving) return true;
    return false;
}

bool PrintPlayer::sounding() const {
    for (const auto& v : voices) if (v.clip) return true;
    return false;
}

void PrintPlayer::begin(PrintClip* clip, uint32_t frame, bool fade_in, float rate) {
    auto& m = voices[0];
    if (m.clip) close(m);
    if (!clip || frame >= clip->frames) return;
    m.clip = clip;
    m.at = frame;
    m.gain = fade_in ? 0.0f : 1.0f;
    m.step = fade_in ? 1.0f / (fade_seconds * rate) : 0.0f;
    if (clip->resident) return;
    const uint32_t safe = clip->safe_from(frame);
    m.seeking = safe == 0;
    m.stream = pager ? pager->open(clip->part, 2 * frame, false, ahead_speed(rate), safe) : -1;
    if (m.stream < 0) ++underruns; // no stream free: silent until the next start
}

void PrintPlayer::retire_main(bool ring, float rate) {
    auto& m = voices[0];
    if (!m.clip) return;
    Voice* to = nullptr;
    for (unsigned n = 1; n < voice_count && !to; ++n) if (!voices[n].clip) to = &voices[n];
    if (!to) { // none free: the one nearest its end makes room
        to = &voices[1];
        for (unsigned n = 2; n < voice_count; ++n)
            if (voices[n].clip->frames - voices[n].at < to->clip->frames - to->at) to = &voices[n];
        close(*to);
    }
    *to = m;
    if (!ring) to->step = std::min(to->step, -1.0f / (fade_seconds * rate));
    m = Voice {};
}

void PrintPlayer::release(float rate) {
    auto& m = voices[0];
    if (!m.clip) return;
    const PrintClip& c = *m.clip;
    // A Loop or Cut print let go where its pass ends (a launch on the bar it ends on): it stops there. Past that
    // seam a Loop print holds the pattern's own top again, its first hit with it.
    if (c.tail != PrintTail::Ring && (m.at <= slack || m.at + slack >= c.frames)) { close(m); return; }
    bool ring = c.tail == PrintTail::Ring && m.at + slack >= c.pass; // its tail is what follows the pass
    retire_main(ring, rate);
}

void PrintPlayer::follow(PrintClip* clip, uint32_t at, double start, double tick, float rate) {
    auto& m = voices[0];
    if (!clip) { release(rate); return; }
    if (m.clip != clip) { // a launch, a follow action, a song clip, Play, or the freeze itself
        release(rate);
        const uint64_t frame = at ? uint64_t(std::max(0.0, std::ceil(start - 1e-6))) : 0;
        begin(clip, uint32_t(std::min<uint64_t>(frame, UINT32_MAX)), at != 0, rate);
        expect = frame;
        carry = start - double(frame);
    } else if (at == 0) { // the pattern comes round
        if (clip->tail == PrintTail::Loop) {
            // The print wraps itself as the pass ends; a tick a frame or two off leaves it where it is.
            bool near_top = m.at <= slack, near_end = m.at + slack >= clip->frames;
            if (near_end && !near_top) { begin(clip, 0, false, rate); } // the last few frames of the pass skipped
            else if (!near_top) { retire_main(false, rate); begin(clip, 0, false, rate); } // started again from the top
        } else { // Ring rings its tail on in another voice; Cut has none
            retire_main(clip->tail == PrintTail::Ring && m.at + slack >= clip->pass, rate);
            begin(clip, 0, false, rate);
        }
        expect = 0;
        carry = 0;
    } else if (m.at + slack < expect || m.at > expect + slack) { // off its place (a Locate, MIDI clock in): moved there
        retire_main(false, rate);
        begin(clip, uint32_t(std::min<uint64_t>(expect, UINT32_MAX)), true, rate);
    }
    carry += tick; // as render() places the next tick: the frames up to it, rounded up
    double run = std::ceil(carry);
    expect += uint64_t(std::max(0.0, run));
    carry -= run;
}

void PrintPlayer::render(float* left, float* right, unsigned frames) {
    for (unsigned n = 0; n < voice_count; ++n) {
        auto& v = voices[n];
        if (!v.clip) continue;
        const PrintClip& c = *v.clip;
        if (!c.resident && (v.stream < 0 || pager->dead(v.stream))) { // no stream, or the card went: silent
            if (v.stream >= 0) close(v);
            continue;
        }
        uint32_t limit = c.resident ? c.part.total : pager->limit(v.stream);
        bool late = false, waited = false;
        unsigned f = 0;
        while (f < frames) {
            if (v.at >= c.frames) {
                if (n == 0 && c.tail == PrintTail::Loop && !v.leaving) { // a Loop print wraps on itself
                    v.at = 0;
                    if (!c.resident) {
                        uint32_t safe = c.safe_from(0);
                        pager->restart(v.stream, 0, safe);
                        v.seeking = safe == 0;
                        limit = pager->limit(v.stream);
                    }
                } else {
                    close(v);
                    break;
                }
            }
            const unsigned run = unsigned(std::min<uint64_t>(frames - f, c.frames - v.at));
            const uint64_t from = uint64_t(v.at) * 2;
            const unsigned ready = limit > from ? unsigned(std::min<uint64_t>(run, (limit - from) / 2)) : 0;
            if (ready) v.seeking = false;
            const float* d = c.data() + from;
            if (v.step == 0 && v.gain == 1) {
                for (unsigned k = 0; k < ready; ++k) { left[f + k] += d[2 * k]; right[f + k] += d[2 * k + 1]; }
            } else {
                for (unsigned k = 0; k < run; ++k) {
                    v.gain = std::clamp(v.gain + v.step, 0.0f, 1.0f);
                    if (k < ready) { left[f + k] += d[2 * k] * v.gain; right[f + k] += d[2 * k + 1] * v.gain; }
                }
                if (v.step > 0 && v.gain >= 1) v.step = 0;
            }
            if (ready < run) (v.seeking ? waited : late) = true;
            v.at += run;
            f += run;
            if (v.step < 0 && v.gain <= 0) { close(v); break; }
        }
        underruns += late;
        waits += waited;
        if (v.clip && v.stream >= 0) pager->place(v.stream, uint32_t(std::min<uint64_t>(uint64_t(v.at) * 2, c.part.total)));
    }
}

} // namespace melodee
