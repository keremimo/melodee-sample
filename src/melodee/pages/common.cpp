// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/lesson.h"
#include "melodee/shell/levels_keys.h"
#include "melodee/shell/mix_hub.h"
#include "melodee/shell/mixer_channels.h"
#include "melodee/shell/features.h"
#include "melodee/shell/pad_bank.h"
#include "melodee/shell/pad_job.h"
#include "melodee/pages/common.h"
#include "melodee/shell/free_length.h"
#include "melodee/shell/step_sounds.h"
#include "melodee/shell/undo_scope.h"
#include "melodee/shell/midi_ports.h"
#include "melodee/shell/midi_config_rows.h"
#include "melodee/engine/punch.h"
#include "melodee/engine/arp.h"
#include "melodee/engine/effect_info.h"
#include "melodee/engine/engine_info.h"
#include "melodee/engine/macros.h"
#include "melodee/engine/take_recorder.h"
#include "melodee/engine/pad_fx_info.h"
#include "melodee/engine/sampler.h"
#include "melodee/model/chords.h"
#include "melodee/model/mod_matrix.h"
#include "melodee/model/scales.h"
#include "melodee/shell/bindings.h"
#include "melodee/shell/fader_job.h"
#include "melodee/shell/input_choices.h"
#include "melodee/shell/looper_jobs.h"
#include "melodee/shell/name_suggest.h"
#include "melodee/shell/pad_colour.h"
#include "melodee/engine/midi_engine.h"
#include "melodee/shell/stage_map.h"
#include "melodee/shell/sample_mode.h"
#include "melodee/shell/input_route.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace melodee {

juce::Colour colour_of(const Snapshot& s) { return ui::track_colour(s.ui.track); }
juce::Colour channel_shade(unsigned strip, unsigned knob) {
    switch (channel_kind(strip)) {
    case ChannelKind::Track: return ui::knob_shade(strip, knob);
    case ChannelKind::Submix: return ui::sub_shade(channel_sub(strip), knob);
    case ChannelKind::Bus: return ui::loop_shade(channel_bus(strip), knob);
    default: return channel_kind(strip) == ChannelKind::Input ? ui::colour::ice : ui::colour::text; // neutral, as the Bus and Master pages
    }
}
juce::Colour load_colour(float load) { return load >= 0.85f ? ui::colour::rec : load >= 0.6f ? ui::colour::amber : ui::colour::play; }

ui::Header header_of(const Snapshot& s) {
    ui::Header h;
    h.playing = s.playing;
    h.recording = s.ui.recording;
    h.armed = s.ui.rec_armed && !s.ui.recording; // Seq Record armed: Play starts the take
    char bpm[16];
    int tempo = int(s.tempo);
    if (tempo % 10) std::snprintf(bpm, sizeof(bpm), "%d.%d", tempo / 10, tempo % 10);
    else std::snprintf(bpm, sizeof(bpm), "%d", tempo / 10);
    h.bpm = bpm;
    if (s.ui.lesson) { // a lesson's step has the header's first slot (shell/lesson.h): which step, and what to do in two or three words
        const Lesson& lesson = lesson_at(s.ui.lesson - 1u);
        const LessonStep* step = lesson_step(s.ui);
        h.left.push_back(ui::pill(juce::String(std::min<unsigned>(s.ui.lesson_step + 1u, lesson.count)) + "/" + juce::String(lesson.count)));
        // A control with no light and no place on the screen gets its glyph: Shift, the encoder turned or pushed.
        ui::Icon glyph = !step ? ui::Icon::Check : step->with == int8_t(SurfaceButton::Shift) ? ui::Icon::Shift
                       : step->kind == LessonKind::Wheel ? ui::Icon::Wheel : step->kind == LessonKind::Push ? ui::Icon::Push : ui::Icon::None;
        h.left.push_back(ui::chip(glyph, step ? step->words : "DONE", ui::colour::text));
        h.lead = 2;
        return h;
    }
    // Count-In: the beat counted before the take, an amber metronome chip.
    if (s.count_in) h.left.push_back(ui::chip(ui::Icon::Metro, juce::String(s.count_in), ui::colour::amber));
    if (s.following) h.left.push_back(ui::chip(s.following_mtc ? "EXT MTC" : "EXT CLOCK", ui::colour::text)); // MIDI clock / MTC in leads (phase 9, 9b)
    // A render as it plays (Resample, the song's mixdown), or Resample waiting for its pad.
    if (s.ui.rendering) {
        unsigned seconds = s.render_frames / 44100;
        char clock[32];
        std::snprintf(clock, sizeof(clock), "%s %u:%02u", s.ui.rendering == 2 ? "MIXDOWN" : "RESAMPLE", seconds / 60, seconds % 60);
        h.left.push_back(ui::chip(clock, ui::colour::rec));
    } else if (s.ui.resample_pick) h.left.push_back(ui::chip(s.ui.resample_song ? "SONG: TAP A PAD" : "RESAMPLE: TAP A PAD", ui::colour::rec));
    if (sample_held_named(s.ui, s.now)) { // Sample held on Steps or Roll: the sound the pads give, the editor in view
        const auto& t = s.track();
        juce::String what;
        if (t.engine == EngineKind::Sampler) {
            char label[4];
            pad_label(t.last_pad, label);
            auto name = pad_name(s, t.last_pad % sampler_pads, true);
            what = juce::String(label) + (name.isNotEmpty() ? juce::String::fromUTF8(" \xc2\xb7 ") + name : juce::String());
        } else {
            for (unsigned n = 0; n < 16; ++n)
                if (s.held_keys[n] >= 0 && (s.ui.held_pads & (1u << n))) what += (what.isEmpty() ? "" : " ") + juce::String(note_name(s.held_keys[n]));
        }
        h.left.insert(h.left.begin(), ui::chip(what.isEmpty() ? juce::String("HELD") : "HELD " + what, colour_of(s)));
    }
    // The pads chip (shell/pad_job.h): what the pads do when they do not play the track, first among a page's chips.
    if (const char* job = pad_job(s.project, s.ui); *job) h.left.push_back(ui::chip(ui::Icon::Pads, job, ui::colour::text));
    h.lead = unsigned(h.left.size());
    return h;
}

ui::HeaderItem bank_pill(const Snapshot& s) {
    return ui::pill(ui::Icon::Pattern, juce::String("BANK ") + juce::String::charToString(juce::juce_wchar('A' + s.ui.pattern_bank % pattern_banks)),
                    ui::colour::text);
}

// The pattern the editors and the pattern cards edit: in the track's colour, amber while another one plays.
ui::HeaderItem edit_pattern_chip(const Snapshot& s) {
    unsigned edited = s.track().pattern % pattern_count;
    bool apart = s.playing && s.playing_patterns[s.ui.track % track_count] % pattern_count != edited;
    return ui::chip(ui::Icon::Pattern, "P" + juce::String(edited + 1), apart ? ui::colour::amber : colour_of(s));
}

ui::HeaderItem tag_of(const Snapshot& s, unsigned track) {
    const auto& t = s.project.tracks[track % track_count];
    const auto& info = engine_info(t.engine);
    return ui::track_tag(int(track % track_count) + 1, t.used() ? info.name : "", t.used() ? name_case(t.name) : "Empty",
                         ui::track_colour(track));
}

void region_wave(ui::Paint& p, const Snapshot& s, const AudioRegion& r, float x, float y, float w, float h, juce::Colour c) {
    if (!s.audio_peaks || w < 2 || !r.frames) return;
    auto found = s.audio_peaks->find(r.take);
    if (found == s.audio_peaks->end() || found->second.empty()) return;
    const auto& peaks = found->second;
    const unsigned columns = std::max(1u, unsigned(w / 1.5f));
    const float gain = std::pow(10.0f, r.gain / 200.0f), mid = y + h / 2, cw = w / columns;
    for (unsigned n = 0; n < columns; ++n) {
        uint32_t a = uint32_t(uint64_t(r.frames) * n / columns), b = uint32_t(uint64_t(r.frames) * (n + 1) / columns);
        if (r.reverse) { uint32_t ra = r.frames - b, rb = r.frames - a; a = ra; b = rb; }
        float v = peak_over(peaks, r.offset + a, r.offset + b) * gain;
        const float at = float(uint64_t(r.frames) * n / columns);
        if (r.fade_in && at < r.fade_in) v *= at / r.fade_in;
        if (r.fade_out && r.frames - at < r.fade_out) v *= (r.frames - at) / r.fade_out;
        float half = std::max(0.5f, std::min(1.0f, v) * (h / 2 - 1));
        p.fill(x + n * cw, mid - half, std::max(1.0f, cw * 0.7f), half * 2, c);
    }
}

ui::Icon engine_icon(EngineKind kind) {
    switch (kind) {
    case EngineKind::Sampler: return ui::Icon::Pads;
    case EngineKind::Midi: return ui::Icon::Din;
    case EngineKind::Plugin: return ui::Icon::Plug;
    case EngineKind::Keygroup: return ui::Icon::Keyboard;
    case EngineKind::Audio: return ui::Icon::Wav;
    default: return ui::Icon::None;
    }
}


std::array<ui::KnobCard, 3> cards_of(const Snapshot& s) {
    auto bindings = page_bindings(s.project, s.ui, s.pattern);
    auto second = shift_bindings(s.project, s.ui, s.pattern);
    std::array<ui::KnobCard, 3> cards;
    Binding followed; // the Fader page (fader C): the card the fader follows, ringed
    if (s.ui.page == PageId::FaderPage)
        if (auto job = fader_job(s.project, s.ui); job.source == FaderJob::Source::Target) followed = job.binding;
    int moved = fader_card(s); // the fader just moved this card's value: it says so, not a card of the fader's own
    for (unsigned n = 0; n < 3; ++n) {
        const auto& b = bindings[n];
        auto& c = cards[n];
        if (!b.bound()) continue;
        if (second[n].bound()) c.shift = s.ui.shift ? 2 : 1;
        if (s.ui.page == PageId::Stage) c.focus = int(n) == stage_focus(s.ui, s.track()); // the wheel's card
        if (s.ui.page == PageId::Samples && s.ui.browse.up()) c.focus = int(n) == 1 + s.ui.browse.focus; // the trim sheet: the wheel's end
        if (s.ui.page == PageId::FaderPage) c.focus = followed.bound() && followed.kind == b.kind && followed.id == b.id;
        // A bus or the master belongs to no track: its cards are neutral.
        bool neutral = s.ui.page == PageId::Bus || s.ui.page == PageId::Master;
        bool loop = s.ui.page == PageId::Looper || (s.ui.page == PageId::Rack && s.ui.fx_loop >= 0); // L1-L4: their own colours
        unsigned l = s.ui.page == PageId::Looper ? s.ui.loop : unsigned(std::max<int>(s.ui.fx_loop, 0));
        c.knob_fx = b.kind == Binding::Kind::KnobParam; // Knob FX owns the pot: white, never the track's colours
        c.shade = neutral || c.knob_fx ? ui::colour::text : loop ? ui::loop_shade(l, n) : ui::knob_shade(s.ui.track, n);
        c.track = neutral || c.knob_fx ? ui::colour::text : loop ? ui::loop_colour(l) : colour_of(s);
        if (s.ui.page == PageId::Rack && s.ui.fx_sub >= 0) { // a submix's chain: its colours
            c.shade = channel_shade(strip_sub(unsigned(s.ui.fx_sub)), n);
            c.track = channel_shade(strip_sub(unsigned(s.ui.fx_sub)), 0);
        }
        if (s.ui.page == PageId::Mixer && s.ui.mixer_bank == 1) { // the mixer's second row: the channel in hand's colours
            c.shade = channel_shade(mixer_strip(s.ui), n);
            c.track = channel_shade(mixer_strip(s.ui), 0);
        }
        c.fill = b.fraction();
        c.frozen = print_covers(s.freeze, freeze_scope(s.project, s.ui, b)); // freeze plan F2c
        const auto& pot = s.ui.pots[n];
        if (pot.position >= 0 && !pot.engaged && pot.bound == b.key(s.ui.track)) {
            c.state = ui::KnobCard::State::Pickup;
            c.pot = pot.position / 127.0f;
        } else if (s.ui.turning == int(n) && s.now - s.ui.turned_at < 1500) {
            c.state = ui::KnobCard::State::Turning;
        } else {
            c.state = ui::KnobCard::State::Rest;
        }
        if (int(n) == moved) { // as a pot turning, or hollow with the fader's place until it reaches the value
            const auto& f = s.ui.fader;
            c.fader = true;
            c.state = f.position >= 0 && !f.engaged ? ui::KnobCard::State::Pickup : ui::KnobCard::State::Turning;
            c.pot = c.state == ui::KnobCard::State::Pickup ? f.position / 127.0f : -1;
        }
        describe_card(s, b, c);
    }
    return cards;
}

bool fader_moved(const Snapshot& s) {
    return s.ui.fader_moved_at && s.now - s.ui.fader_moved_at <= 1500 && s.ui.page != PageId::FaderPage;
}

int fader_card(const Snapshot& s) {
    if (!fader_moved(s) || s.ui.shift) return -1; // Shift shows the cards' second jobs: the header says it
    return fader_twin(fader_job(s.project, s.ui), page_bindings(s.project, s.ui, s.pattern));
}

// A card's label, value and unit for a Binding (the knob strip, the fader's card).
void describe_card(const Snapshot& s, const Binding& b, ui::KnobCard& c) {
    using Kind = Binding::Kind;
    const auto& track = s.track();
    char text[32];
    switch (b.kind) {
    case Kind::Param: {
        const auto& p = track_param_info(track.engine, track.params, unsigned(b.id));
        auto shown = show(p, b.value);
        c.label = p.label; c.value = shown.value; c.unit = shown.unit; c.bipolar = bipolar(p);
        c.fill = gauge_of(p, b.value);
        if (track.engine == EngineKind::Midi && midi_track::is_cc(b.id)) // a slot by what it sends: "CC 74", "BEND" (roadmap P4)
            c.label = midi_track::slot_label(track.params[size_t(midi_track::slot_address(b.id - midi_track::Cc1))]);
        if (track.engine == EngineKind::Midi && midi_track::slot_of_address(b.id) >= 0) c.label = "SLOT " + juce::String(midi_track::slot_of_address(b.id) + 1);
        if (track.engine == EngineKind::Midi && b.id == midi_track::AudioIn && ((s.ui.midi_in_use >> (s.ui.track % track_count)) & 1))
            c.unit = "IN USE"; // the ADC is routed to the other input: silent until it is free (roadmap P4, M5)
        if (track.engine == EngineKind::Sampler && b.id == sampler::Level) c.label = "KIT VOLUME";
        break;
    }
    case Kind::Level: c.label = "LEVEL"; c.value = juce::String(b.value); break;
    case Kind::Pan:
        c.label = "PAN"; c.bipolar = true;
        c.value = b.value == 0 ? juce::String("C") : (b.value < 0 ? "L" : "R") + juce::String(std::abs(b.value));
        break;
    case Kind::Send: // on a kit's Stage SENDS (Shift + K1) the whole kit's, beside its pads' own
        c.label = juce::String(track.engine == EngineKind::Sampler && s.ui.page == PageId::Stage ? "KIT SEND " : "SEND ") + char('A' + unsigned(b.id) % bus_count);
        c.value = juce::String(b.value); c.unit = "%";
        break;
    case Kind::PadCloud: { // the pad's cloud, shown as the kit's CLOUD values were
        const auto& p = track_param_info(EngineKind::Sampler, track.params, unsigned(sampler::Cloud + std::clamp(b.id, 0, 5)));
        auto shown = show(p, b.value);
        c.label = p.label; c.value = shown.value; c.unit = shown.unit; c.fill = gauge_of(p, b.value);
        break;
    }
    case Kind::PadSend: c.label = juce::String("SEND ") + char('A' + unsigned(b.id) % bus_count); c.value = juce::String(b.value); c.unit = "%"; break;
    case Kind::PadOut: c.label = "OUTPUT"; c.value = b.value ? "SUB " + juce::String(b.value) : juce::String("KIT"); break;
    case Kind::Root: c.label = "ROOT"; c.value = note_letter(unsigned(b.value)); break;
    case Kind::Scale: c.label = "SCALE"; c.value = scales()[size_t(b.value)].name; break;
    case Kind::Layout: c.label = "LAYOUT"; c.value = layout_name(PadLayout(b.value)); break;
    case Kind::ProgKey: c.label = "KEY"; c.value = note_letter(unsigned(b.value)); break;
    case Kind::ProgVoicing: c.label = "VOICING"; c.value = prog_voicing_name(unsigned(b.value)); break;
    case Kind::BuildRoot: c.label = "ROOT"; c.value = note_letter(unsigned(b.value)); break;
    case Kind::BuildChord: c.label = "CHORD"; c.value = juce::String::fromUTF8(short_chord_name(built_chords()[unsigned(b.value) % built_chord_count].name).c_str()); if (c.value.isEmpty()) c.value = "maj"; break;
    case Kind::BuildInvert: c.label = "INVERSION"; c.value = chord_invert_name(unsigned(b.value)); break;
    case Kind::Transpose:
        c.label = "TRANSPOSE"; c.bipolar = true; c.unit = "st";
        c.value = (b.value > 0 ? "+" : b.value < 0 ? juce::String::fromUTF8("\xe2\x88\x92") : juce::String()) + juce::String(std::abs(b.value));
        break;
    case Kind::Engine: c.label = "ENGINE"; c.value = engine_info(track.engine).name; break;
    case Kind::Jump:
        std::snprintf(text, sizeof(text), "%02d", b.value + 1);
        c.label = "JUMP"; c.value = text; c.unit = "/ " + juce::String(b.max + 1);
        break;
    case Kind::Audition: c.label = "AUDITION"; c.value = juce::String(b.value); c.unit = "vel"; break;
    case Kind::StepLength: c.label = "LENGTH"; c.value = juce::String(b.value); c.unit = b.value == 1 ? "step" : "steps"; break;
    case Kind::StepVelocity: c.label = "VELOCITY"; c.value = juce::String(b.value); break;
    case Kind::EventVelocity: c.label = "VELOCITY"; c.value = juce::String(b.value); break;
    case Kind::MarksVelocity: { // Roll's K2 over marked notes: lowest to highest, moving together
        int high = b.value + 127 - b.max;
        c.label = "VELOCITY"; c.value = juce::String(b.value) + (high > b.value ? "-" + juce::String(high) : juce::String());
        break;
    }
    case Kind::EventLength: c.label = "LENGTH"; c.value = juce::String(b.value); c.unit = b.value == 1 ? "step" : "steps"; break;
    case Kind::StepChance: case Kind::EventChance: c.label = "CHANCE"; c.value = juce::String(b.value); c.unit = "%"; break;
    case Kind::EntryLength: c.label = "LENGTH"; c.value = juce::String(b.value); c.unit = b.value == 1 ? "step" : "steps"; break;
    case Kind::EntryVelocity: c.label = "VELOCITY"; c.value = b.value ? juce::String(b.value) : juce::String("Played"); break;
    case Kind::EntryChance: c.label = "CHANCE"; c.value = juce::String(b.value); c.unit = "%"; break;
    case Kind::MarksRamp: // Roll's Shift + K2: the marked notes' velocity, the last this far off the first
        c.label = "VEL RAMP"; c.bipolar = true;
        c.value = (b.value > 0 ? "+" : b.value < 0 ? juce::String::fromUTF8("\xe2\x88\x92") : juce::String()) + juce::String(std::abs(b.value));
        c.unit = "%";
        break;
    case Kind::EventNudge: // Roll's Shift + K1: early or late of its grid line, in ticks
        c.label = "NUDGE"; c.bipolar = true;
        c.value = (b.value > 0 ? "+" : b.value < 0 ? juce::String::fromUTF8("\xe2\x88\x92") : juce::String()) + juce::String(std::abs(b.value));
        c.unit = std::abs(b.value) == 1 ? "tick" : "ticks";
        break;
    case Kind::FeelStrength: c.label = "STRENGTH"; c.value = juce::String(b.value); c.unit = "%"; break; // a pattern's feel (roadmap P1)
    case Kind::FeelSwing: c.label = "SWING"; c.value = juce::String(b.value); c.unit = "%"; break;
    case Kind::FeelHumanize: c.label = "HUMANIZE"; c.value = juce::String(b.value); c.unit = "%"; break;
    case Kind::FeelVelocity: c.label = "HUMAN VEL"; c.value = juce::String(b.value); c.unit = "%"; break;
    case Kind::GrooveFrom: // a groove taken from track T's pattern P
        c.label = "GROOVE";
        c.value = b.value <= 0 ? juce::String("Off")
                               : "T" + juce::String((b.value - 1) / int(pattern_count) + 1) + " P" + juce::String((b.value - 1) % int(pattern_count) + 1);
        break;
    case Kind::GrooveAmount: c.label = "AMOUNT"; c.value = juce::String(b.value); c.unit = "%"; break;
    case Kind::EuclidHits: c.label = "HITS"; c.value = juce::String(b.value); c.unit = "of " + juce::String(b.max); break; // generators (roadmap P1)
    case Kind::EuclidSteps: c.label = "STEPS"; c.value = juce::String(b.value); break;
    case Kind::EuclidRotate: c.label = "ROTATE"; c.value = juce::String(b.value); break;
    case Kind::VaryAmount: c.label = "AMOUNT"; c.value = juce::String(b.value); c.unit = "%"; break;
    case Kind::TrackSpeed: c.label = "SPEED"; c.value = juce::String::fromUTF8(speed_name(unsigned(b.value))); break; // roadmap P1
    case Kind::FollowAction: case Kind::FollowOther: { // roadmap P5: a follow action, the first or the second
        static const char* const names[] {"Off", "Next", "Previous", "Any", "Again", "Stop"};
        c.label = b.kind == Kind::FollowAction ? "FOLLOW" : "OR";
        c.value = names[unsigned(b.value) % unsigned(FollowAction::Count)];
        break;
    }
    case Kind::AutoLow: c.label = "LOW"; c.value = juce::String(note_name(b.value)); break; // the AUTO SAMPLE sheet
    case Kind::AutoHigh: c.label = "HIGH"; c.value = juce::String(note_name(b.value)); break;
    case Kind::AutoStep: c.label = "STEP"; c.value = juce::String(b.value); c.unit = b.value == 1 ? "semitone" : "semitones"; break;
    case Kind::AutoLayers: { // Auto: as many as the check heard the tone needs, shown beside it once heard
        const auto& a = s.ui.auto_sheet.settings;
        c.label = "LAYERS";
        c.value = b.value ? juce::String(b.value) : juce::String("Auto");
        if (!b.value && a.found.heard) c.unit = juce::String(a.layer_count());
        break;
    }
    case Kind::AutoHold: // Auto: as long as the check found the sound needs, shown beside it once heard
        c.label = "HOLD";
        if (b.value > 0) { c.value = juce::String(b.value / 10.0, 1); c.unit = "s"; }
        else {
            const auto& a = s.ui.auto_sheet.settings;
            c.value = "Auto";
            auto seconds = [](float v) { return juce::String(v, v == std::floor(v) ? 0 : 1); };
            const float under = a.hold_seconds(0), over = a.hold_seconds(a.found.split); // a split: each side's
            c.unit = !a.found.heard || a.found.hold <= 0 ? juce::String()
                   : a.found.split && over != under ? seconds(under) + "/" + seconds(over) + " s" : seconds(under) + " s";
        }
        break;
    case Kind::AutoTail: c.label = "TAIL"; c.value = b.value ? juce::String(b.value / 10.0, 1) : juce::String("Off"); c.unit = b.value ? "s" : ""; break;
    case Kind::AutoTap: c.label = "TAP"; c.value = b.value ? "Post FX" : "Pre FX"; break;
    case Kind::AutoLoop: c.label = "LOOP"; c.value = b.value == 0 ? "Auto" : b.value == 1 ? "On" : "Off"; break; // Auto: as the check heard
    case Kind::AutoLevel: c.label = "LEVEL"; c.value = b.value ? juce::String::fromUTF8("\xe2\x88\x92" "1") : juce::String("Played"); c.unit = b.value ? "dB peak" : ""; break;
    case Kind::PrintTapPick: c.label = "TAP"; c.value = b.value ? "Pre FX" : "Post FX"; break; // freeze plan F2c, the PRINT sheet
    case Kind::PrintTailPick: c.label = "TAIL"; c.value = b.value == 2 ? "Cut" : b.value == 1 ? "Ring" : "Loop"; break;
    case Kind::FollowAfter: c.label = "AFTER"; c.value = juce::String(b.value % 64 + 1); c.unit = b.value >= 64 ? (b.value == 64 ? "play" : "plays") : (b.value == 0 ? "bar" : "bars"); break;
    case Kind::FollowChance: c.label = "CHANCE"; c.value = juce::String(b.value); c.unit = "%"; break;
    case Kind::SceneColour: c.label = "COLOUR"; c.value = b.value ? juce::String(ui::track_colour_names[size_t(b.value - 1) % 16]) : juce::String("None"); break;
    case Kind::SceneTempo: c.label = "SEQ " + juce::String(b.id + 1) + " TEMPO"; c.label_mixed = "Seq " + juce::String(b.id + 1) + " tempo"; c.value = b.value < 30 ? juce::String("None") : juce::String(b.value); c.unit = b.value < 30 ? "" : "BPM"; break;
    case Kind::PatternProgram: { // the pattern in hand's own program, as PROGRAM shows one
        const auto& p = engine_info(EngineKind::Midi).params[midi_track::PatternProgram0];
        auto shown = show(p, b.value);
        c.label = "P" + juce::String(track.pattern % pattern_count + 1) + " PROG"; c.value = shown.value; c.unit = shown.unit;
        c.fill = gauge_of(p, b.value);
        break;
    }
    case Kind::SongTempo: // roadmap P5: the tempo lane at the cursor; with no point there a turn makes one
        c.label = "BAR TEMPO"; c.value = b.value < 30 ? juce::String("Off") : juce::String(b.value); c.unit = b.value < 30 ? "" : s.project.song.tempo.at_bar(s.ui.song_cursor) >= 0 ? "BPM" : "add";
        break;
    case Kind::SongTempoShape: c.label = "SHAPE"; c.value = b.value ? "Ramp" : "Step"; break;
    case Kind::LaneLength: // a kit lane's own length (roadmap P1); as long as the pattern: none of its own
        c.label = "LANE"; c.value = b.value >= int(s.pattern.steps()) ? juce::String("All") : juce::String(b.value); c.unit = b.value >= int(s.pattern.steps()) ? "" : "steps";
        break;
    case Kind::Lock: { // a lane locked on the step or note held (roadmap P1): as the lane's own card
        auto m = motion_param(track, int(b.id), &s.project);
        if (!m.info) break;
        auto shown = show(*m.info, b.value);
        c.label = m.info->label; c.value = shown.value; c.unit = shown.unit; c.bipolar = bipolar(*m.info); c.fill = gauge_of(*m.info, b.value);
        break;
    }
    case Kind::StepSound: { // Step Edit's SOUND row: the step's own sample by the pad and track it is on ("B05" T1), else PAD
        c.label = "SOUND";
        const auto choices = step_sound_choices(s.project);
        if (b.value > 0 && size_t(b.value) <= choices.size()) { // the panel names the sample
            const auto& choice = choices[size_t(b.value - 1)];
            char label[4];
            pad_label(choice.pad, label);
            c.value = label;
            c.unit = "T" + juce::String(choice.track + 1);
        } else if (b.id > 0 && unsigned(b.id) <= sample_slots) { // a sample no pad holds now: the step keeps it
            c.value = juce::String::fromUTF8(s.project.samples[unsigned(b.id) - 1].name);
        } else {
            c.value = "PAD";
        }
        break;
    }
    case Kind::StepSlice: // 0 the pad's region, a slice of the chop (id: how many), id + 1 a slice at random each hit
        c.label = "SLICE";
        if (b.id <= 0 || b.value <= 0) c.value = "PAD";
        else if (b.value > b.id) c.value = "RND";
        else { c.value = juce::String(b.value); c.unit = "/ " + juce::String(b.id); }
        break;
    case Kind::StepPlay: c.label = "PLAY"; c.value = step_play_name(StepPlay(std::clamp(b.value, 0, 3))); break;
    case Kind::StepCondition: case Kind::EventCondition: // trig conditions (roadmap P1)
        c.label = "COND"; c.value = trig_name(unsigned(b.value));
        break;
    case Kind::StepRatchet: case Kind::EventRatchet: // ratchets (roadmap P1)
        c.label = "RATCHET"; c.value = b.value <= 1 ? juce::String("Off") : juce::String::fromUTF8("\xc3\x97") + juce::String(b.value);
        break;
    case Kind::StepRamp: case Kind::EventRamp: case Kind::StepPitchRamp: case Kind::EventPitchRamp: {
        bool pitch = b.kind == Kind::StepPitchRamp || b.kind == Kind::EventPitchRamp;
        c.label = pitch ? "PITCH RAMP" : "RAMP"; c.bipolar = true;
        c.unit = pitch ? "st" : "%";
        c.value = (b.value > 0 ? "+" : b.value < 0 ? juce::String::fromUTF8("\xe2\x88\x92") : juce::String()) + juce::String(std::abs(b.value));
        break;
    }
    case Kind::ModSource: c.label = "SOURCE"; c.value = mod_source_name(ModSource(b.value)); break;
    case Kind::ModDest: {
        const char* name = mod_dest_label(engine_info(track.engine), ModDest(b.value));
        c.label = "DEST"; c.value = name ? name : mod_dest_name(ModDest(b.value));
        break;
    }
    case Kind::ModAmount:
        c.label = "AMOUNT"; c.bipolar = true;
        c.value = (b.value > 0 ? "+" : b.value < 0 ? juce::String::fromUTF8("\xe2\x88\x92") : juce::String()) + juce::String(std::abs(b.value));
        break;
    case Kind::PatternSteps: case Kind::PatternLength: { // by the bar; a length not whole bars in sixteenths (issue 27), else steps of Q
        unsigned bar = std::max(s.project.bar_ticks(), 1u);
        juce::String which = "P" + juce::String(s.track().pattern % pattern_count + 1) + " "; // the pattern it belongs to
        c.label = which + "LENGTH"; c.label_mixed = which + "Length"; // mixed case where capitals don't fit (P128)
        if (s.ui.free_growing) { // Free length: the bars the take has reached, growing (shell/free_length.h)
            unsigned bars = s.position / bar + 1;
            c.label = which + "FREE"; c.label_mixed = which + "Free";
            c.value = juce::String(bars); c.unit = bars == 1 ? "bar" : "bars";
            break;
        }
        uint32_t length = free_shown(s.pattern, s.ui.column_ticks); // a Free pattern: the take's length (0 Free)
        if (!length) { c.value = "Free"; break; }
        if (length % bar && length % sixteenth_ticks == 0) { c.value = juce::String(length / sixteenth_ticks) + "/16"; break; }
        if (length % bar) { c.value = juce::String((length + s.pattern.q() - 1) / s.pattern.q()); c.unit = "steps"; break; }
        unsigned bars = length / bar;
        c.value = juce::String(bars); c.unit = bars == 1 ? "bar" : "bars";
        break;
    }
    case Kind::Division: c.label = "P" + juce::String(s.track().pattern % pattern_count + 1) + " Q"; c.value = division_name(Division(b.value)); break;
    case Kind::TimeSignature: {
        auto sig = time_signatures[unsigned(b.value) % time_signature_count];
        c.label = "TIME SIG"; c.value = juce::String(sig.beats) + "/" + juce::String(sig.unit);
        break;
    }
    case Kind::TcShift:
        c.label = "SHIFT"; c.bipolar = true; c.unit = "ticks";
        c.value = (b.value > 0 ? "+" : b.value < 0 ? juce::String::fromUTF8("\xe2\x88\x92") : juce::String()) + juce::String(std::abs(b.value));
        break;
    case Kind::TcSwing: c.label = "SWING"; c.value = juce::String(b.value); c.unit = "%"; break;
    case Kind::Swing: c.label = "SWING"; c.value = juce::String(b.value); c.unit = "%"; break;
    case Kind::JamFrom: c.label = "FROM"; c.value = juce::String(b.value); c.unit = "bar"; break;
    case Kind::JamTo: c.label = "TO"; c.value = juce::String(b.value); c.unit = "bar"; break;
    case Kind::PadMute: { // issue 32: a kit pad's mute lane on Motion
        char label[4];
        pad_label(unsigned(b.id), label);
        c.label = juce::String(label) + " MUTE"; c.value = b.value ? "On" : "Off";
        break;
    }
    case Kind::Strip: { // the mixer (docs/MIXER_PLAN.md): one value of a strip, as the mock names it
        static const char* const bands[] {"63 Hz", "125 Hz", "250 Hz", "500 Hz", "1 kHz", "2 kHz", "4 kHz", "8 kHz", "16 kHz"};
        static const char* const speeds[] {"FAST", "MED", "SLOW", "AUTO"};
        const auto part = StripPart(b.id & 0xff);
        const juce::String minus = juce::String::fromUTF8("\xe2\x88\x92");
        auto tenths = [&](int v) { return (v > 0 ? juce::String("+") : v < 0 ? minus : juce::String()) + juce::String(std::abs(v) / 10.0, 1); }; // -5.0, +4.0
        const unsigned p = unsigned(part);
        if (p >= unsigned(StripPart::Eq0) && p <= unsigned(StripPart::Eq8)) {
            c.label = bands[p - unsigned(StripPart::Eq0)]; c.value = tenths(b.value); c.unit = "dB"; c.bipolar = true;
            break;
        }
        switch (part) {
        case StripPart::Trim: c.label = "TRIM"; c.value = tenths(b.value); c.unit = "dB"; c.bipolar = true; break;
        case StripPart::Width: c.label = "WIDTH"; c.value = juce::String(b.value); c.unit = "%"; break;
        case StripPart::Amount: c.label = "AMOUNT"; c.value = b.value ? minus + juce::String(b.value) : juce::String("OFF"); c.unit = b.value ? "dB" : ""; break;
        case StripPart::Ratio: c.label = "RATIO"; c.value = (b.value % 10 ? juce::String(b.value / 10.0, 1) : juce::String(b.value / 10)) + ":1"; break;
        case StripPart::Speed: c.label = "SPEED"; c.value = speeds[unsigned(b.value) % 4]; break;
        case StripPart::Key: { // what the compressor hears: its own channel, a track, a bus, a submix
            c.label = "KEY";
            const unsigned k = unsigned(b.value);
            if (!k) c.value = "OWN";
            else if (fx_key_is_track(k)) c.value = "TRACK " + juce::String(k);
            else if (fx_key_is_bus(k)) c.value = juce::String("BUS ") + char('A' + (k - 17));
            else c.value = "SUB " + juce::String(k - fx_keys + 1);
            break;
        }
        case StripPart::Mix: c.label = "MIX"; c.value = juce::String(b.value); c.unit = "%"; break;
        case StripPart::Gain: c.label = "GAIN"; c.value = tenths(b.value); c.unit = "dB"; break;
        default: c.label = "STRIP"; c.value = juce::String(b.value); break;
        }
        break;
    }
    case Kind::SubMix: { // a submix's level, pan or send (engine/command.h SubPart)
        const unsigned part = unsigned(b.id) & 0xff;
        if (part == Command::SubPan) {
            c.label = "PAN"; c.bipolar = true;
            c.value = b.value == 0 ? juce::String("C") : (b.value < 0 ? "L" : "R") + juce::String(std::abs(b.value));
        } else if (part >= Command::SubSend0) {
            c.label = juce::String("SEND ") + char('A' + (part - Command::SubSend0) % bus_count);
            c.value = juce::String(b.value); c.unit = "%";
        } else { c.label = "LEVEL"; c.value = juce::String(b.value); }
        break;
    }
    case Kind::InputMonitor: { // the mixer's input, K2: Input Config's monitor
        static const char* const words[] {"OFF", "AUTO", "ON"};
        c.label = "MONITOR"; c.value = words[unsigned(b.value) % 3];
        break;
    }
    case Kind::InputWet: c.label = "RECORD"; c.value = b.value ? "WET" : "DRY"; break; // takes after the input's strip, or before
    case Kind::TrackOut: { // where the track plays: the master or a submix, by its name when it has one
        c.label = "OUTPUT";
        if (!b.value) { c.value = "MASTER"; break; }
        const auto& sub = s.project.subs[unsigned(b.value - 1) % sub_count];
        c.value = sub.name[0] ? name_case(sub.name) : "SUB " + juce::String(b.value);
        break;
    }
    case Kind::SongZoom: c.label = "ZOOM"; c.value = juce::String(song_zooms[b.value]); c.unit = "bars"; break;
    case Kind::SongPattern: c.label = "PATTERN"; c.value = "P" + juce::String(b.value + 1); break;
    case Kind::SongLength: c.label = "LENGTH"; c.value = juce::String(b.value); c.unit = b.value == 1 ? "bar" : "bars"; break;
    case Kind::RegionGain: { // freeze plan F3: Stage on an audio track's pattern's take
        c.label = "LEVEL";
        juce::String sign = b.value < 0 ? juce::String::fromUTF8("\xe2\x88\x92") : b.value > 0 ? juce::String("+") : juce::String();
        c.value = sign + juce::String(std::abs(b.value) / 10.0, 1); c.unit = "dB"; c.bipolar = true;
        break;
    }
    case Kind::RegionStart: case Kind::RegionEnd:
        c.label = b.kind == Kind::RegionStart ? "START" : "END";
        c.value = juce::String(b.value / 1000.0, b.value < 10000 ? 3 : 2); c.unit = "s";
        break;
    case Kind::RegionFadeIn: case Kind::RegionFadeOut:
        c.label = b.kind == Kind::RegionFadeIn ? "FADE IN" : "FADE OUT";
        if (b.value < 1000) { c.value = juce::String(b.value); c.unit = "ms"; } else { c.value = juce::String(b.value / 1000.0, 2); c.unit = "s"; }
        break;
    case Kind::RegionReverse: c.label = "REVERSE"; c.value = b.value ? "On" : "Off"; break;
    case Kind::RegionWarp: { static const char* const how[] {"Off", "Stretch", "Repitch"}; c.label = "WARP"; c.value = how[std::clamp(b.value, 0, 2)]; break; }
    case Kind::RegionBpm: c.label = "BPM"; c.value = juce::String(b.value / 10) + "." + juce::String(b.value % 10); break;
    case Kind::RegionSemi: c.label = "SEMI"; c.value = (b.value > 0 ? "+" : "") + juce::String(b.value); c.bipolar = true; break;
    case Kind::RegionFine: c.label = "FINE"; c.value = (b.value > 0 ? "+" : "") + juce::String(b.value); c.unit = "ct"; c.bipolar = true; break;
    case Kind::FxParam: {
        const auto& slot = s.project.chain(unsigned(b.id) / 64).slots[(unsigned(b.id) / 8) % 8];
        const auto& pi = effect_info(EffectKind(slot.kind)).params[unsigned(b.id) % 8];
        auto shown = show(pi, b.value);
        c.label = pi.label; c.value = shown.value; c.unit = shown.unit; c.bipolar = bipolar(pi); c.fill = gauge_of(pi, b.value);
        int sync = delay_sync_param(EffectKind(slot.kind)); // roadmap P4: a delay on the clock shows its note value as its TIME
        if (sync >= 0 && unsigned(b.id) % 8 == 0 && slot.params[size_t(sync)] > 0) { c.value = delay_sync_name(slot.params[size_t(sync)]); c.unit = "sync"; }
        break;
    }
    case Kind::SendBus: c.label = "BUS"; c.value = juce::String::charToString(juce::juce_wchar('A' + b.value)); break;
    // Roadmap P4, the MOD page.
    case Kind::ModPick: c.label = "MOD"; c.value = juce::String(b.value + 1); break;
    case Kind::ModKind: c.label = "KIND"; c.value = mod_kind_name(ModKind(b.value)); break;
    case Kind::ModShape: c.label = "SHAPE"; c.value = lfo_shape_name(LfoShape(b.value)); break;
    case Kind::ModSync: c.label = "CLOCK"; c.value = b.value ? "Sync" : "Free"; break;
    case Kind::ModRate: {
        const auto& m = s.project.modulators[s.ui.mod_index % modulator_count];
        c.label = "RATE";
        if (m.sync) c.value = lfo_division_name(unsigned(b.value));
        else { double hz = lfo_hertz(unsigned(b.value)); c.value = juce::String(hz, hz < 1 ? 2 : 1); c.unit = "Hz"; }
        c.fill = float(b.value - b.min) / float(std::max(b.max - b.min, 1));
        break;
    }
    case Kind::ModInput:
        c.label = "SOURCE";
        if (b.value < int(track_count)) { c.value = juce::String(b.value + 1); c.unit = "track"; }
        else { c.value = juce::String::charToString(juce::juce_wchar('A' + b.value - int(track_count))); c.unit = "bus"; }
        break;
    case Kind::ModAttack: c.label = "RISE"; c.value = juce::String(int(std::lround(follower_attack_ms(unsigned(b.value))))); c.unit = "ms"; c.fill = b.value / 100.0f; break;
    case Kind::ModRelease: c.label = "FALL"; c.value = juce::String(int(std::lround(follower_release_ms(unsigned(b.value))))); c.unit = "ms"; c.fill = b.value / 100.0f; break;
    case Kind::ModDepth:
        c.label = "DEPTH"; c.value = (b.value > 0 ? "+" : b.value < 0 ? juce::String::fromUTF8("\xe2\x88\x92") : juce::String()) + juce::String(std::abs(b.value));
        c.unit = "%"; c.bipolar = true; c.fill = (b.value + 100) / 200.0f;
        break;
    case Kind::MacroPick: c.label = "MACRO"; c.value = juce::String(b.value + 1); break;
    case Kind::MacroValue: c.label = "VALUE"; c.value = juce::String(b.value); c.fill = b.value / 127.0f; break;
    case Kind::MacroLow: case Kind::MacroHigh: {
        c.label = b.kind == Kind::MacroLow ? "LOW" : "HIGH";
        const auto& g = s.project.macros[s.ui.macro_index % macro_count].targets[s.ui.macro_target % macro_targets];
        auto m = motion_param(s.project.tracks[g.track % track_count], motion_lane(g.param, g.pad), &s.project);
        if (m.info) { auto shown = show(*m.info, b.value); c.value = shown.value; c.unit = shown.unit; c.fill = gauge_of(*m.info, b.value); }
        else c.value = juce::String(b.value);
        break;
    }
    case Kind::MidiPort: { // roadmap P4 (M4): the port, a USB device by its name; one not there now OFFLINE (all USB meanwhile)
        bool offline = false;
        c.value = midi_port_text(s.ui, track, b.value, &offline);
        c.label = offline ? "OFFLINE" : "PORT"; // its notes go to every USB output meanwhile
        break;
    }
    case Kind::MidiSysexFile:
        if (unsigned sending = s.ui.sysex_percent[s.ui.track % track_count]) { c.label = "SENDING"; c.value = juce::String(sending); c.unit = "%"; break; }
        c.label = "FILE";
        c.value = track.midi.sysex[0] ? juce::String(track.midi.sysex).upToLastOccurrenceOf(".", false, false) : juce::String("None");
        break;
    case Kind::MidiDrumNote: { // roadmap P4 (M3): a drum pad's note, by its General MIDI name where it has one
        c.label = "NOTE";
        const char* gm = gm_drum_name(unsigned(b.value));
        c.value = gm ? juce::String(gm) : juce::String(b.value);
        c.unit = gm ? juce::String(b.value) : juce::String();
        break;
    }
    case Kind::FxKey: // roadmap P4: what keys a Compressor or Gate
        c.label = "KEY";
        if (fx_key_is_track(unsigned(b.value))) { c.value = juce::String(b.value); c.unit = "track"; }
        else if (fx_key_is_bus(unsigned(b.value))) { c.value = juce::String::charToString(juce::juce_wchar('A' + b.value - fx_key_bus(0))); c.unit = "bus"; }
        else c.value = "Off";
        break;
    // The Looper (docs/LOOPER.md).
    case Kind::LoopPick: c.label = "LOOP"; c.value = "L" + juce::String(b.value + 1); break;
    case Kind::LoopLength: { // set before a take; a loop with audio shows the bars it spans
        unsigned bars = looper::lengths[size_t(b.value) % looper::lengths.size()];
        const auto& v = s.loops[unsigned(b.id) % loop_tracks].view;
        if (v.ticks > 0) bars = unsigned(std::max(1L, std::lround(v.ticks / std::max(1u, s.project.bar_ticks()))));
        c.label = "LENGTH"; c.value = bars ? juce::String(bars) : juce::String("Free"); c.unit = !bars ? "" : bars == 1 ? "bar" : "bars";
        break;
    }
    case Kind::LoopLevel: c.label = "LEVEL"; c.value = juce::String(b.value); break;
    case Kind::LoopFeedback: c.label = "FEEDBACK"; c.value = juce::String(b.value); c.unit = "%"; break;
    case Kind::LoopQuantize: c.label = "QUANTIZE"; c.value = looper::quantize_name(unsigned(b.value)); break;
    case Kind::LoopThen: c.label = "AFTER TAKE"; c.value = looper::then_name(unsigned(b.value)); break;
    case Kind::LoopPan:
        c.label = "PAN"; c.bipolar = true;
        c.value = b.value == 0 ? juce::String("C") : (b.value < 0 ? "L" : "R") + juce::String(std::abs(b.value));
        break;
    case Kind::LoopSend:
        c.label = juce::String("SEND ") + char('A' + s.project.loop_mix[unsigned(b.id) % loop_tracks].bus % bus_count);
        c.value = juce::String(b.value); c.unit = "%";
        break;
    case Kind::LoopBus: c.label = "BUS"; c.value = juce::String::charToString(juce::juce_wchar('A' + b.value)); break;
    case Kind::LoopSource: { // the loop's sheet: where it records from, by name
        std::array<uint8_t, looper::source_count> sources {};
        unsigned own = 0, count = loop_sources(s.project, unsigned(b.id), sources, own);
        c.label = "SOURCE";
        c.value = juce::String(loop_source_name(sources[size_t(std::clamp(b.value, 0, int(count) - 1))]));
        break;
    }
    case Kind::BusReturn: c.label = "RETURN"; c.value = juce::String(b.value); c.unit = "%"; break;
    case Kind::PadPitch: c.label = "PITCH"; c.value = juce::String(b.value); c.unit = "st"; c.bipolar = true; break;
    case Kind::PadLevel: c.label = "VOLUME"; c.value = decibels(b.value); c.unit = b.value > 0 ? "dB" : ""; break;
    case Kind::PadPan:
        c.label = "PAN"; c.bipolar = true;
        c.value = b.value == 0 ? juce::String("C") : (b.value < 0 ? "L" : "R") + juce::String(std::abs(b.value));
        break;
    case Kind::PadStart: case Kind::PadEnd: case Kind::ChopStart: case Kind::ChopEnd: case Kind::PadLoopPoint: // frames, as seconds
        c.label = b.kind == Kind::PadLoopPoint ? "LOOP" : b.kind == Kind::PadEnd || b.kind == Kind::ChopEnd ? "END" : "START";
        c.value = juce::String(b.value / 44100.0, b.max - b.min < 4410 ? 4 : 3); c.unit = "s"; // zoomed under 0.1 s: a tenth of a ms
        break;
    case Kind::TrimZoom: case Kind::ChopZoom: // 0-100%, as the MPC's
        c.label = b.kind == Kind::ChopZoom ? (b.id ? "ZOOM END" : "ZOOM START") : b.id == 0 ? "ZOOM START" : b.id == 1 ? "ZOOM END" : "ZOOM LOOP";
        c.value = juce::String(b.value); c.unit = "%";
        break;
    case Kind::PadLoop: c.label = "LOOP"; c.value = b.value ? "On" : "Off"; break;
    case Kind::PadMono: c.label = "VOICES"; c.value = b.value ? "Mono" : "Poly"; break;
    case Kind::PadLink: {
        char label[4]; pad_label(track.last_pad / 16 * 16 + unsigned(b.value + 15) % 16, label);
        c.label = "PAD LINK"; c.value = b.value ? juce::String(label) : juce::String("Off");
        break;
    }
    case Kind::PadOffset: c.label = "OFFSET"; c.value = juce::String(b.value); c.unit = "%"; break;
    case Kind::PadVelSense: c.label = "VEL SENS"; c.value = juce::String(b.value); c.unit = "%"; break;
    case Kind::PadDecayFrom: c.label = "DECAY FROM"; c.value = b.value ? "End" : "Start"; break;
    case Kind::PadFilterType: c.label = "TYPE"; c.value = pad_filter_name(PadFilter(b.value)); break;
    case Kind::PadCutoff: c.label = "CUTOFF"; c.value = juce::String(b.value); c.unit = "%"; break;
    case Kind::PadResonance: c.label = "RESO"; c.value = juce::String(b.value); c.unit = "%"; break;
    case Kind::PadFAmount: c.label = "DEPTH"; c.value = juce::String(b.value); c.unit = "%"; break;
    case Kind::PadFSustain: c.label = "SUSTAIN"; c.value = juce::String(b.value); c.unit = "%"; break;
    case Kind::PadFAttack: case Kind::PadFDecay: case Kind::PadFRelease: {
        c.label = b.kind == Kind::PadFAttack ? "ATTACK" : b.kind == Kind::PadFDecay ? "DECAY" : "RELEASE";
        float seconds = time_seconds(b.value) * (b.kind == Kind::PadFAttack ? 0.3f : b.kind == Kind::PadFRelease ? 0.5f : 1.0f);
        if (seconds < 1) { c.value = juce::String(int(seconds * 1000)); c.unit = "ms"; } else { c.value = juce::String(seconds, 1); c.unit = "s"; }
        break;
    }
    case Kind::PadWarp: {
        auto warp = warp_of_card(b.value);
        c.label = "WARP";
        if (warp == warp_seq) {
            c.value = "Seq";
            // Seq stretches the pad to the project's tempo from its own: past 2x either way it says by how much, in amber.
            const auto& t = s.track();
            if (t.engine == EngineKind::Sampler && s.sample.frames) {
                const auto& pad = t.pads[t.last_pad % sampler_pads];
                uint32_t end = pad.end ? pad.end : s.sample.frames;
                double seconds = end > pad.start ? (end - pad.start) / 44100.0 : 0;
                double own = pad.beats && seconds > 0 ? pad.beats * 60.0 / seconds : pad.own_tempo / 10.0, now = s.project.tempo / 10.0;
                double stretch = own > 0 && now > 0 ? own / now : 1;
                if (stretch > 2 || stretch < 0.5) { c.unit = juce::String::fromUTF8("\xc3\x97") + juce::String(stretch, 1); c.shade = ui::colour::amber; }
            }
        }
        else if (warp == warp_off) c.value = "Off";
        else { c.value = juce::String(int(warp)); c.unit = "%"; }
        break;
    }
    case Kind::PadBeats: c.label = "# BEATS"; c.value = b.value ? juce::String(b.value) : juce::String("Auto"); break;
    case Kind::LayerPick: { // roadmap P3: the layer in hand, empty ones said so
        const auto& pad = track.pads[track.last_pad % sampler_pads];
        c.label = "LAYER"; c.value = juce::String(b.value + 1);
        if (!pad.layer(unsigned(b.value) % pad_layers).used()) c.unit = "empty";
        break;
    }
    case Kind::LayerLow: c.label = "VEL LOW"; c.value = juce::String(b.value); break;
    case Kind::ZonePick: c.label = "ZONE"; c.value = juce::String(b.value + 1); c.unit = "/ " + juce::String(b.max + 1); break; // round 3
    case Kind::ZoneTune: c.label = "TUNE"; c.value = (b.value > 0 ? "+" : "") + juce::String(b.value); c.unit = "ct"; c.bipolar = true; break;
    case Kind::ZoneLevel: c.label = "LEVEL"; c.value = (b.value > 0 ? "+" : "") + juce::String(b.value / 10.0, 1); c.unit = "dB"; c.bipolar = true; break;
    case Kind::PadLoopXfade: c.label = "XFADE"; c.value = b.value ? juce::String(b.value) : juce::String("Off"); c.unit = b.value ? "ms" : ""; break;
    case Kind::LayerHigh: c.label = "VEL HIGH"; c.value = juce::String(b.value); break;
    case Kind::LayerPitch: c.label = "TUNE"; c.value = juce::String(b.value); c.unit = "st"; c.bipolar = true; break;
    case Kind::LayerGain: c.label = "LEVEL"; c.value = (b.value > 0 ? "+" : "") + juce::String(b.value / 10.0, 1); c.unit = "dB"; c.bipolar = true; break;
    case Kind::LayerPlayMode: c.label = "PLAY"; c.value = name_case(juce::String(layer_play_name(LayerPlay(b.value)))); break;
    case Kind::ChopMode: c.label = "CHOP TYPE"; c.value = chop_mode_name(ChopMode(b.value)); break;
    case Kind::ChopAmount: {
        auto mode = ChopMode(b.id);
        c.label = mode == ChopMode::Threshold ? "THRESHOLD" : mode == ChopMode::Regions || mode == ChopMode::Auto ? "SLICES" : "DIVISION";
        c.value = juce::String(mode == ChopMode::Auto ? int(auto_slice_count(b.value)) : b.value); c.unit = mode == ChopMode::Threshold ? "%" : "";
        if (mode == ChopMode::Beats) { c.value = division_value(unsigned(b.value)); c.unit = division_unit(unsigned(b.value)); } // 1/16 ... 4 bars
        break;
    }
    case Kind::FileJump:
        c.label = "JUMP"; c.value = juce::String(b.value + 1).paddedLeft('0', 2); c.unit = "/ " + juce::String(s.ui.files_count);
        break;
    case Kind::BrowseStart: case Kind::BrowseEnd: { // Samples' trim sheet (shell/browse_trim.h): frames, as seconds
        c.label = b.kind == Kind::BrowseEnd ? "END" : "START";
        uint32_t frames = uint32_t(std::max(b.value, 0));
        if (b.id) { // not read yet: the file's length where this project has it already
            frames = 0;
            const auto* file = s.ui.files_cursor < s.files.size() ? &s.files[s.ui.files_cursor] : nullptr;
            if (b.kind == Kind::BrowseEnd && file)
                for (const auto& ref : s.project.samples) if (ref.used() && ref.frames && file->path == ref.path) { frames = ref.frames; break; }
            if (b.kind == Kind::BrowseEnd && !frames) { c.value = juce::String::fromUTF8("\xe2\x80\x94"); break; } // an em dash: not known yet
        }
        c.value = juce::String(frames / 44100.0, !b.id && b.max - b.min < 4410 ? 4 : 3); c.unit = "s"; // zoomed under 0.1 s: a tenth of a ms
        break;
    }
    case Kind::BrowseZoom: c.label = b.id ? "ZOOM END" : "ZOOM START"; c.value = juce::String(b.value); c.unit = "%"; break;
    case Kind::PluginJump:
        c.label = "JUMP"; c.value = juce::String(b.value + 1).paddedLeft('0', 2); c.unit = "/ " + juce::String(int(s.plugins.files.size()));
        break;
    case Kind::ProjectJump:
        c.label = "JUMP"; c.value = juce::String(b.value + 1); c.unit = "/ " + juce::String(b.max + 1);
        break;
    case Kind::ProjectSort: c.label = "SORT"; c.value = b.value ? "Name" : "Newest"; break;
    case Kind::PadFine: c.label = "FINE"; c.value = juce::String(b.value); c.unit = "ct"; c.bipolar = true; break;
    case Kind::PadPlayMode: c.label = "NOTE ON"; c.value = b.value ? "Off" : "On"; break;
    case Kind::PadGroup: c.label = "MUTE GROUP"; c.value = b.value ? juce::String(b.value) : juce::String("Off"); break;
    case Kind::PadColour: // Stage's COLOUR: the last pad's by name (the card keeps the track's colours); Kit: the kit's own
        c.label = "COLOUR";
        c.value = b.value == colour_track ? juce::String("Track") : b.value < colour_own ? juce::String(ui::track_colour_names[size_t(b.value - 1)]) : juce::String("Kit");
        break;
    case Kind::PadReverse: c.label = "REVERSE"; c.value = b.value ? "On" : "Off"; break;
    case Kind::PadAttack: case Kind::PadDecay: case Kind::PadRelease: {
        c.label = b.kind == Kind::PadAttack ? "ATTACK" : b.kind == Kind::PadDecay ? "DECAY" : "RELEASE";
        if (b.kind == Kind::PadDecay && b.value >= 100) { c.value = "Hold"; break; }
        float seconds = time_seconds(b.value) * (b.kind == Kind::PadAttack ? 0.3f : b.kind == Kind::PadRelease ? 0.5f : 1.0f);
        if (seconds < 1) { c.value = juce::String(int(seconds * 1000)); c.unit = "ms"; } else { c.value = juce::String(seconds, 1); c.unit = "s"; }
        break;
    }
    case Kind::PadSustain: c.label = "SUSTAIN"; c.value = juce::String(b.value); c.unit = "%"; break;
    case Kind::Tempo: c.label = "TEMPO"; c.value = juce::String(int(s.tempo) / 10); c.unit = "BPM"; break;
    case Kind::NameSuggest: {
        std::array<const char*, max_suggestions> names {};
        unsigned count = name_suggestions(s.ui.name, names);
        c.label = "SUGGEST"; c.value = count ? juce::String(names[std::min<unsigned>(unsigned(b.value), count - 1)]) : juce::String("None");
        break;
    }
    case Kind::NameChar: {
        char ch = NameEntry::charset[unsigned(b.value) % NameEntry::charset_size()];
        c.label = "CHARACTER"; c.value = ch == ' ' ? juce::String("Space") : juce::String::charToString(juce::juce_wchar(ch));
        break;
    }
    case Kind::PunchDepth: c.label = "DEPTH"; c.value = juce::String(b.value); c.unit = "%"; break;
    case Kind::PunchSnap: c.label = "SNAP"; c.value = punch_snaps[b.value % punch_snap_count]; break;
    case Kind::PunchTail: c.label = "TAIL"; c.value = punch_tails[b.value % 3]; break;
    case Kind::ClickLevel: c.label = "CLICK"; c.value = juce::String(b.value); c.unit = "%"; break;
    case Kind::Features: c.label = "FEATURES"; c.value = b.value ? "Full" : "Basic"; break;
    case Kind::UsageLog: c.label = "USAGE LOG"; c.value = b.value ? "On" : "Off"; break;
    case Kind::SettingRow: { // Settings: the row under the cursor, at the card's value (one that waits for K1 to rest too)
        Settings shown = s.ui.settings;
        shown.set_choice(unsigned(b.id), unsigned(b.value));
        c.label = juce::String(Settings::row_name(unsigned(b.id))).toUpperCase(); c.label_mixed = Settings::row_name(unsigned(b.id));
        c.value = shown.value_text(unsigned(b.id));
        break;
    }
    case Kind::MidiRow: { // MIDI Config: the same; the track's own two rows read off the card's value (no copy of the track)
        unsigned row = unsigned(b.id);
        c.label = juce::String(midi_row_name(row)).toUpperCase(); c.label_mixed = midi_row_name(row);
        if (row == TrackChannelRow) { c.value = midi_channel_text(uint8_t(b.value), "Off"); break; }
        if (row == TrackInRow) { c.value = track_midi_in_name(TrackMidiIn(b.value)); break; }
        Settings shown = s.ui.settings;
        midi_row_set(shown, row, unsigned(b.value));
        c.value = midi_row_value(shown, track, row);
        break;
    }
    case Kind::ArpMode: c.label = "MODE"; c.value = arp_mode_name(ArpMode(b.value)); break;
    case Kind::ArpRate: c.label = "RATE"; c.value = arp_rate_name(unsigned(b.value)); break;
    case Kind::ArpOctaves: c.label = "OCTAVES"; c.value = juce::String(b.value); break;
    case Kind::ChordType: c.label = "TYPE"; c.value = chord_type_name(unsigned(b.value)); break;
    case Kind::ChordVoicing: c.label = "VOICING"; c.value = chord_voicing_name(unsigned(b.value)); break;
    case Kind::ChordInvert: c.label = "INVERT"; c.value = chord_invert_name(unsigned(b.value)); break;
    case Kind::RecThreshold:
        c.label = "THRESHOLD";
        if (b.value <= InputConfig::threshold_min) c.value = "At once";
        else { c.value = (b.value < 0 ? juce::String::fromUTF8("\xe2\x88\x92") : juce::String()) + juce::String(std::abs(b.value)); c.unit = "dB"; }
        break;
    case Kind::RecLength:
        c.label = "REC LENGTH";
        if (b.value == int(rec_free)) c.value = "Free";
        else if (b.value == int(rec_seq)) c.value = "Seq";
        else { auto bars = rec_bars(unsigned(b.value)); c.value = juce::String(bars); c.unit = bars == 1 ? "bar" : "bars"; }
        break;
    case Kind::RecSource: {
        c.label = "SOURCE";
        unsigned code = input_choices(s.project, s.ui.track, s.ui.settings.input).code_at(b.value);
        if (code < input_track_code) {
            c.value = input_source_name(InputSource(code));
            if (source_in_use(s, InputSource(code))) c.unit = "IN USE"; // the ADC is on the other input: silent
            break;
        }
        unsigned t = code - input_track_code; // the track on the label, its engine as the value
        c.label = "TRACK " + juce::String(t + 1);
        c.value = sounding_track(s.project.tracks[t]) ? engine_word(s.project.tracks[t].engine) : juce::String("Silent");
        break;
    }
    case Kind::RecTap: c.label = "TAP"; c.value = b.value ? "Post FX" : "Pre FX"; break;
    case Kind::AudioSource: { // freeze plan F3
        const InputSource source = audio_sources[unsigned(b.value) % audio_source_count];
        c.label = "SOURCE"; c.value = audio_source_label(source);
        if (source_in_use(s, source)) c.unit = "IN USE";
        break;
    }
    case Kind::AudioMonitorMode: c.label = "MONITOR"; c.value = track_monitor_name(TrackMonitor(b.value)); break;
    case Kind::KnobParam: {
        if (unsigned(b.id) == knob_morph) { // the fader on Knob FX (knob fx C): the way from A to B
            c.label = "A TO B"; c.value = juce::String(b.value * 100 / 127); c.unit = "%"; c.fill = b.value / 127.0f;
            break;
        }
        const auto& pi = effect_info(EffectKind(s.project.knob_fx.kind)).params[unsigned(b.id) % effect_params];
        auto shown = show(pi, b.value);
        c.label = pi.label; c.value = shown.value; c.unit = shown.unit; c.bipolar = bipolar(pi); c.fill = gauge_of(pi, b.value);
        break;
    }
    case Kind::PluginParam: { // as the shell last read it from the plug-in (plugin/plugin_panel.h)
        const unsigned k = plugin_binding_k(b.id), shown = plugin_binding_shown(b.id);
        const auto& card = s.ui.plugin_panel.params[k];
        // Its name as the plug-in gives it (capitals, else mixed case, else over two lines: ui/knobs.cpp); its number
        // where it has none.
        auto name = juce::String::fromUTF8(card.name).trim();
        c.label = name.isEmpty() ? juce::String(shown >= plugin_screen_slot ? k + 1 : shown + 1) : name.toUpperCase();
        if (name.isNotEmpty()) c.label_mixed = name;
        c.value = juce::String::fromUTF8(card.text);
        c.fill = std::clamp(b.value / float(plugin_param_max), 0.0f, 1.0f);
        break;
    }
    case Kind::PadFxParam: {
        const auto& pi = mpc_pad_fx(unsigned(b.id) / 4).params[unsigned(b.id) % 4 % punch_params];
        auto shown = show(pi, b.value);
        c.label = pi.label; c.value = shown.value; c.unit = shown.unit; c.fill = gauge_of(pi, b.value);
        break;
    }
    case Kind::FlexMode: c.label = "MODE"; c.value = b.value ? "Loop" : "One Shot"; break;
    case Kind::FlexMix: c.label = "MIX"; c.value = juce::String(b.value); c.unit = "%"; break;
    case Kind::CompAttack: case Kind::CompRelease: {
        float ms = b.kind == Kind::CompAttack ? comp_attack_ms(b.value) : comp_release_ms(b.value);
        c.label = b.kind == Kind::CompAttack ? "ATTACK" : "RELEASE";
        c.value = ms < 10 ? juce::String(ms, 1) : juce::String(int(std::lround(ms)));
        c.unit = "ms";
        break;
    }
    case Kind::CompAmount: c.label = "AMOUNT"; c.value = juce::String(b.value); c.unit = "%"; break;
    case Kind::CompBoost: c.label = "IN BOOST"; c.value = "+" + juce::String(b.value); c.unit = "dB"; break;
    case Kind::RecGain:
        c.label = "GAIN"; c.unit = "dB";
        c.value = (b.value > 0 ? "+" : b.value < 0 ? juce::String::fromUTF8("\xe2\x88\x92") : juce::String()) + juce::String(std::abs(b.value));
        break;
    case Kind::ModWheel: c.label = "MOD WHEEL"; c.value = juce::String(b.value); break;
    case Kind::PitchBend: // centred as a pan: 0 in the middle, a sign either side
        c.label = "PITCH BEND"; c.bipolar = true;
        c.value = (b.value > 0 ? "+" : b.value < 0 ? juce::String::fromUTF8("\xe2\x88\x92") : juce::String()) + juce::String(std::abs(b.value));
        break;
    case Kind::StageSound:
        std::snprintf(text, sizeof(text), "%02d", b.value + 1);
        c.label = "SOUND"; c.value = text; c.unit = "/ " + juce::String(b.max + 1);
        break;
    case Kind::SoundList: { static const char* lists[] {"All", "Starred", "Recent"}; c.label = "LIST"; c.value = lists[b.value % 3]; break; }
    case Kind::ArpGate: c.label = "GATE"; c.value = juce::String(b.value); c.unit = "%"; break;
    case Kind::ArpHold: c.label = "HOLD"; c.value = b.value ? "On" : "Off"; break;
    case Kind::ArpOn: c.label = "ARP"; c.value = b.value ? "On" : "Off"; break;
    case Kind::ChordOn: c.label = "CHORD"; c.value = b.value ? "On" : "Off"; break;
    case Kind::LevelsType: {
        std::array<LevelsKind, 6> types {};
        unsigned count = levels_types(s.project, track, types);
        c.label = "LEVELS"; c.value = levels_kind_name(types[size_t(b.value) % std::max(count, 1u)]);
        break;
    }
    case Kind::LevelsLow: case Kind::LevelsHigh: { // as the pad at that end plays it
        auto kind = LevelsKind(track.levels_kind % unsigned(LevelsKind::Count));
        c.label = b.kind == Kind::LevelsLow ? "LOW" : "HIGH";
        bool signed_value = kind == LevelsKind::Scale || kind == LevelsKind::Tune;
        c.value = (signed_value && b.value > 0 ? "+" : b.value < 0 ? juce::String::fromUTF8("\xe2\x88\x92") : juce::String()) + juce::String(std::abs(b.value));
        c.unit = kind == LevelsKind::Tune ? "st" : kind == LevelsKind::Filter ? "%" : kind == LevelsKind::Scale ? (std::abs(b.value) == 1 ? "step" : "steps") : "";
        c.bipolar = signed_value;
        break;
    }
    case Kind::NewEngine: {
        std::array<EngineKind, size_t(EngineKind::Count)> engines {};
        unsigned count = new_engines(engines);
        c.label = "ENGINE"; c.value = count ? engine_info(engines[unsigned(b.value) % count]).name : "";
        break;
    }
    case Kind::NewSound:
        std::snprintf(text, sizeof(text), "%02d", b.value + 1);
        c.label = "SOUND"; c.value = text; c.unit = "/ " + juce::String(b.max + 1);
        break;
    case Kind::NewSource: { static const char* sources[] {"Card", "Input"}; c.label = "SOURCE"; c.value = sources[unsigned(b.value) % new_sources]; break; }
    case Kind::NewPort: { static const char* ports[] {"External", "USB-C", "All USB"}; c.label = "PORT"; c.value = ports[unsigned(b.value) % 3]; break; }
    case Kind::NewChannel: c.label = "CHANNEL"; c.value = juce::String(b.value); break;
    case Kind::TrackPick:
        std::snprintf(text, sizeof(text), "%02d", b.value + 1);
        c.label = "TRACK"; c.value = text;
        break;
    case Kind::TrackPattern: c.label = "PATTERN"; c.value = "P" + juce::String(b.value + 1); break;
    case Kind::None: break;
    }
}

void midi_route(ui::Paint& p, const Snapshot& s, float x, float y, float w, float h) {
    using namespace ui;
    const auto& t = s.track();
    auto c = colour_of(s);
    unsigned outputs = s.ui.midi_outputs, devices = (outputs >> 8) & 255;
    Icon port = Icon::Din; // the MIDI Out jack; a computer over USB-C; every USB device
    bool missing = false;
    switch (midi_track::dest_of(t.params[midi_track::Port])) {
    case MidiDest::UsbC: port = Icon::Laptop; missing = !(outputs & 2); break;
    case MidiDest::AllUsb: port = Icon::Usb; missing = !devices && !(outputs & 2); break;
    default: break;
    }
    constexpr float g = 24;
    float cy = y + 30, x0 = x + 28, x1 = x + w / 2, x2 = x + w - 46;
    float a0 = x0 + g / 2 + 8, a1 = x1 - g / 2 - 8, b0 = x1 + g / 2 + 8, b1 = x2 - g / 2 - 8;
    bool sending = s.ui.track_out_at[s.ui.track] && s.now - s.ui.track_out_at[s.ui.track] < 150;
    p.fill(a0, cy - 0.8f, a1 - a0, 1.6f, c.withAlpha(sending ? 1.0f : 0.45f), 0.8f);
    p.fill(b0, cy - 0.8f, b1 - b0, 1.6f, c.withAlpha(sending ? 1.0f : 0.45f), 0.8f);
    icon(p, Icon::Pads, x0 - g / 2, cy - g / 2, g, colour::sec, 1.4f);
    icon(p, missing ? Icon::Warn : port, x1 - g / 2, cy - g / 2, g, missing ? colour::amber : c, 1.4f);
    icon(p, Icon::Keyboard, x2 - g / 2, cy - g / 2, g, colour::text, 1.4f);
    juce::String name = t.name[0] ? name_case(juce::String(t.name)) : juce::String("MIDI");
    p.text(name, x2 - 60, cy + g / 2 + 6, 120, 16, p.fit(name, plain(colour::text, type::body, Weight::SemiBold), 112, type::floor),
           juce::Justification::centred);
    // Notes on their way out: a dot each, from the track to the instrument in 0.7 s, brighter for harder.
    constexpr uint32_t travel = 700, shown = 4000;
    float span = (a1 - a0) + (b1 - b0);
    for (const auto& n : s.ui.sent_notes) {
        if (n.track != s.ui.track || !n.at || s.now - n.at > travel) continue;
        float d = span * float(s.now - n.at) / travel, dx = d < a1 - a0 ? a0 + d : b0 + (d - (a1 - a0));
        p.circle(dx, cy, 2.6f, c.withAlpha(0.45f + 0.55f * n.velocity / 127.0f));
    }
    // The last notes sent: four seconds, newest on the right, each as long as it was held.
    float ry = y + h - 60, rh = 58;
    p.fill(x, ry, w, rh, colour::quiet, 4);
    int lo = 127, hi = 0;
    for (const auto& n : s.ui.sent_notes)
        if (n.track == s.ui.track && n.at && s.now - n.at < shown) { lo = std::min<int>(lo, n.key); hi = std::max<int>(hi, n.key); }
    if (hi < lo) return;
    lo -= 2; hi += 2;
    for (const auto& n : s.ui.sent_notes) {
        if (n.track != s.ui.track || !n.at || s.now - n.at >= shown) continue;
        float nx0 = x + w - w * float(s.now - n.at) / shown, nx1 = n.until ? x + w - w * float(s.now - std::min(n.until, s.now)) / shown : x + w;
        float ny = ry + 4 + (rh - 12) * (1 - float(n.key - lo) / float(hi - lo));
        p.fill(nx0, ny, std::max(2.0f, nx1 - nx0), 4, c.withAlpha(0.5f + 0.5f * n.velocity / 127.0f), 2);
    }
}

FaderShown fader_shown(const Snapshot& s) {
    FaderShown out;
    auto job = fader_job(s.project, s.ui);
    const auto& t = s.track();
    if (job.source == FaderJob::Source::None) { out.what = "OFF"; out.off = true; return out; }
    char pad[4];
    pad_label(t.last_pad % sampler_pads, pad);
    bool kit_pad = job.source == FaderJob::Source::Target && job.pad; // the last pad's own value: its name carries the pad
    if (!job.binding.bound()) { out.what = kit_pad ? juce::String(pad) + " EMPTY" : juce::String("NOTHING"); return out; }
    out.value = true;
    out.card.fill = job.binding.fraction();
    out.card.track = out.card.shade = colour_of(s);
    describe_card(s, job.binding, out.card);
    if (job.binding.kind == Binding::Kind::Param && job.binding.id == sampler::Level && t.engine == EngineKind::Sampler) out.card.label = "KIT VOLUME";
    out.what = kit_pad ? juce::String(pad) + " " + out.card.label : out.card.label;
    if (job.learned && job.binding.kind == Binding::Kind::FxParam) { // an effect's value (fader C): which effect
        const auto& slot = t.fx.slots[size_t(unsigned(job.binding.id) % 64 / 8 % track_slots)];
        out.what = juce::String(effect_info(EffectKind(slot.kind)).name).toUpperCase() + " " + out.card.label;
    }
    const auto& f = s.ui.fader;
    out.waiting = f.position >= 0 && !f.engaged;
    if (out.waiting) { out.card.state = ui::KnobCard::State::Pickup; out.card.pot = f.position / 127.0f; }
    return out;
}

void frame(ui::Paint& p, const Snapshot& s, const ui::SoftKeys& keys, const ui::Header& header) {
    frame_keys(p, s, keys);
    auto cards = cards_of(s);
    bool knob_fx = std::any_of(cards.begin(), cards.end(), [](const ui::KnobCard& c) { return c.knob_fx; });
    if (knob_fx && s.ui.page != PageId::KnobFx) { // the pots are Knob FX's: the header names its effect first
        auto shown = header;
        auto name = juce::String(knob_fx_entry(s.project.knob_fx.choice).name).toUpperCase();
        shown.left.insert(shown.left.begin() + std::min<size_t>(shown.lead, shown.left.size()), ui::chip(ui::Icon::Knob, name, ui::colour::text)); // after the states and the pads chip
        ui::header(p, shown);
    } else {
        ui::header(p, header);
    }
    ui::knob_strip(p, cards);
}

void frame_keys(ui::Paint& p, const Snapshot& s, const ui::SoftKeys& keys) {
    if (s.ui.pad_bank && !track_overlay_shown(s.ui)) { // Pad Bank held: its keys over the page's (shell/pad_bank.h)
        ui::SoftKeys held {{"TRACKS", "MOVE", "COPY"}, -1, colour_of(s)};
        held.glyphs[1] = ui::Icon::Swap; held.glyphs[2] = ui::Icon::Copy;
        ui::soft_keys(p, held);
    } else if (auto k = levels_keys(s.track()); levels_held_cards(s.project, s.ui) && k.key[0] != LevelsKey::None
               && (s.ui.levels_used || s.now - s.ui.levels_down >= levels_hold_ms)) { // 16 Levels held: its keys, by the type (shell/levels_keys.h)
        ui::SoftKeys held {{levels_key_name(k.key[0]), levels_key_name(k.key[1]), levels_key_name(k.key[2])}, k.lit, colour_of(s)};
        ui::soft_keys(p, held);
    } else {
        ui::soft_keys(p, keys);
    }
}

void play_pad(Context& c, unsigned pad, uint8_t velocity, bool down) { c.play_pad(pad, velocity, down); }
void octave(Context& c, int way) {
    auto& t = c.track();
    if (drum_pads(t)) { // Drums pads: − / + the bank, A-D
        t.bank = uint8_t(std::clamp(int(drum_bank(t)) + way, 0, int(drum_banks) - 1));
        c.notice(std::string("Bank ") + char('A' + drum_bank(t)));
        return;
    }
    t.octave = int8_t(std::clamp(t.octave + way, 0, 7));
}
void no_wheel(Context&, int) {}
void no_push(Context&) {}
unsigned key_pages(const Snapshot& s, unsigned key, std::array<juce::String, 6>& names, unsigned& at) {
    key %= 3; at = 0;
    unsigned n = 0;
    const auto& t = s.track();
    bool kit = t.engine == EngineKind::Sampler;
    if (s.ui.page == PageId::Stage) { // the group's pages, as its key names them
        if (!t.used()) return 0;
        const auto& g = stage_groups(t.engine);
        StageSkip instrument = stage_skip(s.project, s.ui, t);
        if (!stage_has(g, key) || stage_opens_edit(t, key)) return 0;
        auto here = stage_at(s.ui, t, instrument);
        for (unsigned sub = 0; sub < stage_count(g, key) && n < names.size(); ++sub) {
            if (stage_skips(g, key, sub, instrument)) continue;
            if (here.group == key && here.sub == sub) at = n;
            names[n++] = g.groups[key].pages[sub].key;
        }
    } else if (s.ui.page == PageId::Mixer) { // B1: SENDS and FX on a track or submix; B2: the EQ's three groups (pages/mixer_page.cpp)
        if (key == 0 && full_features(s.ui) && channel_sends(channel_kind(mixer_strip(s.ui)))) {
            names[0] = "SENDS"; names[1] = "FX"; n = 2;
            at = s.ui.mixer_b1 ? 1 : 0;
        }
        if (key == 1 && full_features(s.ui)) {
            names[0] = "LOW"; names[1] = "MID"; names[2] = "HIGH"; n = 3;
            at = s.ui.mixer_eq_group % 3;
        }
    } else if (mix_section(s.ui.page) != MixSection::None) { // FX: along the signal
        if (key == 2) {
            std::array<PageId, 4> fx {};
            unsigned count = mix_fx_pages(kit, fx);
            for (unsigned k = 0; k < count; ++k) names[n++] = fx[k] == PageId::Rack ? "TRACK" : fx[k] == PageId::Bus ? "BUS" : fx[k] == PageId::Master ? "MASTER" : "PAD";
            at = mix_section(s.ui.page) == MixSection::Fx ? mix_fx_at(s.ui.page, kit) : 0;
        }
    } else if ((s.ui.page == PageId::Motion || s.ui.page == PageId::ModPage) && key == 2 && mod_shown(s.ui)) { // Seq's B3: its three pages
        names[0] = "MOTION"; names[1] = "MOD"; names[2] = "MACRO"; n = 3;
        at = s.ui.page == PageId::Motion ? 0 : s.ui.mod_view == 1 ? 2 : 1;
    } else if ((s.ui.page == PageId::Patterns || s.ui.page == PageId::Scenes) && key == 0) { // Seq's B1: its two pages
        names[0] = "SEQUENCES"; names[1] = "PATTERNS"; n = 2; at = s.ui.page == PageId::Patterns ? 1 : 0;
    }
    return n > 1 ? n : 0;
}

ui::SoftKeys mix_keys(const Snapshot& s) {
    bool kit = s.track().engine == EngineKind::Sampler;
    auto here = mix_section(s.ui.page);
    std::array<PageId, 4> fx {};
    unsigned count = mix_fx_pages(kit, fx);
    ui::SoftKeys keys {{"MIXER", "", "FX"}, here == MixSection::None ? -1 : int(here), colour_of(s)}; // decision 8
    keys.dots[2] = uint8_t(count);
    keys.dot_at[2] = uint8_t(here == MixSection::Fx ? mix_fx_at(s.ui.page, kit) : 0);
    return keys;
}
void mix_soft(Context& c, int key) {
    bool kit = c.track().engine == EngineKind::Sampler;
    if (key == 1) return; // B2: none (the mixer's SENDS block is the sends)
    PageId next = mix_next(c.ui.page, unsigned(key), kit);
    if (next == PageId::Rack) { c.ui.fx_loop = -1; c.ui.fx_sub = -1; } // the track's own chain, not a loop's or a submix's
    if (next == PageId::Mixer) { // B1 MIXER: the mixer on the channel whose chain this is
        if (c.ui.page == PageId::Bus) pick_other(c.ui, bus_chain(c.ui.sends_bus % bus_count));
        else if (c.ui.page == PageId::Master) pick_other(c.ui, master_chain);
        else if (c.ui.page == PageId::Rack || c.ui.page == PageId::PadFx) c.ui.mixer_bank = 0;
    }
    if (next != c.ui.page) c.open(next);
}

const char* motion_key(const Snapshot& s) { return motion_shown(s.ui, s.pattern) ? "MOTION" : ""; }

void seq_hub_soft(Context& c, int key) {
    static const PageId pages[] {seq_home, PageId::Roll, PageId::Motion}; // B1: Sequences (again there: Patterns)
    if (unsigned(key) % 3 == 2 && !motion_shown(c.ui, c.pattern())) return; // Basic: no MOTION key (shell/features.h)
    if (pages[unsigned(key) % 3] != c.ui.page) c.open(pages[unsigned(key) % 3]);
}

void play_soft(Context& c, int key) {
    // Stage on that section: its first page, or its own kept when it was the one open (as Sample held's keys).
    const auto& t = c.track();
    c.open(PageId::Stage);
    if (t.used() && stage_has(stage_groups(t.engine), unsigned(key) % 3) && stage_at(c.ui, t, stage_skip(c.project, c.ui, t)).group != unsigned(key) % 3)
        stage_go(c.ui, unsigned(key) % 3, 0);
}

// A pad's name: its sample's, or for pads sharing one sample (a chop), the
// last word of it and which slice: "Break 5" in full, "Brk 5" on a pad.
juce::String pad_name(const Snapshot& s, unsigned kit, bool short_form) {
    const auto& pads = s.track().pads;
    const auto& pad = pads[kit];
    if (!pad.used()) return {};
    juce::String name = s.project.samples[pad.sample % sample_slots].name;
    unsigned index = 0, sharing = 0;
    for (unsigned n = 0; n < sampler_pads; ++n)
        if (pads[n].sample == pad.sample) { ++sharing; if (n <= kit) ++index; }
    if (sharing < 2) return name;
    // The last word with letters in it ("LOOP 1 001" gives Loop).
    juce::StringArray words;
    words.addTokens(name, " ", "");
    juce::String word = name;
    for (int k = words.size() - 1; k >= 0; --k)
        if (words[k].containsAnyOf("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz")) { word = words[k]; break; }
    word = word.toLowerCase();
    word = word.substring(0, 1).toUpperCase() + word.substring(1);
    if (short_form) { // the first letter and the next two consonants
        juce::String brief = word.substring(0, 1);
        for (int k = 1; k < word.length() && brief.length() < 3; ++k)
            if (!juce::String("aeiou").containsChar(word[k])) brief += word[k];
        word = brief;
    }
    return word + " " + juce::String(index);
}


juce::String name_case(const juce::String& name) {
    // Words kept as written: roman numerals and the acronyms sound names use.
    static const char* keep[] {"I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X", "FM", "LFO", "MIDI", "SID", "CZ", "DX", "PWM",
                               "TB", "TR", "VCO", "VCF", "EP", "OB", "SH", "JX", "MS", "AM", "DJ", "HQ", "UK", "USA", "BPM", "SFZ", "MPC", "EQ", "SP"};
    juce::StringArray words;
    words.addTokens(name, " ", "");
    for (auto& word : words) {
        bool letters = word.isNotEmpty() && word.containsOnly("ABCDEFGHIJKLMNOPQRSTUVWXYZ'-");
        bool kept = false;
        for (auto k : keep) kept = kept || word == k;
        if (letters && !kept && word.length() > 1) { // each part after a hyphen too: "Auto-Pan"
            juce::String out;
            bool start = true;
            for (auto ch : word) {
                out += start ? juce::CharacterFunctions::toUpperCase(ch) : juce::CharacterFunctions::toLowerCase(ch);
                start = ch == '-';
            }
            word = out;
        }
    }
    return words.joinIntoString(" ");
}

juce::String cell_name(const ui::Paint& p, const juce::String& name, const ui::Paint::Text& style, float w) {
    return cell_name(name, p.width(name, style) <= w);
}

juce::String cell_name(const juce::String& name, bool capitals) {
    if (capitals) return name;
    juce::StringArray words;
    words.addTokens(name.toLowerCase(), " ", "");
    for (auto& word : words) word = word.substring(0, 1).toUpperCase() + word.substring(1);
    return words.joinIntoString(" ");
}

bool cells_in_capitals(const ui::Paint& p, const juce::StringArray& names, const ui::Paint::Text& style, float w) {
    for (const auto& name : names) if (p.width(name, style) > w) return false;
    return true;
}

} // namespace melodee

namespace melodee {
// An engine as a word: Prophet, Sampler; the short ones as they are printed (FM6, SID, CZ, 909).
juce::String engine_word(EngineKind kind) {
    juce::String name = engine_info(kind).name;
    return name.length() > 3 ? name.substring(0, 1) + name.substring(1).toLowerCase() : name;
}

bool source_in_use(const Snapshot& s, InputSource source) { return InputRoute::adc_taken(source, s.ui.adc_needs); }

juce::String input_name(const Snapshot& s, bool with_tap) {
    const auto& in = s.ui.settings.input;
    if (in.source != InputSource::Track) return input_source_name(in.source);
    unsigned t = in.track % track_count;
    juce::String name = "Track " + juce::String(t + 1);
    if (with_tap) return name + (in.post ? " Post" : " Pre"); // the header: the track and its tap
    if (sounding_track(s.project.tracks[t])) name += " " + engine_word(s.project.tracks[t].engine);
    return name;
}

bool record_keys_shown(const Snapshot& s) {
    auto state = TakeRecorder::State(s.take.state);
    return s.ui.page == PageId::Record && internal_source(s.ui.settings.input.source) && s.ui.rec_slot >= 0
        && s.ui.rec_track == s.ui.track
        && (state == TakeRecorder::State::Armed || state == TakeRecorder::State::Cued || state == TakeRecorder::State::Recording);
}
} // namespace melodee
