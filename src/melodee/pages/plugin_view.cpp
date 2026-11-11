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
    if (!ours || panel.status == PluginStatus::None || panel.status == PluginStatus::Loading) {
        p.text("Opening", x, top + 40, w, 20, plain(colour::dim, type::small, Weight::Medium), juce::Justification::centred);
        return;
    }
    juce::String what = juce::String(panel.format) + juce::String::fromUTF8(" \xc2\xb7 ") + (panel.instrument ? "INSTRUMENT" : "EFFECT");
    p.text(what, x, top + 22, w, 14, caps_text(colour::mid));
    if (panel.status == PluginStatus::Failed || panel.status == PluginStatus::Crashed) {
        juce::String head = panel.status == PluginStatus::Crashed ? "Stopped working" : "Could not open";
        p.text(head, x, top + 44, w, 18, plain(colour::text, type::body, Weight::SemiBold), juce::Justification::centred);
        juce::String why = panel.status == PluginStatus::Crashed ? juce::String("B1 picks it again from the card") : juce::String::fromUTF8(panel.error);
        p.text(why, x, top + 64, w, 16, p.fit(why, plain(colour::dim, type::small, Weight::Medium), w, type::floor), juce::Justification::centred);
        return;
    }
    if (panel.rows > 1) {
        float dots = page_dots(p, x + w, top + 11, int(panel.rows), int(panel.row), c, true);
        juce::String label = "ROW " + juce::String(panel.row + 1) + " / " + juce::String(panel.rows);
        p.text(label, x, top, w - dots - 6, 22, caps_text(colour::mid), juce::Justification::centredRight);
    }
    if (!panel.rows) {
        p.text("No parameters to show", x, top + 50, w, 18, plain(colour::dim, type::small, Weight::Medium), juce::Justification::centred);
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
