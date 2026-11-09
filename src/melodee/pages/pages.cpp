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
    case PageId::Stage:
    default: return stage_page;
    }
}

} // namespace melodee
