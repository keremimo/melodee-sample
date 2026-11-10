// SPDX-License-Identifier: AGPL-3.0-only
// Melodee's model and audio side on the host: pad layouts, knob bindings,
// the PROPHET engine and the engine host's sequencer and mixer.
#include "melodee/engine/effects.h"
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
#include "melodee/engine/punch.h"
#include "melodee/model/mod_matrix.h"
#include "melodee/engine/arp.h"
#include "melodee/model/chords.h"
#include "melodee/model/project_file.h"
#include "melodee/model/scales.h"
#include "melodee/shell/bindings.h"
#include "melodee/shell/settings.h"
#include "melodee/shell/sound_list.h"
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
        bank.at[0][0].steps[0].notes[0] = {57, 100}; // pattern 1: a note on its first step only
        bank.at[0][1].steps[0].notes[0] = {69, 100};
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
    // Patterns: a launch waits for the next bar; 1/8 steps take twice as long as 1/16.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        setup(*project, *bank);
        bank->at[0][0].division = Division::Eighth;
        EngineHost host;
        host.load(*project, *bank);
        host.send({Command::Type::Play});
        std::vector<int32_t> block(256);
        for (unsigned n = 0; n < 80; ++n) host.render(block.data(), 128, 1); // 10240 samples: before the second eighth (11025)
        CHECK(host.step_of(0) == 0 && host.pattern_of(0) == 0);
        for (unsigned n = 0; n < 10; ++n) host.render(block.data(), 128, 1); // 11520: past it
        CHECK(host.step_of(0) == 1);
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
    auto even = chop(ChopMode::Even, 4, data.frames, hits, found, 1200);
    CHECK(even.count == 4 && even.start[2] == 22050);
    auto beats = chop(ChopMode::Beats, 1, data.frames, hits, found, 1200);
    CHECK(beats.count == 2 && beats.start[1] == 22050);
    auto clicks = chop(ChopMode::Hits, 100, data.frames, hits, found, 1200);
    CHECK(clicks.count >= 4);
    for (unsigned n = 1; n < 4; ++n) CHECK(std::abs(int(clicks.start[n]) - int(n * 11024)) < 600);
    Slices edited = even;
    CHECK(edited.add(1000) && edited.count == 5 && edited.start[1] == 1000);
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
    recorder->arm(-30, 1000, 0, 7);
    recorder->process(quiet.data(), quiet.data(), 128);
    CHECK(recorder->state() == TakeRecorder::State::Armed && recorder->frames() == 0);
    for (unsigned n = 0; n < 10; ++n) recorder->process(loud.data(), loud.data(), 128);
    CHECK(recorder->state() == TakeRecorder::State::Done && recorder->frames() == 1000 && recorder->slot() == 7);
    CHECK(std::fabs(recorder->left()[999] - 0.5f) < 1e-6f && recorder->peak(127) > 100);
    recorder->arm(-30, 1000, 0, 1); // held: a new take waits for release
    CHECK(recorder->frames() == 1000);
    recorder->release();
    CHECK(recorder->state() == TakeRecorder::State::Idle);
    // At once (-60 dB), with gain, stopped early.
    recorder->arm(-60, 0, 6, 2);
    CHECK(recorder->state() == TakeRecorder::State::Recording);
    recorder->process(quiet.data(), quiet.data(), 128);
    recorder->stop();
    CHECK(recorder->state() == TakeRecorder::State::Done && recorder->frames() == 128 && std::fabs(recorder->left()[0] - 0.001995f) < 1e-5f);
    recorder->release();
    // The host records the mix (resample) for a bar at 120 BPM.
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    load_sound(project->tracks[0], EngineKind::Prophet, 0);
    EngineHost host;
    host.load(*project, *bank);
    host.send({Command::Type::RecordArm, 2, 1, 5, -60});
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
    bank->at[0][3].length = 96; bank->at[0][3].division = Division::Eighth;
    bank->at[0][3].steps[95].notes[1] = {64, 77}; bank->at[0][3].steps[95].flags = step_tie;
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
    CHECK(copy_bank->at[0][3].length == 96 && copy_bank->at[0][3].division == Division::Eighth);
    CHECK(copy_bank->at[0][3].steps[95].notes[0].key == 64 && copy_bank->at[0][3].steps[95].flags == step_tie);
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
    a.pad_response = 2; a.pad_lights = 1; a.pickup = false; a.screen = 60;
    Settings b;
    b.read("start=/x\n" + a.text() + "colour=pink\nscreen=900\n");
    CHECK(b.pad_response == 2 && b.pad_lights == 1 && !b.pickup && b.screen == 100);
    b.read(a.text());
    CHECK(b.screen == 60);
    // Pad response: soft lifts a light hit, hard lowers it, fixed is always 100, medium leaves it.
    Settings soft, medium, hard, fixed;
    soft.pad_response = 0; hard.pad_response = 2; fixed.pad_response = 3;
    CHECK(soft.velocity(40) > 40 && hard.velocity(40) < 40 && medium.velocity(40) == 40 && fixed.velocity(7) == 100);
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
} // namespace

int main() {
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
