// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The bundle's layout (tools/web_bundle.py): "MLWB", then for each file its
// path, its type and its bytes, each as a 32-bit little-endian length and
// that many bytes. The assembler reads it from the build folder (make runs at
// the repository root).
#include "melodee/web/web_bundle.h"
#include "melodee/ui/rubik_data.h"
#include <cstdint>
#include <cstring>
#include <map>

asm(R"(
    .section .rodata
    .balign 16
    .global melodee_web_bundle_start
melodee_web_bundle_start:
    .incbin "build/gen/web_bundle.bin"
    .global melodee_web_bundle_end
melodee_web_bundle_end:
    .previous
)");

extern "C" const unsigned char melodee_web_bundle_start[], melodee_web_bundle_end[];

namespace melodee::web {
namespace {
const std::map<std::string, File>& files() {
    static const std::map<std::string, File> all = [] {
        std::map<std::string, File> out;
        const unsigned char* at = melodee_web_bundle_start;
        const unsigned char* end = melodee_web_bundle_end;
        auto field = [&](const char*& data, uint32_t& size) {
            if (end - at < 4) return false;
            size = uint32_t(at[0]) | uint32_t(at[1]) << 8 | uint32_t(at[2]) << 16 | uint32_t(at[3]) << 24;
            at += 4;
            if (uint32_t(end - at) < size) return false;
            data = reinterpret_cast<const char*>(at);
            at += size;
            return true;
        };
        if (end - at < 4 || std::memcmp(at, "MLWB", 4) != 0) return out;
        at += 4;
        for (;;) {
            const char *path, *type, *data;
            uint32_t path_size, type_size, data_size;
            if (!field(path, path_size) || !field(type, type_size) || !field(data, data_size)) break;
            out[std::string(path, path_size)] = File {data, data_size, std::string(type, type_size)};
        }
        out["/font/rubik.ttf"] = File {reinterpret_cast<const char*>(melodee_rubik_start),
                                       size_t(melodee_rubik_end - melodee_rubik_start), "font/ttf"};
        return out;
    }();
    return all;
}
} // namespace

File bundled(const std::string& path) {
    auto& all = files();
    auto found = all.find(path);
    if (found == all.end() && path.size() > 1 && path.back() == '/') found = all.find(path.substr(0, path.size() - 1));
    return found == all.end() ? File {} : found->second;
}

} // namespace melodee::web
