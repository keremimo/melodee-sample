// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Stems (src/melodee/shell/stems.h) with stand-ins for the separating program: shell scripts that take its
// arguments, print its lines and write its four files. A job copies the program into its work folder, gives it the
// model and the sample as a WAV file, writes what it made to the paths asked for at the sample's length and leaves
// nothing else behind; a job cancelled, a program that fails, one that makes too little and one that is not there each say so
// and leave no file. A BEST job hands the program "best" after the file; the others hand it nothing more.
#include "melodee/shell/stems.h"
#include "melodee/engine/sample_data.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using namespace melodee;
namespace fs = std::filesystem;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", what); ++failures; }
}

std::string script(const fs::path& folder, const char* name, const char* body) {
    const auto path = folder / name;
    std::ofstream(path) << "#!/bin/sh\n" << body;
    fs::permissions(path, fs::perms::owner_all);
    return path.string();
}

constexpr uint32_t frames = 88200; // 2 s

StemsJob job(const fs::path& folder, const std::string& engine, const char* name, uint32_t length = frames) {
    StemsJob j;
    j.engine = engine;
    j.model = (folder / "model.f16").string();
    j.work = (folder / "work").string();
    j.left.resize(length);
    j.right.resize(length);
    for (uint32_t n = 0; n < length; ++n) {
        j.left[n] = 0.5f * std::sin(float(n) * 0.05f);
        j.right[n] = 0.25f * std::sin(float(n) * 0.031f);
    }
    for (unsigned n = 0; n < stem_count; ++n) j.paths[n] = (folder / "Stems" / (std::string(name) + " " + stem_name(n) + ".wav")).string();
    return j;
}

// The job's result, waited for at most `seconds`.
bool finished(StemSeparator& separator, StemsResult& out, unsigned seconds) {
    for (unsigned n = 0; n < seconds * 100; ++n) {
        if (separator.take(out)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

bool none_left(const StemsResult& r) {
    for (const auto& path : r.paths) if (fs::exists(path)) return false;
    return true;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const fs::path folder = argv[1];
    // The program's arguments are the model, its threads and the file, then "best" for BEST; it prints how far it is.
    std::ofstream(folder / "model.f16") << "MLST";
    const char* lines = "[ -f \"$1\" ] && [ \"$2\" = 3 ] || exit 9\n"
                        "[ $# = 3 ] || [ $# = 4 -a \"$4\" = best ] || exit 7\n"
                        "echo 'progress 5'\n"
                        "echo 'progress 50'\n";
    const auto good = script(folder, "good.sh", (std::string(lines) + "for stem in Drums Bass Music Vocals; do cp \"$3\" \"$3_$stem.wav\" || exit 8; done\n").c_str());
    const auto slow = script(folder, "slow.sh", (std::string(lines) + "sleep 30\n").c_str());
    const auto failing = script(folder, "failing.sh", "echo 'melodee-stems: cannot read the file' >&2\nexit 1\n");
    const auto half = script(folder, "half.sh", (std::string(lines) + "cp \"$3\" \"$3_Drums.wav\"\n").c_str());
    const auto best = script(folder, "best.sh", (std::string(lines) + "[ \"$4\" = best ] || exit 6\n"
                                                 "for stem in Drums Bass Music Vocals; do cp \"$3\" \"$3_$stem.wav\" || exit 8; done\n").c_str());

    check(stems_memory(60 * 44100, false) > uint64_t(656 + 100) << 20 && stems_memory(13 * 44100, false) > uint64_t(478 + 22) << 20, "a job counts the memory measured and its files in RAM");
    check(stems_memory(60 * 44100, true) > uint64_t(841 + 100) << 20 && stems_memory(13 * 44100, true) > uint64_t(655 + 22) << 20, "BEST counts its own");
    check(stems_seconds(60 * 44100, false) > stems_seconds(13 * 44100, false), "a longer sample takes longer");
    check(stems_seconds(13 * 44100, true) > 2 * stems_seconds(13 * 44100, false) && stems_seconds(60 * 44100, true) > 2 * stems_seconds(60 * 44100, false), "BEST takes over twice as long");

    StemSeparator separator;
    StemsResult result;
    {
        check(separator.start(job(folder, good, "LOOP")), "a job starts");
        check(!separator.start(job(folder, good, "OTHER")), "one job at a time");
        check(finished(separator, result, 20), "the job ends");
        check(result.error.empty(), "the job has no error");
        check(!separator.busy() && separator.progress().fraction > 0.999f, "it is done, all of it");
        for (unsigned n = 0; n < stem_count; ++n) {
            SampleData stem;
            check(read_wav(result.paths[n], stem).empty(), "a stem's file reads");
            check(stem.frames == frames, "a stem is as long as the sample, to the frame");
            bool same = stem.frames == frames;
            for (uint32_t f = 0; same && f < frames; f += 97)
                same = std::fabs(stem.left[f] - 0.5f * std::sin(float(f) * 0.05f)) < 1e-4f && std::fabs(stem.right[f] - 0.25f * std::sin(float(f) * 0.031f)) < 1e-4f;
            check(same, "a stem holds what the program made, each side its own");
        }
        check(fs::path(result.paths[0]).filename() == "LOOP DRUMS.wav" && fs::path(result.paths[1]).filename() == "LOOP BASS.wav"
              && fs::path(result.paths[2]).filename() == "LOOP MUSIC.wav" && fs::path(result.paths[3]).filename() == "LOOP VOCALS.wav", "the files are the ones asked for");
        check(!fs::exists(folder / "work"), "the work folder is gone");
    }
    {
        auto asked = job(folder, best, "BEST");
        asked.best = true;
        check(separator.start(std::move(asked)), "a BEST job starts");
        check(finished(separator, result, 20) && result.error.empty(), "the program is told best, and the job ends");
        check(fs::path(result.paths[3]).filename() == "BEST VOCALS.wav" && fs::exists(result.paths[3]), "its files are written");
        check(separator.start(job(folder, best, "NOTBEST")), "a job that is not BEST starts");
        check(finished(separator, result, 20) && result.error == "program failed", "the program is not told best");
        check(none_left(result), "and it leaves nothing");
    }
    {
        check(separator.start(job(folder, slow, "CUT")), "a second job starts once the first is taken");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        check(separator.busy() && separator.progress().busy, "it runs");
        separator.cancel();
        check(finished(separator, result, 5), "a cancelled job ends at once");
        check(result.error == "stopped", "a cancelled job says stopped");
        check(none_left(result) && !fs::exists(folder / "work"), "a cancelled job leaves nothing");
    }
    {
        check(separator.start(job(folder, failing, "BAD")), "a job with a failing program starts");
        check(finished(separator, result, 20) && result.error == "program failed", "a failing program is named");
        check(none_left(result) && !fs::exists(folder / "work"), "a failed job leaves nothing");
    }
    {
        check(separator.start(job(folder, half, "HALF")), "a job with a program that makes one file starts");
        check(finished(separator, result, 20) && result.error == "no BASS made", "the missing stem is named");
        check(none_left(result), "the stem that was written goes too");
    }
    {
        check(separator.start(job(folder, (folder / "none").string(), "NONE")), "a job without the program starts");
        check(finished(separator, result, 20) && result.error == "no program on the card", "the missing program is named");
        check(separator.start(job(folder, good, "TINY", stems_least_frames - 1)), "a job with too short a sample starts");
        check(finished(separator, result, 20) && result.error == "too short", "too short a sample is refused");
        fs::remove(folder / "model.f16");
        check(separator.start(job(folder, good, "BARE")), "a job without the model starts");
        check(finished(separator, result, 20) && result.error == "no model on the card", "the missing model is named");
    }
    if (failures) return 1;
    std::printf("stems ok\n");
    return 0;
}
