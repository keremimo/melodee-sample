// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/looper.h"
#include "melodee/model/project.h"
#include "melodee/shell/bindings.h"
#include <cstdint>
#include <string>

// The Looper's controls (docs/LOOPER.md), on the MIDI thread: Loop tapped
// twice or held opens its page, the pads there are the loop grid, the page's
// sheet (B3 MORE) holds the rest, and each ask keeps the project's values in
// step with what goes to the audio thread (Command::Looper).
namespace melodee {

class Context;

constexpr uint32_t loop_double_ms = 450; // Loop tapped twice within this: the Looper
constexpr uint32_t loop_hold_ms = 500;   // Loop held this long: the Looper
constexpr uint32_t loop_clear_ms = 600;  // a grid pad of the bottom row held this long clears its loop

// One ask of the Looper, its project values kept (Length, Feedback, Source, the shared settings, on or off).
void looper_do(Context& c, looper::Do what, unsigned loop, int value = 0, uint32_t position = 0);
void set_looper(Context& c, bool on);
// Loop, pressed and let go: a tap is the MPC's (the last pad's loop); twice, or held, the Looper; on the Looper
// it goes back. Shift + Loop stays Reverse.
void loop_press(Context& c, bool pressed);
// The loop grid: each column a loop (pad 1 bottom left). Top row record / overdub / play, then play or stop on
// the next boundary (Shift: at once), undo then redo, hold to clear.
void loop_grid_pad(Context& c, unsigned pad, bool down);
void loop_mute_pad(Context& c, unsigned pad); // mute mode on the Looper: a column mutes its loop track; Shift solos
// The loop track's strip (K1-K3 on MIX, the remote): LoopLevel, LoopPan, LoopSend, LoopBus; mute and solo.
void set_loop_mix(Context& c, unsigned loop, Binding::Kind kind, int value);
void set_loop_mute(Context& c, unsigned loop, bool on);
void set_loop_solo(Context& c, unsigned loop, bool on);
// Often (ms): Loop held, a clear pad held, what the audio thread refused, the metronome while a loop records.
void looper_tick(Context& c, uint32_t now);
// Commit to pad (shell/looper_files.cpp): the loop as a WAV onto the selected kit's next empty pad; `note`
// also writes a note at bar 1 of the kit's pattern as long as the loop, and stops the loop.
void commit_loop(Context& c, unsigned loop, bool note);
// A project's loops beside its file (`path`, the .mldp): written as it is saved (their lengths, tempos and
// layers go into the project first), read back once it is opened.
void save_loop_files(Context& c, const std::string& path);
void load_loop_files(Context& c, const std::string& path);
// The sheet (B3 MORE): its rows, and Push on the one under the cursor.
enum class LoopRow : uint8_t { Source, Toggle, Stop, Undo, Commit, CommitNote, Clear, Off, Count };
void loop_sheet_push(Context& c);
// The next source a loop can record after `source`: the input, the tracks that exist, the other loops, the mix,
// then the hardware inputs of its own (Rear, Rear L/R, USB, USB L/R, Mic) that do not clash with another loop's.
uint8_t next_loop_source(const Project& project, unsigned loop, uint8_t source);
// The mic and the rear line inputs share one ADC: one loop on each can never record at once.
bool loop_source_clashes(const LooperSettings& looper, unsigned loop, uint8_t source);
// What the loops' own hardware inputs need routed (InputRoute::need_*); none while the Looper is off.
unsigned loop_input_needs(const LooperSettings& looper);
std::string loop_source_name(uint8_t source);  // "Input", "Track 3", "L2", "Mix", "Rear L", "USB"
std::string loop_length_name(unsigned bars);   // "Free", "1 bar", "4 bars"
// The remote's `melodee-looper` (docs/LOOPER.md): parsed where the command arrives (an error is said at once),
// done on the MIDI thread. status is read straight from the engine's views.
struct LooperAsk {
    enum class Op : uint8_t { Do, Mix, Mute, Solo, Commit } op = Op::Do;
    looper::Do what = looper::Do::Primary;
    Binding::Kind mix = Binding::Kind::None; // Op::Mix: LoopLevel, LoopPan, LoopSend, LoopBus
    uint8_t loop = 0;
    int value = 0;
    uint32_t position = 0;
};
bool parse_looper(const std::string& argument, LooperAsk& out, std::string& error);
void looper_remote(Context& c, const LooperAsk& ask);
std::string looper_status(const Looper& looper);

} // namespace melodee
