// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/engine_host.h"
#include "melodee/model/print.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Freeze plan F2a: one print rendered by a print host (EngineHost::Lean) on
// the calling thread, as fast as it goes. The host holds a print_project()
// (model/print.h) and has its samples and plug-ins; render_print() plays the
// pattern from its first tick and keeps the track's sound at the tap: a pass
// of the pattern (Cut), a pass and its tail (Ring), or the second of two
// passes (Loop). What is kept is moved back by the track's own latency (a
// plug-in's, a Limiter's look-ahead; at Pre FX its engine's alone), so its
// first frame is the pattern's first tick. A Ring print's sequencer stops on the frame the pass ends, so
// only the tail sounds after it. Frames are the device's; the shell converts
// them to 44.1 kHz.
namespace melodee {

struct PrintAudio {
    std::vector<float> left, right; // device frames
    uint64_t pass = 0;              // one pass of the pattern, device frames
    uint64_t rendered = 0;          // frames the host rendered for it
    unsigned latency = 0;           // the track's own, device frames
    // A kit's pad sends (PrintRecord::sends): for each bus in `sends`, what its pads sent into it, as many frames as
    // the print, moved back by the engine's latency alone (they leave before the chain), so they meet the print.
    uint8_t sends = 0;
    std::array<std::vector<float>, 4> send_left, send_right;
    std::string error;              // empty: made
};

// Track `track` of `host`, whose pattern is `ticks` ticks long as it plays (played_pattern()). `keep_going` is asked
// between blocks; false stops it ("stopped"). The host must not be playing yet.
// `sends`: the buses whose pad sends ride along (print_sends()), 0 none.
PrintAudio render_print(EngineHost& host, unsigned track, uint32_t ticks, PrintTap tap, PrintTail tail,
                        const std::function<bool()>& keep_going = {}, uint8_t sends = 0);

// The frames of a pass at the model rate, as render_print() counts a pass at the device's (for PrintRecord::pass).
uint32_t model_frames_of(uint64_t device_frames, float rate);

} // namespace melodee
