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
extern const PageSpec stage_page, keys_page, mix_page, sounds_page, edit_page, steps_page, roll_page, motion_page, patterns_page, song_page, input_config_page,
    rack_page, sends_page, bus_page, master_page,
    pads_page, samples_page, trim_page, chop_page,
    levels_page, record_page, project_page, tempo_page, arp_page, chord_page, capture_page,
    pad_edit_page, play_edit_page, envelope_page, filter_page, follow_page, cloud_page,
    punch_page, pad_punch_page, settings_page, about_page, pad_fx_page, time_correct_page, step_edit_page,
    pad_mix_page, pad_filter_page, filter_env_page;
// The sampler's ten pages share their soft keys: the neighbours and PAGE n / 10 (sampler_common.cpp).
ui::SoftKeys sampler_keys(PageId page, juce::Colour colour);
void sampler_soft(Context& c, int key);
}
