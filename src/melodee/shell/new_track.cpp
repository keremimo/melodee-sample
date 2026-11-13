// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/new_track.h"
#include "melodee/engine/midi_engine.h"
#include "melodee/shell/context.h"
#include "melodee/shell/sample_library.h"
#include "melodee/shell/stage_map.h"
#include "melodee/shell/track_copy.h"
#include <algorithm>
#include <memory>

namespace melodee {

void offer_new_track(Context& c) {
    auto& ui = c.ui;
    if (c.track().used() || ui.recording) return;
    if (!ui.new_open) { ui.new_back = ui.page; ui.new_pick = 0; }
    ui.new_open = true;
    ui.new_return = -1;
}

bool new_track_button(Context& c, SurfaceButton button) {
    using B = SurfaceButton;
    auto& ui = c.ui;
    if (!ui.new_open) return false;
    if (button == B::Play || button == B::Stop || button == B::PadBank) return false;
    if (button == B::TopLeft) { ui.new_open = false; return true; } // BACK: the track stays empty
    if (button == B::WheelPush) make_new_track(c, NewChoice(ui.new_pick % new_choice_count));
    return true; // the rest wait
}

void new_track_wheel(UiState& ui, int delta) {
    ui.new_pick = uint8_t(std::clamp<int>(int(ui.new_pick) + delta, 0, int(new_choice_count) - 1));
}

void make_new_track(Context& c, NewChoice choice) {
    auto& ui = c.ui;
    ui.new_open = false;
    if (c.track().used()) return;
    PageId back = ui.new_back;
    if (choice == NewChoice::Plugin) { // the card's instruments; the one picked loads and this page comes back
        c.open_plugin_picker(UiState::PluginTarget::Instrument);
        ui.plugin_back = back;
        return;
    }
    // One undo for the track made (Shift + Minus leaves it empty again), as Load Kit.
    unsigned t = ui.track % track_count;
    Edit edit = c.begin_edit("New track", t, -1, false);
    edit.motion_before.reset();
    edit.track_before = std::make_unique<TrackState>(track_state(c, t));
    auto kit = [&] {
        c.load_sound(ui.track, EngineKind::Sampler, 0);
        c.track().set_name("KIT");
        stage_go(ui, 0, 0); // TRIM when Stage opens
    };
    auto midi = [&](unsigned port) {
        c.load_sound(ui.track, EngineKind::Midi, 0);
        c.set_param(ui.track, midi_track::Port, int(port));
        c.set_param(ui.track, midi_track::Channel, std::clamp<int>(ui.new_channel, 1, 16));
        stage_go(ui, 0, 0); // DEVICE
    };
    switch (choice) {
    case NewChoice::Kit: kit(); break;
    case NewChoice::Samples: // Sample Select; the first sample loaded comes back here
        kit();
        c.open(PageId::Samples);
        if (c.library) c.library->rescan();
        ui.new_return = int8_t(ui.track);
        break;
    case NewChoice::Record: kit(); c.open(PageId::Record); break; // Sample Record
    case NewChoice::MidiOut: midi(0); break;
    case NewChoice::MidiUsb: midi(1); break;
    case NewChoice::MidiAllUsb: midi(2); break;
    default: return;
    }
    edit.track_after = std::make_unique<TrackState>(track_state(c, t));
    c.edits.push(std::move(edit));
    c.dirty = true;
}

void new_track_landed(Context& c) {
    auto& ui = c.ui;
    if (ui.new_return < 0 || unsigned(ui.new_return) != ui.track % track_count) return;
    ui.new_return = -1;
    if (ui.page == PageId::Samples) c.open(ui.new_back);
}

} // namespace melodee
