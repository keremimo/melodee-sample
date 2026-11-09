// SPDX-License-Identifier: AGPL-3.0-only
// Melodee's model and audio side on the host: pad layouts, knob bindings,
// the PROPHET engine and the engine host's sequencer and mixer.
#include "melodee/engine/engine_host.h"
#include "melodee/engine/prophet.h"
#include "melodee/model/mod_matrix.h"
#include "melodee/model/scales.h"
#include "melodee/shell/bindings.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

using namespace melodee;

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

namespace {
float peak_of(const std::vector<float>& v, size_t from = 0, size_t to = SIZE_MAX) {
    float peak = 0;
    for (size_t n = from; n < std::min(to, v.size()); ++n) { CHECK(std::isfinite(v[n])); peak = std::max(peak, std::fabs(v[n])); }
    return peak;
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
    pattern.steps[0].notes[0] = {50, 90};
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
    bank->at[0][0].steps[4].notes[0] = {45, 100};
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
    Command write {Command::Type::SetStep, 0, 0, 0, 0};
    write.step.notes[0] = {60, 120};
    host.send(write);
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
        for (unsigned s = 0; s < 16; ++s) bank->at[0][0].steps[s].notes[0] = {57, 100};
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
} // namespace

int main() {
    mod_matrix();
    motion();
    layouts();
    bindings();
    prophet_engine();
    host_sequencer();
    std::puts("melodee ok");
}
