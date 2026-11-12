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
// selected track's level, Sends its send, Knob FX slides from its setting
// A to B (round 2), the Compressor its amount, Flex Beat its mix (Step Edit,
// step entry since round 2, has none: Roll's Shift + K1 nudges a note).
// Everywhere else the track's own target (the Fader page, Shift + Pad 9),
// unless it is off.
// Plain functions of the project and the interface, for the test.
namespace melodee {

struct FaderJob {
    enum class Source : uint8_t { None, Target, Page };
    Source source = Source::None; // None: the track's target is off (or it has no track)
    Binding binding;              // the value; unbound for a pad target with no sample on the last pad
    bool centred = false;         // a value either side of a middle (pan, tune): the light is brightest there
    bool learned = false;         // Target: the pot it learned (fader C), not one of the eight
    bool pad = false;             // Target: the last pad hit's own value (its name carries the pad)
};

FaderJob fader_job(const Project& project, const UiState& ui);
// The track's own target as a card (whatever the page): pad values of the last pad hit.
Binding fader_target_binding(const Track& track, FaderTarget target);
// Fader C (round 2): what the fader can learn from a card turned on track `t`
// (one of its values: an engine parameter, an effect's on its chain, a kit
// pad's own value, the mod wheel, its level, pan or a send); None for the
// rest. And the card it is now: unbound where it no longer is one (another
// engine, the effect gone), or a pad's value with no sample on the last pad.
FaderLearned fader_learnable(const Track& track, unsigned t, const Binding& b);
Binding fader_learned_binding(const Track& track, unsigned t);
bool fader_learned_pad(const FaderLearned& learned); // a kit pad's own value (it follows the last pad hit)
// How bright the fader light is for the job, 0..1: the value's place, or its
// nearness to the middle for pan and tune; 0 with no job.
float fader_light_level(const FaderJob& job);
// Round 3 (fader readout B): which of the page's K1-K3 shows the value the fader drives, 0-2, or -1 where none
// does. That card says the fader moved; with none the header does (pages/overlays.cpp).
int fader_twin(const FaderJob& job, const std::array<Binding, 3>& cards);

} // namespace melodee
