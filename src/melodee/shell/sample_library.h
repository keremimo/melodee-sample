// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/sample_data.h"
#include "melodee/model/looper.h"
#include "melodee/model/sampler.h"
#include "surface/midi.h"
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct Hardware;

// The samples on the drives and the ones loaded, run on the service thread:
// it finds the card's Samples folder (or $MELODEE_SAMPLES on a host), lists
// the folder the browser is in, reads the files asked for, works out their
// waveform and hits, hands the audio to the engine host, and writes the
// files Melodee makes (Save Sample, Trim Sample, Extract, takes). The MIDI
// and screen threads read what it found through view() under its own short
// lock; the audio thread never touches it.
//
// Two drives, as the MPC's browser has (B2): the card, starting in its
// Samples folder, and the internal one, read only: the factory library's
// categories (/content/Samples) and the stock app's own samples
// (/data/UserContent/Samples) side by side. Melodee writes only to the card.
namespace melodee {

class EngineHost;

enum class Drive : uint8_t { Card, Internal, Count };
inline const char* drive_name(Drive d) { return d == Drive::Internal ? "INTERNAL" : "CARD"; }

struct SampleFile {
    std::string path, name; // name: the stem in capitals, or the folder's
    uint64_t bytes = 0;
    bool folder = false;
    bool bank = false;      // a kit bank file (".mldb", model/project_file.h), listed with the WAV files
};
struct LoadedSample {
    std::string name, path, error;
    uint32_t frames = 0;
    Overview overview {};
    std::array<Hit, max_hits> hits {};
    unsigned hit_count = 0;
    bool ready = false;
};

// A file Melodee makes from a slot's audio, on the service thread.
struct SampleJob {
    enum class Kind : uint8_t { Save, Derive };
    Kind kind = Kind::Save;
    uint8_t source = 0;   // the slot the audio comes from
    uint8_t target = 0;   // Derive: the slot the new file goes into
    uint32_t from = 0, to = 0; // frames; to 0: the end
    char name[40] {};     // the file's name, without .wav; a free one is found
    char folder[16] {};   // under the card's Samples ("" itself)
};

class SampleLibrary {
public:
    explicit SampleLibrary(EngineHost& host);
    ~SampleLibrary();

    // MIDI thread: ask for `path` in project slot `slot`; ask for a fresh list.
    bool load(unsigned slot, const std::string& path);
    void rescan() { scan_wanted.store(true); }
    // Service thread: a take or a render is writing to the card (SD card access waits for it).
    bool busy() const;
    // MIDI thread: the browser. A folder to list (empty: the drive's start),
    // up one folder, or the other drive.
    void browse(Drive drive, const std::string& folder);
    void browse_up();
    // MIDI thread: a file to hear (preview_slot), empty for none; ready()
    // counts the previews that arrived, so the MIDI thread knows when to play one.
    void preview(const std::string& path);
    unsigned previews() const { return preview_count.load(std::memory_order_acquire); }
    std::string previewed() const;
    // MIDI thread: write a file; done() hands back what came of it (a notice).
    bool write(const SampleJob& job) { return jobs.push(job); }
    struct Done { char text[32] {}; int16_t failed = -1; }; // failed: the Derive target slot that got nothing
    bool done(Done& out) { return finished.pop(out); }
    // MIDI thread: kit bank files. Save Bank writes `bytes` as Samples/Banks/<name>.mldb (a free
    // name; done() tells how it went); a bank file to read, whose bytes take_bank() hands back.
    void write_bank(const std::string& name, std::vector<uint8_t> bytes);
    void read_bank(const std::string& path);
    struct BankBytes { std::string path, error; std::vector<uint8_t> bytes; };
    bool take_bank(BankBytes& out);
    // Changes each time the list or a loaded sample changes.
    unsigned generation() const { return changes.load(std::memory_order_acquire); }

    // Service thread, every few milliseconds: files, loads, finished takes, recalls and renders.
    void service(Hardware& hardware);

    // MIDI thread (parity phase 6, shell/sample_capture.cpp). Audio Recall:
    // the ring's last 25 s (from the first sound) into slot `slot`, as
    // Samples/Recorded/RECALL n.wav. A render of the mix the engine host is
    // about to play: into slot `slot` (Resample, Samples/Recorded/RESAMPLE
    // n.wav), or straight into Melodee/Mixdowns/<name>.wav on the card (the
    // song). `frames`: how long it should run, to make room up front.
    bool recall(unsigned slot) { return recalls.push(uint8_t(slot)); }
    struct Render { bool to_file = false; uint8_t slot = 0; uint32_t frames = 0; char name[40] {}; };
    bool render(const Render& job) { return renders.push(job); }
    std::string card() const; // the card's top folder; empty without one

    // The Looper (docs/LOOPER.md, shell/looper_files.cpp). Commit to pad: loop `loop` into slot `slot`, as
    // Samples/Looper/L<n> Loop m.wav, once the audio thread lets it be read. A project's loops: written as
    // <dir>/Loop N.wav for the loops in `mask` (the others' files go), or read back into the Looper.
    struct LoopCommit { uint8_t slot = 0, loop = 0; };
    bool commit_loop(const LoopCommit& job) { return commits.push(job); }
    struct LoopFiles { bool save = true; std::string dir; uint8_t mask = 0; std::array<LoopTrack, loop_tracks> loops {}; };
    void loop_files(LoopFiles job);
    bool loops_busy() const; // loop files still being written (leaving waits)

    // Any thread but audio: a copy of what is known.
    struct View {
        std::string root;  // the card's Samples folder, where Melodee writes; empty: no card
        Drive drive = Drive::Card;
        std::string folder; // the folder listed; empty: the internal drive's top
        bool top = true;    // nothing above it to go back to
        std::vector<SampleFile> files; // folders first, then WAV files, each by name
        int came_from = -1; // after going up: the folder it came out of
        std::array<LoadedSample, sample_slots> slots;
    };
    void view(View& out) const;
    LoadedSample slot(unsigned n) const;
    // MIDI thread: the loudest sample of slot `n` between two frames (0..1+), for
    // Normalize; -1 when it is not loaded. Reads the audio under the lock.
    float peak(unsigned n, uint32_t from, uint32_t to) const;
    std::vector<SampleFile> files() const;
    std::string root() const;
    std::string folder() const;
    Drive drive() const;
    int came_from() const;
    bool top() const;

    // Where each drive starts; tests and hosts may point them elsewhere.
    static std::vector<std::string> internal_tops();

private:
    struct Request { uint8_t slot = 0; char path[160] {}; };
    void scan();
    void read(const Request& request);
    void make(const SampleJob& job);
    void hear(const std::string& path);
    void finish(const std::string& text, int failed = -1);
    std::string free_name(const std::string& folder, const std::string& name, const char* extension = ".wav") const;
    void bank_files(); // the bank writes and reads asked for

    EngineHost& host;
    MidiQueue<16, Request> requests; // MIDI thread produces, service thread consumes
    MidiQueue<8, SampleJob> jobs;    // the same
    MidiQueue<8, Done> finished;     // service thread produces, MIDI thread consumes
    std::atomic<bool> scan_wanted {true};
    std::atomic<unsigned> changes {0};
    std::atomic<unsigned> preview_count {0};
    std::atomic<unsigned> take_count {0};
    void save_take();
    // Recall and renders (shell/sample_capture.cpp).
    MidiQueue<4, uint8_t> recalls;
    MidiQueue<4, Render> renders;
    void save_recall(unsigned slot);
    void drain_render();
    // A sample made here: written as Samples/<folder>/<stem> n.wav when there is a
    // card, offered to `slot` and listed; `what` names it in a failure.
    void land(unsigned slot, std::unique_ptr<SampleData> data, const char* folder, const char* stem, const char* what);
    bool rendering = false;
    Render render_job;
    // The Looper's files: a commit waiting for its loop, file jobs in turn (shell/looper_files.cpp).
    MidiQueue<4, LoopCommit> commits;
    struct Waiting { bool on = false; LoopCommit job; unsigned passes = 0; };
    Waiting committing;
    mutable std::mutex loop_lock;
    std::vector<LoopFiles> loop_jobs;
    LoopFiles loop_job;
    bool loop_job_on = false;
    unsigned loop_at = 0, loop_passes = 0;
    std::atomic<bool> loop_busy {false};
    void loop_service(); // a step of the commit and the file jobs, never waiting
    std::unique_ptr<SampleData> render_data;
    WavStream render_file;
    std::string render_path;
    std::vector<float> render_chunk;
    std::vector<std::unique_ptr<SampleData>> owned; // service thread: every sample the audio thread may hold
    std::array<const SampleData*, sample_slots> data_of {}; // under `lock`: what each slot holds now, for peak()
    std::string root_seen, card_top;
    unsigned ticks = 0;
    mutable std::mutex lock;
    View known;
    // Under `lock`: what the MIDI thread asked of the browser.
    struct Wanted { bool pending = false, up = false; Drive drive = Drive::Card; std::string folder; } wanted;
    std::string preview_wanted, preview_heard; // under `lock`
    std::vector<std::pair<std::string, std::vector<uint8_t>>> bank_writes; // under `lock`: name, bytes
    std::vector<std::string> bank_reads;  // under `lock`: paths
    std::vector<BankBytes> banks_read;    // under `lock`: what came of them
    bool preview_pending = false;
};

} // namespace melodee
