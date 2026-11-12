// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/looper.h"
#include "melodee/model/project.h"
#include <array>
#include <cstdint>

// The Looper's loop sheet (B3 MORE on the Looper, docs/LOOPER.md) and the sources a loop can record: pure, for
// the shell, the page and the tests. One screen, no list: the pots set (K1 the loop's source, K2 its length, K3
// its level; Shift + K1-K3 feedback, Quantize and After take, as on the page), the keys act (B1 BACK, B2 COMMIT,
// B3 PLAY or STOP on the next boundary; Shift held, B1 CLEAR, B2 + NOTE, B3 STOP or PLAY at once), the wheel and
// -/+ pick the loop, Push keeps its one job (record, close, overdub, play) and the pads stay the loop grid.
namespace melodee {

// B1-B3, then Shift + B1-B3.
enum class LoopJob : uint8_t { Back, Commit, Toggle, Clear, CommitNote, Now, Count };
constexpr LoopJob loop_job(unsigned key, bool shift) { return LoopJob(key % 3 + (shift ? 3 : 0)); }
// The loop goes on: armed, recording, playing or overdubbing. B3 and Shift + B3 say STOP then, else PLAY.
bool loop_going(const Looper::View& v);
// Whether a job does anything for a loop in this state; `can_commit`: the selected track is a kit with an empty
// pad. BACK always; COMMIT and + NOTE a loop playing or stopped; PLAY / STOP on the boundary a loop with audio
// (again cancels); CLEAR any loop but an empty one (it asks); STOP NOW a loop that goes on, PLAY NOW a stopped one.
bool loop_job_can(LoopJob job, const Looper::View& v, bool can_commit);
// The kit pad a commit lands on: the kit's next empty pad after its last one, round, as Context::next_empty_pad
// finds it; -1 when the track is no kit or has no empty pad.
int commit_pad(const Track& track);

// The mic and the rear line inputs share one ADC: one loop on each can never record at once.
bool loop_source_clashes(const LooperSettings& looper, unsigned loop, uint8_t source);
// What the loops' own hardware inputs need routed (InputRoute::need_*); none while the Looper is off.
unsigned loop_input_needs(const LooperSettings& looper);
// The sources K1 turns through, in order: the input, the tracks that exist, the other loops, the mix, then the
// hardware inputs (Rear, Rear L/R, USB, USB L/R, Mic) that do not clash with another loop's; the loop's own source
// always among them. Returns how many, `at` where its own one is.
unsigned loop_sources(const Project& project, unsigned loop, std::array<uint8_t, looper::source_count>& out, unsigned& at);

} // namespace melodee
