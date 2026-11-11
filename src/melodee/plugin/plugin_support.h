// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <string>

// What plug-ins need beside Melodee, carried inside its binary (the build's
// plugin-support pack, tools/plugin_support/): no-op stand-ins for the GUI
// libraries plug-in editors link (stubs/, tools/plugin_support/make_stubs.py),
// the real libraries plug-ins use for sound that the MPC lacks (libs/:
// libsndfile and its codecs, tools/plugin_support/SURVEY.md), and the 32-bit
// bridge with its armhf runtime (bridge32/). The first plug-in
// to load unpacks them into RAM (/run; the card is noexec) and loads the
// stubs, so a plug-in listing libGL.so.1 binds to them. A build without the
// pack (a computer's) loads only plug-ins that need neither.
namespace melodee {

bool ensure_plugin_support();          // rack thread; true when there is nothing more to do (or no pack)
std::string plugin_support_folder();   // where it was unpacked; empty before, or with no pack

} // namespace melodee
