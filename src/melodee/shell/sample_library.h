// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/bounce.h"
#include "melodee/shell/expansion_install.h"
#include "melodee/engine/onsets.h"
#include "melodee/engine/recall_ring.h"
#include "melodee/engine/sample_data.h"
#include "melodee/engine/sfz.h"
#include "melodee/model/looper.h"
#include "melodee/model/sampler.h"
#include "melodee/shell/sample_cache.h"
#include "melodee/shell/zone_shapes.h"
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
    bool instrument = false; // a multisampled instrument (".sfz", ".xpm", ".exs"), loaded onto a pad as a WAV file is
    bool kit = false;        // an MPC drum program (".xpm", ".xty"): Push loads it onto the track as Load Kit does
    bool expansion = false;  // an MPC expansion (".xpn"): Push installs it onto the card
    bool presets = false;    // a SoundFont (".sf2", roadmap P3): listed as a folder, its presets inside it
    bool midi = false;       // a Standard MIDI File (".mid", roadmap P4): Push lays its notes on the track's pattern
};
struct LoadedSample {
    std::string name, path, error;
    uint32_t frames = 0;
    Overview overview {};
    std::array<Hit, max_hits> hits {};
    unsigned hit_count = 0;
    BeatGrid beats;          // its own tempo and beats, for Chop's Beats type
    bool ready = false;
    // A multisampled instrument: no frames of its own, so nothing to trim or chop.
    bool instrument = false;
    unsigned zones = 0, parts = 0, parts_ready = 0, parts_failed = 0, left = 0;
    uint8_t low_key = 127, high_key = 0;
    std::string ignored;     // what it asked for and is not played, named
    std::array<uint16_t, 128> map {}; // for each key, the velocities it has (a bit for each 8)
    // Round 3 "instrument C": each zone as Stage draws it, how far each of its files got, its velocity layers and the
    // longest round robin.
    std::vector<ZoneShape> shapes;
    std::vector<PartState> part_state;
    unsigned layers = 1, rounds = 0;
    std::array<int8_t, 4> stages {{-1, -1, -1, -1}}; // the pad's attack, decay, sustain, release, -1: none named
    int8_t velocity = 100;   // the pad's Velocity Sense
};

// A multisampled instrument's zones as the library loads them (".sfz", an MPC
// keygroup program, a pad of an MPC program or project, ".exs"), its samples
// found on the card. `error` names why where it gives nothing.
SfzRead read_instrument(const std::string& path, std::string* error);

// A file Melodee makes from a slot's audio, on the service thread.
struct SampleJob {
    enum class Kind : uint8_t { Save, Derive };
    Kind kind = Kind::Save;
    uint8_t source = 0;   // the slot the audio comes from
    uint8_t target = 0;   // Derive: the slot the new file goes into
    uint32_t from = 0, to = 0; // frames; to 0: the end
    bool edges = false;   // Extract: 2 ms fades where the slice was cut inside the source (shell/chop.h)
    char name[40] {};     // the file's name, without .wav; a free one is found
    char folder[16] {};   // under the card's Samples ("" itself)
};

class SampleLibrary {
public:
    explicit SampleLibrary(EngineHost& host);
    ~SampleLibrary();

    // MIDI thread: ask for `path` in project slot `slot`; ask for a fresh list.
    bool load(unsigned slot, const std::string& path);
    // MIDI thread: slots asked for by load() not yet read and offered to the audio thread, and instruments still
    // landing (a project's loading).
    unsigned loads_waiting() const {
        unsigned left = asked.load(std::memory_order_relaxed) - answered.load(std::memory_order_acquire);
        return left + landings_open.load(std::memory_order_relaxed);
    }
    void rescan() { scan_wanted.store(true); }
    // Service thread: a take or a render is writing to the card (SD card access waits for it).
    bool busy() const;
    // Service thread: SD card access. Instruments are read from the card, so before it is
    // lent they let go of it (false if they could not in time), and come back after.
    bool release_instruments();
    void restore_instruments();
    bool instruments_away() const { return !away.empty(); }
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
    // MIDI thread: an MPC expansion (.xpn) to install onto the card (shell/expansion_install.h);
    // done() says when it starts and ends, and the browser then shows its folder.
    void install(const std::string& path);
    // Changes each time the list or a loaded sample changes.
    unsigned generation() const { return changes.load(std::memory_order_acquire); }
    // Freeze plan F2a, any thread: each slot's sample now (nullptr: none), for a print host to play, held until
    // release_samples(): one the audio thread lets go of meanwhile is freed only after that.
    std::array<const SampleData*, sample_slots> hold_samples();
    void release_samples();

    // Service thread, every few milliseconds: files, loads, finished takes, recalls and renders.
    void service(Hardware& hardware);

    // MIDI thread (parity phase 6, shell/sample_capture.cpp). Audio Recall:
    // the ring's last 25 s (from the first sound) into slot `slot`, as
    // Samples/Recorded/RECALL n.wav. A render of the mix the engine host is
    // about to play: into slot `slot` (Resample, Samples/Recorded/RESAMPLE
    // n.wav), or straight into Melodee/Mixdowns/<name>.wav on the card (the
    // song). `frames`: how long it should run, to make room up front.
    bool recall(unsigned slot, const RecallOptions& options) { return recalls.push({uint8_t(slot), options}); }
    // Roadmap P4: a mixdown's file as 32-bit float (bits 32) or integer PCM (24, 16: dithered); stems beside it (each
    // track and bus the engine host's StemRing takes, as Melodee/Stems/<name>/NN <stem>.wav, the mix as Mix.wav).
    struct Render {
        bool to_file = false; uint8_t slot = 0; uint32_t frames = 0; char name[40] {};
        uint8_t bits = 32;
        bool stems = false;
        std::array<std::array<char, 24>, StemRing::stem_count> stem_names {};
    };
    bool render(const Render& job) { return renders.push(job); }
    StemRing* stem_ring(); // MIDI thread: made the first time stems are wanted (21 MB), kept after
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
    // Round 3 "instrument C": the zone of slot `n`'s instrument a press of `key` at `velocity` reaches (-1 none),
    // and zone `zone`'s file as its regions name it ("" none). Under the lock.
    int zone_at(unsigned n, int key, int velocity) const;
    std::string zone_file(unsigned n, unsigned zone) const;
    // Pads (round 2, pads B): slot `n`'s waveform between two frames (to 0: its end) in `columns` peaks
    // 0..255, from the overview it keeps; false when the slot holds no audio.
    bool thumbnail(unsigned n, uint32_t from, uint32_t to, uint8_t* out, unsigned columns) const;
    // MIDI thread: the loudest sample of slot `n` between two frames (0..1+), for
    // Normalize; -1 when it is not loaded. Reads the audio under the lock.
    float peak(unsigned n, uint32_t from, uint32_t to) const;
    // MIDI thread: `frame` of slot `n` pulled onto the nearest zero crossing (shell/chop.h's
    // Zero Snap); the frame itself when the slot is not loaded. Reads the audio under the lock.
    uint32_t zero_crossing(unsigned n, uint32_t frame) const;
    // The same going one way (the wheel's fine edit, shell/fine_edit.cpp): the first crossing at or past `frame`.
    uint32_t zero_crossing_toward(unsigned n, uint32_t frame, int way) const;
    // Trim and Chop zoomed in (shell/zoom_view.h): slot `n`'s peaks between two frames in `columns` columns
    // 0..255, read from its audio (the overview is too coarse that close); false when it holds no audio.
    bool detail(unsigned n, uint32_t from, uint32_t to, uint8_t* out, unsigned columns) const;
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
    // The browser's preview read a piece a pass while its start plays (hear()): the file, the sample it fills and
    // the frames of a piece; `path` the file asked for (a program's preview is another file).
    struct Hearing;
    std::unique_ptr<Hearing> hearing;
    bool hear_piece();   // the next piece; false once it is all there (or cut short) and the file let go
    void hear_more();    // a pass: the next piece, unless another file, or none, is wanted now
    void stop_hearing(); // what is there plays to its end; nothing more is read
    void finish(const std::string& text, int failed = -1);
    std::string free_name(const std::string& folder, const std::string& name, const char* extension = ".wav") const;
    void bank_files(); // the bank writes and reads asked for
    void install_service(); // an expansion asked for: opened, then a file of it each pass
    std::unique_ptr<ExpansionInstall> installing;
    std::vector<std::string> installs_wanted; // under `lock`
    // Instruments (sample_library_instruments.cpp): read, then one of their files a pass.
    void begin_instrument(const Request& request);
    void land_next();
    void cancel_landing(unsigned slot);
    void hear_instrument(const std::string& path);
    bool reap();       // what the audio thread has let go of; true if anything went
    // Roadmap P2: a sample handed to the audio thread with its copy at the device's rate made first (while every
    // copy together stays under playback_budget; past it the sampler converts that sample as it plays); and, when the
    // device's rate has changed, one slot's sample a pass handed over again with its copy made anew.
    static constexpr size_t playback_budget = size_t(256) << 20;
    bool offer(unsigned slot, SampleData* data);
    void refresh_playback();
    struct Landing {
        uint8_t slot = 0;
        std::string folder;
        std::string sf2;                        // roadmap P3: an .sf2's preset, its sounds taken from this file
        SfzRead read;
        std::shared_ptr<Multisample> multi;
        std::vector<std::string> names;         // each part's file, as its regions name it
        std::vector<std::vector<uint16_t>> zones; // the zones of each part
        unsigned next = 0;
    };
    std::vector<Landing> landings;
    SampleCache cache;
    bool cache_tidied = false;
    std::vector<std::pair<uint8_t, std::string>> away; // the instruments put down for SD card access

    EngineHost& host;
    MidiQueue<16, Request> requests; // MIDI thread produces, service thread consumes
    std::atomic<unsigned> asked {0}, answered {0}; // requests taken by load(), and read since (loads_waiting())
    std::atomic<unsigned> landings_open {0};        // instruments still landing, a part a pass
    MidiQueue<8, SampleJob> jobs;    // the same
    MidiQueue<8, Done> finished;     // service thread produces, MIDI thread consumes
    std::atomic<bool> scan_wanted {true};
    std::atomic<unsigned> changes {0};
    std::atomic<unsigned> preview_count {0};
    std::atomic<unsigned> take_count {0};
    void save_take();
    // Recall and renders (shell/sample_capture.cpp).
    struct RecallJob { uint8_t slot = 0; RecallOptions options; };
    MidiQueue<4, RecallJob> recalls;
    MidiQueue<4, Render> renders;
    void save_recall(const RecallJob& job);
    void drain_render();
    // A sample made here, `data`'s left and right at `rate` Hz (a take, a recall or a render at the device's): written
    // so, as Samples/<folder>/<stem> n.wav when there is a card, kept as written (adopt(): its own frames play, its
    // 44.1 kHz frames are what the project counts), offered to `slot` and listed; `what` names it in a failure.
    void land(unsigned slot, std::unique_ptr<SampleData> data, unsigned rate, const char* folder, const char* stem, const char* what);
    bool rendering = false;
    Render render_job;
    unsigned render_rate = 44100; // the render's frames, the device's rate when it began
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
    std::unique_ptr<StemRing> stems_ring;                       // roadmap P4
    std::array<WavStream, StemRing::stem_count> stem_files;
    uint32_t stem_open = 0;                                     // the stems being written, a bit each
    std::vector<float> stem_chunk;
    void drain_stems(bool last);
    std::string render_path;
    std::vector<float> render_chunk;
    std::vector<std::unique_ptr<SampleData>> owned; // service thread: every sample the audio thread may hold
    std::array<const SampleData*, sample_slots> data_of {}; // under `lock`: what each slot holds now, for peak()
    unsigned print_holds = 0;                                // under `lock`: hold_samples() not released yet
    std::vector<std::unique_ptr<SampleData>> held_back;      // service thread: let go of by the audio thread while held
    std::array<std::vector<std::string>, sample_slots> part_names; // under `lock`: an instrument's files (zone_file)
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
