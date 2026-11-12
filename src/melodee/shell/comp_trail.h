// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

// The Compressor page's picture over time (screen thread): what the detector
// heard (the mix, or the key with Sidechain) and what it took off, one
// column every comp_column_ms however often the screen draws, oldest first.
namespace melodee {

struct CompColumn {
    float level = -90;  // dB the detector heard at most (In Boost in)
    float reduce = 0;   // dB it took off at most
};
constexpr unsigned comp_columns = 80;     // 2 px each across the picture
constexpr uint32_t comp_column_ms = 33;   // 80 columns: about 2.6 s

class CompTrail {
public:
    // What the compressor gave since the last call; columns fall due by the clock.
    void update(float level_db, float reduce_db, uint32_t now) {
        pending.level = std::max(pending.level, level_db);
        pending.reduce = std::max(pending.reduce, reduce_db);
        if (!started || int32_t(now - next) > 1000) { next = now; started = true; } // the first time, or after a long gap
        bool pushed = false;
        for (unsigned n = 0; n < comp_columns && int32_t(now - next) >= 0; ++n) { // a slow frame: the same column again
            std::rotate(columns.begin(), columns.begin() + 1, columns.end());
            columns.back() = pending;
            next += comp_column_ms;
            pushed = true;
        }
        if (pushed) pending = {};
    }
    const std::array<CompColumn, comp_columns>& trail() const { return columns; }
private:
    std::array<CompColumn, comp_columns> columns {};
    CompColumn pending {};
    uint32_t next = 0;
    bool started = false;
};

// Round 2 (compressor B, "one bar of pump"): what it took off laid across one
// bar of the song by where the transport is in it, the bar before kept as a
// ghost, and where the detector's level jumped (the kick's hits, with
// Sidechain the key's). Screen thread; stopped, the bars stay as they were.
constexpr unsigned comp_bar_columns = 154; // one bar across the picture, 2 px each
struct CompBarColumn {
    float reduce = 0; // dB taken off at most
    bool hit = false; // the level jumped here
};
class CompBar {
public:
    void update(float level_db, float reduce_db, uint32_t tick, uint32_t bar_ticks, bool playing) {
        if (!playing || !bar_ticks) { at_column = -1; previous = -90; return; }
        uint32_t bar = tick / bar_ticks;
        int column = int(uint64_t(tick % bar_ticks) * comp_bar_columns / bar_ticks);
        if (bar != this_bar || at_column < 0 || column < at_column) { // a new bar (or a jump): what was drawn is the ghost
            if (at_column >= 0) ghost_columns = columns;
            columns = {};
            this_bar = bar;
            at_column = column - 1;
        }
        bool hit = level_db > -30 && level_db - previous > 6; // a jump up from well below
        previous = level_db;
        for (int c = std::max(0, at_column + 1); c <= column; ++c) columns[size_t(c)].reduce = reduce_db; // columns a slow frame skipped
        auto& now = columns[size_t(column)];
        now.reduce = std::max(now.reduce, reduce_db);
        now.hit = now.hit || hit;
        at_column = column;
    }
    const std::array<CompBarColumn, comp_bar_columns>& bar() const { return columns; }
    const std::array<CompBarColumn, comp_bar_columns>& ghost() const { return ghost_columns; }
    int at() const { return at_column; } // the column the transport is at; -1 stopped
private:
    std::array<CompBarColumn, comp_bar_columns> columns {}, ghost_columns {};
    uint32_t this_bar = 0;
    int at_column = -1;
    float previous = -90;
};

} // namespace melodee
