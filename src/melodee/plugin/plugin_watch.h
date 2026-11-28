// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

// A plug-in in use opened again by itself when its file on the card changes (a new build copied over it through card
// access, SSH or the web page), with the state it has, as picking it again does (PluginRack::reload). The rack's
// thread looks every watch_every_ms at the file each entry was opened from: its size and time, its "stamp". A
// changed one is opened again once it has held still for watch_settle_ms, so a file still being copied never opens.
// Only the plug-in's own file is looked at (a bundle's: its library), never the folders beside it, which a plug-in may
// write itself. A CLAP plug-in may ask for the same with request_restart() (restart_allowed() below). Pure, so the
// host tests follow it.
namespace melodee {

constexpr uint32_t watch_every_ms = 2000;
constexpr uint32_t watch_settle_ms = 1000;

struct FileStamp {
    int64_t size = -1;                   // -1: no file there
    int64_t time = 0;                    // its last write, in the file clock's ticks
    bool there() const { return size >= 0; }
    bool operator==(const FileStamp& o) const { return size == o.size && time == o.time; }
    bool operator!=(const FileStamp& o) const { return !(*this == o); }
};

inline FileStamp stamp_of(const std::string& path) {
    std::error_code e;
    FileStamp s;
    auto size = std::filesystem::file_size(path, e);
    if (e) return {};
    auto time = std::filesystem::last_write_time(path, e);
    if (e) return {};
    s.size = int64_t(size);
    s.time = int64_t(time.time_since_epoch().count());
    return s;
}

// One entry's watch: the stamp of what it was opened from (a file not there then: size -1, so one copied there later
// counts as changed), and a changed stamp seen since, from when.
struct FileWatch {
    FileStamp opened;
    FileStamp seen;
    uint32_t seen_at = 0;
    bool watching = false;               // set when it is opened from a file; nothing to look at before
};

// A look at the file now (`now`, at `now_ms` on a clock that only goes up): true when the entry should be opened
// again, its watch then on the new stamp. A file gone stays as it was (it fails to open; nothing gained).
inline bool watch_look(FileWatch& w, const FileStamp& now, uint32_t now_ms, uint32_t settle_ms = watch_settle_ms) {
    if (!w.watching || !now.there() || now == w.opened) {
        w.seen = {};
        return false;
    }
    if (now != w.seen) { // changed, or still changing: from now
        w.seen = now;
        w.seen_at = now_ms;
        return false;
    }
    if (now_ms - w.seen_at < settle_ms) return false;
    w.opened = now;
    w.seen = {};
    return true;
}

// A CLAP plug-in's request_restart(): honoured as the file's change is (opened again with its state), once it has
// played restart_quiet_ms since it opened; one asked sooner is let go, so a plug-in that asks at each start never
// opens over and over. `ready_at`, `now_ms`: on the same clock as watch_look's.
constexpr uint32_t restart_quiet_ms = 3000;
inline bool restart_allowed(uint32_t ready_at, uint32_t now_ms) { return now_ms - ready_at >= restart_quiet_ms; }

} // namespace melodee
