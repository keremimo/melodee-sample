// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include "melodee/shell/card_access.h"
#include "melodee/shell/context.h"
#include "melodee/shell/project_kits.h"
#include "melodee/shell/looper_jobs.h"
#include "melodee/shell/meter_lights.h"
#include "melodee/shell/midi_io.h"
#include "melodee/shell/pad_context.h"
#include "melodee/shell/project_store.h"
#include "melodee/shell/sample_library.h"
#include "melodee/shell/snapshot.h"
#include "melodee/shell/ui_state.h"
#include "runtime/app.h"
#include "surface/leds.h"
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
    // MIDI thread (parity phase 9, shell/midi_io.cpp): MIDI in from an external port, its loss, and the timed output.
    void midi(const MidiPortInfo& port, const MidiPacket& packet, uint32_t now_ms);
    void midi_lost();
    void midi_output(MidiOutput& out, MidiIo::Clock::time_point now, uint32_t now_ms);
    void midi_panic(uint32_t now_ms); // `melodee-midi panic`, as MIDI Config's row
    void looper_ask(const LooperAsk& ask, uint32_t now_ms); // `melodee-looper` (shell/looper_jobs.h)
    float volume() const { return main_volume.load(std::memory_order_relaxed); }
    unsigned screen() const { return screen_wanted.load(std::memory_order_relaxed); } // brightness %, 0: leave it
    // Any thread: Settings' sample rate and buffer (periods): the card's from start_settings(), else the default.
    unsigned audio_rate() const { return audio_wanted.load(std::memory_order_relaxed) & 0xfffff; }
    unsigned audio_periods() const { return audio_wanted.load(std::memory_order_relaxed) >> 20; }
    // Before the threads start: the card's settings (Settings::read() text), so that the runtime opens the audio
    // device at their rate and buffer at once. The service thread reads the same file again later.
    void start_settings(const std::string& text);

    // Service thread: Power was pressed. MIDI thread acts on it in lights().
    void power_pressed() { power.fetch_add(1, std::memory_order_relaxed); }
    // Any thread: LEAVE was chosen on the Exit sheet.
    bool leaving() const { return leave.load(std::memory_order_relaxed); }
    // Any thread: POWER OFF was chosen on the Exit sheet and the project is on the card: the MPC may turn off.
    bool powering_off() const { return power_off.load(std::memory_order_relaxed); }
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
    void set_input(InputRoute* input) { input_ = input; context_.input = input; }
    void set_plugins(PluginRack* plugins) { context_.plugins = plugins; } // the app's plug-in rack (plugin/plugin_rack.h)
    // SD card access (parity phase 10b): the MIDI thread asks, the service thread lends.
    CardLink& card() { return card_link; }

private:
    void button(SurfaceButton button, bool pressed);
    void press(SurfaceButton button, bool pressed); // button() inside its undo step (shell/undo_scope.h)
    void pot(unsigned index, int position, bool syncing);
    void wheel(int delta);
    void pad(unsigned pad, uint8_t velocity, bool down);
    void hit(unsigned pad, uint8_t velocity, bool down); // pad() inside its undo step
    void check_pickup(unsigned index, const Binding& binding);
    Takeover pot_takeover(const Binding& binding) const;
    std::string button_text() const; // the button lights for `status`
    std::string lit_pads_text() const; // and the lit pads
    std::string meter_text() const;    // and the meters beside the screen
    std::string meter_db_text() const; // and the levels they show
    std::string pad_summary() const;   // the last pad's Sample Mode values (phase 4)
    std::string p3_summary() const;    // roadmap P3: layers, loop crossfade, voices and their slots
    unsigned follows_total() const;    // roadmap P5: follow actions come on every track since start
    std::atomic<uint32_t> capture_kept {0}; // roadmap P5: notes the capture ring holds, for `status`
    std::atomic<uint32_t> loading_now {0}, load_us_now {0}; // the Loading sheet (bit 0) and samples left; the last parked build
    std::atomic<uint64_t> button_levels {0};
    std::atomic<uint64_t> pad_rungs {0}, pad_hues {0}; // each pad's rung and hue (shell/pad_light.h), 4 bits a pad, for `status`
    std::array<uint32_t, 16> pad_let_go {}; // when each pad was let go, ms + 1: its flare fades from there (shell/pad_lights.h)
    unsigned count_beat_seen = 0;           // the count-in's beat the pads last pulsed for
    uint32_t count_beat_at = 0;             // and when it began, ms
    PadFacts pad_facts() const;             // what pad_context() asks of the shell (shell/pad_context.h)
    std::atomic<uint32_t> meter_bits {0}; // their LEDs (left, right << 8) and the input shown (<< 16), for `status`
    bool record_track_keys() const; // Record: a synth track source plays the pads (Minus / Plus: its octave)
    bool kit_pads() const; // the pads play the kit here: Erase + pad deletes, Shift + Erase copies
    bool song_buttons() const { return ui_.page == PageId::Song && !ui_.song_export && !ui_.shift; } // Chop, Erase, Note Repeat work on the clip
    void end_copy_away();  // copying stops away from the pads it copies

    EngineHost& host;
    std::unique_ptr<Project> project_;
    std::unique_ptr<PatternBank> patterns_;
    UiState ui_;
    Context context_;
    std::mutex lock; // MIDI thread while it changes state; screen thread while it copies
    // Presses the Loading sheet took (ui.loading): their releases are taken too, even after it has gone.
    std::array<bool, 256> eaten_buttons {};
    std::array<bool, 128> eaten_pads {};
    CompTrail comp_trail; // the screen thread's, in snapshot(): the Compressor page's picture over time
    CompBar comp_bar;     // and over one bar of the song (round 2, compressor B)
    HeadTrail trail; // the screen thread's, in snapshot(): heads kept while their wakes fade
    std::atomic<float> main_volume {0.7f};
    std::atomic<unsigned> screen_wanted {0}; // 0 until the settings say; then 20..100 %
    std::atomic<uint32_t> audio_wanted {44100u | 3u << 20}; // rate | periods << 20
    void want_audio_format() { audio_wanted.store(ui_.settings.audio_rate | uint32_t(ui_.settings.audio_periods) << 20, std::memory_order_relaxed); }
    std::atomic<unsigned> power {0};
    unsigned power_seen = 0;
    std::atomic<bool> leave {false}, power_off {false};
    SampleLibrary* library_ = nullptr;
    unsigned library_seen = ~0u;
    unsigned format_seen = 0; // the engine's format_changes() the project was last sent again for
    uint32_t tap_down = 0;
    bool tap_held = false;
    ProjectStore* store_ = nullptr;
    InputRoute* input_ = nullptr;
    CardLink card_link;
    MidiIo midi_io;
    unsigned store_seen = ~0u, settings_seen = ~0u, sounds_seen = ~0u;
    bool settings_loaded = false;
    int leave_after_save = -1; // the saves count to wait for before leaving
    bool off_after_save = false; // and then turn the MPC off instead of starting the stock app
    uint32_t off_saved_at = 0;   // when the save to turn off after landed (ms): the MPC turns off a second later
    std::atomic<uint32_t> page_track {0}, pad_sample {0}, pad_frames {0}, tempo_now {0}; // copies for summary()
    std::atomic<uint32_t> stage_bits {0};
    std::string stage_key_text; // under text_lock
    // Phase 2, for remote checks: Seq Record (0 off, 1 armed, 2 recording), the selected pattern's
    // length in ticks, its notes, its Q and the first note's tick, and the time signature.
    std::atomic<uint32_t> rec_state {0}, pattern_ticks {0}, pattern_events {0}, pattern_q {0}, first_tick {0}, signature_now {0};
    std::atomic<uint32_t> step_now {0}, event_now {0}, step_events_now {0}, event_tick {0}, ask_now {0}, motion_now {0}; // Step Edit, a question, Motion
    // Roll: the cursor's key, whether a note is in focus, the first step in view, the top row's key, the sheet open.
    std::atomic<uint32_t> roll_key_now {0}, roll_focus_now {0}, roll_view_now {0}, roll_top_now {0}, sheet_now {0};
    std::atomic<uint32_t> pad_bits {0}; // phase 4: the last pad's loop, lock, Note On, reverse, mono, filter, link; 16 Levels (summary())
    std::atomic<int32_t> pad_gain {0};
    // Roadmap P3, for remote checks: the last pad's layers (count, how a hit picks) and loop crossfade; the selected
    // track's voices heard now and the sample slots they play (a bit a slot).
    std::atomic<uint32_t> p3_bits {0}, voices_now {0};
    std::atomic<uint64_t> voice_slots {0};
    // Phase 5: Chop (type, slices, the slice in hand, armed), the last pad's region and file, slots
    // used, the browser (rows, cursor, drive, preview) and its folder.
    std::atomic<uint32_t> record_bits {0}; // phase 6: source, monitor, the take's pad + 1, cuts, rendering, pick, threshold
    std::atomic<uint32_t> knob_bits {0}, knob_targets {0}, comp_bits {0}, comp_side {0}; // phase 7: Knob FX and the master compressor (Sidechain, key)
    std::atomic<uint32_t> padfx_bits {0}, punch_slots_bits {0};            // phase 7b: Pad FX and Flex Beat
    std::atomic<uint32_t> fader_bits {0}, fader_value {0};                 // phase 8: the fader
    std::atomic<int> fader_at {-1};                                        // and where it sits
    std::atomic<uint32_t> chop_bits {0}, pad_start_now {0}, pad_end_now {0}, files_bits {0};
    mutable std::mutex text_lock;
    std::string pad_name_text, folder_text; // under text_lock
    // Phase 10: undo depth, Seq held / the sampler map / a gesture open, the last step's name (under text_lock).
    std::atomic<uint32_t> phase10_bits {0};
    std::string undo_text;
    std::string phase10_summary() const;
    std::string input_summary() const;  // phase 6: Input Config, takes, Recall, renders
    std::string fx_summary() const;     // phase 7: Knob FX, the master compressor
    std::string padfx_summary() const;  // phase 7b: Pad FX, Flex Beat
    std::string fader_summary() const;  // phase 8: the fader and the takeover modes
    std::string midi_config_summary() const; // phase 9: MIDI Config
    std::atomic<uint32_t> midi_bits {0}, track_channel {0}, midi_row {0};
    std::string sample_summary() const;
};


} // namespace melodee
