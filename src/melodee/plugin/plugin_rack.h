// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include "melodee/plugin/plugin_files.h"
#include "melodee/plugin/plugin_panel.h"
#include "melodee/plugin/plugin_screen.h"
#include "melodee/plugin/plugin_sleep.h"
#include "melodee/plugin/plugin_watch.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

// Melodee's plug-ins (docs/MELODEE.md, Plug-ins): VST3 bundles, CLAP and VST2
// libraries and Open MPC plug-ins from the card's Melodee/Plugins folder, as PLUGIN
// tracks and as effects in the track, bus and master chains.
//
// The rack follows the project. sync() compares Project::plugins and what uses
// each entry with what is loaded; its own thread then copies a plug-in into
// RAM (the card is mounted noexec), opens it at the device's rate and hands it
// to the engine host, which plays it from its next block. A plug-in nothing
// uses any more is let go the same way, and freed here once the audio thread
// hands it back. VST3 plug-ins are made on a JUCE message thread of the
// rack's; CLAP ones on the rack's own thread (plugin/clap_instance.h);
// 32-bit ARM ones run in a bridge process; the no-op GUI libraries some link
// (libGL, libX11...) and the bridge's runtime come out of the binary into
// /run the first time a plug-in loads. Plug-ins keep their presets
// and settings under $HOME, which is read only on the MPC: before one opens,
// $HOME points at Melodee/Plugin Data on the card.
namespace melodee {

class EngineHost;
class PluginHandle;
class CompanionHost;
struct PluginSkin;

using PluginStates = std::array<std::vector<uint8_t>, max_plugins>;

// What a companion screen (plugin/companion_screen.h) asked of its plug-in, for the MIDI thread to make: a parameter
// (its index, `value` 0..1) or a program (its number in the plug-in). `generation`: the entry's when it was asked, so
// one asked of a plug-in since replaced is dropped.
struct ScreenRequest {
    uint8_t entry = no_plugin;
    uint32_t generation = 0;
    bool program = false;
    int index = 0;
    float value = 0;
};

// Heap freed back to the system (after a plug-in is freed; freeze plan F2d). Not on the audio thread.
void give_back_memory();
// JUCE's message thread for VST3 plug-ins started now (it lasts while any rack does), so a print host's rack does not
// start it among its own threads (shell/print_engine.cpp).
void start_plugin_messages();

class PluginRack {
public:
    // `stage`: put before its plug-ins' folder names under the stage root, so two racks (the live one and a print
    // host's, shell/print_engine.h) never copy into the same folder.
    explicit PluginRack(EngineHost& host, std::string stage = "");
    ~PluginRack(); // after the audio has stopped: frees every plug-in
    PluginRack(const PluginRack&) = delete;
    PluginRack& operator=(const PluginRack&) = delete;

    // MIDI thread: load what the project uses and is not loaded, let go of what
    // it no longer uses; at a new device rate, everything again (keeping its
    // state). Cheap when nothing changed. Freeze plan F2d: `asleep`, a bit a
    // deep-frozen track: an entry used by those tracks alone is not loaded, or
    // is let go with its state kept as it was (saved with the project
    // untouched), and loads with that state once its track wakes.
    void sync(const Project& project, uint32_t asleep = 0);
    bool asleep(unsigned entry) const;
    // A project's file was read: the states it carried, each used the next
    // time its entry loads (sync() right after).
    void set_states(PluginStates states);
    // Saving: each entry's state as it is now (a plug-in still loading: what
    // it will load with). Any thread but audio.
    PluginStates states() const;
    // A copy of `entry` onto `copy` (a track copy): the same plug-in and state.
    void clone(unsigned entry, unsigned copy);
    // The same plug-in opened again at the next sync() (picked again after it failed or stopped working), with the
    // state it last had. The live rack also does it by itself for a plug-in whose file on the card changed
    // (plugin/plugin_watch.h); reload_notice() then names it, once ("" none).
    void reload(unsigned entry);
    std::string reload_notice();
    // One entry's state as it is now, as states() gives it.
    std::vector<uint8_t> state(unsigned entry) const;
    // Undo or redo of a plug-in replaced (shell/plugin_jobs.cpp): the entry opened again at the next sync() with
    // `state`, whichever plug-in the project names for it then.
    void restore(unsigned entry, std::vector<uint8_t> state);

    // MIDI thread: the panel for `entry` with row `row` of its parameters on
    // K1-K3; a parameter by its place among the shown ones, 0..plugin_param_max.
    void panel(unsigned entry, unsigned row, PluginPanel& out) const;
    void set_param(unsigned entry, unsigned shown, int value);
    // A plug-in's own screen (plugin/plugin_screen.h). MIDI thread: screen_view() each tick, the page in view (entry
    // no_plugin: none); screen_input() an input on that page, handed to the plug-in by the screen thread;
    // set_screen_param() a value for the parameter its screen puts on K`k`. screen_picture(): the newest picture of
    // the screen in view, null none (any thread). panel() gives `screen` once the plug-in has drawn and named K1-K3.
    void screen_view(const ScreenView& view);
    void screen_input(unsigned entry, const ScreenInput& input);
    // The printed buttons the page of `entry`'s screen takes now (screen_key_mute, screen_key_erase), 0 none. Any thread.
    uint8_t screen_keys(unsigned entry) const;
    void set_screen_param(unsigned entry, unsigned k, int value);
    ScreenPicturePtr screen_picture() const;
    // Companion screens (plugin/companion_screen.h, sdk/melodee-screen/melodee_companion.h). MIDI thread:
    // take_screen_requests() what the screens asked for since, to make as a pot turn does; set_param_index() one
    // parameter by its index in the plug-in (a companion's edit, 0..plugin_param_max) while the entry is still on
    // `generation`; screen_notice() why a screen stopped, once ("" none). screens() for `status`: entry:folder:state
    // each, comma separated.
    void take_screen_requests(std::vector<ScreenRequest>& out);
    void set_param_index(unsigned entry, uint32_t generation, int index, int value);
    uint32_t live_generation(unsigned entry) const; // the generation the entry's plug-in was loaded on, 0 none
    std::string screen_notice();
    std::string screens() const;
    // Its skin (plugin/plugin_skin.h), null none; loaded with it and never changed after. Any thread.
    std::shared_ptr<const PluginSkin> skin(unsigned entry) const;
    // PRESETS: the plug-in's named programs, and one of them picked (by its
    // number in the plug-in), switched to on the rack's thread. Picking the
    // one it is on loads it again (edits since are dropped). A CLAP plug-in's
    // presets are files: listed once the rack has looked for them (`finding`).
    PluginPrograms programs(unsigned entry) const;
    void select_program(unsigned entry, int program);
    PluginStatus status(unsigned entry) const;
    int instrument(unsigned entry) const; // 1, 0, or -1 while not known
    // A drum plug-in's voices (plugin/drum_map.h) on `entry`; none for any other.
    void drum_voices(unsigned entry, DrumVoices& out) const;

    // The picker. set_card(): the card's top folder ("" none), from the sample
    // library; the picker starts in its Plugins folder and goes up as far as
    // the card's top, so a plug-in anywhere on the card can be picked.
    void set_card(const std::string& top);
    // "" the Plugins folder itself. With `row` (a listed plug-in's path, and its id in a bundle of several), the
    // listing's came_from is where that row is now. A folder no longer there lists the Plugins folder.
    void browse(const std::string& folder, const std::string& row = {}, int32_t row_id = 0);
    void browse_up();
    struct View {
        std::string root;   // the card's Melodee/Plugins folder; empty: no card
        std::string folder; // the folder listed
        bool top = true;    // the card's top: nothing above it to go back to
        bool home = true;   // the Plugins folder itself
        int came_from = -1; // after going up: the row of the folder it came out of; after browse() with a row: that row
        std::vector<PluginFile> files;
    };
    View view() const;
    unsigned changes() const { return change_count.load(std::memory_order_acquire); } // the view or a status moved
    // The web page installed, replaced or deleted something on the card (any thread). What opening a plug-in found is
    // forgotten for one gone or changed since, and card_changes() counts up: an open picker lists again.
    void card_changed();
    unsigned card_changes() const { return card_count.load(std::memory_order_acquire); }
    // For `status`: each entry in use as entry:state:name:format:load (tenths of a percent), comma separated.
    std::string summary() const;
    // About (round 2): how many plug-ins are loaded and running, and their share of the audio time together.
    struct Usage { unsigned loaded = 0; float load = 0; };
    Usage usage() const;

private:
    struct Entry : PluginPlan {           // its bookkeeping (plugin/plugin_sleep.h), and:
        PluginHandle* live = nullptr;     // offered to the host (owned in `handles`)
        bool transport = true;            // the project's TRANSPORT for it (PluginRef::transport), for the next one too
        std::string preset;               // the preset picked last (PluginRef::preset), named again when it reopens
        std::string error;
        PluginFile file;
        FileWatch watch;                  // its file on the card, as it was opened (plugin/plugin_watch.h)
    };
    struct Job {
        enum class Kind : uint8_t { Load, Unload, Browse, Probe, Program, Sleep, Presets } kind = Kind::Browse;
        unsigned entry = 0;
        uint32_t generation = 0;
        std::string path;
        double rate = 0;
        int32_t id = 0;
        std::string row {}; // Browse: the row to find again (browse())
    };
    void run();                                     // the rack's thread
    void load(const Job& job);
    void unload(unsigned entry, uint32_t generation);
    void sleep(unsigned entry, uint32_t generation); // freeze plan F2d: its state taken, then let go as unload() does
    void switch_program(unsigned entry, uint32_t generation, int program);
    void find_presets(unsigned entry, uint32_t generation); // a CLAP plug-in's, once it plays
    std::string use_plugin_home();                  // before a plug-in opens: $HOME on the card; it ("" no card)
    void list(const std::string& folder, const std::string& row, int32_t row_id); // `row` as in browse()
    void probe_listed();                            // instrument or effect, for listed plug-ins not known yet
    void free_retired();                            // plug-ins the audio thread has handed back
    void watch_files();                             // plug-ins whose file changed: opened again (live rack only)
    void keep_state(Entry& entry);                  // a working plug-in's state now into `state` (under `lock`)
    void idle_plugins();                            // CLAP plug-ins' main-thread requests
    void write_page_problems(const std::string& top); // the page files' problems on the card (plugin/page_file.h)
    void write_screen_problems(const std::string& top); // the companion screens' (plugin/companion_package.h)
    void find_companion(PluginHandle& handle, const PluginFile& file, const std::string& top, const std::string& stage);
    void destroy(PluginHandle* handle);
    void screen_run();                              // the screen thread: draws the screen in view
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
    // `time`: the file's or the folder's own when it was opened (card_changed() forgets it once that moves).
    struct Known { std::string path, name; int8_t instrument = 0; std::vector<Class> classes; std::filesystem::file_time_type time {}; };
    std::vector<Known> known;
    Known probe(const PluginFile& file);            // opens it just long enough to ask
    // Page files that could not be used, by file, and what was last written of them (rack's thread only).
    std::map<std::string, std::vector<std::string>> page_problems;
    std::string page_problems_written;
    std::string stage_tag;
    uint32_t stage_count = 0;
    std::atomic<unsigned> change_count {0}, card_count {0};
    std::thread thread;
    // The screen thread's: what the MIDI thread told it and the newest picture (under `screen_lock`, never held while
    // `lock` is taken); `screen_calls` held while it calls into a plug-in, so destroy() frees none meanwhile.
    mutable std::mutex screen_lock;
    std::condition_variable screen_wake;
    bool screen_stop = false, screen_poked = false;
    ScreenView screen_now;
    std::deque<std::pair<unsigned, ScreenInput>> screen_inputs;
    ScreenPicturePtr screen_latest;
    std::mutex screen_calls;
    // The companion screens' process (plugin/companion_host.h), used under `screen_calls` only; a session number for
    // each screen made (rack's thread); folders whose screen stopped this run, not tried again, and why screens
    // stopped, for the shell's notice (under `lock`); problems by folder, as written (rack's thread).
    std::unique_ptr<CompanionHost> companions;
    uint32_t companion_sessions = 0;
    std::set<std::string> companion_stopped;
    std::vector<std::string> screen_notices;
    std::vector<std::string> reload_notices;        // plug-ins opened again as their file changed (under `lock`)
    uint32_t watch_last = 0;                        // when watch_files() last looked (rack's thread)
    std::map<std::string, std::vector<std::string>> screen_problems;
    std::string screen_problems_written;
    std::thread screen_thread;
};

} // namespace melodee
