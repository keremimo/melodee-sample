// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/expansion_install.h"
#include <juce_core/juce_core.h>
#include <filesystem>
#include <fstream>
#include <vector>

namespace melodee {
namespace fs = std::filesystem;

struct ExpansionInstall::Inside {
    std::unique_ptr<juce::ZipFile> zip;
    std::string folder, title;
    int next = 0;
    unsigned done = 0;
};

ExpansionInstall::ExpansionInstall() : inside(std::make_unique<Inside>()) {}
ExpansionInstall::~ExpansionInstall() = default;
const std::string& ExpansionInstall::folder() const { return inside->folder; }
const std::string& ExpansionInstall::title() const { return inside->title; }
unsigned ExpansionInstall::done() const { return inside->done; }
unsigned ExpansionInstall::total() const { return inside->zip ? unsigned(inside->zip->getNumEntries()) : 0; }

namespace {
// A title as a folder's name: no slashes or colons, at most 60 characters, trimmed.
std::string folder_name(std::string title) {
    for (auto& c : title)
        if (c == '/' || c == '\\' || c == ':' || static_cast<unsigned char>(c) < 32) c = '-';
    if (title.size() > 60) title.resize(60);
    while (!title.empty() && (title.back() == ' ' || title.back() == '.')) title.pop_back();
    while (!title.empty() && (title.front() == ' ' || title.front() == '.')) title.erase(title.begin());
    return title;
}
} // namespace

std::string ExpansionInstall::open(const std::string& xpn, const std::string& into) {
    auto& in = *inside;
    in = Inside {};
    in.zip = std::make_unique<juce::ZipFile>(juce::File(xpn));
    if (!in.zip->getNumEntries()) return "Not an expansion";
    // Its title, from the Expansion.xml at its top.
    if (const auto* entry = in.zip->getEntry("Expansion.xml", true)) {
        std::unique_ptr<juce::InputStream> stream(in.zip->createStreamForEntry(*entry));
        if (stream && entry->uncompressedSize < (1 << 20))
            if (auto xml = juce::XmlDocument::parse(stream->readEntireStreamAsString()))
                in.title = folder_name(xml->getChildElementAllSubText("title", {}).trim().toStdString());
    }
    if (in.title.empty()) in.title = folder_name(fs::path(xpn).stem().string());
    if (in.title.empty()) in.title = "Expansion";
    in.folder = (fs::path(into) / in.title).string();
    std::error_code error;
    fs::create_directories(in.folder, error);
    if (error) return "Card full";
    return {};
}

bool ExpansionInstall::step(std::string& error) {
    auto& in = *inside;
    if (!in.zip) return false;
    while (in.next < in.zip->getNumEntries()) {
        const int index = in.next++;
        const auto* entry = in.zip->getEntry(index);
        if (!entry) continue;
        const auto relative = expansion_entry_path(entry->filename.toStdString());
        if (relative.empty()) continue; // a name that would leave the folder: left out
        const fs::path target = fs::path(in.folder) / relative;
        std::error_code fs_error;
        if (entry->filename.endsWithChar('/') || entry->filename.endsWithChar('\\')) { fs::create_directories(target, fs_error); continue; }
        ++in.done;
        if (fs::exists(target, fs_error) && fs::file_size(target, fs_error) == uint64_t(entry->uncompressedSize)) return true; // there already
        fs::create_directories(target.parent_path(), fs_error);
        std::unique_ptr<juce::InputStream> stream(in.zip->createStreamForEntry(index));
        if (!stream) { error = "Unreadable"; return false; }
        const auto part = target.string() + ".part";
        {
            std::ofstream out(part, std::ios::binary | std::ios::trunc);
            std::vector<char> buffer(64 * 1024);
            while (!stream->isExhausted()) {
                const int got = stream->read(buffer.data(), int(buffer.size()));
                if (got <= 0) break;
                out.write(buffer.data(), got);
            }
            if (!out) { fs::remove(part, fs_error); error = "Card full"; return false; }
        }
        fs::rename(part, target, fs_error);
        if (fs_error) { error = "Card full"; return false; }
        return true;
    }
    return false;
}

} // namespace melodee
