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
// another one jumps to the first page of its own, one with no pages does nothing. Each track keeps its own
// button and page. Pure, for the pages, the shell and the tests.
namespace melodee {

struct StageAt { unsigned group = 0, sub = 0; };

inline unsigned stage_count(const StageGroups& g, unsigned group) { return std::max<unsigned>(g.groups[group % 3].count, 1); }
// Whether a button has pages at all (an empty table's has none: it does nothing).
inline bool stage_has(const StageGroups& g, unsigned group) { return g.groups[group % 3].count > 0; }
// Whether a button opens the Edit page instead of a Stage page (a MIDI track's B3 EDIT).
inline bool stage_opens_edit(const Track& t, unsigned key) {
    const auto& g = stage_groups(t.engine).groups[key % 3];
    return g.count && g.pages[0].kind == StageKind::OpenEdit;
}

// A kit whose last pad holds a multisampled instrument (an .sfz, a keygroup, an .sf2 preset, an MPC drum program's
// pad "<file>#<pad>"): its zones are its layers, picked by key and velocity, so its Stage has no LAYERS (TRIM is ZONES).
inline bool stage_instrument(const Project& p, const Track& t) {
    if (t.engine != EngineKind::Sampler) return false;
    const auto& pad = t.pads[t.last_pad % sampler_pads];
    return pad.used() && p.samples[pad.sample % sample_slots].instrument();
}
// Whether Stage passes over a page: LAYERS on an instrument pad.
inline bool stage_skips(const StageGroups& g, unsigned group, unsigned sub, bool instrument) {
    return instrument && g.groups[group % 3].pages[sub % stage_count(g, group)].kind == StageKind::Layers;
}
// A button's pages as its dots count them, and a page's place among them (a page passed over is not one).
inline unsigned stage_shown(const StageGroups& g, unsigned group, bool instrument) {
    unsigned n = 0;
    for (unsigned sub = 0; sub < stage_count(g, group); ++sub) n += !stage_skips(g, group, sub, instrument);
    return n;
}
inline unsigned stage_shown_at(const StageGroups& g, unsigned group, unsigned sub, bool instrument) {
    unsigned n = 0;
    for (unsigned k = 0; k < sub; ++k) n += !stage_skips(g, group, k, instrument);
    return n;
}

// `instrument`: stage_instrument() for the track.
inline StageAt stage_at(const UiState& ui, const Track& t, bool instrument) {
    const auto& g = stage_groups(t.engine);
    unsigned track = ui.track % track_count, group = ui.stage_group[track] % 3;
    if (!stage_has(g, group) || stage_opens_edit(t, group)) group = 0; // left on a button with no Stage pages under it
    unsigned sub = ui.stage_sub[track] % stage_count(g, group);
    if (stage_skips(g, group, sub, instrument)) sub = 0; // left on LAYERS: an instrument pad shows ZONES, a sample pad LAYERS again
    return {group, sub};
}

// Where a kind of page sits on an engine's Stage (the first one of that kind); found false when it has none.
struct StageFound { StageAt at; bool found = false; };
inline StageFound stage_find(EngineKind engine, StageKind kind) {
    const auto& g = stage_groups(engine);
    for (unsigned group = 0; group < 3; ++group)
        for (unsigned sub = 0; sub < g.groups[group].count; ++sub)
            if (g.groups[group].pages[sub].kind == kind) return {{group, sub}, true};
    return {};
}

inline const StagePage& stage_page_at(const Track& t, unsigned group, unsigned sub) {
    const auto& g = stage_groups(t.engine);
    return g.groups[group % 3].pages[sub % stage_count(g, group)];
}

inline const StagePage& stage_page_now(const UiState& ui, const Track& t, bool instrument) {
    auto at = stage_at(ui, t, instrument);
    return stage_page_at(t, at.group, at.sub);
}

// B1-B3 (`key` 0-2) on Stage.
inline void stage_press(UiState& ui, const Track& t, unsigned key, bool instrument) {
    const auto& g = stage_groups(t.engine);
    unsigned track = ui.track % track_count;
    auto at = stage_at(ui, t, instrument);
    key %= 3;
    if (!stage_has(g, key) || stage_opens_edit(t, key)) return; // the page opens Edit (stage_page.cpp)
    if (at.group == key) {
        unsigned count = stage_count(g, key), sub = (at.sub + 1) % count;
        while (sub != at.sub && stage_skips(g, key, sub, instrument)) sub = (sub + 1) % count;
        ui.stage_sub[track] = uint8_t(sub);
        return;
    }
    ui.stage_group[track] = uint8_t(key);
    ui.stage_sub[track] = 0;
}

// Puts the selected track on a page of Stage (Shift + Pad 13 and the old Trim's openers: a kit's TRIM).
inline void stage_go(UiState& ui, unsigned group, unsigned sub) {
    ui.stage_group[ui.track % track_count] = uint8_t(group % 3);
    ui.stage_sub[ui.track % track_count] = uint8_t(sub);
}
// The same by the kind of page; false (nothing moves) when the engine's Stage has none.
inline bool stage_go(UiState& ui, EngineKind engine, StageKind kind) {
    auto f = stage_find(engine, kind);
    if (f.found) stage_go(ui, f.at.group, f.at.sub);
    return f.found;
}

// The card the wheel fine-edits on Stage, and Push moves on (the MPC's encoder press): the pot touched last.
// On an empty track it is also the door Push makes (K1 a synth, K2 a kit, K3 a MIDI track); there the wheel moves it.
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
constexpr unsigned new_sources = 2; // samples on the card, record the input (Audio Recall and Resample stay on their keys)

} // namespace melodee
