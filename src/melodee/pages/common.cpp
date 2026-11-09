// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/pages/common.h"
#include "melodee/model/mod_matrix.h"
#include "melodee/model/scales.h"
#include "melodee/shell/bindings.h"
#include <algorithm>
#include <cstdio>

namespace melodee {

juce::Colour colour_of(const Snapshot& s) { return ui::track_colour(s.ui.track); }

ui::Header header_of(const Snapshot& s) {
    ui::Header h;
    h.playing = s.playing;
    h.recording = s.ui.recording;
    char bpm[16];
    int tempo = s.project.tempo;
    if (tempo % 10) std::snprintf(bpm, sizeof(bpm), "%d.%d", tempo / 10, tempo % 10);
    else std::snprintf(bpm, sizeof(bpm), "%d", tempo / 10);
    h.bpm = bpm;
    return h;
}

ui::HeaderItem tag_of(const Snapshot& s, unsigned track) {
    const auto& t = s.project.tracks[track % track_count];
    const auto& info = engine_info(t.engine);
    return ui::track_tag(int(track % track_count) + 1, t.used() ? info.name : "", t.used() ? t.name : "Empty",
                         ui::track_colour(track));
}

ui::SoftKeys play_keys(const Snapshot& s, int active) { return {{"STAGE", "KEYS", "MIX"}, active, colour_of(s)}; }

std::array<ui::KnobCard, 3> cards_of(const Snapshot& s) {
    using Kind = Binding::Kind;
    auto bindings = page_bindings(s.project, s.ui, s.pattern);
    const auto& track = s.track();
    std::array<ui::KnobCard, 3> cards;
    for (unsigned n = 0; n < 3; ++n) {
        const auto& b = bindings[n];
        auto& c = cards[n];
        if (!b.bound()) continue;
        c.shade = ui::knob_shade(s.ui.track, n);
        c.track = colour_of(s);
        c.fill = b.fraction();
        const auto& pot = s.ui.pots[n];
        if (pot.position >= 0 && !pot.engaged && pot.bound == b.key(s.ui.track)) {
            c.state = ui::KnobCard::State::Pickup;
            c.pot = pot.position / 127.0f;
        } else if (s.ui.turning == int(n) && s.now - s.ui.turned_at < 1500) {
            c.state = ui::KnobCard::State::Turning;
        } else {
            c.state = ui::KnobCard::State::Rest;
        }
        char text[32];
        switch (b.kind) {
        case Kind::Param: {
            const auto& p = engine_info(track.engine).params[b.id];
            auto shown = show(p, b.value);
            c.label = p.label; c.value = shown.value; c.unit = shown.unit; c.bipolar = bipolar(p);
            c.fill = gauge_of(p, b.value);
            break;
        }
        case Kind::Level: c.label = "LEVEL"; c.value = juce::String(b.value); break;
        case Kind::Pan:
            c.label = "PAN"; c.bipolar = true;
            c.value = b.value == 0 ? juce::String("C") : (b.value < 0 ? "L" : "R") + juce::String(std::abs(b.value));
            break;
        case Kind::Send: c.label = "SEND A"; c.value = juce::String(b.value); c.unit = "%"; break;
        case Kind::Root: c.label = "ROOT"; c.value = note_letter(unsigned(b.value)); break;
        case Kind::Scale: c.label = "SCALE"; c.value = scales()[size_t(b.value)].name; break;
        case Kind::Layout: c.label = "LAYOUT"; c.value = layout_name(PadLayout(b.value)); break;
        case Kind::Engine: c.label = "ENGINE"; c.value = engine_info(track.engine).name; break;
        case Kind::Jump:
            std::snprintf(text, sizeof(text), "%02d", b.value + 1);
            c.label = "JUMP"; c.value = text; c.unit = "/ " + juce::String(b.max + 1);
            break;
        case Kind::Audition: c.label = "AUDITION"; c.value = juce::String(b.value); c.unit = "vel"; break;
        case Kind::StepLength: c.label = "LENGTH"; c.value = juce::String(b.value); c.unit = b.value == 1 ? "step" : "steps"; break;
        case Kind::StepVelocity: c.label = "VELOCITY"; c.value = juce::String(b.value); break;
        case Kind::StepChance: c.label = "CHANCE"; c.value = juce::String(b.value); c.unit = "%"; break;
        case Kind::ModSource: c.label = "SOURCE"; c.value = mod_source_name(ModSource(b.value)); break;
        case Kind::ModDest: c.label = "DEST"; c.value = mod_dest_name(ModDest(b.value)); break;
        case Kind::ModAmount:
            c.label = "AMOUNT"; c.bipolar = true;
            c.value = (b.value > 0 ? "+" : b.value < 0 ? juce::String::fromUTF8("\xe2\x88\x92") : juce::String()) + juce::String(std::abs(b.value));
            break;
        case Kind::None: break;
        }
    }
    return cards;
}

void frame(ui::Paint& p, const Snapshot& s, const ui::SoftKeys& keys, const ui::Header& header) {
    ui::soft_keys(p, keys);
    ui::header(p, header);
    ui::knob_strip(p, cards_of(s));
}

void play_pad(Context& c, unsigned pad, uint8_t velocity, bool down) { c.play_pad(pad, velocity, down); }
void octave(Context& c, int way) { auto& t = c.track(); t.octave = int8_t(std::clamp(t.octave + way, 0, 7)); }
void no_wheel(Context&, int) {}
void no_push(Context&) {}
void play_soft(Context& c, int key) {
    static const PageId pages[] {PageId::Stage, PageId::Keys, PageId::Mix};
    c.open(pages[key % 3]);
}

} // namespace melodee
