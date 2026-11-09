// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/pages/pages.h"

namespace melodee {

const PageSpec& page_spec(PageId page) {
    switch (page) {
    case PageId::Keys: return keys_page;
    case PageId::Mix: return mix_page;
    case PageId::Sounds: return sounds_page;
    case PageId::Edit: return edit_page;
    case PageId::Steps: return steps_page;
    case PageId::Roll: return roll_page;
    case PageId::Motion: return motion_page;
    case PageId::Patterns: return patterns_page;
    case PageId::Song: return song_page;
    case PageId::Length: return length_page;
    case PageId::Rack: return rack_page;
    case PageId::Sends: return sends_page;
    case PageId::Bus: return bus_page;
    case PageId::Master: return master_page;
    case PageId::Pads: return pads_page;
    case PageId::Samples: return samples_page;
    case PageId::Trim: return trim_page;
    case PageId::Chop: return chop_page;
    case PageId::Levels: return levels_page;
    case PageId::Record: return record_page;
    case PageId::Stage:
    default: return stage_page;
    }
}

} // namespace melodee
