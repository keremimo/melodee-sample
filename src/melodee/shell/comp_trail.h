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

} // namespace melodee
