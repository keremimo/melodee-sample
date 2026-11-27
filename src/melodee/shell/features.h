// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/pattern.h"
#include "melodee/shell/stage_map.h"
#include "melodee/shell/ui_state.h"

// Basic and Full (Settings, K1 FEATURES; Settings::full_features). Full shows every page. Basic shows the stock
// app's pages plus tracks, plug-ins, the mixer and Song, and leaves out what Full adds:
//   the MENU's GO      MOTION and LOOPER (the other tiles keep their pads; pad 10 is free since the mixer)
//   the MENU's HERE    LOOPER, FREEZE and FOLLOW TO ALL
//   Seq's keys         B3 MOTION, unless the pattern in hand holds motion; MOD and MACRO after it
//   the mixer          EQ, COMP, FX and the submixes: SENDS shows send A, Shift pan (decision 3)
//   Stage              a kit's SENDS (the pads' own); LAYERS, COLOUR, FOLLOW, CLOUD and KIT FILT too, so a kit pad has
//                      the stock app's seven pages (Trim, Tune, Amp Env, Play, Mix, Filter, Filt Env)
//   Patterns           Shift + B2 FOLLOW and Shift + B3 VARY; on Sequences the follow cards and Shift + B3 ALL
//   the Looper         Loop held's grid and Loop + 16 Levels (Loop tapped stays the last pad's loop)
// Nothing is deleted or changed either way: what a project holds keeps playing, and Full shows it again. Melodee
// starts on Full; Basic only once it is picked (shell/settings.h). Pure, for the shell, the screen and the tests.
namespace melodee {

// A lesson (shell/lesson.h) shows Basic's pages whatever the setting, so it reads the same on both.
inline bool full_features(const UiState& ui) { return ui.settings.full_features && !ui.lesson; }

// The MENU's GO: a tile's place is shown. MOTION (pad 8), SENDS (pad 10) and LOOPER (pad 12) are Full's.
inline bool menu_tile_shown(const UiState& ui, unsigned tile) {
    tile %= 16;
    if (tile == 9) return false; // SENDS, gone with the mixer (docs/MIXER_PLAN.md step 6): free
    return full_features(ui) || (tile != 7 && tile != 11);
}
// Seq's B3 MOTION: Full, or the pattern in hand holds motion.
inline bool motion_shown(const UiState& ui, const Pattern& pattern) { return full_features(ui) || !pattern.motion.empty(); }
// The Looper (its page, Loop held's grid, Loop + 16 Levels), the follow actions' cards and keys, VARY, FREEZE in
// the MENU.
inline bool looper_shown(const UiState& ui) { return full_features(ui) || ui.page == PageId::Looper; }
// Loop held's grid on the pads, on any page but the Looper's own.
inline bool loop_grid_held(const UiState& ui) { return ui.loop_held && ui.page != PageId::Looper && full_features(ui); }
inline bool follow_shown(const UiState& ui) { return full_features(ui); }
// MOD and MACRO, the pages after MOTION on Seq's B3 (pages/mod_page.cpp): Full's, or the one open.
inline bool mod_shown(const UiState& ui) { return full_features(ui) || ui.page == PageId::ModPage; }
inline bool vary_shown(const UiState& ui) { return full_features(ui); }
inline bool freeze_shown(const UiState& ui) { return full_features(ui); }

// Stage's pages Basic passes over, a bit a StageKind.
constexpr uint64_t stage_bit(StageKind kind) { return uint64_t(1) << unsigned(kind); }
constexpr uint64_t basic_stage_skips = stage_bit(StageKind::Sends) | stage_bit(StageKind::Layers) | stage_bit(StageKind::Colour)
                                       | stage_bit(StageKind::Follow) | stage_bit(StageKind::Cloud) | stage_bit(StageKind::KitFilter);
// What Stage passes over for a track now: MIX, SENDS but on a kit, LAYERS on an instrument pad, and in Basic the
// pages above.
// PANEL (a plug-in's own screen, shell/screen_view.h) is passed over unless the track's plug-in draws one, as its
// panel says, and Settings > SCREEN > Plug-in screens is on.
inline bool plugin_screen_staged(const UiState& ui, const Track& track) {
    const auto& panel = ui.plugin_panel;
    return ui.settings.plugin_skins && track.engine == EngineKind::Plugin && panel.entry == track.plugin && panel.has_screen
        && panel.status == PluginStatus::Ready;
}
inline StageSkip stage_skip(const Project& project, const UiState& ui, const Track& track) {
    uint64_t kinds = full_features(ui) ? 0 : basic_stage_skips;
    // Since the mixer (docs/MIXER_PLAN.md step 6, decision 4): no MIX page on any track, SENDS on a kit only (its pads'
    // own sends; the track's are the mixer's).
    kinds |= stage_bit(StageKind::Mix);
    if (track.engine != EngineKind::Sampler) kinds |= stage_bit(StageKind::Sends);
    if (!plugin_screen_staged(ui, track)) kinds |= stage_bit(StageKind::PluginScreen);
    return StageSkip {stage_instrument(project, track), kinds};
}

} // namespace melodee
