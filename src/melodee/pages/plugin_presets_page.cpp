// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// PRESETS (docs/MELODEE.md, Plug-ins): a plug-in's own programs, opened with
// B2 on a PLUGIN track's Edit or with Presets on a plug-in effect's sheet.
// The named ones list with their numbers; the wheel and Minus / Plus move,
// Push loads the one under the cursor and stays, so the next can be heard
// straight after (the pads play the track). The one picked last says LOADED
// (LOADING until the plug-in has it). B1 goes back. A plug-in finds its
// presets where it keeps them, under Melodee/Plugin Data on the card.
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/plugin/plugin_rack.h"
#include "melodee/ui/widgets.h"

namespace melodee {
namespace {
using namespace ui;
constexpr unsigned visible = list_rows;

bool for_instrument(const Snapshot& s) { return s.ui.presets_back == PageId::Edit; }

void draw(Paint& p, const Snapshot& s) {
    auto c = for_instrument(s) ? colour_of(s) : colour::text;
    const auto& v = s.presets;
    unsigned entry = s.ui.presets_entry;
    Header h = header_of(s);
    h.left.push_back(chip("PRESETS", c));
    h.left.push_back(pill(plugin_name(s, entry) + juce::String::fromUTF8(" \xc2\xb7 ") + juce::String(int(v.list.size())), c));
    if (for_instrument(s)) h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, {{"BACK", "", ""}, -1, c, {KeyIcon::Back, KeyIcon::None, KeyIcon::None}}, h);
    if (v.list.empty()) {
        p.text("No presets", zone::pad, zone::picture_y, zone::width - 2 * zone::pad, 24, plain(colour::dim, type::small, Weight::Medium),
               juce::Justification::centred);
        return;
    }
    unsigned count = unsigned(v.list.size()), cursor = std::min<unsigned>(s.ui.presets_cursor, count - 1);
    unsigned first = cursor < 2 ? 0 : std::min(cursor - 2, count > visible ? count - visible : 0);
    for (unsigned row = 0; row < visible && first + row < count; ++row) {
        const auto& program = v.list[first + row];
        ListRow r;
        r.number = juce::String(program.index + 1);
        r.name = juce::String::fromUTF8(program.name.c_str());
        r.accent = c;
        r.current = first + row == cursor;
        if (program.index == v.current) r.tag_lead = v.loading ? "LOADING" : "LOADED";
        p.g.saveState();
        list_row(p, zone::pad, zone::picture_y + 0.5f + row * (list_row_h + list_row_gap), zone::width - 2 * zone::pad, r);
        p.g.restoreState();
    }
}

void wheel(Context& c, int delta) {
    unsigned count = c.plugins ? unsigned(c.plugins->programs(c.ui.presets_entry).list.size()) : 0;
    c.ui.presets_cursor = uint16_t(std::clamp<int>(c.ui.presets_cursor + delta, 0, std::max(int(count) - 1, 0)));
}

void push(Context& c) { c.choose_plugin_program(c.ui.presets_cursor); }

void soft(Context& c, int key) {
    if (key == 0) c.open(c.ui.presets_back);
}
} // namespace

const PageSpec plugin_presets_page {draw, play_pad, wheel, push, soft, wheel}; // Minus / Plus move the list

} // namespace melodee
