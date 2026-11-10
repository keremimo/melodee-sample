// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Chop's pads (parity phase 5): a pad plays its slice and picks it for K1 /
// K2. In Manual, armed (K3 onto Manual, or Push), pad 1 plays the whole
// region and every pad tapped while it plays cuts it there (up to 16
// slices, as the MPC's), each cut pulled onto the strongest hit just before
// it, since a tap comes a little after the sound it means.
#include "melodee/shell/context.h"
#include "melodee/shell/sample_library.h"

namespace melodee {

void Context::chop_tap(unsigned pad, uint8_t velocity) {
    auto& sl = ui.slices;
    uint32_t frames = ui.chop_sample < sample_slots ? project.samples[ui.chop_sample].frames : 0;
    if (!frames || !sl.count) return;
    auto audition = [&](uint32_t from, uint32_t to) {
        Command a {Command::Type::Audition, ui.track, 0, 0, int16_t(velocity)};
        a.pad.sample = ui.chop_sample; a.pad.start = from; a.pad.end = to;
        send(a);
    };
    if (ui.chop_take_at) { // Manual's take: a cut where the region is now
        uint32_t at = sl.start[0] + uint32_t(uint64_t(now - ui.chop_take_at) * 441 / 10);
        if (at < sl.end(sl.count - 1, frames)) {
            if (library) {
                auto loaded = library->slot(ui.chop_sample);
                at = snap_to_hit(at, loaded.hits, loaded.hit_count);
            }
            if (sl.add(at, frames, manual_slices))
                for (unsigned n = 0; n < sl.count; ++n) if (sl.start[n] == at) ui.chop_slice = uint8_t(n);
            return;
        }
        ui.chop_take_at = 0; // it has played out: the pads play slices again
    }
    if (ui.chop_mode == ChopMode::Manual && ui.chop_armed && pad == 0) {
        sl.count = 1; // the take starts from one slice, the whole region
        ui.chop_slice = 0; ui.chop_armed = false;
        ui.chop_take_at = now ? now : 1;
        audition(sl.start[0], sl.end(0, frames));
        return;
    }
    if (pad >= sl.count) return;
    ui.chop_slice = uint8_t(pad);
    audition(sl.start[pad], sl.end(pad, frames));
}

} // namespace melodee
