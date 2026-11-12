// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/engine_info.h"
#include "melodee/shell/ui_state.h"
#include <algorithm>

// Stage's pages for the selected track (engine/engine_info.h, stage_groups()):
// which button (B1-B3) and page of its cycle the track is on, the page itself
// (a 909's VOICE is the voice the last pad played), and what B1-B3 do there,
// as the MPC's Sample Mode: the lit button again steps on through its cycle,
// another one jumps to the first page of its own. Each track keeps its own
// button and page. Pure, for the pages, the shell and the tests.
namespace melodee {

struct StageAt { unsigned group = 0, sub = 0; };

inline unsigned stage_count(const StageGroups& g, unsigned group) { return std::max<unsigned>(g.groups[group % 3].count, 1); }

inline StageAt stage_at(const UiState& ui, const Track& t) {
    const auto& g = stage_groups(t.engine);
    unsigned track = ui.track % track_count, group = ui.stage_group[track] % 3;
    return {group, ui.stage_sub[track] % stage_count(g, group)};
}

// A 909's voice the last pad played (pads 1-11 as kit909::Drum), the kick before any.
inline unsigned stage_voice(const Track& t) {
    const auto& g = stage_groups(t.engine);
    if (!g.pad_voice) return 0;
    int pad = int(t.last_note) - 36;
    int voice = pad >= 0 && pad < 16 ? g.pad_voice[pad] : -1;
    return voice < 0 ? 0 : unsigned(voice);
}

inline const StagePage& stage_page_at(const Track& t, unsigned group, unsigned sub) {
    const auto& g = stage_groups(t.engine);
    const auto& page = g.groups[group % 3].pages[sub % stage_count(g, group)];
    if (page.kind == StageKind::Voice && g.voices) return g.voices[stage_voice(t)];
    return page;
}

inline const StagePage& stage_page_now(const UiState& ui, const Track& t) {
    auto at = stage_at(ui, t);
    return stage_page_at(t, at.group, at.sub);
}

// B1-B3 (`key` 0-2) on Stage.
inline void stage_press(UiState& ui, const Track& t, unsigned key) {
    unsigned track = ui.track % track_count;
    auto at = stage_at(ui, t);
    key %= 3;
    if (at.group == key) { ui.stage_sub[track] = uint8_t((at.sub + 1) % stage_count(stage_groups(t.engine), key)); return; }
    ui.stage_group[track] = uint8_t(key);
    ui.stage_sub[track] = 0;
}

// Puts the selected track on a page of Stage (Shift + Pad 13 and the old Trim's openers: a kit's TRIM).
inline void stage_go(UiState& ui, unsigned group, unsigned sub) {
    ui.stage_group[ui.track % track_count] = uint8_t(group % 3);
    ui.stage_sub[ui.track % track_count] = uint8_t(sub);
}

// The card the wheel fine-edits on Stage, and Push moves on (the MPC's encoder press): the pot touched last.
// On an empty track it is also the door Push makes (K1 a synth, K2 a kit, K3 a MIDI track).
inline int stage_focus(const UiState& ui, const Track&) { return ui.last_pot >= 0 ? ui.last_pot % 3 : 0; }

// The sampler page a kit's Stage page edits the same values as (their bindings and undo scope are shared).
constexpr PageId stage_kit_page(StageKind kind) {
    switch (kind) {
    case StageKind::Trim: return PageId::Trim;
    case StageKind::PadMix: return PageId::PadMix;
    case StageKind::AmpEnv: return PageId::EnvelopePage;
    case StageKind::Tune: return PageId::PadPage;
    case StageKind::Play: return PageId::PlayPage;
    case StageKind::Filter: return PageId::PadFilterPage;
    case StageKind::FiltEnv: return PageId::FilterEnvPage;
    case StageKind::Follow: return PageId::FollowPage;
    case StageKind::Cloud: return PageId::CloudPage;
    case StageKind::KitFilter: return PageId::FilterPage;
    case StageKind::Layers: return PageId::LayersPage;
    default: return PageId::Count;
    }
}

// PRESET: a turned sound loads once the turning has stopped this long (as mpc-sample-plugins' Synth presets).
constexpr uint32_t stage_preset_ms = 250;

// The empty track's KIT sources, as the MPC gets a sample onto a pad.
constexpr unsigned new_sources = 4; // samples on the card, record the input, Audio Recall, Resample

} // namespace melodee
