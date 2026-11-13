// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/pattern.h"
#include <cstddef>
#include <vector>

// Edits of chosen notes for Roll, Steps and Step Edit (shell/note_jobs.h): the notes are taken out of the pattern,
// changed as a list and put back (a note landing on another of the same tick, key and pitch replaces it). Pure.
namespace melodee {

// The events at `at` (indexes in order) out of the pattern, in order; and a list of them back in.
std::vector<Event> take_events(Pattern& p, const std::vector<size_t>& at);
void put_events(Pattern& p, const std::vector<Event>& events);

// Chop: each note two steps of Q or longer split in two at the grid line nearest its middle; how many were split.
unsigned split_events(std::vector<Event>& notes, unsigned q);
// Legato: each note held until the next note starts (in `rest` or `notes`; with `per_key` the next of its own key),
// the last until the pattern's end.
void legato_events(std::vector<Event>& notes, const Pattern& rest, bool per_key);
// Reverse: the notes backwards in time over the span they cover, each keeping its length.
void reverse_events(std::vector<Event>& notes);
// Quantize: each note `percent` of the way to its nearest grid line of Q (at least a tick), never past it.
void quantize_events(std::vector<Event>& notes, unsigned q, uint32_t length, unsigned percent);
// The velocity ramp over the notes in order: the last `percent` off the first (-100..100), the ones between evenly.
void ramp_events(std::vector<Event>& notes, int percent);
int ramp_of(const std::vector<Event>& notes); // the ramp the notes have now, read off the first and last

// Arp to notes: each chord (the notes starting on one tick) replaced by what the arp plays over it for the chord's
// length: `mode` (engine/arp.h ArpMode), a note every `rate` (arp_rate_ticks) held `gate` percent of it, over
// `octaves`. `lane` as roll_lane (a kit pad's pitches: rows over C4), else the notes' keys up to `top`.
std::vector<Event> arp_events(const std::vector<Event>& notes, unsigned mode, unsigned rate, unsigned octaves, unsigned gate,
                              uint32_t length, int lane, int top);

} // namespace melodee
