// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/looper.h"
#include "melodee/model/project.h"
#include "melodee/shell/bindings.h"
#include "melodee/shell/loop_sheet.h"
#include <cstdint>
#include <string>

// The Looper's controls (docs/LOOPER.md), on the MIDI thread: LOOPER on Tracks, or Loop and 16 Levels pressed
// together (shell/looper_chord.h), opens its page, the pads there (and anywhere while Loop is held) are the loop
// grid, the loop's sheet (B3 MORE, shell/loop_sheet.h) holds the rest, and each ask keeps the project's values in
// step with what goes to the audio thread (Command::Looper).
namespace melodee {

class Context;

constexpr uint32_t loop_hold_ms = 500;   // Loop held this long: a hold (its release does nothing), not a tap
constexpr uint32_t loop_chord_ms = 150;  // Shift this soon after Loop: Shift + Loop (later, Shift + a grid pad)
constexpr uint32_t loop_clear_ms = 600;  // a grid pad of the bottom row held this long clears its loop

// One ask of the Looper, its project values kept (Length, Feedback, Source, the shared settings, on or off).
void looper_do(Context& c, looper::Do what, unsigned loop, int value = 0, uint32_t position = 0);
void set_looper(Context& c, bool on);
// Loop, pressed and let go: a tap is the MPC's (the last pad's loop; on Song the clip's brace), done on
// release; held, the pads are the loop grid on any page until it is let go, and the release does nothing.
// Shift + Loop let go within half a second: Reverse; held that long, the Looper on or off instead (off asks
// first while a loop holds audio). Shift pressed during a plain Loop tap makes it Shift + Loop.
void loop_press(Context& c, bool pressed);
// A pad while Loop is held off the Looper: the loop grid's (its release then does nothing). False otherwise.
bool loop_held_pad(Context& c, unsigned pad, bool down);
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
// The loop's sheet (B3 MORE, shell/loop_sheet.h): B1-B3 (`key` 0-2), with Shift the second row, on the selected
// loop. BACK closes it; the rest leave it open (CLEAR asks first); a job that would do nothing does nothing.
void loop_sheet_key(Context& c, unsigned key);
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
