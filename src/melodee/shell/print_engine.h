// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/recall_ring.h"
#include "melodee/engine/engine_host.h"
#include "melodee/model/print.h"
#include "melodee/shell/auto_sample.h"
#include "melodee/model/project.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Freeze plan F2a: the print engine. One thread at SCHED_IDLE (it takes only
// the time nothing else wants, so the live audio and its render workers never
// wait for it) prints the patterns of one track at a time: for each, a print
// host (EngineHost::Lean) holding print_project(), with its own plug-in rack
// (its plug-ins staged in folders of their own beside the live rack's), plays
// the pattern as fast as it goes (engine/print_render.h), and the sound is
// written as a 32-bit float WAV at the device's rate in "<project> Prints/". A pattern
// whose record already has the same hash and whose file is there is skipped.
// The samples it plays are the live library's, held while it runs.
namespace melodee {

class SampleLibrary;
class StreamPager;
struct SampleData;
struct FreezePack;

struct PrintJob {
    std::string project_path;            // whose prints (results for another project are dropped)
    std::string card_top;                // the card's top folder, where the plug-ins are
    unsigned track = 0;
    std::vector<uint8_t> patterns;       // to print, in this order
    PrintTap tap = PrintTap::PostFx;
    PrintTail tail = PrintTail::Loop;
    std::unique_ptr<Project> project;    // copies, as they are now
    std::unique_ptr<PatternBank> bank;
    std::array<std::vector<uint8_t>, max_plugins> states; // each plug-in's state now (PluginRack::states())
    PrintTable known;                    // the prints as they stand
    // Freeze plan F2b: once printed, the patterns' prints mapped for the audio thread to play (a FreezeReady).
    bool freeze = false;
    bool deep = false;                   // freeze plan F2d: its pack plays as a deep freeze (FreezePack::deep)
    uint32_t serial = 0;                 // the freeze it is for (the shell drops one a later freeze replaced)
    // The auto sampler: the track's instrument played a note at a time into `sample_folder` as `sample_name`.xpm
    // and its WAVs, not its patterns printed.
    bool autosample = false;
    // On a MIDI track: the instrument outside, played over the track's MIDI and heard at `input` (the live engine's
    // input ring, on the input the track takes as its sound) in real time (engine/external_notes.h).
    bool external = false;
    const RecallRing* input = nullptr;
    bool probing = false;                // the check alone (AutoFindings), nothing sampled
    bool probe_first = false;            // autosample: the check, then sampled as it heard (`melodee-autosample ... auto`)
    AutoSampleSettings sampling;
    std::string sample_folder, sample_name;
};

// Freeze plan F2c: a print's first pass in small, for Stage: the loudest of each column, 0..255 full scale.
constexpr unsigned print_peak_columns = 96;
using PrintPeaks = std::array<uint8_t, print_peak_columns>;

// Freeze plan F2b: a frozen track's prints ready to play, or why not.
struct FreezeReady {
    std::string project_path;
    unsigned track = 0;
    uint32_t serial = 0;
    FreezePack* pack = nullptr; // the caller's to send or give back (discard()); nullptr: failed
    std::string error;
    std::vector<std::pair<uint8_t, PrintPeaks>> peaks; // freeze plan F2c: each pattern's print, in small
    // Patterns without notes whose prints are silent (their plug-in plays nothing by itself there): left out of the
    // pack, a bit each, for the shell to take out of the freeze. They play live, as silent as their print.
    std::array<uint64_t, (pattern_count + 63) / 64> silent {};
};

struct PrintResult {
    std::string project_path;
    PrintRecord record;
    bool made = false;     // a new file (false: skipped as unchanged, or failed)
    std::string error;     // empty: made or skipped
    double seconds = 0;    // wall time, the plug-ins' opening included
    double times_real = 0; // the pattern's frames rendered against the wall time of the rendering alone
    double host_mb = 0;    // the print host's resident memory (and its rack's plug-ins), measured as it was made
    uint64_t bytes = 0;    // the file's
};

// Freeze plan F2c: how the job under way goes, for the PRINT sheet.
struct PrintProgress {
    bool busy = false;
    uint8_t track = 0;
    bool freeze = false;
    bool autosample = false;   // the auto sampler's notes (`pattern`: the key, `count`: the samples)
    uint32_t serial = 0;       // the freeze it is for
    int16_t pattern = -1;      // the pattern printing now, -1 none
    float fraction = 0;        // of it, rendered
    float times_real = 0;      // its speed against real time (0: not known yet)
    float seconds_left = 0;    // the job's, estimated from the pattern printing now
    std::array<uint64_t, (pattern_count + 63) / 64> done {}, failed {}; // the job's patterns through (made or unchanged), and failed
    uint16_t count = 0, through = 0;   // the job's patterns, and how many are through
    // The auto sampler's note under way, as AUTO SAMPLE draws it: its level a bar at a time (0-255 for -36 to 0 dB),
    // `wave_hold` the bar where the key is let go, and the layer's velocity. The bars cover more time each (two
    // merged into one) when a long release outgrows them.
    static constexpr unsigned wave_bars = 128;
    std::array<uint8_t, wave_bars> wave {};
    uint8_t wave_count = 0, wave_hold = 0, velocity = 0;
    // The check under way (`checking`): how far it has got (0 type, 1 glide or sounds, 2 accent, 3 takes, 4 done) and
    // what it has heard so far.
    bool checking = false;
    bool opened = false; // the check's plug-in is open in the print host: its notes play (before that it is opening)
    uint8_t check_step = 0;
    AutoFindings found;
    bool is_done(unsigned p) const { p %= pattern_count; return (done[p / 64] >> (p % 64)) & 1; }
    bool is_failed(unsigned p) const { p %= pattern_count; return (failed[p / 64] >> (p % 64)) & 1; }
};

struct NoteSource; // where the auto sampler's notes are played and heard (print_engine.cpp)

class PrintEngine {
public:
    explicit PrintEngine(SampleLibrary* library);
    ~PrintEngine(); // a print under way stops
    PrintEngine(const PrintEngine&) = delete;
    PrintEngine& operator=(const PrintEngine&) = delete;

    // MIDI thread: false while a job runs.
    bool start(PrintJob job);
    void cancel(); // the job under way stops after the block it is on
    bool busy() const { return running.load(std::memory_order_acquire); }
    // MIDI thread: each pattern's result, in order.
    bool take(PrintResult& out);
    // MIDI thread: an auto sampler job's program, made or not.
    bool take_sampled(AutoSampleResult& out);
    // MIDI thread: a check's findings.
    bool take_probed(AutoProbeResult& out);
    // An instrument outside: its notes, sent by the MIDI thread once the input's clock (RecallRing::written()) has
    // reached their frame, so their lengths do not hang on the print thread's turn. post(): the print thread;
    // due(): the MIDI thread, each command whose frame has come, in order.
    void post(const Command& command, uint64_t at);
    void forget(); // the print thread: what was posted and not sent yet is not sent (a run of notes called off)
    bool due(uint64_t now, Command& out);
    // Freeze plan F2b. The pager the audio thread reads prints with (set before any freeze). MIDI thread: a freeze's
    // prints ready; a pack the audio thread has finished with, freed on the print thread once the pager has let go of
    // its streams (and its files' mappings with it).
    void set_pager(StreamPager* p) { pager = p; }
    bool take_ready(FreezeReady& out);
    void discard(FreezePack* pack);
    // Any thread: what it is doing and the latest results, a line each (`melodee-print status`); a line of its own.
    std::string status() const;
    void note(const std::string& line);
    // Any thread: the job under way, as far as it has gone (freeze plan F2c).
    PrintProgress progress() const;

private:
    void run();
    PrintResult print_one(const PrintJob& job, unsigned pattern, const std::array<const SampleData*, sample_slots>& samples);
    // `heard_by`: the check's own notes, opened already (an instrument outside, checked in the same job); none: opened here.
    AutoSampleResult sample_track(const PrintJob& job, const std::array<const SampleData*, sample_slots>& samples,
                                  std::unique_ptr<NoteSource> heard_by = nullptr);
    // `keep`: where its notes are left once it is through, for sample_track() to go on with.
    AutoProbeResult probe_track(const PrintJob& job, const std::array<const SampleData*, sample_slots>& samples,
                                std::unique_ptr<NoteSource>* keep = nullptr);
    FreezeReady freeze_pack(const PrintJob& job, const PrintTable& known);
    void empty_trash(); // print thread: the packs discarded, freed
    SampleLibrary* library;
    StreamPager* pager = nullptr;
    std::deque<FreezeReady> ready;   // under lock
    std::vector<FreezePack*> trash;  // under lock
    mutable std::mutex lock;
    std::condition_variable wake;
    std::unique_ptr<PrintJob> waiting; // under lock
    std::deque<PrintResult> results;   // under lock
    std::deque<AutoSampleResult> sampled; // under lock
    std::deque<AutoProbeResult> probed;   // under lock
    std::deque<std::pair<uint64_t, Command>> outbox; // under lock: post()'s, by frame
    std::deque<std::string> lines;     // under lock: the latest results
    std::string doing;                 // under lock
    PrintProgress shown;               // under lock
    bool stopping = false;             // under lock
    std::atomic<bool> running {false}, cancelled {false};
    std::thread thread;
};

} // namespace melodee
