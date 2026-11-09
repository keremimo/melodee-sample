// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/pages/common.h"
#include "melodee/engine/arp.h"
#include "melodee/engine/effect_info.h"
#include "melodee/model/chords.h"
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

ui::SoftKeys play_keys(const Snapshot& s, int active) {
    return {{"STAGE", s.track().engine == EngineKind::Sampler ? "PADS" : "KEYS", "MIX"}, active, colour_of(s)};
}

std::array<ui::KnobCard, 3> cards_of(const Snapshot& s) {
    using Kind = Binding::Kind;
    auto bindings = page_bindings(s.project, s.ui, s.pattern);
    const auto& track = s.track();
    std::array<ui::KnobCard, 3> cards;
    for (unsigned n = 0; n < 3; ++n) {
        const auto& b = bindings[n];
        auto& c = cards[n];
        if (!b.bound()) continue;
        // A bus or the master belongs to no track: its cards are neutral.
        bool neutral = s.ui.page == PageId::Bus || s.ui.page == PageId::Master;
        c.shade = neutral ? ui::colour::text : ui::knob_shade(s.ui.track, n);
        c.track = neutral ? ui::colour::text : colour_of(s);
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
        case Kind::Send:
            c.label = juce::String("SEND ") + char('A' + track.mix.bus % bus_count); c.value = juce::String(b.value); c.unit = "%";
            break;
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
        case Kind::PatternBars: // turned by the bar, shown by the step
            c.label = "LENGTH"; c.value = juce::String(s.pattern.length); c.unit = s.pattern.length == 1 ? "step" : "steps";
            break;
        case Kind::Division: c.label = "DIVISION"; c.value = division_name(Division(b.value)); break;
        case Kind::Swing: c.label = "SWING"; c.value = juce::String(b.value); c.unit = "%"; break;
        case Kind::SongZoom: c.label = "ZOOM"; c.value = juce::String(song_zooms[b.value]); c.unit = "bars"; break;
        case Kind::SongPattern: c.label = "PATTERN"; c.value = "P" + juce::String(b.value + 1); break;
        case Kind::SongLength: c.label = "LENGTH"; c.value = juce::String(b.value); c.unit = b.value == 1 ? "bar" : "bars"; break;
        case Kind::FxParam: {
            const auto& slot = s.project.chain(unsigned(b.id) / 64).slots[(unsigned(b.id) / 8) % 8];
            const auto& pi = effect_info(EffectKind(slot.kind)).params[unsigned(b.id) % 8];
            auto shown = show(pi, b.value);
            c.label = pi.label; c.value = shown.value; c.unit = shown.unit; c.bipolar = bipolar(pi); c.fill = gauge_of(pi, b.value);
            break;
        }
        case Kind::SendBus: c.label = "BUS"; c.value = juce::String::charToString(juce::juce_wchar('A' + b.value)); break;
        case Kind::BusReturn: c.label = "RETURN"; c.value = juce::String(b.value); c.unit = "%"; break;
        case Kind::PadPitch: c.label = "PITCH"; c.value = juce::String(b.value); c.unit = "st"; c.bipolar = true; break;
        case Kind::PadLevel: c.label = "LEVEL"; c.value = juce::String(b.value); break;
        case Kind::PadPan:
            c.label = "PAN"; c.bipolar = true;
            c.value = b.value == 0 ? juce::String("C") : (b.value < 0 ? "L" : "R") + juce::String(std::abs(b.value));
            break;
        case Kind::PadStart: case Kind::PadEnd: case Kind::ChopStart:
            c.label = b.kind == Kind::PadEnd ? "END" : "START"; c.value = juce::String(b.value / 1000.0, 3); c.unit = "s";
            break;
        case Kind::TrimZoom: c.label = "ZOOM"; c.value = juce::String::fromUTF8("\xc3\x97") + juce::String(1 << b.value); break;
        case Kind::ChopMode: c.label = "MODE"; c.value = chop_mode_name(ChopMode(b.value)); break;
        case Kind::ChopAmount: {
            auto mode = ChopMode(b.id);
            c.label = mode == ChopMode::Hits ? "HITS" : mode == ChopMode::Even ? "SLICES" : "BEATS";
            c.value = juce::String(b.value); c.unit = mode == ChopMode::Hits ? "%" : "";
            break;
        }
        case Kind::FileJump:
            c.label = "JUMP"; c.value = juce::String(b.value + 1).paddedLeft('0', 2); c.unit = "/ " + juce::String(s.ui.files_count);
            break;
        case Kind::ProjectJump:
            c.label = "JUMP"; c.value = juce::String(b.value + 1); c.unit = "/ " + juce::String(b.max + 1);
            break;
        case Kind::ProjectSort: c.label = "SORT"; c.value = b.value ? "Name" : "Newest"; break;
        case Kind::PadFine: c.label = "FINE"; c.value = juce::String(b.value); c.unit = "ct"; c.bipolar = true; break;
        case Kind::PadPlayMode: c.label = "PLAY"; c.value = b.value ? "One shot" : "Gate"; break;
        case Kind::PadGroup: c.label = "GROUP"; c.value = b.value ? juce::String(b.value) : juce::String("None"); break;
        case Kind::PadReverse: c.label = "REVERSE"; c.value = b.value ? "On" : "Off"; break;
        case Kind::PadAttack: case Kind::PadDecay: case Kind::PadRelease: {
            c.label = b.kind == Kind::PadAttack ? "ATTACK" : b.kind == Kind::PadDecay ? "DECAY" : "RELEASE";
            if (b.kind == Kind::PadDecay && b.value >= 100) { c.value = "Hold"; break; }
            float seconds = time_seconds(b.value) * (b.kind == Kind::PadAttack ? 0.3f : b.kind == Kind::PadRelease ? 0.5f : 1.0f);
            if (seconds < 1) { c.value = juce::String(int(seconds * 1000)); c.unit = "ms"; } else { c.value = juce::String(seconds, 1); c.unit = "s"; }
            break;
        }
        case Kind::PadSustain: c.label = "SUSTAIN"; c.value = juce::String(b.value); c.unit = "%"; break;
        case Kind::Tempo: c.label = "TEMPO"; c.value = juce::String(s.project.tempo / 10); c.unit = "bpm"; break;
        case Kind::ClickLevel: c.label = "CLICK"; c.value = juce::String(b.value); c.unit = "%"; break;
        case Kind::ArpMode: c.label = "MODE"; c.value = arp_mode_name(ArpMode(b.value)); break;
        case Kind::ArpRate: c.label = "RATE"; c.value = arp_rate_name(unsigned(b.value)); break;
        case Kind::ArpOctaves: c.label = "OCTAVES"; c.value = juce::String(b.value); break;
        case Kind::ChordType: c.label = "TYPE"; c.value = chord_type_name(unsigned(b.value)); break;
        case Kind::ChordVoicing: c.label = "VOICING"; c.value = chord_voicing_name(unsigned(b.value)); break;
        case Kind::ChordInvert: c.label = "INVERT"; c.value = chord_invert_name(unsigned(b.value)); break;
        case Kind::CaptureBars: c.label = "BARS"; c.value = juce::String(b.value); c.unit = "of 4"; break;
        case Kind::CaptureQuantise: c.label = "QUANTISE"; c.value = b.value == 0 ? "Off" : b.value == 1 ? "1/16" : "1/8"; break;
        case Kind::CaptureInto: c.label = "INTO"; c.value = "P" + juce::String(b.value + 1); break;
        case Kind::RecThreshold:
            c.label = "START AT";
            if (b.value <= -60) c.value = "Now";
            else { c.value = (b.value < 0 ? juce::String::fromUTF8("\xe2\x88\x92") : juce::String()) + juce::String(std::abs(b.value)); c.unit = "dB"; }
            break;
        case Kind::RecBars:
            c.label = "LENGTH";
            if (b.value) { c.value = juce::String(b.value); c.unit = b.value == 1 ? "bar" : "bars"; } else c.value = "Until stop";
            break;
        case Kind::RecGain:
            c.label = "GAIN"; c.unit = "dB";
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
    // On a sampler track B2 is PADS.
    PageId keys = c.track().engine == EngineKind::Sampler ? PageId::Pads : PageId::Keys;
    const PageId pages[] {PageId::Stage, keys, PageId::Mix};
    c.open(pages[key % 3]);
}

} // namespace melodee
