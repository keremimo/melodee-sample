// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once

// The card sheet's rows (ui::card_sheet): cards two to a row, three rows in view.
namespace melodee::ui {

constexpr unsigned card_sheet_rows = 3; // rows in view
// The first row in view: the top until the highlight passes the third row, then the highlight's row last.
constexpr unsigned card_sheet_top(unsigned current, unsigned count) {
    unsigned rows = (count + 1) / 2, row = current / 2;
    unsigned top = row >= card_sheet_rows ? row - (card_sheet_rows - 1) : 0;
    unsigned most = rows > card_sheet_rows ? rows - card_sheet_rows : 0;
    return top < most ? top : most;
}

} // namespace melodee::ui
