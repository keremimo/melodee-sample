// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// PRESETS (docs/MELODEE.md, Plug-ins): a plug-in's own programs, opened with
// B2 on a PLUGIN track's Edit or with Presets on a plug-in effect's sheet.
// The named ones list with their numbers; the wheel and Minus / Plus move,
// Push loads the one under the cursor and stays, so the next can be heard
// straight after (the pads play the track). The one picked last says LOADED
// (LOADING until the plug-in has it). B1 goes back. A plug-in finds its
// presets where it keeps them, under Melodee/Plugin Data on the card.
// Presets in folders (a CLAP plug-in's files: Surge XT's Basses, Leads ...)
// list as those folders, each with how many it holds; Push opens one, B1
// comes back out, and the folder holding the one picked last has its check.
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/plugin/plugin_rack.h"
#include "melodee/plugin/preset_rows.h"
#include "melodee/ui/widgets.h"

namespace melodee {
namespace {
using namespace ui;
constexpr unsigned visible = list_rows;

bool for_instrument(const Snapshot& s) { return s.ui.presets_back == PageId::Edit; }
std::string folder_in_view(const UiState& ui) { return std::string(ui.presets_folder.data()); }

void draw(Paint& p, const Snapshot& s) {
    auto c = for_instrument(s) ? colour_of(s) : colour::text;
    const auto& v = s.presets;
    unsigned entry = s.ui.presets_entry;
    const auto inside = folder_in_view(s.ui);
    const auto rows = preset_rows(v.list, inside, v.current);
    const auto dot = juce::String::fromUTF8(" \xc2\xb7 ");
    Header h = header_of(s);
    h.left.push_back(chip("PRESETS", c));
    if (inside.empty()) h.left.push_back(pill(plugin_name(s, entry) + dot + juce::String(int(v.list.size())), c));
    else { // the folder in view and the presets in it
        unsigned held = 0;
        for (const auto& r : rows) held += r.folder ? r.count : 1;
        h.left.push_back(pill(Icon::Folder, juce::String::fromUTF8(preset_folder_name(inside).c_str()) + dot + juce::String(held), c));
    }
    if (for_instrument(s)) h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, {{"BACK", "", ""}, -1, c, {KeyIcon::Back, KeyIcon::None, KeyIcon::None}}, h);
    if (rows.empty()) {
        icon(p, v.finding ? Icon::Clock : Icon::Layers, zone::width / 2.0f - 14, zone::picture_y + 40, 28, colour::dim, 1.4f);
        p.text(v.finding ? "LOOKING" : "NONE", 0, zone::picture_y + 76, zone::width, 14, caps_text(colour::dim), juce::Justification::centred);
        return;
    }
    const bool folders = !inside.empty() || rows.front().folder; // numbered in the folder, else by the plug-in's numbers
    unsigned folder_rows = 0;
    for (const auto& r : rows) folder_rows += r.folder ? 1 : 0;
    unsigned count = unsigned(rows.size()), cursor = std::min<unsigned>(s.ui.presets_cursor, count - 1);
    unsigned first = cursor < 2 ? 0 : std::min(cursor - 2, count > visible ? count - visible : 0);
    for (unsigned row = 0; row < visible && first + row < count; ++row) {
        const auto& x = rows[first + row];
        ListRow r;
        r.name = juce::String::fromUTF8(x.name.c_str());
        r.accent = c;
        r.current = first + row == cursor;
        if (x.folder) {
            r.icon = Icon::Folder;
            r.fact = juce::String(x.count);
            r.chevron = true;
            if (x.current) r.fact_icon = Icon::Check;
        } else {
            r.number = juce::String(folders ? int(first + row - folder_rows) + 1 : v.list[x.at].index + 1);
            if (x.current) r.fact_icon = v.loading ? Icon::Clock : Icon::Check;
        }
        p.g.saveState();
        list_row(p, zone::pad, zone::picture_y + 0.5f + row * (list_row_h + list_row_gap), zone::width - 2 * zone::pad, r);
        p.g.restoreState();
    }
}

void wheel(Context& c, int delta) {
    size_t count = 0;
    if (c.plugins) {
        auto v = c.plugins->programs(c.ui.presets_entry);
        count = preset_rows(v.list, folder_in_view(c.ui), v.current).size();
    }
    c.ui.presets_cursor = uint16_t(std::clamp<int>(c.ui.presets_cursor + delta, 0, std::max(int(count) - 1, 0)));
}

void push(Context& c) { c.push_plugin_preset(); }

void soft(Context& c, int key) {
    if (key == 0) c.back_plugin_presets();
}
} // namespace

const PageSpec plugin_presets_page {draw, play_pad, wheel, push, soft, wheel}; // Minus / Plus move the list

} // namespace melodee
