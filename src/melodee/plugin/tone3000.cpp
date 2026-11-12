// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/plugin/tone3000.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <system_error>
#include <utility>
#include <vector>

namespace melodee {
namespace fs = std::filesystem;

namespace {
// A block's size as JUCE writes it: the property's name, its value's length
// (a compressed int: one byte, 9), a double's marker (4), then the double.
constexpr uint8_t size_key[] = {'s', 'l', 'i', 'm', 'S', 'i', 'z', 'e', 0, 1, 9, 4};
constexpr size_t key_length = sizeof(size_key);
constexpr uint8_t lite[8] = {}; // 0.0: a model's smallest size

// Where each size that is not 0 already has its double.
std::vector<size_t> sizes_to_change(const uint8_t* data, size_t size) {
    std::vector<size_t> out;
    const uint8_t* end = data + size;
    for (const uint8_t* at = data;;) {
        at = std::search(at, end, size_key, size_key + key_length);
        if (size_t(end - at) < key_length + sizeof lite) break;
        const uint8_t* value = at + key_length;
        if (std::memcmp(value, lite, sizeof lite) != 0) out.push_back(size_t(value - data));
        at = value;
    }
    return out;
}
} // namespace

unsigned tone3000_lite(uint8_t* data, size_t size) {
    auto sizes = sizes_to_change(data, size);
    for (size_t at : sizes) std::memcpy(data + at, lite, sizeof lite);
    return unsigned(sizes.size());
}

unsigned tone3000_lite_presets(const fs::path& home) {
    // The files read already, with their length and time then.
    static std::map<std::string, std::pair<uintmax_t, fs::file_time_type>> seen;
    std::error_code e;
    fs::path folder = home / ".config" / "TONE3000" / "Presets";
    if (home.empty() || !fs::is_directory(folder, e)) return 0;
    unsigned changed = 0;
    for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, e), end; !e && it != end;
         it.increment(e)) {
        const fs::path& path = it->path();
        std::error_code f;
        if (path.extension() != ".t3kpreset" || !it->is_regular_file(f)) continue;
        uintmax_t length = fs::file_size(path, f);
        auto time = fs::last_write_time(path, f);
        if (f) continue;
        auto known = seen.find(path.string());
        if (known != seen.end() && known->second == std::make_pair(length, time)) continue;
        std::vector<uint8_t> data(length);
        {
            std::ifstream in(path, std::ios::binary);
            if (!in.read(reinterpret_cast<char*>(data.data()), std::streamsize(length))) continue;
        }
        auto sizes = sizes_to_change(data.data(), data.size());
        if (!sizes.empty()) {
            // Only the eight bytes of each size: the file keeps its length, and one cut short still loads.
            std::fstream out(path, std::ios::in | std::ios::out | std::ios::binary);
            for (size_t at : sizes) out.seekp(std::streamoff(at)).write(reinterpret_cast<const char*>(lite), sizeof lite);
            out.close();
            if (!out) continue; // a locked card: it plays full, and is tried again the next time
            ++changed;
            length = fs::file_size(path, f);
            time = fs::last_write_time(path, f);
            if (f) continue;
        }
        seen[path.string()] = {length, time};
    }
    return changed;
}

} // namespace melodee
