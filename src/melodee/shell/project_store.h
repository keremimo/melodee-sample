// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/sound_file.h"
#include "surface/midi.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct Hardware;
struct StorageSpace;

// Projects as files on the card, run on the service thread: Melodee/Projects
// on the first card ($MELODEE_PROJECTS on a host), in folders. The MIDI
// thread asks (list a folder, save these bytes, load, duplicate, delete, new
// folder, open at start) through a queue; loaded files come back through
// another, and the listing is read under a short lock. On the first pass it
// loads the project set to open at start or, with none set, the one last
// opened or saved, so a power-on picks up where the last session left off.
namespace melodee {

struct ProjectEntry {
    enum class Kind : uint8_t { Folder, Project } kind = Kind::Project;
    std::string name, path;
    unsigned tracks = 0, tempo = 0, projects = 0; // a project's tracks and tempo (tenths); a folder's projects
    int64_t modified = 0;
    bool mpc() const { return kind == Kind::Project && path.size() > 4 && !path.compare(path.size() - 4, 4, ".xpj"); } // the MPC's own (shell/mpc_project.h)
};

struct LoadedProject {
    std::string path, error;
    std::vector<uint8_t> bytes;
    bool imported = false; // from an MPC project: the bytes are a Melodee project, with no file of its own yet
    std::string note;      // what the import did, or why it could not
};

class ProjectStore {
public:
    ProjectStore();
    ~ProjectStore();

    // MIDI thread.
    void list(const std::string& folder);   // relative to the root; "" is the root
    // `from`: the file it was saved as before (Save As, freeze plan F2d): its Prints folder copied beside the new one.
    void save(const std::string& path, std::vector<uint8_t> bytes, const std::string& from = "");
    void load(const std::string& path);
    void duplicate(const std::string& path);
    void remove(const std::string& path);
    void new_folder();
    void open_at_start(const std::string& path);
    void opened(const std::string& path);   // the project now open, from its file: the one to resume at the next start
    void save_settings(const std::string& text); // Melodee's settings (shell/settings.h), kept with the start project
    void rename(const std::string& path, const std::string& name); // a project, in its folder
    void save_sound(const std::string& name, std::vector<uint8_t> bytes); // Melodee/Sounds/<name>.mlds
    void move(const std::string& path, const std::string& folder); // a project into a folder ("" the top), its name kept (" 2" on a clash)
    // A project path in the folder in view for a typed name: the name itself,
    // or with " 2", " 3"... when a project has it already.
    std::string named_path(const std::string& name) const;
    std::unique_ptr<LoadedProject> take_loaded(); // a loaded project, if one is waiting
    // A free name in the folder in view: "Project 3", or "<base> copy".
    std::string free_path(const std::string& base) const;

    // Any thread, before the threads start too: where projects live, $MELODEE_PROJECTS or Melodee/Projects on
    // the first card among `storage`'s mounts (empty with none), and the settings file beside them,
    // Melodee/settings: its start= and last= lines, and the rest, which Settings::read() takes.
    struct SettingsFile { std::string start, last, settings; };
    static std::string projects_root(const std::vector<StorageSpace>& storage);
    static SettingsFile read_settings_file(const std::string& root);

    // Service thread.
    void service(Hardware& hardware);
    // Service thread: the card came back from a computer (SD card access): look for it and list it again.
    void rescan() { root.clear(); ticks = 0; }

    // Any thread but audio.
    struct View {
        std::string root, folder, start; // folder relative to root; start: the project opened at start
        std::vector<ProjectEntry> entries;
        uint64_t free_bytes = 0;
        unsigned saves = 0;              // finished saves, so a leave can wait for one
        std::string message;             // the last thing that went wrong, if anything
        std::string settings;            // the settings file's lines but the start one
        bool settings_read = false;      // the card's settings file has been read (or found missing)
        std::vector<UserSound> sounds;   // Melodee/Sounds, by name
        std::vector<std::string> folders; // every folder under the root, relative ("Live/Set 1"), by name
        std::string moved_from, moved_to; // the last move, so an open project can follow its file
    };
    void view(View& out) const;
    unsigned generation() const { return changes.load(std::memory_order_acquire); }
    unsigned saves() const { return saved.load(std::memory_order_acquire); }

private:
    struct Request {
        enum class Kind : uint8_t { List, Save, Load, Duplicate, Remove, NewFolder, OpenAtStart, Opened, Settings, Rename, SaveSound, Move } kind;
        std::string path;
        std::vector<uint8_t> bytes;
        std::string from = {}; // Save: the project's file before (Save As), whose prints come along
    };
    void push(Request* request);
    void run(Request& request);
    void scan();
    void publish_message(const std::string& message);

    MidiQueue<32, Request*> requests;           // MIDI thread produces
    MidiQueue<4, LoadedProject*> loaded;        // service thread produces
    std::string root, folder, start, settings;  // service thread
    std::string last;                           // service thread: the project last opened or saved
    void remember(const std::string& path);     // service thread: `last`, written to the card when it changes
    void write_settings(std::error_code& error);
    void scan_sounds();
    bool started = false;
    unsigned ticks = 0;
    mutable std::mutex lock;
    View known;
    std::atomic<unsigned> changes {0}, saved {0};
};

// The Project list's rows, the one count its list, K1's range and the shell agree on: SD card access at the
// top folder, then the folder's entries (New project and Save are tiles before the list).
inline unsigned project_rows(size_t entries, const std::string& folder) { return unsigned(entries) + (folder.empty() ? 1 : 0); }

} // namespace melodee
