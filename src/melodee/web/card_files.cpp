// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/web/card_files.h"
#include "melodee/web/web_server.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <vector>

namespace melodee::web {
namespace fs = std::filesystem;
namespace {
std::string lowered(std::string text) {
    for (auto& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string top_part(const std::string& rel) { return lowered(rel.substr(0, rel.find('/'))); }

// Whether `inside` is `root` or under it, both made whole first.
bool under(const fs::path& root, const fs::path& inside) {
    auto r = root.lexically_normal(), i = inside.lexically_normal();
    auto a = r.begin(), b = i.begin();
    for (; a != r.end(); ++a, ++b) {
        if (a->empty()) continue; // a trailing slash
        if (b == i.end() || *a != *b) return false;
    }
    return true;
}
} // namespace

bool card_place(const std::string& card, const std::string& rel, fs::path& out, std::string& error) {
    error = "Not on the card";
    if (card.empty()) { error = "No SD card"; return false; }
    fs::path place = card;
    size_t at = 0;
    while (at < rel.size()) {
        size_t slash = rel.find('/', at);
        if (slash == std::string::npos) slash = rel.size();
        std::string part = rel.substr(at, slash - at);
        if (part.empty() || part == "." || part == ".." || part[0] == '.' || part.find('\\') != std::string::npos) return false;
        place /= part;
        at = slash + 1;
    }
    std::error_code e;
    auto real_card = fs::weakly_canonical(card, e);
    if (e) return false;
    auto real = fs::weakly_canonical(place, e);
    if (e || !under(real_card, real)) return false;
    out = place;
    error.clear();
    return true;
}

bool card_writable(const std::string& rel) {
    if (rel.empty()) return true; // the top: files go there (a release zip, a folder of samples), it is never deleted
    auto top = top_part(rel);
    return top != "autorun" && top != "sample-base";
}

std::string folder_json(const fs::path& folder) {
    struct Entry { std::string name; bool dir; uint64_t size; int64_t time; };
    std::vector<Entry> entries;
    std::error_code e;
    for (const auto& item : fs::directory_iterator(folder, e)) {
        auto name = item.path().filename().string();
        if (name.empty() || name[0] == '.' || name == "System Volume Information") continue;
        std::error_code f;
        bool dir = item.is_directory(f);
        uint64_t size = dir ? 0 : uint64_t(item.file_size(f));
        auto when = item.last_write_time(f);
        int64_t time = f ? 0 : int64_t(std::chrono::duration_cast<std::chrono::seconds>(
            when - fs::file_time_type::clock::now() + std::chrono::system_clock::now().time_since_epoch()).count());
        entries.push_back({name, dir, size, time});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.dir != b.dir) return a.dir;
        return lowered(a.name) < lowered(b.name);
    });
    std::string out = "[";
    for (size_t n = 0; n < entries.size(); ++n) {
        const auto& x = entries[n];
        out += (n ? "," : "") + std::string("{\"name\":") + json_text(x.name) + ",\"dir\":" + (x.dir ? "true" : "false")
            + ",\"size\":" + std::to_string(x.size) + ",\"time\":" + std::to_string(x.time) + "}";
    }
    return out + "]";
}

fs::path free_path(const fs::path& folder, const std::string& name) {
    std::error_code error;
    fs::path path = folder / name;
    for (int n = 2; fs::exists(path, error) && n < 1000; ++n)
        path = folder / (fs::path(name).stem().string() + " " + std::to_string(n) + fs::path(name).extension().string());
    return path;
}

std::string file_type(const std::string& name) {
    auto ext = lowered(fs::path(name).extension().string());
    if (ext == ".wav") return "audio/wav";
    if (ext == ".mp3") return "audio/mpeg";
    if (ext == ".flac") return "audio/flac";
    if (ext == ".ogg" || ext == ".oga") return "audio/ogg";
    if (ext == ".aif" || ext == ".aiff") return "audio/aiff";
    if (ext == ".json") return "application/json";
    if (ext == ".txt" || ext == ".sfz" || ext == ".md") return "text/plain; charset=utf-8";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".zip") return "application/zip";
    return "application/octet-stream";
}

} // namespace melodee::web
