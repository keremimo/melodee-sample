// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/print.h"
#include "melodee/model/project.h"
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
    uint32_t serial = 0;                 // the freeze it is for (the shell drops one a later freeze replaced)
};

// Freeze plan F2b: a frozen track's prints ready to play, or why not.
struct FreezeReady {
    std::string project_path;
    unsigned track = 0;
    uint32_t serial = 0;
    FreezePack* pack = nullptr; // the caller's to send or give back (discard()); nullptr: failed
    std::string error;
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
    // Freeze plan F2b. The pager the audio thread reads prints with (set before any freeze). MIDI thread: a freeze's
    // prints ready; a pack the audio thread has finished with, freed on the print thread once the pager has let go of
    // its streams (and its files' mappings with it).
    void set_pager(StreamPager* p) { pager = p; }
    bool take_ready(FreezeReady& out);
    void discard(FreezePack* pack);
    // Any thread: what it is doing and the latest results, a line each (`melodee-print status`); a line of its own.
    std::string status() const;
    void note(const std::string& line);

private:
    void run();
    PrintResult print_one(const PrintJob& job, unsigned pattern, const std::array<const SampleData*, sample_slots>& samples);
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
    std::deque<std::string> lines;     // under lock: the latest results
    std::string doing;                 // under lock
    bool stopping = false;             // under lock
    std::atomic<bool> running {false}, cancelled {false};
    std::thread thread;
};

} // namespace melodee
