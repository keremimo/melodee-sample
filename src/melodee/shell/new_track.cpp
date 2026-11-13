// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/new_track.h"
#include "melodee/engine/midi_engine.h"
#include "melodee/shell/context.h"
#include "melodee/shell/sample_library.h"
#include "melodee/shell/stage_map.h"
#include "melodee/shell/track_copy.h"
#include <algorithm>
#include <cstring>
#include <memory>

namespace melodee {

void offer_new_track(Context& c) {
    auto& ui = c.ui;
    if (c.track().used() || ui.recording) return;
    bool doors = ui.new_for >= 0 && ui.page == PageId::Stage; // another empty track picked on the doors: the same page to go back to
    if (!doors) ui.new_back = ui.page;
    ui.new_for = int8_t(ui.track % track_count);
    c.open(PageId::Stage);
    if (!doors) { ui.new_pick_from = float(ui.new_pick % new_choice_count) + 0.9f; ui.new_pick_at = c.now ? c.now : 1; } // the cards slide in
}

void make_new_track(Context& c, NewChoice choice) {
    auto& ui = c.ui;
    unsigned t = ui.track % track_count;
    if (c.track().used()) return;
    bool picked = ui.new_for == int8_t(t);
    PageId back = ui.new_back;
    if (choice == NewChoice::Plugin) { // the card's instruments: the track turns PLUGIN once one is picked (choose_plugin)
        c.open_plugin_picker(UiState::PluginTarget::Instrument);
        if (picked) ui.plugin_back = back;
        return;
    }
    // One undo for the track made (Shift + Minus leaves it empty again), as Load Kit.
    Edit edit = c.begin_edit("New track", t, -1, false);
    edit.motion_before.reset();
    edit.track_before = std::make_unique<TrackState>(track_state(c, t));
    auto kit = [&] {
        c.load_sound(t, EngineKind::Sampler, 0);
        c.track().set_name("KIT");
        stage_go(ui, 0, 0); // TRIM
    };
    auto midi = [&](unsigned port) {
        c.load_sound(t, EngineKind::Midi, 0);
        c.set_param(t, midi_track::Port, int(port));
        c.set_param(t, midi_track::Channel, std::clamp<int>(ui.new_channel, 1, 16));
        stage_go(ui, 0, 0); // DEVICE
    };
    switch (choice) {
    case NewChoice::Kit: kit(); break;
    case NewChoice::Samples: kit(); break;
    case NewChoice::Record: kit(); break;
    case NewChoice::MidiOut: midi(0); break;
    case NewChoice::MidiUsb: midi(1); break;
    case NewChoice::MidiAllUsb: midi(2); break;
    default: return;
    }
    edit.track_after = std::make_unique<TrackState>(track_state(c, t));
    c.edits.push(std::move(edit));
    c.dirty = true;
    if (choice == NewChoice::Samples) { // Sample Select: the first sample loaded goes back (new_track_landed)
        c.open(PageId::Samples);
        ui.new_kit = int8_t(t); // CANCEL there leaves it empty
        if (c.library) c.library->rescan();
        return;
    }
    if (choice == NewChoice::Record) { ui.new_for = -1; c.open(PageId::Record); return; } // Sample Record: a take of its own
    if (picked) { ui.new_for = -1; c.open(back); }
}

void new_track_landed(Context& c) {
    auto& ui = c.ui;
    if (ui.new_for != int8_t(ui.track % track_count) || ui.page != PageId::Samples) return;
    ui.new_for = -1;
    c.open(ui.new_back);
}

void samples_cancel(Context& c) {
    auto& ui = c.ui;
    unsigned t = ui.track % track_count;
    const auto& tr = c.project.tracks[t];
    bool bare = tr.engine == EngineKind::Sampler && std::none_of(tr.pads.begin(), tr.pads.end(), [](const auto& p) { return p.used(); });
    const Edit* last = c.edits.latest();
    if (ui.new_kit == int8_t(t) && bare && last && last->track == t && !std::strcmp(last->what.c_str(), "New track"))
        c.undo(); // the kit made for its first sample: empty again
    ui.new_kit = -1;
    c.back();
}

} // namespace melodee
