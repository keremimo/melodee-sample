// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Chop's pads (parity phase 5): a pad plays its slice and picks it for K1 /
// K2. In Manual, armed (K3 onto Manual, or Push), pad 1 plays the whole
// region and every pad tapped while it plays cuts it there (up to 16
// slices, as the MPC's), each cut pulled onto the strongest hit just before
// it, since a tap comes a little after the sound it means. With Zero Snap on
// (B2, kept with the settings) every cut then lands on a zero crossing.
#include "melodee/shell/context.h"
#include "melodee/shell/project_store.h"
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
                at = zero_cut(snap_to_hit(at, loaded.hits, loaded.hit_count));
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
    if (pad != ui.chop_slice) ui.chop_fine = false; // another slice: the wheel picks slices again until K1 / K2
    ui.chop_slice = uint8_t(pad);
    audition(sl.start[pad], sl.end(pad, frames));
}

// Roadmap P3: the slices kept with the project, a sample's chop of one region each (the last region chopped).
void Context::keep_chop() {
    if (ui.chop_sample >= sample_slots || !ui.slices.count) return;
    auto& kept = project.chops[ui.chop_sample];
    SampleChop now;
    now.from = ui.chop_from; now.to = ui.chop_to; now.mode = ui.chop_mode;
    now.amount = int16_t(ui.chop_amount[size_t(ui.chop_mode) % ui.chop_amount.size()]);
    now.slices = ui.slices;
    if (kept.from == now.from && kept.to == now.to && kept.mode == now.mode && kept.amount == now.amount && kept.slices.count == now.slices.count
        && kept.slices.last == now.slices.last && kept.slices.start == now.slices.start)
        return;
    kept = now;
    dirty = true;
}

bool Context::restore_chop() {
    if (ui.chop_sample >= sample_slots) return false;
    const auto& kept = project.chops[ui.chop_sample];
    if (!kept.used() || kept.from != ui.chop_from || kept.to != ui.chop_to) return false;
    ui.slices = kept.slices;
    ui.chop_mode = kept.mode;
    ui.chop_amount[size_t(kept.mode) % ui.chop_amount.size()] = kept.amount;
    ui.chop_armed = false;
    ui.chop_take_at = 0;
    return true;
}

uint32_t Context::zero_cut(uint32_t frame) const {
    if (!ui.settings.zero_snap || !library || ui.chop_sample >= sample_slots) return frame;
    return library->zero_crossing(ui.chop_sample, frame);
}

// Each cut inside the sample onto a zero crossing; the first slice's start
// and the last one's end too, unless they are the sample's own ends (its
// file starts and stops there already). A cut kept from its neighbours by
// move_start's 20 ms stays a little off the crossing.
void Context::snap_slices() {
    auto& sl = ui.slices;
    uint32_t frames = ui.chop_sample < sample_slots ? project.samples[ui.chop_sample].frames : 0;
    if (!ui.settings.zero_snap || !frames || !sl.count) return;
    for (unsigned n = 0; n < sl.count; ++n)
        if (sl.start[n]) sl.move_start(n, zero_cut(sl.start[n]), frames);
    uint32_t last = sl.end(sl.count - 1, frames);
    if (last < frames) sl.move_end(sl.count - 1, zero_cut(last), frames);
}

void Context::toggle_zero_snap() {
    ui.settings.zero_snap = !ui.settings.zero_snap;
    if (store) store->save_settings(ui.settings.text());
    if (ui.settings.zero_snap) snap_slices(); // what is there now joins in; off leaves the cuts where they are
    notice(ui.settings.zero_snap ? "Zero snap on" : "Zero snap off");
}

} // namespace melodee
