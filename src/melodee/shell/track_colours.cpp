// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// TRACK COLOUR's sheet (shell/track_colours.h) on the MIDI thread.
#include "melodee/shell/track_colours.h"
#include "melodee/shell/context.h"
#include <string>

namespace melodee {
namespace {
using B = SurfaceButton;

// The colour shown at once: the screen and the pads read it through the slot table (a sequence's, the project).
void set_colour(Context& c, uint8_t colour) {
    if (c.ui.colour_seq) {
        auto& scene = c.project.scenes[c.ui.colour_scene % pattern_count];
        if (scene.colour == colour) return;
        scene.colour = colour;
        c.dirty = true;
        return;
    }
    auto& track = c.project.tracks[c.ui.colour_track % track_count];
    if (track.colour == colour) return;
    track.colour = colour;
    show_track_colours(c.project);
    c.dirty = true;
}

void pick_slot(Context& c, unsigned slot) {
    set_colour(c, uint8_t(slot % 16 + 1));
    c.notice(ui::track_colour_names[slot % 16]);
}

// DONE: one undo step when it changed, and the sheet closes.
void keep(Context& c) {
    auto& ui = c.ui;
    ui.colour_sheet = false;
    if (ui.colour_seq) { c.scene_colour_kept(ui.colour_scene, ui.colour_before); return; }
    const unsigned t = ui.colour_track % track_count;
    const uint8_t now = c.project.tracks[t].colour;
    if (now == ui.colour_before) return;
    c.project.tracks[t].colour = ui.colour_before; // the edit takes it as it was, then as it is
    Edit edit = c.begin_edit(("Colour track " + std::to_string(t + 1)).c_str(), t, -1, false);
    edit.colour_before = ui.colour_before;
    edit.colour_after = now;
    c.project.tracks[t].colour = now;
    c.edits.push(std::move(edit));
    c.dirty = true;
}
} // namespace

void track_colour_open(Context& c) {
    auto& ui = c.ui;
    const unsigned t = ui.track % track_count;
    if (!c.project.tracks[t].used()) { c.notice("Track " + std::to_string(t + 1) + " is empty"); return; }
    ui.colour_sheet = true;
    ui.colour_seq = false;
    ui.colour_track = uint8_t(t);
    ui.colour_before = c.project.tracks[t].colour;
}

void sequence_colour_open(Context& c) {
    auto& ui = c.ui;
    const unsigned scene = c.project.scene % pattern_count; // the sequence in hand
    ui.colour_sheet = true;
    ui.colour_seq = true;
    ui.colour_scene = uint8_t(scene);
    ui.colour_before = c.project.scenes[scene].colour;
}

bool track_colour_button(Context& c, SurfaceButton button) {
    auto& ui = c.ui;
    if (!ui.colour_sheet) return false;
    if (button == B::Play || button == B::Stop || button == B::Shift) return false; // the transport still answers
    switch (button) {
    case B::TopLeft: // BACK: as it was
        set_colour(c, ui.colour_before);
        ui.colour_sheet = false;
        break;
    case B::TopMiddle: // DEFAULT: the place's colour, which follows the track's place; a sequence's NONE: white
        set_colour(c, 0);
        if (ui.colour_seq) c.notice("SEQ " + std::to_string(ui.colour_scene % pattern_count + 1) + ": no colour");
        else c.notice(std::string(ui::track_colour_names[ui.colour_track % 16]) + ": default");
        break;
    case B::TopRight: case B::WheelPush: keep(c); break;
    default: break; // the rest wait
    }
    return true;
}

void track_colour_wheel(Context& c, int delta) {
    if (!c.ui.colour_sheet || !delta) return;
    if (c.ui.colour_seq) {
        const uint8_t now = sequence_colour_step(c.project.scenes[c.ui.colour_scene % pattern_count].colour, delta);
        pick_slot(c, now - 1u);
        return;
    }
    const unsigned t = c.ui.colour_track % track_count;
    pick_slot(c, colour_slot_step(track_colour_slot(c.project.tracks[t], t), delta));
}

void track_colour_pad(Context& c, unsigned pad) {
    if (c.ui.colour_sheet) pick_slot(c, pad % 16);
}

} // namespace melodee
