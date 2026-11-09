// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/pages/page.h"

// Each page file defines one of these; pages.cpp maps PageId to them.
namespace melodee {
// Shift + Push on Song: a marker at the cursor, or its next name (the one
// place Shift + Push is not Back; Kerem's choice).
void song_marker(Context& c);
extern const PageSpec stage_page, keys_page, mix_page, sounds_page, edit_page, steps_page, roll_page, motion_page, patterns_page, song_page, length_page;
}
