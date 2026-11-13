// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// PLUG-INS (docs/MELODEE.md, Plug-ins): the card's Plugins folder, opened for
// a PLUGIN track's instrument (Sample Select on it, or PLUGIN chosen on K1 of
// Sounds) or for a chain's slot (the effect picker's Plug-ins family). Folders
// first, then the plug-ins, each with its format and whether it is an
// instrument or an effect (found by opening it once, so it fills in a moment
// after the folder lists). A bundle holding several plug-ins (LSP's) lists as
// one row with how many, and Push opens it: a row for each. One that cannot
// run here says why: a Windows or Mac download, a PC build. The wheel and Minus / Plus move, Push opens a folder
// or loads the plug-in (on the track, or into the slot) and goes back; B1
// goes back out of a folder, or at the top back where it came from. The pads
// play the track.
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/plugin/plugin_rack.h"
#include "melodee/ui/widgets.h"
#include <filesystem>

namespace melodee {
namespace {
using namespace ui;
constexpr unsigned visible = list_rows;

// What the picker loads into, for the header: the track, or the chain and its slot.
juce::String target_text(const Snapshot& s) {
    if (s.ui.plugin_target == UiState::PluginTarget::Instrument) return "INSTRUMENT";
    unsigned n = s.ui.plugin_chain;
    juce::String where = n < track_count ? "TRACK" : n < master_chain ? juce::String("BUS ") + char('A' + (n - track_count) % bus_count)
                       : n == master_chain ? juce::String("MASTER") : juce::String("LOOP");
    return where + " FX " + juce::String(s.ui.plugin_slot + 1);
}

// The entry the target holds now, so its plug-in can say LOADED.
const PluginRef* loaded_ref(const Snapshot& s) {
    if (s.ui.plugin_target == UiState::PluginTarget::Instrument) {
        const auto& t = s.track();
        return t.engine == EngineKind::Plugin && t.plugin < max_plugins ? &s.project.plugins[t.plugin] : nullptr;
    }
    const auto& chain = s.project.chain(s.ui.plugin_chain);
    if (s.ui.plugin_slot >= chain.size) return nullptr;
    const auto& slot = chain.slots[s.ui.plugin_slot];
    if (slot.kind != plugin_effect_kind || unsigned(slot.params[0]) >= max_plugins) return nullptr;
    return &s.project.plugins[size_t(slot.params[0])];
}

void draw(Paint& p, const Snapshot& s) {
    auto c = s.ui.plugin_target == UiState::PluginTarget::Instrument ? colour_of(s) : colour::text;
    const auto& v = s.plugins;
    Header h = header_of(s);
    juce::String folder = v.root.empty() ? juce::String("NO CARD")
                        : v.top ? juce::String("PLUGINS") : juce::String(std::filesystem::path(v.folder).stem().string());
    h.left.push_back(chip(target_text(s), c));
    h.left.push_back(pill(folder + juce::String::fromUTF8(" \xc2\xb7 ") + juce::String(int(v.files.size())), c));
    if (s.ui.plugin_target == UiState::PluginTarget::Instrument) h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, {{v.top ? "CANCEL" : "BACK", "", ""}, -1, c, {KeyIcon::Back, KeyIcon::None, KeyIcon::None}}, h);
    if (v.files.empty()) { // a glyph and two words: no card, or an empty Plugins folder
        float mid = zone::width / 2.0f, y = zone::picture_y + 40;
        icon(p, v.root.empty() ? Icon::Card : Icon::Folder, mid - 14, y, 28, colour::dim, 1.4f);
        p.text(v.root.empty() ? "NO CARD" : "NO PLUG-INS", 0, y + 36, zone::width, 14, caps_text(colour::dim), juce::Justification::centred);
        if (!v.root.empty() && v.top) // where they go: unzipped plug-in folders in the card's Plugins folder
            p.text(juce::String::fromUTF8("Card \xe2\x80\xba Plugins"), 0, y + 54, zone::width, 14, plain(colour::dim, type::small), juce::Justification::centred);
        return;
    }
    const auto* ref = loaded_ref(s);
    std::string loaded = ref ? ref->path : "";
    int32_t loaded_id = ref ? ref->id : 0;
    unsigned count = unsigned(v.files.size()), cursor = std::min<unsigned>(s.ui.plugins_cursor, count - 1);
    unsigned first = cursor < 2 ? 0 : std::min(cursor - 2, count > visible ? count - visible : 0);
    for (unsigned row = 0; row < visible && first + row < count; ++row) {
        const auto& file = v.files[first + row];
        ListRow r;
        r.name = juce::String(file.name);
        r.accent = c;
        r.current = first + row == cursor;
        if (file.folder) { r.icon = Icon::Folder; r.chevron = true; }
        else if (file.several()) { r.icon = Icon::Plug; r.fact = juce::String(int(file.plugins)); r.chevron = true; } // a bundle of several
        else if (!file.loadable()) { r.icon = Icon::PlugBroken; r.fact = juce::String(file.why); r.dim = true; }
        else {
            r.icon = Icon::Plug;
            r.fact = format_tag(file.format);
            if (!loaded.empty() && file.path == loaded && file.id == loaded_id) r.fact_icon = Icon::Check; // the one loaded
            else if (file.instrument >= 0) r.fact_icon = file.instrument ? Icon::Keyboard : Icon::Sliders;
        }
        p.g.saveState();
        list_row(p, zone::pad, zone::picture_y + 0.5f + row * (list_row_h + list_row_gap), zone::width - 2 * zone::pad, r);
        p.g.restoreState();
    }
}

void wheel(Context& c, int delta) {
    c.ui.plugins_cursor = uint16_t(std::clamp<int>(c.ui.plugins_cursor + delta, 0, std::max(int(c.ui.plugins_count) - 1, 0)));
}

void push(Context& c) {
    if (!c.plugins) return;
    auto view = c.plugins->view();
    if (c.ui.plugins_cursor >= view.files.size()) return;
    c.choose_plugin(view.files[c.ui.plugins_cursor]);
}

void soft(Context& c, int key) {
    if (key != 0 || !c.plugins) return;
    if (c.plugins->view().top) { // CANCEL: an instrument not picked yet leaves the track as it was, on the page it came from
        bool unpicked = c.ui.plugin_target == UiState::PluginTarget::Instrument && c.track().engine != EngineKind::Plugin;
        c.open(unpicked ? c.ui.plugin_from : c.ui.plugin_back);
        return;
    }
    c.plugin_came_back = false; // the cursor lands on the folder it came out of
    c.plugins->browse_up();
}
} // namespace

const PageSpec plugins_page {draw, play_pad, wheel, push, soft, wheel}; // Minus / Plus move the list

} // namespace melodee
