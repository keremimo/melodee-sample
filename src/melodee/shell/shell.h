// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include "melodee/shell/context.h"
#include "melodee/shell/project_store.h"
#include "melodee/shell/sample_library.h"
#include "melodee/shell/snapshot.h"
#include "melodee/shell/ui_state.h"
#include "runtime/app.h"
#include <atomic>
#include <memory>
#include <mutex>

// The page router and the control grammar. It owns the project and the UI
// state on the MIDI thread: printed buttons open their pages, B1-B3 are the
// page's soft keys, the wheel moves the cursor and Push opens or confirms
// (Shift + Push goes back), pots edit through pickup, Pad Bank held with a
// pad picks a track, mute mode mutes or solos tracks or kit pads, Note
// Repeat latches (shell/pad_bank, mute_mode, note_repeat). The screen reads it through
// snapshot(); the audio thread only ever sees Commands.
namespace melodee {

class EngineHost;

class Shell {
public:
    // Starts with starting_project(); the engine host is loaded from it before audio runs.
    explicit Shell(EngineHost& host);

    // MIDI thread.
    void surface(const SurfaceEvent& event, uint32_t now_ms);
    void lights(SurfaceLights& lights, uint32_t now_ms);
    float volume() const { return main_volume.load(std::memory_order_relaxed); }
    unsigned screen() const { return screen_wanted.load(std::memory_order_relaxed); } // brightness %, 0: leave it

    // Service thread: Power was pressed. MIDI thread acts on it in lights().
    void power_pressed() { power.fetch_add(1, std::memory_order_relaxed); }
    // Any thread: LEAVE was chosen on the Exit sheet.
    bool leaving() const { return leave.load(std::memory_order_relaxed); }
    // Any thread: page, track, the last pad's sample slot and its frames, for `status`.
    std::string summary() const;

    // Screen thread: a copy of what the page needs, taken under a short lock.
    void snapshot(Snapshot& out, uint32_t now_ms);

    // Host previews: the state to draw from, without the hardware.
    Project& project() { return *project_; }
    PatternBank& patterns() { return *patterns_; }
    UiState& ui() { return ui_; }
    Context& context() { return context_; }
    // The app's sample library (service thread); the shell follows its changes.
    void set_library(SampleLibrary* library) { library_ = library; context_.library = library; }
    void set_store(ProjectStore* store) { store_ = store; context_.store = store; }

private:
    void button(SurfaceButton button, bool pressed);
    void pot(unsigned index, int position, bool syncing);
    void wheel(int delta);
    void pad(unsigned pad, uint8_t velocity, bool down);
    void check_pickup(unsigned index, const Binding& binding);
    std::string button_text() const; // the button lights for `status`
    std::string lit_pads_text() const; // and the lit pads
    std::atomic<uint64_t> button_levels {0};
    std::atomic<uint32_t> pads_lit {0}; // the pads lit, a bit each, for `status`
    bool kit_pads() const; // the pads play the kit here: Erase + pad deletes, Shift + Erase copies
    void end_copy_away();  // copying stops away from the pads it copies

    EngineHost& host;
    std::unique_ptr<Project> project_;
    std::unique_ptr<PatternBank> patterns_;
    UiState ui_;
    Context context_;
    std::mutex lock; // MIDI thread while it changes state; screen thread while it copies
    std::atomic<float> main_volume {0.7f};
    std::atomic<unsigned> screen_wanted {0}; // 0 until the settings say; then 20..100 %
    std::atomic<unsigned> power {0};
    unsigned power_seen = 0;
    std::atomic<bool> leave {false};
    SampleLibrary* library_ = nullptr;
    unsigned library_seen = ~0u;
    uint32_t tap_down = 0;
    bool tap_held = false;
    ProjectStore* store_ = nullptr;
    unsigned store_seen = ~0u, settings_seen = ~0u, sounds_seen = ~0u;
    bool settings_loaded = false;
    int leave_after_save = -1; // the saves count to wait for before leaving
    std::atomic<uint32_t> page_track {0}, pad_sample {0}, pad_frames {0}, tempo_now {0}; // copies for summary()
};

// The project Melodee starts with: four tracks with sounds and a bass line.
void starting_project(Project& project, PatternBank& patterns);

} // namespace melodee
