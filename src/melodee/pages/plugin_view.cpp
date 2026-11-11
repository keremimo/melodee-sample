// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// A plug-in over the cards (docs/MELODEE.md, Plug-ins), for Edit on a PLUGIN
// track and for a plug-in effect on the chain pages: its name, its format and
// what it is, then while it opens "Opening", if it could not "Could not open"
// and why, if its 32-bit bridge died "Stopped working"; once open, the row of
// parameters on K1-K3 out of how many (the wheel or Minus / Plus step them)
// on the cards, and the share of each audio block's time it takes.
#include "melodee/pages/common.h"
#include "melodee/ui/widgets.h"
#include <filesystem>

namespace melodee {
using namespace ui;

juce::String plugin_name(const Snapshot& s, unsigned entry) {
    if (entry >= max_plugins) return "Plug-in";
    const auto& panel = s.ui.plugin_panel;
    if (panel.entry == entry && panel.name[0]) return juce::String::fromUTF8(panel.name);
    const auto& ref = s.project.plugins[entry];
    if (!ref.used()) return "Plug-in";
    if (ref.name[0]) return juce::String::fromUTF8(ref.name); // as the picker named it
    return juce::String(std::filesystem::path(ref.path).stem().string());
}

void draw_plugin_view(Paint& p, const Snapshot& s, unsigned entry, juce::Colour c, float top) {
    const auto& panel = s.ui.plugin_panel;
    const float x = zone::pad, w = zone::width - 2 * zone::pad;
    bool ours = panel.entry == entry;
    p.text(plugin_name(s, entry), x, top, w - 110, 22, p.fit(plugin_name(s, entry), plain(colour::text, type::title, Weight::SemiBold), w - 110, type::floor));
    if (!ours || panel.status == PluginStatus::None || panel.status == PluginStatus::Loading) { // the plug, waiting
        icon(p, Icon::Plug, x + w / 2 - 14, top + 32, 28, colour::dim, 1.4f);
        p.text("OPENING", x, top + 66, w, 14, caps_text(colour::dim), juce::Justification::centred);
        return;
    }
    juce::String what = juce::String(panel.format) + juce::String::fromUTF8(" \xc2\xb7 ") + (panel.instrument ? "INSTRUMENT" : "EFFECT");
    p.text(what, x, top + 22, w, 14, caps_text(colour::mid));
    if (panel.status == PluginStatus::Failed || panel.status == PluginStatus::Crashed) {
        // The broken plug, the cause in two words, and (dim, for whoever fixes it) what the loader named.
        bool crashed = panel.status == PluginStatus::Crashed;
        auto error = juce::String::fromUTF8(panel.error);
        bool library = error.contains("shared object") || error.contains(".so");
        juce::String cause = crashed ? "STOPPED" : library ? "MISSING LIBRARY" : "WON'T OPEN";
        icon(p, Icon::PlugBroken, x + 2, top + 44, 40, colour::rec, 1.3f);
        icon(p, Icon::Warn, x + 56, top + 48, 13, colour::rec, 1.6f);
        p.text(cause, x + 74, top + 46, w - 74, 16, caps_text(colour::rec, type::body));
        juce::String code = crashed ? juce::String() : library ? error.upToFirstOccurrenceOf(":", false, false).trim() : error;
        if (code.isNotEmpty()) p.text(code, x + 56, top + 66, w - 56, 14, p.fit(code, plain(colour::dim, type::small), w - 56, type::floor));
        return;
    }
    if (panel.rows > 1) {
        float dots = page_dots(p, x + w, top + 11, int(panel.rows), int(panel.row), c, true);
        juce::String label = "ROW " + juce::String(panel.row + 1) + " / " + juce::String(panel.rows);
        p.text(label, x, top, w - dots - 6, 22, caps_text(colour::mid), juce::Justification::centredRight);
    }
    if (!panel.rows) {
        p.text("NO PARAMETERS", x, top + 50, w, 18, caps_text(colour::dim), juce::Justification::centred);
        return;
    }
    // How much of each block's time it takes: the cards below hold its parameters.
    float gy = top + 78, share = panel.load / 1000.0f;
    p.text("AUDIO TIME", x, gy, 100, 14, caps_text(colour::mid));
    juce::String load = juce::String(panel.load / 10.0f, 1) + " %";
    p.text(load, x, gy, w, 14, caps_text(share > 0.7f ? colour::text : colour::mid), juce::Justification::centredRight);
    gauge(p, x, gy + 19, w, std::clamp(share, 0.0f, 1.0f), share > 0.7f ? colour::text : c, 3);
}

} // namespace melodee
