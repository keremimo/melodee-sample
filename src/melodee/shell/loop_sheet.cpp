// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/loop_sheet.h"
#include "melodee/shell/input_route.h"

namespace melodee {
using S = Looper::State;

bool loop_going(const Looper::View& v) {
    return v.state == S::Armed || v.state == S::Recording || v.state == S::Playing || v.state == S::Overdub;
}

bool loop_job_can(LoopJob job, const Looper::View& v, bool can_commit) {
    bool audio = v.length > 0;
    switch (job) {
    case LoopJob::Back: return true;
    case LoopJob::Commit: case LoopJob::CommitNote: return can_commit && audio && (v.state == S::Playing || v.state == S::Stopped);
    case LoopJob::Toggle: return audio;
    case LoopJob::Clear: return v.state != S::Empty;
    case LoopJob::Now: return loop_going(v) || (audio && v.state == S::Stopped);
    case LoopJob::Count: break;
    }
    return false;
}

int commit_pad(const Track& t) {
    if (t.engine != EngineKind::Sampler) return -1;
    for (unsigned k = 1; k <= sampler_pads; ++k) {
        unsigned n = (t.last_pad + k) % sampler_pads;
        if (!t.pads[n].used()) return int(n);
    }
    return -1;
}

namespace {
// The ADC route a loop's own input needs: 1 line, 2 mic, 0 none.
unsigned adc_of(uint8_t source) {
    if (!looper::source_is_direct(source)) return 0;
    auto in = looper::direct_input(source);
    return from_rear(in) ? InputRoute::need_line : in == InputSource::Mic ? InputRoute::need_mic : 0;
}
} // namespace

bool loop_source_clashes(const LooperSettings& lp, unsigned loop, uint8_t source) {
    unsigned adc = adc_of(source);
    if (!adc) return false;
    for (unsigned n = 0; n < loop_tracks; ++n)
        if (n != loop % loop_tracks && adc_of(lp.loops[n].source) && adc_of(lp.loops[n].source) != adc) return true;
    return false;
}

unsigned loop_input_needs(const LooperSettings& lp) {
    if (!lp.on) return 0;
    unsigned needs = 0;
    for (const auto& l : lp.loops) {
        needs |= adc_of(l.source);
        if (looper::source_is_direct(l.source) && from_usb(looper::direct_input(l.source))) needs |= InputRoute::need_usb;
    }
    if ((needs & InputRoute::need_line) && (needs & InputRoute::need_mic)) needs &= ~InputRoute::need_mic; // never both (a file from elsewhere)
    return needs;
}

unsigned loop_sources(const Project& project, unsigned loop, std::array<uint8_t, looper::source_count>& out, unsigned& at) {
    loop %= loop_tracks;
    uint8_t own = project.looper.loops[loop].source;
    unsigned count = 0;
    at = 0;
    for (unsigned n = 0; n < looper::source_count; ++n) {
        auto source = uint8_t(n);
        if (source != own) {
            if (looper::source_is_track(source) && !project.tracks[source - 1].used()) continue;
            if (source == looper::source_loop(loop) || loop_source_clashes(project.looper, loop, source)) continue;
        } else {
            at = count;
        }
        out[count++] = source;
    }
    return count;
}

} // namespace melodee
