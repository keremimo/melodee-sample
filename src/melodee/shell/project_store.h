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

// Projects as files on the card, run on the service thread: Melodee/Projects
// on the first card ($MELODEE_PROJECTS on a host), in folders. The MIDI
// thread asks (list a folder, save these bytes, load, duplicate, delete, new
// folder, open at start) through a queue; loaded files come back through
// another, and the listing is read under a short lock. On the first pass it
// loads the project set to open at start, if any.
namespace melodee {

struct ProjectEntry {
    enum class Kind : uint8_t { Folder, Project } kind = Kind::Project;
    std::string name, path;
    unsigned tracks = 0, tempo = 0, projects = 0; // a project's tracks and tempo (tenths); a folder's projects
    int64_t modified = 0;
};

struct LoadedProject {
    std::string path, error;
    std::vector<uint8_t> bytes;
};

class ProjectStore {
public:
    ProjectStore();
    ~ProjectStore();

    // MIDI thread.
    void list(const std::string& folder);   // relative to the root; "" is the root
    void save(const std::string& path, std::vector<uint8_t> bytes);
    void load(const std::string& path);
    void duplicate(const std::string& path);
    void remove(const std::string& path);
    void new_folder();
    void open_at_start(const std::string& path);
    void save_settings(const std::string& text); // Melodee's settings (shell/settings.h), kept with the start project
    void rename(const std::string& path, const std::string& name); // a project, in its folder
    void save_sound(const std::string& name, std::vector<uint8_t> bytes); // Melodee/Sounds/<name>.mlds
    // A project path in the folder in view for a typed name: the name itself,
    // or with " 2", " 3"... when a project has it already.
    std::string named_path(const std::string& name) const;
    std::unique_ptr<LoadedProject> take_loaded(); // a loaded project, if one is waiting
    // A free name in the folder in view: "Project 3", or "<base> copy".
    std::string free_path(const std::string& base) const;

    // Service thread.
    void service(Hardware& hardware);

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
    };
    void view(View& out) const;
    unsigned generation() const { return changes.load(std::memory_order_acquire); }
    unsigned saves() const { return saved.load(std::memory_order_acquire); }

private:
    struct Request {
        enum class Kind : uint8_t { List, Save, Load, Duplicate, Remove, NewFolder, OpenAtStart, Settings, Rename, SaveSound } kind;
        std::string path;
        std::vector<uint8_t> bytes;
    };
    void push(Request* request);
    void run(Request& request);
    void scan();
    void publish_message(const std::string& message);

    MidiQueue<32, Request*> requests;           // MIDI thread produces
    MidiQueue<4, LoadedProject*> loaded;        // service thread produces
    std::string root, folder, start, settings;  // service thread
    void write_settings(std::error_code& error);
    void scan_sounds();
    bool started = false;
    unsigned ticks = 0;
    mutable std::mutex lock;
    View known;
    std::atomic<unsigned> changes {0}, saved {0};
};

} // namespace melodee
