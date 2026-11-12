// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/sf2_file.h"
#include <fcntl.h>
#include <mutex>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace melodee {

Sf2Mapped::~Sf2Mapped() {
    if (map) munmap(map, size);
}

const Sf2Preset* Sf2Mapped::preset(unsigned index) const {
    for (const auto& p : sf2.presets()) if (p.index == index) return &p;
    return nullptr;
}

namespace {
std::mutex lock;
struct Kept { std::string path; int64_t stamp = 0; std::shared_ptr<Sf2Mapped> file; };
std::vector<Kept> kept; // the last few, newest last
} // namespace

void forget_sf2_files() {
    std::lock_guard<std::mutex> guard(lock);
    kept.clear();
}

std::shared_ptr<Sf2Mapped> open_sf2(const std::string& path) {
    struct stat st {};
    auto made = std::make_shared<Sf2Mapped>();
    if (stat(path.c_str(), &st) || !S_ISREG(st.st_mode)) { made->error = "Files missing"; return made; }
    const int64_t stamp = int64_t(st.st_mtime) ^ (int64_t(st.st_size) << 20);
    std::lock_guard<std::mutex> guard(lock);
    for (auto& k : kept) if (k.path == path && k.stamp == stamp) return k.file;
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) { made->error = "cannot read it"; return made; }
    made->size = size_t(st.st_size);
    void* at = made->size ? mmap(nullptr, made->size, PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
    ::close(fd);
    if (at == MAP_FAILED) { made->size = 0; made->error = "cannot read it"; return made; }
    made->map = at;
    made->error = made->sf2.open(static_cast<const uint8_t*>(at), made->size);
    kept.push_back({path, stamp, made});
    if (kept.size() > 3) kept.erase(kept.begin());
    return made;
}

} // namespace melodee
