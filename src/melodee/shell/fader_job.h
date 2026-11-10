// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/fader.h"
#include "melodee/shell/bindings.h"
#include "melodee/shell/ui_state.h"

// What the fader does on the page in view (parity phase 8), as a card like
// a knob's, so the shell moves it through the same Context::apply (and Motion
// records it in the same lanes) and the screen and the fader light read it.
// Pages with an obvious job take it over (Kerem, open question 2): Mix the
// selected track's level, Sends its send, Step Edit nudges the event, Knob FX
// its mix, the Compressor its amount, Flex Beat its mix. Everywhere else the
// track's own target (the Fader page, Shift + Pad 9), unless it is off.
// Plain functions of the project and the interface, for the test.
namespace melodee {

struct FaderJob {
    enum class Source : uint8_t { None, Target, Page, Nudge };
    Source source = Source::None; // None: the track's target is off (or it has no track)
    Binding binding;              // the value; unbound for a nudge, or a pad target with no sample on the last pad
    bool centred = false;         // pan and tune: the light is brightest at the middle
};

FaderJob fader_job(const Project& project, const UiState& ui);
// The track's own target as a card (whatever the page): pad values of the last pad hit.
Binding fader_target_binding(const Track& track, FaderTarget target);
// How bright the fader light is for the job, 0..1: the value's place, or its
// nearness to the middle for pan and tune; 0 with no job.
float fader_light_level(const FaderJob& job);

} // namespace melodee
