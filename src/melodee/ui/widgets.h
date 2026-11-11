// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/ui/glyphs.h"
#include "melodee/ui/paint.h"
#include <array>
#include <utility>

// The smaller shared pieces the pictures are made of.
namespace melodee::ui {

// Page or row dots: the current one is a short bar. Returns the width drawn,
// right-aligned when `right` is set (x is then the right edge).
float page_dots(Paint& p, float x, float cy, int count, int at, juce::Colour c, bool right = false);

// A row of a list (sounds, samples, projects, sheets): number, name and an
// optional tag on the right. The current row is raised with an accent bar.
struct ListRow {
    juce::String number, name, tag;
    bool current = false, starred = false, dim = false;
    juce::Colour accent = colour::text;
    // A file heard as it plays (the current row only): the row fills in the
    // accent from the left as far as `played` (0..1; < 0 none), faint as
    // `played_wake` fades, with a thin head at the fill's end as strong as
    // `played_head`. `tag_lead` then leads the tag in the accent, the tag in mid.
    float played = -1, played_wake = 1, played_head = 0;
    juce::String tag_lead;
};
void list_row(Paint& p, float x, float y, float w, const ListRow& row);
constexpr float list_row_h = 23, list_row_gap = 1;
constexpr int list_rows = 6; // rows in the picture zone

// A thin level bar, horizontal: track LINE, `fill` 0..1 in `c`.
void gauge(Paint& p, float x, float y, float w, float fill, juce::Colour c, float thickness = 2);

// A sheet: a raised card over a dimmed picture, with an optional power icon,
// a title and a line or two of body text (the Exit question and its kin).
struct Sheet {
    juce::String title, body;
    bool power_icon = false;
};
void sheet(Paint& p, const Sheet& s);

// A menu sheet: a card of actions over the dimmed picture, each row a name
// and its value; the current row is raised with an accent bar (the note,
// clip and file sheets).
struct MenuRow {
    juce::String name, value;
    bool on = false, danger = false;
    bool off = false; // not possible here: drawn dim
};
struct MenuSheet {
    juce::String title, detail;
    std::array<MenuRow, 8> rows {};
    unsigned count = 0, current = 0;
    juce::Colour accent = colour::text;
    juce::Colour detail_colour = colour::mid;
};
void menu_sheet(Paint& p, const MenuSheet& sheet);

// The same rows as a wide card over the whole picture, two columns of up to
// four, with a chip and a title (the clip sheet).
void wide_sheet(Paint& p, const MenuSheet& sheet, const juce::String& chip_text, juce::Colour chip_colour);

// Choices as tiles straight under the soft keys that pick them, each with a
// caret up to its key: a glyph, then a word (or a big value with a small word
// under it). The primary one is outlined. An empty tile (no glyph) is skipped.
struct KeyTile {
    Icon icon = Icon::None;
    juce::String word, big, sub;
    bool primary = false;
    juce::Colour colour = colour::text;
};
void key_tiles(Paint& p, const std::array<KeyTile, 3>& tiles, float y = zone::picture_y + 12, float h = 116);
inline KeyTile word_tile(Icon i, juce::String word, bool primary = false, juce::Colour c = colour::text) {
    KeyTile t; t.icon = i; t.word = std::move(word); t.primary = primary; t.colour = c; return t;
}
inline KeyTile value_tile(Icon i, juce::String big, juce::String sub, bool primary = false) {
    KeyTile t; t.icon = i; t.big = std::move(big); t.sub = std::move(sub); t.primary = primary; return t;
}

// The question before an erase or a loss, without sentences: a glyph (the
// trash, or what the question is about), the thing in question as a chip
// and a short name, counts as glyph and number, and the undo keys when it
// can be undone (Shift + Minus). Over a dimmed picture, outlined in REC.
struct ConfirmSheet {
    Icon icon = Icon::Trash;
    juce::String chip, name;            // "P2" in `chip_colour`; "TRACK 3"
    juce::Colour chip_colour = colour::text;
    std::array<std::pair<Icon, juce::String>, 3> counts {}; // e.g. {Note, "24"}, {Motion, "2"}
    bool undo = false;                  // Shift + Minus brings it back
    juce::String note;                  // two words at most under the name ("SAVED FIRST")
    juce::Colour note_colour = colour::mid;
};
void confirm_sheet(Paint& p, const ConfirmSheet& s);

// A small five-point star, for favourites.
void star(Paint& p, float cx, float cy, float r, juce::Colour c);

} // namespace melodee::ui
