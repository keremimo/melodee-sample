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
    // Glyph first, facts last: what the row is as a glyph before its name (folder, file, kit, bank, plug-in), and
    // at the right edge its facts as written ("3.1 s", "VST3"), a glyph fact (a check for the one loaded), the
    // pad a sample is on as a chip ("A05"), and a chevron for what opens further.
    Icon icon = Icon::None, fact_icon = Icon::None;
    juce::String fact, pad;
    bool chevron = false;
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
// under it). Plain panels without outlines: the likely one a shade lighter
// with white type, the others grey. An empty tile (no glyph) is skipped.
struct KeyTile {
    Icon icon = Icon::None;
    juce::String word, big, sub;
    bool primary = false;
    juce::Colour colour = colour::text;
    bool off = false;    // it would do nothing here: dim
    bool danger = false; // a loss (it asks first): its glyph and word in REC
};
void key_tiles(Paint& p, const std::array<KeyTile, 3>& tiles, float y = zone::picture_y + 12, float h = 116);
inline KeyTile word_tile(Icon i, juce::String word, bool primary = false, juce::Colour c = colour::text) {
    KeyTile t; t.icon = i; t.word = std::move(word); t.primary = primary; t.colour = c; return t;
}
inline KeyTile value_tile(Icon i, juce::String big, juce::String sub, bool primary = false) {
    KeyTile t; t.icon = i; t.big = std::move(big); t.sub = std::move(sub); t.primary = primary; return t;
}
// A word with a small line under it ("SAVE" over the project's name).
inline KeyTile word_sub_tile(Icon i, juce::String word, juce::String sub, bool primary = false) {
    KeyTile t; t.icon = i; t.word = std::move(word); t.sub = std::move(sub); t.primary = primary; return t;
}
// A sheet behind Push as it opens: the one action most likely wanted as the
// tile under B3, B2 MORE (how many others) for the rest as a list, over a
// dimmed page; the keys say BACK, MORE and the verb.
void likely_sheet(Paint& p, const KeyTile& likely, unsigned others);

// The question before an erase or a loss, asked in words as a person would:
// a glyph and the question ("Clear pattern 2?"), one or two short plain lines
// saying what goes and whether undo brings it back, and at the foot what it
// is about as a chip and a name, with the undo keys (Shift + Minus) when it
// can be undone. Over a dimmed picture, outlined in REC.
struct ConfirmSheet {
    Icon icon = Icon::Trash;
    juce::String title;                  // "Clear pattern 2?"
    std::array<juce::String, 2> lines;   // "24 notes of track 3 go.", "Undo brings them back."
    juce::String chip, name;             // "P2" in `chip_colour`; "TRACK 3"
    juce::Colour chip_colour = colour::text;
    bool undo = false;                   // Shift + Minus brings it back
};
void confirm_sheet(Paint& p, const ConfirmSheet& s);

// A small five-point star, for favourites.
void star(Paint& p, float cx, float cy, float r, juce::Colour c);

} // namespace melodee::ui
