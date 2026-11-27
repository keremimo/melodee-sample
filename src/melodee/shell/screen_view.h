// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Which plug-in's own screen (plugin/plugin_screen.h, docs/PLUGIN_SCREENS_PLAN.md) the page in view shows, and as
// what: its page under B1-B3 (Stage's PANEL on its PLUGIN track; an effect slot's full page, Push on it), or an
// effect's strip under its chain's slots. Settings > SCREEN > Plug-in
// screens off: none. Pure, for the shell (what the rack is told) and the pages (what is drawn), so both agree.
#pragma once
#include "melodee/shell/bindings.h"
#include "melodee/shell/features.h"
#include "melodee/shell/stage_map.h"
#include "melodee/shell/ui_state.h"

namespace melodee {

constexpr bool fx_chain_page(PageId page) { return page == PageId::Rack || page == PageId::Bus || page == PageId::Master; }

struct ScreenPlace {
    int entry = -1;   // Project::plugins, -1 none
    uint8_t area = 0; // 0 its page under B1-B3, 1 its strip under a chain's slots
};

inline ScreenPlace plugin_screen_place(const Project& project, const UiState& ui) {
    const auto& panel = ui.plugin_panel;
    if (!ui.settings.plugin_skins || !panel.has_screen || panel.status != PluginStatus::Ready) return {};
    const auto& t = project.tracks[ui.track % track_count];
    const bool own = t.engine == EngineKind::Plugin && t.plugin == panel.entry;
    if (ui.page == PageId::Stage)
        return own && stage_page_now(ui, t, stage_skip(project, ui, t)).kind == StageKind::PluginScreen ? ScreenPlace {t.plugin, 0} : ScreenPlace {};
    if (fx_chain_page(ui.page) && !ui.picker_open && !ui.sheet_open) {
        const unsigned n = page_chain(project, ui), k = ui.fx_cursor[UiState::cursor_of(n)];
        const auto& chain = project.chain(n);
        if (k < chain.count() && chain.slots[k].kind == plugin_effect_kind && !chain.slots[k].bypass
            && unsigned(chain.slots[k].params[0]) == panel.entry)
            return {int(panel.entry), uint8_t(ui.fx_panel ? 0 : 1)};
    }
    return {};
}

} // namespace melodee
