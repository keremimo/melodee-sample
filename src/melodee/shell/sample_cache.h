// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/multisample.h"
#include "melodee/engine/sample_data.h"
#include <cstdint>
#include <string>

// A multisampled instrument's frames laid out once on the card, as
// melodee-ios's mapped audio: 32-bit float at the project's rate, the left
// side, then the right starting on a chunk boundary, so chunks are whole pages
// to pin. A 64 KB header says what the file holds, so opening it again reads
// nothing else. Named by the source's path on the card, size and date and the
// cache version; a change misses it by itself. Service thread only.
namespace melodee {

struct LaidOut {
    uint32_t frames = 0, channels = 0;
    uint32_t source_rate = 0;            // the file's own, for its region's frames
    int64_t loop_start = -1, loop_end = -1; // its smpl chunk's, at the source's rate
};

constexpr uint64_t cache_most_bytes = 4000000000ull; // least lately played go first past this
constexpr int cache_unplayed_days = 30;               // not played for this long: removed
// Raise when what a source reads as changes (a decoder or loop fix, the layout):
// every older file then misses and ages out.
constexpr unsigned cache_version = 2;

class SampleCache {
public:
    void set_folder(const std::string& folder) { folder_ = folder; }
    const std::string& folder() const { return folder_; }
    // Where a source's frames go; empty when it cannot be read or there is no folder.
    std::string file_of(const std::string& source) const;
    // One sound inside a file holding many (an .sf2's, roadmap P3): named after the file and `part`.
    std::string file_of(const std::string& source, const std::string& part) const;
    // True when `file` is whole and says what it holds. Marks it played.
    bool ready(const std::string& file, LaidOut& out) const;
    // Written to a .part, then renamed. Empty on success.
    std::string lay_out(const std::string& file, const SampleData& sample, const LaidOut& info) const;
    // Mapped read only, frames left 0 for the caller to publish.
    static bool map(const std::string& file, const LaidOut& info, ZonePart& part);
    static uint64_t bytes_of(uint32_t frames, uint32_t channels);
    bool has_room(uint64_t bytes) const; // on the card, beyond a margin
    // Removes .part files, files not played for cache_unplayed_days, then the least lately played past `most`.
    // Before any lay_out of this run: a .part found is one a stop left behind.
    void tidy(uint64_t most = cache_most_bytes) const;
private:
    std::string folder_;
};

} // namespace melodee
