// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include "melodee/shell/context.h"
#include "melodee/shell/snapshot.h"
#include "melodee/shell/ui_state.h"
#include "runtime/app.h"
#include <atomic>
#include <memory>
#include <mutex>

// The page router and the control grammar. It owns the project and the UI
// state on the MIDI thread: printed buttons open their pages, B1-B3 are the
// page's soft keys, the wheel moves the cursor and Push opens or confirms
// (Shift + Push goes back), pots edit through pickup, Pad Bank or Mute held
// with a pad picks, mutes or solos a track. The screen reads it through
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

    // Service thread: Power was pressed. MIDI thread acts on it in lights().
    void power_pressed() { power.fetch_add(1, std::memory_order_relaxed); }
    // Any thread: LEAVE was chosen on the Exit sheet.
    bool leaving() const { return leave.load(std::memory_order_relaxed); }

    // Screen thread: a copy of what the page needs, taken under a short lock.
    void snapshot(Snapshot& out, uint32_t now_ms);

    // Host previews: the state to draw from, without the hardware.
    Project& project() { return *project_; }
    PatternBank& patterns() { return *patterns_; }
    UiState& ui() { return ui_; }
    Context& context() { return context_; }

private:
    void button(SurfaceButton button, bool pressed);
    void pot(unsigned index, int position, bool syncing);
    void wheel(int delta);
    void pad(unsigned pad, uint8_t velocity, bool down);
    void check_pickup(unsigned index, const Binding& binding);

    EngineHost& host;
    std::unique_ptr<Project> project_;
    std::unique_ptr<PatternBank> patterns_;
    UiState ui_;
    Context context_;
    std::mutex lock; // MIDI thread while it changes state; screen thread while it copies
    std::atomic<float> main_volume {0.7f};
    std::atomic<unsigned> power {0};
    unsigned power_seen = 0;
    std::atomic<bool> leave {false};
};

// The project Melodee starts with: four tracks with sounds and a bass line.
void starting_project(Project& project, PatternBank& patterns);

} // namespace melodee
