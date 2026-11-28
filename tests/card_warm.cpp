// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The card's folders read ahead (src/melodee/shell/card_warm.h): a small exFAT volume made here, walked from its
// image file. Every folder is found once, by a chain in the FAT or by its length; only a folder's entries up to
// its end mark are read; a folder that names one above it does not go round; what is not exFAT is left alone.
#include "melodee/shell/card_warm.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace melodee;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", what); ++failures; }
}

constexpr unsigned sector = 512, cluster = 4096; // 8 sectors a cluster
constexpr unsigned fat_at = 8 * sector, fat_bytes = 8 * sector, heap_at = 32 * sector, clusters = 64;

struct Image {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(heap_at + clusters * cluster, 0);
    void u32(size_t at, uint32_t v) { for (unsigned n = 0; n < 4; ++n) bytes[at + n] = uint8_t(v >> (8 * n)); }
    void u64(size_t at, uint64_t v) { u32(at, uint32_t(v)); u32(at + 4, uint32_t(v >> 32)); }
    size_t at(uint32_t c) const { return heap_at + size_t(c - 2) * cluster; }
    void fat(uint32_t c, uint32_t next) { u32(fat_at + size_t(c) * 4, next); }
    void boot(uint32_t root) {
        std::memcpy(&bytes[3], "EXFAT   ", 8);
        u32(0x50, fat_at / sector); u32(0x54, fat_bytes / sector); u32(0x58, heap_at / sector); u32(0x5c, clusters); u32(0x60, root);
        bytes[0x6c] = 9; bytes[0x6d] = 3;
    }
    // An entry set at `where` (bytes from the volume's start): a file entry, its stream entry, its name. Returns what follows.
    size_t entry(size_t where, const std::string& name, bool folder, uint32_t first, uint64_t length, bool contiguous, bool deleted = false) {
        const unsigned names = unsigned((name.size() + 14) / 15);
        bytes[where] = deleted ? 0x05 : 0x85; bytes[where + 1] = uint8_t(1 + names); bytes[where + 4] = folder ? 0x10 : 0x20;
        size_t stream = where + 32;
        bytes[stream] = deleted ? 0x40 : 0xc0; bytes[stream + 1] = uint8_t(1 | (contiguous ? 2 : 0)); bytes[stream + 3] = uint8_t(name.size());
        u32(stream + 20, first); u64(stream + 24, length); u64(stream + 8, length);
        for (unsigned n = 0; n < names; ++n) {
            size_t e = where + 64 + size_t(n) * 32;
            bytes[e] = deleted ? 0x41 : 0xc1;
            for (unsigned c = 0; c < 15 && n * 15 + c < name.size(); ++c) bytes[e + 2 + c * 2] = uint8_t(name[n * 15 + c]);
        }
        return where + 64 + size_t(names) * 32;
    }
    std::string write(const std::string& path) const {
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        return path;
    }
};
} // namespace

int main(int argc, char** argv) {
    const std::string folder = argc > 1 ? argv[1] : ".";
    std::atomic<bool> stop {false};

    // The root in clusters 4 and 9 (a chain in the FAT). In it: Melodee (5, by its length), a file, a deleted folder,
    // Samples (6 then 12, a chain), Other (7). Melodee holds Cache (8); Samples holds Drums (10), named in its second
    // cluster; Other holds Deep (11), which names Other again.
    Image image;
    image.boot(4);
    image.fat(4, 9); image.fat(9, 0xffffffff);
    image.fat(6, 12); image.fat(12, 0xffffffff);
    size_t at = image.at(4);
    at = image.entry(at, "Melodee", true, 5, cluster, true);
    at = image.entry(at, "a long file name that takes three entries.wav", false, 20, 123456, true);
    at = image.entry(at, "Gone", true, 30, cluster, true, true);
    while (at + 96 <= image.at(4) + cluster) at = image.entry(at, "f.wav", false, 21, 10, true); // the cluster filled: no end mark in it
    at = image.entry(image.at(9), "Samples", true, 6, 2 * cluster, false);
    at = image.entry(at, "Other", true, 7, cluster, true);
    image.entry(image.at(5), "Cache", true, 8, cluster, true);
    at = image.entry(image.at(6), "a name of thirty one characters", false, 22, 10, true); // five entries, so the rest fill it exactly
    while (at + 96 <= image.at(6) + cluster) at = image.entry(at, "g.wav", false, 22, 10, true);
    check(at == image.at(6) + cluster && image.bytes[image.at(4) + cluster - 32] != 0, "the test's full clusters hold no end mark");
    image.entry(image.at(12), "Drums", true, 10, cluster, true);
    image.entry(image.at(7), "Deep", true, 11, cluster, true);
    image.entry(image.at(11), "Round", true, 7, cluster, true); // back to Other
    const std::string path = image.write(folder + "/card_warm_test.img");

    CardWarmth w = warm_exfat(path, stop);
    check(w.exfat, "an exFAT volume is read");
    check(w.folders == 7, "the root and its six folders, each once");
    check(w.whole, "all of it was reached");
    // The boot sector, the FAT whole, an entry of it for each of the two chains (a chain is not followed past an end
    // mark), and a cluster each: the root's two, Samples' two and the five others. No folder twice.
    check(w.bytes == 512 + fat_bytes + 8 + 9 * cluster, "a folder's clusters are read once");

    // The folders Melodee uses come first: with room for three, the root, Melodee and its Cache, before Samples.
    CardWarmth few = warm_exfat(path, stop, 3);
    check(few.folders == 3 && !few.whole, "it stops at the most folders allowed");
    check(few.bytes == 512 + fat_bytes + 4 + 4 * cluster, "Melodee's folders before the others");

    stop.store(true);
    CardWarmth stopped = warm_exfat(path, stop);
    check(stopped.exfat && stopped.folders == 0 && !stopped.whole, "a stop asked for before it begins reads no folder");
    stop.store(false);

    // Not exFAT: left alone.
    Image other = image;
    std::memcpy(&other.bytes[3], "MSDOS5.0", 8);
    other.write(path);
    check(!warm_exfat(path, stop).exfat, "another file system is not read");
    // A root outside the volume, a cluster of no size: refused.
    Image broken = image;
    broken.u32(0x60, 9999);
    broken.write(path);
    check(!warm_exfat(path, stop).exfat, "a root outside the volume is refused");
    check(!warm_exfat(folder + "/no such file", stop).exfat, "a device that cannot be opened");
    check(exfat_device("/no/such/mount").empty(), "no device for a mount that is not there");

    // On its own thread: nothing for a mount that is not exFAT, and stop() is safe either way.
    CardWarm warm;
    warm.start("/no/such/mount");
    check(!warm.running(), "nothing begins without an exFAT card");
    warm.stop();

    std::remove(path.c_str());
    if (failures) return 1;
    std::printf("card warm ok\n");
    return 0;
}
