// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/plugin/plugin_support.h"
#include <algorithm>
#include <cstdint>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

// The pack, linked in by the build (src/melodee/plugin/bridge32/support_pack.S); absent, these are null.
extern "C" {
extern const unsigned char melodee_plugin_support[] __attribute__((weak));
extern const unsigned char melodee_plugin_support_end[] __attribute__((weak));
}

namespace melodee {
namespace fs = std::filesystem;

namespace {
std::mutex lock;
std::string unpacked;
bool tried = false;

uint32_t word(const unsigned char*& at) {
    uint32_t v = uint32_t(at[0]) | uint32_t(at[1]) << 8 | uint32_t(at[2]) << 16 | uint32_t(at[3]) << 24;
    at += 4;
    return v;
}

// "MPS1", a count, then each file: its name's length and name (relative), its mode, its size and bytes.
bool unpack(const unsigned char* at, const unsigned char* end, const fs::path& into) {
    if (end - at < 8 || std::memcmp(at, "MPS1", 4) != 0) return false;
    at += 4;
    uint32_t count = word(at);
    std::error_code error;
    for (uint32_t n = 0; n < count; ++n) {
        if (end - at < 4) return false;
        uint32_t name_length = word(at);
        if (uint32_t(end - at) < name_length + 8) return false;
        std::string name(reinterpret_cast<const char*>(at), name_length);
        at += name_length;
        uint32_t mode = word(at), size = word(at);
        if (uint32_t(end - at) < size || name.find("..") != std::string::npos) return false;
        fs::path file = into / name;
        fs::create_directories(file.parent_path(), error);
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(at), size);
        if (!out) return false;
        out.close();
        chmod(file.c_str(), mode & 0777);
        at += size;
    }
    return true;
}

fs::path support_root() {
    std::error_code error;
    if (fs::is_directory("/run", error) && access("/run", W_OK) == 0) return "/run/melodee-plugins/support";
    return fs::temp_directory_path(error) / "melodee-plugins/support";
}

void load_stubs(const fs::path& folder) {
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(folder / "stubs", error))
        dlopen(entry.path().c_str(), RTLD_NOW | RTLD_GLOBAL); // its soname now satisfies any plug-in that lists it
    // The real libraries the MPC lacks (libsndfile and its codecs), in the order their names give: each before what
    // links it. One the MPC has after all is left alone.
    std::vector<fs::path> real;
    for (const auto& entry : fs::directory_iterator(folder / "libs", error)) real.push_back(entry.path());
    std::sort(real.begin(), real.end());
    for (const auto& path : real) {
        auto name = path.filename().string();
        auto soname = name.substr(name.find('-') + 1);
        if (dlopen(soname.c_str(), RTLD_NOW | RTLD_NOLOAD) || dlopen(soname.c_str(), RTLD_LAZY | RTLD_LOCAL)) continue;
        dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    }
}
} // namespace

bool ensure_plugin_support() {
    std::lock_guard<std::mutex> held(lock);
    if (tried) return true;
    tried = true;
    if (const char* own = std::getenv("MELODEE_PLUGIN_SUPPORT")) { // a developer's unpacked copy
        unpacked = own;
        load_stubs(unpacked);
        return true;
    }
    if (!melodee_plugin_support || !melodee_plugin_support_end) return true; // a build without the pack
    fs::path into = support_root();
    std::error_code error;
    fs::remove_all(into, error);
    if (!unpack(melodee_plugin_support, melodee_plugin_support_end, into)) return false;
    unpacked = into.string();
    load_stubs(into);
    return true;
}

std::string plugin_support_folder() {
    std::lock_guard<std::mutex> held(lock);
    return unpacked;
}

} // namespace melodee
