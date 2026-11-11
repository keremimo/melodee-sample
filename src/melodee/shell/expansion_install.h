// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <memory>
#include <string>

// An MPC expansion (.xpn: a zip of programs, samples, previews and an
// Expansion.xml) installed onto the card, a file a pass so the service thread
// keeps answering: into Samples/Expansions/<its title>, where the browser
// then lists its programs as it lists any folder's. A file already there at
// its size is kept, so a second install only finishes what the first left.
namespace melodee {

// A zip entry's name as a path inside the expansion's folder: forward slashes,
// no empty, "." or ".." part, nothing absolute; empty when it would leave it.
inline std::string expansion_entry_path(const std::string& name) {
    std::string out, part;
    auto take = [&]() {
        if (part.empty() || part == ".") { part.clear(); return true; }
        if (part == ".." || part.find(':') != std::string::npos) return false;
        out += (out.empty() ? "" : "/") + part;
        part.clear();
        return true;
    };
    for (char c : name) {
        if (c == '/' || c == '\\') { if (!take()) return {}; }
        else if (c == '\0') return {};
        else part += c;
    }
    if (!take()) return {};
    return out;
}

class ExpansionInstall {
public:
    ExpansionInstall();
    ~ExpansionInstall();
    // Opens `xpn` and works out its folder under `into` (its Expansion.xml's title, else
    // the file's name). Empty on success.
    std::string open(const std::string& xpn, const std::string& into);
    // One file out. False once every file is out (or on a failure, `error` then set).
    bool step(std::string& error);
    const std::string& folder() const;
    const std::string& title() const;
    unsigned done() const;
    unsigned total() const;
private:
    struct Inside;
    std::unique_ptr<Inside> inside;
};

} // namespace melodee
