// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/voice_tap.h"
#include <algorithm>
#include <array>
#include <cstdint>

// The screen's side of the voice taps: each voice's head where it is heard
// now, kept a little after the voice has gone so its wake can fade (0.35 s).
// Screen thread only (Shell::snapshot); pages draw from the heads it leaves
// in the snapshot (ui/waveform.h draws them).
namespace melodee {

struct PlayHead {
    uint32_t serial = 0;
    int16_t pad = -1;       // -1: none; sampler_pads: the audition voice
    uint16_t slot = 0xffff; // the sample
    double at = 0;          // the frame in the file heard now
    uint32_t from = 0, to = 0, loop_at = 0;
    float step = 0;         // signed: reversed < 0
    float level = 0;        // its envelope heard now; 0 once it has gone
    float wake = 0;         // 1 while it sounds, falling to 0 over HeadTrail::wake_ms once it is silent
    uint32_t pass = 0;      // times its loop went round
    uint8_t flags = 0;      // tap::Flag
    bool shown() const { return pad >= 0 && wake > 0; }
    bool reverse() const { return step < 0; }
};

class HeadTrail {
public:
    static constexpr unsigned size = TrackTaps::voices;
    static constexpr uint32_t wake_ms = 350;
    static constexpr float silent = 0.002f; // an envelope under this is not heard

    // `frames`: output frames from the taps' stamp to the frame heard now (negative: the latency).
    void update(const TrackTaps& taps, double frames, uint32_t now_ms) {
        if (taps.track != track) { kept = {}; track = taps.track; }
        std::array<bool, size> seen {};
        for (unsigned i = 0; i < taps.count && i < TrackTaps::voices; ++i) {
            const VoiceTap& t = taps.voice[i];
            HeardPlace h = heard_at(t, frames);
            if (!h.begun) continue; // not heard yet: no head
            unsigned k = find(t.serial);
            if (k == size) k = room(seen);
            Kept& e = kept[k];
            if (e.head.serial != t.serial || e.head.pad < 0) e = Kept {};
            seen[k] = true;
            e.head.serial = t.serial; e.head.pad = t.pad; e.head.slot = t.slot;
            e.head.at = h.at; e.head.pass = h.pass;
            e.head.from = t.from; e.head.to = t.to; e.head.loop_at = t.loop_at;
            e.head.step = t.step; e.head.flags = t.flags;
            e.head.level = t.level;
            if (t.level < silent) { if (!e.quiet) { e.quiet = true; e.quiet_at = now_ms; } }
            else e.quiet = false;
        }
        for (unsigned k = 0; k < size; ++k) {
            Kept& e = kept[k];
            if (e.head.pad < 0) continue;
            if (!seen[k]) { // gone from the engine: the head stays where it was heard last, silent
                e.head.level = 0;
                if (!e.quiet) { e.quiet = true; e.quiet_at = now_ms; }
            }
            uint32_t since = e.quiet ? now_ms - e.quiet_at : 0;
            e.head.wake = e.quiet ? std::max(0.0f, 1.0f - float(since) / float(wake_ms)) : 1.0f;
            if (e.quiet && since >= wake_ms) e = Kept {};
        }
    }
    // Every head still shown, newest first; the rest have pad -1.
    std::array<PlayHead, size> heads() const {
        std::array<PlayHead, size> out {};
        unsigned n = 0;
        for (const auto& e : kept) {
            if (!e.head.shown()) continue;
            unsigned i = n++; // in by serial, newest first (eight at most: a plain insertion)
            while (i > 0 && out[i - 1].serial < e.head.serial) { out[i] = out[i - 1]; --i; }
            out[i] = e.head;
        }
        return out;
    }

private:
    struct Kept {
        PlayHead head;
        bool quiet = false;
        uint32_t quiet_at = 0;
    };
    unsigned find(uint32_t serial) const {
        for (unsigned k = 0; k < size; ++k) if (kept[k].head.pad >= 0 && kept[k].head.serial == serial) return k;
        return size;
    }
    unsigned room(const std::array<bool, size>& seen) const { // a free place, else the one whose wake is furthest gone, else the oldest
        unsigned best = size;
        for (unsigned k = 0; k < size; ++k) {
            if (seen[k]) continue;
            if (kept[k].head.pad < 0) return k;
            if (best == size) { best = k; continue; }
            const auto& a = kept[k].head;
            const auto& b = kept[best].head;
            if (a.wake < b.wake || (a.wake == b.wake && a.serial < b.serial)) best = k;
        }
        return best == size ? 0 : best;
    }
    std::array<Kept, size> kept {};
    uint8_t track = 255;
};

} // namespace melodee
