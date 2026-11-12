// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/smf_jobs.h"
#include "melodee/model/sampler.h"
#include "melodee/shell/sample_library.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace melodee {
namespace fs = std::filesystem;

void export_midi(Context& c, const std::string& name) {
    std::string top = c.library ? c.library->card() : std::string();
    if (top.empty()) { c.notice("MIDI file: no card"); return; }
    auto tracks = smf_tracks(c.project, c.patterns);
    if (tracks.empty()) { c.notice("MIDI file: no notes"); return; }
    auto ts = c.project.time_signature();
    auto bytes = write_smf(tracks, c.project.tempo, ts.beats, ts.unit, smf_tempos(c.project)); // roadmap P5: the tempo lane too
    std::error_code error;
    std::string folder = top + "/Melodee/MIDI";
    fs::create_directories(folder, error);
    std::string clean;
    for (char ch : name) clean += std::string("/\\:\"*?<>|").find(ch) != std::string::npos ? '-' : ch;
    if (clean.empty()) clean = "Song";
    std::string path;
    for (unsigned n = 1; n < 1000 && path.empty(); ++n) {
        auto candidate = folder + "/" + clean + (n > 1 ? " " + std::to_string(n) : std::string()) + ".mid";
        if (!fs::exists(candidate, error)) path = candidate;
    }
    std::ofstream file(path, std::ios::binary);
    if (path.empty() || !file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()))) { c.notice("MIDI file: cannot write"); return; }
    c.notice("MIDI file " + fs::path(path).stem().string());
}

void import_midi(Context& c, const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() > (8u << 20)) { c.notice("MIDI file: too big"); return; }
    std::vector<SmfTrack> tracks;
    std::string why;
    if (!read_smf(bytes, tracks, why)) { c.notice("MIDI file: " + why); return; }
    auto& track = c.track();
    if (!track.used()) { c.notice("MIDI file: pick a track with a sound"); return; }
    Pattern laid;
    if (!smf_pattern(tracks, track, c.project.bar_ticks(), laid)) { c.notice("MIDI file: no notes for this track"); return; }
    unsigned t = c.ui.track, p = track.pattern % pattern_count;
    std::string what = "MIDI file onto P" + std::to_string(p + 1);
    c.edit_pattern(t, p, what.c_str(), [&] {
        auto& pattern = c.patterns.at[t][p];
        pattern.events = laid.events;
        pattern.length = laid.length;
        pattern.fit_lanes();
        c.sync_pattern(t, p);
    });
    c.dirty = true;
    c.notice(what);
}

} // namespace melodee
