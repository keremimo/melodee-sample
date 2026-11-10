// SPDX-License-Identifier: AGPL-3.0-only
// Melodee's model and audio side on the host: pad layouts, knob bindings,
// the PROPHET engine and the engine host's sequencer and mixer.
#include "melodee/engine/effects.h"
#include <functional>
#include "melodee/engine/sample_data.h"
#include "melodee/engine/sampler.h"
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
#include "melodee/shell/fader_job.h"
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
#include "melodee/shell/shift_pad_jobs.h"
#include "melodee/shell/mute_mode.h"
#include "melodee/shell/note_repeat.h"
#include "melodee/shell/pad_bank.h"
#include "melodee/shell/step_edit.h"
#include "melodee/model/pad_params.h"
#include "melodee/model/input_config.h"
#include "melodee/model/song_flatten.h"
#include "melodee/engine/recall_ring.h"
#include "melodee/engine/bounce.h"
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
        auto hear = [&](int how, int pitch_how, int follow, int semitones) {
            SamplerEngine engine;
            engine.set_samples(&tones);
            engine.set_tempo(1200);
            engine.set_param(sampler::Follow, follow); engine.set_param(sampler::FollowHow, how); engine.set_param(sampler::PitchHow, pitch_how);
            PadSound sound; sound.sample = 0; sound.own_tempo = 600; sound.pitch = int8_t(semitones); sound.decay = 100;
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
    for (unsigned n : {0u, 3u, 9u}) CHECK(shift_pad_job(n).kind == K::Toggle);
    CHECK(shift_pad_job(2).kind == K::Action && shift_pad_job(11).kind == K::Open && shift_pad_job(15).kind == K::Open);
    for (unsigned n : {1u, 5u, 6u}) CHECK(shift_pad_job(n).kind == K::Action); // Half Seq, Half / Double Speed (phase 2)
    CHECK(shift_pad_job(13).kind == K::Open);                                  // Time Correct (phase 2)
    CHECK(shift_pad_job(14).kind == K::Action);                                // Warp: Time stretch / Pitch (phase 4)
    CHECK(shift_pad_job(12).kind == K::Action);                                // Trim Sample (phase 5)
    CHECK(shift_pad_job(10).kind == K::Action);                                // Resample (phase 6)
    CHECK(shift_pad_job(4).kind == K::Open);                                   // Compressor (phase 7)
    CHECK(shift_pad_job(8).kind == K::Open);                                   // Fader (phase 8)
    CHECK(shift_pad_job(7).kind == K::Later && shift_pad_job(7).phase == 9);   // MIDI Config

    UiState ui;
    CHECK(!ui.settings.full_level() && !ui.settings.count_in && ui.settings.rec_quantize);
    CHECK(shift_pad_level(ui, 0) == 1 && shift_pad_level(ui, 9) == 2 && shift_pad_level(ui, 7) == 0 && shift_pad_level(ui, 11) == 1);
    ui.settings.pad_response = 2;
    ui.settings.toggle_full_level();
    CHECK(ui.settings.full_level() && ui.settings.velocity(5) == 127 && shift_pad_level(ui, 0) == 2);
    ui.settings.count_in = true; ui.settings.rec_quantize = false;
    Settings kept; kept.read(ui.settings.text()); // the toggles live in Melodee's settings file
    CHECK(kept.full_level() && kept.count_in && !kept.rec_quantize);
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
    // A full stack drops its oldest page.
    for (unsigned n = 0; n < 40; ++n) go(n % 2 ? PageId::Tempo : PageId::Project), go(PageId(unsigned(PageId::Samples) + n % 3));
    CHECK(nav.back_depth <= UiState::back_capacity);

    // Faint lights: Chop and Loop only with a kit, Erase where it does something.
    using B = SurfaceButton;
    UiState l; l.page = PageId::Stage; l.track = 0;
    CHECK(button_has_job(*project, l, B::Seq, false, false));
    CHECK(!button_has_job(*project, l, B::Chop, false, false) && !button_has_job(*project, l, B::Loop, false, false));
    CHECK(!button_has_job(*project, l, B::Erase, false, false) && button_has_job(*project, l, B::Erase, false, true));
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
    CHECK(!repeat_keys(*project, ui));
    project->tracks[0].arp = true;
    CHECK(repeat_keys(*project, ui));
    ui.page = PageId::Arp;
    CHECK(!repeat_keys(*project, ui)); // its own keys stay on Arp
    Arp arp; arp.top = int(sampler_last_key);
    arp.press(150, 90);
    int velocity = 0;
    CHECK(arp.next(velocity) == 150 && velocity == 90); // a kit pad in bank H repeats as itself

    // Mute mode: kit pads where the pads play the kit, tracks on Stage and Mix.
    ui.page = PageId::Pads;
    CHECK(mute_on_kit(ui, true) && !mute_on_kit(ui, false));
    ui.page = PageId::Stage;
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
    UiState held; held.pad_bank = true;
    CHECK(tracks_on_pads(held));
    project->tracks[1].engine = EngineKind::Prophet;
    held.track = 1;
    CHECK(button_has_shift_job(*project, held, SurfaceButton::Erase, false));
    held.pad_bank = false; held.copying = UiState::Copying::Tracks;
    CHECK(tracks_on_pads(held));

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

    // Double / Half Seq, Half / Double Speed.
    Pattern d;
    d.add({0, 24, 36, 100, 100, 0}); d.add({100, 24, 38, 100, 100, 0}); d.add({384, 24, 40, 100, 100, 0}); // the last is past the end
    CHECK(double_pattern(d, 128 * 384) && d.length == 768 && d.events.size() == 4 && d.events[2].tick == 384 && d.events[3].tick == 484);
    CHECK(!double_pattern(d, 768 + 1));
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
        CHECK(std::fabs(run(*deaf, 8).first - run(*engine_with(base), 0).first) >= 0 && run(*deaf, 8).first > 0.45f);
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
        CHECK(middle > 0.4f && fallen < middle * 0.3f);
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
        CHECK(run(*opened, 10, 2).second > 0.2f);
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
    CHECK(wild.input.source == InputSource::UsbR && wild.input.threshold == -96 && wild.input.length == rec_lengths - 1);

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

    // The project file: Knob FX and the compressor come back; older files (neither section) load them off.
    project->knob_fx.choice = 16; project->knob_fx.kind = uint8_t(EffectKind::AmpSim); project->knob_fx.params = {3, 70, 40, 2, -3, 5};
    project->knob_fx.on = true; project->knob_fx.bypass = true;
    project->knob_fx.set_track(4, true); project->knob_fx.set_pad(1, 70, true);
    project->compressor = {true, true, 12, 80, 66, 9};
    auto data = save_project(*project, *bank);
    auto back = std::make_unique<Project>();
    auto back_bank = std::make_unique<PatternBank>();
    CHECK(load_project(data, *back, *back_bank).empty());
    const auto& k = back->knob_fx;
    CHECK(!k.on && k.bypass && k.choice == 16 && k.kind == uint8_t(EffectKind::AmpSim) && k.params[4] == -3 && k.track_on(4) && !k.track_on(3)
          && k.pad_on(1, 70) && !k.pad_on(1, 6));
    const auto& c = back->compressor;
    CHECK(c.on && c.color && c.attack == 12 && c.release == 80 && c.amount == 66 && c.boost == 9);
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

int main() {
    fader_and_takeover();
    pad_fx_and_flex();
    performance_effects();
    audio_input_and_rendering();
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
    std::puts("melodee ok");
}
