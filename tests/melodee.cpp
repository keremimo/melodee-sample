// SPDX-License-Identifier: AGPL-3.0-only
// Melodee's model and audio side on the host: pad layouts, knob bindings,
// the PROPHET engine and the engine host's sequencer and mixer.
#include "melodee/engine/effects.h"
#include <fstream>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <map>
#include <unistd.h>
#include "melodee/engine/sample_data.h"
#include "melodee/engine/sampler.h"
#include "melodee/engine/sfz.h"
#include "melodee/engine/keygroup.h"
#include "melodee/engine/exs.h"
#include "melodee/shell/expansion_install.h"
#include "melodee/engine/stream_pager.h"
#include "melodee/shell/sample_cache.h"
#include "melodee/engine/take_recorder.h"
#include "melodee/shell/chop.h"
#include "melodee/engine/engine_host.h"
#include "melodee/engine/cz.h"
#include "melodee/engine/fm6.h"
#include "melodee/engine/kit909.h"
#include "melodee/engine/sid.h"
#include "melodee/engine/prophet.h"
#include "melodee/engine/expression.h"
#include "melodee/engine/punch.h"
#include "melodee/engine/pad_fx_info.h"
#include "melodee/model/mod_matrix.h"
#include "melodee/engine/arp.h"
#include "melodee/model/chords.h"
#include "melodee/model/project_file.h"
#include "melodee/model/scales.h"
#include "melodee/shell/bindings.h"
#include "melodee/shell/settings.h"
#include "melodee/shell/place_bar.h"
#include "melodee/shell/playheads.h"
#include "melodee/engine/voice_tap.h"
#include "melodee/shell/input_choices.h"
#include "melodee/shell/comp_trail.h"
#include "melodee/shell/fader_job.h"
#include "melodee/shell/midi_route.h"
#include "melodee/shell/mtc_in.h"
#include "melodee/shell/midi_config_rows.h"
#include "melodee/engine/midi_out.h"
#include "melodee/engine/midi_engine.h"
#include "melodee/shell/takeover.h"
#include "melodee/model/fader.h"
#include "melodee/engine/effect_info.h"
#include "melodee/shell/history.h"
#include "melodee/shell/name_entry.h"
#include "melodee/shell/name_suggest.h"
#include "melodee/model/sound_file.h"
#include "melodee/shell/sound_list.h"
#include "melodee/shell/button_jobs.h"
#include "melodee/shell/page_stack.h"
#include "melodee/shell/project_kits.h"
#include "melodee/shell/sampler_map.h"
#include "melodee/shell/stage_map.h"
#include "melodee/shell/undo_scope.h"
#include "melodee/shell/roll.h"
#include "melodee/shell/meter_lights.h"
#include "melodee/shell/shift_pad_jobs.h"
#include "melodee/shell/mute_mode.h"
#include "melodee/shell/card_access.h"
#include "melodee/shell/note_repeat.h"
#include "melodee/shell/pad_bank.h"
#include "melodee/shell/step_edit.h"
#include "melodee/model/pad_params.h"
#include "melodee/model/input_config.h"
#include "melodee/model/song_flatten.h"
#include "melodee/engine/recall_ring.h"
#include "melodee/engine/bounce.h"
#include "melodee/engine/looper.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <unistd.h>
#include <memory>
#include <string>
#include <vector>

using namespace melodee;

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

namespace {
float peak_of(const std::vector<float>& v, size_t from = 0, size_t to = SIZE_MAX) {
    float peak = 0;
    for (size_t n = from; n < std::min(to, v.size()); ++n) { CHECK(std::isfinite(v[n])); peak = std::max(peak, std::fabs(v[n])); }
    return peak;
}
// One note on step `step` of the pattern's Q, in place of what the step held.
void put(Pattern& p, unsigned step, uint8_t key, uint8_t velocity, uint8_t length = 1, uint8_t flags = 0) {
    Step s;
    s.notes[0] = {key, velocity};
    s.length = length; s.flags = flags;
    p.set_step(step, s);
}
std::vector<float> run(Engine& engine, unsigned frames) {
    std::vector<float> out(frames, 0.0f);
    for (unsigned done = 0; done < frames; done += 128) engine.render(out.data() + done, std::min(128u, frames - done));
    return out;
}

void layouts() {
    Project project;
    project.root = 2; project.scale = 2; // D Dorian
    Track track;
    track.octave = 2;
    // Pad 1 is D2 (38); sixteen notes of the scale up to E4.
    CHECK(pad_note(project, track, 0).key == 38 && pad_note(project, track, 0).root);
    CHECK(pad_note(project, track, 1).key == 40);
    CHECK(pad_note(project, track, 7).key == 50 && pad_note(project, track, 7).root);
    CHECK(pad_note(project, track, 15).key == 64);
    CHECK(note_name(38) == "D2" && note_name(60) == "C4");
    track.layout = PadLayout::Chromatic;
    CHECK(pad_note(project, track, 5).key == 43);
    track.layout = PadLayout::Levels; track.last_note = 57;
    CHECK(pad_note(project, track, 9).key == 57 && level_velocity(0) < level_velocity(15) && level_velocity(15) == 127);
}

void bindings() {
    Project project;
    load_sound(project.tracks[0], EngineKind::Prophet, 1);
    UiState ui;
    Pattern pattern;
    ui.stage_sub[0] = 1; // Stage's TONE: the favourites
    auto stage = page_bindings(project, ui, pattern);
    CHECK(stage[0].kind == Binding::Kind::Param && stage[0].id == prophet::Cutoff);
    CHECK(stage[0].at(0) == 0 && stage[0].at(127) == 100 && stage[0].at(64) == 50);
    ui.page = PageId::Mix;
    auto mix = page_bindings(project, ui, pattern);
    CHECK(mix[1].kind == Binding::Kind::Pan && mix[1].at(0) == -50 && mix[1].at(127) == 50);
    ui.page = PageId::Steps;
    CHECK(!page_bindings(project, ui, pattern)[0].bound()); // an empty step has nothing on the cards
    put(pattern, 0, 50, 90);
    CHECK(page_bindings(project, ui, pattern)[1].value == 90);
    ui.page = PageId::Edit; ui.edit_page[0] = 4; ui.edit_row[0] = 1;
    CHECK(page_bindings(project, ui, pattern)[0].id == prophet::FilterRelease);
    // Every sound sets values inside its parameters' ranges.
    const auto& info = prophet::info();
    for (unsigned s = 0; s < info.sound_count; ++s) {
        std::array<int16_t, max_params> values;
        sound_values(info, s, values);
        for (unsigned p = 0; p < info.param_count; ++p) CHECK(values[p] >= info.params[p].min && values[p] <= info.params[p].max);
    }
}

void prophet_engine() {
    // Every sound plays a note that sounds, stays finite and ends after release.
    for (unsigned s = 0; s < prophet::info().sound_count; ++s) {
        auto engine = std::make_unique<ProphetEngine>();
        std::array<int16_t, max_params> values;
        sound_values(prophet::info(), s, values);
        for (unsigned p = 0; p < prophet::Count; ++p) engine->set_param(int(p), values[p]);
        engine->note_on(57, 110);
        auto held = run(*engine, 22050);
        CHECK(peak_of(held) > 0.01f);
        CHECK(peak_of(held) < 1.5f);
        engine->note_off(57);
        run(*engine, 44100 * 12); // the longest release is 10 s
        CHECK(!engine->sounding());
    }
    // Eight voices at once; a ninth steals the oldest.
    ProphetEngine poly;
    for (int k = 0; k < 9; ++k) poly.note_on(48 + k, 100);
    CHECK(peak_of(run(poly, 4410)) > 0.01f);
    poly.silence();
    CHECK(!poly.sounding());
}

void host_sequencer() {
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    load_sound(project->tracks[0], EngineKind::Prophet, 1);
    project->tempo = 1200;
    put(bank->at[0][0], 4, 45, 100);
    EngineHost host;
    host.load(*project, *bank);
    std::vector<int32_t> out(256);
    // Stopped: silent.
    host.render(out.data(), 128, 1);
    for (auto s : out) CHECK(s == 0);
    // Playing at 120 BPM a step is 5512.5 samples; step 5 starts at 22050.
    host.send({Command::Type::Play});
    std::vector<float> left;
    for (unsigned n = 0; n < 240; ++n) {
        host.render(out.data(), 128, 1);
        for (unsigned f = 0; f < 128; ++f) left.push_back(out[2 * f] / 2147483648.0f);
    }
    CHECK(host.playing());
    CHECK(peak_of(left, 0, 22000) == 0);
    CHECK(peak_of(left, 22050, 30000) > 0.005f);
    // Mute silences the track's mix but keeps the clock.
    host.send({Command::Type::Mute, 0, 0, 0, 1});
    host.send({Command::Type::Stop});
    for (unsigned n = 0; n < 50; ++n) host.render(out.data(), 128, 1);
    for (auto s : out) CHECK(s == 0);
    CHECK(!host.playing());
    // A step written while running lands in the mirror and plays.
    host.send({Command::Type::Mute, 0, 0, 0, 0});
    Command write {Command::Type::PatternData, 0, 0};
    write.data = new Pattern;
    put(*write.data, 0, 60, 120);
    host.send(write); // the copy it replaces goes back to be freed (here, by the host when it goes)
    host.send({Command::Type::Play});
    float peak = 0;
    for (unsigned n = 0; n < 40; ++n) {
        host.render(out.data(), 128, 1);
        for (auto s : out) peak = std::max(peak, std::fabs(s / 2147483648.0f));
    }
    CHECK(peak > 0.005f);
    CHECK(host.dropped() == 0);
}
// Slide, Tie and Accent as played: a tied note goes on into the next of its key, a slid one overlaps the next
// (a mono synth's legato) and glides there without a new attack, and an accented note comes out louder.
void sequenced_legato() {
    {   // MIDI out, which shows the notes as they go.
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        load_sound(project->tracks[0], EngineKind::Midi, 0);
        project->tracks[0].params[midi_track::Port] = midi_track::UsbC; project->tracks[0].params[midi_track::Channel] = 1;
        project->tempo = 1200;
        auto& p = bank->at[0][0];
        put(p, 0, 60, 100, 1, step_tie); put(p, 1, 60, 100); put(p, 2, 64, 100, 1, step_slide); put(p, 3, 67, 100);
        EngineHost host;
        host.load(*project, *bank);
        host.set_audio_format(44100, 384);
        host.send({Command::Type::Play});
        std::vector<int32_t> out(256);
        std::vector<MidiOutEvent> notes;
        for (unsigned n = 0; n < 230; ++n) { // past step 4 (5512.5 frames a step)
            host.render(out.data(), 128, 1);
            MidiOutEvent e;
            while (host.take_midi(e)) if (e.kind == MidiOutEvent::Kind::NoteOn || e.kind == MidiOutEvent::Kind::NoteOff) notes.push_back(e);
        }
        auto find = [&](MidiOutEvent::Kind kind, uint8_t key) {
            int count = 0; uint64_t at = 0;
            for (auto& e : notes) if (e.kind == kind && e.a == key) { if (!count) at = e.frame; ++count; }
            return std::pair<int, uint64_t>{count, at};
        };
        using K = MidiOutEvent::Kind;
        auto on60 = find(K::NoteOn, 60), off60 = find(K::NoteOff, 60), on64 = find(K::NoteOn, 64), off64 = find(K::NoteOff, 64), on67 = find(K::NoteOn, 67);
        CHECK(on60.first == 1 && off60.first == 1);              // tied onto the same key: one note
        CHECK(off60.second > on60.second + 5512 + 4000);          // held through step 2's note
        CHECK(on64.first == 1 && on67.first == 1 && off64.first == 1);
        CHECK(off64.second > on67.second);                        // the slide overlaps the next note
    }
    {   // A mono Prophet: the slide's next note has no new attack, so it is not quieter at its start than held on.
        auto level = [](uint8_t flags) {
            auto project = std::make_unique<Project>();
            auto bank = std::make_unique<PatternBank>();
            load_sound(project->tracks[0], EngineKind::Prophet, 1);
            project->tempo = 1200;
            put(bank->at[0][0], 0, 57, 80, 1, flags); put(bank->at[0][0], 1, 64, 80);
            EngineHost host;
            host.load(*project, *bank);
            host.send({Command::Type::Play});
            std::vector<int32_t> out(256);
            std::vector<float> left;
            for (unsigned n = 0; n < 80; ++n) {
                host.render(out.data(), 128, 1);
                for (unsigned f = 0; f < 128; ++f) left.push_back(out[2 * f] / 2147483648.0f);
            }
            return std::pair<float, float>{peak_of(left, 0, 5000), peak_of(left, 5050, 5500)}; // the first note; its end, before the second
        };
        auto plain = level(0), accent = level(step_accent), slide = level(step_slide), tie = level(step_tie);
        CHECK(accent.first > plain.first * 1.3f);  // louder, at a velocity with room left or not
        CHECK(slide.second > plain.second * 1.5f); // held across the step's end, not released short
        CHECK(tie.second > plain.second * 1.5f);
    }
}
void motion() {
    // The store: a second write at the same place replaces, clear drops one lane, and it fills at the limit.
    Motion m;
    CHECK(m.write({0, 0, 4, prophet::Cutoff, 10}) && m.write({0, 0, 4, prophet::Cutoff, 20}) && m.count == 1);
    CHECK(m.events[0].value == 20);
    CHECK(m.write({0, 0, 8, prophet::Resonance, 5}) && m.write({1, 0, 8, prophet::Cutoff, 5}));
    m.clear(0, 0, prophet::Cutoff);
    CHECK(m.count == 2 && !m.has(0, 0, prophet::Cutoff) && m.has(0, 0, prophet::Resonance) && m.has(1, 0, prophet::Cutoff));
    Motion full;
    for (unsigned n = 0; n < motion_events; ++n) CHECK(full.write({0, 0, uint16_t(n), 0, 0}));
    CHECK(!full.write({0, 0, 999, 0, 0}) && full.count == motion_events);

    // Played: the cutoff closed by motion from step 0 makes a held line much quieter,
    // and Stop gives the track its own value back.
    auto level = [](bool closed) {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        load_sound(project->tracks[0], EngineKind::Prophet, 0);
        for (unsigned s = 0; s < 16; ++s) put(bank->at[0][0], s, 57, 100);
        if (closed) project->motion.write({0, 0, 0, prophet::Cutoff, 0});
        EngineHost host;
        host.load(*project, *bank);
        host.send({Command::Type::Play});
        std::vector<int32_t> out(256);
        double energy = 0;
        for (unsigned n = 0; n < 200; ++n) {
            host.render(out.data(), 128, 1);
            for (auto v : out) energy += double(v / 2147483648.0f) * (v / 2147483648.0f);
        }
        return energy;
    };
    CHECK(level(true) * 20 < level(false));
}
void mod_matrix() {
    // Aftertouch into the cutoff opens a closed filter while the pad is pressed.
    auto loud = [](float pressure, int amount) {
        ProphetEngine engine;
        engine.set_param(prophet::Cutoff, 10);
        engine.set_param(prophet::EnvAmount, 0);
        engine.set_param(prophet::AtBright, 0); engine.set_param(prophet::AtVib, 0); // pressure through the route only
        engine.set_mod(0, {uint8_t(ModSource::Aftertouch), uint8_t(ModDest::Cutoff), int8_t(amount)});
        engine.note_on(57, 100);
        engine.set_pressure(57, pressure);
        auto out = run(engine, 22050);
        double energy = 0;
        for (auto v : out) energy += double(v) * v;
        return energy;
    };
    CHECK(loud(1, 100) > 4 * loud(0, 100));
    CHECK(std::fabs(loud(1, 0) - loud(0, 0)) < 1e-9 * (loud(0, 0) + 1)); // amount 0 does nothing
    // The Edit pages end with the mod matrix, whose cards bind the route under the wheel.
    const auto& info = prophet::info();
    CHECK(edit_page_count(info) == info.page_count + 1 && is_mod_page(info, info.page_count));
    Project project;
    load_sound(project.tracks[0], EngineKind::Prophet, 0);
    UiState ui;
    ui.page = PageId::Edit; ui.edit_page[0] = uint8_t(info.page_count); ui.edit_row[0] = 2;
    Pattern pattern;
    CHECK(!page_bindings(project, ui, pattern)[0].bound()); // an empty route: Push starts it
    project.tracks[0].mods[2] = {uint8_t(ModSource::Lfo), uint8_t(ModDest::Cutoff), 34};
    auto cards = page_bindings(project, ui, pattern);
    CHECK(cards[0].kind == Binding::Kind::ModSource && cards[2].value == 34 && cards[2].id == 2 && cards[2].min == -100);
}
// Phase 1d: pad expression on every synth (MELODEE_PARITY.md).
std::unique_ptr<Engine> synth(EngineKind kind, std::initializer_list<std::pair<int, int>> changes = {}) {
    std::unique_ptr<Engine> e;
    switch (kind) {
    case EngineKind::Prophet: e = std::make_unique<ProphetEngine>(); break;
    case EngineKind::Fm6: e = std::make_unique<Fm6Engine>(); break;
    case EngineKind::Sid: e = std::make_unique<SidEngine>(); break;
    default: e = std::make_unique<CzEngine>(); break;
    }
    std::array<int16_t, max_params> values {};
    sound_values(engine_info(kind), 0, values);
    for (unsigned n = 0; n < engine_info(kind).param_count; ++n) e->set_param(int(n), values[n]);
    for (auto [id, value] : changes) e->set_param(id, value);
    return e;
}
struct Heard { double energy = 0, bright = 0; }; // bright: the share of energy in the sample-to-sample changes
Heard heard(const std::vector<float>& v, size_t from = 0) {
    Heard h;
    double diff = 0;
    for (size_t n = std::max<size_t>(from, 1); n < v.size(); ++n) { h.energy += double(v[n]) * v[n]; diff += double(v[n] - v[n - 1]) * (v[n] - v[n - 1]); }
    h.bright = diff / (h.energy + 1e-12);
    return h;
}
// A held note, pressed from the start of `frames` at `pressure`.
std::vector<float> held_note(Engine& e, int velocity, float pressure, unsigned frames = 22050) {
    e.note_on(57, velocity);
    if (pressure > 0) e.set_pressure(57, pressure);
    return run(e, frames);
}
void pad_expression() {
    struct Synth { EngineKind kind; int at_vib, at_bright; };
    const Synth synths[] {{EngineKind::Prophet, prophet::AtVib, prophet::AtBright}, {EngineKind::Fm6, fm6::AtVib, fm6::AtBright},
                          {EngineKind::Sid, sid::AtVib, sid::AtBright}, {EngineKind::Cz, cz::AtVib, cz::AtBright}};
    for (const auto& s : synths) {
        const auto& info = engine_info(s.kind);
        // The new parameters sit at the end of the list, so older files' values keep their places.
        CHECK(info.legacy_params && info.legacy_params < info.param_count && s.at_vib >= int(info.legacy_params));
        CHECK(info.params[s.at_vib].initial > 0 && info.params[s.at_bright].initial > 0);
        // Velocity is clearly heard at the defaults: a soft hit well under a hard one (more than 6 dB).
        auto soft = heard(held_note(*synth(s.kind), 40, 0)), hard = heard(held_note(*synth(s.kind), 127, 0));
        CHECK(soft.energy * 4 < hard.energy);
        // Pressure at the defaults: brighter, and the vibrato moves the pitch.
        auto pressed = heard(held_note(*synth(s.kind), 100, 1), 4410), plain = heard(held_note(*synth(s.kind), 100, 0), 4410);
        CHECK(pressed.bright > plain.bright * 1.15);
        auto vib = held_note(*synth(s.kind, {{s.at_bright, 0}, {s.at_vib, 100}}), 100, 1);
        auto still = held_note(*synth(s.kind, {{s.at_bright, 0}, {s.at_vib, 100}}), 100, 0);
        double apart = 0, total = 0;
        for (size_t n = 11025; n < vib.size(); ++n) { apart += double(vib[n] - still[n]) * (vib[n] - still[n]); total += double(still[n]) * still[n]; }
        CHECK(apart > total * 0.1);
        // Both at 0: pressure does nothing.
        auto off = held_note(*synth(s.kind, {{s.at_bright, 0}, {s.at_vib, 0}}), 100, 1);
        auto none = held_note(*synth(s.kind, {{s.at_bright, 0}, {s.at_vib, 0}}), 100, 0);
        for (size_t n = 0; n < off.size(); ++n) CHECK(off[n] == none[n]);
        // The mod matrix is heard: aftertouch into the engine's brightness (cutoff, modulators, DCW).
        auto route = [&](float pressure) {
            auto e = synth(s.kind, {{s.at_bright, 0}, {s.at_vib, 0}});
            e->set_mod(3, {uint8_t(ModSource::Aftertouch), uint8_t(ModDest::Cutoff), 100});
            return heard(held_note(*e, 100, pressure), 4410);
        };
        CHECK(route(1).bright > route(0).bright * 1.15);
        // An LFO route into pitch moves the pitch with no pressure.
        auto lfo = synth(s.kind, {{s.at_bright, 0}, {s.at_vib, 0}});
        lfo->set_mod(0, {uint8_t(ModSource::Lfo), uint8_t(ModDest::Pitch), 50});
        auto wobble = held_note(*lfo, 100, 0);
        double moved = 0;
        for (size_t n = 11025; n < wobble.size(); ++n) moved += double(wobble[n] - none[n]) * (wobble[n] - none[n]);
        CHECK(moved > total * 0.1);
        // Every factory sound's velocity and pressure stay bounded.
        for (unsigned k = 0; k < info.sound_count; ++k) {
            auto e = synth(s.kind);
            std::array<int16_t, max_params> values {};
            sound_values(info, k, values);
            for (unsigned n = 0; n < info.param_count; ++n) e->set_param(int(n), values[n]);
            CHECK(peak_of(held_note(*e, 127, 1, 44100)) < 2.0f);
        }
    }
    // Destinations an engine has none of are not offered, and the others carry its own names.
    CHECK(!mod_dest_label(engine_info(EngineKind::Cz), ModDest::Resonance));
    CHECK(std::string(mod_dest_label(engine_info(EngineKind::Cz), ModDest::Cutoff)) == "DCW");
    CHECK(std::string(mod_dest_label(engine_info(EngineKind::Prophet), ModDest::Noise)) == "Noise");
    std::array<uint8_t, size_t(ModDest::Count)> dests {};
    CHECK(mod_dest_list(engine_info(EngineKind::Prophet), dests) == unsigned(ModDest::Count));
    CHECK(mod_dest_list(engine_info(EngineKind::Cz), dests) == 7);
    // Older files: the new parameters get their initial values, not the zeros the file carried.
    std::array<int16_t, max_params> old {};
    sound_values(engine_info(EngineKind::Sid), 0, old);
    for (unsigned n = sid::legacy_count; n < max_params; ++n) old[n] = 0;
    upgrade_params(EngineKind::Sid, 0, old);
    CHECK(old[sid::Velocity] == 70 && old[sid::AtVib] == expression::default_at_vib && old[sid::LfoRate] == expression::default_vib_rate);
    old[sid::AtVib] = 0;
    upgrade_params(EngineKind::Sid, params_version, old); // a newer file's 0 is a 0
    CHECK(old[sid::AtVib] == 0);
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    load_sound(project->tracks[2], EngineKind::Cz, 0);
    project->tracks[2].params[cz::AtVib] = 0;
    auto bytes = save_project(*project, *bank);
    unsigned version = 9;
    auto back = std::make_unique<Project>();
    CHECK(load_project(bytes, *back, *bank, &version).empty() && version == params_version && back->tracks[2].params[cz::AtVib] == 0);
    UserSound us;
    us.engine = EngineKind::Cz;
    auto sound_bytes = save_sound(us);
    CHECK(load_sound_file(sound_bytes, us, &version) && version == params_version);
    sound_bytes.pop_back(); // as written before phase 1d
    CHECK(load_sound_file(sound_bytes, us, &version) && version == 0);
    // The arp's notes follow the pressure of the pad holding their key, an octave up too.
    auto arp_bright = [](int pressure) {
        auto p = std::make_unique<Project>();
        auto b = std::make_unique<PatternBank>();
        load_sound(p->tracks[0], EngineKind::Prophet, 0);
        p->tracks[0].params[prophet::AtVib] = 0;
        p->tracks[0].params[prophet::AtBright] = 100;
        p->tracks[0].params[prophet::Cutoff] = 30;
        EngineHost host;
        host.load(*p, *b);
        host.send({Command::Type::Arp, 0, 0, 0, 1});
        host.send({Command::Type::Arp, 0, 0, 3, 2}); // two octaves: notes the pad never played
        host.send({Command::Type::ArpKey, 0, 0, 48, 100});
        host.send({Command::Type::Pressure, 0, 0, 48, int16_t(pressure)});
        std::vector<int32_t> out(256);
        std::vector<float> mono;
        for (unsigned n = 0; n < 400; ++n) { host.render(out.data(), 128, 1); for (unsigned f = 0; f < 128; ++f) mono.push_back(out[2 * f] / 2147483648.0f); }
        return heard(mono, 4410).bright;
    };
    CHECK(arp_bright(127) > arp_bright(0) * 1.15);
    // Settings: Pad pressure on by default, kept in the settings file.
    Settings set;
    CHECK(set.pressure && set.value_text(Settings::PadPressure) == "On");
    set.step(Settings::PadPressure, 1);
    Settings read_back;
    read_back.read(set.text());
    CHECK(!read_back.pressure);
}
void arrange() {
    // The song store: placing a clip cuts what it covers; splits keep both sides.
    Song song;
    CHECK(song.place({0, 0, 0, 8, false}) && song.place({0, 1, 2, 2, false}));
    CHECK(song.count == 3 && song.clip_at(0, 1) >= 0 && song.clips[size_t(song.clip_at(0, 3))].pattern == 1);
    CHECK(song.clips[size_t(song.clip_at(0, 5))].pattern == 0 && song.clips[size_t(song.clip_at(0, 5))].bar == 4);
    CHECK(song.clip_at(1, 0) < 0 && song.length() == 8);
    song.erase(unsigned(song.clip_at(0, 3)));
    CHECK(song.clip_at(0, 3) < 0 && song.count == 2);
    // Markers: added at a bar, named after the one before, stepped through the names, then gone.
    CHECK(song.step_marker(4) && song.marker_count == 1 && std::string(song.markers[0].name) == "INTRO");
    CHECK(song.step_marker(12) && std::string(song.markers[size_t(song.marker_at(12))].name) == "VERSE");
    CHECK(song.step_marker(4) && std::string(song.markers[size_t(song.marker_at(4))].name) == "VERSE");
    for (unsigned n = 0; n < Song::marker_name_count - 2; ++n) CHECK(song.step_marker(4));
    CHECK(std::string(song.markers[size_t(song.marker_at(4))].name) == "OUTRO");
    CHECK(song.step_marker(4) && song.marker_at(4) < 0 && song.marker_count == 1 && song.marker_at(12) >= 0);

    auto setup = [](Project& project, PatternBank& bank) {
        load_sound(project.tracks[0], EngineKind::Prophet, 0);
        project.tempo = 1200;
        put(bank.at[0][0], 0, 57, 100); // pattern 1: a note on its first step only
        put(bank.at[0][1], 0, 69, 100);
    };
    // One bar at 120 BPM is 88200 samples; render `bars` and say in which bars track 1 sounded.
    auto sounding = [](EngineHost& host, unsigned bars) {
        std::vector<bool> out;
        std::vector<int32_t> block(256);
        for (unsigned b = 0; b < bars; ++b) {
            float peak = 0;
            for (unsigned n = 0; n < 88200 / 128; ++n) {
                host.render(block.data(), 128, 1);
                for (auto v : block) peak = std::max(peak, std::fabs(v / 2147483648.0f));
            }
            out.push_back(peak > 0.002f);
        }
        return out;
    };
    // The song: track 1 has a clip in bar 2 only.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        setup(*project, *bank);
        project->tracks[0].params[prophet::AmpRelease] = 0;
        project->song.place({0, 0, 1, 1, false});
        EngineHost host;
        host.load(*project, *bank);
        host.send({Command::Type::Play, 0, 0, 0, 1});
        auto bars = sounding(host, 3);
        CHECK(!bars[0] && bars[1]);
        CHECK(!host.playing()); // past the song's end
    }
    // Patterns: a launch waits for the next bar; the position counts ticks (96 a quarter, 229.7 samples each at 120 BPM).
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        setup(*project, *bank);
        EngineHost host;
        host.load(*project, *bank);
        host.send({Command::Type::Play});
        std::vector<int32_t> block(256);
        for (unsigned n = 0; n < 80; ++n) host.render(block.data(), 128, 1); // 10240 samples: tick 44
        CHECK(host.position_of(0) == 44 && host.pattern_of(0) == 0);
        for (unsigned n = 0; n < 10; ++n) host.render(block.data(), 128, 1); // 11520: tick 50
        CHECK(host.position_of(0) == 50);
        host.send({Command::Type::Pattern, 0, 0, 0, 1});
        host.render(block.data(), 128, 1);
        CHECK(host.pattern_of(0) == 0); // still the old one until the bar ends
        for (unsigned n = 0; n < 88200 / 128; ++n) host.render(block.data(), 128, 1);
        CHECK(host.pattern_of(0) == 1);
    }
}
void effects() {
    // Every built effect stays finite and bounded on a loud chord, at its initial values and at the extremes.
    DelayPool pool;
    for (unsigned k = 1; k < unsigned(EffectKind::Count); ++k) {
        auto kind = EffectKind(k);
        if (!effect_info(kind).built) continue;
        for (int extreme = 0; extreme < 3; ++extreme) {
            Effect effect;
            effect.set_kind(kind, pool);
            if (extreme)
                for (unsigned q = 0; q < effect_params; ++q) {
                    const auto& pi = effect_info(kind).params[q];
                    if (pi.label) effect.set_param(q, extreme == 1 ? pi.min : pi.max);
                }
            std::vector<float> l(128), r(128);
            float peak = 0;
            for (unsigned b = 0; b < 400; ++b) {
                for (unsigned f = 0; f < 128; ++f) {
                    float t = float(b * 128 + f) / 44100.0f;
                    l[f] = 0.8f * std::sin(6.283f * 220 * t) + 0.2f * std::sin(6.283f * 331 * t);
                    r[f] = 0.8f * std::sin(6.283f * 277 * t);
                }
                effect.process(l.data(), r.data(), 128);
                for (unsigned f = 0; f < 128; ++f) { CHECK(std::isfinite(l[f]) && std::isfinite(r[f])); peak = std::max({peak, std::fabs(l[f]), std::fabs(r[f])}); }
            }
            CHECK(peak < 20); // a resonant filter or makeup gain may rise, but nothing runs away
            effect.set_kind(EffectKind::None, pool); // gives its delay memory back
        }
    }
    // Looper: what is recorded on one pass plays on the next with the input gone.
    {
        Effect looper;
        looper.set_kind(EffectKind::Looper, pool);
        looper.set_param(1, 1); // record
        unsigned length = unsigned(time_seconds(70) * 44100);
        std::vector<float> l(1), r(1);
        for (unsigned n = 0; n < length; ++n) { l[0] = r[0] = n < 2000 ? 0.5f : 0; looper.process(l.data(), r.data(), 1); }
        looper.set_param(1, 0);
        float heard = 0;
        for (unsigned n = 0; n < 2000; ++n) { l[0] = r[0] = 0; looper.process(l.data(), r.data(), 1); heard = std::max(heard, std::fabs(l[0])); }
        CHECK(heard > 0.4f);
        looper.set_kind(EffectKind::None, pool);
    }
    // Formant: an 800 Hz tone sits on A's first formant and between I's (280 and 2200 Hz).
    {
        auto level = [&](int vowel) {
            Effect formant;
            formant.set_kind(EffectKind::Formant, pool);
            formant.set_param(0, vowel);
            std::vector<float> l(128), r(128);
            float peak = 0;
            for (unsigned b = 0; b < 100; ++b) {
                for (unsigned f = 0; f < 128; ++f) l[f] = r[f] = 0.3f * std::sin(6.2831853f * 800 * float(b * 128 + f) / 44100.0f);
                formant.process(l.data(), r.data(), 128);
                if (b > 50) for (unsigned f = 0; f < 128; ++f) peak = std::max(peak, std::fabs(l[f]));
            }
            return peak;
        };
        CHECK(level(0) > level(50) * 2);
    }
    // When the pool is used up, a further delay effect passes sound through untouched.
    std::vector<Effect> many(DelayPool::blocks + 1);
    for (auto& e : many) e.set_kind(EffectKind::Echo, pool);
    std::vector<float> l(128, 0.5f), r(128, 0.5f);
    many.back().process(l.data(), r.data(), 128);
    CHECK(l[0] == 0.5f && r[127] == 0.5f);

    // Along the signal: a track's low-pass quietens it; its send into an echo bus keeps sounding after the note.
    auto render = [](bool filter, bool echo) {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        load_sound(project->tracks[0], EngineKind::Prophet, 0);
        project->tracks[0].params[prophet::AmpRelease] = 0;
        if (filter) { project->tracks[0].fx.slots[0].kind = uint8_t(EffectKind::Filter); project->tracks[0].fx.slots[0].params = {5, 0, 0, 0, 0, 0}; }
        if (echo) {
            project->tracks[0].mix.send = 100;
            project->buses[0].fx.slots[0].kind = uint8_t(EffectKind::Echo);
            project->buses[0].fx.slots[0].params = {60, 50, 100, 80, 0, 0};
        }
        EngineHost host;
        host.load(*project, *bank);
        host.send({Command::Type::NoteOn, 0, 0, 57, 110});
        std::vector<int32_t> out(256);
        double held = 0, after = 0;
        for (unsigned n = 0; n < 700; ++n) {
            if (n == 172) host.send({Command::Type::NoteOff, 0, 0, 57, 0}); // half a second in
            host.render(out.data(), 128, 1);
            for (auto v : out) (n < 172 ? held : n > 200 ? after : held) += double(v / 2147483648.0f) * (v / 2147483648.0f);
        }
        return std::pair<double, double>(held, after);
    };
    auto dry = render(false, false), filtered = render(true, false), echoed = render(false, true);
    CHECK(filtered.first * 10 < dry.first);
    CHECK(dry.second < 1e-6 && echoed.second > 1e-3);
}
void put32(std::ofstream& f, uint32_t v) { for (int n = 0; n < 4; ++n) f.put(char(v >> (8 * n))); }
void put16(std::ofstream& f, uint16_t v) { f.put(char(v)); f.put(char(v >> 8)); }

void sampler_kit() {
    // A 16-bit stereo WAV at 22.05 kHz: four clicks a quarter second apart, read back at 44.1 kHz.
    std::string path = "/tmp/melodee-test-" + std::to_string(::getpid()) + ".wav";
    {
        std::ofstream f(path, std::ios::binary);
        unsigned frames = 22050;
        f.write("RIFF", 4); put32(f, 36 + frames * 4); f.write("WAVEfmt ", 8); put32(f, 16); put16(f, 1); put16(f, 2);
        put32(f, 22050); put32(f, 22050 * 4); put16(f, 4); put16(f, 16); f.write("data", 4); put32(f, frames * 4);
        for (unsigned n = 0; n < frames; ++n) {
            unsigned at = n % 5512;
            int16_t v = int16_t(at < 400 ? 20000 * std::exp(-double(at) / 80.0) * std::sin(n * 0.3) : 0);
            put16(f, uint16_t(v)); put16(f, uint16_t(v));
        }
    }
    SampleData data;
    CHECK(read_wav(path, data).empty());
    std::remove(path.c_str());
    CHECK(data.frames == 44100 && data.left.size() == 44100);
    CHECK(!read_wav("/nonexistent.wav", data).empty());
    Overview overview;
    make_overview(data, overview);
    if (!(overview[0] > 50 && overview[60] == 0)) std::fprintf(stderr, "overview %d %d frames %u\n", overview[0], overview[60], data.frames);
    CHECK(overview[0] > 50 && overview[60] == 0);
    std::array<Hit, max_hits> hits {};
    unsigned found = find_hits(data, hits);
    CHECK(found >= 4 && hits[0].frame == 0);
    // Chop: hits at the clicks, even parts, beats at 120 BPM (22050 frames each).
    auto even = chop(ChopMode::Regions, 4, data.frames, hits, found, 1200);
    CHECK(even.count == 4 && even.start[2] == 22050);
    auto beats = chop(ChopMode::Beats, 1, data.frames, hits, found, 1200);
    CHECK(beats.count == 2 && beats.start[1] == 22050);
    auto clicks = chop(ChopMode::Threshold, 0, data.frames, hits, found, 1200);
    CHECK(clicks.count >= 4);
    for (unsigned n = 1; n < 4; ++n) CHECK(std::abs(int(clicks.start[n]) - int(n * 11024)) < 600);
    Slices edited = even;
    CHECK(edited.add(1000, data.frames) && edited.count == 5 && edited.start[1] == 1000);
    edited.remove(1);
    CHECK(edited.count == 4 && edited.start[1] == 11025);

    // The engine: a one shot plays to its region's end after the pad lets go; a gate stops.
    SamplerEngine::Table table {};
    table[3] = &data;
    auto run_pad = [&](PadPlay play, bool release) {
        SamplerEngine engine;
        engine.set_samples(&table);
        PadSound pad; pad.sample = 3; pad.start = 0; pad.end = 4410; pad.play = play;
        engine.set_pad(5, pad);
        engine.note_on(int(sampler_first_note) + 5, 120);
        std::vector<float> l(128, 0), r(128, 0);
        float peak = 0;
        for (unsigned b = 0; b < 40; ++b) {
            if (release && b == 1) engine.note_off(int(sampler_first_note) + 5);
            std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
            engine.render_stereo(l.data(), r.data(), 128);
            if (b > 10) for (unsigned f = 0; f < 128; ++f) peak = std::max(peak, std::fabs(l[f]));
        }
        return std::pair<float, bool>(peak, engine.sounding());
    };
    auto one_shot = run_pad(PadPlay::OneShot, true), gate = run_pad(PadPlay::Gate, true);
    CHECK(!one_shot.second);           // done by its end (4410 frames, ~34 blocks)
    CHECK(gate.first < 0.02f);         // released early, so its later blocks are near silent
    // A mute group: the second pad cuts the first.
    SamplerEngine engine;
    engine.set_samples(&table);
    PadSound a; a.sample = 3; a.group = 1; a.play = PadPlay::OneShot;
    engine.set_pad(0, a); engine.set_pad(1, a);
    engine.note_on(int(sampler_first_note), 100);
    engine.note_on(int(sampler_first_note) + 1, 100);
    std::vector<float> l(128, 0), r(128, 0);
    for (unsigned b = 0; b < 2; ++b) engine.render_stereo(l.data(), r.data(), 128);
    CHECK(engine.sounding());
    engine.sample_gone(3); // a sample about to be replaced: everything playing it stops
    CHECK(!engine.sounding());

    // Cloud: a held pad becomes grains, and sounds while held.
    {
        SamplerEngine cloud;
        cloud.set_samples(&table);
        PadSound pad; pad.sample = 3; pad.play = PadPlay::Gate;
        cloud.set_pad(2, pad);
        cloud.set_param(sampler::Cloud, 1); cloud.set_param(sampler::Density, 60);
        cloud.note_on(int(sampler_first_note) + 2, 120);
        std::vector<float> l(128, 0), r(128, 0);
        float peak = 0;
        for (unsigned b = 0; b < 60; ++b) {
            std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
            cloud.render_stereo(l.data(), r.data(), 128);
            for (auto v : l) { CHECK(std::isfinite(v)); peak = std::max(peak, std::fabs(v)); }
        }
        CHECK(peak > 0.005f);
        cloud.note_off(int(sampler_first_note) + 2);
        for (unsigned b = 0; b < 400; ++b) cloud.render_stereo(l.data(), r.data(), 128);
        CHECK(!cloud.sounding());
    }
    // A one shot's cloud: one press sounds about as long as the pad plays, then stops, without a release.
    {
        SamplerEngine cloud;
        cloud.set_samples(&table);
        PadSound pad; pad.sample = 3; pad.start = 0; pad.end = 4410; pad.play = PadPlay::OneShot;
        cloud.set_pad(2, pad);
        cloud.set_param(sampler::Cloud, 1); cloud.set_param(sampler::Density, 60);
        cloud.note_on(int(sampler_first_note) + 2, 120);
        cloud.note_off(int(sampler_first_note) + 2);
        std::vector<float> l(128, 0), r(128, 0);
        cloud.render_stereo(l.data(), r.data(), 128);
        CHECK(cloud.sounding());
        for (unsigned b = 0; b < 400; ++b) cloud.render_stereo(l.data(), r.data(), 128);
        CHECK(!cloud.sounding());
    }
    // Level: a centred pad plays -3 dB a side (constant power, as the stock MPC measured over USB);
    // hard left is the whole sample on the left. Mono and stereo files alike.
    {
        SampleData flat;
        flat.frames = 44100;
        flat.left.assign(flat.frames, 0.5f); flat.right.assign(flat.frames, 0.5f);
        SamplerEngine::Table level_table {};
        level_table[0] = &flat;
        auto sides = [&](int pan) {
            SamplerEngine e;
            e.set_samples(&level_table);
            PadSound pad; pad.sample = 0; pad.end = flat.frames; pad.pan = int8_t(pan);
            e.set_pad(0, pad);
            e.note_on(int(sampler_first_note), 127);
            std::vector<float> l(128, 0), r(128, 0);
            for (unsigned b = 0; b < 20; ++b) {
                std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
                e.render_stereo(l.data(), r.data(), 128);
            }
            return std::pair<float, float>(l[64], r[64]);
        };
        auto centre = sides(0), left = sides(-50);
        CHECK(std::fabs(centre.first - 0.5f * 0.7071f) < 0.005f && std::fabs(centre.second - 0.5f * 0.7071f) < 0.005f);
        CHECK(std::fabs(left.first - 0.5f) < 0.005f && std::fabs(left.second) < 0.005f);
    }
    // Follow: a pad played at 120 BPM in a 60 BPM project lasts twice as long.
    {
        auto length = [&](bool follow) {
            SamplerEngine e;
            e.set_samples(&table);
            PadSound pad; pad.sample = 3; pad.end = 4410; pad.own_tempo = 1200;
            e.set_pad(0, pad);
            e.set_tempo(600);
            e.set_param(sampler::Follow, follow);
            e.note_on(int(sampler_first_note), 100);
            std::vector<float> l(128, 0), r(128, 0);
            unsigned blocks = 0;
            while (e.sounding() && blocks < 1000) { e.render_stereo(l.data(), r.data(), 128); ++blocks; }
            return blocks;
        };
        unsigned plain = length(false), followed = length(true);
        CHECK(followed > plain * 18 / 10 && followed < plain * 22 / 10);
    }
    // PadFX: a pad with a chain plays through it; a pad without one is untouched.
    {
        DelayPool pool;
        auto effects = std::make_unique<SamplerEngine::PadEffects>();
        (*effects)[5][0].set_kind(EffectKind::Crush, pool);
        (*effects)[5][0].set_param(0, 2); // two bits
        auto play = [&](unsigned pad, bool chained) {
            SamplerEngine engine;
            engine.set_samples(&table);
            if (chained) engine.set_pad_effects(effects.get());
            PadSound sound; sound.sample = 3; sound.end = 4410;
            engine.set_pad(pad, sound);
            engine.note_on(int(sampler_first_note + pad), 120);
            std::vector<float> l(128), r(128), all;
            for (unsigned b = 0; b < 30; ++b) {
                std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
                engine.render_stereo(l.data(), r.data(), 128);
                all.insert(all.end(), l.begin(), l.end());
            }
            return all;
        };
        auto dry = play(5, false), wet = play(5, true), other = play(6, true), other_dry = play(6, false);
        float difference = 0;
        for (size_t n = 0; n < dry.size(); ++n) difference = std::max(difference, std::fabs(dry[n] - wet[n]));
        CHECK(difference > 0.01f);
        CHECK(other == other_dry);
    }
    // Follow: a 441 Hz tone recorded at 60 BPM, played at 120. By Beats or
    // Tones it lasts half as long at the same pitch; by Tape it also rises
    // an octave. PITCH Stretch keeps an octave up to the tone's own length.
    {
        auto tone = std::make_unique<SampleData>();
        tone->frames = 44100;
        tone->left.resize(44100); tone->right.resize(44100);
        for (unsigned n = 0; n < 44100; ++n) tone->left[n] = tone->right[n] = 0.5f * std::sin(2 * 3.14159265 * 441 * n / 44100.0);
        SamplerEngine::Table tones {};
        tones[0] = tone.get();
        struct Heard { unsigned frames; float hz; };
        auto hear = [&](int how, int pitch_how, int follow, int semitones, int warp = warp_off) {
            SamplerEngine engine;
            engine.set_samples(&tones);
            engine.set_tempo(1200);
            engine.set_param(sampler::Follow, follow); engine.set_param(sampler::FollowHow, how); engine.set_param(sampler::PitchHow, pitch_how);
            PadSound sound; sound.sample = 0; sound.own_tempo = 600; sound.pitch = int8_t(semitones); sound.decay = 100; sound.warp = uint8_t(warp);
            engine.set_pad(0, sound);
            engine.note_on(int(sampler_first_note), 120);
            std::vector<float> l(128), r(128), all;
            for (unsigned b = 0; b < 1000 && engine.sounding(); ++b) {
                std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
                engine.render_stereo(l.data(), r.data(), 128);
                all.insert(all.end(), l.begin(), l.end());
            }
            unsigned crossings = 0, from = 2000, to = std::min<unsigned>(unsigned(all.size()), 12000);
            for (unsigned n = from + 1; n < to; ++n) crossings += (all[n - 1] < 0) != (all[n] < 0);
            return Heard {unsigned(all.size()), crossings / 2.0f / ((to - from) / 44100.0f)};
        };
        for (int how : {0, 1}) {
            auto h = hear(how, 0, 1, 0);
            if (!(std::abs(int(h.frames) - 22050) < 600 && std::fabs(h.hz - 441) < 20)) std::fprintf(stderr, "how %d: %u frames %f Hz\n", how, h.frames, h.hz);
            CHECK(std::abs(int(h.frames) - 22050) < 600 && std::fabs(h.hz - 441) < 20);
        }
        auto tape = hear(2, 0, 1, 0);
        CHECK(std::abs(int(tape.frames) - 22050) < 600 && std::fabs(tape.hz - 882) < 30);
        auto up = hear(1, 1, 0, 12); // no follow, an octave up, kept to length
        if (!(std::abs(int(up.frames) - 44100) < 600 && std::fabs(up.hz - 882) < 30)) std::fprintf(stderr, "up: %u frames %f Hz\n", up.frames, up.hz);
        CHECK(std::abs(int(up.frames) - 44100) < 600 && std::fabs(up.hz - 882) < 30);
        auto plain = hear(1, 0, 0, 12); // an octave up by speed: half as long
        CHECK(std::abs(int(plain.frames) - 22050) < 600 && std::fabs(plain.hz - 882) < 30);
        // Warp's mode. Warp on by Time stretch holds a note to Warp's length;
        // by Pitch the pad's Semi is out and Warp moves pitch with length;
        // Warp Off plays by speed in either mode.
        auto heard = [&](const char* name, Heard h, unsigned frames, float hz) {
            bool ok = std::abs(int(h.frames) - int(frames)) < 600 && std::fabs(h.hz - hz) < 30;
            if (!ok) std::fprintf(stderr, "warp %s: %u frames %f Hz\n", name, h.frames, h.hz);
            CHECK(ok);
        };
        for (int how : {0, 1}) {
            heard("stretch 100 up", hear(how, 0, 0, 12, 100), 44100, 882);
            heard("stretch 200 up", hear(how, 0, 0, 12, 200), 88200, 882);
            heard("stretch 50 same", hear(how, 0, 0, 0, 50), 22050, 441);
            heard("off up", hear(how, 0, 0, 12), 22050, 882);
        }
        heard("pitch 100 semi out", hear(2, 0, 0, 12, 100), 44100, 441);
        heard("pitch 50", hear(2, 0, 0, 12, 50), 22050, 882);
        heard("pitch off up", hear(2, 0, 0, 12), 22050, 882);
    }
    // Punches written into the song play where they were written, for as long, and survive a save.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        project->song.place({0, 0, 0, 4, false});
        project->song.add_punch({1, 4, 2, uint8_t(PunchKind::Gate), 255, 100});
        auto bytes = save_project(*project, *bank);
        auto back = std::make_unique<Project>();
        CHECK(load_project(bytes, *back, *bank).empty());
        CHECK(back->song.punch_count == 1 && back->song.punches[0].bar == 1 && back->song.punches[0].step == 4 && back->song.punches[0].length == 2);
        EngineHost host;
        host.load(*back, *bank);
        host.send({Command::Type::Play, 0, 0, 0, 1});
        std::vector<int32_t> out(256);
        // At 120 BPM a sixteenth is 5512.5 samples: the punch runs from 22050 * 4 + 4 * 5512.5 for two sixteenths.
        auto at = [&](double sample) {
            static double done = 0;
            while (done < sample) { host.render(out.data(), 128, 1); done += 128; }
            return host.punching();
        };
        double start = 88200 + 4 * 5512.5;
        CHECK(!at(start - 2000));
        CHECK(at(start + 2000));
        CHECK(at(start + 2 * 5512.5 - 1000));
        CHECK(!at(start + 2 * 5512.5 + 1000));
    }
    // Seq Record: a song playing on past its end while recording.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        project->song.place({0, 0, 0, 1, false});
        EngineHost host;
        host.load(*project, *bank);
        host.send({Command::Type::SongRecord, 0, 0, 0, 1});
        host.send({Command::Type::Play, 0, 0, 0, 1});
        std::vector<int32_t> out(256);
        for (unsigned b = 0; b < 3 * 88200 / 128; ++b) host.render(out.data(), 128, 1);
        CHECK(host.playing() && host.song_bar() >= 2);
        host.send({Command::Type::SongRecord, 0, 0, 0, 0});
        for (unsigned b = 0; b < 2 * 88200 / 128; ++b) host.render(out.data(), 128, 1);
        CHECK(!host.playing());
    }
    // Motion on an effect: the track's filter shut at step 8 by a written event, open again from step 0.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        load_sound(project->tracks[0], EngineKind::Prophet, 1);
        put(bank->at[0][0], 0, 45, 110, 16);
        auto& slot = project->tracks[0].fx.slots[0];
        slot.kind = uint8_t(EffectKind::Filter);
        const auto& info = effect_info(EffectKind::Filter);
        for (unsigned q = 0; q < effect_params; ++q) slot.params[q] = info.params[q].label ? info.params[q].initial : 0;
        slot.params[0] = 100;
        project->motion.write({0, 0, 8, uint8_t(motion_fx_param(0, 0)), 0});
        CHECK(motion_param(project->tracks[0], int(motion_fx_param(0, 0))).info == &info.params[0]);
        CHECK(!motion_param(project->tracks[0], int(motion_fx_param(1, 0))).info); // no effect in slot 2
        UiState ui;
        auto lanes = motion_lanes(*project, ui);
        CHECK(lanes[0] == int(motion_fx_param(0, 0)));
        EngineHost host;
        host.load(*project, *bank);
        host.send({Command::Type::Play});
        std::vector<int32_t> out(256);
        auto level = [&](unsigned blocks) {
            float peak = 0;
            for (unsigned b = 0; b < blocks; ++b) { host.render(out.data(), 128, 1); for (auto v : out) peak = std::max(peak, std::fabs(v / 2147483648.0f)); }
            return peak;
        };
        level(100);                    // into the first half bar (a step is 5512.5 samples)
        float open = level(100);       // steps 2-4
        level(150);                    // past step 8
        float shut = level(100);
        CHECK(open > 0.01f && shut < open * 0.2f);
    }
    // Transposed pads: a track's semitones move its pads and survive a save.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        auto& t = project->tracks[1];
        load_sound(t, EngineKind::Prophet, 0);
        int before = pad_note(*project, t, 0).key;
        t.transpose = 3;
        CHECK(pad_note(*project, t, 0).key == before + 3);
        auto bytes = save_project(*project, *bank);
        auto back = std::make_unique<Project>();
        CHECK(load_project(bytes, *back, *bank).empty() && back->tracks[1].transpose == 3);
    }
    // Pad chains survive a save, four slots each.
    {
        auto project = std::make_unique<Project>();
        auto patterns = std::make_unique<PatternBank>();
        auto& chain = project->tracks[4].pad_fx.at[17];
        CHECK(chain.size == pad_slots && project->chain(pad_chain(4, 17)).size == pad_slots);
        chain.slots[0].kind = uint8_t(EffectKind::Drive); chain.slots[0].params[0] = 77;
        chain.slots[1].kind = uint8_t(EffectKind::Room); chain.slots[1].bypass = true;
        auto bytes = save_project(*project, *patterns);
        auto back = std::make_unique<Project>();
        CHECK(load_project(bytes, *back, *patterns).empty());
        const auto& loaded = back->tracks[4].pad_fx.at[17];
        CHECK(loaded.size == pad_slots && loaded.count() == 2 && loaded.slots[0].params[0] == 77 && loaded.slots[1].bypass);
        CHECK(back->tracks[4].pad_fx.at[16].count() == 0);
    }
    // The host takes a sample from the service side and gives the old one back.
    EngineHost host;
    SampleData other;
    CHECK(host.offer_sample(3, &data) && host.offer_sample(3, &other));
    std::vector<int32_t> out(256);
    host.render(out.data(), 128, 1);
    const SampleData* back = nullptr;
    CHECK(host.take_retired(back) && back == &data && !host.take_retired(back));
}
void recording() {
    // A take waits for the level, records its length, and is held until released.
    auto recorder = std::make_unique<TakeRecorder>();
    std::vector<float> quiet(128, 0.001f), loud(128, 0.5f);
    recorder->arm(-30, 1000, 7);
    recorder->process(quiet.data(), quiet.data(), 128);
    CHECK(recorder->state() == TakeRecorder::State::Armed && recorder->frames() == 0);
    for (unsigned n = 0; n < 10; ++n) recorder->process(loud.data(), loud.data(), 128);
    CHECK(recorder->state() == TakeRecorder::State::Done && recorder->frames() == 1000 && recorder->slot() == 7);
    CHECK(std::fabs(recorder->left()[999] - 0.5f) < 1e-6f && recorder->peak(127) > 100);
    recorder->arm(-30, 1000, 1); // held: a new take waits for release
    CHECK(recorder->frames() == 1000);
    recorder->release();
    CHECK(recorder->state() == TakeRecorder::State::Idle);
    // At once (-96 dB), stopped early (the engine host applies the gain before it).
    recorder->arm(-96, 0, 2);
    CHECK(recorder->state() == TakeRecorder::State::Recording);
    recorder->process(quiet.data(), quiet.data(), 128);
    recorder->stop();
    CHECK(recorder->state() == TakeRecorder::State::Done && recorder->frames() == 128 && std::fabs(recorder->left()[0] - 0.001f) < 1e-6f);
    recorder->release();
    // The host records the mix (resample) for a bar at 120 BPM.
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    load_sound(project->tracks[0], EngineKind::Prophet, 0);
    EngineHost host;
    host.load(*project, *bank);
    host.send({Command::Type::RecordArm, uint8_t(InputSource::Resample), 1, 5, -96});
    host.send({Command::Type::NoteOn, 0, 0, 57, 100});
    std::vector<int32_t> out(256);
    for (unsigned n = 0; n < 700; ++n) host.render(out.data(), 128, 1, nullptr);
    CHECK(host.recorder().state() == TakeRecorder::State::Done && host.recorder().frames() == 88200);
    float peak = 0;
    for (uint32_t f = 0; f < 88200; ++f) peak = std::max(peak, std::fabs(host.recorder().left()[f]));
    CHECK(peak > 0.01f);
}
void project_files() {
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    std::snprintf(project->name, sizeof(project->name), "Night drive");
    project->tempo = 1245; project->swing = 58; project->root = 2; project->scale = 2;
    load_sound(project->tracks[0], EngineKind::Prophet, 1);
    project->tracks[0].params[prophet::Cutoff] = 33;
    project->tracks[0].mix = {90, -20, 40, 2, true, false};
    project->tracks[0].mods[3] = {1, 2, -45};
    project->tracks[0].fx.slots[0] = {uint8_t(EffectKind::Echo), true, {60, 50, 30, 70, 0, 0}};
    load_sound(project->tracks[4], EngineKind::Sampler, 0);
    project->tracks[4].pads[9].sample = 2; project->tracks[4].pads[9].start = 1234; project->tracks[4].pads[9].group = 3;
    std::snprintf(project->samples[2].path, sizeof(project->samples[2].path), "/media/card/Samples/Kick.wav");
    bank->at[0][3].length = 96 * 48; bank->at[0][3].division = Division::Eighth;
    bank->at[0][3].add({95 * 48 + 5, 40, 64, 77, 100, step_tie}); // five ticks late, Rec Quantize off
    project->motion.write({0, 3, 7, prophet::Resonance, 61});
    project->song.place({0, 3, 8, 4, true});
    project->song.step_marker(8);
    project->song.loop_start = 8; project->song.loop_end = 12;
    project->buses[1].level = 33; project->buses[1].fx.slots[0].kind = uint8_t(EffectKind::Room);
    project->master.slots[0].kind = uint8_t(EffectKind::Limiter);

    auto data = save_project(*project, *bank);
    auto copy = std::make_unique<Project>();
    auto copy_bank = std::make_unique<PatternBank>();
    CHECK(load_project(data, *copy, *copy_bank).empty());
    CHECK(std::string(copy->name) == "Night drive" && copy->tempo == 1245 && copy->swing == 58 && copy->scale == 2);
    CHECK(copy->tracks[0].engine == EngineKind::Prophet && copy->tracks[0].params[prophet::Cutoff] == 33);
    CHECK(copy->tracks[0].mix.pan == -20 && copy->tracks[0].mix.bus == 2 && copy->tracks[0].mix.mute);
    CHECK(copy->tracks[0].mods[3].amount == -45 && copy->tracks[0].fx.slots[0].bypass && copy->tracks[0].fx.slots[0].params[3] == 70);
    CHECK(copy->tracks[4].engine == EngineKind::Sampler && copy->tracks[4].pads[9].start == 1234 && copy->tracks[4].pads[9].group == 3);
    CHECK(!copy->tracks[4].pads[8].used());
    CHECK(std::string(copy->samples[2].path) == "/media/card/Samples/Kick.wav");
    CHECK(copy_bank->at[0][3].length == 96 * 48 && copy_bank->at[0][3].division == Division::Eighth);
    CHECK(copy_bank->at[0][3].events.size() == 1 && copy_bank->at[0][3].events[0] == Event({95 * 48 + 5, 40, 64, 77, 100, step_tie}));
    CHECK(copy_bank->at[0][3].step(95).notes[0].key == 64 && copy_bank->at[0][3].step(95).notes[0].offset == 5);
    CHECK(copy->motion.count == 1 && copy->motion.events[0].value == 61);
    CHECK(copy->song.count == 1 && copy->song.clips[0].muted && copy->song.marker_count == 1 && copy->song.loop_end == 12);
    CHECK(copy->buses[1].level == 33 && copy->buses[1].fx.slots[0].kind == uint8_t(EffectKind::Room));
    CHECK(copy->master.slots[0].kind == uint8_t(EffectKind::Limiter));
    // A file that does not fit is refused and changes nothing; an unknown section is passed over.
    auto damaged = data;
    damaged.resize(damaged.size() / 2);
    copy->tempo = 999;
    CHECK(!load_project(damaged, *copy, *copy_bank).empty() && copy->tempo == 999);
    auto extended = data;
    const uint8_t extra[] {'N', 'E', 'X', 'T', 3, 0, 0, 0, 1, 2, 3};
    extended.insert(extended.end(), extra, extra + sizeof(extra));
    CHECK(load_project(extended, *copy, *copy_bank).empty() && copy->tempo == 1245);
    CHECK(!load_project({1, 2, 3}, *copy, *copy_bank).empty());
}
void helpers() {
    // Chords from the scale: pad 2 in D Dorian from octave 3 is Em7, E3 G3 B3 D4 (the Chord board).
    Project project;
    project.root = 2; project.scale = 2;
    Track track;
    track.octave = 2; track.chord_type = 1;
    std::array<int, notes_per_step> keys {};
    CHECK(chord_notes(project, track, 1, keys) == 4 && keys[0] == 40 && keys[1] == 43 && keys[2] == 47 && keys[3] == 50);
    CHECK(chord_name(project, track, 1) == "Em7" && chord_name(project, track, 0) == "Dm7");
    track.chord_invert = 1;
    chord_notes(project, track, 1, keys);
    CHECK(keys[0] == 43 && keys[3] == 52); // first inversion: E goes up
    track.chord_type = 4; track.chord_invert = 0;
    CHECK(chord_notes(project, track, 0, keys) == 3 && keys[2] == keys[0] + 12 && chord_name(project, track, 0) == "D5");
    // The arp: up, down and up-down over two keys and two octaves.
    Arp a;
    a.press(64, 90); a.press(60, 100);
    a.octaves = 2;
    int v = 0;
    std::array<int, 6> up {};
    for (auto& k : up) k = a.next(v);
    CHECK(up[0] == 60 && up[1] == 64 && up[2] == 72 && up[3] == 76 && up[4] == 60);
    a.mode = ArpMode::Down; a.position = 0;
    CHECK(a.next(v) == 76 && a.next(v) == 72);
    a.mode = ArpMode::Order; a.position = 0; a.octaves = 1;
    CHECK(a.next(v) == 64 && v == 90 && a.next(v) == 60);
    a.release(64); a.release(60);
    CHECK(a.next(v) == -1);
    // The host plays the arp while the song is stopped.
    auto p = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    load_sound(p->tracks[0], EngineKind::Prophet, 0);
    EngineHost host;
    host.load(*p, *bank);
    host.send({Command::Type::Arp, 0, 0, 0, 1});
    host.send({Command::Type::ArpKey, 0, 0, 60, 100});
    std::vector<int32_t> out(256);
    float peak = 0;
    for (unsigned n = 0; n < 100; ++n) { host.render(out.data(), 128, 1); for (auto s : out) peak = std::max(peak, std::fabs(s / 2147483648.0f)); }
    CHECK(!host.playing() && peak > 0.005f);
}
void drums() {
    // Every drum of the 909 kit sounds, stays bounded and ends.
    for (unsigned d = 0; d < kit909::Drums; ++d) {
        Kit909Engine kit;
        kit.note_on(int(36 + d), 120);
        auto out = run(kit, 44100 * 10);
        CHECK(peak_of(out, 0, 4410) > 0.02f);
        CHECK(peak_of(out) < 2.0f);
        CHECK(!kit.sounding());
    }
    // The closed hat chokes the open one.
    Kit909Engine kit;
    kit.note_on(36 + kit909::OpenHat, 120);
    kit.note_on(36 + kit909::ClosedHat, 120);
    auto out = run(kit, 44100);
    CHECK(peak_of(out, 22050) < 0.001f);
}
// Every factory sound of an engine plays a note that sounds, stays bounded and ends after release.
template <class E> void sounds_play(const EngineInfo& info, int key) {
    for (unsigned s = 0; s < info.sound_count; ++s) {
        E engine;
        std::array<int16_t, max_params> values;
        sound_values(info, s, values);
        for (unsigned q = 0; q < info.param_count; ++q) engine.set_param(int(q), values[q]);
        engine.note_on(key, 110);
        auto held = run(engine, 22050);
        if (!(peak_of(held) > 0.005f && peak_of(held) < 2.0f)) std::fprintf(stderr, "%s: %s peak %f\n", info.name, info.sounds[s].name, peak_of(held));
        CHECK(peak_of(held) > 0.005f && peak_of(held) < 2.0f);
        engine.note_off(key);
        run(engine, 44100 * 12);
        CHECK(!engine.sounding());
    }
}
void more_engines() {
    sounds_play<Fm6Engine>(fm6::info(), 60);
    sounds_play<SidEngine>(sid::info(), 48);
    sounds_play<CzEngine>(cz::info(), 57);
    // Phase distortion: with no bend every wave is the cosine itself.
    for (unsigned w = 0; w < 5; ++w)
        for (float ph : {0.1f, 0.3f, 0.7f}) CHECK(std::fabs(cz::shape(w, ph, 0) + std::cos(2 * 3.14159265f * ph)) < 0.05f || w == 2 || w == 3);
    // Every algorithm is heard.
    for (unsigned a = 0; a < fm6::algorithms; ++a) {
        Fm6Engine e;
        e.set_param(fm6::Algorithm, int(a));
        e.note_on(60, 100);
        CHECK(peak_of(run(e, 4410)) > 0.005f);
    }
}
void punches() {
    // A test signal with no repeats in it: noise.
    auto signal = [](unsigned n) { uint32_t h = n * 2654435761u; return (int32_t(h) >> 8) / 8388608.0f * 0.5f; };
    const double bar = 88320; // about two seconds a bar, a whole number of samples a slice
    for (unsigned k = 0; k < punch_count; ++k) {
        Punch punch;
        std::vector<float> l(128), r(128);
        unsigned at = 0;
        auto block = [&] { for (unsigned f = 0; f < 128; ++f) l[f] = r[f] = signal(at + f); punch.process(l.data(), r.data(), 128); at += 128; };
        for (unsigned b = 0; b < 200; ++b) block(); // history
        punch.start(PunchKind(k), 1, bar, bar / 16, 1);
        float peak = 0;
        for (unsigned b = 0; b < 1400; ++b) { // four seconds held, past the history's length
            block();
            for (unsigned f = 0; f < 128; ++f) { CHECK(std::isfinite(l[f]) && std::isfinite(r[f])); peak = std::max(peak, std::fabs(l[f])); }
        }
        if (peak > 3) std::fprintf(stderr, "%s peak %f\n", punch_name(PunchKind(k)), peak);
        CHECK(peak < 3);
        punch.stop();
        for (unsigned b = 0; b < 40; ++b) block(); // the 60 ms fade and then some
        CHECK(!punch.active());
        block();
        for (unsigned f = 0; f < 128; ++f) CHECK(l[f] == signal(at - 128 + f)); // dry again, exactly
    }
    // Loop 16: the first slice plays as it comes, then again and again.
    {
        Punch punch;
        std::vector<float> l(1), r(1), out;
        unsigned at = 0, loop = unsigned(bar / 16);
        punch.start(PunchKind::Loop16, 1, bar, bar / 16, 0);
        for (; at < loop * 4; ++at) { l[0] = r[0] = signal(at); punch.process(l.data(), r.data(), 1); out.push_back(l[0]); }
        for (unsigned n = 0; n < loop; ++n) {
            CHECK(out[n] == signal(n));
            CHECK(std::fabs(out[loop + n] - signal(n)) < 1e-6f && std::fabs(out[3 * loop + n] - signal(n)) < 1e-6f);
        }
    }
    // Stutter: the slice, then silence as long.
    {
        Punch punch;
        std::vector<float> l(1), r(1), out;
        unsigned loop = unsigned(bar / 32);
        punch.start(PunchKind::Stutter, 1, bar, bar / 16, 0);
        for (unsigned at = 0; at < loop * 3; ++at) { l[0] = r[0] = signal(at); punch.process(l.data(), r.data(), 1); out.push_back(l[0]); }
        CHECK(peak_of(out, loop + 2, 2 * loop - 2) == 0 && peak_of(out, 2 * loop + 2, 3 * loop - 2) > 0.1f);
    }
    // Half depth: halfway between dry and wet (the gate's closed half).
    {
        Punch punch;
        std::vector<float> l(1), r(1);
        punch.start(PunchKind::Gate, 0.5f, bar, 1000, 0);
        float worst = 0;
        for (unsigned at = 0; at < 1000; ++at) {
            l[0] = r[0] = signal(at); punch.process(l.data(), r.data(), 1);
            if (at > 510) worst = std::max(worst, std::fabs(l[0] - signal(at) * 0.5f));
        }
        CHECK(worst < 1e-6f);
    }
    // The host: a master punch while stopped starts at once; Off lets go; a pad punch on a sampler track.
    {
        auto host = std::make_unique<EngineHost>();
        auto project = std::make_unique<Project>();
        auto patterns = std::make_unique<PatternBank>();
        host->load(*project, *patterns);
        std::vector<int32_t> out(256);
        host->send({Command::Type::PunchOn, 255, uint8_t(2 | 1 << 4), uint16_t(PunchKind::Gate), 100});
        host->render(out.data(), 128, 1);
        CHECK(host->punching());
        host->send({Command::Type::PunchOff, 254});
        host->render(out.data(), 128, 1);
        CHECK(!host->punching());
        host->send({Command::Type::Engine, 4, 0, uint16_t(EngineKind::Sampler)});
        host->send({Command::Type::PunchOn, 4, 2, uint16_t(unsigned(PunchKind::Stutter) | 3 << 8), 80});
        host->render(out.data(), 128, 1);
        CHECK(host->punching());
        host->send({Command::Type::PunchOff, 4, 0, uint16_t(3 << 8)});
        host->render(out.data(), 128, 1);
        CHECK(!host->punching());
    }
    // Punch settings and each pad's punch survive a save.
    {
        auto project = std::make_unique<Project>();
        auto patterns = std::make_unique<PatternBank>();
        project->punch_target = 1; project->punch_bank = 1; project->punch_depth = 64; project->punch_snap = 3; project->punch_tail = 2;
        auto& pad = project->tracks[4].pads[9];
        pad.sample = 0; pad.punch = 15; pad.punch_rate = 1; pad.punch_depth = 33;
        std::strcpy(project->samples[0].path, "/x.wav");
        auto bytes = save_project(*project, *patterns);
        auto back = std::make_unique<Project>();
        CHECK(load_project(bytes, *back, *patterns).empty());
        CHECK(back->punch_target == 1 && back->punch_bank == 1 && back->punch_depth == 64 && back->punch_snap == 3 && back->punch_tail == 2);
        const auto& p2 = back->tracks[4].pads[9];
        CHECK(p2.punch == 15 && p2.punch_rate == 1 && p2.punch_depth == 33);
    }
}
void settings() {
    // Settings survive their text, ignore what they do not know and keep values in range.
    Settings a;
    a.pad_response = 2; a.pad_lights = 1; a.knob_takeover = Takeover::Instant; a.screen = 60;
    Settings b;
    b.read("start=/x\n" + a.text() + "colour=pink\nscreen=900\n");
    CHECK(b.pad_response == 2 && b.pad_lights == 1 && b.knob_takeover == Takeover::Instant && b.screen == 100);
    b.read(a.text());
    CHECK(b.screen == 60);
    // Pad response: soft lifts a light hit, hard lowers it, fixed is always 100, medium leaves it.
    Settings soft, medium, hard, fixed;
    soft.pad_response = 0; hard.pad_response = 2; fixed.pad_response = 3;
    CHECK(soft.velocity(40) > 40 && hard.velocity(40) < 40 && medium.velocity(40) == 40 && fixed.velocity(7) == 127);
    CHECK(soft.velocity(127) == 127 && hard.velocity(127) == 127 && hard.velocity(1) >= 1);
    // Rows step and wrap; the screen stays between 20 and 100.
    Settings c;
    c.step(Settings::Screen, 1); CHECK(c.screen == 100);
    for (int n = 0; n < 20; ++n) c.step(Settings::Screen, -1);
    CHECK(c.screen == 20);
    c.step(Settings::PadLights, -1); CHECK(c.pad_lights == 2);
    c.step(Settings::Palette, 1); CHECK(c.value_text(Settings::Palette) == "NIGHT");
    // Double Seq: Copy notes (the MPC's) or Empty, kept in the text.
    CHECK(c.double_copies && c.value_text(Settings::DoubleSeq) == "Copy notes");
    c.step(Settings::DoubleSeq, 1); CHECK(!c.double_copies && c.value_text(Settings::DoubleSeq) == "Empty");
    { Settings back; back.read(c.text()); CHECK(!back.double_copies); }
    // Favourites: starred sounds by engine, kept in the text; the Sounds page's FAVOURITES lists them.
    static_assert(Settings::engines == unsigned(EngineKind::Count), "a favourites row per engine");
    Settings f;
    f.toggle_favourite(unsigned(EngineKind::Prophet), 3); f.toggle_favourite(unsigned(EngineKind::Prophet), 9);
    f.toggle_favourite(unsigned(EngineKind::Cz), 2); f.toggle_favourite(unsigned(EngineKind::Cz), 2);
    Settings g;
    g.read(f.text());
    CHECK(g.favourite(unsigned(EngineKind::Prophet), 3) && g.favourite(unsigned(EngineKind::Prophet), 9) && !g.favourite(unsigned(EngineKind::Cz), 2));
    auto track = std::make_unique<Track>();
    load_sound(*track, EngineKind::Prophet, 0);
    UiState ui;
    ui.settings = g; ui.sounds_filter = 1;
    std::array<uint16_t, max_listed> list {};
    CHECK(sound_list(*track, ui, list) == 2 && list[0] == 3 && list[1] == 9);
}
void names() {
    // Pads type like phone keys: a tap the first letter, again within 0.6 s the other.
    NameEntry e;
    e.start(NameTarget::ProjectSave, "", 31, false);
    e.pad(0, 1000);            // A
    e.pad(0, 1300);            // B (again in time)
    e.pad(0, 2500);            // A (too late: a new letter)
    CHECK(std::string(e.text) == "BA");
    e.lower = true;
    e.pad(1, 3000); e.pad(1, 3100); // d
    CHECK(std::string(e.text) == "BAd");
    e.pad(13, 3500);           // space
    e.pad(14, 3600);           // digits
    e.pad(1, 3700);            // 2
    CHECK(std::string(e.text) == "BAd 2" && e.cursor == 5);
    e.pad(15, 3800);           // delete
    CHECK(std::string(e.text) == "BAd " && e.length == 4);
    e.move(-2);
    e.erase();                 // the A
    CHECK(std::string(e.text) == "Bd " && e.cursor == 1);
    e.step_char(1);            // d -> e, under the cursor
    CHECK(std::string(e.text) == "Be ");
    e.clear();
    CHECK(e.length == 0 && e.text[0] == 0);
    // Limits, capitals and characters a card would refuse.
    NameEntry m;
    m.start(NameTarget::Marker, "verse/two:", 11, true);
    CHECK(std::string(m.text) == "VERSETWO");
    for (unsigned k = 0; k < 10; ++k) m.pad(4, 10000 + k * 1000);
    CHECK(m.length == 11);
    m.lower = true; m.clear(); m.pad(0, 50000);
    CHECK(std::string(m.text) == "A"); // markers stay capitals
    // K1's ready names: the marker list after the marker's own.
    std::array<const char*, max_suggestions> names {};
    NameEntry k;
    k.start(NameTarget::Marker, "VERSE", 11, true);
    unsigned count = name_suggestions(k, names);
    CHECK(count == 1 + Song::marker_name_count && std::string(names[0]) == "VERSE" && std::string(names[1]) == "INTRO");
    // A sound of one's own: its file keeps the engine, the name, the values and the routes.
    auto track = std::make_unique<Track>();
    load_sound(*track, EngineKind::Fm6, 2);
    track->params[3] = -7; track->mods[1] = {2, 3, -40};
    auto bytes = save_sound(sound_of(*track, "BELL TREE"));
    UserSound back;
    CHECK(load_sound_file(bytes, back));
    CHECK(back.engine == EngineKind::Fm6 && std::string(back.name) == "BELL TREE" && back.params == track->params);
    CHECK(back.mods[1].source == 2 && back.mods[1].destination == 3 && back.mods[1].amount == -40);
    bytes[0] = 'X';
    CHECK(!load_sound_file(bytes, back));
    // The Sounds list counts sounds of one's own after the factory ones.
    UiState ui;
    ui.user_sound_count = 2;
    std::array<uint16_t, max_listed> list {};
    unsigned listed = sound_list(*track, ui, list);
    CHECK(listed == fm6::info().sound_count + 2 && list[listed - 1] == fm6::info().sound_count + 1);
}
} // namespace

// Undo and redo: edits move between the lists, a new one drops the redos,
// and only the last 32 are kept.
void history() {
    Track t;
    t.pads[3].sample = 7; t.pads[3].pitch = 5; t.pad_fx.at[3].slots[0].kind = 2;
    PadState copied = pad_state(t, 3);
    set_pad_state(t, 20, copied);
    CHECK(t.pads[20].sample == 7 && t.pads[20].pitch == 5 && t.pad_fx.at[20].slots[0].kind == 2 && t.pad_fx.at[20].size == pad_slots);
    CHECK(pad_text("Delete", 20) == "Delete B05");

    History h;
    CHECK(!h.undo() && !h.redo());
    for (int n = 0; n < 40; ++n) { Edit e; e.pad = int16_t(n); h.push(std::move(e)); }
    int undone = 0;
    while (Edit* e = h.undo()) { CHECK(e->pad == 39 - undone); ++undone; }
    CHECK(undone == int(History::depth));
    Edit* again = h.redo();
    CHECK(again && again->pad == 8);   // the oldest kept
    CHECK(h.can_redo());
    Edit fresh; fresh.pad = 99;
    h.push(std::move(fresh));          // a new edit: nothing to redo
    CHECK(!h.can_redo() && h.undo()->pad == 99 && h.undo()->pad == 8 && !h.can_undo());

    // A Sample Record take: one edit for its pad and the pads cut during it, kept whole through undo and redo.
    History takes;
    Edit take; take.what = pad_text("Take on", 0);
    take.pads_before = {{0, PadState {}}, {5, copied}};
    take.pads_after = {{0, copied}, {5, PadState {}}};
    takes.push(std::move(take));
    Edit* undone_take = takes.undo();
    CHECK(undone_take && undone_take->pads_before.size() == 2 && undone_take->pads_before[1].first == 5 && undone_take->pads_before[1].second.sound.sample == 7);
    Edit* redone_take = takes.redo();
    CHECK(redone_take && redone_take->what == "Take on A01" && redone_take->pads_after[0].second.sound.pitch == 5);
}

// Parity phase 0: a step written on a kit gets the pad hit last, and
// Keys has Transpose on Shift + K3 (phase 1b; it was row 2).
void parity_basics() {
    Track kit; kit.engine = EngineKind::Sampler; kit.last_pad = 21; kit.last_note = 50;
    CHECK(kit.step_note() == sampler_first_note + 21);
    Track synth; synth.engine = EngineKind::Prophet; synth.last_note = 62;
    CHECK(synth.step_note() == 62);

    auto project = std::make_unique<Project>();
    project->tracks[0].engine = EngineKind::Prophet; project->tracks[0].transpose = -3;
    UiState ui; ui.page = PageId::Keys; ui.track = 0;
    Pattern pattern;
    CHECK(page_bindings(*project, ui, pattern)[0].kind == Binding::Kind::Root);
    CHECK(page_bindings(*project, ui, pattern)[2].kind == Binding::Kind::Layout);
    ui.shift = true;
    auto shifted = page_bindings(*project, ui, pattern);
    CHECK(shifted[2].kind == Binding::Kind::Transpose && shifted[2].value == -3 && shifted[2].min == -12 && shifted[2].max == 12);
    CHECK(shifted[0].kind == Binding::Kind::Root && shifted[1].kind == Binding::Kind::Scale); // no Shift card: as without
    CHECK(page_bindings(*project, ui, pattern, false)[2].kind == Binding::Kind::Layout);    // Shift + wheel after a plain turn
    auto second = shift_bindings(*project, ui, pattern);
    CHECK(!second[0].bound() && !second[1].bound() && second[2].bound());
    project->tracks[0].engine = EngineKind::Kit909; // drums have no transpose
    CHECK(!shift_bindings(*project, ui, pattern)[2].bound());
}

// Phase 1b: Shift + Pad 1-16 as printed, their toggles and lights, and the
// buttons that glow while Shift is held.
void shift_pads() {
    const char* const printed[16] {"Full Level", "Half Seq", "Double Seq", "Count-In", "Compressor", "Half Speed",
                                   "Double Speed", "MIDI Config", "Fader", "Rec Quantize", "Resample", "Song",
                                   "Trim Sample", "Time Correct", "Warp", "Project"};
    for (unsigned n = 0; n < 16; ++n) CHECK(std::string(shift_pad_job(n).name) == printed[n]);
    using K = ShiftPadJob::Kind;
    for (unsigned n : {0u, 3u}) CHECK(shift_pad_job(n).kind == K::Toggle);
    CHECK(shift_pad_job(9).kind == K::Open); // Rec Quantize: its page (on / off and the grid)
    CHECK(shift_pad_job(2).kind == K::Action && shift_pad_job(11).kind == K::Open && shift_pad_job(15).kind == K::Open);
    for (unsigned n : {1u, 5u, 6u}) CHECK(shift_pad_job(n).kind == K::Action); // Half Seq, Half / Double Speed (phase 2)
    CHECK(shift_pad_job(13).kind == K::Open);                                  // Time Correct (phase 2)
    CHECK(shift_pad_job(14).kind == K::Action);                                // Warp: Time stretch / Pitch (phase 4)
    CHECK(shift_pad_job(12).kind == K::Action);                                // Trim Sample (phase 5)
    CHECK(shift_pad_job(10).kind == K::Action);                                // Resample (phase 6)
    CHECK(shift_pad_job(4).kind == K::Open);                                   // Compressor (phase 7)
    CHECK(shift_pad_job(8).kind == K::Open);                                   // Fader (phase 8)
    CHECK(shift_pad_job(7).kind == K::Open);                                   // MIDI Config (phase 9): all sixteen built

    UiState ui;
    CHECK(!ui.settings.full_level() && !ui.settings.count_in && ui.settings.rec_quantize);
    CHECK(shift_pad_level(ui, 0) == 1 && shift_pad_level(ui, 9) == 2 && shift_pad_level(ui, 7) == 1 && shift_pad_level(ui, 11) == 1);
    ui.settings.pad_response = 2;
    ui.settings.toggle_full_level();
    CHECK(ui.settings.full_level() && ui.settings.velocity(5) == 127 && shift_pad_level(ui, 0) == 2);
    ui.settings.count_in = true; ui.settings.rec_quantize = false;
    Settings kept; kept.read(ui.settings.text()); // the toggles live in Melodee's settings file
    CHECK(kept.full_level() && kept.count_in && !kept.rec_quantize);
    CHECK(shift_pad_level(ui, 9) == 1); // off: the pad faint, its page still there
    // Rec Quantize's grid: kept with the settings; a file from before it follows the pattern's Q.
    CHECK(kept.rec_grid == 0);
    ui.settings.rec_grid = 1 + unsigned(Division::ThirtySecondTriplet);
    kept.read(ui.settings.text());
    CHECK(kept.rec_grid == 1 + unsigned(Division::ThirtySecondTriplet));
    Settings old; old.read("rec_quantize=1\n");
    CHECK(old.rec_quantize && old.rec_grid == 0);
    old.read("rec_grid=99\n");
    CHECK(old.rec_grid == unsigned(Division::Count)); // clamped to the last, 1/64
    CHECK(grid_division(0, Division::Eighth) == Division::Eighth && grid_division(5, Division::Eighth) == Division::Sixteenth);
    CHECK(grid_division(9, Division::Eighth) == Division::SixtyFourth && grid_division(10, Division::Eighth) == Division::Eighth);
    {
        Project project; UiState g; g.page = PageId::RecQuantize; g.settings.rec_grid = 3;
        Pattern none;
        auto cards = page_bindings(project, g, none);
        CHECK(cards[0].kind == Binding::Kind::RecGrid && cards[0].value == 3 && cards[0].max == int(Division::Count) && !cards[1].bound());
    }
    kept.toggle_full_level();
    CHECK(kept.pad_response == 2); // back to the curve it came from

    auto project = std::make_unique<Project>();
    project->tracks[0].engine = EngineKind::Prophet;
    ui.track = 0; ui.page = PageId::Stage; ui.shift = true;
    using B = SurfaceButton;
    CHECK(button_has_shift_job(*project, ui, B::Sample, false) && button_has_shift_job(*project, ui, B::Minus, false));
    CHECK(!button_has_shift_job(*project, ui, B::Loop, false) && !button_has_shift_job(*project, ui, B::Erase, false));
    CHECK(!button_has_shift_job(*project, ui, B::SampleRecord, false) && !button_has_shift_job(*project, ui, B::Chop, false));
    project->tracks[0].engine = EngineKind::Sampler;
    CHECK(button_has_shift_job(*project, ui, B::Loop, true) && button_has_shift_job(*project, ui, B::Erase, true));
    ui.name.start(NameTarget::Marker, "A", 11, true);
    CHECK(!button_has_shift_job(*project, ui, B::Sample, true));
}

// Phase 1a: Patterns' cards, Input Config, Back's stack and which buttons glow.
void button_remap() {
    auto project = std::make_unique<Project>();
    project->tracks[0].engine = EngineKind::Prophet;
    project->tracks[1].engine = EngineKind::Sampler;
    Pattern pattern; pattern.length = 3 * project->bar_ticks();
    UiState ui; ui.track = 0;
    ui.page = PageId::Patterns;
    auto cards = page_bindings(*project, ui, pattern);
    CHECK(cards[0].kind == Binding::Kind::PatternLength && pattern_lengths[cards[0].value] == 2); // 1, 2, 4 ..: 3 bars sits on 2
    CHECK(cards[1].kind == Binding::Kind::Division && cards[2].kind == Binding::Kind::Swing);
    ui.page = PageId::InputConfig; ui.settings.input.threshold = -24;
    cards = page_bindings(*project, ui, pattern);
    CHECK(cards[0].kind == Binding::Kind::RecSource && cards[1].kind == Binding::Kind::RecLength);
    CHECK(cards[2].kind == Binding::Kind::RecThreshold && cards[2].value == -24 && cards[2].min == -96);
    ui.page = PageId::Record; // the same cards as Input Config (phase 6), Shift + K3 the gain
    auto record_cards = page_bindings(*project, ui, pattern);
    for (unsigned n = 0; n < 3; ++n) CHECK(record_cards[n].kind == cards[n].kind && record_cards[n].value == cards[n].value);
    CHECK(page_bindings(*project, ui, pattern, true)[2].kind == Binding::Kind::RecGain);
    // Seq held, on any page: K1-K3 the pattern's length, Q and swing; Shift as on Patterns; before Knob FX's.
    ui.page = PageId::Mix; ui.seq_held = true;
    cards = page_bindings(*project, ui, pattern);
    CHECK(cards[0].kind == Binding::Kind::PatternLength && cards[1].kind == Binding::Kind::Division && cards[2].kind == Binding::Kind::Swing);
    cards = page_bindings(*project, ui, pattern, true);
    CHECK(cards[0].kind == Binding::Kind::PatternBars && cards[1].kind == Binding::Kind::TimeSignature && cards[2].kind == Binding::Kind::ClickLevel);
    project->knob_fx.on = true; project->knob_fx.kind = 1;
    CHECK(page_bindings(*project, ui, pattern)[0].kind == Binding::Kind::PatternLength);
    ui.seq_held = false;
    CHECK(page_bindings(*project, ui, pattern)[0].kind != Binding::Kind::PatternLength); // Knob FX's again
    project->knob_fx.on = false; project->knob_fx.kind = 0;

    // Back: Stage -> Sounds -> Steps -> Roll -> Motion -> Song; Back walks Song, Steps group, Sounds, Stage.
    UiState nav;
    auto go = [&](PageId page) { push_back_stack(nav, page); nav.page = page; };
    auto back = [&] { PageId to = back_target(nav); go(to); };
    go(PageId::Sounds); go(PageId::Steps); go(PageId::Roll); go(PageId::Motion); go(PageId::Song);
    CHECK(nav.back_depth == 3);
    back(); CHECK(nav.page == PageId::Motion);
    back(); CHECK(nav.page == PageId::Sounds); // Steps, Roll and Motion are one stop
    back(); CHECK(nav.page == PageId::Stage && nav.back_depth == 0);
    back(); CHECK(nav.page == PageId::Stage);
    // Opening a page already on the stack goes back to it instead of growing it.
    go(PageId::Sounds); go(PageId::Patterns); go(PageId::Sounds);
    CHECK(nav.back_depth == 1 && nav.back_stack[0] == PageId::Stage);
    // Seq's levels: a tap opens Patterns from anywhere; on Patterns the track's editor (Steps on a kit, Roll on a
    // synth until one is opened, then that one, per track); on an editor back to Patterns; Step Edit goes up to its editor.
    {
        UiState q; q.track = 0;
        q.page = PageId::Stage; CHECK(seq_tap_page(q, false) == PageId::Patterns);
        q.page = PageId::Song; CHECK(seq_tap_page(q, true) == PageId::Patterns);
        q.page = PageId::Patterns; CHECK(seq_tap_page(q, false) == PageId::Roll && seq_tap_page(q, true) == PageId::Steps);
        remember_editor(q, PageId::Motion);
        CHECK(seq_tap_page(q, false) == PageId::Motion && seq_tap_page(q, true) == PageId::Motion);
        q.track = 1; CHECK(seq_tap_page(q, false) == PageId::Roll); // each track its own
        q.track = 0; remember_editor(q, PageId::Stage); CHECK(seq_editor_page(q, false) == PageId::Motion); // only editors count
        q.page = PageId::Motion; CHECK(seq_tap_page(q, false) == PageId::Patterns);
        q.page = PageId::Steps; CHECK(seq_tap_page(q, true) == PageId::Patterns);
        q.page = PageId::StepEdit; CHECK(seq_tap_page(q, false) == PageId::Motion);
        CHECK(seq_editor(PageId::Roll) && !seq_editor(PageId::StepEdit) && !seq_editor(PageId::Patterns));
    }
    // A full stack drops its oldest page.
    for (unsigned n = 0; n < 40; ++n) go(n % 2 ? PageId::Tempo : PageId::Project), go(PageId(unsigned(PageId::Samples) + n % 3));
    CHECK(nav.back_depth <= UiState::back_capacity);

    // Faint lights: Chop only with a kit; Loop everywhere (twice or held, the Looper); Erase where it does something.
    using B = SurfaceButton;
    UiState l; l.page = PageId::Stage; l.track = 0;
    CHECK(button_has_job(*project, l, B::Seq, false, false));
    CHECK(!button_has_job(*project, l, B::Chop, false, false) && button_has_job(*project, l, B::Loop, false, false));
    CHECK(!button_has_job(*project, l, B::Erase, false, false) && button_has_job(*project, l, B::Erase, false, true));
    l.page = PageId::Patterns; // the pads play here now: Erase has nothing of its own on a synth track
    CHECK(!button_has_job(*project, l, B::Erase, false, false) && !button_has_shift_job(*project, l, B::Erase, false));
    l.seq_held = true; // Seq held: Erase + pad clears a pattern, Shift + Erase copies one, on any page
    l.page = PageId::Stage;
    CHECK(button_has_job(*project, l, B::Erase, false, false) && button_has_shift_job(*project, l, B::Erase, false));
    l.seq_held = false;
    l.track = 1;
    CHECK(button_has_job(*project, l, B::Loop, true, false) && !button_has_job(*project, l, B::Chop, true, false)); // the last pad is empty
    project->tracks[1].pads[0].sample = 0;
    CHECK(button_has_job(*project, l, B::Chop, true, false));
    l.exit_open = true;
    CHECK(!button_has_job(*project, l, B::Play, true, false));
}

// Phase 1c: Mute (kit pads), Note Repeat's rates and swing, eight banks,
// a whole track handed to the audio thread.
void parity_1c() {
    // Eight banks of sixteen: labels A01 .. H16, keys past 127 for the last banks.
    CHECK(sampler_banks == 8 && sampler_pads == 128 && sampler_last_key == 163);
    CHECK(pad_text("Copy", 127) == "Copy H16" && pad_text("Copy", 64) == "Copy E01");

    // Note Repeat: B1-B3 1/8, 1/16, 1/32; Shift: triplets. Swing on every second straight note.
    CHECK(std::string(arp_rate_name(repeat_rate(0, false))) == "1/8" && std::string(arp_rate_name(repeat_rate(2, false))) == "1/32");
    CHECK(std::string(arp_rate_name(repeat_rate(1, true))) == "1/16T" && arp_rate_ticks(repeat_rate(2, true)) == 2);
    for (unsigned r = 1; r < arp_rates; ++r) CHECK(arp_rate_ticks(r) < arp_rate_ticks(r - 1)); // slowest first, as K2 turns
    CHECK(arp_swing_ticks(4, 1, 75) == 3.0 && arp_swing_ticks(4, 2, 75) == 0 && arp_swing_ticks(5, 1, 75) == 0 && arp_swing_ticks(4, 1, 50) == 0);
    auto project = std::make_unique<Project>();
    UiState ui;
    ui.page = PageId::Keys;
    CHECK(!repeat_keys(*project, ui));
    project->tracks[0].arp = true;
    CHECK(repeat_keys(*project, ui));
    ui.page = PageId::Stage; // latched, Stage keeps its buttons; held, they are the rates
    CHECK(!repeat_keys(*project, ui));
    ui.repeat_held = true; CHECK(repeat_keys(*project, ui)); ui.repeat_held = false;
    ui.page = PageId::Arp;
    CHECK(!repeat_keys(*project, ui)); // its own keys stay on Arp
    Arp arp; arp.top = int(sampler_last_key);
    arp.press(150, 90);
    int velocity = 0;
    CHECK(arp.next(velocity) == 150 && velocity == 90); // a kit pad in bank H repeats as itself

    // Mute mode: kit pads where the pads play the kit (Stage too), tracks on Mix and Tracks.
    ui.page = PageId::Pads;
    CHECK(mute_on_kit(ui, true) && !mute_on_kit(ui, false));
    ui.page = PageId::Stage;
    CHECK(mute_on_kit(ui, true));
    ui.page = PageId::Tracks;
    CHECK(!mute_on_kit(ui, true));
    ui.page = PageId::Mix;
    CHECK(!mute_on_kit(ui, true));
    ui.mute_latch = true;
    CHECK(ui.mute_mode());
    Track kit; kit.engine = EngineKind::Sampler;
    kit.pads[3].sample = 0; kit.pads[100].sample = 0;
    kit.set_pad_muted(100, true);
    CHECK(kit.pad_muted(100) && !kit.pad_muted(3) && kit.any_pad_muted() && pad_soloed(kit, 3) && !pad_soloed(kit, 100));
    kit.set_pad_muted(100, false);
    CHECK(!kit.any_pad_muted() && !pad_soloed(kit, 3));

    // Pad Bank held or a track copy: the pads are the tracks; Shift + Erase there copies.
    UiState held; held.pad_bank = true; held.pad_bank_down = 5000;
    CHECK(tracks_on_pads(held));
    // The pads are the tracks at once; the screen shows them a second on (a tap shows nothing).
    CHECK(!track_overlay_shown(held, 5000 + pad_bank_overlay_ms - 1) && track_overlay_shown(held, 5000 + pad_bank_overlay_ms));
    project->tracks[1].engine = EngineKind::Prophet;
    held.track = 1;
    CHECK(button_has_shift_job(*project, held, SurfaceButton::Erase, false));
    held.pad_bank = false; held.copying = UiState::Copying::Tracks;
    CHECK(tracks_on_pads(held) && track_overlay_shown(held, 0)); // a track copy: at once

    // The project file: banks E-H, pad mutes and kit keys past 127 come back; TRAK still holds A-D alone.
    auto bank = std::make_unique<PatternBank>();
    load_sound(project->tracks[4], EngineKind::Sampler, 0);
    auto& t4 = project->tracks[4];
    t4.pads[5].sample = 1; t4.pads[70].sample = 2; t4.pads[70].pitch = -7; t4.pads[127].sample = 3; t4.bank = 7; t4.last_pad = 127;
    t4.set_pad_muted(70, true); t4.set_pad_muted(5, true);
    put(bank->at[4][0], 2, uint8_t(sampler_first_note + 127), 100);
    auto data = save_project(*project, *bank);
    auto copy = std::make_unique<Project>();
    auto copy_bank = std::make_unique<PatternBank>();
    CHECK(load_project(data, *copy, *copy_bank).empty());
    const auto& c4 = copy->tracks[4];
    CHECK(c4.pads[5].sample == 1 && c4.pads[70].sample == 2 && c4.pads[70].pitch == -7 && c4.pads[127].sample == 3);
    CHECK(c4.bank == 7 && c4.last_pad == 127 && c4.pad_muted(70) && c4.pad_muted(5) && !c4.pad_muted(6));
    CHECK(copy_bank->at[4][0].step(2).notes[0].key == sampler_first_note + 127);
    for (const char* tag : {"KITX", "PMUT"}) CHECK(std::search(data.begin(), data.end(), tag, tag + 4) != data.end());

    // The engine: a pad in bank H plays from its key; muted, it is not heard, and what it played fades.
    SampleData sound;
    sound.frames = 44100; sound.left.assign(44100, 0.5f); sound.right.assign(44100, 0.5f);
    SamplerEngine::Table table {};
    table[0] = &sound;
    SamplerEngine engine;
    engine.set_samples(&table);
    PadSound pad; pad.sample = 0; pad.play = PadPlay::OneShot;
    engine.set_pad(120, pad);
    std::vector<float> l(128), r(128);
    auto loud = [&] {
        float peak = 0;
        for (unsigned b = 0; b < 4; ++b) {
            std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
            engine.render_stereo(l.data(), r.data(), 128);
            for (float v : l) peak = std::max(peak, std::fabs(v));
        }
        return peak;
    };
    engine.note_on(int(sampler_first_note) + 120, 100);
    CHECK(loud() > 0.05f);
    engine.set_pad_mute(120, true);
    loud();
    CHECK(loud() == 0 && !engine.sounding());
    engine.note_on(int(sampler_first_note) + 120, 100);
    CHECK(loud() == 0);
    engine.set_pad_mute(120, false);
    engine.note_on(int(sampler_first_note) + 120, 100);
    CHECK(loud() > 0.05f);

    // The host: a TrackLoad swaps a whole track in (its patterns play) and hands the pack back;
    // a synth never hears a key past 127.
    auto host = std::make_unique<EngineHost>();
    auto start = std::make_unique<Project>();
    auto start_bank = std::make_unique<PatternBank>();
    load_sound(start->tracks[0], EngineKind::Prophet, 1);
    host->load(*start, *start_bank);
    std::vector<int32_t> out(256);
    Track synth;
    load_sound(synth, EngineKind::Prophet, 1);
    std::array<Pattern, pattern_count> patterns {};
    put(patterns[0], 0, 48, 110);
    auto pack = std::make_unique<TrackPack>(2, synth, patterns, Motion {});
    Command load {Command::Type::TrackLoad, 2};
    load.track_pack = pack.get();
    CHECK(host->send(load));
    host->send({Command::Type::NoteOn, 0, 0, 140, 100}); // ignored: a synth's keys stop at 127
    host->send({Command::Type::Play});
    float peak = 0;
    for (unsigned n = 0; n < 20; ++n) {
        host->render(out.data(), 128, 1);
        for (auto v : out) peak = std::max(peak, std::fabs(v / 2147483648.0f));
    }
    CHECK(peak > 0.005f);
    const TrackPack* back = nullptr;
    CHECK(host->take_done_track(back) && back == pack.get());
    CHECK(host->dropped() == 0);
}

// Parity phase 2: notes on any tick, patterns as event lists up to 128 bars,
// older files, the pattern edits, count-in, time signature, BPM per pattern.
void timing() {
    // The step view: notes keep where in the step they fell, and their own length while the step's is unchanged.
    Pattern p;
    p.add({30, 40, 60, 100, 100, 0}); // step 1 of 1/16, six ticks late, held 40 ticks
    p.add({31, 24, 62, 90, 100, 0});
    for (uint8_t k = 0; k < 5; ++k) p.add({24 * 3, 24, uint8_t(70 + k), 100, 100, 0}); // five notes on one step
    Step one = p.step(1);
    CHECK(one.notes[0].key == 60 && one.notes[0].offset == 6 && one.notes[1].offset == 7 && one.length == 2);
    one.notes[1].velocity = 50; // a velocity changed on Steps: places and lengths stay
    p.set_step(1, one);
    CHECK(p.events[0] == Event({30, 40, 60, 100, 100, 0}) && p.events[1].velocity == 50 && p.events[1].tick == 31);
    Step three = p.step(3);
    CHECK(three.notes[3].key == 73);
    p.set_step(3, Step {}); // the four shown go; the fifth stays
    CHECK(p.events.size() == 3 && p.events.back().key == 74);
    CHECK(p.steps() == 16 && !p.empty());
    // Q is only the grid: a coarser one sees the same notes in fewer, longer steps.
    p.division = Division::Quarter;
    CHECK(p.steps() == 4 && p.step(0).notes[0].offset == 30);
    p.division = Division::Sixteenth;
    CHECK(division_ticks(Division::SixtyFourth) == 6 && division_ticks(Division::QuarterTriplet) == 64 && std::string(division_name(Division::ThirtySecondTriplet)) == "1/32T");

    // Rec Quantize: the nearest step, the last step's second half onto the first.
    Pattern q;
    q.length = 384;
    CHECK(quantise_tick(q, 11) == 0 && quantise_tick(q, 13) == 24 && quantise_tick(q, 380) == 0);
    q.division = Division::EighthTriplet;
    CHECK(quantise_tick(q, 40) == 32);
    // A grid of its own (Settings::rec_grid), whatever the pattern's Q: 1/32, 1/32T, 1/64, a quarter; past the end, the first.
    CHECK(quantise_tick(q, 13, division_ticks(Division::ThirtySecond)) == 12 && quantise_tick(q, 17, division_ticks(Division::ThirtySecond)) == 12);
    CHECK(quantise_tick(q, 18, division_ticks(Division::ThirtySecond)) == 24);
    CHECK(quantise_tick(q, 11, division_ticks(Division::ThirtySecondTriplet)) == 8 && quantise_tick(q, 12, division_ticks(Division::ThirtySecondTriplet)) == 16);
    CHECK(quantise_tick(q, 4, division_ticks(Division::SixtyFourth)) == 6 && quantise_tick(q, 140, division_ticks(Division::Quarter)) == 96);
    CHECK(quantise_tick(q, 382, division_ticks(Division::SixtyFourth)) == 0 && quantise_tick(q, 378, division_ticks(Division::SixtyFourth)) == 378);
    CHECK(quantise_tick(q, 40, division_ticks(grid_division(0, q.division))) == quantise_tick(q, 40)); // 0: the pattern's Q

    // Double / Half Seq, Half / Double Speed.
    Pattern d;
    d.add({0, 24, 36, 100, 100, 0}); d.add({100, 24, 38, 100, 100, 0}); d.add({384, 24, 40, 100, 100, 0}); // the last is past the end
    CHECK(double_pattern(d, 128 * 384) && d.length == 768 && d.events.size() == 4 && d.events[2].tick == 384 && d.events[3].tick == 484);
    CHECK(!double_pattern(d, 768 + 1));
    { // Settings' Double Seq "Empty": twice as long, the new half bare
        Pattern e;
        e.add({0, 24, 36, 100, 100, 0}); e.add({100, 24, 38, 100, 100, 0}); e.add({384, 24, 40, 100, 100, 0});
        CHECK(double_pattern(e, 128 * 384, false) && e.length == 768 && e.events.size() == 2 && e.events[1].tick == 100);
    }
    half_pattern(d);
    CHECK(d.length == 384 && d.events.size() == 2);
    scale_pattern(d, true); // half speed: twice as far apart; 100 -> 200
    CHECK(d.events.size() == 2 && d.events[1].tick == 200 && d.events[1].length == 48 && d.length == 384);
    scale_pattern(d, true); // again: 400 falls past the end
    CHECK(d.events.size() == 1);
    d.add({300, 48, 38, 100, 100, 0});
    scale_pattern(d, false); // double speed
    CHECK(d.events.size() == 2 && d.events[1].tick == 150 && d.events[1].length == 24);

    // Time Correct: the chosen keys onto Q, the second sixteenth of each eighth late by the swing, then shifted.
    Pattern t;
    t.length = 384;
    t.add({5, 24, 36, 100, 100, 0}); t.add({27, 24, 36, 100, 100, 0}); t.add({27, 24, 42, 100, 100, 0}); t.add({370, 24, 36, 100, 100, 0}); // step 15 (370 + 12 rounds down), swung to 372
    std::array<uint64_t, 3> keys {};
    keys[0] |= uint64_t(1) << 36;
    time_correct_all(t, &keys, 75, 0); // 75 %: half a sixteenth late on the off sixteenths
    CHECK(t.events.size() == 4 && t.events[0].tick == 0 && t.events[0].key == 36);
    CHECK(t.events[1].tick == 27 && t.events[1].key == 42); // not chosen: where it was
    CHECK(t.events[2].tick == 36 && t.events[2].key == 36); // step 1 + 12 of swing
    time_correct_all(t, nullptr, 50, -2); // every note, two ticks early; the first wraps to the end
    CHECK(t.events.back().tick == 382 && t.events.front().tick == 22);

    // A file from before phase 2: steps at 24 a quarter and their motion become ticks and sixteenths.
    {
        std::vector<uint8_t> bytes {'M', 'E', 'L', 'O', 'D', 'E', 'E', '1'};
        auto section = [&](const char* tag, std::vector<uint8_t> body) {
            bytes.insert(bytes.end(), tag, tag + 4);
            uint32_t n = uint32_t(body.size());
            for (int k = 0; k < 4; ++k) bytes.push_back(uint8_t(n >> (8 * k)));
            bytes.insert(bytes.end(), body.begin(), body.end());
        };
        // Track 1, pattern 4: 32 steps of 1/8 (old division 0), one note of two steps on step 5.
        section("PATT", {0, 3, 32, 0, 0, 1, 0, 5, 0, 1, 60, 100, 2, 100, step_accent});
        section("MOTN", {1, 0, 0, 3, 5, 0, uint8_t(prophet::Cutoff), 20, 0});
        Project project; auto bank = std::make_unique<PatternBank>();
        CHECK(load_project(bytes, project, *bank).empty());
        const auto& old = bank->at[0][3];
        CHECK(old.length == 32 * 48 && old.division == Division::Eighth && old.events.size() == 1);
        CHECK(old.events[0] == Event({5 * 48, 2 * 48, 60, 100, 100, step_accent}));
        CHECK(project.motion.count == 1 && project.motion.events[0].step == 10); // eighth 5 is sixteenth 10
        // Saved again it is an event list (EVNT), and loads the same.
        project.signature = 12; project.seq_tempo = true; project.pattern_tempo[3] = 905;
        auto data = save_project(project, *bank);
        for (const char* tag : {"EVNT", "MOT2", "TIME"}) CHECK(std::search(data.begin(), data.end(), tag, tag + 4) != data.end());
        const char* patt = "PATT";
        CHECK(std::search(data.begin(), data.end(), patt, patt + 4) == data.end());
        Project again; auto again_bank = std::make_unique<PatternBank>();
        CHECK(load_project(data, again, *again_bank).empty());
        CHECK(again_bank->at[0][3] == old && again.motion.events[0].step == 10);
        CHECK(again.signature == 12 && again.time_signature().beats == 7 && again.bar_ticks() == 336 && again.seq_tempo && again.pattern_tempo[3] == 905);
        CHECK(again.tempo_of(3) == 905 && again.tempo_of(2) == again.tempo);
    }
    // 128 bars of sixteenths: 2048 notes, saved and loaded whole.
    {
        Project project; auto bank = std::make_unique<PatternBank>();
        auto& big = bank->at[2][0];
        big.length = project.max_length();
        for (unsigned n = 0; n < 2048; ++n) big.events.push_back({n * 24, 12, uint8_t(36 + n % 24), 100, 100, 0});
        auto data = save_project(project, *bank);
        Project copy; auto copy_bank = std::make_unique<PatternBank>();
        CHECK(load_project(data, copy, *copy_bank).empty() && copy_bank->at[2][0] == big && big.length == 128 * 384);
    }

    // The engine: a note six ticks off the grid sounds six ticks late.
    auto host_with = [](Pattern pattern, Project* settings = nullptr) {
        auto project = std::make_unique<Project>();
        if (settings) *project = *settings;
        auto bank = std::make_unique<PatternBank>();
        load_sound(project->tracks[0], EngineKind::Prophet, 1);
        project->tempo = 1200;
        bank->at[0][0] = pattern;
        auto host = std::make_unique<EngineHost>();
        host->load(*project, *bank);
        return host;
    };
    auto render = [](EngineHost& host, unsigned blocks) {
        std::vector<int32_t> out(256);
        std::vector<float> left;
        for (unsigned n = 0; n < blocks; ++n) {
            host.render(out.data(), 128, 1);
            for (unsigned f = 0; f < 128; ++f) left.push_back(out[2 * f] / 2147483648.0f);
        }
        return left;
    };
    {
        Pattern late;
        late.add({4 * 24 + 6, 24, 45, 110, 100, 0}); // tick 102: 23428 samples at 120 BPM
        auto host = host_with(late);
        host->send({Command::Type::Play});
        auto left = render(*host, 240);
        CHECK(peak_of(left, 0, 23300) == 0 && peak_of(left, 23500, 30000) > 0.005f);
    }
    // Count-In: a bar of clicks first, the pattern from its start after it.
    {
        Pattern first;
        first.add({0, 24, 45, 110, 100, 0});
        auto host = host_with(first);
        host->send({Command::Type::Play, 1});
        render(*host, 2);
        CHECK(host->count_in() == 1 && host->position_of(0) < 0);
        render(*host, 22050 / 128 + 2); // the second beat
        CHECK(host->count_in() == 2);
        render(*host, 3 * 22050 / 128); // past the bar
        CHECK(host->count_in() == 0 && host->position_of(0) >= 0);
    }
    // Time signature 3/4: a queued pattern starts after three beats, not four.
    {
        auto host = host_with(Pattern {});
        host->send({Command::Type::Signature, 0, 0, 96, 288});
        host->send({Command::Type::Play});
        render(*host, 10);
        host->send({Command::Type::Pattern, 0, 0, 0, 1});
        render(*host, 3 * 22050 / 128 - 20);
        CHECK(host->pattern_of(0) == 0);
        render(*host, 40);
        CHECK(host->pattern_of(0) == 1);
    }
    // BPM per pattern: launching pattern 2 brings its own tempo; global again, the project's.
    {
        Project settings;
        settings.seq_tempo = true; settings.pattern_tempo[1] = 905;
        auto host = host_with(Pattern {}, &settings);
        CHECK(host->tempo_now() == 1200);
        host->send({Command::Type::Pattern, 0, 0, 0, 1});
        render(*host, 1);
        CHECK(host->tempo_now() == 905);
        host->send({Command::Type::SeqTempo, 0, 0, 1, 0});
        render(*host, 1);
        CHECK(host->tempo_now() == 1200);
    }
    // Locate: the wheel moves the playhead while it plays; Play from a position starts there.
    {
        Pattern four; four.length = 4 * 384;
        auto host = host_with(four);
        host->send({Command::Type::Play});
        render(*host, 4);
        Command to {Command::Type::Locate};
        to.position = 2 * 384;
        host->send(to);
        render(*host, 1);
        CHECK(host->position_of(0) >= 2 * 384 && host->position_of(0) < 2 * 384 + 4);
        host->send({Command::Type::Stop});
        Command from {Command::Type::Play};
        from.position = 384 + 96;
        host->send(from);
        render(*host, 1);
        CHECK(host->position_of(0) >= 384 + 96 && host->position_of(0) < 384 + 100);
    }
    // A note written just ahead of the playhead (Rec Quantize) was heard live: skipped once, then played.
    {
        Pattern ahead;
        ahead.add({0, 24, 45, 110, 100, 0});
        auto host = host_with(ahead);
        Command wrote {Command::Type::Recorded, 0, 0, 45};
        wrote.position = 0;
        host->send(wrote);
        host->send({Command::Type::Play});
        auto first_bar = render(*host, 88200 / 128 - 30);
        auto second = render(*host, 100);
        CHECK(peak_of(first_bar, 0, 40000) == 0 && peak_of(second) > 0.005f);
    }

    // Patterns' cards: K1 by 1, 2, 4 .. 128 bars, Shift + K1 any count, Shift + K2 the time signature.
    {
        Project project;
        UiState ui; ui.page = PageId::Patterns;
        Pattern five; five.length = 5 * 384;
        auto base = page_bindings(project, ui, five, false);
        CHECK(base[0].kind == Binding::Kind::PatternLength && base[0].max == 7 && pattern_lengths[base[0].value] == 4);
        auto shifted = page_bindings(project, ui, five, true);
        CHECK(shifted[0].kind == Binding::Kind::PatternBars && shifted[0].value == 5 && shifted[0].max == 128);
        CHECK(shifted[1].kind == Binding::Kind::TimeSignature && shifted[1].value == int(common_time));
        CHECK(shifted[2].kind == Binding::Kind::ClickLevel);
        ui.page = PageId::TimeCorrect; ui.tc_swing = 60;
        auto tc = page_bindings(project, ui, five, false);
        CHECK(tc[0].kind == Binding::Kind::Division && tc[1].kind == Binding::Kind::TcShift && tc[2].kind == Binding::Kind::TcSwing && tc[2].value == 60);
        CHECK(shift_pad_job(1).kind == ShiftPadJob::Kind::Action && shift_pad_job(13).kind == ShiftPadJob::Kind::Open);
    }
}

// Parity phase 3: Step Edit's view of a step, and Motion on a kit pad's own values.
void step_edit_and_pad_motion() {
    // The events starting in a step of Q, and the selection kept inside it.
    Pattern p;
    p.length = 4 * ticks_per_quarter;
    for (auto e : {Event {0, 24, 36, 100, 100, 0}, Event {24, 24, 38, 90, 100, 0}, Event {24, 12, 37, 80, 100, 0}, Event {31, 6, 40, 70, 100, 0}, Event {48, 24, 36, 60, 100, 0}})
        p.add(e);
    auto in = step_events(p, 1);
    CHECK(in.first == 1 && in.count == 3 && p.events[1].key == 37 && p.events[3].tick == 31);
    CHECK(step_events(p, 3).count == 0 && step_events(p, 2).count == 1);
    UiState ui;
    ui.step_cursor = 1; ui.step_event = 9; // past the step's last: its last
    CHECK(selected_event(p, ui) == 3);
    ui.step_cursor = 3;
    CHECK(selected_event(p, ui) == -1);
    p.division = Division::Eighth; // Q 48: step 0 holds four
    ui.step_cursor = 0; ui.step_event = 0;
    CHECK(step_events(p, 0).count == 4 && selected_event(p, ui) == 0);

    // Pad lanes: one lane per pad, apart from the same value on another pad and from engine lanes.
    unsigned level = motion_pad_param(PadParam::Level), tune = motion_pad_param(PadParam::Pitch);
    CHECK(level >= motion_fx_base + track_slots * 6 && pad_param_of(level) == int(PadParam::Level) && pad_param_of(prophet::Cutoff) < 0);
    Motion m;
    CHECK(m.write({4, 0, 0, uint8_t(level), 20, 9}) && m.write({4, 0, 0, uint8_t(level), 90, 2}) && m.write({4, 0, 8, uint8_t(level), 64, 9}));
    CHECK(m.write({4, 0, 0, uint8_t(level), 30, 9}) && m.count == 3 && m.events[0].value == 30); // the same place: replaced
    CHECK(m.has(4, 0, uint8_t(level), 9) && m.has(4, 0, uint8_t(level), 2) && !m.has(4, 0, uint8_t(level), 3) && !m.has(4, 0, uint8_t(tune), 9));
    m.clear(4, 0, uint8_t(level), 9);
    CHECK(m.count == 1 && m.events[0].pad == 2);

    // The kit's lanes on Motion: the last pad's recorded ones first, then its tune, level and pan.
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    load_sound(project->tracks[4], EngineKind::Sampler, 0);
    auto& kit = project->tracks[4];
    kit.pads[9].sample = 0; kit.pads[9].level = 100; kit.pads[2].sample = 0;
    kit.last_pad = 9;
    project->motion.write({4, 0, 4, uint8_t(level), 0, 9});
    project->motion.write({4, 0, 2, uint8_t(motion_pad_param(PadParam::Attack)), 50, 2}); // pad 3's: not shown while pad 10 is the last
    UiState kit_ui;
    kit_ui.track = 4;
    auto lanes = motion_lanes(*project, kit_ui);
    CHECK(lanes[0] == motion_lane(level, 9) && lanes[1] == motion_lane(tune, 9) && lanes[2] == motion_lane(motion_pad_param(PadParam::Pan), 9));
    CHECK(lane_name(kit, lanes[0]) == "A10 LEVEL" && motion_param(kit, lanes[0]).value == 100);
    auto card = lane_binding(kit, 4, lanes[0]);
    CHECK(card.kind == Binding::Kind::PadLevel && card.max == 127 && card.value == 100);
    CHECK(motion_param(kit, motion_lane(motion_pad_param(PadParam::VelocitySense), 9)).info); // parity phase 4 gave pads it
    CHECK(!motion_param(project->tracks[0], motion_lane(level, 9)).info);                      // not a kit
    kit.last_pad = 2;
    CHECK(motion_lanes(*project, kit_ui)[0] == motion_lane(motion_pad_param(PadParam::Attack), 2));
    kit.last_pad = 9;

    // The file: pad lanes in their own MOTP section, so a file without it (or an older build) keeps the rest.
    project->motion.write({0, 0, 3, uint8_t(prophet::Cutoff), 77});
    auto data = save_project(*project, *bank);
    auto back = std::make_unique<Project>();
    auto back_bank = std::make_unique<PatternBank>();
    CHECK(load_project(data, *back, *back_bank).empty() && back->motion.count == 3);
    CHECK(back->motion.has(4, 0, uint8_t(level), 9) && back->motion.has(4, 0, uint8_t(motion_pad_param(PadParam::Attack)), 2));
    CHECK(back->motion.has(0, 0, uint8_t(prophet::Cutoff)));
    auto older = data;
    bool renamed = false;
    for (size_t n = 0; n + 4 <= older.size() && !renamed; ++n)
        if (std::memcmp(&older[n], "MOTP", 4) == 0) { std::memcpy(&older[n], "XXXX", 4); renamed = true; }
    CHECK(renamed && load_project(older, *back, *back_bank).empty() && back->motion.count == 1 && back->motion.events[0].param == prophet::Cutoff);

    // The audio side: pad 10's level held at 0 from step 4 silences its hits there, and Stop gives it back.
    SampleData data_wav;
    data_wav.frames = 44100;
    data_wav.left.assign(44100, 0.5f); data_wav.right.assign(44100, 0.5f);
    for (unsigned n = 0; n < 44100; ++n) data_wav.left[n] = data_wav.right[n] = float(std::sin(n * 0.05) * 0.5);
    auto quiet = std::make_unique<Project>(*project);
    quiet->motion = Motion {};
    quiet->motion.write({4, 0, 4, uint8_t(level), 0, 9});
    quiet->tracks[4].pads[9].end = 2000; quiet->tracks[4].pads[9].play = PadPlay::OneShot;
    auto& beat = bank->at[4][0];
    beat = Pattern {};
    for (unsigned st : {0u, 8u}) beat.add({st * sixteenth_ticks, uint16_t(sixteenth_ticks), uint8_t(sampler_first_note + 9), 120, 100, 0});
    EngineHost host;
    host.load(*quiet, *bank);
    CHECK(host.offer_sample(0, &data_wav));
    host.send({Command::Type::Play});
    std::vector<int32_t> out(256);
    auto peak = [&](double from, double to) {
        static double done = 0;
        float most = 0;
        while (done < to) {
            host.render(out.data(), 128, 1);
            if (done >= from) for (auto v : out) most = std::max(most, std::fabs(v / 2147483648.0f));
            done += 128;
        }
        return most;
    };
    double step = 5512.5; // a sixteenth at 120 BPM
    float first = peak(0, step), second = peak(8 * step, 9 * step);
    CHECK(first > 0.01f && second < first * 0.05f);
    float again = peak(16 * step, 17 * step); // the pattern's start: its own level back
    CHECK(again > first * 0.5f);
    const SampleData* gone = nullptr;
    while (host.take_retired(gone)) {}
}

// Parity phase 4: a kit pad as deep as the MPC's Sample Mode.
void sample_mode() {
    // Volume in dB as the MPC shows it.
    CHECK(volume_db(100) == 0 && std::fabs(volume_db(1) + 74) < 0.01f && std::fabs(volume_db(127) - 6) < 0.01f && volume_gain(0) == 0);
    CHECK(decibels(0) == "-inf" && decibels(127) == "+6.0" && decibels(100) == "0.0");

    // A second of two sines: a low one on the left, a high one on the right.
    SampleData data;
    data.frames = 44100;
    data.left.resize(44100); data.right.resize(44100);
    for (unsigned n = 0; n < 44100; ++n) { data.left[n] = float(std::sin(n * 0.02) * 0.5); data.right[n] = float(std::sin(n * 1.0) * 0.5); }
    SamplerEngine::Table table {};
    table[0] = &data;
    const int key = int(sampler_first_note);
    // Renders `blocks` of 128; the peak of each side from block `from` on.
    auto run = [&](SamplerEngine& e, unsigned blocks, unsigned from = 0) {
        std::vector<float> l(128), r(128);
        std::pair<float, float> peak {0, 0};
        for (unsigned b = 0; b < blocks; ++b) {
            std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
            e.render_stereo(l.data(), r.data(), 128);
            if (b >= from) for (unsigned f = 0; f < 128; ++f) {
                CHECK(std::isfinite(l[f]) && std::isfinite(r[f]));
                peak.first = std::max(peak.first, std::fabs(l[f])); peak.second = std::max(peak.second, std::fabs(r[f]));
            }
        }
        return peak;
    };
    auto engine_with = [&](const PadSound& pad, unsigned at = 0) {
        auto e = std::make_unique<SamplerEngine>();
        e->set_samples(&table);
        e->set_pad(at, pad);
        return e;
    };
    PadSound base;
    base.sample = 0; base.end = 4410; base.play = PadPlay::OneShot;

    { // Loop: a one shot loop plays past its end, and a second hit stops it.
        PadSound pad = base; pad.loop = true;
        auto e = engine_with(pad);
        e->note_on(key, 127);
        CHECK(run(*e, 60, 50).first > 0.1f && e->sounding());
        e->note_off(key);
        CHECK(e->sounding()); // a one shot loop plays on without the pad
        e->note_on(key, 127);  // hit again: it stops
        run(*e, 200);
        CHECK(!e->sounding());
        auto plain = engine_with(base);
        plain->note_on(key, 127);
        run(*plain, 40);
        CHECK(!plain->sounding());
        pad.reverse = true; pad.loop_lock = false; pad.loop_start = 2000; // reversed, from the end to the loop point and round
        auto back = engine_with(pad);
        back->note_on(key, 127);
        CHECK(run(*back, 60, 50).first > 0.1f && back->sounding());
    }
    { // Offset: 100 % waits a sixteenth (5512 frames at 120 BPM).
        PadSound pad = base; pad.offset = 100;
        auto e = engine_with(pad);
        e->set_tempo(1200);
        e->note_on(key, 127);
        CHECK(run(*e, 42).first == 0.0f);
        CHECK(run(*e, 10).first > 0.1f);
    }
    { // Pad Link: pad 1 plays pad 3 of its bank too, and its note off lets a Note On pad go.
        PadSound link; link.link = 3;
        PadSound target = base; target.play = PadPlay::Gate; target.end = 40000;
        auto e = engine_with(link);
        e->set_pad(2, target);
        e->note_on(key, 127);
        CHECK(run(*e, 4).first > 0.1f);
        e->note_off(key);
        run(*e, 400);
        CHECK(!e->sounding());
    }
    { // Mono: a second hit stops the first; Poly lets them sum.
        PadSound pad = base; pad.end = 40000;
        auto poly = engine_with(pad);
        poly->note_on(key, 127); poly->note_on(key, 127);
        float both = run(*poly, 8, 2).first;
        pad.mono = true;
        auto mono = engine_with(pad);
        mono->note_on(key, 127); mono->note_on(key, 127);
        float one = run(*mono, 8, 2).first;
        CHECK(both > one * 1.7f && one > 0.1f);
    }
    { // Velocity sensitivity: at 0 a soft hit is as loud as a hard one.
        PadSound pad = base;
        auto soft = engine_with(pad), hard = engine_with(pad);
        soft->note_on(key, 10); hard->note_on(key, 127);
        CHECK(run(*soft, 8).first < run(*hard, 8).first * 0.5f);
        pad.velocity_sense = 0;
        auto deaf = engine_with(pad);
        deaf->note_on(key, 10);
        CHECK(std::fabs(run(*deaf, 8).first - run(*engine_with(base), 0).first) >= 0 && run(*deaf, 8).first > 0.45f * 0.7071f); // a centred pad: -3 dB a side
    }
    { // Decay from the end: whole until the decay before the end; from the start it falls at once.
        PadSound pad = base; pad.end = 22050; pad.decay = 30;
        auto end = engine_with(pad);
        end->note_on(key, 127);
        float middle = run(*end, 90, 80).first;
        pad.decay_from_start = true; pad.sustain = 0;
        auto start = engine_with(pad);
        start->note_on(key, 127);
        float fallen = run(*start, 90, 80).first;
        CHECK(middle > 0.4f * 0.7071f && fallen < middle * 0.3f); // a centred pad: -3 dB a side
    }
    { // The pad filter: a low pass takes the high sine away and keeps the low one; a high pass the other way.
        PadSound pad = base; pad.end = 40000; pad.filter = PadFilter::Lpf4; pad.cutoff = 50;
        auto low = engine_with(pad);
        low->note_on(key, 127);
        auto lp = run(*low, 30, 10);
        CHECK(lp.first > 0.3f && lp.second < 0.05f);
        pad.filter = PadFilter::Hpf2; pad.cutoff = 70;
        auto high = engine_with(pad);
        high->note_on(key, 127);
        auto hp = run(*high, 30, 10);
        CHECK(hp.second > 0.3f && hp.first < 0.1f);
        pad.filter = PadFilter::Classic; pad.cutoff = 20; pad.resonance = 100; // rings, but stays finite and bounded
        auto classic = engine_with(pad);
        classic->note_on(key, 127);
        auto cl = run(*classic, 60);
        CHECK(cl.first < 4 && cl.second < 4);
        // The envelope opens a closed filter: with depth the high sine comes through at the attack's top.
        pad.filter = PadFilter::Lpf2; pad.resonance = 0; pad.cutoff = 10; pad.filter_amount = 100; pad.filter_attack = 0; pad.filter_decay = 100;
        auto opened = engine_with(pad);
        opened->note_on(key, 127);
        CHECK(run(*opened, 10, 2).second > 0.2f * 0.7071f); // a centred pad: -3 dB a side
        // 16 Levels' cutoff on a pad without a filter: Classic at that cutoff.
        auto levels = engine_with(base);
        levels->play_pad(0, 127, 0, 5);
        CHECK(run(*levels, 10, 2).second < 0.05f);
    }
    { // Warp: 200 % by Pitch (Tape) plays twice as long.
        PadSound pad = base; pad.warp = 200;
        auto e = engine_with(pad);
        e->set_param(sampler::FollowHow, 2);
        e->note_on(key, 127);
        run(*e, 50);
        CHECK(e->sounding());
        run(*e, 30);
        CHECK(!e->sounding());
        pad.warp = warp_seq; pad.beats = 1; // a beat: 4410 frames is 600 BPM, at 120 five times as long
        auto seq = engine_with(pad);
        seq->set_param(sampler::FollowHow, 2); seq->set_tempo(1200);
        seq->note_on(key, 127);
        run(*seq, 150);
        CHECK(seq->sounding());
    }
    { // Motion on the new pad values reaches the voice: offset moved to 100 waits a sixteenth.
        auto e = engine_with(base);
        e->set_tempo(1200);
        e->move_pad(0, PadParam::Offset, 100);
        e->note_on(key, 127);
        CHECK(run(*e, 42).first == 0.0f);
        e->restore_pads();
    }

    // 16 Levels: Tune has the original on pad 4, Filter sweeps up to the pad's cutoff by pad 8.
    auto project = std::make_unique<Project>();
    load_sound(project->tracks[4], EngineKind::Sampler, 0);
    auto& kit = project->tracks[4];
    kit.pads[2].sample = 0; kit.pads[2].cutoff = 60; kit.levels_pad = 2;
    kit.levels_kind = uint8_t(LevelsKind::Tune);
    CHECK(levels_of(*project, kit, 3).semitones == 0 && levels_of(*project, kit, 0).semitones == -3 && levels_of(*project, kit, 15).semitones == 12);
    kit.levels_kind = uint8_t(LevelsKind::Filter);
    CHECK(levels_of(*project, kit, 0).cutoff == 0 && levels_of(*project, kit, 7).cutoff == 60 && levels_of(*project, kit, 15).cutoff == 100);
    CHECK(levels_of(*project, project->tracks[0], 5).velocity == level_velocity(5)); // a synth: Filter is soft to loud

    // Cards: Trim's Shift row zooms at each handle; Play's Shift + K2 Pad Link; Warp's card both ways.
    kit.last_pad = 2; kit.pads[2].end = 30000; project->samples[0].frames = 44100;
    UiState ui;
    ui.track = 4; ui.page = PageId::Trim;
    Pattern none;
    auto trim = page_bindings(*project, ui, none, false);
    CHECK(trim[2].kind == Binding::Kind::PadLoopPoint && trim[2].value == 0);
    auto zoom = page_bindings(*project, ui, none, true);
    CHECK(zoom[0].kind == Binding::Kind::TrimZoom && zoom[2].id == 2);
    ui.page = PageId::PlayPage;
    CHECK(page_bindings(*project, ui, none, true)[1].kind == Binding::Kind::PadLink);
    ui.page = PageId::PadMix;
    CHECK(page_bindings(*project, ui, none, true)[1].kind == Binding::Kind::Param && page_bindings(*project, ui, none, true)[1].id == sampler::Level);
    CHECK(warp_of_card(warp_card(150)) == 150 && warp_of_card(warp_card(warp_seq)) == warp_seq && warp_of_card(0) == warp_off && warp_card(50) == 1);
    for (unsigned p = 0; p < pad_param_count; ++p) CHECK(pad_param_built(PadParam(p)));

    // The file: PAD4 keeps every new value; a file without it decays from the start, as it did.
    auto& pad = kit.pads[2];
    pad.loop = true; pad.loop_lock = false; pad.loop_start = 1234; pad.mono = true; pad.link = 7; pad.offset = 33; pad.velocity_sense = 44;
    pad.filter = PadFilter::Bpf4; pad.resonance = 21; pad.filter_attack = 1; pad.filter_decay = 2; pad.filter_sustain = 3; pad.filter_release = 4;
    pad.filter_amount = 5; pad.warp = 175; pad.beats = 16; pad.gain = 612; pad.group = 16;
    auto bank = std::make_unique<PatternBank>();
    auto data_file = save_project(*project, *bank);
    auto back = std::make_unique<Project>();
    auto back_bank = std::make_unique<PatternBank>();
    CHECK(load_project(data_file, *back, *back_bank).empty());
    const auto& b = back->tracks[4].pads[2];
    CHECK(b.loop && !b.loop_lock && b.loop_start == 1234 && b.mono && b.link == 7 && b.offset == 33 && b.velocity_sense == 44);
    CHECK(b.filter == PadFilter::Bpf4 && b.cutoff == 60 && b.resonance == 21 && b.filter_attack == 1 && b.filter_decay == 2);
    CHECK(b.filter_sustain == 3 && b.filter_release == 4 && b.filter_amount == 5 && b.warp == 175 && b.beats == 16 && b.gain == 612);
    CHECK(b.group == 16 && !b.decay_from_start);
    bool renamed = false;
    for (size_t n = 0; n + 4 <= data_file.size() && !renamed; ++n)
        if (std::memcmp(&data_file[n], "PAD4", 4) == 0) { std::memcpy(&data_file[n], "XXXX", 4); renamed = true; }
    CHECK(renamed && load_project(data_file, *back, *back_bank).empty());
    CHECK(back->tracks[4].pads[2].decay_from_start && !back->tracks[4].pads[2].loop && back->tracks[4].pads[2].used());
}

// Parity phase 5: Chop on a region (types, slice start / end, Split, Merge,
// Manual's limit and its taps pulled onto hits), the cards and their zoom,
// the preview slot in the engine, Trim Sample as a Shift + pad action.
void chop_and_browser() {
    std::array<Hit, max_hits> hits {};
    unsigned found = 0;
    for (uint32_t at : {0u, 10000u, 20000u, 30000u, 40000u, 50000u}) hits[found++] = {at, uint8_t(at == 30000 ? 80 : 200)};
    const uint32_t frames = 60000;
    // Regions between a pad's start and end; slices touch, the last ends at the region's end.
    auto regions = chop(ChopMode::Regions, 4, 8000, 48000, frames, hits, found, 1200);
    CHECK(regions.count == 4 && regions.start[0] == 8000 && regions.start[1] == 18000 && regions.end(3, frames) == 48000);
    // Threshold: hits inside the region at least as strong as it asks (30000 is faint).
    auto strong = chop(ChopMode::Threshold, 70, 5000, 0, frames, hits, found, 1200);
    CHECK(strong.count == 5 && strong.start[0] == 5000 && strong.start[1] == 10000 && strong.start[3] == 40000);
    CHECK(chop(ChopMode::Threshold, 0, 5000, 0, frames, hits, found, 1200).count == 6);
    CHECK(chop(ChopMode::Threshold, 100, 5000, 0, frames, hits, found, 1200).count == 1); // higher, fewer (as on the MPC)
    // Beats at 120 BPM: a slice a beat (22050 frames) from the region's start.
    auto beats = chop(ChopMode::Beats, 1, 1000, 0, frames, hits, found, 1200);
    CHECK(beats.count == 3 && beats.start[1] == 23050 && beats.start[2] == 45100);
    // Manual starts as one slice, the whole region; up to 16 cuts.
    auto manual = chop(ChopMode::Manual, 0, 2000, 50000, frames, hits, found, 1200);
    CHECK(manual.count == 1 && manual.end(0, frames) == 50000);
    for (uint32_t at = 3000; at < 49000; at += 1000) manual.add(at, frames, manual_slices);
    CHECK(manual.count == manual_slices);
    // K1 / K2: a slice's start is the slice before's end; its end the next one's start; never under 20 ms.
    auto s = regions;
    s.move_end(1, 30000, frames);
    CHECK(s.start[2] == 30000 && s.end(1, frames) == 30000);
    s.move_start(1, 0, frames);
    CHECK(s.start[1] == 8000 + min_slice);
    s.move_end(3, 59000, frames);
    CHECK(s.end(3, frames) == 59000);
    s.move_end(2, 59500, frames); // up against the last slice: it keeps 20 ms
    CHECK(s.start[3] == 59000 - min_slice);
    // Split and Merge.
    auto m = regions;
    CHECK(m.split(1, frames) && m.count == 5 && m.start[2] == 23000);
    m.remove(2); // Merge slice 3 into slice 2
    CHECK(m.count == 4 && m.end(1, frames) == 28000);
    m.remove(3); // the last: the slice before now ends where it did
    CHECK(m.count == 3 && m.end(2, frames) == 48000);
    CHECK(!m.add(28100, frames)); // too close to a cut
    Slices tiny = chop(ChopMode::Manual, 0, 0, 1500, frames, hits, found, 1200);
    CHECK(!tiny.split(0, frames));
    // A tap a little after a hit lands on it; one far from any stays put.
    CHECK(snap_to_hit(41200, hits, found) == 40000);
    CHECK(snap_to_hit(45000, hits, found) == 45000);
    CHECK(snap_to_hit(39800, hits, found) == 40000); // up to 10 ms early

    // The cards: K1 start, K2 end, K3 type; Shift: zoom at each, the type's amount (none for Manual).
    Project project;
    load_sound(project.tracks[0], EngineKind::Sampler, 0);
    project.samples[0].frames = 441000; std::snprintf(project.samples[0].path, sizeof(project.samples[0].path), "/x.wav");
    UiState ui;
    Pattern pattern;
    ui.page = PageId::Chop; ui.chop_sample = 0;
    ui.slices = chop(ChopMode::Regions, 4, 0, 0, 441000, hits, 0, 1200);
    ui.chop_mode = ChopMode::Regions; ui.chop_slice = 1;
    auto cards = page_bindings(project, ui, pattern);
    CHECK(cards[0].kind == Binding::Kind::ChopStart && cards[0].value == 2500 && cards[0].min == 20 && cards[0].max == 4980);
    CHECK(cards[1].kind == Binding::Kind::ChopEnd && cards[1].value == 5000 && cards[1].max == 7480);
    CHECK(cards[2].kind == Binding::Kind::ChopMode && cards[2].max == 3);
    ui.chop_zoom = 3; // an eighth of the sample (1250 ms) round each handle
    cards = page_bindings(project, ui, pattern);
    CHECK(cards[0].min == 2500 && cards[0].max == 3750 && cards[1].min == 5000 && cards[1].max == 6250);
    auto shifted = page_bindings(project, ui, pattern, true);
    CHECK(shifted[0].kind == Binding::Kind::ChopZoom && shifted[0].id == 0 && shifted[1].id == 1);
    CHECK(shifted[2].kind == Binding::Kind::ChopAmount && shifted[2].max == int(max_slices));
    ui.chop_mode = ChopMode::Manual;
    CHECK(!shift_bindings(project, ui, pattern)[2].bound() && page_bindings(project, ui, pattern, true)[2].kind == Binding::Kind::ChopMode);

    // The preview slot: the engine plays a file no pad holds; a stop ends it.
    SampleData data;
    data.frames = 44100; data.left.assign(44100, 0.5f); data.right.assign(44100, 0.5f);
    SamplerEngine::Table table {};
    table[preview_slot] = &data;
    SamplerEngine engine;
    engine.set_samples(&table);
    engine.audition(uint16_t(preview_slot), 0, 0, 100);
    std::vector<float> l(128, 0), r(128, 0);
    engine.render_stereo(l.data(), r.data(), 128);
    CHECK(engine.sounding());
    engine.audition(no_sample, 0, 0, 0);
    for (unsigned b = 0; b < 40; ++b) engine.render_stereo(l.data(), r.data(), 128);
    CHECK(!engine.sounding());

    // Trim Sample is a Shift + pad action now.
    CHECK(shift_pad_job(12).kind == ShiftPadJob::Kind::Action && std::string(shift_pad_job(12).name) == "Trim Sample");
}


// Parity phase 6: Input Config, the input path, Sample Record's Seq take,
// Audio Recall's ring, renders of the mix and Song -> new pattern.
// The meter lights beside the screen: the engine's left and right levels (the input source, and the mix before
// main volume), falling 30 dB a second, and the LEDs a strip lights for them.
void meter_lights() {
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    for (auto& t : project->tracks) t.engine = EngineKind::None;
    auto host = std::make_unique<EngineHost>();
    host->load(*project, *bank);
    std::vector<int32_t> input(256), silent(256), out(256);
    for (unsigned f = 0; f < 128; ++f) { input[2 * f] = int32_t(0.5 * 2147483647.0); input[2 * f + 1] = int32_t(0.25 * 2147483647.0); }
    host->send({Command::Type::RecordSource, uint8_t(InputSource::Rear)});
    host->send({Command::Type::Monitor, 0, 0, 0, 1});
    for (unsigned n = 0; n < 4; ++n) host->render(out.data(), 128, 2, input.data());
    CHECK(std::fabs(host->in_level(0) - 0.5f) < 1e-3f && std::fabs(host->in_level(1) - 0.25f) < 1e-3f);
    CHECK(std::fabs(host->out_level(0) - 0.5f) < 1e-3f && std::fabs(host->out_level(1) - 0.25f) < 1e-3f); // main volume 2 unheard
    host->send({Command::Type::Monitor, 0, 0, 0, 0});
    for (unsigned n = 0; n < unsigned(dsp::sample_rate()) / 128; ++n) host->render(out.data(), 128, 2, silent.data());
    float fell = 20 * std::log10(host->in_level(0) / 0.5f), fell_out = 20 * std::log10(host->out_level(1) / 0.25f);
    CHECK(fell < -29 && fell > -31 && fell_out < -29 && fell_out > -31);

    // A bar up to the level, nothing held. The input's steps (-42, -30, -18, -9, -2 dBFS); the mix's: a synth's
    // median (-22) two greens, its loudest (-9) yellow, the 909's loudest (-6) red.
    auto at = [](float db) { return std::pow(10.0f, db / 20); };
    const auto& in = meter_input_db;
    CHECK(meter_lit(0, in) == 0 && meter_lit(at(-45), in) == 0);
    CHECK(meter_lit(at(-35), in) == 0b00001 && meter_lit(at(-15), in) == 0b00111 && meter_lit(at(-5), in) == 0b01111);
    CHECK(meter_lit(at(0), in) == surface_meter_all && meter_lit(at(6), in) == surface_meter_all);
    CHECK(meter_lit(at(-22), meter_mix_db) == 0b00011 && meter_lit(at(-9), meter_mix_db) == 0b01111);
    CHECK(meter_lit(at(-6), meter_mix_db) == surface_meter_all);
}

void audio_input_and_rendering() {
    // Input Config: Auto hears the source only on Record; the mix is never monitored; the monitored mic quiets the speaker.
    InputConfig in;
    in.source = InputSource::Mic; in.monitor = Monitor::Auto;
    CHECK(in.monitoring(true) && !in.monitoring(false) && in.mutes_speaker(true) && !in.mutes_speaker(false));
    in.monitor = Monitor::On; CHECK(in.monitoring(false) && in.mutes_speaker(false));
    in.source = InputSource::Rear; CHECK(in.monitoring(false) && !in.mutes_speaker(false));
    in.source = InputSource::Resample; CHECK(!in.monitoring(true));
    in.monitor = Monitor::Off; in.source = InputSource::Usb; CHECK(!in.monitoring(true));
    CHECK(std::string(input_route(InputSource::Mic)) == "mic" && std::string(input_route(InputSource::RearR)) == "line"
          && std::string(input_route(InputSource::UsbL)) == "off" && std::string(input_route(InputSource::Resample)) == "off");
    CHECK(rec_bars(rec_free) == 0 && rec_bars(rec_seq) == 0 && rec_bars(2) == 1 && rec_bars(rec_lengths - 1) == 16);
    // Kept with the settings; an older settings file (no input keys) gets the defaults.
    Settings saved;
    saved.input.source = InputSource::UsbR; saved.input.monitor = Monitor::On; saved.input.threshold = -72;
    saved.input.length = rec_seq; saved.input.fx = true; saved.input.gain = -6;
    Settings back; back.read(saved.text());
    CHECK(back.input.source == InputSource::UsbR && back.input.monitor == Monitor::On && back.input.threshold == -72
          && back.input.length == rec_seq && back.input.fx && back.input.gain == -6);
    Settings old; old.read("pad_response=2\npickup=1\n");
    CHECK(old.input.source == InputSource::Rear && old.input.monitor == Monitor::Auto && old.input.threshold == -30 && old.input.length == rec_free);
    Settings wild; wild.read("input_source=99\nthreshold=-200\nrec_length=99\n");
    CHECK(wild.input.source == InputSource::Track && wild.input.threshold == -96 && wild.input.length == rec_lengths - 1);

    // The engine host's input path: each source's channels, the gain, the meter, and Monitor.
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    for (auto& t : project->tracks) t.engine = EngineKind::None;
    auto host = std::make_unique<EngineHost>();
    host->load(*project, *bank);
    std::vector<int32_t> input(256), out(256);
    for (unsigned f = 0; f < 128; ++f) { input[2 * f] = int32_t(0.5 * 2147483647.0); input[2 * f + 1] = int32_t(0.25 * 2147483647.0); }
    std::vector<float> usb(256);
    for (unsigned f = 0; f < 128; ++f) { usb[2 * f] = 0.125f; usb[2 * f + 1] = -0.0625f; }
    auto level_of = [&](InputSource source, int gain) {
        host->send({Command::Type::RecordSource, uint8_t(source)});
        host->send({Command::Type::RecordGain, 0, 0, 0, int16_t(gain)});
        for (unsigned n = 0; n < 40; ++n) host->render(out.data(), 128, 1, input.data(), nullptr, usb.data());
        return host->recorder().level();
    };
    CHECK(std::fabs(level_of(InputSource::Rear, 0) - 0.5f) < 1e-3f && host->record_source() == unsigned(InputSource::Rear));
    CHECK(std::fabs(level_of(InputSource::Mic, 0) - 0.375f) < 0.01f);  // the mic: both sides averaged (meter falling from 0.5)
    CHECK(std::fabs(level_of(InputSource::UsbL, 0) - 0.125f) < 0.02f);
    CHECK(level_of(InputSource::RearR, 6) > 0.49f && level_of(InputSource::RearR, 6) < 0.51f); // 0.25 at +6 dB
    level_of(InputSource::Rear, 0);
    host->send({Command::Type::Monitor, 0, 0, 0, 1});
    host->render(out.data(), 128, 1, input.data());
    CHECK(host->monitoring() && out[0] > int32_t(0.3 * 2147483647.0)); // the line input heard (soft clipped)
    host->send({Command::Type::Monitor, 0, 0, 0, 0});
    host->render(out.data(), 128, 1, input.data());
    CHECK(out[0] == 0);
    host->send({Command::Type::RecordSource, uint8_t(InputSource::Resample)});
    host->send({Command::Type::Monitor, 0, 0, 0, 1});
    host->render(out.data(), 128, 1, input.data());
    CHECK(out[0] == 0); // the mix is never monitored onto itself
    host->send({Command::Type::Monitor, 0, 0, 0, 0});

    // Audio Recall's ring: always filling from the source, the last 25 s copied out; a new source starts it again.
    auto ring = std::make_unique<RecallRing>();
    std::vector<float> ramp_l(128), ramp_r(128);
    for (uint32_t block = 0; block < 27 * 44100 / 128; ++block) {
        for (unsigned f = 0; f < 128; ++f) { ramp_l[f] = float(block * 128 + f); ramp_r[f] = -ramp_l[f]; }
        ring->write(ramp_l.data(), ramp_r.data(), 128);
    }
    uint32_t written = 27 * 44100 / 128 * 128;
    CHECK(ring->filled() == 25 * 44100);
    std::vector<float> l(25 * 44100), r(25 * 44100);
    CHECK(ring->copy_last(25 * 44100, l.data(), r.data()));
    CHECK(l.back() == float(written - 1) && l.front() == float(written - 25 * 44100) && r.back() == -l.back());
    ring->restart();
    CHECK(ring->filled() == 0);
    ring->write(ramp_l.data(), ramp_r.data(), 128);
    CHECK(ring->filled() == 128);
    // The host fills its ring from the source, and starts it again when the source changes.
    host->send({Command::Type::RecordSource, uint8_t(InputSource::Rear)});
    for (unsigned n = 0; n < 10; ++n) host->render(out.data(), 128, 1, input.data());
    CHECK(host->recall().filled() == 1280);
    host->send({Command::Type::RecordSource, uint8_t(InputSource::RearL)});
    host->render(out.data(), 128, 1, input.data());
    CHECK(host->recall().filled() == 128);

    // A render of the patterns: from Play, exactly the pattern's length (a bar at 120 BPM: 88200 frames), then done.
    load_sound(project->tracks[0], EngineKind::Prophet, 0);
    bank->at[0][0].length = project->bar_ticks();
    bank->at[0][0].add({0, 24, 57, 110, 100, 0});
    bank->at[0][0].add({192, 24, 60, 110, 100, 0});
    host = std::make_unique<EngineHost>();
    host->load(*project, *bank);
    Command arm {Command::Type::BounceArm, 0, 0, 0, 0};
    arm.position = project->bar_ticks();
    host->send(arm);
    host->send({Command::Type::Play, 0});
    std::vector<float> taken(2 * 4096);
    uint32_t got = 0;
    float loudest = 0;
    for (unsigned n = 0; n < 1000 && host->bounce().state() != Bounce::State::Done; ++n) {
        host->render(out.data(), 128, 1, nullptr);
        for (unsigned k; (k = host->bounce().pop(taken.data(), 4096)) > 0; got += k)
            for (unsigned f = 0; f < 2 * k; ++f) loudest = std::max(loudest, std::fabs(taken[f]));
    }
    for (unsigned k; (k = host->bounce().pop(taken.data(), 4096)) > 0;) got += k;
    CHECK(host->bounce().state() == Bounce::State::Done && got + 1 >= 88200 && got <= 88201 && host->bounce().frames() == got && loudest > 0.01f);
    CHECK(host->playing()); // the shell stops it (or lets it play on)
    host->bounce().release();
    CHECK(host->bounce().state() == Bounce::State::Idle);
    // Stop meanwhile drops it.
    host->send({Command::Type::Stop});
    host->send(arm);
    host->send({Command::Type::Play, 0});
    for (unsigned n = 0; n < 50; ++n) host->render(out.data(), 128, 1, nullptr);
    CHECK(host->bounce().state() == Bounce::State::Running);
    host->send({Command::Type::Stop});
    host->render(out.data(), 128, 1, nullptr);
    CHECK(host->bounce().state() == Bounce::State::Cancelled);
    while (host->bounce().pop(taken.data(), 4096)) {}
    host->bounce().release();

    // The song's render: once through (the loop brace ignored), then its tail until the mix falls silent.
    Clip clip; clip.track = 0; clip.pattern = 0; clip.bar = 0; clip.bars = 2;
    project->song.place(clip);
    project->song.loop_start = 0; project->song.loop_end = 1;
    host = std::make_unique<EngineHost>();
    host->load(*project, *bank);
    host->send({Command::Type::BounceArm, 0, 0, 0, 1});
    host->send({Command::Type::Play, 0, 0, 0, 1});
    bool tail = false;
    for (unsigned n = 0; n < 6000 && host->bounce().state() != Bounce::State::Done; ++n) {
        host->render(out.data(), 128, 1, nullptr);
        tail |= host->bounce().state() == Bounce::State::Tail;
        while (host->bounce().pop(taken.data(), 4096)) {}
    }
    uint32_t song_frames = host->bounce().frames();
    CHECK(tail && host->bounce().state() == Bounce::State::Done && !host->playing());
    CHECK(song_frames >= 2 * 88200 && song_frames <= 2 * 88200 + Bounce::tail_limit + 128);
    host->bounce().release();

    // Rec length Seq while playing: the take waits for the pattern to come round, then runs its length.
    host = std::make_unique<EngineHost>();
    project->song = Song {};
    host->load(*project, *bank);
    host->send({Command::Type::Play, 0});
    for (unsigned n = 0; n < 100; ++n) host->render(out.data(), 128, 1, input.data()); // into the bar
    Command seq {Command::Type::RecordArm, uint8_t(InputSource::Resample), 255, 3, -96};
    seq.position = 0;
    host->send(seq);
    host->render(out.data(), 128, 1, input.data());
    CHECK(host->recorder().state() == TakeRecorder::State::Cued && host->recorder().frames() == 0);
    unsigned blocks = 1;
    while (host->recorder().state() == TakeRecorder::State::Cued && blocks < 1000) { host->render(out.data(), 128, 1, input.data()); ++blocks; }
    CHECK(host->recorder().state() == TakeRecorder::State::Recording && blocks + 100 >= 88200 / 128 && blocks + 100 <= 88200 / 128 + 2);
    for (unsigned n = 0; n < 800 && host->recorder().state() == TakeRecorder::State::Recording; ++n) host->render(out.data(), 128, 1, input.data());
    CHECK(host->recorder().state() == TakeRecorder::State::Done && host->recorder().frames() == 88200);
    host->recorder().release();
    // Stopped, Seq is the pattern's length from the threshold (here at once); Stop cancels a cued take.
    host->send({Command::Type::Stop});
    host->send(seq);
    host->render(out.data(), 128, 1, input.data());
    CHECK(host->recorder().state() == TakeRecorder::State::Recording && host->recorder().length() == 88200);
    host->send({Command::Type::RecordStop});
    host->render(out.data(), 128, 1, input.data());
    host->recorder().release();

    // The mixdown's file, written as it comes: read back, the same frames.
    {
        std::string path = "/tmp/melodee-wavstream-test.wav";
        WavStream wav;
        CHECK(wav.open(path));
        std::vector<float> chunk(2 * 1000);
        for (unsigned f = 0; f < 1000; ++f) { chunk[2 * f] = std::sin(f * 0.05f) * 0.5f; chunk[2 * f + 1] = -chunk[2 * f]; }
        CHECK(wav.add(chunk.data(), 1000) && wav.add(chunk.data(), 500) && wav.frames() == 1500 && wav.close());
        SampleData read;
        CHECK(read_wav(path, read).empty() && read.frames == 1500 && std::fabs(read.left[700] - chunk[1400]) < 1e-4f
              && std::fabs(read.right[1200] - chunk[2 * 200 + 1]) < 1e-4f);
        std::remove(path.c_str());
    }

    // Song -> new pattern: clips laid end to end, each repeating its pattern from its start; muted clips left out.
    Song song;
    Pattern beat; beat.length = 2 * 96; beat.add({0, 24, 36, 100, 100, 0}); beat.add({96, 24, 38, 100, 100, 0});
    beat.add({300, 24, 40, 100, 100, 0}); // past its end: never heard
    std::array<Pattern, pattern_count> lane {};
    lane[1] = beat;
    unsigned bar = 384;
    Clip a; a.track = 4; a.pattern = 1; a.bar = 1; a.bars = 2; song.place(a);
    Clip muted = a; muted.bar = 4; muted.bars = 1; muted.muted = true; song.place(muted);
    Clip other = a; other.track = 5; song.place(other);
    Pattern flat; bool full = false;
    CHECK(flatten_lane(song, 4, lane, bar, song.length(), flat, full) && !full);
    CHECK(flat.length == 5 * bar && flat.events.size() == 8); // two bars of a half-bar pattern: four times two notes
    CHECK(flat.events.front().tick == bar && flat.events.back().tick == 3 * bar - 96 && flat.events.back().key == 38);
    CHECK(!flatten_lane(song, 6, lane, bar, song.length(), flat, full));
}


// Parity phase 7: the new effects, Knob FX (tracks, kit pads, the input) and the master compressor.
void performance_effects() {
    // The picker: 41 effects, every one built, two columns of six at most a family; Knob FX's list: the MPC's 28 first.
    CHECK(effect_count() == 41);
    for (unsigned f = 0; f < unsigned(Family::Count); ++f) { FamilyList list {}; CHECK(family_effects(Family(f), list) <= 12); }
    for (unsigned k = 1; k < unsigned(EffectKind::Count); ++k) CHECK(effect_info(EffectKind(k)).built);
    CHECK(knob_fx_count() == 43 && knob_fx_mpc_count() == 28);
    CHECK(std::string(knob_fx_entry(0).name) == "Delay" && std::string(knob_fx_entry(27).name) == "Tape Emulator");
    CHECK(knob_fx_entry(8).kind == EffectKind::Filter && knob_fx_entry(8).param == 2 && knob_fx_entry(8).value == 2); // HP Filter
    for (unsigned e = 0; e < knob_fx_count(); ++e) CHECK(effect_info(knob_fx_entry(e).kind).built);
    // New formats.
    ParamInfo ms {"LEFT", 0, 250, 0, Format::Ms}, hz {"LOW CUT", 20, 1000, 0, Format::Hertz}, sec {"TIME", 10, 100, 0, Format::Seconds};
    CHECK(show(ms, 12).value == "12" && show(ms, 12).unit == "ms" && show(hz, 1000).unit == "kHz" && show(sec, 25).value == "2.5");

    DelayPool pool;
    auto run = [](Effect& e, std::vector<float>& l, std::vector<float>& r) {
        for (size_t at = 0; at < l.size(); at += 128) e.process(l.data() + at, r.data() + at, unsigned(std::min<size_t>(128, l.size() - at)));
    };
    // Sample delay: the right side late by its RIGHT, the left on time.
    {
        Effect e; e.set_kind(EffectKind::SampleDelay, pool);
        e.set_param(0, 0); e.set_param(1, 10);
        std::vector<float> l(2048, 0), r(2048, 0);
        l[100] = r[100] = 1;
        run(e, l, r);
        CHECK(l[100] > 0.99f && std::fabs(r[100]) < 1e-3f && r[100 + 441] > 0.99f);
        e.set_kind(EffectKind::None, pool);
    }
    // Pumper: on the beat the gain dips by DEPTH, between beats it is back.
    {
        Effect e; e.set_kind(EffectKind::Pumper, pool);
        e.set_param(2, 100); e.set_param(3, 0); e.set_param(4, 30); // full depth, no attack, hold
        effect_clock.quarter = 22050;
        std::vector<float> l(22050, 0.5f), r(22050, 0.5f);
        effect_clock.beats = 0;
        for (size_t at = 0; at < l.size(); at += 128) {
            effect_clock.beats = double(at) / 22050;
            e.process(l.data() + at, r.data() + at, unsigned(std::min<size_t>(128, l.size() - at)));
        }
        CHECK(l[1500] < 0.1f && l[21000] > 0.4f);
    }
    // Vintage SP1200: steps held for 44100 / 26040 samples, so a ramp comes out in stairs.
    {
        Effect e; e.set_kind(EffectKind::Vintage, pool); e.set_param(0, 2);
        std::vector<float> l(4096), r(4096);
        for (unsigned n = 0; n < 4096; ++n) l[n] = r[n] = float(n) / 4096 - 0.5f;
        run(e, l, r);
        CHECK(std::isfinite(l[4000]) && std::fabs(l[4000] - (4000.0f / 4096 - 0.5f)) < 0.1f);
    }

    // Knob FX on a track: a low pass at its bottom quietens it while heard; bypassed, the track is as it was.
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    load_sound(project->tracks[0], EngineKind::Prophet, 0);
    project->tracks[0].params[prophet::Cutoff] = 100;
    auto energy = [&](const std::function<void(EngineHost&)>& set) {
        EngineHost host;
        host.load(*project, *bank);
        set(host);
        host.send({Command::Type::NoteOn, 0, 0, 69, 110});
        std::vector<int32_t> out(256);
        double sum = 0;
        for (unsigned n = 0; n < 200; ++n) {
            host.render(out.data(), 128, 1);
            if (n > 40) for (auto v : out) sum += double(v / 2147483648.0f) * (v / 2147483648.0f);
        }
        return sum;
    };
    auto knob = [](EngineHost& host, EffectKind kind, std::initializer_list<std::pair<int, int>> params, bool heard, uint16_t tracks) {
        host.send({Command::Type::KnobFx, 0, 0, 0, int16_t(kind)});
        for (auto [q, v] : params) host.send({Command::Type::KnobFx, 0, uint8_t(q), 1, int16_t(v)});
        host.send({Command::Type::KnobFx, 0, 0, 2, int16_t(heard ? 1 : 0)});
        Command t {Command::Type::KnobFx, 0, 0, 3}; t.position = tracks; host.send(t);
    };
    double dry = energy([](EngineHost&) {});
    double filtered = energy([&](EngineHost& h) { knob(h, EffectKind::Filter, {{0, 0}, {1, 0}}, true, 1); });
    double other = energy([&](EngineHost& h) { knob(h, EffectKind::Filter, {{0, 0}, {1, 0}}, true, 2); });   // another track
    double off = energy([&](EngineHost& h) { knob(h, EffectKind::Filter, {{0, 0}, {1, 0}}, false, 1); });    // bypassed / off
    CHECK(filtered * 20 < dry && std::fabs(other - dry) < dry * 0.01 && std::fabs(off - dry) < dry * 0.01);

    // Knob FX on kit pads: pad 1 through it, pad 2 not.
    {
        auto kit = std::make_unique<Project>(*project);
        load_sound(kit->tracks[1], EngineKind::Sampler, 0);
        for (unsigned pad : {0u, 1u}) { kit->tracks[1].pads[pad].sample = 0; kit->tracks[1].pads[pad].play = PadPlay::OneShot; }
        SampleData tone;
        tone.frames = 44100; tone.left.resize(44100); tone.right.resize(44100);
        for (unsigned n = 0; n < 44100; ++n) tone.left[n] = tone.right[n] = float(std::sin(n * 0.3) * 0.5); // about 2.1 kHz
        auto hit = [&](unsigned pad, bool knobbed) {
            EngineHost host;
            host.load(*kit, *bank);
            CHECK(host.offer_sample(0, &tone));
            knob(host, EffectKind::Filter, {{0, 0}, {1, 0}}, true, 0);
            Command pads {Command::Type::KnobFx, 1, 0, 4}; pads.position = knobbed ? 1u : 0u; host.send(pads); // pad A01
            host.send({Command::Type::NoteOn, 1, 0, uint16_t(sampler_first_note + pad), 110});
            std::vector<int32_t> out(256);
            double sum = 0;
            for (unsigned n = 0; n < 100; ++n) { host.render(out.data(), 128, 1); if (n > 10) for (auto v : out) sum += double(v / 2147483648.0f) * (v / 2147483648.0f); }
            const SampleData* gone = nullptr;
            while (host.take_retired(gone)) {}
            return sum;
        };
        double a_dry = hit(0, false), a_knob = hit(0, true), b_knob = hit(1, true);
        CHECK(a_dry > 0 && a_knob * 20 < a_dry && std::fabs(b_knob - a_dry) < a_dry * 0.05);
        // Playing, the sequencer's ticks split the host's blocks: a pad through Knob FX (none chosen, so it passes
        // untouched) must come out frame for frame as it does dry, whole and in place, as when stopped.
        auto take = [&](bool knobbed, bool playing) {
            EngineHost host;
            host.load(*kit, *bank);
            CHECK(host.offer_sample(0, &tone));
            knob(host, EffectKind::None, {}, true, 0);
            Command pads {Command::Type::KnobFx, 1, 0, 4}; pads.position = knobbed ? 1u : 0u; host.send(pads); // pad A01
            if (playing) host.send({Command::Type::Play});
            host.send({Command::Type::NoteOn, 1, 0, uint16_t(sampler_first_note + 0), 110});
            std::vector<int32_t> out(256);
            std::vector<float> left;
            for (unsigned n = 0; n < 100; ++n) {
                host.render(out.data(), 128, 1);
                for (unsigned f = 0; f < 128; ++f) left.push_back(out[2 * f] / 2147483648.0f);
            }
            CHECK(host.playing() == playing);
            const SampleData* gone = nullptr;
            while (host.take_retired(gone)) {}
            return left;
        };
        auto energy_of = [](const std::vector<float>& s) { double e = 0; for (float v : s) e += double(v) * v; return e; };
        auto apart = [](const std::vector<float>& a, const std::vector<float>& b) {
            float most = 0;
            for (size_t f = 0; f < a.size(); ++f) most = std::max(most, std::fabs(a[f] - b[f]));
            return most;
        };
        auto still_dry = take(false, false), still_knob = take(true, false);
        auto run_dry = take(false, true), run_knob = take(true, true);
        double e_dry = energy_of(run_dry);
        CHECK(e_dry > 0 && apart(still_knob, still_dry) < 1e-4f);                                       // stopped: untouched
        CHECK(apart(run_dry, still_dry) < 1e-4f);                                                       // the ticks alone change nothing
        CHECK(std::fabs(energy_of(run_knob) - e_dry) < e_dry * 0.01 && apart(run_knob, run_dry) < 1e-4f); // playing: whole and in place
    }

    // Rec Input FX: the take hears Knob FX on the input (the mix here, Resample), the speakers do not.
    auto take = [&](bool rec_fx) {
        EngineHost host;
        host.load(*project, *bank);
        knob(host, EffectKind::Filter, {{0, 0}, {1, 0}}, true, 0);
        host.send({Command::Type::KnobFx, 0, 0, 2, int16_t(1 | (rec_fx ? 2 : 0))});
        host.send({Command::Type::RecordArm, uint8_t(InputSource::Resample), 1, 5, -96});
        host.send({Command::Type::NoteOn, 0, 0, 69, 110});
        std::vector<int32_t> out(256);
        for (unsigned n = 0; n < 700; ++n) host.render(out.data(), 128, 1, nullptr);
        double sum = 0;
        for (uint32_t f = 2000; f < host.recorder().frames(); ++f) sum += double(host.recorder().left()[f]) * host.recorder().left()[f];
        return sum;
    };
    double plain_take = take(false), fx_take = take(true);
    CHECK(plain_take > 0 && fx_take * 20 < plain_take);

    // The master compressor: engaged at full amount it takes the loud chord down, and says by how much.
    auto loud = [&](bool on, bool color) {
        EngineHost host;
        host.load(*project, *bank);
        host.send({Command::Type::Compressor, 0, 0, 4, 100});
        host.send({Command::Type::Compressor, 0, 0, 1, int16_t(color)});
        host.send({Command::Type::Compressor, 0, 0, 0, int16_t(on)});
        for (int key : {57, 60, 64, 69}) host.send({Command::Type::NoteOn, 0, 0, uint16_t(key), 127});
        std::vector<int32_t> out(256);
        float most = 0;
        for (unsigned n = 0; n < 300; ++n) {
            host.render(out.data(), 128, 1);
            if (n > 100) for (auto v : out) { most = std::max(most, std::fabs(v / 2147483648.0f)); CHECK(v != INT32_MIN); }
        }
        return std::pair<float, float>(most, host.compression());
    };
    auto uncompressed = loud(false, false), compressed = loud(true, false), coloured = loud(true, true);
    CHECK(uncompressed.second == 0 && compressed.second > 3 && coloured.second > 3 && coloured.first > 0.01f);

    // Sidechain: track 1 keys it; track 3 ducks under it, track 1 passes untouched, no makeup without the key,
    // and a muted key still ducks (a ghost key).
    {
        auto two = std::make_unique<Project>(*project);
        load_sound(two->tracks[2], EngineKind::Prophet, 0);
        two->tracks[2].params[prophet::Cutoff] = 100;
        struct Heard { double energy; float reduction; };
        auto side = [&](bool on, bool key_plays, bool other_plays, bool key_muted) {
            EngineHost host;
            host.load(*two, *bank);
            host.send({Command::Type::Compressor, 0, 0, 4, 100});
            host.send({Command::Type::Compressor, 0, 0, 7, 0});
            host.send({Command::Type::Compressor, 0, 0, 6, 1});
            host.send({Command::Type::Compressor, 0, 0, 0, int16_t(on)});
            if (key_muted) host.send({Command::Type::Mute, 0, 0, 0, 1});
            if (key_plays) for (int note : {45, 57}) host.send({Command::Type::NoteOn, 0, 0, uint16_t(note), 127});
            if (other_plays) for (int note : {64, 69}) host.send({Command::Type::NoteOn, 2, 0, uint16_t(note), 110});
            std::vector<int32_t> out(256);
            double sum = 0;
            for (unsigned n = 0; n < 200; ++n) {
                host.render(out.data(), 128, 1);
                if (n > 40) for (auto v : out) sum += double(v / 2147483648.0f) * (v / 2147483648.0f);
            }
            return Heard {sum, host.compression()};
        };
        auto key_dry = side(false, true, false, false), key_keyed = side(true, true, false, false);
        auto other_dry = side(false, false, true, false), other_alone = side(true, false, true, false);
        auto both_dry = side(false, true, true, false), both_keyed = side(true, true, true, false);
        auto ghost = side(true, true, true, true);
        CHECK(key_keyed.reduction > 3 && std::fabs(key_keyed.energy - key_dry.energy) < key_dry.energy * 0.01);       // the key untouched
        CHECK(other_alone.reduction == 0 && std::fabs(other_alone.energy - other_dry.energy) < other_dry.energy * 0.01); // no key, no change
        CHECK(both_keyed.energy < both_dry.energy - other_dry.energy * 0.5);                                           // the rest ducks
        CHECK(ghost.reduction > 3 && ghost.energy < other_dry.energy * 0.5);                                           // muted key, track 3 ducked
    }
    // Sidechain on one kit pad: A01 keys it and passes untouched; A02 on the same kit ducks under it.
    {
        auto kit = std::make_unique<Project>(*project);
        load_sound(kit->tracks[1], EngineKind::Sampler, 0);
        for (unsigned pad : {0u, 1u}) { kit->tracks[1].pads[pad].sample = 0; kit->tracks[1].pads[pad].play = PadPlay::OneShot; }
        SampleData tone;
        tone.frames = 44100; tone.left.resize(44100); tone.right.resize(44100);
        for (unsigned n = 0; n < 44100; ++n) tone.left[n] = tone.right[n] = float(std::sin(n * 0.05) * 0.8);
        auto hit = [&](bool keyed, bool a01, bool a02) {
            EngineHost host;
            host.load(*kit, *bank);
            CHECK(host.offer_sample(0, &tone));
            host.send({Command::Type::Compressor, 0, 0, 4, 100});
            host.send({Command::Type::Compressor, 0, 0, 7, 1});
            host.send({Command::Type::Compressor, 0, 0, 8, 0});
            host.send({Command::Type::Compressor, 0, 0, 6, 1});
            host.send({Command::Type::Compressor, 0, 0, 0, int16_t(keyed)});
            if (a01) host.send({Command::Type::NoteOn, 1, 0, uint16_t(sampler_first_note + 0), 110});
            if (a02) host.send({Command::Type::NoteOn, 1, 0, uint16_t(sampler_first_note + 1), 110});
            std::vector<int32_t> out(256);
            double sum = 0;
            for (unsigned n = 0; n < 100; ++n) { host.render(out.data(), 128, 1); if (n > 10) for (auto v : out) sum += double(v / 2147483648.0f) * (v / 2147483648.0f); }
            const SampleData* gone = nullptr;
            while (host.take_retired(gone)) {}
            return std::pair<double, float>(sum, host.compression());
        };
        auto a_dry = hit(false, true, false), a_keyed = hit(true, true, false);
        auto b_dry = hit(false, false, true), b_keyed = hit(true, false, true);
        auto both_dry = hit(false, true, true), both_keyed = hit(true, true, true);
        CHECK(a_dry.first > 0 && a_keyed.second > 3 && std::fabs(a_keyed.first - a_dry.first) < a_dry.first * 0.01); // the key pad untouched
        CHECK(b_keyed.second == 0 && std::fabs(b_keyed.first - b_dry.first) < b_dry.first * 0.01);                   // no key, no change
        CHECK(both_keyed.first < both_dry.first - b_dry.first * 0.5);                                                // A02 ducks
    }
    // The page's picture: the compressor hands over its peaks, the trail keeps a column every 33 ms whatever the frame rate.
    {
        EngineHost host;
        host.load(*project, *bank);
        host.send({Command::Type::Compressor, 0, 0, 4, 100});
        host.send({Command::Type::Compressor, 0, 0, 0, 1});
        for (int note : {57, 64}) host.send({Command::Type::NoteOn, 0, 0, uint16_t(note), 127});
        std::vector<int32_t> out(256);
        for (unsigned n = 0; n < 100; ++n) host.render(out.data(), 128, 1);
        float level = -90, reduce = 0;
        host.compressor_activity(level, reduce);
        CHECK(level > -30 && reduce > 3);
        host.compressor_activity(level, reduce); // taken: both start again
        CHECK(level == -90 && reduce == 0);
        CompTrail trail;
        trail.update(-6, 5, 1000);                     // the first column at once
        CHECK(trail.trail().back().level == -6 && trail.trail().back().reduce == 5);
        trail.update(-20, 1, 1010);                    // not due: kept for the next column
        trail.update(-90, 0, 1040);
        CHECK(trail.trail().back().level == -20 && trail.trail()[comp_columns - 2].level == -6);
        trail.update(-12, 2, 1140);                    // a slow frame: the column due three times
        CHECK(trail.trail().back().level == -12 && trail.trail()[comp_columns - 3].level == -12);
    }

    // The project file: Knob FX and the compressor come back; older files (neither section) load them off.
    project->knob_fx.choice = 16; project->knob_fx.kind = uint8_t(EffectKind::AmpSim); project->knob_fx.params = {3, 70, 40, 2, -3, 5};
    project->knob_fx.on = true; project->knob_fx.bypass = true;
    project->knob_fx.set_track(4, true); project->knob_fx.set_pad(1, 70, true);
    project->compressor = {true, true, 12, 80, 66, 9, true, 5, 7};
    auto data = save_project(*project, *bank);
    auto back = std::make_unique<Project>();
    auto back_bank = std::make_unique<PatternBank>();
    CHECK(load_project(data, *back, *back_bank).empty());
    const auto& k = back->knob_fx;
    CHECK(!k.on && k.bypass && k.choice == 16 && k.kind == uint8_t(EffectKind::AmpSim) && k.params[4] == -3 && k.track_on(4) && !k.track_on(3)
          && k.pad_on(1, 70) && !k.pad_on(1, 6));
    const auto& c = back->compressor;
    CHECK(c.on && c.color && c.attack == 12 && c.release == 80 && c.amount == 66 && c.boost == 9 && c.sidechain && c.key == 5 && c.key_pad == 7);
    auto older = data;
    for (size_t n = 0; n + 4 <= older.size(); ++n)
        if (std::memcmp(&older[n], "KNFX", 4) == 0 || std::memcmp(&older[n], "CMPR", 4) == 0) std::memcpy(&older[n], "XXXX", 4);
    CHECK(load_project(older, *back, *back_bank).empty() && back->knob_fx.kind == 0 && !back->knob_fx.tracks && !back->compressor.on);

    // Bindings: with the mode on the pots are Knob FX's on any page, Shift its second row; Shift + K on a chain gives the other row.
    UiState ui;
    ui.page = PageId::Mix;
    project->knob_fx.on = true; project->knob_fx.bypass = false;
    Pattern pattern;
    auto cards = page_bindings(*project, ui, pattern, false);
    CHECK(cards[0].kind == Binding::Kind::KnobParam && cards[0].id == 0 && cards[1].value == 70);
    cards = page_bindings(*project, ui, pattern, true);
    CHECK(cards[0].kind == Binding::Kind::KnobParam && cards[0].id == 3 && cards[2].value == 5);
    project->knob_fx.on = false;
    CHECK(page_bindings(*project, ui, pattern, false)[0].kind == Binding::Kind::Level);
    ui.page = PageId::Rack; ui.track = 2;
    project->tracks[2].fx.slots[0].kind = uint8_t(EffectKind::Spring);
    project->tracks[2].fx.slots[0].params = {0, 25, 30, 70, 60, 120};
    cards = page_bindings(*project, ui, pattern, true);
    CHECK(cards[0].kind == Binding::Kind::FxParam && cards[0].id % 8 == 3 && cards[2].value == 120);
    ui.page = PageId::Compressor;
    cards = page_bindings(*project, ui, pattern, false);
    CHECK(cards[0].kind == Binding::Kind::CompAttack && cards[2].kind == Binding::Kind::CompAmount && cards[2].value == 66);
    CHECK(page_bindings(*project, ui, pattern, true)[2].kind == Binding::Kind::CompBoost);
}


// Parity phase 7b: the MPC's Pad FX, four live slots, Flex Beat.
void pad_fx_and_flex() {
    // The model's defaults are the guide's initial values; banks: Melodee 1, Melodee 2 (+ Flex Beat's four), MPC.
    for (unsigned m = 0; m < 16; ++m)
        for (unsigned n = 0; n < punch_params; ++n) {
            const auto& pi = mpc_pad_fx(m).params[n];
            CHECK(pad_fx_defaults[m][n] == (pi.label ? pi.initial : 0));
        }
    CHECK(std::string(mpc_pad_fx(0).name) == "Half Speed" && std::string(mpc_pad_fx(15).name) == "Reverb");
    CHECK(punch_on_pad(2, 0) == int(PunchKind::HalfSpeed) && punch_on_pad(2, 15) == int(PunchKind::Reverb));
    CHECK(punch_on_pad(0, 3) == int(PunchKind::Loop32) && punch_on_pad(1, 10) == int(PunchKind::Vinyl));
    CHECK(punch_on_pad(1, 11) == int(PunchKind::Backspin) && punch_on_pad(1, 14) == int(PunchKind::Pump) && punch_on_pad(1, 15) == -1);
    CHECK(std::string(flex_beat(0).name) == "Empty");
    for (unsigned n = 1; n < flex_pads; ++n) CHECK(unsigned(flex_beat(n).kind) < punch_count);

    auto signal = [](unsigned n) { uint32_t h = n * 2654435761u; return (int32_t(h) >> 8) / 8388608.0f * 0.5f; };
    const double bar = 88200, even_bar = 88320; // the second a whole number of samples a sixteenth
    // Delay 1/4 of a bar: an impulse comes back 22050 samples later, then again quieter.
    {
        Punch punch;
        const int16_t params[] {3, 50, 0};
        punch.start(PunchKind::Delay, 1, bar, bar / 16, 0, params);
        std::vector<float> l(1), r(1), out;
        for (unsigned n = 0; n < 50000; ++n) { l[0] = r[0] = n == 10 ? 1.0f : 0.0f; punch.process(l.data(), r.data(), 1); out.push_back(l[0]); }
        CHECK(std::fabs(out[10] - 1) < 1e-6f && out[10 + 22050] > 0.6f && out[10 + 44100] > 0.2f && out[10 + 44100] < out[10 + 22050]);
    }
    // Beat Repeat 1/16 reversed: after the slice, it plays backwards.
    {
        Punch punch;
        const int16_t params[] {4, 1, 0};
        punch.start(PunchKind::BeatRepeat, 1, even_bar, even_bar / 16, 0, params);
        unsigned loop = unsigned(even_bar / 16);
        std::vector<float> l(1), r(1), out;
        for (unsigned n = 0; n < loop * 3; ++n) { l[0] = r[0] = signal(n); punch.process(l.data(), r.data(), 1); out.push_back(l[0]); }
        CHECK(std::fabs(out[loop + 100] - signal(loop - 101)) < 1e-4f);
    }
    // Rev Stepper: forwards, backwards, forwards ... then a new slice let through.
    {
        Punch punch;
        const int16_t params[] {4, 2, 0};
        punch.start(PunchKind::RevStepper, 1, even_bar, even_bar / 16, 0, params);
        unsigned loop = unsigned(even_bar / 16);
        std::vector<float> l(1), r(1), out;
        for (unsigned n = 0; n < loop * 5; ++n) { l[0] = r[0] = signal(n); punch.process(l.data(), r.data(), 1); out.push_back(l[0]); }
        CHECK(std::fabs(out[3 * loop + 500] - signal(3 * loop + 500)) < 1e-6f); // two plays, then the input again
    }
    // K1-K3 change a running punch: LoFi at 2 bits holds only a few levels.
    {
        Punch punch;
        punch.start(PunchKind::LoFi, 1, bar, bar / 16, 0);
        const int16_t two_bits[] {2, 0, 0};
        punch.set_params(two_bits);
        std::vector<float> l(256), r(256);
        for (unsigned n = 0; n < 256; ++n) l[n] = r[n] = signal(n);
        punch.process(l.data(), r.data(), 256);
        for (unsigned n = 0; n < 256; ++n) CHECK(l[n] == 0 || std::fabs(std::fabs(l[n]) - 0.5f) < 1e-6f || std::fabs(std::fabs(l[n]) - 1) < 1e-6f);
    }

    // The host: four live slots at once, each let go on its own; Flex Beat's One Shot gives way to Empty after a bar.
    {
        auto host = std::make_unique<EngineHost>();
        auto project = std::make_unique<Project>();
        auto patterns = std::make_unique<PatternBank>();
        host->load(*project, *patterns);
        std::vector<int32_t> out(256);
        const PunchKind kinds[] {PunchKind::HalfSpeed, PunchKind::Chorus, PunchKind::Comb, PunchKind::Granulator};
        for (unsigned n = 0; n < 4; ++n) {
            Command on {Command::Type::PunchOn, 255, uint8_t(2 | 1 << 4), uint16_t(kinds[n]), 100};
            on.position = n + 1;
            host->send(on);
        }
        host->render(out.data(), 128, 1);
        CHECK(host->live_punches() == 0xf);
        Command off {Command::Type::PunchOff, 255};
        off.position = 2;
        host->send(off);
        host->render(out.data(), 128, 1);
        CHECK(host->live_punches() == 0xd);
        host->send({Command::Type::PunchParam, 0, 2, 11, 200}); // Granulator's grain length while it plays
        host->send({Command::Type::PunchOff, 254});
        host->render(out.data(), 128, 1);
        CHECK(host->live_punches() == 0 && !host->punching());
        host->send({Command::Type::FlexBeat, 0, 0, 3, 0}); // quantize off: at once
        host->send({Command::Type::FlexBeat, 0, 0, 0, 3});
        host->render(out.data(), 128, 1);
        CHECK(host->flex_playing() == 3);
        for (unsigned n = 0; n < 700; ++n) host->render(out.data(), 128, 1); // a bar at 120 is 88200 samples
        CHECK(host->flex_playing() == 0);
        host->send({Command::Type::FlexBeat, 0, 0, 1, 1}); // Loop
        host->send({Command::Type::FlexBeat, 0, 0, 0, 7});
        for (unsigned n = 0; n < 1400; ++n) host->render(out.data(), 128, 1);
        CHECK(host->flex_playing() == 7);
        host->send({Command::Type::FlexBeat, 0, 0, 0, 0}); // Empty
        host->render(out.data(), 128, 1);
        CHECK(host->flex_playing() == 0);
    }
    // The project file keeps K1-K3 and Flex Beat; older files (no PFXP) load the defaults; the MPC's bank is 2.
    {
        auto project = std::make_unique<Project>();
        auto patterns = std::make_unique<PatternBank>();
        CHECK(project->punch_bank == 2);
        project->pad_fx_params[14] = {2, 80, 2};
        project->flex = {1, 60, false};
        auto bytes = save_project(*project, *patterns);
        auto back = std::make_unique<Project>();
        CHECK(load_project(bytes, *back, *patterns).empty());
        CHECK(back->pad_fx_params[14][1] == 80 && back->pad_fx_params[14][2] == 2 && back->flex.mode == 1 && back->flex.mix == 60 && !back->flex.quantize);
        for (size_t n = 0; n + 4 <= bytes.size(); ++n) if (std::memcmp(&bytes[n], "PFXP", 4) == 0) std::memcpy(&bytes[n], "XXXX", 4);
        auto old = std::make_unique<Project>();
        CHECK(load_project(bytes, *old, *patterns).empty() && old->pad_fx_params == pad_fx_defaults && old->flex.quantize);
    }
}

void fader_and_takeover() {
    using Kind = Binding::Kind;
    auto project = std::make_unique<Project>();
    auto& kit = project->tracks[3];
    load_sound(kit, EngineKind::Sampler, 0);
    kit.pads[8].sample = 0; kit.last_pad = 8; kit.pads[8].level = 90; kit.pads[8].pan = 0;
    auto& synth = project->tracks[2];
    load_sound(synth, EngineKind::Prophet, 0);
    // Defaults by engine; a kit target on a synth falls back to the mod wheel.
    CHECK(fader_target(kit) == FaderTarget::PadVolume && fader_target(synth) == FaderTarget::ModWheel);
    synth.fader_target = uint8_t(FaderTarget::PadPan);
    CHECK(fader_target(synth) == FaderTarget::ModWheel);
    synth.fader_target = fader_by_engine;

    // What it drives: the track's target, or the page's own job.
    UiState ui; ui.track = 3; ui.page = PageId::Stage;
    auto job = fader_job(*project, ui);
    CHECK(job.source == FaderJob::Source::Target && job.binding.kind == Kind::PadLevel && job.binding.value == 90);
    ui.page = PageId::Mix;
    job = fader_job(*project, ui);
    CHECK(job.source == FaderJob::Source::Page && job.binding.kind == Kind::Level);
    ui.page = PageId::Sends; CHECK(fader_job(*project, ui).binding.kind == Kind::Send);
    ui.page = PageId::StepEdit; CHECK(fader_job(*project, ui).source == FaderJob::Source::Nudge);
    ui.page = PageId::Compressor; CHECK(fader_job(*project, ui).binding.kind == Kind::CompAmount);
    ui.page = PageId::FlexBeat; CHECK(fader_job(*project, ui).binding.kind == Kind::FlexMix);
    ui.page = PageId::KnobFx; // no effect loaded: the track's target
    CHECK(fader_job(*project, ui).source == FaderJob::Source::Target);
    project->knob_fx.kind = uint8_t(EffectKind::Chorus);
    job = fader_job(*project, ui);
    CHECK(job.source == FaderJob::Source::Page && job.binding.kind == Kind::KnobParam
          && std::string(effect_info(EffectKind::Chorus).params[size_t(job.binding.id)].label) == "MIX");
    project->knob_fx.kind = uint8_t(EffectKind::Filter); // no MIX: K1's, the cutoff
    CHECK(fader_job(*project, ui).binding.id == 0);
    // Off: only the page's own jobs remain.
    ui.page = PageId::Stage; kit.fader_on = false;
    CHECK(fader_job(*project, ui).source == FaderJob::Source::None && fader_light_level(fader_job(*project, ui)) == 0);
    ui.page = PageId::Mix; CHECK(fader_job(*project, ui).source == FaderJob::Source::Page);
    kit.fader_on = true; ui.page = PageId::Stage;
    // Decay / Release swaps with the pad's play mode; an empty last pad has nothing to drive.
    kit.fader_target = uint8_t(FaderTarget::PadDecay); kit.pads[8].play = PadPlay::OneShot;
    CHECK(fader_job(*project, ui).binding.kind == Kind::PadDecay);
    kit.pads[8].play = PadPlay::Gate;
    CHECK(fader_job(*project, ui).binding.kind == Kind::PadRelease);
    kit.last_pad = 9;
    CHECK(fader_job(*project, ui).source == FaderJob::Source::Target && !fader_job(*project, ui).binding.bound());
    kit.last_pad = 8;
    kit.fader_target = uint8_t(FaderTarget::KitVolume);
    CHECK(fader_job(*project, ui).binding.kind == Kind::Param && fader_job(*project, ui).binding.id == sampler::Level);
    // The light: the value's place; pan and tune brightest at the middle.
    kit.fader_target = uint8_t(FaderTarget::PadPan);
    CHECK(std::fabs(fader_light_level(fader_job(*project, ui)) - 1) < 0.02f);
    kit.pads[8].pan = -50;
    CHECK(fader_light_level(fader_job(*project, ui)) < 0.1f);
    kit.fader_target = uint8_t(FaderTarget::PadVolume); kit.pads[8].level = 127;
    CHECK(fader_light_level(fader_job(*project, ui)) > 0.99f);
    kit.pads[8].level = 0;
    CHECK(fader_light_level(fader_job(*project, ui)) < 0.1f);
    // Motion lanes: the pad's own on the last pad, the mod wheel's, none for the mix strip.
    CHECK(binding_lane(kit, Binding {Kind::PadLevel, 0, 0, 127, 0}) == motion_lane(motion_pad_param(PadParam::Level), 8));
    CHECK(binding_lane(synth, Binding {Kind::ModWheel, 0, 0, 127, 0}) == motion_lane(motion_wheel));
    CHECK(binding_lane(kit, Binding {Kind::Level, 0, 0, 127, 0}) < 0);
    CHECK(lane_name(synth, motion_lane(motion_wheel)) == "MOD WHEEL" && lane_binding(synth, 2, motion_lane(motion_wheel)).kind == Kind::ModWheel);

    // Takeover: Pickup waits until the control reaches the value, Instant jumps, Scaled moves along and meets it.
    Binding b {Kind::Level, 0, 0, 127, 100};
    Pot pot; int out = -1;
    pot.engaged = takeover_meets(10, b);
    CHECK(!pot.engaged && !takeover_move(Takeover::Pickup, pot, 10, 20, b, out) && !pot.engaged);
    CHECK(takeover_move(Takeover::Pickup, pot, 95, 105, b, out) && pot.engaged && out == 105); // crossed it
    pot = {};
    CHECK(takeover_move(Takeover::Instant, pot, 10, 20, b, out) && out == 20 && pot.engaged);
    pot = {};
    Binding scaled = b;
    int position = 10;
    for (; position < 127; ++position) { // up from 10: the value rises with it and they meet at the top
        if (takeover_move(Takeover::Scaled, pot, position, position + 1, scaled, out)) { CHECK(out >= scaled.value - 2); scaled.value = out; } // at most a step back where they meet
    }
    CHECK(pot.engaged && scaled.value == 127);
    pot = {}; scaled.value = 50;
    bool moved = false;
    for (position = 127; position > 0; --position) { // down from the top: the value falls with it, never past the control
        if (takeover_move(Takeover::Scaled, pot, position, position - 1, scaled, out)) { CHECK(out <= scaled.value + 2); scaled.value = out; moved = true; }
        if (pot.engaged) break;
    }
    CHECK(moved && pot.engaged && std::abs(position - scaled.value) <= 3);

    // Settings: the MPC's defaults; the old "Knob pickup Off" reads as Instant everywhere, On as the defaults.
    Settings fresh;
    CHECK(fresh.knob_takeover == Takeover::Scaled && fresh.knob_fx_takeover == Takeover::Scaled && fresh.fader_takeover == Takeover::Pickup);
    Settings old_off; old_off.read("pad_response=1\npickup=0\n");
    CHECK(old_off.knob_takeover == Takeover::Instant && old_off.knob_fx_takeover == Takeover::Instant && old_off.fader_takeover == Takeover::Instant);
    Settings old_on; old_on.read("pickup=1\n");
    CHECK(old_on.knob_takeover == Takeover::Scaled && old_on.fader_takeover == Takeover::Pickup);
    Settings mixed; mixed.knob_takeover = Takeover::Pickup; mixed.knob_fx_takeover = Takeover::Instant; mixed.fader_takeover = Takeover::Scaled;
    auto text = mixed.text();
    CHECK(text.find("pickup=1") != std::string::npos); // older builds still read a pickup they know
    Settings back; back.read(text);
    CHECK(back.knob_takeover == Takeover::Pickup && back.knob_fx_takeover == Takeover::Instant && back.fader_takeover == Takeover::Scaled);
    back.step(Settings::FaderTakeover, 1); CHECK(back.fader_takeover == Takeover::Instant);
    back.step(Settings::FaderTakeover, 1); CHECK(back.fader_takeover == Takeover::Pickup);
    CHECK(std::string(Settings::row_name(Settings::KnobFxTakeover)) == "Knob FX takeover");

    // The file: the fader's target and on / off per track (FADR), the mod wheel's lane (MOTW); older files load the defaults.
    {
        auto patterns = std::make_unique<PatternBank>();
        kit.fader_target = uint8_t(FaderTarget::PadCutoff); kit.fader_on = false;
        MotionEvent wheel; wheel.track = 2; wheel.pattern = 0; wheel.step = 4; wheel.param = uint8_t(motion_wheel); wheel.value = 90;
        MotionEvent cutoff; cutoff.track = 2; cutoff.pattern = 0; cutoff.step = 2; cutoff.param = 3; cutoff.value = 40;
        project->motion.write(wheel); project->motion.write(cutoff);
        auto bytes = save_project(*project, *patterns);
        auto loaded = std::make_unique<Project>();
        CHECK(load_project(bytes, *loaded, *patterns).empty());
        CHECK(loaded->tracks[3].fader_target == uint8_t(FaderTarget::PadCutoff) && !loaded->tracks[3].fader_on);
        CHECK(loaded->tracks[2].fader_target == fader_by_engine && loaded->tracks[2].fader_on);
        CHECK(loaded->motion.count == 2 && loaded->motion.has(2, 0, uint8_t(motion_wheel), 0) && loaded->motion.has(2, 0, 3, 0));
        for (size_t n = 0; n + 4 <= bytes.size(); ++n)
            if (std::memcmp(&bytes[n], "FADR", 4) == 0 || std::memcmp(&bytes[n], "MOTW", 4) == 0) std::memcpy(&bytes[n], "XXXX", 4);
        auto older = std::make_unique<Project>();
        CHECK(load_project(bytes, *older, *patterns).empty());
        CHECK(older->tracks[3].fader_target == fader_by_engine && older->tracks[3].fader_on && fader_target(older->tracks[3]) == FaderTarget::PadVolume);
        CHECK(older->motion.count == 1 && older->motion.has(2, 0, 3, 0)); // MOT2 never held the wheel
    }
}

// Parity phase 9: MIDI Config, where MIDI in goes, the clocks out and the engine's MIDI out and clock in.
void midi_and_sync() {
    // Settings: new keys only; a file without them gets the defaults; Reset keeps the starred sounds.
    {
        Settings set;
        CHECK(set.midi.port == MidiPort::External && set.midi.in_channel == 0 && set.midi.out_channel == 1 && set.midi.pad_in &&
              set.midi.pad_out == PadMidiOut::Never && set.midi.sync_in == MidiSync::Off && !set.midi.cv_out && set.midi.cv_ppqn() == 2);
        set.midi.port = MidiPort::Usb; set.midi.in_channel = 10; set.midi.out_channel = 16; set.midi.pad_out = PadMidiOut::Empty;
        set.midi.sync_in = MidiSync::Clock; set.midi.sync_out = MidiSync::Mtc; set.midi.thru = true; set.midi.program_change = true;
        set.midi.cv_out = true; set.midi.cv_base = 4; set.midi.cv_division = 24; set.midi.pad_in = false;
        Settings back; back.read(set.text());
        CHECK(back.midi == set.midi && back.midi.cv_ppqn() == 96);
        Settings older; older.read("pad_response=2\nscreen=80\n");
        CHECK(older.midi == MidiConfig {} && older.pad_response == 2);
        Settings bad; bad.read("cv_base=40\ncv_division=0\nmidi_out_channel=0\nmidi_in_channel=99\n");
        CHECK(bad.midi.cv_base == 8 && bad.midi.cv_division == 1 && bad.midi.out_channel == 1 && bad.midi.in_channel == 16);
        set.toggle_favourite(0, 3); set.screen = 50;
        set.reset_factory();
        CHECK(set.midi == MidiConfig {} && set.screen == 100 && set.favourite(0, 3));
    }
    // The page's rows: channels stop at their ends, lists go round, takeovers through Settings.
    {
        Settings set; Track track;
        midi_row_step(set, track, InChannelRow, -1); CHECK(set.midi.in_channel == 0);
        for (int n = 0; n < 20; ++n) midi_row_step(set, track, InChannelRow, 1);
        CHECK(set.midi.in_channel == 16 && midi_row_value(set, track, InChannelRow) == "16");
        midi_row_step(set, track, TrackChannelRow, 1); CHECK(track.midi_channel == 1 && midi_row_value(set, track, TrackChannelRow) == "1");
        midi_row_step(set, track, TrackChannelRow, -1); CHECK(midi_row_value(set, track, TrackChannelRow) == "Off");
        midi_row_step(set, track, PadOutRow, -1); CHECK(set.midi.pad_out == PadMidiOut::Empty);
        midi_row_step(set, track, SyncOutRow, 1); CHECK(midi_row_value(set, track, SyncOutRow) == "MIDI Clock");
        midi_row_step(set, track, FaderTakeoverRow, 1); CHECK(set.fader_takeover == Takeover::Scaled);
        CHECK(midi_row_value(set, track, ProgramChangeRow) == "Off" && std::string(midi_row_name(ResetRow)) == "Reset Factory Settings");
    }
    // Where notes go: a track's own channel wins; the In channel (All or one) plays the selected track.
    {
        MidiConfig config;
        std::array<uint8_t, track_count> own {};
        own[5] = 3; own[9] = 3;
        CHECK(note_tracks(config, own, 2, 3) == ((1u << 5) | (1u << 9)));
        CHECK(note_tracks(config, own, 2, 1) == (1u << 2));
        config.in_channel = 7;
        CHECK(note_tracks(config, own, 2, 1) == 0 && note_tracks(config, own, 2, 7) == (1u << 2));
        Track kit; kit.engine = EngineKind::Sampler;
        Track synth; synth.engine = EngineKind::Prophet;
        Track drums; drums.engine = EngineKind::Kit909;
        CHECK(note_key(kit, 35) == -1 && note_key(kit, 36) == 36 && note_key(kit, 127) == 127);
        CHECK(note_key(synth, 12) == 12 && note_key(drums, 35) == -1 && note_key(drums, 46) == 46 && note_key(drums, 47) == -1);
        CHECK(port_is(MidiPort::External, "SerialMIDI", false) && !port_is(MidiPort::External, "SerialMIDI", true));
        CHECK(port_is(MidiPort::Usb, "MIDI Gadget", false) && port_is(MidiPort::Usb, "USB-Audio", false) && !port_is(MidiPort::Usb, "SerialMIDI", false));
        Track own_channel; own_channel.midi_channel = 9;
        CHECK(out_channel_of(config, own_channel) == 9 && out_channel_of(config, synth) == 1);
    }
    // MIDI clock in: the tempo from the pulses' stamps, steady through jitter; a pause starts over.
    {
        ClockFollower f;
        CHECK(f.tempo_tenths() == 0);
        uint64_t at = 1000000000ull;
        uint32_t noise = 12345;
        for (unsigned n = 0; n < 96; ++n) {
            noise = noise * 1664525u + 1013904223u;
            f.pulse(at + n * 20833333ull + (noise >> 16) % 100000); // 120 BPM, up to 0.1 ms of jitter
        }
        CHECK(f.tempo_tenths() >= 1199 && f.tempo_tenths() <= 1201);
        CHECK(f.jitter_us() > 5 && f.jitter_us() < 60); // up to 0.1 ms each way: about 29 us
        f.pulse(at + 96 * 20833333ull + 500000000ull);
        CHECK(f.tempo_tenths() == 0);
        for (unsigned n = 1; n < 30; ++n) f.pulse(at + 96 * 20833333ull + 500000000ull + n * 15625000ull); // 160 BPM
        CHECK(f.tempo_tenths() >= 1599 && f.tempo_tenths() <= 1601);
    }
    // The clocks out, from the tempo (not the swung ticks), to the frame.
    {
        using K = MidiOutEvent::Kind;
        std::vector<MidiOutEvent> got;
        auto keep = [&](const MidiOutEvent& e) { got.push_back(e); };
        SyncOut sync;
        sync.set(true, true, true, 2);
        double quarter = 22050; // 120 BPM at 44.1 kHz
        sync.start(1000, 0, 0, quarter, keep);
        for (unsigned b = 0; b < 200; ++b) sync.run(1000 + b * 128, 128, quarter, keep);
        CHECK(got[0].kind == K::Start && got[0].frame == 1000 && got[1].kind == K::MtcFull && got[1].value == 0);
        std::vector<uint64_t> clocks, highs, lows, quarters;
        for (auto& e : got) {
            if (e.kind == K::Clock) clocks.push_back(e.frame);
            if (e.kind == K::CvHigh) highs.push_back(e.frame);
            if (e.kind == K::CvLow) lows.push_back(e.frame);
            if (e.kind == K::MtcQuarter) quarters.push_back(e.frame);
        }
        CHECK(clocks.size() == 28 && clocks[0] == 1000 && clocks[24] == 1000 + 22050); // 25600 frames: pulses 0..27
        for (size_t n = 1; n < clocks.size(); ++n) CHECK(clocks[n] - clocks[n - 1] >= 918 && clocks[n] - clocks[n - 1] <= 919);
        CHECK(highs.size() == 3 && highs[1] - highs[0] == 11025 && lows[0] - highs[0] >= 220 && lows[0] - highs[0] <= 221); // 2 PPQN, 5 ms high
        CHECK(quarters.size() == 59 && quarters[1] - quarters[0] == 441);
        CHECK(SyncOut::mtc_nibble(25 * 61 + 3, 0) == 3 && SyncOut::mtc_nibble(25 * 61 + 3, 2) == 1 && SyncOut::mtc_nibble(25 * 61 + 3, 4) == 1 &&
              SyncOut::mtc_nibble(0, 7) == 2); // 0:01:01:03; 25 fps in the hours piece
        got.clear();
        sync.stop(30000, keep);
        CHECK(got.size() == 2 && got[0].kind == K::Stop && got[1].kind == K::CvLow);
        // Continue from 1.3 quarters: Song Position 6 (the next sixteenth), clocks from pulse 36 there.
        got.clear();
        sync.set(true, false, false, 2);
        sync.start(0, 1.3, 0.65, quarter, keep);
        sync.run(0, 8192, quarter, keep);
        CHECK(got.size() >= 2 && got[0].kind == K::Continue && got[0].value == 6 && got[0].frame == 4410);
        CHECK(got[1].kind == K::Clock && got[1].frame == got[0].frame);
        // Following clock in: the clock out never runs ahead of the pulses let through, and catches up a pulse behind.
        got.clear();
        SyncOut follow; follow.set(true, false, false, 2);
        follow.start(0, 0, 0, quarter, keep);
        follow.set_external(true);
        follow.allow(0);
        follow.run(0, 4096, quarter, keep);
        CHECK(got.size() == 1 && got[0].kind == K::Start); // nothing yet
        follow.allow(3);
        follow.run(4096, 128, quarter, keep);
        CHECK(got.size() == 2); // three pulses behind (beats ran on): one now, the rest at twice the pace
        follow.allow(10);
        follow.run(4224, 8192, quarter, keep);
        std::vector<uint64_t> out_pulses;
        for (auto& e : got) if (e.kind == K::Clock) out_pulses.push_back(e.frame);
        CHECK(out_pulses.size() == 10);
        for (size_t n = 1; n < out_pulses.size(); ++n) CHECK(out_pulses[n] - out_pulses[n - 1] >= 459); // never closer than half a pulse
        // Another rate (48 or 96 kHz later): MTC's quarter frames follow it.
        got.clear();
        SyncOut fast; fast.set_rate(96000); fast.set(false, true, false, 2);
        fast.start(0, 0, 0, 48000, keep);
        fast.run(0, 2000, 48000, keep);
        CHECK(got.size() == 4 && got[2].frame == 960 && got[3].frame == 1920);
    }
    // The audio clock: frames to CLOCK_MONOTONIC and back, at the device's rate.
    {
        AudioClock clock;
        clock.set_format(48000, 384);
        clock.stamp(1000000000, 0);
        CHECK(clock.heard_at(0) == 1000000000 + 8000000); // 384 frames at 48 kHz
        CHECK(clock.frame_at(1000000000 + 10000000) == 480);
        clock.stamp(1000000000 + 2666666 + 1000000, 128); // a block a millisecond late barely moves it
        CHECK(std::llabs(clock.heard_at(0) - 1008000000) < 10000);
    }
    // The engine host: patterns and pads out on their channel, Empty, the clocks, and following MIDI clock in.
    {
        using K = MidiOutEvent::Kind;
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        load_sound(project->tracks[0], EngineKind::Prophet, 1);
        project->tempo = 1200;
        put(bank->at[0][0], 4, 45, 100);
        put(bank->at[1][0], 0, 60, 90); // track 2 has no engine: a MIDI track under Empty
        EngineHost host;
        host.load(*project, *bank);
        host.set_audio_format(44100, 384);
        std::vector<int32_t> out(256);
        auto drain = [&] { std::vector<MidiOutEvent> e; MidiOutEvent one; while (host.take_midi(one)) e.push_back(one); return e; };
        host.send({Command::Type::MidiTrack, 0, 0, 5, 1}); // track 1: channel 5, always
        host.send({Command::Type::MidiTrack, 1, 0, 2, 2}); // track 2: channel 2, empty
        host.send({Command::Type::MidiSync, 0, 0, 0, 1});  // MIDI clock out
        host.render(out.data(), 128, 1);
        CHECK(drain().empty());
        host.send({Command::Type::NoteOn, 0, 0, 50, 99});  // a pad
        host.send({Command::Type::NoteOn, 0, 1, 51, 99});  // a note from MIDI in: never echoed
        host.render(out.data(), 128, 1);
        auto pads = drain();
        CHECK(pads.size() == 1 && pads[0].kind == K::NoteOn && pads[0].channel == 5 && pads[0].a == 50 && pads[0].b == 99 && pads[0].frame == 128);
        host.send({Command::Type::NoteOff, 0, 0, 50});
        host.render(out.data(), 128, 1);
        pads = drain();
        CHECK(pads.size() == 1 && pads[0].kind == K::NoteOff && pads[0].a == 50 && pads[0].frame == 256);
        host.send({Command::Type::Play});
        std::vector<MidiOutEvent> all;
        for (unsigned n = 0; n < 240; ++n) { host.render(out.data(), 128, 1); for (auto& e : drain()) all.push_back(e); }
        uint64_t start = 384; // Play came with the fourth block
        int empty_on = -1, synth_on = -1, synth_off = -1, clocks = 0;
        bool started = false;
        for (auto& e : all) {
            if (e.kind == K::Start) { started = true; CHECK(e.frame == start); }
            if (e.kind == K::Clock) ++clocks;
            if (e.kind == K::NoteOn && e.channel == 2) empty_on = int(e.frame - start);
            if (e.kind == K::NoteOn && e.channel == 5 && e.a == 45) synth_on = int(e.frame - start);
            if (e.kind == K::NoteOff && e.channel == 5 && e.a == 45) synth_off = int(e.frame - start);
        }
        CHECK(started && empty_on == 0 && synth_on >= 22050 && synth_on <= 22051 && synth_off > synth_on && synth_off < synth_on + 5513);
        CHECK(clocks >= 33 && clocks <= 34); // 30720 frames at 918.75 a pulse
        host.send({Command::Type::Stop});
        host.render(out.data(), 128, 1);
        auto stopped = drain();
        CHECK(!stopped.empty() && stopped[0].kind == K::Stop);
        // MIDI clock in: Play waits for the first pulse, then four ticks a pulse, each let through at its frame.
        host.send({Command::Type::ExtSync, 0, 0, 0, 1});
        host.send({Command::Type::Play});
        host.send({Command::Type::ExtSync, 0, 0, 3});
        for (unsigned n = 0; n < 20; ++n) host.render(out.data(), 128, 1);
        CHECK(host.playing() && host.following() && host.position_of(0) == -1); // not a tick yet
        uint64_t now = 128 * (1 + 1 + 1 + 240 + 1 + 20);
        Command pulse {Command::Type::ExtSync, 0, 0, 1};
        pulse.position = uint32_t(now + 64);
        host.send(pulse);
        host.render(out.data(), 128, 1);
        CHECK(host.ticks() >= 1 && host.ticks() <= 4);
        for (unsigned n = 0; n < 20; ++n) host.render(out.data(), 128, 1);
        CHECK(host.ticks() == 4); // no more pulses: it waits
        host.send({Command::Type::ExtSync, 0, 0, 4});
        host.send({Command::Type::Stop});
        host.render(out.data(), 128, 1);
        CHECK(!host.following() && host.midi_dropped() == 0 && host.dropped() == 0);
    }
    // The file: a track's own channel (MIDC); older files have none.
    {
        auto project = std::make_unique<Project>();
        auto patterns = std::make_unique<PatternBank>();
        project->tracks[4].midi_channel = 11;
        auto bytes = save_project(*project, *patterns);
        auto loaded = std::make_unique<Project>();
        CHECK(load_project(bytes, *loaded, *patterns).empty());
        CHECK(loaded->tracks[4].midi_channel == 11 && loaded->tracks[3].midi_channel == 0);
        for (size_t n = 0; n + 4 <= bytes.size(); ++n) if (std::memcmp(&bytes[n], "MIDC", 4) == 0) std::memcpy(&bytes[n], "XXXX", 4);
        auto older = std::make_unique<Project>();
        CHECK(load_project(bytes, *older, *patterns).empty());
        CHECK(older->tracks[4].midi_channel == 0);
    }
}

// MTC in (parity phase 9b): generated time code at each rate, read, settled, followed through jitter, a jump, a lost
// piece, Stop and running backwards; then the engine host placing the patterns and the song and keeping them on time.
struct Qf { uint8_t data; uint64_t ns; double seconds; };

// Quarter frames from frame `first` (hour 1, as a DAW starts) for `rounds` rounds of eight; `pace` above 1 runs fast.
std::vector<Qf> mtc_stream(MtcRate rate, uint32_t first, unsigned rounds, uint64_t start_ns, double pace = 1, uint32_t jitter_ns = 0) {
    std::vector<Qf> out;
    double fps = mtc_frames_a_second(rate);
    uint32_t noise = 777;
    for (unsigned r = 0; r < rounds; ++r) {
        unsigned m = 0, sec = 0, f = 0, h = 1;
        uint32_t code = first + 2 * r;
        mtc_code_of(code, rate, m, sec, f);
        h += code / uint32_t(std::lround(fps * 3600)); // past the hour (not reached here)
        uint8_t nib[8] {uint8_t(f & 15), uint8_t(f >> 4), uint8_t(sec & 15), uint8_t(sec >> 4), uint8_t(m & 15), uint8_t(m >> 4),
                        uint8_t(h & 15), uint8_t(((h >> 4) & 1) | unsigned(rate) << 1)};
        for (unsigned k = 0; k < 8; ++k) {
            double at = (code + k / 4.0) / fps; // where the master is as piece k goes out
            double since = (code - first + k / 4.0) / fps / pace;
            noise = noise * 1664525u + 1013904223u;
            uint64_t j = jitter_ns ? (noise >> 8) % jitter_ns : 0;
            out.push_back({uint8_t(k << 4 | nib[k]), start_ns + uint64_t(since * 1e9) + j, at});
        }
    }
    return out;
}

void mtc_in() {
    using K = MtcReader::Event::Kind;
    // Frame indexes and back; 29.97 drop-frame skips ;00 and ;01 each minute but every tenth.
    {
        unsigned m = 0, s = 0, f = 0;
        CHECK(mtc_frame_index(1, 0, 2, MtcRate::Fps2997Drop) == 1800 && mtc_frame_index(10, 0, 0, MtcRate::Fps2997Drop) == 17982);
        mtc_code_of(1799, MtcRate::Fps2997Drop, m, s, f); CHECK(m == 0 && s == 59 && f == 29);
        mtc_code_of(1800, MtcRate::Fps2997Drop, m, s, f); CHECK(m == 1 && s == 0 && f == 2);
        mtc_code_of(17982, MtcRate::Fps2997Drop, m, s, f); CHECK(m == 10 && s == 0 && f == 0);
        for (uint32_t n = 0; n < 107892; n += 7) {
            mtc_code_of(n, MtcRate::Fps2997Drop, m, s, f);
            CHECK(mtc_frame_index(m, s, f, MtcRate::Fps2997Drop) == n);
        }
        CHECK(mtc_frame_index(2, 3, 4, MtcRate::Fps24) == (2 * 60 + 3) * 24 + 4 && mtc_frame_index(0, 1, 0, MtcRate::Fps30) == 30);
    }
    // Each rate: settles within six frames, starts where the master is, then every quarter frame is a time sample.
    for (auto rate : {MtcRate::Fps24, MtcRate::Fps25, MtcRate::Fps2997Drop, MtcRate::Fps30}) {
        double fps = mtc_frames_a_second(rate);
        // From 0:59:59 (29.97 df: across the minute, where two labels are skipped), started on piece 3, 0.2 ms jitter.
        uint32_t first = mtc_frame_index(0, 59, 0, rate);
        auto stream = mtc_stream(rate, first, 200, 5000000000ull, 1, 200000);
        MtcReader reader;
        int started = -1, times = 0, others = 0;
        double worst = 0;
        for (size_t n = 3; n < stream.size(); ++n) {
            auto e = reader.quarter(stream[n].data, stream[n].ns);
            if (e.kind == K::Start) { started = int(n); CHECK(std::fabs(e.seconds - stream[n].seconds) < 1e-6); }
            else if (e.kind == K::Time) { ++times; worst = std::max(worst, std::fabs(e.seconds - stream[n].seconds)); }
            else if (e.kind != K::None) ++others;
        }
        CHECK(started > 0 && started <= 3 + 5 + 16 && (stream[size_t(started)].ns - stream[3].ns) / 1e9 < 6 / fps);
        CHECK(times == int(stream.size()) - started - 1 && others == 0 && worst < 1e-6 && reader.rate() == rate && reader.jumps() == 0);
        // Quarter frames stop: Stop once, 120 ms after the last.
        uint64_t last = stream.back().ns;
        CHECK(reader.idle(last + 100000000).kind == K::None);
        CHECK(reader.idle(last + 130000000).kind == K::Stop && !reader.running());
        CHECK(reader.idle(last + 140000000).kind == K::None);
    }
    // A jump while running: settles again, then Jump to the new place; a lost piece is no jump.
    {
        MtcReader reader;
        auto a = mtc_stream(MtcRate::Fps25, 250, 20, 1000000000ull);
        for (auto& q : a) reader.quarter(q.data, q.ns);
        CHECK(reader.running());
        auto b = mtc_stream(MtcRate::Fps25, 750, 20, a.back().ns + 10000000);
        int jumped = -1, times = 0;
        for (size_t n = 0; n < b.size(); ++n) {
            auto e = reader.quarter(b[n].data, b[n].ns);
            if (e.kind == K::Jump) { jumped = int(n); CHECK(std::fabs(e.seconds - b[n].seconds) < 1e-6 && e.seconds > 30); }
            if (e.kind == K::Time && jumped < 0) { ++times; CHECK(e.seconds < 12); } // before the new code: on from the old
            CHECK(e.kind != K::Stop && e.kind != K::Start);
        }
        CHECK(jumped > 8 && jumped < 24 && times == 7 && reader.jumps() == 1); // pieces 0-6 on from the old code
        auto c = mtc_stream(MtcRate::Fps25, 790, 10, b.back().ns + 10000000);
        bool stray = false;
        for (size_t n = 0; n < c.size(); ++n) {
            if (n == 13) continue; // lost
            auto e = reader.quarter(c[n].data, c[n].ns);
            stray |= e.kind != K::Time && e.kind != K::None;
            if (e.kind == K::Time) CHECK(std::fabs(e.seconds - c[n].seconds) < 1e-6);
        }
        CHECK(!stray && reader.jumps() == 1 && reader.running());
        // Running backwards: Stop.
        auto back = mtc_stream(MtcRate::Fps25, 900, 4, c.back().ns + 10000000);
        for (auto& q : back) reader.quarter(q.data, q.ns);
        CHECK(reader.running());
        CHECK(reader.quarter(back[30].data, back.back().ns + 10000000).kind == K::Stop && !reader.running());
        // A full frame while stopped is a cue; while running, the quarter frames after it place the song.
        auto cue = reader.full(1, 2, 30, 12, MtcRate::Fps25, back.back().ns + 300000000);
        CHECK(cue.kind == K::Cue && std::fabs(cue.seconds - (150 + 12 / 25.0)) < 1e-9);
        auto d = mtc_stream(MtcRate::Fps25, 3762, 6, back.back().ns + 500000000);
        int starts = 0;
        for (auto& q : d) starts += reader.quarter(q.data, q.ns).kind == K::Start;
        CHECK(starts == 1);
        auto moved = reader.full(1, 0, 10, 0, MtcRate::Fps25, d.back().ns + 5000000);
        CHECK(moved.kind == K::None && !reader.running());
        auto e2 = mtc_stream(MtcRate::Fps25, 250, 6, d.back().ns + 10000000);
        int jumps = 0;
        for (auto& q : e2) jumps += reader.quarter(q.data, q.ns).kind == K::Jump;
        CHECK(jumps == 1);
    }
    // The engine host: MTC places the patterns through the tempo playing, keeps them on time, catches a jump.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        load_sound(project->tracks[0], EngineKind::Prophet, 1);
        project->tempo = 1200;
        put(bank->at[0][0], 4, 45, 100);
        EngineHost host;
        host.load(*project, *bank);
        host.set_audio_format(44100, 384);
        std::vector<int32_t> out(256);
        uint64_t frame = 0;
        auto render = [&] { host.render(out.data(), 128, 1); frame += 128; };
        auto mtc = [&](uint16_t what, double seconds, uint64_t at) {
            Command c {Command::Type::MtcIn, 0, 0, what};
            c.position = uint32_t(at); c.micros = uint32_t(seconds * 1e6);
            host.send(c);
        };
        render();
        host.send({Command::Type::Play});
        mtc(1, 2.5, frame); // 2.5 s at 120 BPM: tick 480, the bar's second beat
        render();
        CHECK(host.playing() && host.mtc_following() && host.ticks() >= 480 && host.ticks() <= 481 && host.position_of(0) >= 96 &&
              host.position_of(0) <= 97 && host.mtc_places() == 1);
        // A master 0.1 % fast with up to 1 ms of jitter: the song keeps on its time, trimmed about 1000 ppm.
        double start = 2.5;
        uint64_t from = frame - 128; // where 2.5 s was
        uint32_t noise = 99;
        for (unsigned n = 0; n < 3445 * 2; ++n) { // 20 s
            if (n % 2 == 0) {
                double master = start + double(frame - from) / 44100 * 1.001;
                noise = noise * 1664525u + 1013904223u;
                int64_t late = int64_t((noise >> 8) % 44) - 22; // the stamp up to half a millisecond either way
                mtc(2, master, uint64_t(int64_t(frame) + late));
            }
            render();
        }
        double master = start + double(frame - from) / 44100 * 1.001;
        double ticks = master * 192;
        CHECK(std::fabs(double(host.ticks()) - ticks) < 2 && host.mtc_places() == 1);
        CHECK(host.mtc_trim_ppm() > 900 && host.mtc_trim_ppm() < 1100 && host.mtc_error_us() > 300 && host.mtc_error_us() < 700);
        // A sample far off is ignored; four in a row place it again.
        mtc(2, master + 5, frame); render();
        CHECK(host.mtc_places() == 1);
        for (unsigned n = 0; n < 4; ++n) { mtc(2, master + 5 + n * 128 / 44100.0, frame); render(); }
        CHECK(host.mtc_places() == 2 && std::fabs(double(host.ticks()) - (master + 5) * 192) < 4);
        // A locate within 20 ms of where it is: no jump.
        mtc(1, master + 5 + 5 * 128 / 44100.0, frame); render();
        CHECK(host.mtc_places() == 2);
        // BPM per pattern: pattern 1 at 90 BPM leads, so 2 s is tick 288.
        host.send({Command::Type::Stop});
        host.send({Command::Type::PatternTempo, 0, 0, 0, 900});
        host.send({Command::Type::SeqTempo, 0, 0, 0, 1});
        render();
        CHECK(!host.playing() && !host.mtc_following());
        host.send({Command::Type::Play});
        mtc(1, 2.0, frame);
        render();
        CHECK(host.ticks() >= 288 && host.ticks() <= 289);
        host.send({Command::Type::Stop});
        host.send({Command::Type::SeqTempo, 0, 0, 0, 0});
        // The song: bar 5 and a beat; then round its loop (bars 3-4); past its end (no loop) it stops.
        Clip clip; clip.track = 0; clip.pattern = 0; clip.bar = 0; clip.bars = 8;
        host.send([&] { Command c {Command::Type::SongClip}; c.index = 0; c.clip = clip; return c; }());
        host.send({Command::Type::SongCount, 0, 0, 0, 1});
        render();
        host.send({Command::Type::Play, 0, 0, 0, 1});
        mtc(1, 8.5, frame);
        render();
        CHECK(host.song_playing() && host.song_bar() == 4 && host.position_of(0) >= 96 && host.position_of(0) <= 97);
        for (unsigned n = 0; n < 600; ++n) render(); // into bar 6: three beats left of bar 5 (1.5 s), 1.74 s rendered
        CHECK(host.song_bar() == 5);
        host.send({Command::Type::Stop});
        host.send({Command::Type::SongLoop, 0, 0, 2, 4});
        host.send({Command::Type::Play, 0, 0, 0, 1});
        mtc(1, 10.0, frame); // bar 6 on the bar line: the loop's bar 3 (index 2 + (5 - 2) % 2 = 3)
        render();
        CHECK(host.song_bar() == 3 && host.position_of(0) >= 0 && host.position_of(0) <= 1);
        host.send({Command::Type::Stop});
        host.send({Command::Type::SongLoop, 0, 0, 0, 0});
        host.send({Command::Type::Play, 0, 0, 0, 1});
        mtc(1, 15.5, frame); // the last bar's last beat: the song ends by itself and MTC no longer leads
        render();
        CHECK(host.song_bar() == 7 && host.mtc_following());
        for (unsigned n = 0; n < 200; ++n) render();
        CHECK(!host.playing() && !host.mtc_following());
        host.send({Command::Type::Play, 0, 0, 0, 1});
        mtc(1, 20.5, frame); // bar 11 of an eight-bar song
        render(); render();
        CHECK(!host.playing() && !host.mtc_following());
        CHECK(host.dropped() == 0 && host.midi_dropped() == 0);
    }
}

// Parity phase 10: the sampler's page groups, undo scopes and Load Kit.
// Parity phase 10b: SD card access. While the card is away (or its page is open) only B1, the question's B3,
// Play and Stop answer; B1 takes the card back, or asks first while the computer holds it; the shell follows the
// service thread's view: the card leaving opens the page, coming back leaves it.
void card_access() {
    using B = SurfaceButton;
    UiState ui;
    ui.page = PageId::CardAccess; // Project's row opened it; the lend not answered yet (state home)
    CHECK(card_owns_controls(ui));
    auto r = card_press(ui, B::Seq);
    CHECK(r.taken && !r.leave && r.request == CardRequest::None);       // other modes wait
    CHECK(!card_press(ui, B::Play).taken && !card_press(ui, B::Stop).taken); // the transport still works
    r = card_press(ui, B::TopLeft);
    CHECK(r.taken && r.leave && r.request == CardRequest::None);        // nothing lent: B1 is back

    CardView lent; lent.state = 2; lent.connected = true;
    ui.page = PageId::Stage; // lent from anywhere (the remote): the shell opens the page
    auto change = card_follow(ui, lent);
    CHECK(change.left && !change.back && card_owns_controls(ui));
    CHECK(card_press(ui, B::Shift).taken && card_press(ui, B::TopRight).request == CardRequest::None);
    CHECK(card_press(ui, B::TopLeft).request == CardRequest::Return);   // B1: take it back
    lent.locked = true; card_follow(ui, lent);                         // ... but the computer still holds it
    r = card_press(ui, B::TopLeft);
    CHECK(r.request == CardRequest::None && ui.ask == UiState::Ask::TakeCardBack); // B1 again: asks
    CHECK(card_press(ui, B::TopLeft).request == CardRequest::None && ui.ask == UiState::Ask::None); // Cancel: stays lent
    card_press(ui, B::TopLeft);
    CHECK(card_press(ui, B::TopRight).request == CardRequest::Force && ui.ask == UiState::Ask::None); // B3: take it anyway
    lent.ejected = true; card_follow(ui, lent);                        // ejected on the computer: B1 simply returns it
    CHECK(card_press(ui, B::TopLeft).request == CardRequest::Return);
    CHECK(std::string(card_title(ui.card)) == "Ejected");

    CardView back; back.returns = 1;
    ui.ask = UiState::Ask::TakeCardBack;
    change = card_follow(ui, back);
    CHECK(change.back && !change.left && ui.ask == UiState::Ask::None && !card_away(ui.card));
    CHECK(!card_follow(ui, back).back); // once

    // The lights: only the transport (and B1, unlit) has a job while the card is away; no Shift jobs.
    Project project;
    ui.page = PageId::CardAccess; ui.card = lent;
    CHECK(!button_has_job(project, ui, B::Seq, false, false) && button_has_job(project, ui, B::Play, false, false));
    CHECK(!button_has_shift_job(project, ui, B::Seq, false));
    CardView none; none.note = CardNote::NoCard;
    CHECK(std::string(card_title(none)) == "No card");
}

void projects_and_polish() {
    // B1 Trim -> Mix -> Amp Env, B2 Tune -> Play -> Follow -> Cloud, B3 Filter -> Filt Env -> Kit filter; another group's key: its first.
    CHECK(sampler_next(PageId::Trim, 0) == PageId::PadMix && sampler_next(PageId::PadMix, 0) == PageId::EnvelopePage);
    CHECK(sampler_next(PageId::EnvelopePage, 0) == PageId::Trim);
    CHECK(sampler_next(PageId::Trim, 1) == PageId::PadPage && sampler_next(PageId::PadPage, 1) == PageId::PlayPage);
    CHECK(sampler_next(PageId::PlayPage, 1) == PageId::FollowPage && sampler_next(PageId::CloudPage, 1) == PageId::PadPage);
    CHECK(sampler_next(PageId::PadMix, 2) == PageId::PadFilterPage && sampler_next(PageId::PadFilterPage, 2) == PageId::FilterEnvPage);
    CHECK(sampler_next(PageId::FilterEnvPage, 2) == PageId::FilterPage && sampler_next(PageId::FilterPage, 2) == PageId::PadFilterPage);
    CHECK(sampler_next(PageId::Pads, 1) == PageId::PadPage); // from outside the ten: the group's first
    CHECK(is_sampler_page(PageId::CloudPage) && !is_sampler_page(PageId::Chop) && !is_sampler_page(PageId::Pads));
    unsigned covered = 0;
    for (unsigned g = 0; g < sampler_groups; ++g) {
        CHECK(sampler_group_size(g) >= 2);
        for (unsigned k = 0; k < sampler_group_size(g); ++k) { CHECK(sampler_group(sampler_group_first(g) + k) == g); ++covered; }
    }
    CHECK(covered == sampler_page_count);

    // Undo scopes: what a card or a press is part of.
    Project project;
    load_sound(project.tracks[0], EngineKind::Prophet, 1);
    load_sound(project.tracks[2], EngineKind::Sampler, 0);
    project.tracks[2].last_pad = 17;
    UiState ui;
    ui.track = 2;
    using K = Binding::Kind;
    using S = UndoScope::Kind;
    auto scope = [&](K kind, int id = 0) { Binding b; b.kind = kind; b.id = id; b.max = 100; return binding_scope(project, ui, b); };
    CHECK(scope(K::PadCutoff) == (UndoScope {S::Pad, 2, 17}) && scope(K::PadStart).kind == S::Pad && scope(K::PadWarp).kind == S::Pad);
    CHECK(scope(K::Level).kind == S::Mix && scope(K::BusReturn).kind == S::Mix && scope(K::Send).kind == S::Mix);
    CHECK(scope(K::FxParam, 3 * 64 + 1 * 8 + 2) == (UndoScope {S::Chain, 2, 3}));
    CHECK(scope(K::KnobParam).kind == S::Master && scope(K::CompAmount).kind == S::Master && scope(K::FlexMix).kind == S::Master);
    CHECK(scope(K::ChopStart).kind == S::Chop && scope(K::ChopMode).kind == S::Chop);
    CHECK(scope(K::Param).kind == S::Sound && scope(K::ModAmount).kind == S::Sound);
    CHECK(scope(K::SongLength, 2).kind == S::Song && scope(K::SongLength, -1).none());
    CHECK(scope(K::Tempo).none() && scope(K::PatternLength).none() && scope(K::FileJump).none());
    ui.track = 0; // a synth track has no kit pad
    CHECK(scope(K::PadLevel).none());
    using B = SurfaceButton;
    ui.track = 2; ui.page = PageId::Stage;
    CHECK(press_scope(project, ui, B::Loop, false) == (UndoScope {S::Pad, 2, 17}));
    ui.shift = true;
    CHECK(press_scope(project, ui, B::Chop, false).kind == S::Pad);
    ui.shift = false;
    CHECK(press_scope(project, ui, B::TopLeft, false).none()); // Stage keeps nothing
    ui.page = PageId::Compressor; CHECK(press_scope(project, ui, B::KnobFx, false).none()); // a mode, not an edit
    ui.page = PageId::Stage;
    ui.page = PageId::Song; CHECK(press_scope(project, ui, B::WheelPush, false).kind == S::Song);
    ui.page = PageId::Rack; CHECK(press_scope(project, ui, B::TopRight, false) == (UndoScope {S::Chain, 2, 2}));
    ui.page = PageId::Master; CHECK(press_scope(project, ui, B::TopRight, false).index == master_chain);
    ui.page = PageId::Compressor; CHECK(press_scope(project, ui, B::TopLeft, false).kind == S::Master);
    CHECK(scope_name(project, ui, press_scope(project, ui, B::TopLeft, false)) == "Compressor");
    ui.page = PageId::Chop; CHECK(press_scope(project, ui, B::TopMiddle, false).kind == S::Chop);
    ui.chop_armed = true; CHECK(press_scope(project, ui, B::Count, true).none()); // Manual's taps
    ui.page = PageId::PlayPage; CHECK(press_scope(project, ui, B::Count, true).kind == S::Pad);
    CHECK(scope_name(project, ui, UndoScope {S::Pad, 2, 17}) == "B02");
    CHECK(scope_name(project, ui, UndoScope {S::Chain, 0, uint16_t(bus_chain(1))}) == "Bus B FX");
    CHECK(scope_name(project, ui, UndoScope {S::Chain, 0, 4}) == "T5 FX");
    CHECK(scope_name(project, ui, UndoScope {S::Chain, 2, uint16_t(pad_chain(2, 3))}) == "A04 FX");
    // Roll R0: on Roll, Steps and Step Edit a press keeps the selected track's pattern, and so do the note cards.
    project.tracks[2].pattern = 3;
    ui.page = PageId::Roll; CHECK(press_scope(project, ui, B::Count, true) == (UndoScope {S::Pattern, 2, 3}));
    ui.page = PageId::Steps; CHECK(press_scope(project, ui, B::WheelPush, false).kind == S::Pattern);
    ui.page = PageId::StepEdit; CHECK(press_scope(project, ui, B::TopRight, false).kind == S::Pattern);
    CHECK(press_scope(project, ui, B::Play, false).none()); // the transport is not an edit
    CHECK(scope(K::StepVelocity) == (UndoScope {S::Pattern, 2, 3}) && scope(K::StepLength).kind == S::Pattern);
    CHECK(scope(K::StepChance).kind == S::Pattern && scope(K::EventVelocity).kind == S::Pattern && scope(K::EventLength).kind == S::Pattern);
    CHECK(scope(K::Division).none()); // the grid moves no note
    CHECK(scope_name(project, ui, pattern_scope(project, ui)) == "T3 P4");
    ui.page = PageId::Patterns; CHECK(press_scope(project, ui, B::Count, true).none()); // a pad there plays, it writes nothing
    ui.page = PageId::Roll; ui.seq_held = true; // Seq held: a pad launches, B1-B3 open editors; nothing is written
    CHECK(press_scope(project, ui, B::Count, true).none() && press_scope(project, ui, B::TopMiddle, false).none());
    ui.seq_held = false;
    // The states compare as they should: mute and Knob FX's targets are left out.
    auto mix = mix_of(project);
    project.tracks[5].mix.mute = true; project.knob_fx.tracks = 3;
    auto mix1 = mix_of(project);
    CHECK(mix == mix1);
    auto master = master_of(project);
    auto master1 = master_of(project);
    CHECK(master == master1);
    project.tracks[5].mix.pan = -20; project.compressor.amount = 77;
    auto mix2 = mix_of(project); auto master2 = master_of(project);
    CHECK(!(mix == mix2) && mix2.pan[5] == -20);
    CHECK(!(master == master2) && master2.compressor.amount == 77);
    History h;
    unsigned before = h.pushes;
    h.push(Edit {});
    CHECK(h.pushes == before + 1 && h.size() == 1 && h.last());

    // A new project: one empty kit on track 1, C Chromatic, no notes; every pad its own semitone.
    {
        auto p = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        starting_project(*p, *bank);
        CHECK(p->used_tracks() == 1 && p->tracks[0].engine == EngineKind::Sampler && kit_track(*p) == -1);
        CHECK(p->root == 0 && std::string(scales()[p->scale].name) == "Chromatic");
        unsigned notes = 0;
        for (unsigned t = 0; t < track_count; ++t) for (unsigned q = 0; q < pattern_count; ++q) notes += unsigned(bank->at[t][q].events.size());
        CHECK(notes == 0);
        auto& t = p->tracks[1];
        t.octave = 4; // C4
        CHECK(pad_note(*p, t, 0).key == 60 && pad_note(*p, t, 0).root && pad_note(*p, t, 7).key == 67 && !pad_note(*p, t, 7).root);
        CHECK(pad_note(*p, t, 12).key == 72 && pad_note(*p, t, 12).root);
        CHECK(levels_of(*p, t, 15).semitones == 0); // Velocity until asked
        t.levels_kind = uint8_t(LevelsKind::Scale);
        CHECK(levels_of(*p, t, 15).semitones == 15);
        // Chords in Chromatic: a major triad on each pad's own semitone, not a cluster.
        std::array<int, notes_per_step> keys {};
        t.chord_type = 0; t.chord_voicing = 0; t.chord_invert = 0;
        CHECK(chord_notes(*p, t, 1, keys) == 3 && keys[0] == 61 && keys[1] == 65 && keys[2] == 68);
        CHECK(chord_name(*p, t, 1) == "C#");
        t.chord_type = 5;
        CHECK(chord_notes(*p, t, 0, keys) == 4 && keys[3] == 74); // Dia 9
    }
    // The project's key on a kit: Shift + K1 / K2 on Pads, Tempo and 16 Levels; of type Scale plain K1 / K2, even on an empty pad.
    {
        auto p = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        starting_project(*p, *bank);
        UiState ui;
        Pattern pattern;
        using K = Binding::Kind;
        for (PageId page : {PageId::Pads, PageId::Tempo, PageId::Levels}) {
            ui.page = page;
            auto shifted = page_bindings(*p, ui, pattern, true);
            CHECK(shifted[0].kind == K::Root && shifted[1].kind == K::Scale && shifted[1].max == 12);
        }
        ui.page = PageId::Levels;
        CHECK(!page_bindings(*p, ui, pattern, false)[0].bound());
        p->tracks[0].levels_kind = uint8_t(LevelsKind::Scale);
        CHECK(page_bindings(*p, ui, pattern, false)[0].kind == K::Root && page_bindings(*p, ui, pattern, false)[1].kind == K::Scale);
    }
    // The metronome is Melodee's, not the project's: kept in the settings file; older files read Off at 40.
    {
        Settings a;
        a.click = 2; a.click_level = 75;
        Settings b;
        b.read(a.text());
        CHECK(b.click == 2 && b.click_level == 75);
        Settings old;
        old.read("screen=50\n");
        CHECK(old.click == 0 && old.click_level == 40);
    }

    // Load Kit: the first kit with a sample, onto a synth track; each file asked for once, a pad without a slot left empty.
    auto from = std::make_unique<Project>();
    load_sound(from->tracks[1], EngineKind::Prophet, 2);
    load_sound(from->tracks[3], EngineKind::Sampler, 0);
    load_sound(from->tracks[6], EngineKind::Sampler, 0);
    auto ref = [&](unsigned slot, const char* name) {
        std::snprintf(from->samples[slot].path, sizeof(from->samples[slot].path), "/card/Samples/%s.wav", name);
        std::snprintf(from->samples[slot].name, sizeof(from->samples[slot].name), "%s", name);
    };
    ref(0, "KICK"); ref(5, "SNARE"); ref(9, "GONE");
    auto& kit = from->tracks[6];
    kit.pads[0].sample = 0; kit.pads[1].sample = 0; kit.pads[2].sample = 5; kit.pads[40].sample = 9;
    kit.pads[2].pitch = 7; kit.pad_fx.at[2].slots[0].kind = uint8_t(EffectKind::Drive);
    kit.params[sampler::Filter] = 33;
    CHECK(kit_track(*from) == 6);
    Track into = project.tracks[0]; // a PROPHET
    into.mix.level = 55;
    unsigned asked = 0;
    auto result = import_kit(*from, 6, into, [&](const SampleRef& r) {
        ++asked;
        if (std::string(r.name) == "GONE") return -1;
        return std::string(r.name) == "KICK" ? 20 : 21;
    });
    CHECK(asked == 3 && result.pads == 3 && result.missing == 1);
    CHECK(into.engine == EngineKind::Sampler && into.pads[0].sample == 20 && into.pads[1].sample == 20 && into.pads[2].sample == 21);
    CHECK(into.pads[2].pitch == 7 && into.pad_fx.at[2].slots[0].kind == uint8_t(EffectKind::Drive) && !into.pads[40].used());
    CHECK(into.params[sampler::Filter] == 33 && into.mix.level == 55); // the kit's own values; the track keeps its strip

    // Bank files: bank C of a kit as a file, every pad value and chain kept, its files listed once.
    kit.pads[32].sample = 5; kit.pads[32].pitch = -3; kit.pads[32].loop = true; kit.pads[32].filter = PadFilter::Bpf2;
    kit.pads[32].link = 4; kit.pad_fx.at[32].slots[0].kind = uint8_t(EffectKind::Drive);
    kit.pads[33].sample = 5; kit.pads[47].sample = 0; kit.pads[47].group = 3;
    kit.pads[34].sample = 7; // a slot with no file: left out
    kit.pads[40] = PadSound {};
    auto saved = bank_of(*from, 6, 2, "BREAKS C");
    CHECK(saved.used() == 3 && saved.pads[0].sample == 0 && saved.pads[1].sample == 0 && saved.pads[15].sample == 1);
    CHECK(std::string(saved.samples[0].name) == "SNARE" && std::string(saved.samples[1].name) == "KICK" && !saved.samples[2].used());
    CHECK(!bank_of(*from, 6, 7, "EMPTY").used());
    BankFile loaded;
    CHECK(load_bank(save_bank(saved), loaded));
    CHECK(std::string(loaded.name) == "BREAKS C" && loaded.used() == 3);
    CHECK(loaded.pads[0].pitch == -3 && loaded.pads[0].loop && loaded.pads[0].filter == PadFilter::Bpf2 && loaded.pads[0].link == 4);
    CHECK(loaded.pads[15].group == 3 && loaded.pads[15].sample == 1 && !loaded.pads[2].used());
    CHECK(loaded.chains[0].slots[0].kind == uint8_t(EffectKind::Drive) && loaded.chains[0].size == pad_slots && !loaded.chains[1].count());
    CHECK(std::string(loaded.samples[1].path) == "/card/Samples/KICK.wav");
    CHECK(!load_bank(save_project(*from, *std::make_unique<PatternBank>()), loaded)); // a project is not a bank
    auto cut = save_bank(saved);
    cut.resize(cut.size() - 5);
    BankFile damaged;
    CHECK(!load_bank(cut, damaged));
    // Into bank B of another kit: only its sixteen pads change (and their mutes); each file asked for once.
    Track other = into;
    other.pads[16].sample = 30; other.pads[20].sample = 31; other.set_pad_muted(17, true);
    other.pads[3].sample = 32; other.pad_fx.at[20].slots[0].kind = uint8_t(EffectKind::Drive);
    asked = 0;
    auto put = import_bank(loaded, 1, other, [&](const SampleRef& r) { ++asked; return std::string(r.name) == "KICK" ? 40 : 41; });
    CHECK(asked == 2 && put.pads == 3 && put.missing == 0);
    CHECK(other.pads[16].sample == 41 && other.pads[16].pitch == -3 && other.pads[17].sample == 41 && other.pads[31].sample == 40);
    CHECK(!other.pads[20].used() && !other.pad_fx.at[20].count() && other.pad_fx.at[16].slots[0].kind == uint8_t(EffectKind::Drive));
    CHECK(!other.pad_muted(17) && other.pads[3].sample == 32 && other.params[sampler::Filter] == 33);
    asked = 0;
    put = import_bank(loaded, 7, other, [&](const SampleRef&) { ++asked; return -1; }); // no free slot
    CHECK(asked == 2 && put.pads == 0 && put.missing == 3 && !other.pads[112].used());
}

// Settings' sample rate (44.1 / 48 / 96 kHz): the engine follows the device's rate, so a project plays at the same
// tempo and pitch at each, and what is recorded or rendered is kept at 44.1 kHz.
void device_rates() {
    // The sequencer: a new rate waits, silent, for the project again; then at 48 kHz step 5 at 120 BPM starts 0.5 s in.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        load_sound(project->tracks[0], EngineKind::Prophet, 1);
        project->tempo = 1200;
        put(bank->at[0][0], 4, 45, 100);
        auto host = std::make_unique<EngineHost>();
        host->load(*project, *bank);
        host->set_audio_format(48000, 384);
        CHECK(host->format_changes() == 1 && dsp::sample_rate() == 48000.0f);
        std::vector<int32_t> out(256);
        host->send({Command::Type::Audition, 0, 0, 0, 60});
        for (unsigned n = 0; n < 10; ++n) { host->render(out.data(), 128, 1); for (auto v : out) CHECK(v == 0); }
        Command reload {Command::Type::Reload};
        reload.pack = new ProjectPack(*project, *bank);
        host->send(reload);
        host->send({Command::Type::Play});
        std::vector<float> left;
        for (unsigned n = 0; n < 260; ++n) {
            host->render(out.data(), 128, 1);
            for (unsigned f = 0; f < 128; ++f) left.push_back(out[2 * f] / 2147483648.0f);
        }
        CHECK(host->playing());
        CHECK(peak_of(left, 0, 23900) == 0);
        CHECK(peak_of(left, 24000, 32000) > 0.005f);
        const ProjectPack* done = nullptr;
        while (host->take_done_pack(done)) delete done;
        host->set_audio_format(48000, 512); // the same rate, more buffer: nothing to work out again
        CHECK(host->format_changes() == 1);
    }
    // A pad: a 441 Hz sample (at 44.1 kHz, as samples are kept) plays at 441 Hz and lasts a second at 48 and 96 kHz.
    {
        auto tone = std::make_unique<SampleData>();
        tone->frames = 44100;
        tone->left.resize(44100); tone->right.resize(44100);
        for (unsigned n = 0; n < 44100; ++n) tone->left[n] = tone->right[n] = 0.5f * std::sin(2 * 3.14159265 * 441 * n / 44100.0);
        SamplerEngine::Table tones {};
        tones[0] = tone.get();
        for (float rate : {48000.0f, 96000.0f}) {
            dsp::device_rate.store(rate);
            SamplerEngine engine;
            engine.set_samples(&tones);
            engine.set_tempo(1200);
            PadSound sound; sound.sample = 0; sound.decay = 100;
            engine.set_pad(0, sound);
            engine.note_on(int(sampler_first_note), 120);
            std::vector<float> l(128), r(128), all;
            for (unsigned b = 0; b < 2000 && engine.sounding(); ++b) {
                std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
                engine.render_stereo(l.data(), r.data(), 128);
                all.insert(all.end(), l.begin(), l.end());
            }
            unsigned crossings = 0, from = 2000, to = unsigned(rate * 0.8f);
            for (unsigned n = from + 1; n < to; ++n) crossings += (all[n - 1] < 0) != (all[n] < 0);
            float hz = crossings / 2.0f / ((to - from) / rate);
            if (!(std::fabs(float(all.size()) - rate) < 600 && std::fabs(hz - 441) < 5)) std::fprintf(stderr, "%g: %zu frames %f Hz\n", double(rate), all.size(), double(hz));
            CHECK(std::fabs(float(all.size()) - rate) < 600 && std::fabs(hz - 441) < 5);
        }
    }
    // A take at 96 kHz: a second of 1 kHz is kept as 44100 frames of 1 kHz; 30 kHz (above what 44.1 kHz holds) is filtered out, not folded down.
    {
        dsp::device_rate.store(96000.0f);
        for (float hz : {1000.0f, 30000.0f}) {
            auto take = std::make_unique<TakeRecorder>();
            take->set_rate(96000);
            take->arm(-100, 0, 0);
            std::vector<float> l(128), r(128);
            for (unsigned b = 0; b < 750; ++b) {
                for (unsigned f = 0; f < 128; ++f) l[f] = r[f] = 0.5f * std::sin(2 * 3.14159265f * hz * float(b * 128 + f) / 96000.0f);
                take->process(l.data(), r.data(), 128);
            }
            take->stop();
            CHECK(take->state() == TakeRecorder::State::Done && std::abs(int(take->frames()) - 44100) <= 2);
            std::vector<float> kept(take->left(), take->left() + take->frames());
            if (hz < 2000) {
                unsigned crossings = 0;
                for (unsigned n = 1001; n < 41000; ++n) crossings += (kept[n - 1] < 0) != (kept[n] < 0);
                CHECK(std::fabs(crossings / 2.0f / (40000 / 44100.0f) - 1000) < 5);
                CHECK(peak_of(kept, 1000) > 0.45f);
            } else CHECK(peak_of(kept, 1000) < 0.03f);
        }
        // Resample at 96 kHz: a second rendered is 44100 frames kept.
        auto bounce = std::make_unique<Bounce>();
        bounce->set_rate(96000);
        bounce->cue(); bounce->begin(0);
        std::vector<float> l(128, 0.25f), r(128, 0.25f), drained(4096 * 2);
        unsigned popped = 0;
        for (unsigned b = 0; b < 750; ++b) { bounce->process(l.data(), r.data(), 128); popped += bounce->pop(drained.data(), 4096); }
        CHECK(std::abs(int(bounce->frames()) - 44100) <= 2 && popped == bounce->frames());
    }
    dsp::device_rate.store(44100.0f);
    // Settings: kept and read back; a rate the device has no row for is left out, a buffer between rows goes up to one.
    {
        Settings a;
        CHECK(a.audio_rate == 44100 && a.audio_periods == 3 && a.value_text(Settings::AudioBuffer) == "384  8.7 ms");
        a.step(Settings::SampleRate, 1); a.step(Settings::SampleRate, 1); a.step(Settings::SampleRate, 1);
        a.step(Settings::AudioBuffer, 1); a.step(Settings::AudioBuffer, 1);
        CHECK(a.audio_rate == 96000 && a.audio_periods == 6 && a.value_text(Settings::SampleRate) == "96 kHz");
        Settings b;
        b.read(a.text());
        CHECK(b.audio_rate == 96000 && b.audio_periods == 6);
        b.read("audio_rate=22050\naudio_periods=5\n");
        CHECK(b.audio_rate == 96000 && b.audio_periods == 6);
        b.read("audio_periods=1\n");
        CHECK(b.audio_periods == 2 && b.value_text(Settings::AudioBuffer) == "256  2.7 ms");
        b.reset_factory();
        CHECK(b.audio_rate == 44100 && b.audio_periods == 3);
    }
}

// Sampling from an engine: the Track input source (before or after the
// track's chain, heard once while monitored), a Free take ending on its own
// after a quiet stretch, Audio Recall's cuts, the K1 list and the settings.
void track_source() {
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    for (auto& t : project->tracks) t.engine = EngineKind::None;
    load_sound(project->tracks[0], EngineKind::Prophet, 0);
    auto host = std::make_unique<EngineHost>();
    host->load(*project, *bank);
    std::vector<int32_t> out(256);
    auto run = [&](unsigned blocks) { for (unsigned n = 0; n < blocks; ++n) host->render(out.data(), 128, 1, nullptr); };
    auto source = [&](bool post) { host->send({Command::Type::RecordSource, uint8_t(InputSource::Track), 0, 0, int16_t(post)}); };
    source(true);
    host->send({Command::Type::NoteOn, 0, 0, 60, 110});
    run(80);
    CHECK(host->record_source() == unsigned(InputSource::Track) && host->record_track() == 0 && host->record_post());
    float post_full = host->recorder().level();
    CHECK(post_full > 0.01f);
    source(false);
    run(40);
    float pre_full = host->recorder().level();
    CHECK(!host->record_post() && pre_full > 0.01f);
    // The track turned down: after its level the tap follows, the engine alone does not.
    host->send({Command::Type::Level, 0, 0, 0, 10});
    run(80);
    CHECK(std::fabs(host->recorder().level() - pre_full) < pre_full * 0.35f);
    source(true);
    run(80);
    CHECK(host->recorder().level() < post_full * 0.5f);
    // Muted, it still records (the tap is before mute).
    host->send({Command::Type::Level, 0, 0, 0, 80});
    host->send({Command::Type::Mute, 0, 0, 0, 1});
    run(80);
    CHECK(host->recorder().level() > 0.01f);
    run(4);
    int32_t muted_peak = 0;
    for (auto v : out) muted_peak = std::max(muted_peak, std::abs(v));
    CHECK(muted_peak == 0);
    host->send({Command::Type::Mute, 0, 0, 0, 0});
    // Monitored, the track is heard through the input instead of the mix: once, as loud as before.
    auto peak_out = [&]() {
        int32_t peak = 0;
        for (unsigned n = 0; n < 40; ++n) { host->render(out.data(), 128, 1, nullptr); for (auto v : out) peak = std::max(peak, std::abs(v)); }
        return peak;
    };
    int32_t mixed = peak_out();
    host->send({Command::Type::Monitor, 0, 0, 0, 1});
    int32_t monitored = peak_out();
    CHECK(mixed > 0 && std::abs(double(monitored) / mixed - 1.0) < 0.2);
    host->send({Command::Type::Monitor, 0, 0, 0, 0});
    // Another track or tap: what the ring holds is another source's.
    run(10);
    CHECK(host->recall().filled() > 1280);
    source(false);
    run(1);
    CHECK(host->recall().filled() == 128);
    host->send({Command::Type::RecordSource, uint8_t(InputSource::Track), 0, 1, 0});
    run(1);
    CHECK(host->recall().filled() == 128 && host->record_track() == 1);
    // Rec length Seq follows the track named (the source's pattern: a bar at 120 BPM).
    bank->at[0][0].length = project->bar_ticks();
    host = std::make_unique<EngineHost>();
    host->load(*project, *bank);
    Command seq {Command::Type::RecordArm, uint8_t(InputSource::Track), 255, 3, -96};
    seq.position = 0;
    host->send(seq);
    run(1);
    CHECK(host->recorder().length() + 1 >= 88200 && host->recorder().length() <= 88201);
    host->send({Command::Type::RecordStop});
    run(1);
    host->recorder().release();
    host->send({Command::Type::NoteOff, 0, 0, 60, 0});

    // A Free take ends on its own once quiet for 1.5 s, 10 ms after its last sound; without it, it waits.
    {
        auto take = std::make_unique<TakeRecorder>();
        uint32_t quiet = uint32_t(1.5f * dsp::model_rate);
        take->arm(-96, 0, 0, false, quiet, -60);
        std::vector<float> loud(128, 0.5f), silent(128, 0.0f);
        unsigned blocks = 0;
        for (; blocks * 128 < 22050; ++blocks) take->process(loud.data(), loud.data(), 128);
        uint32_t sounded = blocks * 128;
        for (unsigned n = 0; n < 1000 && take->state() == TakeRecorder::State::Recording; ++n) take->process(silent.data(), silent.data(), 128);
        CHECK(take->state() == TakeRecorder::State::Done);
        CHECK(take->frames() == sounded + uint32_t(dsp::model_rate) / 100);
        take->release();
        take->arm(-96, 0, 0, false, 0, -60);
        take->process(loud.data(), loud.data(), 128);
        for (unsigned n = 0; n < 1000; ++n) take->process(silent.data(), silent.data(), 128);
        CHECK(take->state() == TakeRecorder::State::Recording);
        take->stop();
        take->release();
        take->arm(-96, 0, 0, false, quiet, -60); // quiet from the start: nothing heard, nothing ends it
        for (unsigned n = 0; n < 1000; ++n) take->process(silent.data(), silent.data(), 128);
        CHECK(take->state() == TakeRecorder::State::Recording);
    }

    // Audio Recall: burst A, 2 s quiet, burst B, 0.5 s quiet.
    {
        const uint32_t rate = RecallRing::rate, pad = rate / 100;
        uint32_t a0 = rate / 2, a1 = a0 + rate / 2, b0 = a1 + 2 * rate, b1 = b0 + rate / 2, total = b1 + rate / 2;
        std::vector<float> l(total, 0.0f), r(total, 0.0f);
        for (uint32_t f = a0; f < a1; ++f) l[f] = 0.4f;
        for (uint32_t f = b0; f < b1; ++f) r[f] = -0.4f;
        RecallOptions o;
        uint32_t first = 0, end = 0;
        CHECK(recall_bounds(l.data(), r.data(), total, rate, o, first, end) && first == b0 - pad && end == b1 + pad);
        o.gap = 4 * rate;
        CHECK(recall_bounds(l.data(), r.data(), total, rate, o, first, end) && first == a0 - pad && end == b1 + pad);
        o.grab = 1; o.gap = rate;
        CHECK(recall_bounds(l.data(), r.data(), total, rate, o, first, end) && first == a0 - pad && end == b1 + pad);
        o.trim_end = false;
        CHECK(recall_bounds(l.data(), r.data(), total, rate, o, first, end) && first == a0 - pad && end == total);
        o.grab = 2;
        CHECK(recall_bounds(l.data(), r.data(), total, rate, o, first, end) && first == 0 && end == total);
        std::vector<float> none(total, 0.0f);
        CHECK(recall_bounds(none.data(), none.data(), total, rate, o, first, end));
        o.grab = 0;
        CHECK(!recall_bounds(none.data(), none.data(), total, rate, o, first, end));
        o.trim_end = true; // one burst with short gaps inside: kept whole
        std::vector<float> one(total, 0.0f);
        for (uint32_t f = a0; f < a1; ++f) one[f] = 0.4f;
        for (uint32_t f = a1 + rate / 2; f < a1 + rate; ++f) one[f] = 0.4f;
        CHECK(recall_bounds(one.data(), one.data(), total, rate, o, first, end) && first == a0 - pad && end == a1 + rate + pad);
    }

    // K1's list: the inputs, then tracks with a sound; the kit taken onto, empty and MIDI tracks left out.
    {
        Project p;
        for (auto& t : p.tracks) t.engine = EngineKind::None;
        p.tracks[0].engine = EngineKind::Prophet; p.tracks[2].engine = EngineKind::Kit909;
        p.tracks[3].engine = EngineKind::Sampler; p.tracks[4].engine = EngineKind::Midi; p.tracks[5].engine = EngineKind::Sampler;
        InputConfig in;
        auto choices = input_choices(p, 3, in);
        CHECK(choices.count == 8 + 3 && choices.codes[8] == input_track_code + 0 && choices.codes[9] == input_track_code + 2
              && choices.codes[10] == input_track_code + 5);
        CHECK(input_choices(p, 0, in).count == 8 + 4); // a synth in view is a source too
        in.source = InputSource::Track; in.track = 7; // gone quiet: still listed while chosen
        auto kept = input_choices(p, 3, in);
        CHECK(kept.count == 12 && kept.codes[kept.index_of(input_code(in))] == input_track_code + 7);
        CHECK(internal_source(InputSource::Track) && internal_source(InputSource::Resample) && !internal_source(InputSource::Mic));
    }

    // The settings: the track and its tap, and the six sampling rows, kept and read back.
    {
        Settings a;
        CHECK(a.value_text(Settings::RecallGrabRow) == "Last Phrase" && a.value_text(Settings::RecallGap) == "1 s"
              && a.value_text(Settings::TakeAutoEnd) == "Synths & Resample" && a.value_text(Settings::AutoEndAfter) == "1.5 s"
              && a.value_text(Settings::SilenceBelow) == "-60 dB" && a.value_text(Settings::RecallTrimEnd) == "On");
        CHECK(a.sampling.auto_ends(InputSource::Track) && !a.sampling.auto_ends(InputSource::Mic));
        a.input.source = InputSource::Track; a.input.track = 5; a.input.post = false;
        a.step(Settings::RecallGrabRow, 1); a.step(Settings::RecallGap, -1); a.step(Settings::RecallTrimEnd, 1);
        a.step(Settings::TakeAutoEnd, 1); a.step(Settings::AutoEndAfter, 1); a.step(Settings::SilenceBelow, 1);
        a.step(Settings::SilenceBelow, 1);
        Settings b;
        b.read(a.text());
        CHECK(b.input.source == InputSource::Track && b.input.track == 5 && !b.input.post);
        CHECK(b.sampling.grab == RecallGrab::FirstSound && b.sampling.gap == 0 && !b.sampling.trim_end && b.sampling.auto_end == AutoEnd::All
              && b.sampling.end_after == 3 && b.sampling.silence_db() == -72 && b.value_text(Settings::RecallGap) == "0.5 s");
        CHECK(b.sampling.auto_ends(InputSource::Mic));
        b.read("recall_grab=9\nrecall_gap=-3\nauto_end_after=40\nsilence_db=-50\ninput_track=99\n");
        CHECK(b.sampling.grab == RecallGrab::Whole && b.sampling.gap == 0 && b.sampling.end_after == 3 && b.sampling.silence_db() == -48
              && b.input.track == 15);
    }
}

// MIDI tracks (docs/MIDI_TRACKS.md, phase M1): the engine, its port and channel, where its notes go, when they end.
void midi_tracks() {
    using K = MidiOutEvent::Kind;
    // The engine: available, one DEVICE page and no mod matrix, port and channel as its parameters.
    {
        const auto& info = engine_info(EngineKind::Midi);
        CHECK(info.available && std::string(info.name) == "MIDI" && info.param_count == midi_track::Count);
        CHECK(edit_page_count(info) == 2 && !is_mod_page(info, 0) && !is_mod_page(info, 1) && edit_page_count(engine_info(EngineKind::Prophet)) == prophet::info().page_count + 1);
        Track t;
        load_sound(t, EngineKind::Midi, 0);
        CHECK(t.engine == EngineKind::Midi && t.params[midi_track::Port] == midi_track::External && t.params[midi_track::Channel] == 1 &&
              std::string(t.name) == "MIDI OUT");
        CHECK(midi_track::dest_of(midi_track::UsbC) == MidiDest::UsbC && midi_track::dest_of(midi_track::AllUsb) == MidiDest::AllUsb &&
              midi_track::dest_of(99) == MidiDest::External);
        std::array<EngineKind, size_t(EngineKind::Count)> engines {};
        unsigned count = available_engines(engines);
        CHECK(count >= 2 && engines[count - 2] == EngineKind::Midi && engines[count - 1] == EngineKind::Plugin); // K1: MIDI, then plug-ins
    }
    // Ports: a MIDI track's own, apart from MIDI Config's.
    {
        CHECK(dest_is(MidiDest::External, MidiPort::Usb, "SerialMIDI", false) && !dest_is(MidiDest::External, MidiPort::Usb, "MIDI Gadget", false));
        CHECK(dest_is(MidiDest::UsbC, MidiPort::External, "MIDI Gadget", false) && !dest_is(MidiDest::UsbC, MidiPort::External, "USB-Audio", false));
        CHECK(dest_is(MidiDest::AllUsb, MidiPort::External, "USB-Audio", false) && dest_is(MidiDest::AllUsb, MidiPort::External, "MIDI Gadget", false) &&
              !dest_is(MidiDest::AllUsb, MidiPort::External, "SerialMIDI", false));
        CHECK(dest_is(MidiDest::Config, MidiPort::External, "SerialMIDI", false) && !dest_is(MidiDest::Config, MidiPort::External, "MIDI Gadget", false));
        CHECK(!dest_is(MidiDest::AllUsb, MidiPort::Usb, "MIDI Gadget", true)); // never the surface
    }
    // MIDI Config's MIDI Panic: a row that changes nothing when stepped.
    {
        Settings set; Track track;
        CHECK(std::string(midi_row_name(PanicRow)) == "MIDI Panic" && PanicRow + 1 == ResetRow);
        MidiConfig before = set.midi;
        midi_row_step(set, track, PanicRow, 1);
        CHECK(set.midi == before && midi_row_value(set, track, PanicRow).empty());
    }
    // The file: Midi in MIDT, None in TRAK, so a build without MIDT loads the track empty.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        load_sound(project->tracks[3], EngineKind::Midi, 0);
        project->tracks[3].params[midi_track::Port] = midi_track::AllUsb; project->tracks[3].params[midi_track::Channel] = 12;
        auto bytes = save_project(*project, *bank);
        auto back = std::make_unique<Project>();
        CHECK(load_project(bytes, *back, *bank).empty());
        CHECK(back->tracks[3].engine == EngineKind::Midi && back->tracks[3].params[midi_track::Port] == midi_track::AllUsb &&
              back->tracks[3].params[midi_track::Channel] == 12 && back->tracks[2].engine == EngineKind::None);
        auto tag = std::search(bytes.begin(), bytes.end(), "MIDT", "MIDT" + 4);
        CHECK(tag != bytes.end());
        std::copy_n("ZZZZ", 4, tag); // as an older build sees it: a section it does not know
        auto older = std::make_unique<Project>();
        CHECK(load_project(bytes, *older, *bank).empty() && older->tracks[3].engine == EngineKind::None);
    }
    // The engine host: a MIDI track sends on its own port and channel, whatever Pad MIDI Out says.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        load_sound(project->tracks[0], EngineKind::Midi, 0);
        project->tracks[0].params[midi_track::Port] = midi_track::UsbC; project->tracks[0].params[midi_track::Channel] = 10;
        load_sound(project->tracks[1], EngineKind::Prophet, 1);
        project->tempo = 1200;
        put(bank->at[0][0], 0, 60, 100, 2);
        bank->at[0][1].length = 4 * project->bar_ticks(); // pattern 2: four bars, one long note
        put(bank->at[0][1], 0, 62, 100, 64);
        EngineHost host;
        host.load(*project, *bank);
        host.set_audio_format(44100, 384);
        std::vector<int32_t> out(256);
        auto drain = [&] { std::vector<MidiOutEvent> e; MidiOutEvent one; while (host.take_midi(one)) e.push_back(one); return e; };
        auto block = [&] { host.render(out.data(), 128, 1); return drain(); };
        host.send({Command::Type::MidiTrack, 1, 0, 5, 1}); // track 2 (a synth): Pad MIDI Out Always, channel 5
        host.send({Command::Type::MidiTrack, 0, 0, 3, 0}); // MIDI Config says Never for track 1: a MIDI track sends anyway
        CHECK(block().empty());
        host.send({Command::Type::NoteOn, 0, 0, 48, 90});
        host.send({Command::Type::NoteOn, 1, 0, 50, 90});
        auto pads = block();
        CHECK(pads.size() == 2);
        CHECK(pads[0].kind == K::NoteOn && pads[0].channel == 10 && pads[0].a == 48 && pads[0].dest == MidiDest::UsbC);
        CHECK(pads[1].kind == K::NoteOn && pads[1].channel == 5 && pads[1].dest == MidiDest::Config);
        CHECK(pads[0].track == 0 && pads[1].track == 255); // the DEVICE picture's light: a MIDI track's own sends only
        // A new channel ends the held note on the old one.
        host.send({Command::Type::Param, 0, 0, midi_track::Channel, 11});
        auto moved = block();
        CHECK(moved.size() == 1 && moved[0].kind == K::NoteOff && moved[0].channel == 10 && moved[0].a == 48 && moved[0].dest == MidiDest::UsbC && moved[0].track == 0);
        // The same value again sends nothing; a new port ends the notes too.
        host.send({Command::Type::NoteOn, 0, 0, 49, 90});
        host.send({Command::Type::Param, 0, 0, midi_track::Channel, 11});
        auto again = block();
        CHECK(again.size() == 1 && again[0].kind == K::NoteOn && again[0].channel == 11);
        host.send({Command::Type::Param, 0, 0, midi_track::Port, midi_track::External});
        auto ported = block();
        CHECK(ported.size() == 1 && ported[0].kind == K::NoteOff && ported[0].a == 49 && ported[0].dest == MidiDest::UsbC);
        host.send({Command::Type::NoteOn, 0, 0, 51, 90});
        CHECK(block()[0].dest == MidiDest::External);
        // Muted: its notes end, new ones stay home; the synth's MIDI is left alone.
        host.send({Command::Type::Mute, 0, 0, 0, 1});
        auto muted = block();
        CHECK(muted.size() == 1 && muted[0].kind == K::NoteOff && muted[0].a == 51 && muted[0].channel == 11);
        host.send({Command::Type::NoteOn, 0, 0, 52, 90});
        CHECK(block().empty());
        host.send({Command::Type::Mute, 0, 0, 0, 0});
        // Another track soloed silences it the same way.
        host.send({Command::Type::NoteOn, 0, 0, 53, 90});
        CHECK(block().size() == 1);
        host.send({Command::Type::Solo, 1, 0, 0, 1});
        auto soloed = block();
        CHECK(soloed.size() == 1 && soloed[0].kind == K::NoteOff && soloed[0].a == 53);
        host.send({Command::Type::Solo, 1, 0, 0, 0});
        host.send({Command::Type::NoteOff, 1, 0, 50});
        block();
        // Its pattern plays on its port and channel; the next pattern launched ends what the old one held.
        host.send({Command::Type::Param, 0, 0, midi_track::Port, midi_track::AllUsb});
        host.send({Command::Type::Play});
        std::vector<MidiOutEvent> all;
        for (unsigned n = 0; n < 120; ++n) for (auto& e : block()) all.push_back(e); // two sixteenths: 11025 frames
        bool on = false, off = false;
        for (auto& e : all) {
            if (e.kind == K::NoteOn && e.a == 60) on = e.channel == 11 && e.dest == MidiDest::AllUsb;
            if (e.kind == K::NoteOff && e.a == 60) off = true;
        }
        CHECK(on && off);
        host.send({Command::Type::Pattern, 0, 0, 0, 1}); // pattern 2 on the next bar
        all.clear();
        for (unsigned n = 0; n < 700; ++n) for (auto& e : block()) all.push_back(e); // past the bar (88200 frames at 120 BPM)
        int long_on = -1;
        for (unsigned n = 0; n < all.size(); ++n) if (all[n].kind == K::NoteOn && all[n].a == 62) long_on = int(n);
        CHECK(long_on >= 0);
        host.send({Command::Type::Pattern, 0, 0, 0, 0}); // back to pattern 1: the long note ends at the bar, before it is due
        all.clear();
        for (unsigned n = 0; n < 700; ++n) for (auto& e : block()) all.push_back(e);
        bool ended = false;
        for (auto& e : all) if (e.kind == K::NoteOff && e.a == 62) { ended = true; break; }
        CHECK(ended);
        // Stop ends what it holds; an engine change too.
        host.send({Command::Type::NoteOn, 0, 0, 54, 90});
        block();
        host.send({Command::Type::Stop});
        auto stopped = block();
        bool stop_off = false;
        for (auto& e : stopped) stop_off |= e.kind == K::NoteOff && e.a == 54 && e.dest == MidiDest::AllUsb;
        CHECK(stop_off);
        host.send({Command::Type::NoteOn, 0, 0, 55, 90});
        block();
        host.send({Command::Type::Engine, 0, 0, uint16_t(EngineKind::Prophet)});
        auto changed = block();
        CHECK(changed.size() == 1 && changed[0].kind == K::NoteOff && changed[0].a == 55);
    }
    // The arp plays a MIDI track as it plays a synth.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        load_sound(project->tracks[0], EngineKind::Midi, 0);
        EngineHost host;
        host.load(*project, *bank);
        host.set_audio_format(44100, 384);
        std::vector<int32_t> out(256);
        host.send({Command::Type::Arp, 0, 0, 0, 1});
        host.send({Command::Type::ArpKey, 0, 0, 60, 100});
        host.send({Command::Type::ArpKey, 0, 0, 64, 100});
        unsigned ons = 0, offs = 0;
        for (unsigned n = 0; n < 400; ++n) { // sixteenths at 120 BPM: about nine
            host.render(out.data(), 128, 1);
            MidiOutEvent e;
            while (host.take_midi(e)) { ons += e.kind == K::NoteOn && e.channel == 1 && e.dest == MidiDest::External; offs += e.kind == K::NoteOff; }
        }
        CHECK(ons >= 4 && offs >= 3);
    }
}

// Stage, context aware (shell/stage_map.h, the engines' stage tables, bindings.cpp).
void stage() {
    using K = Binding::Kind;
    // Every engine's table: three buttons of one to four pages, each named, its parameters the engine's own.
    for (unsigned k = 0; k < unsigned(EngineKind::Count); ++k) {
        const auto& g = stage_groups(EngineKind(k));
        const auto& info = engine_info(EngineKind(k));
        for (const auto& group : g.groups) {
            CHECK(group.count >= 1 && group.count <= 4);
            for (unsigned n = 0; n < group.count; ++n) {
                const auto& page = group.pages[n];
                CHECK(page.key && std::strlen(page.key) > 0);
                for (unsigned c = 0; c < 3; ++c) CHECK(page.params[c] < int(info.param_count) && page.shift[c] < int(info.param_count));
                if (page.picture == StagePicture::Envelope) CHECK(page.params[0] >= 0 && page.params[1] >= 0 && page.params[2] >= 0 && page.shift[0] >= 0);
                if (page.kind == StageKind::Voice) CHECK(g.voices && g.pad_voice);
            }
        }
    }
    CHECK(stage_groups(EngineKind::Prophet).groups[0].pages[0].kind == StageKind::Preset);
    CHECK(stage_groups(EngineKind::Sampler).groups[0].pages[0].kind == StageKind::Trim && stage_groups(EngineKind::Sampler).groups[1].count == 4);
    CHECK(stage_groups(EngineKind::None).groups[2].pages[0].kind == StageKind::NewMidi);

    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    load_sound(project->tracks[0], EngineKind::Prophet, 1);
    load_sound(project->tracks[1], EngineKind::Sampler, 0);
    load_sound(project->tracks[2], EngineKind::Kit909, 0);
    load_sound(project->tracks[3], EngineKind::Midi, 0);
    const Pattern& pattern = bank->at[0][0];
    UiState ui;
    ui.page = PageId::Stage;
    auto key = [&] { return std::string(stage_page_now(ui, project->tracks[ui.track]).key); };
    auto cards = [&](bool shift) { return page_bindings(*project, ui, pattern, shift); };

    // B1 on a synth: PRESET -> TONE -> MIX -> PRESET; B2 jumps to its own first page; each track keeps its own.
    CHECK(key() == "PRESET");
    stage_press(ui, project->tracks[0], 0); CHECK(key() == "TONE");
    stage_press(ui, project->tracks[0], 0); CHECK(key() == "MIX");
    stage_press(ui, project->tracks[0], 0); CHECK(key() == "PRESET");
    stage_press(ui, project->tracks[0], 1); CHECK(key() == "FILTER ENV");
    stage_press(ui, project->tracks[0], 1); CHECK(key() == "AMP ENV");
    ui.track = 1; CHECK(key() == "TRIM");
    stage_press(ui, project->tracks[1], 2); CHECK(key() == "FILTER");
    ui.track = 0; CHECK(key() == "AMP ENV");
    stage_press(ui, project->tracks[0], 0); CHECK(key() == "PRESET"); // another button: its first page

    // TONE: the favourites; AMP ENV: attack, decay, sustain, Shift + K1 release, Shift + K2 velocity; K3 has no Shift card.
    stage_go(ui, 0, 1);
    auto tone = cards(false);
    CHECK(tone[0].kind == K::Param && tone[0].id == prophet::Cutoff && tone[2].id == prophet::EnvAmount);
    stage_go(ui, 1, 1);
    auto amp = cards(false), amp_shift = cards(true);
    CHECK(amp[0].id == prophet::AmpAttack && amp[1].id == prophet::AmpDecay && amp[2].id == prophet::AmpSustain);
    CHECK(amp_shift[0].id == prophet::AmpRelease && amp_shift[1].id == prophet::VelAmp && amp_shift[2].id == prophet::AmpSustain);
    CHECK(shift_bindings(*project, ui, pattern)[0].bound() && !shift_bindings(*project, ui, pattern)[2].bound());
    // PERFORM: glide, unison, detune; the Shift row vintage, bend, voices.
    stage_go(ui, 2, 1);
    CHECK(cards(false)[0].id == prophet::Glide && cards(true)[2].id == prophet::Voices);
    // MIX: level, pan, send; Shift + K3 the bus.
    stage_go(ui, 0, 2);
    CHECK(cards(false)[0].kind == K::Level && cards(false)[1].kind == K::Pan && cards(false)[2].kind == K::Send && cards(true)[2].kind == K::SendBus);
    // ARP: mode, rate, octaves; Shift + K1 gate, Shift + K3 hold.
    stage_go(ui, 2, 0);
    CHECK(cards(false)[0].kind == K::ArpMode && cards(false)[1].kind == K::ArpRate && cards(true)[0].kind == K::ArpGate && cards(true)[2].kind == K::ArpHold);
    // FX: nothing without effects; with one, its first row and (Shift) its second.
    stage_go(ui, 2, 2);
    CHECK(!cards(false)[0].bound());
    project->tracks[0].fx.slots[0].kind = uint8_t(EffectKind::Echo);
    CHECK(cards(false)[0].kind == K::FxParam && cards(false)[0].id == 0 * 64 + 0 * 8 + 0);
    CHECK(!cards(true)[0].bound() || cards(true)[0].id == 3);
    // PRESET: K1 the loaded sound's place in the list, or the one turned to while it waits; K2 the list, K3 the engine.
    stage_go(ui, 0, 0);
    auto preset = cards(false);
    CHECK(preset[0].kind == K::StageSound && preset[0].value == 1 && preset[0].max == int(prophet::info().sound_count) - 1);
    CHECK(preset[1].kind == K::SoundList && preset[2].kind == K::Engine);
    ui.preset_due = 5; ui.preset_cursor = 4;
    CHECK(cards(false)[0].value == 4);
    ui.preset_due = 0;
    CHECK(stage_preset_ms == 250);

    // A kit: Akai's pages on the last pad hit. TRIM start, end, loop, zoom on Shift; MIX K2 the kit's volume;
    // AMP ENV Shift + K2 Decay From; TUNE Shift + K3 # Beats; PLAY Shift + K2 Pad Link.
    ui.track = 1;
    auto& kit = project->tracks[1];
    kit.pads[5].sample = 0; project->samples[0].frames = 44100; kit.last_pad = 5;
    stage_go(ui, 0, 0);
    CHECK(cards(false)[0].kind == K::PadStart && cards(false)[1].kind == K::PadEnd && cards(false)[2].kind == K::PadLoopPoint);
    CHECK(cards(true)[0].kind == K::TrimZoom);
    stage_go(ui, 0, 1);
    CHECK(cards(false)[0].kind == K::PadLevel && cards(false)[1].kind == K::Param && cards(false)[1].id == sampler::Level && cards(false)[2].kind == K::PadPan);
    stage_go(ui, 0, 2);
    CHECK(cards(false)[0].kind == K::PadAttack && cards(true)[1].kind == K::PadDecayFrom);
    stage_go(ui, 1, 0);
    CHECK(cards(false)[0].kind == K::PadPitch && cards(true)[2].kind == K::PadBeats);
    stage_go(ui, 1, 1);
    CHECK(cards(false)[0].kind == K::PadMono && cards(true)[1].kind == K::PadLink);
    stage_go(ui, 2, 0);
    CHECK(cards(false)[2].kind == K::PadFilterType);
    stage_go(ui, 2, 1);
    CHECK(cards(false)[2].kind == K::PadFAmount);
    // Melodee's own kit pages join Akai's groups: B2 TUNE -> PLAY -> FOLLOW -> CLOUD, B3 ... -> KIT FILT.
    stage_go(ui, 1, 1);
    stage_press(ui, kit, 1); CHECK(key() == "FOLLOW" && cards(false)[0].id == sampler::Follow && cards(false)[1].id == sampler::FollowHow);
    stage_press(ui, kit, 1); CHECK(key() == "CLOUD" && cards(false)[0].id == sampler::Position && cards(false)[2].id == sampler::Density);
    CHECK(cards(true)[0].id == sampler::Scan && cards(true)[2].id == sampler::Cloud);
    stage_press(ui, kit, 1); CHECK(key() == "TUNE");
    stage_go(ui, 2, 2);
    CHECK(key() == "KIT FILT" && cards(false)[0].id == sampler::Filter && cards(false)[2].id == sampler::FilterType);
    kit.last_pad = 6; // a pad without a sample: nothing to turn, but the kit's own values still are
    CHECK(cards(false)[0].bound() && cards(false)[0].id == sampler::Filter);
    stage_go(ui, 2, 1);
    CHECK(!cards(false)[0].bound());
    CHECK(stage_kit_page(StageKind::AmpEnv) == PageId::EnvelopePage && stage_kit_page(StageKind::Preset) == PageId::Count);

    // A 909: VOICE follows the last pad: a clap is the snare and clap page, its Shift row clap decay and rim.
    ui.track = 2;
    auto& drums = project->tracks[2];
    stage_go(ui, 0, 1);
    CHECK(key() == "KICK"); // before any pad
    drums.last_note = 36 + kit909::Clap;
    CHECK(key() == "SNARE" && cards(false)[2].id == kit909::SnareSnappy && cards(true)[0].id == kit909::ClapDecay);
    drums.last_note = 36 + kit909::OpenHat;
    CHECK(key() == "HATS");
    drums.last_note = 36 + kit909::Ride;
    CHECK(key() == "CYMBALS" && cards(true)[2].id == kit909::RideDecay);
    stage_go(ui, 0, 2);
    CHECK(key() == "LEVELS" && cards(false)[0].id == kit909::KickLevel && cards(true)[2].id == kit909::CymbalLevel);
    stage_press(ui, drums, 1); CHECK(key() == "MIX");
    stage_press(ui, drums, 1); CHECK(key() == "MIX"); // one page: it stays

    // MIDI: DEVICE, PROGRAM (bank and program, Off at first), CC (values; their numbers on Shift).
    ui.track = 3;
    auto& midi = project->tracks[3];
    stage_go(ui, 0, 1);
    CHECK(key() == "PROGRAM" && cards(false)[2].id == midi_track::Program && midi.params[midi_track::Program] == 0);
    CHECK(show(engine_info(EngineKind::Midi).params[midi_track::Program], 0).value == "Off" && show(engine_info(EngineKind::Midi).params[midi_track::BankMsb], 1).value == "0");
    stage_go(ui, 1, 0);
    CHECK(cards(false)[0].id == midi_track::Cc1 && cards(true)[0].id == midi_track::CcNumber1 && midi.params[midi_track::CcNumber1] == 74);
    CHECK(std::string(midi_track::cc_name(74)) == "Brightness" && !midi_track::cc_name(3));
    CHECK(binding_lane(midi, cards(false)[0]) == motion_lane(midi_track::Cc1)); // a CC records as motion

    // An empty track: SYNTH (engines, not a kit or MIDI), KIT (where its sample comes from), MIDI (port, channel).
    ui.track = 9;
    CHECK(key() == "SYNTH");
    std::array<EngineKind, size_t(EngineKind::Count)> engines {};
    unsigned count = new_engines(engines);
    CHECK(count >= 5);
    for (unsigned n = 0; n < count; ++n) CHECK(engines[n] != EngineKind::Sampler && engines[n] != EngineKind::Midi && engines[n] != EngineKind::None);
    CHECK(cards(false)[0].kind == K::NewEngine && cards(false)[1].kind == K::NewSound);
    stage_press(ui, project->tracks[9], 1); CHECK(key() == "KIT" && cards(false)[0].kind == K::NewSource && cards(false)[0].max == int(new_sources) - 1);
    stage_press(ui, project->tracks[9], 2); CHECK(key() == "MIDI" && cards(false)[0].kind == K::NewPort && cards(false)[1].kind == K::NewChannel);
    ui.last_pot = 2; // K3 is on no card here: the wheel still walks K1's list
    CHECK(stage_focus(ui, project->tracks[9]) == 0 && stage_focus(ui, project->tracks[0]) == 2);
    ui.last_pot = -1;

    // Tracks: K1 the track, K2 its level, K3 its pattern.
    ui.page = PageId::Tracks; ui.track = 0;
    CHECK(cards(false)[0].kind == K::TrackPick && cards(false)[1].kind == K::Level && cards(false)[2].kind == K::TrackPattern);
    CHECK(pad_bank_double_ms == 450);

    // Undo: Shift + B1 / B2 on a kit's TRIM (Loop Lock, Normalize) are the pad's; B1 alone on Stage keeps nothing.
    ui.page = PageId::Stage; ui.track = 1; stage_go(ui, 0, 0); kit.last_pad = 5;
    ui.shift = true;
    CHECK(press_scope(*project, ui, SurfaceButton::TopLeft, false).kind == UndoScope::Kind::Pad);
    CHECK(button_has_shift_job(*project, ui, SurfaceButton::TopMiddle, true));
    ui.shift = false;
    CHECK(press_scope(*project, ui, SurfaceButton::TopLeft, false).none());

    // The file: bank, program and the CCs stay; a MIDT of version 1 (before Stage) gets their initial values.
    midi.params[midi_track::Program] = 12; midi.params[midi_track::BankMsb] = 3; midi.params[midi_track::Cc2] = 20; midi.params[midi_track::CcNumber3] = 7;
    auto bytes = save_project(*project, *bank);
    auto back = std::make_unique<Project>();
    CHECK(load_project(bytes, *back, *bank).empty());
    CHECK(back->tracks[3].params[midi_track::Program] == 12 && back->tracks[3].params[midi_track::BankMsb] == 3 &&
          back->tracks[3].params[midi_track::Cc2] == 20 && back->tracks[3].params[midi_track::CcNumber3] == 7);
    auto tag = std::search(bytes.begin(), bytes.end(), "MIDT", "MIDT" + 4);
    CHECK(tag != bytes.end() && tag[8] == 3 && tag[9] == 2);
    tag[9] = 1; // as a file from before Stage: its zeros are no CC 0s
    auto older = std::make_unique<Project>();
    CHECK(load_project(bytes, *older, *bank).empty());
    CHECK(older->tracks[3].params[midi_track::CcNumber1] == 74 && older->tracks[3].params[midi_track::Program] == 0 &&
          older->tracks[3].params[midi_track::Port] == midi.params[midi_track::Port]);

    // The engine host: a CC turned goes out on the track's channel and port; a program with its bank selects, and again as the track loads.
    {
        using E = MidiOutEvent::Kind;
        auto p2 = std::make_unique<Project>();
        load_sound(p2->tracks[0], EngineKind::Midi, 0);
        p2->tracks[0].params[midi_track::Port] = midi_track::UsbC; p2->tracks[0].params[midi_track::Channel] = 3;
        p2->tracks[0].params[midi_track::Program] = 6; p2->tracks[0].params[midi_track::BankLsb] = 2;
        EngineHost host;
        host.load(*p2, *bank);
        host.set_audio_format(44100, 384);
        std::vector<int32_t> out(256);
        auto block = [&] { host.render(out.data(), 128, 1); std::vector<MidiOutEvent> e; MidiOutEvent one; while (host.take_midi(one)) e.push_back(one); return e; };
        auto loaded = block();
        CHECK(loaded.size() == 2 && loaded[0].kind == E::Control && loaded[0].a == 32 && loaded[0].b == 1 && loaded[0].channel == 3 &&
              loaded[1].kind == E::Program && loaded[1].a == 5 && loaded[1].dest == MidiDest::UsbC && loaded[1].track == 0);
        host.send({Command::Type::Param, 0, 0, midi_track::Cc1, 99});
        auto cc = block();
        CHECK(cc.size() == 1 && cc[0].kind == E::Control && cc[0].a == 74 && cc[0].b == 99 && cc[0].channel == 3);
        host.send({Command::Type::Param, 0, 0, midi_track::CcNumber1, 10}); // a number alone sends nothing
        CHECK(block().empty());
        host.send({Command::Type::Param, 0, 0, midi_track::Program, 0}); // Off: nothing
        CHECK(block().empty());
        host.send({Command::Type::Param, 0, 0, midi_track::Program, 9});
        auto program = block();
        CHECK(program.size() == 2 && program.back().kind == E::Program && program.back().a == 8);
    }
}

// Roll R1 (shell/roll.h): the cursor, the note in focus and the view, on the pattern's events.
void roll() {
    Pattern p;
    p.length = 4 * 4 * ticks_per_quarter; // four bars of 4/4 at 1/16: 64 steps
    auto add = [&](unsigned step, uint8_t key) { Event e; e.tick = step * sixteenth_ticks; e.key = key; CHECK(p.add(e)); };
    add(0, 62); add(6, 60); add(6, 64); add(8, 57); add(40, 69);
    UiState ui;
    ui.step_cursor = 6; ui.roll_key = 60;
    CHECK(roll_focus(p, ui) >= 0 && p.events[size_t(roll_focus(p, ui))].key == 60);
    roll_pitch(p, ui, 1); CHECK(ui.roll_key == 64);                           // Minus / Plus: the chord in the step first
    roll_pitch(p, ui, 1); CHECK(ui.roll_key == 65 && roll_focus(p, ui) < 0);  // then a semitone
    roll_pitch(p, ui, -1); CHECK(ui.roll_key == 64);                          // and back onto the chord's top
    roll_step(p, ui, 2); CHECK(ui.step_cursor == 8 && ui.roll_key == 57);     // the wheel: onto the nearest note starting there
    roll_step(p, ui, 1); CHECK(ui.step_cursor == 9 && ui.roll_key == 57 && roll_focus(p, ui) < 0); // an empty step keeps the pitch
    CHECK(roll_jump(p, ui, 1) && ui.step_cursor == 40 && ui.roll_key == 69);  // Shift + wheel: note to note over empty bars
    CHECK(!roll_jump(p, ui, 1) && ui.step_cursor == 40);
    CHECK(roll_jump(p, ui, -1) && ui.step_cursor == 8 && ui.roll_key == 57);
    // and through a chord one note at a time, low to high, both ways.
    ui.step_cursor = 6; ui.roll_key = 60;
    CHECK(roll_jump(p, ui, 1) && ui.step_cursor == 6 && ui.roll_key == 64 && roll_focus(p, ui) >= 0);
    CHECK(roll_jump(p, ui, 1) && ui.step_cursor == 8 && ui.roll_key == 57);
    CHECK(roll_jump(p, ui, -1) && ui.step_cursor == 6 && ui.roll_key == 64);
    CHECK(roll_jump(p, ui, -1) && ui.step_cursor == 6 && ui.roll_key == 60);
    CHECK(roll_jump(p, ui, -1) && ui.step_cursor == 0 && ui.roll_key == 62);
    CHECK(!roll_jump(p, ui, -1));
    // The view scrolls a step at a time, the cursor two steps from either edge, inside the pattern.
    ui.step_cursor = 40; ui.roll_view = 0; roll_follow(p, ui); CHECK(ui.roll_view == 27 && roll_first(p, ui) == 27);
    ui.step_cursor = 63; roll_follow(p, ui); CHECK(ui.roll_view == 48);
    ui.step_cursor = 30; roll_follow(p, ui); CHECK(ui.roll_view == 28);
    ui.step_cursor = 0; roll_follow(p, ui); CHECK(ui.roll_view == 0);
    ui.roll_key = 80; ui.roll_top = 65; roll_follow(p, ui); CHECK(ui.roll_top == 81); // a row above the cursor's key
    ui.roll_key = 40; roll_follow(p, ui); CHECK(ui.roll_top == 50);                   // and one below
    // Step Edit's selection follows the focus, so its cards, fader and erase act on the note in focus.
    ui.step_cursor = 6; ui.roll_key = 64; roll_select(p, ui);
    CHECK(selected_event(p, ui) == roll_focus(p, ui) && p.events[size_t(selected_event(p, ui))].key == 64);
    CHECK(roll_position(4 * 96 + 96 + 48, 4 * 96, 96, 24) == "2.2.3" && roll_position(0, 384, 96, 96) == "1.1");
    // Roll's cards are the note in focus; none there, no cards.
    Project project;
    load_sound(project.tracks[0], EngineKind::Prophet, 1);
    ui.page = PageId::Roll; ui.track = 0;
    auto cards = page_bindings(project, ui, p);
    CHECK(cards[0].kind == Binding::Kind::EventLength && cards[1].kind == Binding::Kind::EventVelocity && cards[2].kind == Binding::Kind::EventChance);
    CHECK(cards[1].value == 100 && cards[2].value == 100 && cards[0].value == 1);
    ui.roll_key = 63; CHECK(!page_bindings(project, ui, p)[0].bound());
    Binding chance; chance.kind = Binding::Kind::EventChance;
    CHECK(binding_scope(project, ui, chance).kind == UndoScope::Kind::Pattern);
}

// The Looper (docs/LOOPER.md): takes, quantize, overdub, feedback, undo, the transport, sources, the stretch,
// fitting, holding for a save, and its four tracks in the engine host's mix.
void looper_engine() {
    constexpr double bpm = 240, per = 96.0 * bpm / 60.0 / 44100.0; // ticks a frame; a bar of 4/4 is one second
    constexpr unsigned bar_frames = 44100;
    auto signal = [](uint64_t f) { return float(0.3 * std::sin(double(f) * 0.0137) + 0.1 * std::sin(double(f) * 0.29)); };
    auto lp = std::make_unique<Looper>();
    Looper& looper = *lp;
    float out_l[loop_tracks][looper::block], out_r[loop_tracks][looper::block];
    float src_l[looper::block] {}, src_r[looper::block] {}, mix_l[looper::block] {}, mix_r[looper::block] {};
    uint64_t frame = 0;     // frames run so far
    double tick = 0;
    bool running = true, restart = true;
    std::vector<float> heard; // loop 1's output, every frame
    float constant = -1;      // >= 0: the source plays this instead of the signal
    // One block: the source played the block before (as the host keeps it), then the looper.
    auto block = [&](unsigned frames = looper::block) {
        Looper::Clock c;
        c.tick = tick; c.per_frame = per; c.bar = 384; c.beat = 96;
        c.running = running; c.restart = restart; c.rate = 44100;
        restart = false;
        Looper::Sources s;
        s.frames = frame ? frames : 0;
        s.at[looper::source_input] = {src_l, src_r};
        s.at[looper::source_mix] = {mix_l, mix_r};
        looper.run(c, s, frames, out_l, out_r);
        for (unsigned f = 0; f < frames; ++f) {
            heard.push_back(out_l[0][f]);
            float x = constant >= 0 ? constant : signal(frame + f);
            src_l[f] = x; src_r[f] = -x;
            mix_l[f] = out_l[0][f] + 0.5f; mix_r[f] = out_r[0][f] + 0.5f; // what the "mix" played: the loop and a level
        }
        frame += frames;
        if (running) tick += frames * per;
        looper.service();
    };
    auto run_frames = [&](uint64_t frames) { for (uint64_t n = 0; n < frames / looper::block; ++n) block(); };

    // Off: no memory, nothing recorded.
    looper.ask(looper::Do::Primary, 0, 0);
    block();
    CHECK(looper.view(0).state == Looper::State::Empty && !looper.view(0).ready);
    // On: the service thread makes two buffers a loop.
    looper.ask(looper::Do::Enable, 0, 1);
    for (unsigned n = 0; n < 20; ++n) block();
    CHECK(looper.enabled_now() && looper.buffers_out() == 2 * loop_tracks);
    for (unsigned n = 0; n < loop_tracks; ++n) CHECK(looper.view(n).ready);

    // A take of one bar on the bar: Armed, then Recording on bar 2's downbeat, closed one bar later, then playing.
    looper.ask(looper::Do::Length, 0, 1);
    while (tick < 384 - 2 * looper::block * per) block();
    looper.ask(looper::Do::Primary, 0, 0);
    block();
    CHECK(looper.view(0).state == Looper::State::Armed && looper.view(0).pending == Looper::Pending::Start);
    while (tick < 384 + 10 * looper::block * per) block();
    CHECK(looper.view(0).state == Looper::State::Recording && looper.armed() == false && looper.recording());
    uint64_t take_start = uint64_t(std::llround(384 / per)); // the frame of bar 2
    while (tick < 768 + 4 * looper::block * per) block();
    auto v = looper.view(0);
    CHECK(v.state == Looper::State::Playing && v.length == bar_frames && std::fabs(v.ticks - 384) < 1e-6 && v.tempo == 2400 && v.layers == 1);
    // It plays what was played in its bar, on the beat: frame F of bar 3 is the frame a bar before.
    while (tick < 1152 - 4 * looper::block * per) block();
    for (uint64_t f = take_start + bar_frames + 2000; f < take_start + 2 * bar_frames - 2000; f += 997)
        CHECK(std::fabs(heard[f] - signal(f - bar_frames)) < 2e-3f);
    // The waveform was drawn again from the take.
    unsigned lit = 0;
    for (unsigned c = 0; c < looper::peak_columns; ++c) lit += looper.peak(0, c) > 20;
    CHECK(lit > looper::peak_columns * 3 / 4);

    // Overdub a constant for a bar (feedback 100 %): old layers + 0.25; undo takes it back, redo brings it.
    constant = 0.25f;
    block();
    looper.ask(looper::Do::Primary, 0, 0);
    block(); block(); // the writer starts a block (the source's lag) after the press
    CHECK(looper.view(0).state == Looper::State::Overdub && looper.view(0).layers == 2);
    run_frames(bar_frames + 4 * looper::block);
    looper.ask(looper::Do::Primary, 0, 0);
    run_frames(8 * looper::block);
    v = looper.view(0);
    CHECK(v.state == Looper::State::Playing && v.undo == looper::Undo::Undo && v.layers == 2);
    uint64_t at = frame + 1000;
    run_frames(2000);
    CHECK(std::fabs(heard[at] - (signal(at - 3 * bar_frames) + 0.25f)) < 0.01f);
    looper.ask(looper::Do::Undo, 0, 0);
    run_frames(30 * looper::block); // swapped a chunk a block
    v = looper.view(0);
    CHECK(v.undo == looper::Undo::Redo && v.layers == 1);
    at = frame + 1000;
    run_frames(2000);
    CHECK(std::fabs(heard[at] - signal(at - (at - take_start) / bar_frames * bar_frames)) < 0.01f);
    looper.ask(looper::Do::Undo, 0, 0);
    run_frames(30 * looper::block);
    CHECK(looper.view(0).undo == looper::Undo::Undo && looper.view(0).layers == 2);
    // Feedback 0: an overdub pass replaces what it covers.
    looper.ask(looper::Do::Feedback, 0, 0);
    looper.ask(looper::Do::Primary, 0, 0);
    run_frames(bar_frames + 8 * looper::block);
    looper.ask(looper::Do::Primary, 0, 0);
    run_frames(8 * looper::block);
    at = frame + 500;
    run_frames(1000);
    CHECK(std::fabs(heard[at] - 0.25f) < 0.01f);
    looper.ask(looper::Do::Feedback, 0, 100);

    // Stop at the next bar (and cancel), then at once; Now plays it again.
    looper.ask(looper::Do::Toggle, 0, 0);
    CHECK(looper.view(0).pending == Looper::Pending::None); // not until the next block's view
    block();
    CHECK(looper.view(0).pending == Looper::Pending::Stop);
    looper.ask(looper::Do::Toggle, 0, 0);
    block();
    CHECK(looper.view(0).pending == Looper::Pending::None && looper.view(0).state == Looper::State::Playing);
    looper.ask(looper::Do::Halt, 0, 0);
    run_frames(2 * looper::block);
    CHECK(looper.view(0).state == Looper::State::Stopped && !looper.view(0).sounding);
    looper.ask(looper::Do::Now, 0, 0);
    run_frames(2 * looper::block);
    CHECK(looper.view(0).state == Looper::State::Playing && looper.view(0).sounding);

    // The transport stops: loops hold silent; a press on an empty loop arms; Play lines up and starts the take on bar 1.
    running = false;
    run_frames(4 * looper::block);
    CHECK(!looper.view(0).sounding && looper.view(0).state == Looper::State::Playing);
    looper.ask(looper::Do::Length, 1, 0);
    looper.ask(looper::Do::Source, 1, looper::source_mix);
    looper.ask(looper::Do::Primary, 1, 0);
    run_frames(4 * looper::block);
    CHECK(looper.view(1).state == Looper::State::Armed);
    running = true; restart = true; tick = 0;
    block();
    CHECK(looper.view(1).state == Looper::State::Recording && looper.view(0).sounding);
    CHECK(looper.view(0).position < 2 * looper::block); // loop 1 from its start
    // A free take closes on the next bar after the press.
    run_frames(bar_frames / 2);
    looper.ask(looper::Do::Primary, 1, 0);
    block();
    CHECK(looper.view(1).pending == Looper::Pending::Close);
    run_frames(bar_frames);
    v = looper.view(1);
    CHECK(v.state == Looper::State::Playing && v.length == bar_frames && std::fabs(v.ticks - 384) < 1e-6);
    // The source is fixed while it records; never itself.
    looper.ask(looper::Do::Source, 2, looper::source_loop(2));
    block();
    CHECK(looper.view(2).source == looper::source_input && looper.view(2).errors == 1);
    // Takes close on whole bars from their start. Free, pressed: the nearest bar (one just passed drops what ran past
    // it, one ahead waits for its end); a set length plays out to its last bar whatever is pressed; Stop now keeps
    // the whole bars it has.
    {
        auto take = [&](unsigned bars) {
            looper.ask(looper::Do::Length, 2, int(bars));
            looper.ask(looper::Do::Primary, 2, 0);
            block();
            while (looper.view(2).state != Looper::State::Recording) block();
        };
        auto clear = [&] {
            looper.ask(looper::Do::Clear, 2, 0);
            run_frames(16 * looper::block);
            CHECK(looper.view(2).state == Looper::State::Empty);
        };
        take(0);
        run_frames(bar_frames * 5 / 4);
        looper.ask(looper::Do::Primary, 2, 0);
        block();
        v = looper.view(2);
        CHECK(v.state == Looper::State::Playing && v.length == bar_frames && std::fabs(v.ticks - 384) < 1e-6);
        clear();
        take(0);
        run_frames(bar_frames * 17 / 10);
        looper.ask(looper::Do::Primary, 2, 0);
        block();
        CHECK(looper.view(2).state == Looper::State::Recording && looper.view(2).pending == Looper::Pending::Close);
        run_frames(bar_frames / 2);
        v = looper.view(2);
        CHECK(v.state == Looper::State::Playing && v.length == 2 * bar_frames && std::fabs(v.ticks - 768) < 1e-6);
        clear();
        take(0);
        run_frames(bar_frames / 4);
        looper.ask(looper::Do::Primary, 2, 0); // under half a bar: still the first bar
        run_frames(bar_frames);
        v = looper.view(2);
        CHECK(v.state == Looper::State::Playing && v.length == bar_frames);
        clear();
        take(2);
        run_frames(bar_frames / 2);
        looper.ask(looper::Do::Primary, 2, 0);
        block();
        CHECK(looper.view(2).state == Looper::State::Recording);
        run_frames(bar_frames * 3 / 2);
        v = looper.view(2);
        CHECK(v.state == Looper::State::Playing && v.length == 2 * bar_frames && std::fabs(v.ticks - 768) < 1e-6);
        // Armed with that two-bar loop playing (the longest), a take waits for its next start, not the next bar.
        looper.ask(looper::Do::Primary, 3, 0);
        run_frames(bar_frames * 3 / 2);
        CHECK(looper.view(3).state == Looper::State::Armed && looper.view(3).pending == Looper::Pending::Start);
        run_frames(bar_frames * 3 / 5);
        CHECK(looper.view(3).state == Looper::State::Recording);
        looper.ask(looper::Do::Clear, 3, 0);
        run_frames(16 * looper::block);
        CHECK(looper.view(3).state == Looper::State::Empty);
        clear();
        take(0);
        run_frames(bar_frames * 7 / 5);
        looper.ask(looper::Do::Halt, 2, 0);
        block();
        v = looper.view(2);
        CHECK(v.state == Looper::State::Stopped && v.length == bar_frames && std::fabs(v.ticks - 384) < 1e-6);
        clear();
        looper.ask(looper::Do::Length, 2, 0);
    }

    // Another tempo: loop 1 stays on the beat and keeps its pitch (stretched), then Push fits it and overdubs.
    constexpr double faster = 96.0 * 300 / 60.0 / 44100.0;
    auto block_at = [&](double p) {
        Looper::Clock c; c.tick = tick; c.per_frame = p; c.running = true; c.rate = 44100;
        Looper::Sources s; s.frames = looper::block; s.at[looper::source_input] = {src_l, src_r};
        looper.run(c, s, looper::block, out_l, out_r);
        tick += looper::block * p;
        looper.service();
    };
    for (unsigned n = 0; n < 200; ++n) block_at(faster);
    CHECK(looper.view(0).stretched);
    float energy = 0;
    for (unsigned n = 0; n < 100; ++n) { block_at(faster); for (unsigned f = 0; f < looper::block; ++f) energy += out_l[0][f] * out_l[0][f]; }
    CHECK(energy > 1.0f);
    looper.ask(looper::Do::Primary, 0, 0);
    block_at(faster);       // the service thread fits it
    block_at(faster);       // and the audio thread swaps it in, then punches in
    v = looper.view(0);
    CHECK(!v.fitting && !v.stretched && v.length == 35280 && v.tempo == 3000 && v.state == Looper::State::Overdub && v.undo == looper::Undo::None);
    looper.ask(looper::Do::Primary, 0, 0);
    for (unsigned n = 0; n < 10; ++n) block_at(faster);
    // A set length too long for a loop's memory at this tempo is refused.
    looper.ask(looper::Do::Clear, 2, 0);
    looper.ask(looper::Do::Length, 2, 16);
    Looper::Clock slow; slow.tick = tick; slow.per_frame = 96.0 * 40 / 60.0 / 44100.0; slow.running = true; slow.rate = 44100;
    Looper::Sources none;
    looper.run(slow, none, looper::block, out_l, out_r);
    looper.ask(looper::Do::Primary, 2, 0);
    looper.run(slow, none, looper::block, out_l, out_r);
    CHECK(looper.view(2).state == Looper::State::Empty && looper.view(2).error == looper::Error::TooLong);
    looper.ask(looper::Do::Length, 2, 0);

    // Held for a save: granted once nothing writes it; a clear waits until it is let go.
    CHECK(!looper.hold(1));
    block_at(faster);
    CHECK(looper.hold(1) && looper.held_left(1) && looper.view(1).held);
    looper.ask(looper::Do::Clear, 1, 0);
    for (unsigned n = 0; n < 4; ++n) block_at(faster);
    CHECK(looper.view(1).state == Looper::State::Playing);
    looper.release(1);
    for (unsigned n = 0; n < 8; ++n) block_at(faster);
    CHECK(looper.view(1).state == Looper::State::Empty && !looper.view(1).held);

    // A saved loop comes back through the service thread, playing.
    auto* buffer = new LoopBuffer;
    for (unsigned f = 0; f < 1000; ++f) buffer->left[f] = buffer->right[f] = 0.5f;
    LoopLoad item; item.buffer = buffer; item.loop = 3; item.frames = 1000; item.ticks = 96; item.tempo = 2400;
    CHECK(looper.give_load(item));
    for (unsigned n = 0; n < 4; ++n) block_at(per);
    v = looper.view(3);
    CHECK(v.state == Looper::State::Playing && v.length == 1000 && v.tempo == 2400 && looper.buffers_out() == 2 * loop_tracks);
    item.buffer = new LoopBuffer; item.stopped = true; // saved stopped: back stopped
    CHECK(looper.give_load(item));
    for (unsigned n = 0; n < 4; ++n) block_at(per);
    CHECK(looper.view(3).state == Looper::State::Stopped && looper.buffers_out() == 2 * loop_tracks);

    // Off: every loop goes, the memory goes back and is freed.
    looper.ask(looper::Do::Enable, 0, 0);
    for (unsigned n = 0; n < 4; ++n) block_at(per);
    CHECK(!looper.enabled_now() && looper.buffers_out() == 0);
    for (unsigned n = 0; n < loop_tracks; ++n) CHECK(looper.view(n).state == Looper::State::Empty);

    // In the engine host: L1 records track 1 (a PROPHET note), plays into its own track at its level, muted by its strip.
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    for (auto& t : project->tracks) t.engine = EngineKind::None;
    load_sound(project->tracks[0], EngineKind::Prophet, 1);
    project->looper.on = true;
    project->looper.loops[0].source = looper::source_track(0);
    project->looper.loops[0].bars = 1;
    project->tempo = 2400;
    auto host = std::make_unique<EngineHost>();
    host->load(*project, *bank);
    std::vector<int32_t> out(256);
    auto render = [&](unsigned blocks) { for (unsigned n = 0; n < blocks; ++n) { host->render(out.data(), 128, 1); host->looper().service(); } };
    render(20);
    CHECK(host->looper().view(0).ready);
    host->send({Command::Type::Play, 0, 0, 0, 0});
    host->send({Command::Type::Looper, 0, 0, uint16_t(looper::Do::Primary)});
    host->send({Command::Type::NoteOn, 0, 0, 60, 100});
    render(4);
    CHECK(host->looper().view(0).state == Looper::State::Recording);
    render(bar_frames / 128 + 4);
    host->send({Command::Type::NoteOff, 0, 0, 60});
    host->send({Command::Type::Level, 0, 0, 0, 0}); // track 1 silent: what is heard now is the loop
    render(bar_frames / 128 / 2);
    CHECK(host->looper().view(0).state == Looper::State::Playing);
    float loud = 0;
    for (unsigned n = 0; n < 50; ++n) { render(1); for (auto x : out) loud = std::max(loud, std::fabs(x / 2147483648.0f)); }
    host->send({Command::Type::Mute, uint8_t(track_count), 0, 0, 1});
    render(4);
    loud = 0;
    for (unsigned n = 0; n < 50; ++n) { render(1); for (auto x : out) loud = std::max(loud, std::fabs(x / 2147483648.0f)); }
    CHECK(loud < 1e-4f);
    // Loops on hardware inputs of their own, at once: L2 the rear right input, L3 USB-C's left. Monitor On hears
    // them through their tracks; then both takes play back with Monitor Off.
    {
        std::vector<int32_t> adc(256);
        std::vector<float> usb(256);
        for (unsigned f = 0; f < 128; ++f) { adc[2 * f] = int32_t(0.25 * 2147483648.0); adc[2 * f + 1] = int32_t(0.5 * 2147483648.0); usb[2 * f] = 0.125f; usb[2 * f + 1] = 0.75f; }
        auto heard = [&](unsigned blocks) {
            float most = 0;
            for (unsigned n = 0; n < blocks; ++n) {
                host->render(out.data(), 128, 1, adc.data(), nullptr, usb.data());
                host->looper().service();
                if (n + 8 >= blocks) for (auto x : out) most = std::max(most, std::fabs(x / 2147483648.0f));
            }
            return most;
        };
        CHECK(looper::source_of(InputSource::RearR) == looper::source_direct(2) && looper::direct_input(looper::source_of(InputSource::UsbL)) == InputSource::UsbL);
        host->send({Command::Type::Looper, 1, 0, uint16_t(looper::Do::Source), int16_t(looper::source_of(InputSource::RearR))});
        host->send({Command::Type::Looper, 2, 0, uint16_t(looper::Do::Source), int16_t(looper::source_of(InputSource::UsbL))});
        host->send({Command::Type::Looper, 1, 0, uint16_t(looper::Do::Length), 1});
        host->send({Command::Type::Looper, 2, 0, uint16_t(looper::Do::Length), 1});
        host->send({Command::Type::LoopMonitor, 0, 0, 0, int16_t(Monitor::On)});
        loud = heard(16);
        CHECK(loud > 0.55f && loud < 0.7f); // 0.5 and 0.125, each centred
        host->send({Command::Type::LoopMonitor, 0, 0, 0, int16_t(Monitor::Off)});
        CHECK(heard(16) < 1e-4f);
        host->send({Command::Type::LoopMonitor, 0, 0, 0, int16_t(Monitor::Auto)});
        host->send({Command::Type::Looper, 1, 0, uint16_t(looper::Do::Primary)});
        host->send({Command::Type::Looper, 2, 0, uint16_t(looper::Do::Primary)});
        heard(1);
        CHECK(host->looper().view(1).state == Looper::State::Armed && host->looper().view(2).state == Looper::State::Armed);
        loud = heard(8);
        CHECK(loud > 0.55f); // Auto: heard while armed
        heard(3 * bar_frames / 128);
        auto two = host->looper().view(1), three = host->looper().view(2);
        CHECK(two.state == Looper::State::Playing && two.length == bar_frames && three.state == Looper::State::Playing && three.length == bar_frames);
        host->send({Command::Type::LoopMonitor, 0, 0, 0, int16_t(Monitor::Off)});
        std::fill(adc.begin(), adc.end(), 0); std::fill(usb.begin(), usb.end(), 0.0f);
        loud = heard(bar_frames / 128 / 2);
        CHECK(loud > 0.55f && loud < 0.7f); // what each took, played back
    }
    // The project keeps the Looper: its settings, each loop's values and saved audio's length, L1-L4's strips and chains.
    {
        Project saved;
        PatternBank no_patterns;
        saved.looper.on = true; saved.looper.quantize = 1; saved.looper.then = 2; saved.looper.transport = 1;
        saved.looper.loops[2] = LoopTrack {4, 70, looper::source_mix, 176400, 1536.25, 1200, 3, true};
        saved.looper.loops[1].source = looper::source_loop(1); // itself: never (read back as the input)
        saved.loop_mix[3].level = 90; saved.loop_mix[3].pan = -30; saved.loop_mix[3].send = 40; saved.loop_mix[3].bus = 2; saved.loop_mix[3].mute = true;
        saved.loop_fx[3].slots[0].kind = uint8_t(EffectKind::Echo);
        Project back;
        PatternBank back_patterns;
        CHECK(load_project(save_project(saved, no_patterns), back, back_patterns).empty());
        const auto& l = back.looper.loops[2];
        CHECK(back.looper.on && back.looper.quantize == 1 && back.looper.then == 2 && back.looper.transport == 1);
        CHECK(l.bars == 4 && l.feedback == 70 && l.source == looper::source_mix && l.frames == 176400 && std::fabs(l.ticks - 1536.25) < 1e-4 && l.tempo == 1200 && l.layers == 3 && l.stopped);
        CHECK(back.looper.loops[1].source == looper::source_input);
        const auto& m = back.loop_mix[3];
        CHECK(m.level == 90 && m.pan == -30 && m.send == 40 && m.bus == 2 && m.mute && back.loop_fx[3].count() == 1);
        CHECK(back.chain(loop_chain(3)).slots[0].kind == uint8_t(EffectKind::Echo) && is_loop_chain(loop_chain(0)) && !is_loop_chain(master_chain));
        Project plain_project;
        CHECK(load_project(save_project(plain_project, no_patterns), back, back_patterns).empty() && !back.looper.on);
    }
    // Stop: the loop holds silent.
    host->send({Command::Type::Mute, uint8_t(track_count), 0, 0, 0});
    host->send({Command::Type::Stop});
    render(8);
    CHECK(!host->looper().view(0).sounding);
}

// Chop's Zero Snap and the slice edges' fades: a cut lands on a zero
// crossing (summed sides, mostly looking back so a hit is kept whole), and a
// region cut inside its file fades in and out over 2 ms.
void zero_snap() {
    SampleData sine;
    sine.frames = 20000;
    sine.left.resize(sine.frames); sine.right.resize(sine.frames);
    for (uint32_t n = 0; n < sine.frames; ++n) sine.left[n] = sine.right[n] = 0.8f * float(std::sin(6.283185307179586 * 100.0 * n / 44100.0));
    // 100 Hz crosses every 220.5 frames: 1000 lies 118 after 882 and 102 before 1102.5, which counts double.
    uint32_t at = zero_crossing(sine, 1000);
    CHECK(std::abs(int(at) - 882) <= 1);
    CHECK(std::fabs(sine.left[at]) < 0.02f);
    CHECK(zero_crossing(sine, at) == at);                       // a cut on a crossing stays
    CHECK(std::abs(int(zero_crossing(sine, 1095)) - 1102) <= 1); // a crossing just after wins when much nearer
    CHECK(zero_crossing(sine, 0) == 0 && zero_crossing(sine, 30000) == 30000); // the start, and past the end
    // The sides summed: one side the other upside down is silence, so the cut stays.
    SampleData opposite = sine;
    for (auto& v : opposite.right) v = -v;
    for (uint32_t n = 0; n < opposite.frames; ++n) opposite.left[n] = -opposite.right[n];
    CHECK(zero_crossing(opposite, 1000) == 1000);
    // Nothing crosses (an offset): the frame nearest zero in the window.
    SampleData offset = sine;
    for (uint32_t n = 0; n < offset.frames; ++n) offset.left[n] = offset.right[n] = 0.5f + 0.1f * float(std::sin(6.283185307179586 * 100.0 * n / 44100.0));
    uint32_t low = zero_crossing(offset, 1000);
    float lowest = 1;
    for (uint32_t n = 1000 - zero_back; n <= 1000 + zero_ahead; ++n) lowest = std::min(lowest, offset.left[n]);
    CHECK(low != 1000 && offset.left[low] == lowest);
    // Silence, then a hit at 5000: a cut on the hit stays on it, one a little into it goes back to it.
    SampleData hit;
    hit.frames = 10000;
    hit.left.assign(hit.frames, 0.0f);
    for (uint32_t n = 5000; n < hit.frames; ++n) hit.left[n] = 0.9f * float(std::sin(6.283185307179586 * 60.0 * (n - 5000) / 44100.0) + 0.3);
    hit.right = hit.left;
    CHECK(zero_crossing(hit, 5000) <= 5000 && zero_crossing(hit, 5000) + zero_back >= 5000);
    CHECK(zero_crossing(hit, 5030) <= 5000);

    // The setting: on unless turned off, kept with the settings.
    Settings fresh;
    CHECK(fresh.zero_snap);
    Settings off; off.zero_snap = false;
    Settings back; back.read(off.text());
    CHECK(!back.zero_snap);
    back.read(fresh.text());
    CHECK(back.zero_snap);

    // The engine: a level sample, so the fades are all there is to hear.
    SampleData level;
    level.frames = 10000;
    level.left.assign(level.frames, 0.5f); level.right = level.left;
    SamplerEngine::Table table {};
    table[2] = &level;
    auto play = [&](uint32_t start, uint32_t end) {
        SamplerEngine engine;
        engine.set_samples(&table);
        PadSound pad; pad.sample = 2; pad.start = start; pad.end = end; pad.play = PadPlay::OneShot; pad.decay = 100;
        engine.set_pad(0, pad);
        engine.note_on(int(sampler_first_note), 127);
        std::vector<float> out;
        std::vector<float> l(128), r(128);
        for (unsigned b = 0; b < 50; ++b) {
            std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
            engine.render_stereo(l.data(), r.data(), 128);
            out.insert(out.end(), l.begin(), l.end());
        }
        return out;
    };
    auto slice = play(1000, 5000); // cut inside the file at both ends
    float steady = slice[2000];
    CHECK(steady > 0.1f);
    CHECK(slice[0] < 0.05f * steady);                                   // fades in
    CHECK(slice[44] > 0.3f * steady && slice[44] < 0.7f * steady);      // half way at 1 ms
    CHECK(slice[100] > 0.98f * steady);                                 // whole after 2 ms
    CHECK(slice[3990] < 0.2f * steady && slice[3998] < 0.05f * steady); // and out before its end
    auto whole = play(0, 0);       // the file's own ends: no fade (only the attack, which both have)
    if (!(whole[20] > 3 * slice[20])) std::fprintf(stderr, "zero_snap: whole %f %f slice %f %f\n", whole[20], whole[2000], slice[20], slice[2000]);
    CHECK(whole[2000] > 0.1f && whole[20] > 3 * slice[20]);
}

// The SFZ reader, as melodee-ios's: its levels, comments, defines, includes and traps.
void sfz_reader() {
    using S = std::vector<std::string>;
    auto samples = [](const SfzRead& r) { S out; for (auto& g : r.regions) out.push_back(g.sample); return out; };
    {
        auto read = read_sfz(
            "// a comment\n/* a block\n   of comment */\n#define $LOW 36\n"
            "<control> default_path=Samples\\Keys\\ note_offset=0\n"
            "<global> ampeg_release=0.5 ampeg_attack=0.01 ampeg_sustain=80 ampeg_decay=1\n"
            "<group> lovel=1 hivel=64 volume=-6 seq_length=2\n"
            "<region> sample=C2 soft.wav lokey=$LOW hikey=47 pitch_keycenter=c2 tune=-5 seq_position=1 loop_mode=loop_continuous loop_start=100 loop_end=199\n"
            "<region> sample=C2 soft 2.wav lokey=36 hikey=47 pitch_keycenter=36 seq_position=2 pan=-50 bend_up=200\n"
            "<group> lovel=65\n"
            "<region> sample=C4 loud.wav key=c#4 volume=3 transpose=-1 offset=48 end=4799 loop_mode=one_shot group=2 off_by=3 pitch_keytrack=0\n"
            "<region> sample=*sine key=70\n<region> sample=gone.wav hikey=-1\n");
        CHECK(read.regions.size() == 3 && read.left == 2);
        CHECK(read.ignored == std::set<std::string> {"bend_up"});
        const auto& soft = read.regions[0], &second = read.regions[1], &loud = read.regions[2];
        CHECK(soft.sample == "Samples/Keys/C2 soft.wav" && second.sample == "Samples/Keys/C2 soft 2.wav");
        CHECK(soft.zone.low_key == 36 && soft.zone.high_key == 47 && soft.zone.root == 36);
        CHECK(soft.zone.low_velocity == 1 && soft.zone.high_velocity == 64 && soft.zone.gain == -6 && soft.zone.tune == -5);
        CHECK(soft.zone.sequence_length == 2 && soft.zone.sequence == 1 && second.zone.sequence == 2 && second.zone.pan == -0.5f);
        CHECK(soft.zone.loop == ZoneLoop::On && soft.zone.loop_start == 100 && soft.zone.loop_end == 200);
        CHECK(loud.zone.low_key == 61 && loud.zone.high_key == 61 && loud.zone.root == 61 && loud.zone.tune == -100 && loud.zone.gain == 3);
        CHECK(loud.zone.low_velocity == 65 && loud.zone.high_velocity == 127 && loud.zone.one_shot && loud.zone.group == 2 && loud.zone.off_by == 3);
        CHECK(loud.zone.keytrack == 0 && loud.offset == 48 && loud.end == 4800);
        CHECK(read.envelope.attack == 0.01 && read.envelope.decay == 1.0 && read.envelope.sustain == 80.0 && read.envelope.release == 0.5);
    }
    CHECK(sfz_note("60") == 60 && sfz_note("c4") == 60 && sfz_note("C4") == 60 && sfz_note("c#4") == 61 && sfz_note("db4") == 61);
    CHECK(sfz_note("a-1") == 9 && sfz_note("b3") == 59 && !sfz_note("zz") && !sfz_note("c"));
    {
        auto read = read_sfz("<group>hivel=50\n#include \"more.sfz\"\n<region>sample=c.wav", [](const std::string& name) {
            return name == "more.sfz" ? std::optional<std::string>("<region>sample=b.wav") : std::nullopt;
        });
        CHECK(samples(read) == S({"b.wav", "c.wav"}));
        for (auto& g : read.regions) CHECK(g.zone.high_velocity == 50);
    }
    { // laid out as Splendid Grand Piano's file is
        std::map<std::string, std::string> files {
            {"Data/Res.txt", "group_volume=-11\nampeg_attack=0.1\n\n<region> lokey=021 hikey=022 pitch_keycenter=033 sample=PP A0.$EXT\n"},
            {"Data/PP.txt", "<region> region_label=01 lokey=021 hikey=024 pitch_keycenter=023 ampeg_release=0.6 sample=PP B-1.$EXT\n"
                            "<region> region_label=07 lokey=036 hikey=037 pitch_keycenter=037 sample=PP C#1.$EXT volume=1\n"
                            "<region> region_label=08 lokey=038 hikey=039 pitch_keycenter=038 sample=PP D1.$EXT offset=1046\n"
                            "<region> region_label=44 lokey=091 hikey=091 pitch_keycenter=091 ampeg_release=10 sample=PP G5.$EXT pan=-30 offset=135\n"},
            {"Data/FF.txt", "<region> lokey=036 hikey=037 pitch_keycenter=037 sample=FF C#1.$EXT\n"}};
        auto read = read_sfz(
            "#define $RES 70\n#define $RELEASE 72\n#define $EXT flac\n#define $DIR Data\n"
            "<control>\ndefault_path=Samples/\nlabel_cc64=Sus.Pedal\nlabel_cc$RES=Resonance\nset_hdcc$RES=0.5\nset_hdcc$RELEASE=0.35\n"
            "//*******************************************************\n"
            "<global>\nampeg_attack=0.0005\nampeg_hold=8\nampeg_decay=80\nampeg_sustain=0\nampeg_release_oncc$RELEASE=2\nampeg_releasecc64=15\nampeg_release_curvecc64=11\n"
            "<curve>curve_index=11\nv000=0\nv127=1\n"
            "<group>\ngroup=1\nlocc64=65\nlocc$RES=1\n#include \"$DIR/Res.txt\"\n"
            "<group>\nlovel=1\nhivel=40\ncutoff=1000\ngroup_volume=0\n#define $DYN PP\ngroup_label=P$DYN\n#include \"$DIR/$DYN.txt\"\n"
            "<group>\nlovel=41\nhivel=127\ngroup_volume=-3\n#define $DYN FF\ngroup_label=$DYN\n#include \"$DIR/$DYN.txt\"\n",
            [&](const std::string& name) { auto f = files.find(name); return f == files.end() ? std::nullopt : std::optional<std::string>(f->second); });
        CHECK(read.left == 1 && read.left_by == std::set<std::string> {"locc64"}); // the resonance layer waits for the pedal
        CHECK(read.regions.size() == 5);
        const auto& first = read.regions[0], &sharp = read.regions[1], &last = read.regions[3], &loud = read.regions[4];
        CHECK(first.sample == "Samples/PP B-1.flac" && sharp.sample == "Samples/PP C#1.flac" && loud.sample == "Samples/FF C#1.flac");
        for (unsigned n = 0; n < 4; ++n) CHECK(read.regions[n].zone.cutoff == 1000 && read.regions[n].zone.high_velocity == 40);
        CHECK(loud.zone.cutoff == 0 && loud.zone.low_velocity == 41 && loud.zone.gain == -3);
        CHECK(read.regions[2].offset == 1046 && last.zone.pan == -0.3f);
        CHECK(read.envelope.sustain == 100.0); // the hold and decay pass the longest stage: the sample falls by itself
        CHECK(read.envelope.release == 0.001 + 2 * 0.35);
        CHECK(sharp.zone.release == 0 && loud.zone.release == 0);
        CHECK(first.zone.release == sfz_release_step(1.3) - sfz_release_step(0.701));
        CHECK(last.zone.release == 100 - sfz_release_step(0.701));
        CHECK(read.velocity == 100);
        for (auto& name : read.ignored) CHECK(name.rfind("label_", 0) != 0 && name.rfind("set_", 0) != 0);
        CHECK(!read.ignored.count("ampeg_release_oncc72") && !read.ignored.count("cutoff") && read.ignored.count("ampeg_releasecc64"));
    }
    {
        auto found = sfz_opcodes("sample=PP C#1.flac Lokey=36 x-y=1  9a=2 == hikey= 40 path=a=b.wav");
        std::vector<std::pair<std::string, std::string>> expected {
            {"sample", "PP C#1.flac"}, {"lokey", "36 x-"}, {"y", "1  9"}, {"a", "2 =="}, {"hikey", "40"}, {"path", ""}, {"a", "b.wav"}};
        CHECK(found == expected);
    }
    { // a line of stars is a line comment; a /* never closed is the rest of the file
        auto read = read_sfz("//****************\n<region> sample=a.wav // and what follows /* is still this line's\n"
                             "/* a // in a block */ <region> sample=b.wav\n/////////////\n<region> sample=c.wav /* never closed\n<region> sample=d.wav\n");
        CHECK(samples(read) == S({"a.wav", "b.wav", "c.wav"}));
        CHECK(sfz_uncommented("a//b\r\nc/*d*/e\n") == "a\r\nce\n");
    }
    {
        auto read = read_sfz("<control> set_cc1=100 set_hdcc2=0.25\n<region> sample=up.wav key=60 hicc64=63\n<region> sample=down.wav key=60 locc64=64\n"
                             "<region> sample=wheel high.wav key=61 locc1=90\n<region> sample=wheel low.wav key=61 hicc1=89\n"
                             "<region> sample=fine.wav key=62 lohdcc2=0.5\n<region> sample=fine low.wav key=62 hihdcc2=0.5\n<region> sample=note.wav key=63 locc133=64\n");
        CHECK(samples(read) == S({"up.wav", "wheel high.wav", "fine low.wav", "note.wav"}));
        CHECK(read.left == 3 && read.ignored.count("locc133"));
    }
    {
        auto read = read_sfz("<global> sw_lokey=24 sw_hikey=25 sw_default=24\n<region> sample=note.wav key=60 sw_last=24\n<region> sample=other.wav key=60 sw_last=25\n"
                             "<region> sample=tail.wav key=60 trigger=release sw_last=24\n<region> sample=first.wav key=61 trigger=first sw_last=24\n"
                             "<region> sample=slur.wav key=61 trigger=legato sw_last=24\n");
        CHECK(samples(read) == S({"note.wav", "first.wav"}));
        CHECK(read.left == 3 && read.left_by == std::set<std::string>({"trigger", "sw_last"}));
    }
    {
        auto read = read_sfz("<region> sample=a.wav cutoff=800 resonance=9 fil_type=hpf_2p fil_veltrack=1200 fil_keytrack=50 fil_keycenter=c5\n"
                             "<region> sample=b.wav cutoff=800 fil_type=lpf_1p\n<region> sample=c.wav cutoff=800 fil_type=bpf_2p\n"
                             "<region> sample=d.wav cutoff=800 fil_type=brf_2p\n<region> sample=e.wav cutoff=99999 resonance=99\n");
        const auto& r = read.regions;
        CHECK(r[0].zone.cutoff == 800 && r[0].zone.resonance == 9 && r[0].zone.filter == PadFilter::Hpf2 && r[0].zone.cutoff_velocity == 1200
              && r[0].zone.cutoff_key == 50 && r[0].zone.cutoff_center == 72);
        CHECK(r[1].zone.filter == PadFilter::Lpf2 && r[2].zone.filter == PadFilter::Bpf2);
        CHECK(r[3].zone.cutoff == 0 && r[3].zone.filter == PadFilter::Off && read.ignored == std::set<std::string> {"fil_type"});
        CHECK(r[4].zone.cutoff == 20000 && r[4].zone.resonance == 40);
    }
    { // a zone's envelope is off the one most share
        auto read = read_sfz("<region> sample=a.wav ampeg_release=2\n<region> sample=b.wav ampeg_release=0.5 ampeg_attack=0.1\n<region> sample=c.wav ampeg_release=0.5\n"
                             "<region> sample=d.wav ampeg_release=0.5 ampeg_sustain=50 ampeg_decay=1\n<region> sample=e.wav\n");
        CHECK(read.envelope.attack == 0.0 && read.envelope.decay == 0.0 && read.envelope.sustain == 100.0 && read.envelope.release == 0.5);
        const auto& r = read.regions;
        CHECK(r[1].zone.release == 0 && r[0].zone.release == sfz_release_step(2) - sfz_release_step(0.5));
        CHECK(r[1].zone.attack == sfz_attack_step(0.1) && r[2].zone.attack == 0);
        CHECK(r[3].zone.sustain == 50 - 100 && r[3].zone.decay == sfz_decay_step(1) - sfz_decay_step(0));
        CHECK(r[4].zone.release == sfz_release_step(0.001) - sfz_release_step(0.5));
    }
    {
        auto read = read_sfz("<region> sample=a.wav\n<region> sample=b.wav");
        CHECK(!read.envelope.attack && !read.envelope.decay && !read.envelope.sustain && !read.envelope.release);
        for (auto& g : read.regions) CHECK(g.zone.attack == 0 && g.zone.release == 0 && g.zone.sustain == 0 && g.zone.decay == 0);
    }
    {
        auto read = read_sfz("<region> sample=a.wav ampeg_hold=1 ampeg_decay=2 ampeg_sustain=40 amp_veltrack=25");
        CHECK(read.envelope.decay == 3.0 && read.envelope.sustain == 40.0 && read.velocity == 25);
    }
    {
        std::string many;
        for (unsigned n = 0; n < max_zones + 3; ++n) many += "<region>sample=" + std::to_string(n) + ".wav\n";
        auto read = read_sfz(many);
        CHECK(read.regions.size() == max_zones && read.left == 3);
    }
}

// An MPC keygroup program's keygroups and layers as the SFZ reader's zones.
void keygroup_reader() {
    CHECK(std::fabs(keygroup_decibels(1.0)) < 1e-9 && std::fabs(keygroup_decibels(0.5) + 6.0206) < 1e-3);
    CHECK(mpc_level(0.707946) == 100 && mpc_level(1.0) == 127 && mpc_level(0) == 0 && mpc_pan(0.5) == 0 && mpc_pan(0.37) == -13);
    CHECK(mpc_filter(1) == PadFilter::Lpf2 && mpc_filter(7) == PadFilter::Hpf2 && mpc_filter(29) == PadFilter::Classic && mpc_filter(20) == PadFilter::Off);
    CHECK(std::fabs(keygroup_seconds(0) - 0.001) < 1e-12 && std::fabs(keygroup_seconds(1) - 100) < 1e-9);
    KeygroupProgram program;
    program.type = "Keygroup";
    program.tags = {{"KeygroupNumKeygroups", "2"}};
    KeygroupInstrument low;
    low.number = 1;
    low.tags = {{"LowNote", "0"}, {"HighNote", "59"}, {"ZonePlay", "1"}, {"TriggerMode", "2"}, {"VolumeRelease", "0.2"},
                {"VolumeSustain", "0.5"}, {"VelocitySensitivity", "0.5"}, {"FilterType", "2"}};
    low.layers = {{{"Active", "True"}, {"SampleName", "Piano C3 "}, {"RootNote", "61"}, {"VelStart", "0"}, {"VelEnd", "64"},
                   {"Volume", "0.353"}, {"Pan", "0"}, {"Pitch", "0"}, {"TuneCoarse", "2"}, {"TuneFine", "10"}, {"SliceLoop", "0"}},
                  {{"Active", "True"}, {"SampleName", "Piano C3 Hard"}, {"RootNote", "61"}, {"VelStart", "65"}, {"VelEnd", "127"},
                   {"Volume", "1.000000"}, {"Pan", "1"}, {"Pitch", "-0.5"}, {"SliceStart", "100"}, {"SliceEnd", "5000"},
                   {"SliceLoop", "1"}, {"SliceLoopStart", "2000"}},
                  {{"Active", "False"}, {"SampleName", "Off"}},
                  {{"Active", "True"}, {"SampleName", ""}}};
    KeygroupInstrument high;
    high.number = 2;
    high.tags = {{"LowNote", "60"}, {"HighNote", "127"}, {"ZonePlay", "0"}, {"TriggerMode", "0"}, {"IgnoreBaseNote", "True"}};
    high.layers = {{{"SampleName", "Hit A"}, {"VelStart", "0"}, {"VelEnd", "60"}, {"KeyTrack", "False"}, {"RootNote", "73"}},
                   {{"SampleFile", "C:\\Samples\\hit b.wav"}, {"VelStart", "61"}, {"VelEnd", "127"}, {"KeyTrack", "True"}, {"Loop", "True"}}};
    KeygroupInstrument past;
    past.number = 3;
    past.layers = {{{"SampleName", "Never"}}};
    program.instruments = {low, high, past};
    auto read = read_keygroup(program);
    CHECK(read.regions.size() == 4 && read.left == 0);
    const auto& soft = read.regions[0];
    const auto& hard = read.regions[1];
    CHECK(soft.sample == "Piano C3 .WAV" && hard.sample == "Piano C3 Hard.WAV");
    CHECK(soft.zone.low_key == 0 && soft.zone.high_key == 59 && soft.zone.root == 60);
    CHECK(soft.zone.low_velocity == 1 && soft.zone.high_velocity == 64 && hard.zone.low_velocity == 65);
    CHECK(soft.zone.tune == 210 && hard.zone.tune == -50);
    CHECK(std::fabs(soft.zone.gain - 20 * std::log10(0.353)) < 1e-4 && std::fabs(hard.zone.gain) < 1e-4);
    CHECK(soft.zone.pan == -1 && hard.zone.pan == 1);
    CHECK(soft.zone.loop == ZoneLoop::Off && soft.loop_named && !soft.loop_asked);
    CHECK(hard.offset == 100 && hard.end == 5000 && hard.zone.loop == ZoneLoop::On && hard.zone.loop_start == 2000 && hard.zone.loop_end == 5000);
    CHECK(read.ignored.count("FilterType") && read.velocity == 50);
    CHECK(read.envelope.sustain == 50.0 && std::fabs(*read.envelope.release - keygroup_seconds(0.2)) < 1e-9);
    // the cycle: one layer a press, whatever its velocity, a hit a turn
    const auto& a = read.regions[2];
    const auto& b = read.regions[3];
    CHECK(a.sample == "Hit A.WAV" && b.sample == "hit b.wav");
    CHECK(a.zone.low_velocity == 1 && a.zone.high_velocity == 127 && b.zone.low_velocity == 1);
    CHECK(a.zone.sequence == 1 && b.zone.sequence == 2 && a.zone.sequence_length == 2 && b.zone.sequence_length == 2);
    CHECK(a.zone.one_shot && b.zone.one_shot && b.zone.loop == ZoneLoop::Off);
    CHECK(a.zone.keytrack == 0 && b.zone.keytrack == 100 && a.zone.root == 72);
    CHECK(a.zone.release == sfz_release_step(keygroup_seconds(0.63)) - sfz_release_step(keygroup_seconds(0.2)));
    {   // a pick at random splits the dice; a release trigger is left out and named
        KeygroupProgram picked;
        picked.type = "Keygroup";
        KeygroupInstrument one;
        one.tags = {{"ZonePlay", "2"}};
        one.layers = {{{"SampleName", "x"}}, {{"SampleName", "y"}, {"Loop", "True"}}};
        KeygroupInstrument off = one;
        off.tags = {{"TriggerMode", "1"}};
        picked.instruments = {one, off};
        auto r = read_keygroup(picked);
        CHECK(r.regions.size() == 2 && r.left == 2 && r.left_by.count("TriggerMode"));
        CHECK(r.regions[0].zone.high_random == 0.5f && r.regions[1].zone.low_random == 0.5f);
        // Loop with no points: the file's own loop, if it has one
        CHECK(r.regions[1].zone.loop == ZoneLoop::On && r.regions[1].zone.loop_end == 0 && r.regions[1].loop_asked);
        CHECK(r.envelope.release && std::fabs(*r.envelope.release - keygroup_seconds(0.63)) < 1e-9);
    }
    {   // a keygroup's own volume and tune go onto its zones
        KeygroupProgram tuned;
        tuned.type = "Keygroup";
        KeygroupInstrument one;
        one.tags = {{"Volume", "1.0"}, {"TuneCoarse", "-1"}, {"TuneFine", "5"}};
        one.layers = {{{"SampleName", "x"}, {"Volume", "0.5"}, {"TuneCoarse", "2"}}};
        tuned.instruments = {one};
        auto r = read_keygroup(tuned);
        CHECK(r.regions.size() == 1 && r.regions[0].zone.tune == 105 && std::fabs(r.regions[0].zone.gain - (6 + 20 * std::log10(0.5))) < 1e-3);
    }
    {   // the program's own, over every keygroup: an organ's (Vintage Organs) velocity off, its volume and tune
        KeygroupProgram organ;
        organ.type = "Keygroup";
        organ.tags = {{"Volume", "0.707946"}, {"TuneCoarse", "1"}, {"KeygroupTranspose", "-2"}, {"KeygroupMasterTranspose", "0.500000"},
                      {"KeygroupNumKeygroups", "1"}};
        KeygroupInstrument one;
        one.number = 1;
        one.tags = {{"LowNote", "24"}, {"HighNote", "37"}, {"Volume", "0.46"}, {"VelocitySensitivity", "0.000000"}, {"ZonePlay", "1"}};
        one.layers = {{{"SampleName", "Organ-036 C1"}, {"RootNote", "37"}, {"Volume", "0.71"}},
                      {{"SampleName", "Organ-036 C1 b"}, {"RootNote", "37"}}};
        KeygroupInstrument spare = one;
        spare.number = 2; // past KeygroupNumKeygroups
        organ.instruments = {one, spare};
        auto r = read_keygroup(organ);
        CHECK(r.regions.size() == 2 && r.velocity == 0 && r.ignored.empty());
        CHECK(r.regions[0].zone.root == 36 && r.regions[0].zone.tune == -100 && r.regions[1].zone.low_velocity == 1); // both layers stack
        const double kg = 40 * std::log10(0.46) + 6;
        CHECK(std::fabs(r.regions[0].zone.gain - (kg + 20 * std::log10(0.71))) < 1e-3 && std::fabs(r.regions[1].zone.gain - kg) < 1e-3);
        organ.tags["KeygroupMasterTranspose"] = "0.6";
        CHECK(read_keygroup(organ).ignored.count("KeygroupMasterTranspose"));
    }
}

// 16 Levels in pitch on a kit: the octave − / + set over the pads; a synth's keeps its own.
void kit_levels_octave() {
    auto project = std::make_unique<Project>();
    auto& kit = project->tracks[4];
    kit.engine = EngineKind::Sampler;
    kit.levels = true;
    kit.levels_kind = uint8_t(LevelsKind::Tune);
    CHECK(levels_of(*project, kit, 3).semitones == 0); // the original on pad 4, as it is
    kit.octave = 3;
    CHECK(levels_of(*project, kit, 3).semitones == 12 && levels_of(*project, kit, 3).shown == 12);
    kit.octave = -1;
    kit.levels_kind = uint8_t(LevelsKind::Scale);
    CHECK(levels_of(*project, kit, 0).semitones == -36);
    kit.levels_kind = uint8_t(LevelsKind::Velocity);
    CHECK(levels_of(*project, kit, 0).semitones == 0);
    auto& synth = project->tracks[1];
    synth.engine = EngineKind::Prophet;
    synth.levels_kind = uint8_t(LevelsKind::Tune);
    synth.octave = 5;
    CHECK(levels_of(*project, synth, 3).semitones == 0);
    auto bank = std::make_unique<PatternBank>();
    auto bytes = save_project(*project, *bank);
    auto back = std::make_unique<Project>();
    CHECK(load_project(bytes, *back, *bank).empty() && back->tracks[4].octave == -1);
}

// A Logic EXS24 instrument's blocks, in either byte order, as Logic lays them out.
struct ExsBytes {
    bool big = false;
    std::string out;
    void put(std::string& c, size_t at, uint32_t v, unsigned width = 4) const {
        for (unsigned n = 0; n < width; ++n) c[at + n] = char(big ? v >> (8 * (width - 1 - n)) : v >> (8 * n));
    }
    void block(int kind, const std::string& name, const std::string& content) {
        std::string h(84, '\0');
        h[0] = char(big ? 0 : 1); h[1] = 1; h[3] = char(kind);
        put(h, 4, uint32_t(content.size()));
        std::memcpy(&h[16], big ? "SOBT" : "TBOS", 4);
        std::memcpy(&h[20], name.data(), std::min<size_t>(name.size(), 63));
        out += h + content;
    }
    struct Zone { int opts = 0, key = 60, fine = 0, pan = 0, volume = 0, low = 0, high = 127, vlow = 0, vhigh = 127, coarse = 0;
                  uint32_t start = 0, end = 0, ls = 0, le = 0; int lopts = 0, dir = 0; int32_t group = 0, sample = 0; };
    void zone(const Zone& z) {
        std::string c(136, '\0');
        c[0] = char(z.opts); c[1] = char(z.key); c[2] = char(z.fine); c[3] = char(z.pan); c[4] = char(z.volume);
        c[6] = char(z.low); c[7] = char(z.high); c[9] = char(z.vlow); c[10] = char(z.vhigh);
        put(c, 12, z.start); put(c, 16, z.end); put(c, 20, z.ls); put(c, 24, z.le);
        c[33] = char(z.lopts); c[34] = char(z.dir); c[80] = char(z.coarse);
        put(c, 88, uint32_t(z.group)); put(c, 92, uint32_t(z.sample));
        block(1, "zone", c);
    }
    struct Group { int volume = 0, pan = 0, options = 0, exclusive = 0, note = 0, select = 0, high_key = 127; int32_t after = -2; };
    void group(const Group& g) {
        std::string c(132, '\0');
        c[0] = char(g.volume); c[1] = char(g.pan); c[3] = char(g.options); c[4] = char(g.exclusive); c[6] = char(127);
        c[75] = char(g.note); put(c, 80, uint32_t(g.after)); c[84] = char(g.select); c[87] = char(127); c[89] = char(g.high_key);
        block(2, "group", c);
    }
    void sample(const std::string& name, const std::string& folder, bool named = true) {
        std::string c(named ? 592 : 336, '\0');
        std::memcpy(&c[80], folder.data(), folder.size());
        if (named) std::memcpy(&c[336], name.data(), name.size());
        block(3, name, c);
    }
    void parameters(const std::map<int, int>& old_ones, const std::map<int, int>& new_ones) {
        std::string c(4 + 300 + 4 + 4 * new_ones.size(), '\0');
        put(c, 0, 100);
        unsigned n = 0;
        for (const auto& [id, value] : old_ones) { c[4 + n] = char(id); put(c, 4 + 100 + 2 * n, uint16_t(int16_t(value)), 2); ++n; }
        put(c, 304, uint32_t(new_ones.size()));
        n = 0;
        for (const auto& [id, value] : new_ones) { put(c, 308 + 4 * n, uint32_t(id), 2); put(c, 310 + 4 * n, uint32_t(value), 2); ++n; }
        block(4, "Default Param", c);
    }
};

std::string exs_instrument(bool big) {
    ExsBytes b;
    b.big = big;
    b.block(0, "Test", std::string(80, '\0'));
    ExsBytes::Zone a; // velocity range on, tuned, panned, trimmed, looped
    a.opts = 8; a.key = 60; a.low = 0; a.high = 63; a.vlow = 0; a.vhigh = 63; a.fine = 10; a.coarse = 1; a.volume = -3; a.pan = 25;
    a.start = 100; a.end = 5000; a.lopts = 1; a.ls = 1000; a.le = 4000; a.group = 0; a.sample = 0;
    b.zone(a);
    ExsBytes::Zone one; // one shot, pitch off, no velocity range (its numbers unused), a loop one shot leaves alone
    one.opts = 1 | 2; one.key = 72; one.low = 64; one.high = 127; one.vlow = 20; one.vhigh = 30; one.lopts = 1; one.le = 10; one.group = 1; one.sample = 1;
    b.zone(one);
    ExsBytes::Zone first; // round robin, a loop to the end played to its end on release, backwards
    first.group = 2; first.sample = 2; first.lopts = 1 | 4; first.dir = 1;
    b.zone(first);
    ExsBytes::Zone second = first;
    second.group = 3; second.lopts = 0;
    b.zone(second);
    ExsBytes::Zone keyed; keyed.group = 4; keyed.sample = 0; b.zone(keyed);
    ExsBytes::Zone other_key; other_key.group = 5; b.zone(other_key);
    ExsBytes::Zone muted; muted.group = 6; b.zone(muted);
    ExsBytes::Zone nowhere; nowhere.group = -1; nowhere.sample = 9; b.zone(nowhere);
    ExsBytes::Zone gone; gone.group = -1; gone.sample = 3; b.zone(gone);
    ExsBytes::Group g0; g0.volume = -2; g0.pan = -10; g0.high_key = 60; b.group(g0);
    ExsBytes::Group g1; g1.exclusive = 3; b.group(g1);
    ExsBytes::Group g2; g2.select = 2; g2.after = -1; b.group(g2);
    ExsBytes::Group g3; g3.select = 2; g3.after = 2; b.group(g3);
    ExsBytes::Group g4; g4.select = 1; g4.note = 24; b.group(g4);
    ExsBytes::Group g5; g5.select = 1; g5.note = 25; b.group(g5);
    ExsBytes::Group g6; g6.options = 16; b.group(g6);
    b.sample("a.aif", "/Volumes/Library/Samples");
    b.sample("b.wav", "/Volumes/Library/Samples", false); // an older block: its name is the file's
    b.sample("c.wav", "/Volumes/Library/Samples");
    b.sample("gone.wav", "/Volumes/Library/Samples");
    b.parameters({{0x07, -6}, {0x0e, 1}, {0x0f, -5}, {0x51, 127}, {0x55, 64}, {0x5a, -30}}, {{0x16d, 0}});
    return b.out;
}

// A Logic EXS24 instrument's zones, groups, samples and parameters as the SFZ reader's zones.
void exs_reader() {
    std::string error;
    CHECK(read_exs("not an instrument at all, though long enough to hold a head of eighty-four bytes, surely", {}, &error).regions.empty() &&
          error == "Not an EXS instrument");
    CHECK(std::fabs(exs_seconds(127) - 10) < 1e-9 && exs_seconds(0) == 0);
    for (bool big : {false, true}) {
        const auto bytes = exs_instrument(big);
        CHECK(exs_magic(bytes));
        std::set<std::string> asked;
        auto read = read_exs(bytes, [&](const std::string& name, const std::string& folder) -> std::string {
            CHECK(folder == "/Volumes/Library/Samples");
            asked.insert(name);
            return name == "gone.wav" ? "" : "Samples/" + name;
        });
        CHECK(asked == std::set<std::string>({"a.aif", "b.wav", "c.wav", "gone.wav"}));
        CHECK(read.regions.size() == 5);
        CHECK(read.left == 4 && read.left_by == std::set<std::string>({"keyswitch", "muted group", "missing files"}));
        CHECK(read.ignored == std::set<std::string>({"loop direction"}));
        CHECK(std::fabs(read.velocity - 50) < 1e-9);
        CHECK(read.envelope.release && std::fabs(*read.envelope.release - exs_seconds(64)) < 1e-9 && read.envelope.sustain == 100.0);
        if (read.regions.size() != 5) continue;
        const auto& a = read.regions[0];
        CHECK(a.sample == "Samples/a.aif" && a.zone.low_key == 0 && a.zone.high_key == 60 && a.zone.root == 60);
        CHECK(a.zone.low_velocity == 1 && a.zone.high_velocity == 63 && a.zone.keytrack == 100);
        CHECK(a.zone.tune == 100 + 10 + 100 - 5 && std::fabs(a.zone.gain + 11) < 1e-6 && std::fabs(a.zone.pan - 0.3f) < 1e-6);
        CHECK(a.offset == 100 && a.end == 5000 && a.loop_named && a.loop_asked);
        CHECK(a.zone.loop == ZoneLoop::On && a.zone.loop_start == 1000 && a.zone.loop_end == 4000);
        const auto& one = read.regions[1];
        CHECK(one.sample == "Samples/b.wav" && one.zone.low_key == 64 && one.zone.low_velocity == 1 && one.zone.high_velocity == 127);
        CHECK(one.zone.one_shot && one.zone.keytrack == 0 && one.zone.loop == ZoneLoop::Off && !one.loop_asked && one.loop_named);
        CHECK(one.zone.group == 3 && one.zone.off_by == 3 && one.offset == -1 && one.end == -1 && one.zone.tune == 95);
        const auto& first = read.regions[2];
        const auto& second = read.regions[3];
        CHECK(first.zone.sequence == 1 && first.zone.sequence_length == 2 && second.zone.sequence == 2 && second.zone.sequence_length == 2);
        CHECK(first.zone.loop == ZoneLoop::UntilReleased && first.zone.loop_end == 0x7fffffff && second.zone.loop == ZoneLoop::Off);
        CHECK(read.regions[4].zone.sequence_length == 0 && read.regions[4].sample == "Samples/a.aif");
    }
    // Cut short: the blocks that came whole are read.
    auto whole = exs_instrument(false);
    auto cut = read_exs(whole.substr(0, whole.size() - 500), {});
    CHECK(cut.regions.size() == 5 && cut.envelope.release == std::nullopt);
}


// An expansion's zip entries stay inside its folder.
void expansion_paths() {
    CHECK(expansion_entry_path("Expansion.xml") == "Expansion.xml");
    CHECK(expansion_entry_path("Organ_[TrackData]/Organ-036 C1.wav") == "Organ_[TrackData]/Organ-036 C1.wav");
    CHECK(expansion_entry_path("a\\b//./c.wav") == "a/b/c.wav");
    CHECK(expansion_entry_path("/abs/c.wav") == "abs/c.wav");
    CHECK(expansion_entry_path("../evil.wav").empty() && expansion_entry_path("a/../../evil.wav").empty());
    CHECK(expansion_entry_path("C:\\evil.wav").empty() && expansion_entry_path("").empty());
}

// A drum program's pad, as the Dirty Drummer kits have them: four hits cycled,
// the pad's own level, pan, filter and envelope; a one-layer pad plays its WAV.
void drum_program_pads() {
    KeygroupInstrument kick;
    kick.number = 1;
    kick.tags = {{"ZonePlay", "0"}, {"TriggerMode", "0"}, {"Volume", "0.707946"}, {"Pan", "0.370000"}, {"Mono", "True"},
                 {"MuteGroup", "2"}, {"VolumeDecay", "0.050000"}, {"VolumeDecayType", "True"}, {"FilterType", "1"},
                 {"Cutoff", "0.732284"}, {"TuneCoarse", "-1"}, {"VelocitySensitivity", "0.5"}, {"LowNote", "0"}, {"HighNote", "127"}};
    for (const char* hit : {"Acoustic-Kick-dd2 dk k_p1", "Acoustic-Kick-dd2 dk k_p2", "Acoustic-Kick-dd2 dk k_p3", "Acoustic-Kick-dd2 dk k_p4"})
        kick.layers.push_back({{"Active", "True"}, {"SampleName", hit}, {"VelStart", "0"}, {"VelEnd", "127"}, {"RootNote", "0"},
                               {"Volume", "1.000000"}, {"Pan", "0.500000"}, {"Pitch", "-4.000000"}, {"TuneCoarse", "-4"}, {"KeyTrack", "False"},
                               {"SliceLoop", "0"}, {"SliceStart", "0"}, {"SliceEnd", "17557"}});
    kick.layers.push_back({{"Active", "True"}, {"SampleName", ""}});
    CHECK(keygroup_layers(kick).size() == 4);
    auto read = read_drum_pad(kick);
    CHECK(read.regions.size() == 4 && read.left == 0);
    for (unsigned n = 0; n < 4; ++n) {
        const auto& z = read.regions[n].zone;
        CHECK(z.low_key == 0 && z.high_key == 127 && z.root == 60 && z.keytrack == 100); // RootNote 0 is not C-1 here
        CHECK(z.sequence == n + 1 && z.sequence_length == 4 && z.one_shot && z.tune == -400 && z.gain == 0);
        CHECK(read.regions[n].end == 17557);
    }
    CHECK(read.regions[0].sample == "Acoustic-Kick-dd2 dk k_p1.WAV");
    CHECK(!read.envelope.attack && !read.envelope.release && read.regions[2].zone.release == 0); // the pad's envelope, not the zones'
    PadSound pad;
    keygroup_pad(kick, 1.0, pad);
    CHECK(pad.level == 100 && pad.pan == -13 && pad.mono && pad.group == 2 && pad.play == PadPlay::OneShot);
    CHECK(pad.pitch == -1 && pad.fine == 0 && pad.start == 0 && pad.end == 0); // the layers' tune and region are the zones'
    CHECK(pad.filter == PadFilter::Lpf2 && pad.cutoff == 73 && pad.decay == 5 && !pad.decay_from_start && pad.velocity_sense == 50);
    KeygroupInstrument snare = kick;
    snare.layers.resize(1);
    snare.layers[0]["Volume"] = "0.63";
    snare.layers[0]["SliceLoop"] = "1";
    snare.layers[0]["SliceLoopStart"] = "100";
    snare.tags["Cutoff"] = "1.0";
    snare.tags["FilterType"] = "1";
    PadSound one;
    keygroup_pad(snare, 2.0, one);
    CHECK(one.pitch == -5 && one.end == 2 * 17557 && one.loop && !one.loop_lock && one.loop_start == 200);
    CHECK(one.gain == int16_t(std::lround(20 * std::log10(0.63) * 100)) && one.filter == PadFilter::Off); // wide open: off
}

// Laid out once and found again; chunks whole pages; the least lately played going first.
void sample_cache() {
    namespace fs = std::filesystem;
    auto base = fs::temp_directory_path() / ("melodee-cache-" + std::to_string(getpid()));
    fs::create_directories(base / "Cache");
    SampleCache cache;
    cache.set_folder((base / "Cache").string());
    auto source = (base / "PP C#1.wav").string();
    { std::ofstream(source) << "abc"; }
    auto file = cache.file_of(source);
    CHECK(!file.empty() && !cache.file_of((base / "gone.wav").string()).size());
    LaidOut info;
    CHECK(!cache.ready(file, info));
    SampleData data;
    data.frames = 40000; data.left.resize(40000); data.right.resize(40000);
    for (unsigned n = 0; n < 40000; ++n) { data.left[n] = float(n); data.right[n] = -float(n); }
    LaidOut out; out.frames = 40000; out.channels = 2; out.source_rate = 48000; out.loop_start = 7; out.loop_end = 99;
    CHECK(cache.lay_out(file, data, out).empty());
    CHECK(!fs::exists(file + ".part"));
    CHECK(cache.ready(file, info) && info.frames == 40000 && info.channels == 2 && info.source_rate == 48000 && info.loop_start == 7 && info.loop_end == 99);
    {
        ZonePart part;
        CHECK(SampleCache::map(file, info, part));
        CHECK(part.left[39999] == 39999.0f && part.right[39999] == -39999.0f && part.left[0] == 0.0f);
        CHECK(part.pins.size() == 3);
        // both sides start on a chunk boundary of the file, so a chunk is whole pages
        CHECK((reinterpret_cast<uintptr_t>(part.left) - reinterpret_cast<uintptr_t>(part.mapping)) % stream_chunk_bytes == 0);
        CHECK((reinterpret_cast<uintptr_t>(part.right) - reinterpret_cast<uintptr_t>(part.mapping)) % stream_chunk_bytes == 0);
    }
    { // one side has no right
        auto mono = (base / "Cache/mono.f32").string();
        LaidOut one; one.frames = 100; one.channels = 1;
        SampleData m; m.frames = 100; m.left.assign(100, 0.5f); m.right = m.left;
        CHECK(cache.lay_out(mono, m, one).empty());
        LaidOut got;
        CHECK(cache.ready(mono, got) && got.channels == 1);
        ZonePart part;
        CHECK(SampleCache::map(mono, got, part) && part.right == part.left && part.left[99] == 0.5f);
        CHECK(fs::file_size(mono) == SampleCache::bytes_of(100, 1));
    }
    { std::ofstream(file, std::ios::app) << "x"; } // not the size it says: laid out again
    CHECK(!cache.ready(file, info));
    { std::ofstream(source, std::ios::app) << "more"; }
    CHECK(cache.file_of(source) != file); // the source changed
    fs::remove_all(base / "Cache");
    fs::create_directories(base / "Cache");
    for (unsigned n = 0; n < 3; ++n) {
        auto name = base / "Cache" / (std::to_string(n) + ".f32");
        std::ofstream(name) << std::string(1000, 'x');
        fs::last_write_time(name, fs::file_time_type::clock::now() - std::chrono::hours(10 - n));
    }
    { std::ofstream(base / "Cache/other.txt") << std::string(5000, 'x'); }
    cache.tidy(2000);
    CHECK(!fs::exists(base / "Cache/0.f32") && fs::exists(base / "Cache/1.f32") && fs::exists(base / "Cache/2.f32") && fs::exists(base / "Cache/other.txt"));
    fs::remove_all(base);
}

// The pager pins ahead of a voice and lets go behind; the audio thread sees only what is safe.
std::vector<std::pair<uintptr_t, bool>>* pin_log = nullptr;
bool pin_fails = false;
bool logged_pin(const void* address, size_t, bool lock) {
    if (lock && pin_fails) return false;
    if (pin_log) pin_log->push_back({reinterpret_cast<uintptr_t>(address), lock});
    return true;
}

// Rendering on several cores (engine/render_pool.h): a busy project sounds the same, sample for sample, with
// render workers sharing every block as with the audio thread alone; and the workers did share them.
void parallel_render() {
    auto render = [](unsigned workers, uint64_t* helped) {
        auto p = std::make_unique<Project>();
        auto b = std::make_unique<PatternBank>();
        const EngineKind kinds[] {EngineKind::Prophet, EngineKind::Fm6, EngineKind::Cz, EngineKind::Sid,
                                  EngineKind::Kit909, EngineKind::Prophet, EngineKind::Fm6, EngineKind::Cz};
        const EffectKind chains[][2] {{EffectKind::Reverb, EffectKind::Echo}, {EffectKind::Chorus, EffectKind::Drive},
                                      {EffectKind::Phaser, EffectKind::Plate}, {EffectKind::Crush, EffectKind::Filter},
                                      {EffectKind::Compressor, EffectKind::Room}, {EffectKind::TapeEcho, EffectKind::Width},
                                      {EffectKind::Flanger, EffectKind::Shimmer}, {EffectKind::Tape, EffectKind::PingPong}};
        for (unsigned t = 0; t < 8; ++t) {
            load_sound(p->tracks[t], kinds[t], 0);
            p->tracks[t].mix.pan = int8_t(int(t) * 12 - 42);
            p->tracks[t].mix.send = uint8_t(20 + t * 5);
            p->tracks[t].mix.bus = uint8_t(t % bus_count);
            for (unsigned k = 0; k < 2; ++k) p->tracks[t].fx.slots[k] = {uint8_t(chains[t][k]), false, {60, 50, 40, 50, 30, 0}};
        }
        p->buses[1].fx.slots[0] = {uint8_t(EffectKind::Reverb), false, {70, 50, 40, 50, 0, 0}};
        EngineHost host;
        host.set_render_threads(workers, 0);
        host.load(*p, *b);
        std::vector<int32_t> block(256), all;
        for (unsigned n = 0; n < 600; ++n) {
            if (n % 50 == 0)
                for (unsigned t = 0; t < 8; ++t) {
                    host.send({Command::Type::NoteOff, uint8_t(t), 0, uint16_t(48 + t + (n / 50 + 1) % 2 * 7), 0});
                    host.send({Command::Type::NoteOn, uint8_t(t), 0, uint16_t(48 + t + (n / 50) % 2 * 7), int16_t(90 + t)});
                }
            host.render(block.data(), 128, 1);
            all.insert(all.end(), block.begin(), block.end());
        }
        if (helped) *helped = host.render_parallel_blocks();
        return all;
    };
    uint64_t helped = 0;
    auto alone = render(0, nullptr);
    auto shared = render(3, &helped);
    double energy = 0;
    for (int32_t s : alone) energy += std::fabs(s / 2147483648.0);
    CHECK(energy > 100);           // it plays
    CHECK(helped == 600);          // every block went to the workers
    CHECK(shared == alone);        // and sounds the same
    // Twice more: the workers sleep and wake between runs without losing a block.
    for (int again = 0; again < 2; ++again) CHECK(render(3, nullptr) == alone);
}

void stream_pager() {
    std::vector<std::pair<uintptr_t, bool>> log;
    pin_log = &log; pin_fails = false;
    StreamPager pager(logged_pin);
    std::vector<float> side(stream_chunk * 10);
    auto base = reinterpret_cast<uintptr_t>(side.data());
    auto chunk_of = [&](uintptr_t a) { return (a - base) / (stream_chunk * sizeof(float)); };
    ZonePart part;
    part.left = part.right = side.data();
    part.pins.assign(10, 0);
    part.total = stream_chunk * 10;
    part.frames.store(part.total);
    const auto c = stream_chunk;
    int s = pager.open(part, 0, false, 1.0f);
    CHECK(s >= 0);
    CHECK(pager.limit(s) == c); // the head is held, so safe before the pager has answered
    while (pager.pump()) {}
    CHECK(pager.limit(s) == 3 * c && part.pins[0] == 1 && part.pins[1] == 1 && part.pins[2] == 1 && part.pins[3] == 0);
    pager.place(s, 2 * c + 5);
    while (pager.pump()) {}
    CHECK(pager.limit(s) == 5 * c);
    CHECK(part.pins[0] == 0 && part.pins[1] == 1); // a chunk behind stays
    CHECK(!log.empty() && !log.back().second); // the last it did was let go
    { // a second voice over the same chunks keeps them when the first lets go
        int t = pager.open(part, 2 * c, false, 1.0f);
        CHECK(t >= 0 && t != s);
        while (pager.pump()) {}
        CHECK(part.pins[2] == 2);
        log.clear();
        pager.close(s);
        while (pager.pump()) {}
        CHECK(part.pins[2] == 1 && part.pins[1] == 0);
        for (auto& e : log) CHECK(chunk_of(e.first) != 2);
        pager.close(t);
        while (pager.pump()) {}
        for (auto n : part.pins) CHECK(n == 0);
    }
    { // to the end, and no further
        int t = pager.open(part, 8 * c, false, 1.0f);
        while (pager.pump()) {}
        CHECK(pager.limit(t) == 10 * c);
        pager.close(t);
        while (pager.pump()) {}
    }
    { // a loop coming round starts a new run: its old answer no longer counts
        int t = pager.open(part, 0, false, 1.0f);
        while (pager.pump()) {}
        pager.place(t, 2 * c);
        while (pager.pump()) {}
        CHECK(pager.limit(t) == 5 * c);
        pager.restart(t, c / 2);
        CHECK(pager.limit(t) == c / 2 + c); // before the pager answers: the loop's held start only
        while (pager.pump()) {}
        CHECK(pager.limit(t) == 3 * c);
        pager.close(t);
        while (pager.pump()) {}
    }
    { // reversed: nothing is safe until pinned, then downwards
        int t = pager.open(part, 9 * c + 7, true, 1.0f);
        CHECK(pager.limit(t) == UINT32_MAX);
        while (pager.pump()) {}
        CHECK(pager.limit(t) == 7 * c);
        pager.place(t, 5 * c + 3);
        while (pager.pump()) {}
        CHECK(pager.limit(t) == 3 * c && part.pins[9] == 0);
        pager.close(t);
        while (pager.pump()) {}
    }
    { // what stays held for the part outlasts every voice
        CHECK(pager.hold(part, 100, c));
        CHECK(part.pins[0] == 1 && part.pins[1] == 1 && part.pins[2] == 0);
        int t = pager.open(part, 0, false, 1.0f);
        while (pager.pump()) {}
        pager.place(t, 6 * c);
        while (pager.pump()) {}
        CHECK(part.pins[0] == 1 && part.pins[1] == 1);
        pager.close(t);
        while (pager.pump()) {}
        CHECK(part.pins[0] == 1 && part.pins[1] == 1 && part.pins[6] == 0);
    }
    { // a card gone: the stream says so and pins nothing more
        pin_fails = true;
        int t = pager.open(part, 3 * c, false, 1.0f);
        while (pager.pump()) {}
        CHECK(pager.dead(t) && pager.limit(t) == 4 * c);
        pin_fails = false;
        pager.close(t);
        while (pager.pump()) {}
    }
    { // every stream taken: -1, not a reuse
        std::vector<int> all;
        for (unsigned n = 0; n < StreamPager::stream_count; ++n) all.push_back(pager.open(part, 0, false, 1.0f));
        for (int x : all) CHECK(x >= 0);
        CHECK(pager.open(part, 0, false, 1.0f) == -1);
        for (int x : all) pager.close(x);
        while (pager.pump()) {}
        CHECK(pager.open(part, 0, false, 1.0f) >= 0);
    }
    pin_log = nullptr;
}

// Song's clip jobs live on printed buttons: which glow, what undo keeps, and B1-B3 staying the page's under a latched Note Repeat.
static void song_buttons() {
    auto project = std::make_unique<Project>();
    using B = SurfaceButton;
    auto& song = project->song;
    CHECK(song.place({2, 0, 4, 4, false}));
    UiState ui; ui.page = PageId::Song; ui.track = 2;
    auto job = [&](B b) { return button_has_job(*project, ui, b, false, false); };
    ui.song_cursor = 0; // empty time
    CHECK(!job(B::Chop) && !job(B::Erase) && !job(B::NoteRepeat) && job(B::Loop));
    ui.song_cursor = 4; // the clip's first bar: nothing to split off
    CHECK(!job(B::Chop) && job(B::Erase) && !job(B::NoteRepeat));
    ui.song_cursor = 6;
    CHECK(job(B::Chop) && job(B::Erase));
    CHECK(song.next_marker(0) == 0);
    CHECK(song.step_marker(8) && song.next_marker(4) == 8 && !job(B::NoteRepeat)); // the marker is where the clip ends
    CHECK(song.step_marker(16) && song.step_marker(12) && song.next_marker(8) == 12);
    song.markers[size_t(song.marker_at(8))].bar = 2; // now the next is past the clip's end
    CHECK(song.next_marker(4) == 12 && job(B::NoteRepeat));
    ui.song_export = true; // the export sheet: the page's rules, not the clip's
    CHECK(!job(B::Erase));
    ui.song_export = false;
    CHECK(press_scope(*project, ui, B::Loop, false).kind == UndoScope::Kind::Song);
    CHECK(press_scope(*project, ui, B::NoteRepeat, false).kind == UndoScope::Kind::Song);
    CHECK(press_scope(*project, ui, B::Chop, false).kind == UndoScope::Kind::Song);
    CHECK(press_scope(*project, ui, B::Erase, false).kind == UndoScope::Kind::Song);
    ui.shift = true; // Shift + Loop stays Reverse, of the last pad
    CHECK(press_scope(*project, ui, B::Loop, false).kind != UndoScope::Kind::Song);
    ui.shift = false;
    project->tracks[2].arp = true;
    CHECK(!repeat_keys(*project, ui));
    ui.page = PageId::Patterns;
    CHECK(repeat_keys(*project, ui));
}

// A pad holding an instrument: zones by key and velocity, as the pad's own sound in every other way.
struct ZoneRig {
    std::vector<std::vector<float>> audio;
    std::shared_ptr<Multisample> multi = std::make_shared<Multisample>();
    SampleData data;
    SamplerEngine::Table table {};
    std::unique_ptr<SamplerEngine> engine = std::make_unique<SamplerEngine>();
    PadSound pad;
    void part(std::vector<float> frames) { audio.push_back(std::move(frames)); }
    SampleZone& zone(uint16_t which = 0) { multi->zones.emplace_back(); multi->zones.back().part = which; return multi->zones.back(); }
    void build(PadPlay play = PadPlay::OneShot) {
        multi->parts = std::make_unique<ZonePart[]>(audio.size());
        multi->part_count = unsigned(audio.size());
        for (size_t n = 0; n < audio.size(); ++n) {
            auto& p = multi->parts[n];
            p.left = p.right = audio[n].data();
            p.total = uint32_t(audio[n].size());
            p.pins.assign((p.total - 1) / stream_chunk + 1, 0);
            p.frames.store(p.total);
        }
        multi->turns.assign(multi->zones.size(), 0);
        data.multi = multi;
        table[3] = &data;
        engine->set_samples(&table);
        pad.sample = 3; pad.play = play;
        engine->set_pad(0, pad);
    }
    std::vector<float> run(unsigned frames) {
        std::vector<float> out(frames);
        std::vector<float> right(frames);
        for (unsigned done = 0; done < frames; done += 128) {
            unsigned n = std::min(128u, frames - done);
            std::fill(out.begin() + done, out.begin() + done + n, 0.0f);
            std::fill(right.begin() + done, right.begin() + done + n, 0.0f);
            engine->render_stereo(out.data() + done, right.data() + done, n);
        }
        return out;
    }
    void key(int semitones, int velocity = 100) { engine->note_on(int(sampler_first_note) | (uint8_t(int8_t(semitones)) << 8), velocity); }
    void off(int semitones) { engine->note_off(int(sampler_first_note) | (uint8_t(int8_t(semitones)) << 8)); }
};

std::vector<float> zone_tone(double hz, float level, unsigned frames) {
    std::vector<float> out(frames);
    for (unsigned n = 0; n < frames; ++n) out[n] = level * float(std::sin(2 * 3.14159265358979 * hz * n / dsp::model_rate));
    return out;
}
double zone_rms(const std::vector<float>& x, size_t from, size_t to) {
    double sum = 0;
    for (size_t n = from; n < to; ++n) sum += double(x[n]) * x[n];
    return std::sqrt(sum / double(std::max<size_t>(to - from, 1)));
}
double zone_hz(const std::vector<float>& x, size_t from, size_t to) {
    unsigned rises = 0;
    for (size_t n = from + 1; n < to; ++n) rises += x[n - 1] < 0 && x[n] >= 0;
    return rises * double(dsp::model_rate) / double(to - from);
}

void sampler_zones() {
    { // a key plays its zone in tune, whichever zone it is
        ZoneRig r;
        r.part(zone_tone(220, 0.5f, 44100)); r.part(zone_tone(880, 0.5f, 44100));
        auto& low = r.zone(0); low.low_key = 0; low.high_key = 65; low.root = 60;
        auto& high = r.zone(1); high.low_key = 66; high.high_key = 127; high.root = 72;
        r.build();
        r.key(0);
        CHECK(std::fabs(zone_hz(r.run(8000), 800, 8000) - 220) < 8);
        r.engine->silence();
        r.key(5); // key 65, up five semitones from its root
        CHECK(std::fabs(zone_hz(r.run(8000), 800, 8000) - 220 * std::pow(2.0, 5 / 12.0)) < 8);
        r.engine->silence();
        r.key(12); // key 72: the other zone, at its own pitch
        CHECK(std::fabs(zone_hz(r.run(8000), 800, 8000) - 880) < 12);
    }
    { // the pad's Semi moves the key, its Fine tunes every zone
        ZoneRig r;
        r.part(zone_tone(440, 0.5f, 44100));
        r.zone(0).root = 60;
        r.pad.pitch = 12;
        r.build();
        r.key(0);
        CHECK(std::fabs(zone_hz(r.run(8000), 800, 8000) - 880) < 12);
    }
    { // a keytrack of nothing stays at its pitch; a zone's tune is its own
        ZoneRig r;
        r.part(zone_tone(440, 0.5f, 44100));
        auto& z = r.zone(0); z.keytrack = 0; z.tune = 1200;
        r.build();
        r.key(7);
        CHECK(std::fabs(zone_hz(r.run(8000), 800, 8000) - 880) < 12);
    }
    { // a velocity plays its layer
        ZoneRig r;
        r.part(zone_tone(440, 0.1f, 44100)); r.part(zone_tone(440, 0.8f, 44100));
        auto& soft = r.zone(0); soft.high_velocity = 64;
        auto& loud = r.zone(1); loud.low_velocity = 65;
        r.pad.velocity_sense = 0;
        r.build();
        r.key(0, 40); auto a = r.run(4000); r.engine->silence();
        r.key(0, 100); auto b = r.run(4000);
        CHECK(zone_rms(b, 500, 4000) > 6 * zone_rms(a, 500, 4000));
    }
    { // round robin takes turns; every zone a press reaches plays
        ZoneRig r;
        r.part(zone_tone(440, 0.1f, 44100)); r.part(zone_tone(440, 0.8f, 44100));
        auto& one = r.zone(0); one.sequence_length = 2; one.sequence = 1;
        auto& two = r.zone(1); two.sequence_length = 2; two.sequence = 2;
        r.pad.velocity_sense = 0;
        r.build();
        std::vector<double> levels;
        for (unsigned n = 0; n < 4; ++n) { r.key(0); levels.push_back(zone_rms(r.run(3000), 500, 3000)); r.engine->silence(); }
        CHECK(levels[1] > 5 * levels[0] && levels[3] > 5 * levels[2] && std::fabs(levels[0] - levels[2]) < 1e-3);
    }
    { // layers add
        ZoneRig r;
        r.part(zone_tone(440, 0.3f, 44100));
        r.zone(0); r.zone(0);
        r.pad.velocity_sense = 0;
        r.build();
        r.key(0); auto both = r.run(3000); r.engine->silence();
        ZoneRig one;
        one.part(zone_tone(440, 0.3f, 44100));
        one.zone(0);
        one.pad.velocity_sense = 0;
        one.build();
        one.key(0); auto alone = one.run(3000);
        CHECK(std::fabs(zone_rms(both, 500, 3000) / zone_rms(alone, 500, 3000) - 2.0) < 0.05);
    }
    { // a press of a group cuts what is off by it
        ZoneRig r;
        r.part(zone_tone(440, 0.5f, 44100)); r.part(zone_tone(880, 0.5f, 44100));
        auto& open = r.zone(0); open.low_key = 60; open.high_key = 60; open.root = 60; open.off_by = 1;
        auto& shut = r.zone(1); shut.low_key = 61; shut.high_key = 61; shut.root = 61; shut.group = 1;
        r.build();
        r.key(0); r.run(2000);
        r.key(1);
        auto after = r.run(6000);
        CHECK(std::fabs(zone_hz(after, 2000, 6000) - 880) < 15);
    }
    { // a loop holds while the note does, and until let go goes on to the end after
        ZoneRig r;
        r.part(zone_tone(441, 0.5f, 4410));
        auto& z = r.zone(0); z.loop = ZoneLoop::On; z.loop_start = 1000; z.loop_end = 3000;
        r.pad.release = 0;
        r.build(PadPlay::Gate);
        r.key(0);
        CHECK(zone_rms(r.run(44100), 40000, 44100) > 0.2); // long past the sample's length
        r.off(0);
        CHECK(zone_rms(r.run(8000), 6000, 8000) < 1e-3);
        ZoneRig held;
        held.part(zone_tone(441, 0.5f, 8820));
        auto& h = held.zone(0); h.loop = ZoneLoop::UntilReleased; h.loop_start = 1000; h.loop_end = 3000;
        held.pad.decay = 100; held.pad.sustain = 100; held.pad.release = 100;
        held.build(PadPlay::Gate);
        held.key(0);
        held.run(44100);
        held.off(0);
        auto tail = held.run(44100);
        CHECK(zone_rms(tail, 0, 2000) > 0.1); // sounding through the release...
        CHECK(!held.engine->sounding() || zone_rms(tail, 12000, 13000) == 0.0); // ...and gone once past the sample's end
    }
    { // a loop may end at the very end of its file
        ZoneRig r;
        r.part(zone_tone(441, 0.5f, 4000));
        auto& z = r.zone(0); z.loop = ZoneLoop::On; z.loop_start = 2000; z.loop_end = 4000;
        r.build(PadPlay::Gate);
        r.key(0);
        CHECK(zone_rms(r.run(30000), 25000, 30000) > 0.2);
    }
    { // the seam: a loop of whole cycles at any pitch has no step at the join
        ZoneRig r;
        r.part(zone_tone(441, 0.5f, 8000));
        auto& z = r.zone(0); z.loop = ZoneLoop::On; z.loop_start = 1000; z.loop_end = 1000 + 1000; // ten cycles
        r.build(PadPlay::Gate);
        r.key(7); // a fifth up: the position lands between frames at every join
        auto out = r.run(40000);
        double jump = 0;
        for (size_t n = 2000; n < out.size(); ++n) jump = std::max(jump, double(std::fabs(out[n] - out[n - 1])));
        double expected = 0.5 * 0.7071 * 2 * 3.14159265 * 441 * std::pow(2.0, 7 / 12.0) / 44100;
        CHECK(jump < expected * 1.15);
    }
    { // a zone's own envelope is off the pad's
        ZoneRig r;
        r.part(zone_tone(441, 0.5f, 44100 * 2));
        auto& z = r.zone(0); z.release = 60;
        r.pad.release = 10;
        r.build(PadPlay::Gate);
        r.key(0); r.run(2000); r.off(0);
        auto slow = r.run(8000);
        ZoneRig fast;
        fast.part(zone_tone(441, 0.5f, 44100 * 2));
        fast.zone(0);
        fast.pad.release = 10;
        fast.build(PadPlay::Gate);
        fast.key(0); fast.run(2000); fast.off(0);
        auto quick = fast.run(8000);
        CHECK(zone_rms(slow, 4000, 8000) > 5 * zone_rms(quick, 4000, 8000));
    }
    { // a zone's filter is its own, and the pad's cutoff still moves it
        auto heard = [](int cutoff, float hz) {
            ZoneRig r;
            r.part(zone_tone(4000, 0.5f, 44100));
            auto& z = r.zone(0);
            if (hz > 0) { z.filter = PadFilter::Lpf2; z.cutoff = hz; }
            r.pad.cutoff = uint8_t(cutoff);
            r.build();
            r.key(0);
            return zone_rms(r.run(8000), 2000, 8000);
        };
        CHECK(heard(100, 400) < 0.2 * heard(100, 0));
        CHECK(heard(100, 16000) > 0.8 * heard(100, 0));
        CHECK(heard(50, 16000) < 0.5 * heard(100, 16000));
    }
    { // one note of a chord ends alone
        ZoneRig r;
        r.part(zone_tone(441, 0.5f, 44100 * 3));
        r.zone(0);
        r.pad.release = 0;
        r.pad.velocity_sense = 0;
        r.build(PadPlay::Gate);
        r.key(4); r.key(7);
        auto both = r.run(4000);
        r.off(7);
        r.run(4000);
        auto one = r.run(4000);
        CHECK(zone_rms(one, 0, 4000) > 0.2 && zone_rms(one, 0, 4000) < 0.85 * zone_rms(both, 1000, 4000));
        CHECK(std::fabs(zone_hz(one, 0, 4000) - 441 * std::pow(2.0, 4 / 12.0)) < 8);
    }
    { // silent until its file lands
        ZoneRig r;
        r.part(zone_tone(441, 0.5f, 44100));
        r.zone(0);
        r.build();
        r.multi->parts[0].frames.store(0);
        r.key(0);
        CHECK(zone_rms(r.run(4000), 0, 4000) == 0.0 && !r.engine->sounding());
    }
    { // a muted pad, and the pad's level, as any pad
        ZoneRig r;
        r.part(zone_tone(441, 0.5f, 44100));
        r.zone(0);
        r.pad.velocity_sense = 0;
        r.build();
        r.engine->set_pad_mute(0, true);
        r.key(0);
        CHECK(zone_rms(r.run(2000), 0, 2000) == 0.0);
        r.engine->set_pad_mute(0, false);
        r.key(0);
        double full = zone_rms(r.run(2000), 500, 2000);
        r.engine->silence();
        PadSound quiet = r.pad; quiet.level = 50;
        r.engine->set_pad(0, quiet);
        r.key(0);
        CHECK(zone_rms(r.run(2000), 500, 2000) < 0.5 * full);
    }
    { // streamed, it is the same frames as held; starved, a gap with nothing read past what is safe
        pin_log = nullptr; pin_fails = false;
        const unsigned frames = stream_chunk * 6;
        auto held = [&](ZoneRig& r) { r.part(zone_tone(441, 0.5f, frames)); auto& z = r.zone(0); z.start = 100; r.build(); };
        ZoneRig a, b;
        held(a); held(b);
        StreamPager pager(logged_pin);
        b.engine->set_pager(&pager);
        a.key(0); b.key(0);
        while (pager.pump()) {}
        for (unsigned n = 0; n < frames / 128; ++n) {
            auto x = a.run(128), y = b.run(128);
            for (unsigned f = 0; f < 128; ++f) CHECK(x[f] == y[f]);
            while (pager.pump()) {}
        }
        CHECK(!b.engine->sounding() && pager.underruns() == 0); // and its stream was let go at its end
        for (unsigned n = 0; n < StreamPager::stream_count; ++n) CHECK(pager.open(b.multi->parts[0], 0, false, 1.0f) >= 0);

        ZoneRig r;
        r.part(zone_tone(441, 0.5f, frames));
        for (size_t n = stream_chunk; n < frames; ++n) r.audio[0][n] = std::numeric_limits<float>::quiet_NaN(); // what must not be read
        r.zone(0);
        r.build();
        StreamPager slow(logged_pin);
        r.engine->set_pager(&slow);
        r.key(0);
        auto out = r.run(stream_chunk * 3); // never pumped
        double tail = 0;
        for (auto x : out) { CHECK(std::isfinite(x)); }
        tail = zone_rms(out, stream_chunk * 2, stream_chunk * 3);
        CHECK(tail == 0.0 && zone_rms(out, 1000, stream_chunk - 1000) > 0.2);
        CHECK(slow.underruns() == 1);
        for (size_t n = stream_chunk; n < frames; ++n) r.audio[0][n] = 0.25f; // the card catches up
        while (slow.pump()) {}
        CHECK(zone_rms(r.run(4000), 500, 4000) > 0.05);
    }
    { // a card gone: the voice stops and the note is not stuck
        pin_fails = true;
        ZoneRig r;
        r.part(zone_tone(441, 0.5f, stream_chunk * 4));
        r.zone(0);
        r.build();
        StreamPager gone(logged_pin);
        r.engine->set_pager(&gone);
        r.key(0);
        while (gone.pump()) {}
        r.run(stream_chunk * 2);
        CHECK(!r.engine->sounding());
        pin_fails = false;
    }
    { // more voices than streams: what is held plays, and nothing else is read
        ZoneRig r;
        r.part(zone_tone(441, 0.5f, stream_chunk * 3));
        for (size_t n = stream_chunk; n < r.audio[0].size(); ++n) r.audio[0][n] = std::numeric_limits<float>::quiet_NaN();
        r.zone(0);
        r.build();
        StreamPager full(logged_pin);
        for (unsigned n = 0; n < StreamPager::stream_count; ++n) full.open(r.multi->parts[0], 0, false, 1.0f);
        r.engine->set_pager(&full);
        r.key(0);
        auto out = r.run(stream_chunk * 2);
        for (auto x : out) CHECK(std::isfinite(x));
        CHECK(zone_rms(out, 1000, stream_chunk - 1000) > 0.2 && !r.engine->sounding());
    }
}

// A note's pitch: a chord on one pad, kept through steps and files, older files reading as before.
void note_pitches() {
    { // a FLAC's loop, kept from the WAV it was made of
        namespace fs = std::filesystem;
        auto path = (fs::temp_directory_path() / ("melodee-loop-" + std::to_string(getpid()) + ".flac")).string();
        auto write = [&](bool looped) {
            std::string b = "fLaC";
            auto block = [&](uint8_t type, const std::string& body, bool last) {
                b += char(type | (last ? 0x80 : 0)); b += char(body.size() >> 16); b += char(body.size() >> 8); b += char(body.size()); b += body;
            };
            auto le = [](uint32_t v) { return std::string {char(v), char(v >> 8), char(v >> 16), char(v >> 24)}; };
            block(0, std::string(34, 'x'), false);
            block(2, "riff" + std::string("RIFF") + le(1000) + "WAVE", false); // the file's other foreign chunks
            std::string data(60, '\0');
            if (looped) { data.replace(28, 4, le(1)); data.replace(44, 4, le(319390)); data.replace(48, 4, le(322438)); }
            block(2, "riffsmpl" + le(60) + data, false);
            block(1, std::string(100, '\0'), true);
            std::ofstream(path, std::ios::binary) << b;
        };
        WavInfo info;
        write(true);
        CHECK(read_flac_loop(path, info) && info.loop_start == 319390 && info.loop_end == 322439);
        write(false);
        WavInfo none;
        CHECK(!read_flac_loop(path, none) && none.loop_end == -1);
        { std::ofstream(path, std::ios::binary) << "RIFFnotflac"; }
        CHECK(!read_flac_loop(path, none));
        fs::remove(path);
    }
    CHECK(pad_note(40, 0) == 40 && (pad_note(40, 5) & 0xff) == 40 && int8_t(pad_note(40, -7) >> 8) == -7);
    Pattern p;
    Event a; a.tick = 0; a.key = 40; a.pitch = 4;
    Event b = a; b.pitch = 7;
    CHECK(p.add(a) && p.add(b) && p.events.size() == 2 && p.events[0].pitch == 4 && p.events[1].pitch == 7);
    Event again = a; again.velocity = 33;
    CHECK(p.add(again) && p.events.size() == 2 && p.events[0].velocity == 33); // the same note replaced, not a third
    Step st = p.step(0);
    p.set_step(0, st);
    CHECK(p.events.size() == 2 && p.events[0].pitch == 4 && p.events[1].pitch == 7); // edited as steps, they keep their pitch
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    bank->at[2][1].length = 96;
    bank->at[2][1].events = p.events;
    auto bytes = save_project(*project, *bank);
    auto back = std::make_unique<Project>();
    auto read = std::make_unique<PatternBank>();
    CHECK(load_project(bytes, *back, *read).empty());
    const auto& got = read->at[2][1].events;
    CHECK(got.size() == 2 && got[0].pitch == 4 && got[1].pitch == 7);
    { // a file with no PTCH: every pitch stays 0
        Event plain; plain.key = 41;
        bank->at[2][1].events = {plain};
        auto old = std::make_unique<PatternBank>();
        CHECK(load_project(save_project(*project, *bank), *back, *old).empty());
        CHECK(old->at[2][1].events.size() == 1 && old->at[2][1].events[0].pitch == 0);
        auto tag = save_project(*project, *bank);
        CHECK(std::search(tag.begin(), tag.end(), "PTCH", "PTCH" + 4) == tag.end());
    }
    SampleRef ref;
    std::strcpy(ref.path, "/media/card/Piano/Grand.SFZ");
    CHECK(ref.instrument());
    std::strcpy(ref.path, "/media/card/Piano/Grand.wav");
    CHECK(!ref.instrument());
    std::strcpy(ref.path, "/media/card/Keys/Rhodes.XPM");
    CHECK(ref.instrument());
    std::strcpy(ref.path, "/media/card/Keys/Rhodes.xpj");
    CHECK(!ref.instrument());
    std::strcpy(ref.path, "/media/card/Samples/Boys Choir/Boys Choir.exs"); // held while held, as an SFZ is
    CHECK(ref.instrument());
    std::strcpy(ref.path, "/media/card/Kits/Kit-Lofi Patch.xpm#12");
    CHECK(ref.instrument());
    std::strcpy(ref.path, "/media/card/Projects/Cycle 86bpm.xpj#3.16");
    CHECK(ref.instrument());
    std::strcpy(ref.path, "/media/card/Kits/Kick #1.wav");
    CHECK(!ref.instrument());
    std::strcpy(ref.path, "/media/card/Expansions/Organs/Organ-Farfisa VO Miller.xty");
    CHECK(ref.instrument());
    std::strcpy(ref.path, "/media/card/Expansions/Kits/Kit.XTY#5");
    CHECK(ref.instrument());
    Track kit; kit.engine = EngineKind::Sampler;
    CHECK(note_key(kit, 20) == -1 && note_key(kit, 20, true) == 20 && note_key(kit, 127, true) == 127 && note_key(kit, 128, true) == -1);
}

// The place bar (shell/place_bar.h): segments, place and tone from the snapshot, and its Settings row.
// The voices' taps (engine/voice_tap.h) and the screen's heads (shell/playheads.h): where a voice stands after each
// block, forwards, reversed and round a loop, its level as it is let go or cut, placed where it is heard.
void voice_taps() {
    SampleData data;
    data.frames = 44100;
    data.left.assign(data.frames, 0.5f); data.right.assign(data.frames, 0.5f);
    SamplerEngine::Table table {};
    table[2] = &data;
    std::vector<float> l(128, 0), r(128, 0);
    auto block = [&](SamplerEngine& e) { std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f); e.render_stereo(l.data(), r.data(), 128); };
    auto tapped = [&](const SamplerEngine& e) { TrackTaps t; t.track = 0; e.taps(t); return t; };
    const double rate = dsp::model_rate / dsp::sample_rate(); // file frames per output frame at the pad's own pitch

    // Forwards: the tap advances a block at a time; heard a latency earlier, the head is that far behind.
    {
        SamplerEngine e;
        e.set_samples(&table);
        PadSound pad; pad.sample = 2; pad.start = 1000; pad.end = 20000;
        e.set_pad(4, pad);
        e.note_on(int(sampler_first_note) + 4, 100);
        block(e);
        auto a = tapped(e);
        CHECK(a.count == 1 && a.voice[0].pad == 4 && a.voice[0].slot == 2);
        CHECK(a.voice[0].from == 1000 && a.voice[0].to == 20000 && a.voice[0].step > 0);
        CHECK(std::abs(double(a.voice[0].at) - (1000 + 128 * rate)) < 2);
        CHECK(a.voice[0].age == 128 && a.voice[0].level > 0.9f && !(a.voice[0].flags & tap::Loop));
        for (unsigned b = 0; b < 9; ++b) block(e);
        auto b = tapped(e);
        CHECK(b.voice[0].serial == a.voice[0].serial);
        CHECK(std::abs(double(b.voice[0].at) - (1000 + 1280 * rate)) < 2);
        HeardPlace h = heard_at(b.voice[0], -384);
        CHECK(h.begun && std::abs(h.at - (1000 + (1280 - 384) * rate)) < 2);
        CHECK(!heard_at(a.voice[0], -384).begun); // the first block is not heard yet a latency later
        CHECK(heard_at(b.voice[0], 1e6).at <= 20000); // a one shot holds at its end
        // A second hit is the newest tap.
        e.note_on(int(sampler_first_note) + 4, 100);
        block(e);
        auto c = tapped(e);
        CHECK(c.count == 2 && c.voice[0].serial > c.voice[1].serial && c.voice[0].at < c.voice[1].at);
    }
    // Reversed: the step is negative and the tap runs down from the region's end.
    {
        SamplerEngine e;
        e.set_samples(&table);
        PadSound pad; pad.sample = 2; pad.start = 1000; pad.end = 20000; pad.reverse = true;
        e.set_pad(0, pad);
        e.note_on(int(sampler_first_note), 100);
        block(e);
        auto a = tapped(e);
        for (unsigned b = 0; b < 4; ++b) block(e);
        auto b = tapped(e);
        CHECK(a.voice[0].step < 0 && b.voice[0].at < a.voice[0].at && a.voice[0].at < 20000 && a.voice[0].at > 19000);
        CHECK(heard_at(b.voice[0], -256).at > b.voice[0].at);
    }
    // A loop: it goes round from the loop point, the block it does so says Wrapped, and the head heard a latency
    // earlier is found before the turn, near the end.
    {
        SamplerEngine e;
        e.set_samples(&table);
        PadSound pad; pad.sample = 2; pad.start = 0; pad.end = 2000; pad.loop = true; pad.loop_lock = false; pad.loop_start = 1200;
        pad.play = PadPlay::Gate;
        e.set_pad(1, pad);
        e.note_on(int(sampler_first_note) + 1, 100);
        bool wrapped = false;
        VoiceTap after {};
        for (unsigned b = 0; b < 60 && !wrapped; ++b) {
            block(e);
            auto t = tapped(e);
            CHECK(t.count == 1 && (t.voice[0].flags & tap::Loop) && t.voice[0].loop_at == 1200);
            if (t.voice[0].flags & tap::Wrapped) { wrapped = true; after = t.voice[0]; }
        }
        CHECK(wrapped && after.at >= 1200 && after.at < 1200 + 128 * rate + 2);
        HeardPlace h = heard_at(after, -double(after.at - 1200) / rate - 20); // 20 frames before it went round
        CHECK(h.at > 1950 && h.at < 2000 && h.pass == 0);
        HeardPlace on = heard_at(after, 0);
        CHECK(on.pass == 1 && std::abs(on.at - after.at) < 1);
        block(e);
        CHECK(!(tapped(e).voice[0].flags & tap::Wrapped)); // only the block it went round in
        // Reversed loops stay inside loop point .. end.
        VoiceTap back = after; back.step = -float(rate); back.flags = tap::Loop;
        HeardPlace rb = heard_at(back, 2000);
        CHECK(rb.at >= 1200 && rb.at < 2000);
    }
    // Let go: a gate's tap says Releasing and its level falls; a mute group's cut says Choked and it is gone in ms.
    {
        SamplerEngine e;
        e.set_samples(&table);
        PadSound pad; pad.sample = 2; pad.play = PadPlay::Gate; pad.release = 40; pad.group = 3;
        e.set_pad(0, pad); e.set_pad(1, pad);
        e.note_on(int(sampler_first_note), 100);
        for (unsigned b = 0; b < 4; ++b) block(e);
        float held = tapped(e).voice[0].level;
        e.note_off(int(sampler_first_note));
        block(e);
        auto t = tapped(e);
        CHECK((t.voice[0].flags & tap::Releasing) && t.voice[0].level < held);
        for (unsigned b = 0; b < 8; ++b) block(e);
        CHECK(tapped(e).count == 0 || tapped(e).voice[0].level < t.voice[0].level);
        e.note_on(int(sampler_first_note), 100);
        for (unsigned b = 0; b < 2; ++b) block(e);
        e.note_on(int(sampler_first_note) + 1, 100); // the same group: pad 1 cuts pad 0
        auto cut = tapped(e);
        CHECK(cut.count >= 2);
        block(e);
        auto after = tapped(e);
        bool gone = true;
        for (unsigned i = 0; i < after.count; ++i) if (after.voice[i].pad == 0) gone = after.voice[i].level < 0.1f;
        CHECK(gone && after.voice[0].pad == 1);
        CHECK(after.count == 1 || (after.voice[1].flags & tap::Choked) || after.voice[1].pad != 0);
    }
    // A cloud: its tap is where its grains come from, and the grains' places come with it.
    {
        SamplerEngine e;
        e.set_samples(&table);
        PadSound pad; pad.sample = 2; pad.play = PadPlay::Gate;
        e.set_pad(2, pad);
        e.set_param(sampler::Cloud, 1); e.set_param(sampler::Density, 80); e.set_param(sampler::Position, 50);
        e.note_on(int(sampler_first_note) + 2, 100);
        for (unsigned b = 0; b < 20; ++b) block(e);
        auto t = tapped(e);
        CHECK(t.count == 1 && (t.voice[0].flags & tap::Cloud) && std::abs(int(t.voice[0].at) - 22050) < 200);
        CHECK(t.grain_count > 0 && t.grain_count <= TrackTaps::grains);
        // Each grain: the pad's, where it began near the cloud's place, reading on from there, newest first.
        bool whole = true;
        for (unsigned i = 0; i < t.grain_count; ++i) {
            const auto& g = t.grain[i];
            whole = whole && g.pad == 2 && g.slot == 2 && g.length > 0 && g.age <= g.length && g.at >= g.from && g.step > 0;
            whole = whole && std::abs(int(g.from) - 22050) < 2300 && (i == 0 || int32_t(t.grain[i - 1].serial - g.serial) > 0);
        }
        CHECK(whole);
        // Dense and long, more grains play than the taps carry: the newest sixteen.
        e.set_param(sampler::Density, 100); e.set_param(sampler::Size, 100);
        for (unsigned b = 0; b < 200; ++b) block(e);
        t = tapped(e);
        CHECK(t.grain_count == TrackTaps::grains && t.grain[0].age < t.grain[15].age);
    }
    // Chop's audition is tapped too, as pad sampler_pads.
    {
        SamplerEngine e;
        e.set_samples(&table);
        e.audition(2, 5000, 9000, 100);
        block(e);
        auto t = tapped(e);
        CHECK(t.count == 1 && t.voice[0].pad == int(sampler_pads) && t.voice[0].from == 5000);
    }
    // The board: what one side publishes the other reads whole.
    {
        TapBoard board;
        TrackTaps in, out;
        CHECK(board.read(out) && out.track == 255);
        in.track = 3; in.stamp = 123456789012ull; in.count = 1; in.voice[0].at = 777; in.voice[0].step = -1.5f; in.grain[15].at = 9; in.grain[15].pad = 7;
        board.publish(in);
        CHECK(board.read(out) && out.track == 3 && out.stamp == 123456789012ull && out.voice[0].at == 777 && out.voice[0].step == -1.5f && out.grain[15].at == 9 && out.grain[15].pad == 7);
    }
    // The host publishes the watched track's after each block, stamped with the block's end.
    {
        EngineHost host;
        std::vector<int32_t> out(256);
        host.watch_taps(2);
        host.render(out.data(), 128, 1);
        TrackTaps t;
        CHECK(host.taps(t) && t.track == 2 && t.stamp == 128 && t.count == 0);
        host.render(out.data(), 128, 1);
        CHECK(host.taps(t) && t.stamp == 256);
    }
    // The trail: a head shows once heard, keeps its wake while it sounds, and fades out 0.35 s after it has gone.
    {
        HeadTrail trail;
        TrackTaps t;
        t.track = 3; t.count = 1;
        auto& v = t.voice[0];
        v.serial = 5; v.pad = 8; v.slot = 4; v.from = 100; v.to = 10200; v.loop_at = 100; v.at = 10100; v.step = 1; v.level = 1; v.age = 10000;
        trail.update(t, -500, 1000);
        auto h = trail.heads();
        CHECK(h[0].pad == 8 && h[0].wake == 1 && std::abs(h[0].at - 9600) < 1e-6 && h[1].pad < 0);
        t.count = 0; t.stamp = 500; // gone from the engine: run out at its end (a cut fades: voice_heads)
        trail.update(t, -500, 1100);
        h = trail.heads();
        CHECK(h[0].pad == 8 && h[0].level == 0 && h[0].wake == 1);
        trail.update(t, -500, 1275);
        CHECK(std::abs(trail.heads()[0].wake - 0.5f) < 0.01f);
        trail.update(t, -500, 1450);
        CHECK(trail.heads()[0].pad < 0);
        // Silent while still tapped (a release run out) fades the same way; a new track starts afresh.
        t.count = 1; v.level = 0;
        trail.update(t, 0, 2000);
        trail.update(t, 0, 2175);
        CHECK(std::abs(trail.heads()[0].wake - 0.5f) < 0.01f);
        t.track = 4;
        t.count = 0;
        trail.update(t, 0, 2180);
        CHECK(trail.heads()[0].pad < 0);
    }
}

// Mock 4 and 7 (shell/playheads.h): a line along each sounding pad of the bank in view, its newest voice's; and the
// Samples row of the file previewing, how far, the time heard of its length, nothing once the cursor cut it.
void head_lines() {
    auto head = [](int pad, uint32_t serial, double at, float level, float step = 1) {
        PlayHead h;
        h.serial = serial; h.pad = int16_t(pad); h.slot = 3; h.from = 1000; h.to = 5000; h.loop_at = 1000;
        h.at = at; h.step = step; h.level = level; h.wake = 1;
        return h;
    };
    CHECK(std::abs(head_through(head(0, 1, 2000, 1)) - 0.25f) < 1e-6f);
    CHECK(std::abs(head_through(head(0, 1, 2000, 1, -1)) - 0.75f) < 1e-6f); // reversed: from the end
    CHECK(head_through(head(0, 1, 9000, 1)) == 1 && head_through(head(0, 1, 0, 1)) == 0);
    std::array<PlayHead, HeadTrail::size> heads {};
    heads[0] = head(18, 9, 3000, 0.8f);  // bank B, pad 3: newest
    heads[1] = head(18, 4, 4800, 0.2f);  // the same pad, older: not shown
    heads[2] = head(2, 3, 1400, 1);      // bank A: not in view
    heads[3] = head(16, 2, 1000, 0.5f);  // bank B pad 1, just begun: no length yet
    heads[4] = head(31, 1, 3000, 0.0f);  // gone silent, its wake still fading: no line
    heads[4].wake = 0.5f;
    auto lines = bank_lines(heads, 1);
    CHECK(std::abs(lines[2].through - 0.5f) < 1e-6f && std::abs(lines[2].strength - 0.8f) < 1e-6f);
    CHECK(lines[0].through == 0 && std::abs(lines[0].strength - 0.5f) < 1e-6f);
    CHECK(lines[15].strength == 0 && lines[1].strength == 0);
    CHECK(bank_lines(heads, 0)[2].strength == 1 && bank_lines(heads, 0)[2].through > 0);
    // The preview: the audition voice on the preview slot only.
    std::array<PlayHead, HeadTrail::size> pv {};
    pv[0] = head(128, 7, 1000 + 52920, 0.9f); pv[0].from = 1000; pv[0].to = 1000 + 137592; pv[0].slot = 64;
    pv[1] = head(128, 6, 2000, 1);           // another audition (Chop's): another slot
    PreviewPlace at = preview_place(pv, 128, 64, 44100);
    CHECK(at.shown && at.sounding && std::abs(at.heard - 1.2) < 1e-3 && std::abs(at.length - 3.12) < 1e-3);
    CHECK(std::abs(at.through - 52920.0f / 137592.0f) < 1e-5f && std::abs(at.strength - 0.9f) < 1e-6f);
    pv[0].level = 0; pv[0].wake = 0.4f; // ended: the row still fades, B3 no longer lit
    at = preview_place(pv, 128, 64, 44100);
    CHECK(at.shown && !at.sounding && std::abs(at.wake - 0.4f) < 1e-6f);
    pv[0].flags = tap::Choked; // the cursor moved on: nothing for the row
    CHECK(!preview_place(pv, 128, 64, 44100).shown);
    CHECK(!preview_place(std::array<PlayHead, HeadTrail::size> {}, 128, 64, 44100).shown);
}

// Mocks 2 and 3 (shell/playheads.h): a cut voice's head fades over its 60 ms choke and goes on moving, one run out
// at its end is no cut; a loop going round as heard leaves a ghost for 0.2 s; Stage takes the four newest voices of a
// pad, counting only those heard.
void voice_heads() {
    auto voice = [](VoiceTap& v, uint32_t serial, uint32_t at, float level) {
        v = VoiceTap {};
        v.serial = serial; v.pad = 8; v.slot = 4; v.from = 1000; v.to = 50000; v.loop_at = 1000;
        v.at = at; v.step = 1; v.level = level; v.age = at - 1000;
    };
    // Cut: gone from the engine mid-region while loud.
    {
        HeadTrail trail;
        TrackTaps t;
        t.track = 3; t.count = 1; t.stamp = 10000;
        voice(t.voice[0], 5, 20000, 0.8f);
        trail.update(t, -500, 1000);
        CHECK(!trail.heads()[0].cut && trail.heads()[0].sounding());
        t.count = 0; t.stamp = 10480; // the next block, the voice gone
        trail.update(t, -500, 1010);
        auto h = trail.heads()[0];
        CHECK(h.cut && !h.sounding() && std::abs(h.level - 0.8f) < 0.01f && h.wake == 1);
        CHECK(std::abs(h.at - (20000 + 480 - 500)) < 1e-6); // carried on from its last tap
        t.stamp = 11920;
        trail.update(t, -500, 1040);
        h = trail.heads()[0];
        CHECK(std::abs(h.level - 0.4f) < 0.01f && h.at > 20000);
        trail.update(t, -500, 1070);
        CHECK(trail.heads()[0].level == 0 && trail.heads()[0].wake == 1);
        trail.update(t, -500, 1070 + HeadTrail::wake_ms);
        CHECK(trail.heads()[0].pad < 0);
    }
    // A mute group's or mono's cut tapped while it fades (Choked): the level from before the cut, fading.
    {
        HeadTrail trail;
        TrackTaps t;
        t.track = 3; t.count = 1; t.stamp = 10000;
        voice(t.voice[0], 5, 20000, 0.9f);
        trail.update(t, 0, 1000);
        t.voice[0].flags = tap::Choked; t.voice[0].level = 0.1f; t.stamp = 10128; t.voice[0].at += 128;
        trail.update(t, 0, 1015);
        auto h = trail.heads()[0];
        CHECK(h.cut && std::abs(h.level - 0.9f) < 0.01f);
        trail.update(t, 0, 1045);
        CHECK(std::abs(trail.heads()[0].level - 0.45f) < 0.01f);
    }
    // Run out: a one shot gone at its region's end, or a release faded out, is no cut: its line goes at once.
    {
        HeadTrail trail;
        TrackTaps t;
        t.track = 3; t.count = 1; t.stamp = 10000;
        voice(t.voice[0], 5, 49800, 1.0f);
        trail.update(t, -300, 1000);
        t.count = 0; t.stamp = 10512;
        trail.update(t, -300, 1012);
        auto h = trail.heads()[0];
        CHECK(!h.cut && h.level == 0 && h.wake == 1 && h.at >= 49990);
        HeadTrail quiet;
        t.count = 1; t.stamp = 10000;
        voice(t.voice[0], 6, 20000, 0.01f); t.voice[0].flags = tap::Releasing;
        quiet.update(t, 0, 1000);
        t.count = 0; t.stamp = 10256;
        quiet.update(t, 0, 1010);
        CHECK(!quiet.heads()[0].cut && quiet.heads()[0].level == 0);
    }
    // A loop going round as heard: the ghost at the end it left, 1 falling to 0 over 0.2 s; the pass counted.
    {
        HeadTrail trail;
        TrackTaps t;
        t.track = 3; t.count = 1; t.stamp = 10000;
        auto& v = t.voice[0];
        voice(v, 5, 1990, 1); v.from = 0; v.to = 2000; v.loop_at = 1200; v.flags = tap::Loop; v.age = 1990;
        trail.update(t, 0, 1000);
        CHECK(trail.heads()[0].pass == 0 && trail.heads()[0].wrap == 0);
        v.at = 1250; v.age = 2050; v.flags = tap::Loop | tap::Wrapped; t.stamp = 10060;
        trail.update(t, 0, 1016);
        auto h = trail.heads()[0];
        CHECK(h.pass == 1 && h.wrap == 1 && wrap_left(h) == 2000);
        trail.update(t, 0, 1116);
        CHECK(std::abs(trail.heads()[0].wrap - 0.5f) < 0.01f && trail.heads()[0].pass == 1);
        trail.update(t, 0, 1216);
        CHECK(trail.heads()[0].wrap == 0);
        h.step = -1;
        CHECK(wrap_left(h) == 1200); // reversed: it leaves the loop point for the end
    }
    // Stage's voices of a pad: the four newest of its sample, the newest at its envelope, older ones at half;
    // the count only those heard (not cut, not silent), past the cap too; another sample or a cloud not at all.
    {
        std::array<PlayHead, HeadTrail::size> heads {};
        for (unsigned k = 0; k < 7; ++k) {
            auto& h = heads[k];
            h.serial = 20 - k; h.pad = 8; h.slot = 4; h.from = 0; h.to = 10000; h.at = 1000 * k; h.step = 1; h.level = 0.8f; h.wake = 1;
        }
        heads[1].cut = true; heads[1].level = 0.5f;
        heads[2].level = 0;  // silent, its wake fading
        heads[5].slot = 9;   // another sample on the pad
        heads[6].flags = tap::Cloud;
        PadVoices v = pad_voices(heads, 8, 4);
        CHECK(v.count == 4 && v.head[0] == &heads[0] && v.head[3] == &heads[3] && v.sounding == 3);
        CHECK(std::abs(voice_strength(heads[0], true) - 0.8f) < 1e-6f && std::abs(voice_strength(heads[3], false) - 0.4f) < 1e-6f);
        CHECK(pad_voices(heads, 9, 4).count == 0 && pad_voices(heads, 8, 9).sounding == 1);
    }
}

// Mock 5 (shell/playheads.h): the slice Chop's audition voice plays, found from its region; Manual's take is the
// whole region while it is heard, and no slice.
void chop_heads() {
    Slices sl;
    sl.count = 3; sl.start[0] = 1000; sl.start[1] = 5000; sl.start[2] = 9000; sl.last = 12000;
    PlayHead h;
    h.serial = 3; h.pad = 128; h.slot = 4; h.from = 5000; h.to = 9000; h.at = 6000; h.step = 1; h.level = 0.8f; h.wake = 1;
    CHECK(chop_sound(&h, sl, 20000, false).slice == 1 && !chop_sound(&h, sl, 20000, false).take);
    CHECK(chop_sound(nullptr, sl, 20000, false).slice == -1 && chop_sound(&h, Slices {}, 20000, false).slice == -1);
    h.from = 9000; h.to = 12000; // the last slice, up to `last`
    CHECK(chop_sound(&h, sl, 20000, false).slice == 2);
    h.from = 4000; h.to = 9000;  // a slice moved under it: none
    CHECK(chop_sound(&h, sl, 20000, false).slice == -1);
    h.from = 1000; h.to = 12000; // Manual's take over the whole region
    ChopSound take = chop_sound(&h, sl, 20000, true);
    CHECK(take.take && take.slice == -1);
    CHECK(!chop_sound(&h, sl, 20000, false).take);
    h.level = 0;                 // played out: no take, and no slice for it either
    take = chop_sound(&h, sl, 20000, true);
    CHECK(!take.take && take.slice == -1);
    sl.count = 1; h.level = 0.8f; // one slice, the whole region: the take while armed, else that slice
    CHECK(chop_sound(&h, sl, 12000, true).take && chop_sound(&h, sl, 12000, false).slice == 0);
}

// Mock 6 (engine/voice_tap.h, shell/playheads.h): a grain where it is heard, kept by the screen till its mark has
// faded 150 ms after its window closed; each at its own height; a pad's cloud with its grains newest first.
void cloud_grains() {
    GrainTap g;
    g.serial = 40; g.from = 1000; g.at = 1100; g.age = 100; g.length = 400; g.step = 1; g.pad = 8; g.slot = 4;
    HeardGrain h = heard_grain(g, -200); // a latency earlier: not heard yet
    CHECK(!h.begun && h.window == 0 && h.at == 1000);
    h = heard_grain(g, 100);             // half way: the top of its window
    CHECK(h.begun && !h.over && std::abs(h.window - 1.0f) < 1e-4f && std::abs(h.at - 1200) < 1e-9);
    h = heard_grain(g, 350);             // past its end: stopped where it stopped reading
    CHECK(h.over && h.window == 0 && std::abs(h.at - 1400) < 1e-9 && std::abs(h.after - 50) < 1e-9);
    g.step = 0.5f;
    CHECK(std::abs(heard_grain(g, 0).at - 1050) < 1e-9);
    g.step = 1;

    GrainTrail trail;
    TrackTaps t;
    t.track = 3; t.grain_count = 1; t.grain[0] = g;
    trail.update(t, -200, 1000);
    auto shown = [&] { unsigned n = 0; for (const auto& p : trail.grains()) n += p.shown(); return n; };
    CHECK(shown() == 0);                 // not heard yet
    trail.update(t, 100, 1010);
    auto first = [&] { for (const auto& p : trail.grains()) if (p.shown()) return p; return GrainPlace {}; };
    CHECK(shown() == 1 && first().live() && first().serial == 40 && std::abs(first().at - 1200) < 1e-9 && first().pad == 8);
    CHECK(std::abs(grain_alpha(first()) - 1.0f) < 1e-4f);
    // Gone from the engine: carried on from its last tap to its end, then a mark that fades over 150 ms.
    t.grain_count = 0; t.stamp = 300;
    trail.update(t, 100, 1020);
    CHECK(shown() == 1 && !first().live() && first().mark == 1 && std::abs(first().at - 1400) < 1e-9);
    CHECK(std::abs(grain_alpha(first()) - 0.3f) < 1e-6f);
    trail.update(t, 100, 1020 + GrainTrail::mark_ms / 2);
    CHECK(std::abs(first().mark - 0.5f) < 0.01f);
    trail.update(t, 100, 1020 + GrainTrail::mark_ms);
    CHECK(shown() == 0);
    // More grains than it keeps: the newest stay; another track starts afresh.
    for (uint32_t n = 0; n < 3; ++n) {
        t.grain_count = TrackTaps::grains;
        for (unsigned i = 0; i < TrackTaps::grains; ++i) { t.grain[i] = g; t.grain[i].serial = 100 + n * 16 + (15 - i); }
        trail.update(t, 0, 2000 + n);
    }
    unsigned newest = 0;
    for (const auto& p : trail.grains()) newest += p.shown() && p.serial >= 132;
    CHECK(newest == 16 && shown() == GrainTrail::size);
    t.track = 4; t.grain_count = 0;
    trail.update(t, 0, 2010);
    CHECK(shown() == 0);
    // HeadTrail keeps a GrainTrail of its own.
    HeadTrail heads;
    t.grain_count = 1; t.grain[0] = g;
    heads.update(t, 0, 3000);
    unsigned via = 0;
    for (const auto& p : heads.grains()) via += p.live();
    CHECK(via == 1);

    // Heights: the same for a grain every time, spread across, never outside 0..1.
    float lo = 1, hi = 0;
    bool steady = true;
    for (uint32_t n = 1; n < 200; ++n) { float a = grain_lane(n); lo = std::min(lo, a); hi = std::max(hi, a); steady = steady && a == grain_lane(n); }
    CHECK(steady && lo >= 0 && hi < 1 && hi - lo > 0.8f && grain_lane(7) != grain_lane(8));

    // A pad's cloud: its cloud voice's head (the centre), its grains newest first; others' left out.
    std::array<PlayHead, HeadTrail::size> hs {};
    hs[0].pad = 8; hs[0].slot = 4; hs[0].serial = 9; hs[0].at = 5000; hs[0].level = 0.7f; hs[0].wake = 1; hs[0].flags = tap::Cloud;
    hs[1].pad = 8; hs[1].slot = 4; hs[1].serial = 8; hs[1].at = 100; hs[1].level = 0.7f; hs[1].wake = 1; // a plain voice
    std::array<GrainPlace, GrainTrail::size> gs {};
    for (unsigned k = 0; k < 5; ++k) { gs[k].serial = 50 + k; gs[k].pad = 8; gs[k].slot = 4; gs[k].window = 0.5f; }
    gs[3].window = 0; gs[3].mark = 0.4f; // just gone
    gs[4].pad = 9;                        // another pad's
    CloudView v = cloud_view(hs, gs, 8, 4);
    CHECK(v.voice == &hs[0] && v.count == 4 && v.live == 3 && v.grains[0]->serial == 53 && v.grains[3]->serial == 50 && v.sounding());
    CHECK(cloud_view(hs, gs, 9, 4).voice == nullptr && cloud_view(hs, gs, 9, 4).count == 1 && cloud_view(hs, gs, 8, 5).count == 0);
    hs[0].level = 0; for (auto& p : gs) p.window = 0;
    CHECK(!cloud_view(hs, gs, 8, 4).sounding());
}

void place_bar() {
    using Tone = PlaceBar::Tone;
    auto s = std::make_unique<Snapshot>();
    const uint32_t bar = 16 * sixteenth_ticks, beat = 4 * sixteenth_ticks;
    // A 4-bar pattern playing: a segment a bar, beats as notches, green.
    s->pattern.length = 4 * bar; s->playing = true; s->position = 22 * sixteenth_ticks;
    PlaceBar b = place_bar_of(*s);
    CHECK(b.count == 4 && b.segments[0] == bar && b.total() == 4 * bar && b.notch == beat && b.position == 22 * sixteenth_ticks);
    CHECK(b.tone == Tone::Playing && !b.held && b.beat == beat);
    // Recording turns it red; a length not a whole number of bars ends on a short segment.
    s->ui.recording = true; s->pattern.length = bar + 4 * sixteenth_ticks;
    b = place_bar_of(*s);
    CHECK(b.tone == Tone::Recording && b.count == 2 && b.segments[1] == 4 * sixteenth_ticks && b.position == 22 * sixteenth_ticks % (bar + 4 * sixteenth_ticks));
    // 64 bars: no more than 16 segments, each of 4 bars, notched by the bar.
    s->pattern.length = 64 * bar;
    b = place_bar_of(*s);
    CHECK(b.count == 16 && b.segments[0] == 4 * bar && b.notch == bar);
    // Count-In: a block a beat (3/4 here), the ones still to come lit.
    s->count_in = 2; s->project.signature = 2;
    b = place_bar_of(*s);
    CHECK(b.tone == Tone::Waiting && b.blocks == 3 && b.blocks_left == 2);
    s->count_in = 0; s->project.signature = common_time;
    // Stopped: held where Shift + Play carries on from; Seq Record armed waits.
    s->playing = false; s->ui.recording = false; s->pattern.length = 4 * bar; s->ui.locate = 37 * sixteenth_ticks;
    b = place_bar_of(*s);
    CHECK(b.tone == Tone::Stopped && b.held && b.position == 37 * sixteenth_ticks);
    s->ui.rec_armed = true;
    CHECK(place_bar_of(*s).tone == Tone::Waiting);
    s->ui.rec_armed = false;
    // The Song: sections between its markers, the place in bars.
    s->playing = s->song_playing = true;
    Clip clip; clip.track = 0; clip.bar = 0; clip.bars = 12;
    CHECK(s->project.song.place(clip));
    s->project.song.markers[0].bar = 8; s->project.song.markers[1].bar = 4; s->project.song.marker_count = 2;
    s->song_bar = 5; s->ticks = 13 * bar + beat;
    b = place_bar_of(*s);
    CHECK(b.count == 3 && b.segments[0] == 4 * bar && b.segments[1] == 4 * bar && b.segments[2] == 4 * bar);
    CHECK(b.position == 5 * bar + beat && b.notch == bar && b.tone == Tone::Playing);
    s->song_playing = false;
    // The Looper page: the loop selected, its bars, red while it overdubs, amber while armed.
    s->ui.page = PageId::Looper; s->ui.loop = 1;
    auto& v = s->loops[1].view;
    v.state = looper::State::Overdub; v.ticks = 2 * bar; v.length = 1000; v.position = 750;
    b = place_bar_of(*s);
    CHECK(b.count == 2 && b.position == uint32_t(1.5 * bar) && b.tone == Tone::Recording);
    v.state = looper::State::Playing;
    CHECK(place_bar_of(*s).tone == Tone::Playing);
    v.state = looper::State::Armed;
    CHECK(place_bar_of(*s).tone == Tone::Waiting && place_bar_of(*s).held);
    // Settings: Top by default, Bottom kept in the text.
    Settings set;
    CHECK(!set.place_bottom && set.value_text(Settings::PlaceBarRow) == "Top");
    set.step(Settings::PlaceBarRow, 1);
    CHECK(set.place_bottom && set.value_text(Settings::PlaceBarRow) == "Bottom");
    Settings back; back.read(set.text());
    CHECK(back.place_bottom);
}


// Plug-ins (docs/MELODEE.md): the engine host plays whatever PluginRunner the rack offers an entry, as a PLUGIN
// track's instrument and as a chain's Plugin effect; projects keep the table, the tracks' entries and the states.
struct FakePlugin final : PluginRunner {
    bool instrument = true;
    int notes_on = 0, notes_off = 0, silenced = 0;
    unsigned rendered = 0;
    float level = 0;
    void midi(uint8_t status, uint8_t, uint8_t data2) override {
        if ((status & 0xf0) == 0x90 && data2) { ++notes_on; level = 0.25f; }
        if ((status & 0xf0) == 0x80) { ++notes_off; level = 0; }
    }
    void silence() override { ++silenced; level = 0; }
    void render(float* left, float* right, unsigned frames) override {
        rendered += frames;
        for (unsigned f = 0; f < frames; ++f) {
            if (instrument) { left[f] += level; right[f] += level; }
            else { left[f] *= 0.5f; right[f] *= 0.5f; } // an effect: half as loud
        }
    }
};

void plugins() {
    // The file: PLGT makes the track a PLUGIN track (TRAK says None, as older builds should see it), PLUG keeps
    // each entry's path and state.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        project->tracks[2].engine = EngineKind::Plugin;
        project->tracks[2].plugin = 5;
        std::snprintf(project->plugins[5].path, plugin_path_length, "/media/card/Plugins/Nekobi.vst3");
        std::snprintf(project->plugins[9].path, plugin_path_length, "/media/card/Plugins/sd88me - VST - Acid-303");
        std::snprintf(project->plugins[7].path, plugin_path_length, "/media/card/Plugins/lsp-plugins.vst3"); // one of a bundle's
        project->plugins[7].id = -123456789;
        std::snprintf(project->plugins[7].name, sizeof(project->plugins[7].name), "Compressor x2 Stereo");
        project->buses[0].fx.slots[0] = {plugin_effect_kind, false, {7, 0, 0, 0, 0, 0}};
        project->master.slots[0] = {plugin_effect_kind, false, {9, 0, 0, 0, 0, 0}};
        std::array<std::vector<uint8_t>, max_plugins> states;
        states[5] = {1, 2, 3, 250};
        auto data = save_project(*project, *bank, &states);
        auto copy = std::make_unique<Project>();
        auto copy_bank = std::make_unique<PatternBank>();
        std::array<std::vector<uint8_t>, max_plugins> read;
        CHECK(load_project(data, *copy, *copy_bank, nullptr, &read).empty());
        CHECK(copy->tracks[2].engine == EngineKind::Plugin && copy->tracks[2].plugin == 5);
        CHECK(std::string(copy->plugins[5].path) == "/media/card/Plugins/Nekobi.vst3");
        CHECK(std::string(copy->plugins[9].path) == "/media/card/Plugins/sd88me - VST - Acid-303");
        CHECK(read[5] == states[5] && read[9].empty() && !copy->plugins[4].used());
        CHECK(copy->plugins[7].id == -123456789 && std::string(copy->plugins[7].name) == "Compressor x2 Stereo" && copy->plugins[5].id == 0);
        CHECK(copy->master.slots[0].kind == plugin_effect_kind && copy->master.slots[0].params[0] == 9);
        CHECK(plugin_used_by(*copy, 5) && plugin_used_by(*copy, 9) && !plugin_used_by(*copy, 4));
        copy->tracks[2].engine = EngineKind::Prophet; // the entry stays in the table but nothing plays it
        CHECK(!plugin_used_by(*copy, 5));
    }
    // The host: a PLUGIN track with no plug-in yet is silent; once offered, notes reach it and it is heard; a
    // plug-in effect on the track's chain works on what the track plays; a new offer hands the old one back.
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    project->tracks[0].engine = EngineKind::Plugin;
    project->tracks[0].plugin = 3;
    EngineHost host;
    host.load(*project, *bank);
    std::vector<int32_t> out(256);
    auto peak = [&](unsigned blocks) {
        float most = 0;
        for (unsigned n = 0; n < blocks; ++n) {
            host.render(out.data(), 128, 1);
            for (auto v : out) most = std::max(most, std::fabs(v / 2147483648.0f));
        }
        return most;
    };
    host.send({Command::Type::NoteOn, 0, 0, 60, 100});
    CHECK(peak(4) == 0); // nothing offered yet
    FakePlugin synth;
    CHECK(host.offer_plugin(3, &synth));
    host.send({Command::Type::NoteOn, 0, 0, 60, 100});
    float loud = peak(4);
    CHECK(synth.notes_on == 1 && synth.rendered >= 4 * 128 && loud > 0.05f);
    // The effect: entry 7 on the track's chain halves it.
    FakePlugin half;
    half.instrument = false;
    host.send({Command::Type::FxKind, 0, 0, 0, int16_t(EffectKind::Plugin)});
    host.send({Command::Type::FxParam, 0, 0, 0, 7});
    CHECK(host.offer_plugin(7, &half));
    float halved = peak(4);
    CHECK(half.rendered >= 4 * 128 && std::fabs(halved - loud * 0.5f) < loud * 0.05f);
    // Bypassed, the effect is skipped.
    host.send({Command::Type::FxBypass, 0, 0, 0, 1});
    unsigned before = half.rendered;
    CHECK(std::fabs(peak(4) - loud) < loud * 0.05f && half.rendered == before);
    // Another plug-in for entry 3: the old one comes back to be freed and the track plays the new one.
    FakePlugin second;
    CHECK(host.offer_plugin(3, &second));
    peak(1);
    PluginRunner* back = nullptr;
    CHECK(host.take_retired_plugin(back) && back == &synth);
    host.send({Command::Type::NoteOn, 0, 0, 62, 100});
    peak(2);
    CHECK(second.notes_on == 1 && synth.notes_on == 1);
    // The track to another entry: it lets go of the plug-in it played.
    host.send({Command::Type::TrackPlugin, 0, 0, 0, 4});
    CHECK(peak(2) == 0 && second.silenced >= 1);
    // Entries let go of (nullptr) hand theirs back too.
    CHECK(host.offer_plugin(7, nullptr) && host.offer_plugin(3, nullptr));
    peak(1);
    unsigned handed = 0;
    while (host.take_retired_plugin(back)) handed += back == &half || back == &second;
    CHECK(handed == 2);
}

int main() {
    plugins();
    voice_taps();
    voice_heads();
    chop_heads();
    cloud_grains();
    head_lines();
    place_bar();
    song_buttons();
    sfz_reader();
    keygroup_reader();
    exs_reader();
    kit_levels_octave();
    drum_program_pads();
    expansion_paths();
    sample_cache();
    stream_pager();
    parallel_render();
    sampler_zones();
    note_pitches();
    track_source();
    zero_snap();
    looper_engine();
    roll();
    stage();
    midi_tracks();
    device_rates();
    card_access();
    projects_and_polish();
    midi_and_sync();
    mtc_in();
    fader_and_takeover();
    pad_fx_and_flex();
    performance_effects();
    audio_input_and_rendering();
    meter_lights();
    chop_and_browser();
    sample_mode();
    step_edit_and_pad_motion();
    timing();
    pad_expression();
    parity_1c();
    shift_pads();
    button_remap();
    parity_basics();
    history();
    names();
    settings();
    punches();
    more_engines();
    drums();
    helpers();
    project_files();
    recording();
    sampler_kit();
    effects();
    arrange();
    mod_matrix();
    motion();
    layouts();
    bindings();
    prophet_engine();
    host_sequencer();
    sequenced_legato();
    std::puts("melodee ok");
}
