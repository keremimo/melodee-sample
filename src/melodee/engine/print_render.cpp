// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/print_render.h"
#include <algorithm>
#include <cmath>

namespace melodee {

namespace {
constexpr float ring_silence = 0.000316f; // -70 dB: a Ring print's tail is over once it stays below this
constexpr float ring_quiet_seconds = 0.2f; // for this long
constexpr float ring_most_seconds = 10;    // or after this long, as a mixdown's tail (engine/bounce.h)

void drain(EngineHost& host) {
    const ProjectPack* pack = nullptr;
    while (host.take_done_pack(pack)) delete pack;
    Pattern* pattern = nullptr;
    while (host.take_retired_pattern(pattern)) delete pattern;
}
} // namespace

uint32_t model_frames_of(uint64_t device_frames, float rate) {
    return uint32_t(std::llround(double(device_frames) * dsp::model_rate / std::max(1.0f, rate)));
}

PrintAudio render_print(EngineHost& host, unsigned track, uint32_t ticks, PrintTap tap, PrintTail tail, const std::function<bool()>& keep_going) {
    PrintAudio a;
    const float rate = dsp::sample_rate();
    const unsigned B = EngineHost::block;
    host.set_print_tap(track, tap == PrintTap::PostFx);
    host.send({Command::Type::Play});
    std::vector<int32_t> out(B * 2);
    // Kept: the frames [from, to) of what the host plays, `to` moving on while a Ring print's tail sounds.
    uint64_t from = 0, to = 0, quiet = 0, played = 0;
    bool planned = false, done = false, stopped = false;
    const uint64_t quiet_frames = uint64_t(ring_quiet_seconds * rate), most_frames = uint64_t(ring_most_seconds * rate);
    while (true) {
        if (keep_going && !keep_going()) { a.error = "stopped"; break; }
        // Ring: the block split where the pass ends, so the sequencer stops there and only the tail sounds after it.
        unsigned n = B;
        if (planned && tail == PrintTail::Ring && !stopped && a.pass > played) n = unsigned(std::min<uint64_t>(B, a.pass - played));
        host.render(out.data(), n, 1.0f);
        drain(host);
        if (!planned) { // the first block has Play, the tempo and the track's latency in place
            a.pass = host.tick_frame(std::max<uint32_t>(ticks, 1));
            a.latency = host.track_latency(track);
            from = (tail == PrintTail::Loop ? a.pass : 0) + a.latency;
            to = from + a.pass;
            a.left.reserve(size_t(a.pass)); a.right.reserve(size_t(a.pass));
            planned = true;
        }
        const float* l = host.print_left();
        const float* r = host.print_right();
        for (unsigned f = 0; f < n && !done; ++f) {
            uint64_t at = played + f;
            if (at < from) continue;
            if (at >= to && tail != PrintTail::Ring) { done = true; break; }
            a.left.push_back(l[f]); a.right.push_back(r[f]);
            if (at >= to) { // Ring: the tail, until it has been quiet a while
                bool loud = std::fabs(l[f]) >= ring_silence || std::fabs(r[f]) >= ring_silence;
                quiet = loud ? 0 : quiet + 1;
                done = quiet >= quiet_frames || at + 1 - to >= most_frames;
            }
        }
        played += n;
        if (tail != PrintTail::Ring && played >= to) done = true;
        if (planned && tail == PrintTail::Ring && !stopped && played >= a.pass) { host.send({Command::Type::Stop}); stopped = true; }
        if (dsp::sample_rate() != rate) { a.error = "the sample rate changed"; break; }
        if (done) break;
    }
    a.rendered = played;
    host.send({Command::Type::Stop});
    return a;
}

} // namespace melodee
