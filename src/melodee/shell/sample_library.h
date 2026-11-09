// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/sample_data.h"
#include "melodee/model/sampler.h"
#include "surface/midi.h"
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class Platform;

// The samples on the card and the ones loaded, run on the service thread:
// it finds the Samples folder (the SD card's, or $MELODEE_SAMPLES on a
// host), lists its WAV files, reads the ones asked for, works out their
// waveform and hits, and hands the audio to the engine host. The MIDI and
// screen threads read what it found through view() under its own short
// lock; the audio thread never touches it.
namespace melodee {

class EngineHost;

struct SampleFile {
    std::string path, name;
    uint64_t bytes = 0;
};
struct LoadedSample {
    std::string name, error;
    uint32_t frames = 0;
    Overview overview {};
    std::array<Hit, max_hits> hits {};
    unsigned hit_count = 0;
    bool ready = false;
};

class SampleLibrary {
public:
    explicit SampleLibrary(EngineHost& host);
    ~SampleLibrary();

    // MIDI thread: ask for `path` in project slot `slot`; ask for a fresh list.
    bool load(unsigned slot, const std::string& path);
    void rescan() { scan_wanted.store(true); }
    // Changes each time the list or a loaded sample changes.
    unsigned generation() const { return changes.load(std::memory_order_acquire); }

    // Service thread, every few milliseconds.
    void service(const Platform& platform);

    // Any thread but audio: a copy of what is known.
    struct View {
        std::string root;  // empty: no Samples folder found
        std::vector<SampleFile> files;
        std::array<LoadedSample, sample_slots> slots;
    };
    void view(View& out) const;
    LoadedSample slot(unsigned n) const;
    std::vector<SampleFile> files() const;
    std::string root() const;

private:
    struct Request { uint8_t slot = 0; char path[160] {}; };
    void scan(const std::string& root);
    void read(const Request& request);

    EngineHost& host;
    MidiQueue<16, Request> requests; // MIDI thread produces, service thread consumes
    std::atomic<bool> scan_wanted {true};
    std::atomic<unsigned> changes {0};
    std::vector<std::unique_ptr<SampleData>> owned; // service thread: every sample the audio thread may hold
    std::string root_seen;
    unsigned ticks = 0;
    mutable std::mutex lock;
    View known;
};

} // namespace melodee
