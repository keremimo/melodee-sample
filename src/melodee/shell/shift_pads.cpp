// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/shift_pads.h"
#include "melodee/shell/project_store.h"

namespace melodee {

bool shift_pad(Context& c, unsigned pad) {
    // These pages give Shift + pad a job of their own; phase 1b decides which wins.
    if (c.ui.page == PageId::Patterns || c.ui.page == PageId::Punch) return false;
    switch (pad) {
    case 2: // Pad 3: Double Seq
        c.double_length();
        c.notice("Double Seq");
        return true;
    case 11: // Pad 12: Song
        c.open(PageId::Song);
        return true;
    case 15: // Pad 16: Project, the projects on the card
        c.open(PageId::Project);
        if (c.store) c.store->list(c.project_folder);
        c.ui.project_rows = 64; // until the list arrives
        return true;
    default: return false;
    }
}

} // namespace melodee
