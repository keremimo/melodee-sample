// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include "melodee/plugin/plugin_files.h"
#include "melodee/plugin/plugin_panel.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Melodee's plug-ins (docs/MELODEE.md, Plug-ins): VST3 bundles, VST2 libraries
// and Open MPC plug-ins from the card's Plugins folder, as PLUGIN tracks and as
// effects in the track, bus and master chains.
//
// The rack follows the project. sync() compares Project::plugins and what uses
// each entry with what is loaded; its own thread then copies a plug-in into
// RAM (the card is mounted noexec), opens it at the device's rate and hands it
// to the engine host, which plays it from its next block. A plug-in nothing
// uses any more is let go the same way, and freed here once the audio thread
// hands it back. VST3 plug-ins are made on a JUCE message thread of the
// rack's; 32-bit ARM ones run in a bridge process; the no-op GUI libraries
// some link (libGL, libX11...) and the bridge's runtime come out of the
// binary into /run the first time a plug-in loads. Plug-ins keep their presets
// and settings under $HOME, which is read only on the MPC: before one opens,
// $HOME points at Melodee/Plugin Data on the card.
namespace melodee {

class EngineHost;
class PluginHandle;

using PluginStates = std::array<std::vector<uint8_t>, max_plugins>;

class PluginRack {
public:
    explicit PluginRack(EngineHost& host);
    ~PluginRack(); // after the audio has stopped: frees every plug-in
    PluginRack(const PluginRack&) = delete;
    PluginRack& operator=(const PluginRack&) = delete;

    // MIDI thread: load what the project uses and is not loaded, let go of what
    // it no longer uses; at a new device rate, everything again (keeping its
    // state). Cheap when nothing changed.
    void sync(const Project& project);
    // A project's file was read: the states it carried, each used the next
    // time its entry loads (sync() right after).
    void set_states(PluginStates states);
    // Saving: each entry's state as it is now (a plug-in still loading: what
    // it will load with). Any thread but audio.
    PluginStates states() const;
    // A copy of `entry` onto `copy` (a track copy): the same plug-in and state.
    void clone(unsigned entry, unsigned copy);
    // The same plug-in opened again at the next sync() (picked again after it failed or stopped working), with the
    // state it last had.
    void reload(unsigned entry);

    // MIDI thread: the panel for `entry` with row `row` of its parameters on
    // K1-K3; a parameter by its place among the shown ones, 0..plugin_param_max.
    void panel(unsigned entry, unsigned row, PluginPanel& out) const;
    void set_param(unsigned entry, unsigned shown, int value);
    // PRESETS: the plug-in's named programs, and one of them picked (by its
    // number in the plug-in), switched to on the rack's thread. Picking the
    // one it is on loads it again (edits since are dropped).
    PluginPrograms programs(unsigned entry) const;
    void select_program(unsigned entry, int program);
    PluginStatus status(unsigned entry) const;
    int instrument(unsigned entry) const; // 1, 0, or -1 while not known

    // The picker. set_card(): the card's top folder ("" none), from the sample
    // library; the picker starts in its Plugins folder.
    void set_card(const std::string& top);
    void browse(const std::string& folder); // "" the Plugins folder itself
    void browse_up();
    struct View {
        std::string root;   // the card's Plugins folder; empty: no card
        std::string folder; // the folder listed
        bool top = true;    // nothing above it to go back to
        int came_from = -1; // after going up: the row of the folder it came out of
        std::vector<PluginFile> files;
    };
    View view() const;
    unsigned changes() const { return change_count.load(std::memory_order_acquire); } // the view or a status moved
    // For `status`: each entry in use as entry:state:name:format:load (tenths of a percent), comma separated.
    std::string summary() const;

private:
    struct Entry {
        std::string path;                 // what is loaded or loading; empty: none
        int32_t id = 0;                   // which plug-in of its bundle (0: the first)
        uint32_t generation = 0;          // bumped at each change, so a slow load that lost the race is dropped
        double rate = 0;                  // what it was opened at
        PluginHandle* live = nullptr;     // offered to the host (owned in `handles`)
        PluginStatus status = PluginStatus::None;
        std::string error;
        std::vector<uint8_t> state;       // to load with
        bool reload = false;              // a new project: open it again (with `state`) even where the path is the same
        PluginFile file;
    };
    struct Job {
        enum class Kind : uint8_t { Load, Unload, Browse, Probe, Program } kind = Kind::Browse;
        unsigned entry = 0;
        uint32_t generation = 0;
        std::string path;
        double rate = 0;
        int32_t id = 0;
    };
    void run();                                     // the rack's thread
    void load(const Job& job);
    void unload(unsigned entry, uint32_t generation);
    void switch_program(unsigned entry, uint32_t generation, int program);
    void use_plugin_home();                         // before a plug-in opens: $HOME on the card
    void list(const std::string& folder, int came_from);
    void probe_listed();                            // instrument or effect, for listed plug-ins not known yet
    void free_retired();                            // plug-ins the audio thread has handed back
    void destroy(PluginHandle* handle);
    bool offer(unsigned entry, PluginHandle* handle);
    void push(Job job);
    void changed() { change_count.fetch_add(1, std::memory_order_acq_rel); }
    bool referenced(const Project& project, unsigned entry) const;

    EngineHost& host;
    mutable std::mutex lock;                        // everything below but the thread
    std::array<Entry, max_plugins> entries {};
    std::vector<std::unique_ptr<PluginHandle>> handles; // live ones and those the audio thread still holds
    std::vector<PluginHandle*> retiring;            // offered away: freed once the host gives them back
    std::vector<std::pair<unsigned, PluginHandle*>> unoffered; // offers the host's queue had no room for yet
    std::deque<Job> jobs;
    std::condition_variable wake;
    bool stopping = false;
    std::string card_top, root, folder;
    View shown;
    // What opening a plug-in once found: its own name, instrument or effect, and every plug-in a bundle holds.
    struct Class { int32_t id = 0; std::string name; int8_t instrument = 0; };
    struct Known { std::string path, name; int8_t instrument = 0; std::vector<Class> classes; };
    std::vector<Known> known;
    Known probe(const PluginFile& file);            // opens it just long enough to ask
    uint32_t stage_count = 0;
    std::atomic<unsigned> change_count {0};
    std::thread thread;
};

} // namespace melodee
