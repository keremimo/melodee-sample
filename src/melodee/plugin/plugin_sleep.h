// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include "melodee/plugin/plugin_panel.h"
#include <cstdint>
#include <string>
#include <vector>

// What the plug-in rack (plugin/plugin_rack.h) decides for one entry at each
// sync(), apart from the plug-in itself, so the host tests can follow it.
// Freeze plan F2d: an entry used only by deep-frozen tracks sleeps. It is not
// loaded (a project opened with it never opens it), or a loaded one is let
// go with its state taken first; its state is then kept as it is, saved with
// the project untouched, and the entry loads with it when its track thaws.
namespace melodee {

// `entry` used by a track not in `asleep` (a bit a track: its plug-in or a plug-in effect on its chain), or by a
// bus's or the master's chain.
inline bool plugin_used_awake(const Project& project, unsigned entry, uint32_t asleep) {
    for (unsigned t = 0; t < track_count; ++t) {
        if ((asleep >> t) & 1) continue;
        const auto& track = project.tracks[t];
        if (track.engine == EngineKind::Plugin && track.plugin == entry) return true;
    }
    for (unsigned n = 0; n < chain_count; ++n) {
        if (n < track_count && ((asleep >> n) & 1)) continue;
        for (const auto& s : project.chain(n).slots)
            if (s.kind == plugin_effect_kind && unsigned(s.params[0]) == entry) return true;
    }
    return false;
}

// One entry's bookkeeping (the rack's Entry adds the plug-in, its error and its file).
struct PluginPlan {
    std::string path;                 // what is loaded or loading; empty: none
    int32_t id = 0;                   // which plug-in of its bundle (0: the first)
    uint32_t generation = 0;          // bumped at each change, so a slow load that lost the race is dropped
    double rate = 0;                  // what it was opened at
    PluginStatus status = PluginStatus::None;
    std::vector<uint8_t> state;       // to load with
    bool reload = false;              // a new project: open it again (with `state`) even where the path is the same
    bool asleep = false;              // wanted, but only by deep-frozen tracks: not loaded, `state` kept
};

// Keep: nothing to do. Changed: its bookkeeping moved, nothing to load or let go. Load, Unload: as they say.
// Sleep: the plug-in loaded let go, its state taken first (the rack's job). Rerate: Load, after the caller took the
// loaded plug-in's state into `state` (a new device rate keeps it).
enum class PluginStep : uint8_t { Keep, Changed, Load, Unload, Sleep, Rerate };

// `want`, `id`: what the project names for the entry ("" nothing uses it); `only_asleep`: every use is a
// deep-frozen track's; `loaded`: a plug-in is in place now.
inline PluginStep plan_plugin(PluginPlan& e, const std::string& want, int32_t id, bool only_asleep, double rate, bool loaded) {
    const bool same = want == e.path && id == e.id;
    if (!want.empty() && only_asleep) {
        if (e.asleep && same) return PluginStep::Keep;
        const bool busy = loaded || e.status == PluginStatus::Loading;
        if (!same && !e.path.empty() && !e.reload) e.state.clear(); // another plug-in in this entry: its own defaults
        e.path = want;
        e.id = id;
        e.asleep = true;
        e.reload = false;
        ++e.generation; // a load under way is dropped
        e.status = PluginStatus::None;
        if (!busy) return PluginStep::Changed;
        return same ? PluginStep::Sleep : PluginStep::Unload;
    }
    const bool woke = e.asleep && !want.empty();
    e.asleep = false;
    const bool rerate = !want.empty() && same && e.rate != 0 && e.rate != rate && e.status == PluginStatus::Ready;
    const bool again = (e.reload || woke) && !want.empty();
    e.reload = false;
    if (same && !rerate && !again) return PluginStep::Keep;
    const bool keep_live = rerate && loaded;
    // Another plug-in in this entry: its own defaults; but a project just opened (`reload`) brought its own state.
    if (!keep_live && !same && !e.path.empty() && !want.empty() && !again) e.state.clear();
    e.path = want;
    e.id = id;
    e.rate = rate;
    ++e.generation;
    if (want.empty()) {
        e.status = PluginStatus::None;
        e.state.clear();
        return PluginStep::Unload;
    }
    e.status = PluginStatus::Loading;
    return keep_live ? PluginStep::Rerate : PluginStep::Load;
}

} // namespace melodee
