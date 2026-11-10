// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/shell/context.h"
#include "melodee/shell/snapshot.h"
#include "melodee/ui/paint.h"

// A page: what it draws from a snapshot (screen thread) and what its pads,
// wheel, Push, soft keys and Minus / Plus do (MIDI thread). Pots go through
// the page's Bindings (shell/bindings.h). Each page lives in its own file.
namespace melodee {

struct PageSpec {
    void (*draw)(ui::Paint&, const Snapshot&);
    void (*pad)(Context&, unsigned pad, uint8_t velocity, bool down);
    void (*wheel)(Context&, int delta);
    void (*push)(Context&);
    void (*soft)(Context&, int key);  // B1-B3: 0-2
    void (*nudge)(Context&, int way); // Minus -1, Plus +1
};
const PageSpec& page_spec(PageId page);

// Overlays drawn over any page.
void draw_shift_layer(ui::Paint& p, const Snapshot& s);
void draw_exit_sheet(ui::Paint& p, const Snapshot& s);
void draw_name_sheet(ui::Paint& p, const Snapshot& s); // while ui.name is open

} // namespace melodee
