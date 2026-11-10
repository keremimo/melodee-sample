// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/ui/paint.h"
#include <array>

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

// A small five-point star, for favourites.
void star(Paint& p, float cx, float cy, float r, juce::Colour c);

} // namespace melodee::ui
