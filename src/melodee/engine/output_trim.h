// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The synth engines' output levels, staged to one reference so the same
// playing sounds and meters about the same on every track. The reference is
// the kits: the MPC's factory samples on the sampler (at unity, panned
// constant power, as the stock MPC plays them) and the 909 already match on
// a beat, about -13 LUFS momentary. A held note of a synth at full velocity
// sits at -15, between one drum hit and a beat. Each engine's gain was
// measured on the host (K-weighted momentary loudness of the mix before main
// volume; C3, C4 and C5): the middle of INIT VOICE and its held factory
// sounds' median, before this trim, at PROPHET -20.3, FM6 -12.1, SID -16.8
// and CZ -17.3. A trim is a plain gain after the voice, so no sound's
// character changes, only its level.
#pragma once

namespace melodee::output_trim {

constexpr float prophet = 1.841f; // +5.3 dB
constexpr float fm6 = 0.716f;     // -2.9 dB
constexpr float sid = 1.230f;     // +1.8 dB
constexpr float cz = 1.303f;      // +2.3 dB

} // namespace melodee::output_trim
