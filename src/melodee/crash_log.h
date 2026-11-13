// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <cstdint>
#include <string>

// Crash reports. When Melodee crashes (SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT, SIGTRAP, or an exception no one
// caught), a report goes to the card's Melodee/Crashes/crash-<unix time>.txt and to stderr (the journal): the
// signal and fault address, the thread, pc / lr / sp, the binary's build id and load address, a backtrace, the last
// presses (oldest first) and the executable mappings. Addresses in Melodee itself are given as offsets into the
// binary too, for addr2line against the same build.
namespace melodee::crash_log {

// The handlers, once, as the app starts (before its threads, so each inherits the mask).
void install();
// Where reports go: the card's Melodee/Crashes (created now); empty: stderr only. Any thread.
void set_folder(const std::string& folder);
// One line of what happened lately (a press and the page it was on), kept in a ring the report ends with. Any
// thread; never blocks. `key` non-zero: a note with the same key as the one before replaces it (a pot turning).
void note(const char* text, uint32_t key = 0);

} // namespace melodee::crash_log
