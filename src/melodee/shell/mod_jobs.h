// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/shell/bindings.h"
#include "melodee/shell/context.h"

// Roadmap P4, modulators and macros (model/modulators.h) on the MOD page (Motion's lit MOTION again): what the shell
// does for them. A lane is learned from the last pot or card turned that names one (any page), ADD puts it on the
// modulator or macro in hand.
namespace melodee {

// Context::apply's: the lane `b` turns (a Motion lane of the track in hand, the master's chain, a return), kept as
// the one ADD would take; -1 none.
void learn_lane(Context& c, const Binding& b);
// The modulator's settings and targets to the audio thread (each target with where its lane stands as set).
void send_modulator(Context& c, unsigned n);
// ADD on the MOD page: the lane learned last onto the modulator (an LFO if it was off) or macro in hand.
void mod_add(Context& c);
// Push on the MOD page: the target in hand off it.
void mod_remove(Context& c);
// The MOD page's cards (Context::apply).
bool mod_binding(Context& c, const Binding& b, int value);
// A lane set as a pot would set it (a macro's turn): the track's sound, its chain, a pad, its strip, the master.
void set_lane(Context& c, unsigned track, unsigned param, unsigned pad, int value);

} // namespace melodee
