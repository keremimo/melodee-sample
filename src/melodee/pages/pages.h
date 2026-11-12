// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/pages/page.h"
#include "melodee/ui/chrome.h"

// Each page file defines one of these; pages.cpp maps PageId to them.
namespace melodee {
// Shift + Push on Song: a marker at the cursor, or its next name (the one
// place Shift + Push is not Back; Kerem's choice).
void song_marker(Context& c);
// Song's printed buttons, with no Shift and the export sheet closed (shell/shell.cpp):
// Chop splits the clip at the cursor, Erase deletes it, Note Repeat repeats it to the next marker.
void song_chop(Context& c);
void song_erase(Context& c);
void song_repeat(Context& c);
// Loop: the brace on the clip's bars, or off. False when there was nothing to do.
bool song_loop(Context& c);
extern const PageSpec stage_page, keys_page, sounds_page, edit_page, steps_page, roll_page, motion_page, patterns_page, song_page, input_config_page,
    rack_page, sends_page, bus_page, master_page,
    pads_page, samples_page, chop_page,
    levels_page, record_page, project_page, tempo_page,
    pad_edit_page, play_edit_page, envelope_page, filter_page, follow_page, cloud_page, layers_page, mod_page,
    punch_page, pad_punch_page, settings_page, about_page, pad_fx_page, time_correct_page, step_edit_page,
    pad_mix_page, pad_filter_page, filter_env_page, knob_fx_page, compressor_page, flex_beat_page,
    fader_page, midi_config_page, card_access_page, tracks_page, looper_page, rec_quantize_page, plugins_page,
    plugin_presets_page;
// Shift + pad on Samples: the file under the cursor onto the next empty pad (false elsewhere).
bool samples_shift_pad(Context& c);
// The sampler's ten pages share their soft keys: B1 / B2 / B3 their groups, as the MPC (sampler_common.cpp, shell/sampler_map.h).
ui::SoftKeys sampler_keys(PageId page, juce::Colour colour);
void sampler_soft(Context& c, int key);
}
