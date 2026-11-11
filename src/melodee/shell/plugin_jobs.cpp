// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Plug-ins on the MIDI thread (docs/MELODEE.md, Plug-ins): the PLUG-INS picker
// opened for a PLUGIN track's instrument or for a chain's slot, Push on a
// plug-in there, the cards of the plug-in in view, and the rack kept in step
// with the project (plugin/plugin_rack.h does the loading on its own thread).
//
// A plug-in is an entry of Project::plugins: its path on the card. A PLUGIN
// track names its entry; a Plugin effect names it in its first parameter. A
// track or slot that already has one keeps its entry for the new plug-in; a
// new one takes an entry nothing uses. The rack loads what is used and lets go
// of the rest, so undo, a project switch or a removed effect need nothing more.
#include "melodee/shell/context.h"
#include "melodee/engine/effect_info.h"
#include "melodee/plugin/plugin_files.h"
#include "melodee/plugin/plugin_rack.h"
#include "melodee/shell/sample_library.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>

namespace melodee {
using Type = Command::Type;

namespace {
// An entry for a new plug-in: one never set first, else one nothing uses now.
int free_entry(const Project& project) {
    for (unsigned e = 0; e < max_plugins; ++e) if (!project.plugins[e].used()) return int(e);
    for (unsigned e = 0; e < max_plugins; ++e) if (!plugin_used_by(project, e)) return int(e);
    return -1;
}

bool chain_page(PageId page) { return page == PageId::Rack || page == PageId::Bus || page == PageId::Master; }
} // namespace

// One instance plays one place: a track copy (or a copied chain) leaves two users on an entry, and the second and
// after each get an entry of their own with the same plug-in and its state as it is now.
void Context::split_shared_plugins() {
    std::array<bool, max_plugins> seen {};
    auto claim = [&](unsigned entry) -> int { // -1: keep it; else the entry to move to
        if (entry >= max_plugins) return -1;
        if (!seen[entry]) { seen[entry] = true; return -1; }
        int copy = free_entry(project);
        if (copy < 0 || seen[size_t(copy)]) return -1; // none free: it stays shared (and is heard twice)
        project.plugins[size_t(copy)] = project.plugins[entry];
        seen[size_t(copy)] = true;
        if (plugins) plugins->clone(entry, unsigned(copy));
        return copy;
    };
    for (unsigned t = 0; t < track_count; ++t) {
        auto& tr = project.tracks[t];
        if (tr.engine != EngineKind::Plugin) continue;
        int copy = claim(tr.plugin);
        if (copy < 0) continue;
        tr.plugin = uint8_t(copy);
        send({Type::TrackPlugin, uint8_t(t), 0, 0, int16_t(copy)});
    }
    for (unsigned n = 0; n < chain_count; ++n)
        for (unsigned k = 0; k < project.chain(n).size; ++k) {
            const auto& s = project.chain(n).slots[k];
            if (s.kind != plugin_effect_kind) continue;
            int copy = claim(unsigned(s.params[0]));
            if (copy >= 0) set_effect_param(n, k, 0, copy);
        }
}

void Context::open_plugin_picker(UiState::PluginTarget target, unsigned chain, unsigned slot) {
    if (!plugins) { notice("Plug-ins are not available"); return; }
    ui.plugin_target = target;
    ui.plugin_chain = uint8_t(chain);
    ui.plugin_slot = uint8_t(slot);
    ui.plugin_back = target == UiState::PluginTarget::Instrument ? PageId::Edit : ui.page;
    ui.plugins_cursor = 0;
    plugins->browse(""); // the Plugins folder, listed afresh
    open(PageId::Plugins);
}

void Context::choose_plugin(const PluginFile& file) {
    if (!plugins) return;
    if (file.folder || file.several()) { plugins->browse(file.path); ui.plugins_cursor = 0; return; } // a folder, or a bundle's plug-ins
    if (!file.loadable()) { notice(file.why.empty() ? "Not a plug-in for this MPC" : file.why); return; }
    bool instrument = ui.plugin_target == UiState::PluginTarget::Instrument;
    if (instrument && file.instrument == 0) { notice(file.name + " is an effect"); return; }
    if (!instrument && file.instrument == 1) { notice(file.name + " is an instrument"); return; }
    if (file.path.size() + 1 > plugin_path_length) { notice("Its path is too long"); return; }

    int entry = -1;
    unsigned n = ui.plugin_chain, k = ui.plugin_slot;
    if (instrument) {
        const auto& t = track();
        if (t.engine == EngineKind::Plugin && t.plugin < max_plugins) entry = t.plugin;
    } else {
        if (n >= chain_count || k >= project.chain(n).size) return;
        const auto& s = project.chain(n).slots[k];
        if (s.kind == plugin_effect_kind && unsigned(s.params[0]) < max_plugins) entry = s.params[0];
    }
    if (entry < 0) entry = free_entry(project);
    if (entry < 0) { notice("Every plug-in entry is in use"); return; }

    close_gesture();
    auto& ref = project.plugins[size_t(entry)];
    bool same = file.path == ref.path && file.id == ref.id;
    std::snprintf(ref.path, plugin_path_length, "%s", file.path.c_str());
    std::snprintf(ref.name, sizeof(ref.name), "%s", file.name.c_str());
    ref.id = file.id;
    if (same) plugins->reload(unsigned(entry)); // picked again: opened afresh (after it failed or stopped working)
    if (instrument) {
        unsigned t = ui.track % track_count;
        auto& tr = project.tracks[t];
        tr.plugin = uint8_t(entry);
        send({Type::TrackPlugin, uint8_t(t), 0, 0, int16_t(entry)});
        if (tr.engine != EngineKind::Plugin) load_sound(t, EngineKind::Plugin, 0);
        std::string name = file.name;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return char(std::toupper(ch)); });
        tr.set_name(name.c_str());
    } else {
        unsigned count = project.chain(n).count();
        if (k > count) k = count;
        if (project.chain(n).slots[k].kind != plugin_effect_kind) set_effect(n, k, EffectKind::Plugin);
        set_effect_param(n, k, 0, entry);
        ui.fx_cursor[UiState::cursor_of(n)] = uint8_t(k);
        ui.fx_row[UiState::cursor_of(n)] = 0;
    }
    ui.plugin_row[size_t(entry)] = 0;
    dirty = true;
    plugins->sync(project);
    notice("Loading " + file.name);
    open(ui.plugin_back);
}

void Context::set_plugin_param(int32_t id, int value) {
    if (!plugins) return;
    unsigned entry = plugin_binding_entry(id), shown = plugin_binding_shown(id);
    plugins->set_param(entry, shown, value);
    auto& panel = ui.plugin_panel;
    if (panel.entry == entry)
        for (unsigned k = 0; k < 3; ++k)
            if (panel.row * 3u + k == shown) panel.params[k].value = int16_t(std::clamp(value, 0, plugin_param_max));
    dirty = true;
}

int Context::plugin_in_focus() const {
    const auto& t = project.tracks[ui.track % track_count];
    if (ui.page == PageId::Edit && t.engine == EngineKind::Plugin && t.plugin < max_plugins) return t.plugin;
    if (chain_page(ui.page) && !ui.picker_open) {
        unsigned n = page_chain(project, ui), k = ui.fx_cursor[UiState::cursor_of(n)];
        const auto& chain = project.chain(n);
        if (k < chain.count() && chain.slots[k].kind == plugin_effect_kind && unsigned(chain.slots[k].params[0]) < max_plugins)
            return chain.slots[k].params[0];
    }
    return -1;
}

void Context::plugins_tick(uint32_t at) {
    if (!plugins) return;
    if (library) { // the card's top folder, from the sample library's Samples folder beside Plugins
        auto root = library->root();
        plugins->set_card(root.empty() ? std::string() : std::filesystem::path(root).parent_path().string());
    }
    split_shared_plugins();
    plugins->sync(project);
    if (ui.page == PageId::Plugins && plugins->changes() != plugin_view_seen) { // the picker's rows, and where going up lands
        plugin_view_seen = plugins->changes();
        auto view = plugins->view();
        ui.plugins_count = uint16_t(view.files.size());
        if (view.came_from >= 0 && !plugin_came_back) { ui.plugins_cursor = uint16_t(view.came_from); plugin_came_back = true; }
        if (ui.plugins_cursor >= ui.plugins_count) ui.plugins_cursor = uint16_t(ui.plugins_count ? ui.plugins_count - 1 : 0);
    }
    if (at - plugin_panel_at < 50) return; // the cards' texts twenty times a second
    plugin_panel_at = at;
    int entry = plugin_in_focus();
    if (entry < 0) { ui.plugin_panel = PluginPanel {}; return; }
    plugins->panel(unsigned(entry), ui.plugin_row[size_t(entry)], ui.plugin_panel);
    ui.plugin_row[size_t(entry)] = ui.plugin_panel.row; // clamped to its rows
}

} // namespace melodee
