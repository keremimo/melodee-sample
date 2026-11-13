// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"

namespace melodee {

PadJob pad_job(const UiState& ui) {
    return page_spec(ui.page).pad;
}

const PageSpec& page_spec(PageId page) {
    switch (page) {
    case PageId::Keys: return keys_page;
    case PageId::Sounds: return sounds_page;
    case PageId::Edit: return edit_page;
    case PageId::Steps: return steps_page;
    case PageId::Roll: return roll_page;
    case PageId::Motion: return motion_page;
    case PageId::Patterns: return patterns_page;
    case PageId::Song: return song_page;
    case PageId::InputConfig: return input_config_page;
    case PageId::Rack: return rack_page;
    case PageId::Sends: return sends_page;
    case PageId::Bus: return bus_page;
    case PageId::Master: return master_page;
    case PageId::Pads: return pads_page;
    case PageId::Samples: return samples_page;
    case PageId::Chop: return chop_page;
    case PageId::Record: return record_page;
    case PageId::Project: return project_page;
    case PageId::Tempo: return tempo_page;
    case PageId::PadPage: return pad_edit_page;
    case PageId::PlayPage: return play_edit_page;
    case PageId::LayersPage: return layers_page;
    case PageId::ModPage: return mod_page; // roadmap P4
    case PageId::Scenes: return scenes_page; // roadmap P5
    case PageId::EnvelopePage: return envelope_page;
    case PageId::FilterPage: return filter_page;
    case PageId::FollowPage: return follow_page;
    case PageId::CloudPage: return cloud_page;
    case PageId::Punch: return punch_page;
    case PageId::PadPunch: return pad_punch_page;
    case PageId::SettingsPage: return settings_page;
    case PageId::About: return about_page;
    case PageId::PadFx: return pad_fx_page;
    case PageId::TimeCorrect: return time_correct_page;
    case PageId::StepEdit: return step_edit_page;
    case PageId::PadMix: return pad_mix_page;
    case PageId::PadFilterPage: return pad_filter_page;
    case PageId::FilterEnvPage: return filter_env_page;
    case PageId::KnobFx: return knob_fx_page;
    case PageId::Compressor: return compressor_page;
    case PageId::FlexBeat: return flex_beat_page;
    case PageId::FaderPage: return fader_page;
    case PageId::RecQuantize: return rec_quantize_page;
    case PageId::Plugins: return plugins_page;
    case PageId::PluginPresets: return plugin_presets_page;
    case PageId::MidiConfig: return midi_config_page;
    case PageId::CardAccess: return card_access_page;
    case PageId::Tracks: return tracks_page;
    case PageId::Looper: return looper_page;
    case PageId::Print: return print_page; // freeze plan F2c
    case PageId::Trim: // lives on Stage now (Context::open lands there)
    case PageId::Stage:
    default: return stage_page;
    }
}

} // namespace melodee
