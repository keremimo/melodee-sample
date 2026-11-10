// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// SOUNDS (Sample Select): the selected track's engine sounds. The wheel
// scrolls, pads audition the sound under the cursor, Push loads it, Shift +
// Push goes back. B1-B3 filter: all, favourites, recent. K1 engine, K2 jumps
// through the list, K3 audition velocity. Shift + Sample Select stars the
// sound under the cursor (elsewhere, the track's loaded sound).
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/shell/sound_list.h"
#include "melodee/ui/widgets.h"
#include <cstdio>

namespace melodee {
namespace {
using namespace ui;
constexpr unsigned visible = list_rows;

void draw(Paint& p, const Snapshot& s) {
    const auto& track = s.track();
    const auto& info = engine_info(track.engine);
    auto c = colour_of(s);
    Header h = header_of(s);
    h.left.push_back(pill(juce::String(info.name) + juce::String::fromUTF8(" \xc2\xb7 ") + juce::String(info.sound_count), c));
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, {{"ALL", "FAVOURITES", "RECENT"}, s.ui.sounds_filter, c}, h);

    std::array<uint16_t, max_listed> list {};
    unsigned count = sound_list(track, s.ui, list);
    if (!count) {
        p.text(s.ui.sounds_filter == 1 ? "No favourites yet" : "Nothing loaded yet", zone::pad, zone::picture_y,
               zone::width - 2 * zone::pad, 40, plain(colour::dim, type::small, Weight::Medium), juce::Justification::centred);
        return;
    }
    unsigned cursor = std::min<unsigned>(s.ui.sounds_cursor, count - 1);
    unsigned first = cursor < 2 ? 0 : std::min(cursor - 2, count > visible ? count - visible : 0);
    for (unsigned row = 0; row < visible && first + row < count; ++row) {
        unsigned sound = list[first + row];
        char number[8];
        std::snprintf(number, sizeof(number), "%02u", sound + 1);
        ListRow r;
        r.number = number; r.name = info.sounds[sound].name; r.accent = c;
        r.current = first + row == cursor;
        r.tag = sound == track.preset ? "LOADED" : "";
        r.starred = s.ui.settings.favourite(unsigned(track.engine), sound);
        p.g.saveState();
        list_row(p, zone::pad, zone::picture_y + 0.5f + row * (list_row_h + list_row_gap), zone::width - 2 * zone::pad, r);
        p.g.restoreState();
    }
}

unsigned listed(Context& c, std::array<uint16_t, max_listed>& list) { return sound_list(c.track(), c.ui, list); }

void wheel(Context& c, int delta) {
    std::array<uint16_t, max_listed> list {};
    unsigned count = listed(c, list);
    if (!count) return;
    c.ui.sounds_cursor = uint16_t(std::clamp<int>(int(c.ui.sounds_cursor) + delta, 0, int(count) - 1));
}

void push(Context& c) {
    std::array<uint16_t, max_listed> list {};
    unsigned count = listed(c, list);
    if (!count || !c.track().used()) return;
    c.load_sound(c.ui.track, c.track().engine, list[std::min<unsigned>(c.ui.sounds_cursor, count - 1)]);
}

void soft(Context& c, int key) {
    c.audition(-1);
    c.ui.sounds_filter = uint8_t(key);
    c.ui.sounds_cursor = 0;
}

void pad(Context& c, unsigned pad, uint8_t velocity, bool down) {
    if (down) {
        std::array<uint16_t, max_listed> list {};
        unsigned count = listed(c, list);
        if (count) c.audition(list[std::min<unsigned>(c.ui.sounds_cursor, count - 1)]);
    }
    c.play_pad(pad, velocity, down);
}
} // namespace

const PageSpec sounds_page {draw, pad, wheel, push, soft, wheel}; // Minus / Plus move the list

} // namespace melodee
