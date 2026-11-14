// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/sample_cache.h"
#include "melodee/engine/dsp.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

namespace melodee {
namespace fs = std::filesystem;
namespace {
constexpr size_t header_bytes = stream_chunk_bytes;
constexpr char magic[8] = {'M', 'L', 'D', 'F', '3', '2', '0', '1'};

struct Header {
    char tag[8];
    uint32_t frames, channels, source_rate, reserved;
    int64_t loop_start, loop_end;
};

uint64_t fnv(const std::string& text) {
    uint64_t hash = 0xcbf29ce484222325ull;
    for (unsigned char c : text) hash = (hash ^ c) * 0x00000100000001b3ull;
    return hash;
}

// A side's room in the file: whole chunks, but for the last side's own end.
uint64_t padded(uint32_t frames) { return (uint64_t(frames) * sizeof(float) + stream_chunk_bytes - 1) / stream_chunk_bytes * stream_chunk_bytes; }
} // namespace

uint64_t SampleCache::bytes_of(uint32_t frames, uint32_t channels) {
    return header_bytes + (channels > 1 ? padded(frames) : 0) + uint64_t(frames) * sizeof(float);
}

std::string SampleCache::file_of(const std::string& source) const { return file_of(source, {}); }

std::string SampleCache::file_of(const std::string& source, const std::string& part) const {
    if (folder_.empty()) return {};
    struct stat st {};
    if (stat(source.c_str(), &st)) return {};
    // Its path from the card's top (the folder's is <top>/Melodee/Cache), so two packs' C3.wav of one size and
    // date stay apart, and the same card under another mount still finds it; off the card, the whole path.
    fs::path whole = fs::path(source).lexically_normal();
    fs::path where = whole.lexically_relative(fs::path(folder_).parent_path().parent_path());
    if (where.empty() || *where.begin() == "..") where = whole;
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.f32", (unsigned long long)fnv(where.generic_string() + "|" + std::to_string(st.st_size) + "|"
                  + std::to_string(st.st_mtime) + "|" + std::to_string(unsigned(dsp::model_rate)) + "|v" + std::to_string(cache_version)
                  + (part.empty() ? "" : "|" + part)));
    return folder_ + "/" + name;
}

bool SampleCache::ready(const std::string& file, LaidOut& out) const {
    std::FILE* f = std::fopen(file.c_str(), "rb");
    if (!f) return false;
    Header h {};
    bool ok = std::fread(&h, sizeof(h), 1, f) == 1 && !std::memcmp(h.tag, magic, sizeof(magic)) && h.frames && h.channels >= 1 && h.channels <= 2;
    std::fclose(f);
    if (!ok) return false;
    struct stat st {};
    if (stat(file.c_str(), &st) || uint64_t(st.st_size) != bytes_of(h.frames, h.channels)) return false;
    out.frames = h.frames; out.channels = h.channels; out.source_rate = h.source_rate;
    out.loop_start = h.loop_start; out.loop_end = h.loop_end;
    utimes(file.c_str(), nullptr); // played lately, as tidy() goes by
    return true;
}

std::string SampleCache::lay_out(const std::string& file, const SampleData& sample, const LaidOut& info) const {
    if (file.empty() || !sample.frames) return "nothing to lay out";
    std::error_code error;
    fs::create_directories(fs::path(file).parent_path(), error);
    std::string writing = file + ".part";
    std::FILE* f = std::fopen(writing.c_str(), "wb");
    if (!f) return "cannot write the cache";
    std::vector<char> room(header_bytes, 0);
    Header h {};
    std::memcpy(h.tag, magic, sizeof(magic));
    h.frames = sample.frames; h.channels = info.channels; h.source_rate = info.source_rate;
    h.loop_start = info.loop_start; h.loop_end = info.loop_end;
    std::memcpy(room.data(), &h, sizeof(h));
    bool ok = std::fwrite(room.data(), 1, room.size(), f) == room.size();
    ok = ok && std::fwrite(sample.left.data(), sizeof(float), sample.frames, f) == sample.frames;
    if (ok && info.channels > 1) {
        size_t pad = size_t(padded(sample.frames) - uint64_t(sample.frames) * sizeof(float));
        std::fill(room.begin(), room.end(), 0);
        ok = (!pad || std::fwrite(room.data(), 1, pad, f) == pad) && std::fwrite(sample.right.data(), sizeof(float), sample.frames, f) == sample.frames;
    }
    ok = std::fflush(f) == 0 && ok && fsync(fileno(f)) == 0;
    ok = std::fclose(f) == 0 && ok;
    if (!ok) { fs::remove(writing, error); return "cannot write the cache"; }
    fs::rename(writing, file, error);
    if (error) { fs::remove(writing, error); return "cannot write the cache"; }
    return {};
}

bool SampleCache::map(const std::string& file, const LaidOut& info, ZonePart& part) {
    int fd = open(file.c_str(), O_RDONLY);
    if (fd < 0) return false;
    size_t bytes = size_t(bytes_of(info.frames, info.channels));
    // read only: asked for ahead on a mapping it could write to, a system copies every page
    void* start = mmap(nullptr, bytes, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (start == MAP_FAILED) return false;
    part.mapping = start; part.mapping_bytes = bytes;
    part.left = reinterpret_cast<const float*>(static_cast<const char*>(start) + header_bytes);
    part.right = info.channels > 1 ? part.left + padded(info.frames) / sizeof(float) : part.left;
    part.pins.assign((info.frames - 1) / stream_chunk + 1, 0);
    return true;
}

bool SampleCache::has_room(uint64_t bytes) const {
    struct statvfs st {};
    if (statvfs(fs::path(folder_).parent_path().c_str(), &st)) return false;
    return uint64_t(st.f_bavail) * st.f_frsize > bytes + (uint64_t(64) << 20);
}

void SampleCache::tidy(uint64_t most) const {
    struct Kept { fs::path path; uint64_t bytes; fs::file_time_type used; };
    std::vector<Kept> kept;
    std::error_code error;
    uint64_t total = 0;
    const auto unplayed = fs::file_time_type::clock::now() - std::chrono::hours(24 * cache_unplayed_days);
    std::vector<fs::path> gone;
    for (auto it = fs::directory_iterator(folder_, error); !error && it != fs::directory_iterator(); it.increment(error)) {
        const auto kind = it->path().extension();
        if (kind == ".part") { gone.push_back(it->path()); continue; }
        if (kind != ".f32") continue;
        std::error_code e;
        uint64_t bytes = it->file_size(e);
        auto used = it->last_write_time(e);
        if (e) continue;
        if (used < unplayed) { gone.push_back(it->path()); continue; }
        kept.push_back({it->path(), bytes, used});
        total += bytes;
    }
    for (const auto& path : gone) fs::remove(path, error);
    std::sort(kept.begin(), kept.end(), [](const Kept& a, const Kept& b) { return a.used < b.used; });
    for (const auto& k : kept) {
        if (total <= most) break;
        fs::remove(k.path, error); // one still mapped plays on: its frames go when the mapping does
        total -= k.bytes;
    }
}

} // namespace melodee
