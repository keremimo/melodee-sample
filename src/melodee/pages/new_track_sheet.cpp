// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The New track sheet (shell/new_track.h) over the page: what an empty track
// becomes, as cards. The wheel lights one, Push makes it, B1 BACK.
#include "melodee/pages/common.h"
#include "melodee/shell/new_track.h"
#include "melodee/ui/widgets.h"
#include <algorithm>

namespace melodee {
using namespace ui;

void draw_new_track_sheet(Paint& p, const Snapshot& s) {
    auto c = colour_of(s);
    p.fill(0, zone::soft_y, zone::width, zone::soft_h, colour::bg);
    soft_keys(p, SoftKeys {{"BACK", "", ""}, -1, c, {KeyIcon::Back, KeyIcon::None, KeyIcon::None}});
    CardSheet m;
    m.chip = "TRACK " + juce::String(s.ui.track % track_count + 1);
    m.chip_colour = c; m.accent = c;
    m.title = "New track";
    juce::String channel = "CH " + juce::String(std::clamp<int>(s.ui.new_channel, 1, 16));
    m.items[size_t(NewChoice::Plugin)] = card_item(Icon::Plug, "PLUGIN", "VST3 / VST2");
    m.items[size_t(NewChoice::Kit)] = card_item(Icon::Pads, "KIT", "EMPTY");
    m.items[size_t(NewChoice::Samples)] = card_item(Icon::Folder, "KIT", "SAMPLES");
    m.items[size_t(NewChoice::Record)] = card_item(Icon::Mic, "KIT", "RECORD");
    m.items[size_t(NewChoice::MidiOut)] = card_item(Icon::Din, "MIDI", "OUT JACK " + juce::String::fromUTF8("\xc2\xb7 ") + channel);
    m.items[size_t(NewChoice::MidiUsb)] = card_item(Icon::Din, "MIDI", "USB-C " + juce::String::fromUTF8("\xc2\xb7 ") + channel);
    m.items[size_t(NewChoice::MidiAllUsb)] = card_item(Icon::Din, "MIDI", "ALL USB " + juce::String::fromUTF8("\xc2\xb7 ") + channel);
    m.count = new_choice_count;
    m.current = s.ui.new_pick % new_choice_count;
    card_sheet(p, m);
}

} // namespace melodee
