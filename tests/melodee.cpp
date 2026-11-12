// SPDX-License-Identifier: AGPL-3.0-only
// Melodee's model and audio side on the host: pad layouts, knob bindings,
// the engine host's sequencer and mixer, and synth tracks as plug-ins.
#include "melodee/engine/effects.h"
#include <fstream>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <map>
#include <atomic>
#include <chrono>
#include <thread>
#include <unistd.h>
#include "melodee/engine/sample_data.h"
#include "melodee/engine/sampler.h"
#include "melodee/engine/sfz.h"
#include "melodee/engine/keygroup.h"
#include "melodee/engine/exs.h"
#include "melodee/engine/sf2.h"
#include "melodee/engine/dspreset.h"
#include "melodee/shell/expansion_install.h"
#include "melodee/engine/stream_pager.h"
#include "melodee/shell/sample_cache.h"
#include "melodee/engine/take_recorder.h"
#include "melodee/shell/chop.h"
#include "melodee/engine/engine_host.h"
#include "melodee/engine/print_render.h"
#include "melodee/model/print.h"
#include "melodee/engine/plugin_engine.h"
#include "melodee/plugin/rate_bridge.h"
#include "melodee/engine/punch.h"
#include "melodee/engine/pad_fx_info.h"
#include "melodee/model/mod_matrix.h"
#include "melodee/engine/arp.h"
#include "melodee/model/chords.h"
#include "melodee/model/project_file.h"
#include "melodee/model/generators.h"
#include "melodee/model/smf.h"
#include "melodee/model/lane_value.h"
#include "melodee/model/scales.h"
#include "melodee/shell/bindings.h"
#include "melodee/shell/settings.h"
#include "melodee/shell/capture_ring.h"
#include "melodee/shell/place_bar.h"
#include "melodee/shell/playheads.h"
#include "melodee/shell/record_place.h"
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
#include "melodee/shell/midi_ports.h"
#include "melodee/model/fader.h"
#include "melodee/engine/effect_info.h"
#include "melodee/shell/history.h"
#include "melodee/shell/name_entry.h"
#include "melodee/shell/name_suggest.h"
#include "melodee/model/sound_file.h"
#include "melodee/shell/sound_list.h"
#include "melodee/shell/key_pick.h"
#include "melodee/shell/button_jobs.h"
#include "melodee/shell/page_stack.h"
#include "melodee/shell/project_kits.h"
#include "melodee/shell/sampler_map.h"
#include "melodee/shell/zone_shapes.h"
#include "melodee/shell/stage_map.h"
#include "melodee/shell/undo_scope.h"
#include "melodee/shell/roll.h"
#include "melodee/shell/meter_lights.h"
#include "melodee/shell/shift_pad_jobs.h"
#include "melodee/shell/mute_mode.h"
#include "melodee/shell/card_access.h"
#include "melodee/shell/note_repeat.h"
#include "melodee/shell/pad_bank.h"
#include "melodee/shell/track_copy.h"
#include "melodee/shell/track_swap.h"
#include "melodee/shell/step_edit.h"
#include "melodee/model/pad_params.h"
#include "melodee/model/input_config.h"
#include "melodee/model/song_flatten.h"
#include "melodee/engine/recall_ring.h"
#include "melodee/engine/bounce.h"
#include "melodee/engine/looper.h"
#include "melodee/shell/looper_chord.h"
#include "melodee/shell/loop_sheet.h"
#include "melodee/plugin/drum_map.h"
#include "melodee/plugin/tone3000.h"
#include <algorithm>
#include <tuple>
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
// A point of Motion (roadmap P1: the pattern's own) on sixteenth `sixteenth`; a lock with `hold` ticks.
MotionPoint point(unsigned sixteenth, unsigned param, int value, unsigned pad = 0, unsigned hold = 0) {
    MotionPoint m;
    m.tick = sixteenth * sixteenth_ticks; m.param = uint8_t(param); m.value = int16_t(value); m.pad = uint8_t(pad); m.hold = uint16_t(hold);
    return m;
}
// One note on step `step` of the pattern's Q, in place of what the step held.
void put(Pattern& p, unsigned step, uint8_t key, uint8_t velocity, uint8_t length = 1, uint8_t flags = 0) {
    Step s;
    s.notes[0] = {key, velocity};
    s.length = length; s.flags = flags;
    p.set_step(step, s);
}

// A synth track since Melodee's synths became plug-ins of their own (Melodee Poly, PD and Chip): a PLUGIN track. In
// the host tests a ToneRunner plays it, as the rack's plug-in would: a sine a note at its key's pitch and its
// velocity's level, a 5 ms attack and a release of `release` seconds, keeping every MIDI message it was sent.
struct ToneRunner final : PluginRunner {
    struct Message { uint8_t status = 0, a = 0, b = 0; };
    std::vector<Message> heard;
    float release = 0.003f;
    struct Voice { int key = -1; bool held = false; float phase = 0, level = 0, gain = 0; };
    std::array<Voice, 16> voices {};
    void midi(uint8_t status, uint8_t a, uint8_t b) override {
        heard.push_back({status, a, b});
        uint8_t kind = status & 0xf0;
        if (kind == 0x90 && b) {
            Voice* v = nullptr;
            for (auto& x : voices) if (x.key == a) v = &x;
            if (!v) for (auto& x : voices) if (x.key < 0) { v = &x; break; }
            if (!v) v = &voices[0];
            v->key = a; v->held = true; v->gain = b / 127.0f * 0.3f;
        } else if (kind == 0x80 || kind == 0x90) {
            for (auto& x : voices) if (x.key == a) x.held = false;
        } else if (kind == 0xb0 && (a == 120 || a == 123)) {
            for (auto& x : voices) { x.held = false; if (a == 120) x = Voice {}; }
        }
    }
    void silence() override { for (auto& x : voices) x = Voice {}; }
    void render(float* left, float* right, unsigned frames) override {
        float fall = std::exp(-1.0f / (release * 44100.0f)), rise = 1.0f - std::exp(-1.0f / (0.005f * 44100.0f));
        for (auto& v : voices) {
            if (v.key < 0) continue;
            float step = 440.0f * std::exp2((v.key - 69) / 12.0f) / 44100.0f;
            for (unsigned f = 0; f < frames; ++f) {
                v.level = v.held ? v.level + (v.gain - v.level) * rise : v.level * fall;
                if (!v.held && v.level < 1e-5f) { v = Voice {}; break; } // ended on its own frame, however the blocks are cut
                float x = std::sin(2 * 3.14159265f * v.phase) * v.level;
                v.phase += step; v.phase -= std::floor(v.phase);
                left[f] += x; right[f] += x;
            }
        }
    }
    unsigned count(uint8_t kind, int a = -1, int b = -1) const {
        unsigned n = 0;
        for (auto& m : heard) if ((m.status & 0xf0) == kind && (a < 0 || m.a == a) && (b < 0 || m.b == b)) ++n;
        return n;
    }
};
// Track `t` of a project a synth: a PLUGIN track on plug-in entry `entry`.
void synth(Track& t, unsigned entry) {
    load_sound(t, EngineKind::Plugin, 0);
    t.plugin = uint8_t(entry);
}
// A second of a sine at `hz`, half scale, for a kit's pad to play.
SampleData tone_sample(float hz = 220) {
    SampleData d;
    d.frames = 44100; d.left.resize(d.frames); d.right.resize(d.frames);
    for (unsigned n = 0; n < d.frames; ++n) d.left[n] = d.right[n] = 0.5f * std::sin(2 * 3.14159265f * hz * float(n) / 44100.0f);
    return d;
}
// Track `t` a kit whose first four pads play sample slot 0 (offer it a tone_sample()), each held as long as its note.
void tone_kit(Project& project, unsigned t) {
    load_sound(project.tracks[t], EngineKind::Sampler, 0);
    std::snprintf(project.samples[0].path, sizeof(project.samples[0].path), "/media/card/Samples/Tone.wav");
    project.samples[0].frames = 44100;
    for (unsigned pad = 0; pad < 4; ++pad) project.tracks[t].pads[pad].sample = 0;
}
// The synths a host's tracks play: a ToneRunner for each PLUGIN track's entry, offered to the host (it takes them
// at its next block). Kept as long as the host plays them.
struct Tones {
    std::array<ToneRunner, max_plugins> at {};
    void offer(EngineHost& host, const Project& project) {
        for (const auto& t : project.tracks)
            if (t.engine == EngineKind::Plugin && t.plugin < max_plugins) CHECK(host.offer_plugin(t.plugin, &at[t.plugin]));
    }
};
// A Motion lane of an engine's own value (0..max_params-1), for the tests of how Motion is kept.
constexpr unsigned engine_lane = 16;
// For tests that only need their synth tracks heard: tone runners for a host's PLUGIN tracks, kept to the run's end.
void tones_for(EngineHost& host, const Project& project) {
    static std::vector<std::unique_ptr<Tones>> kept;
    kept.push_back(std::make_unique<Tones>());
    kept.back()->offer(host, project);
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
    load_sound(project.tracks[0], EngineKind::Midi, 0);
    UiState ui;
    Pattern pattern;
    ui.stage_group[0] = 1; // a MIDI track's Stage CC: its three controllers
    auto stage = page_bindings(project, ui, pattern);
    CHECK(stage[0].kind == Binding::Kind::Param && stage[0].id == midi_track::Cc1);
    CHECK(stage[0].at(0) == 0 && stage[0].at(127) == 127 && stage[0].at(64) == 64);
    ui.stage_group[0] = 0;
    ui.page = PageId::Tracks; // the strip in hand: the track, its level and pan
    auto mix = page_bindings(project, ui, pattern);
    CHECK(mix[0].kind == Binding::Kind::TrackPick && mix[1].kind == Binding::Kind::Level);
    CHECK(mix[2].kind == Binding::Kind::Pan && mix[2].at(0) == -50 && mix[2].at(127) == 50);
    ui.page = PageId::Steps;
    CHECK(!page_bindings(project, ui, pattern)[0].bound()); // an empty step has nothing on the cards
    put(pattern, 0, 50, 90);
    CHECK(page_bindings(project, ui, pattern)[1].value == 90);
    ui.page = PageId::Edit; ui.edit_page[0] = 1; ui.edit_row[0] = 0; // a MIDI track's PROGRAM page
    CHECK(page_bindings(project, ui, pattern)[0].id == midi_track::BankMsb);
    // Every engine's every sound sets values inside its parameters' ranges.
    std::array<EngineKind, size_t(EngineKind::Count)> engines {};
    for (unsigned k = 0, count = available_engines(engines); k < count; ++k) {
        const auto& info = engine_info(engines[k]);
        for (unsigned s = 0; s < info.sound_count; ++s) {
            std::array<int16_t, max_params> values;
            sound_values(info, s, values);
            for (unsigned p = 0; p < info.param_count; ++p) CHECK(values[p] >= info.params[p].min && values[p] <= info.params[p].max);
        }
    }
}

void host_sequencer() {
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    synth(project->tracks[0], 0);
    project->tempo = 1200;
    put(bank->at[0][0], 4, 45, 100);
    EngineHost host;
    host.load(*project, *bank);
    Tones tones;
    tones.offer(host, *project);
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
    {   // A synth (a PLUGIN track): the accent is a louder velocity; a slide names the key the next note goes on from
        // (CC 84, portamento control) just before it, and a tie holds the legato pedal (CC 68) over that, as Melodee
        // Poly, PD and Chip take them.
        auto heard = [](uint8_t flags) {
            auto project = std::make_unique<Project>();
            auto bank = std::make_unique<PatternBank>();
            synth(project->tracks[0], 0);
            project->tempo = 1200;
            put(bank->at[0][0], 0, 57, 80, 1, flags); put(bank->at[0][0], 1, 64, 80);
            EngineHost host;
            host.load(*project, *bank);
            auto tones = std::make_unique<Tones>();
            tones->offer(host, *project);
            host.send({Command::Type::Play});
            std::vector<int32_t> out(256);
            for (unsigned n = 0; n < 80; ++n) host.render(out.data(), 128, 1);
            return tones->at[0].heard;
        };
        using Heard = std::vector<ToneRunner::Message>;
        auto velocity = [](const Heard& h) { for (auto& m : h) if ((m.status & 0xf0) == 0x90 && m.b) return int(m.b); return 0; };
        auto at = [](const Heard& h, uint8_t status, uint8_t a, int b = -1) {
            for (size_t n = 0; n < h.size(); ++n) if (h[n].status == status && h[n].a == a && (b < 0 || h[n].b == b)) return int(n);
            return -1;
        };
        auto plain = heard(0), accent = heard(step_accent), slide = heard(step_slide), tie = heard(step_tie);
        CHECK(velocity(plain) == 80 && velocity(accent) == 127); // 80 x 1.6, as far as MIDI goes
        CHECK(at(plain, 0xb0, 84) < 0 && at(plain, 0xb0, 68) < 0);
        int named = at(slide, 0xb0, 84, 57), next = at(slide, 0x90, 64), off = at(slide, 0x80, 57);
        CHECK(named >= 0 && next == named + 1 && off == next + 1 && at(slide, 0xb0, 68) < 0);
        int down = at(tie, 0xb0, 68, 127), tied = at(tie, 0xb0, 84, 57), on = at(tie, 0x90, 64), up = at(tie, 0xb0, 68, 0);
        CHECK(down >= 0 && tied == down + 1 && on == tied + 1 && up > on);
    }
}
void motion() {
    // The store (a pattern's own since roadmap P1): a second write at the same place replaces, clear drops one lane,
    // and it fills at the limit.
    Pattern m;
    constexpr unsigned a = sampler::Filter, b = sampler::Resonance; // two lanes of engine values
    CHECK(m.write_motion(point(4, a, 10)) && m.write_motion(point(4, a, 20)) && m.motion.size() == 1);
    CHECK(m.motion[0].value == 20);
    CHECK(m.write_motion(point(8, b, 5)) && m.write_motion(point(2, a, 5)));
    CHECK(m.motion[0].tick == 2 * sixteenth_ticks && m.motion[2].param == b); // in order of ticks
    CHECK(m.clear_lane(a, 0) && m.motion.size() == 1 && !m.has_lane(a, 0) && m.has_lane(b, 0));
    Pattern full;
    for (unsigned n = 0; n < motion_points; ++n) { MotionPoint p; p.tick = n; CHECK(full.write_motion(p)); }
    MotionPoint over; over.tick = motion_points;
    CHECK(!full.write_motion(over) && full.motion.size() == motion_points);

    // Played: the kit's volume brought down by motion from step 0 makes a line of hits much quieter.
    auto level = [](bool closed) {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        tone_kit(*project, 0);
        for (unsigned s = 0; s < 16; ++s) put(bank->at[0][0], s, uint8_t(sampler_first_note), 100);
        if (closed) bank->at[0][0].write_motion(point(0, sampler::Level, 0));
        EngineHost host;
        host.load(*project, *bank);
        auto tone = tone_sample();
        CHECK(host.offer_sample(0, &tone));
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
// Pad pressure (phase 1d) as Melodee keeps and sends it; what a synth does with it is the synth's own (Melodee
// Poly, PD and Chip test theirs).
void pad_expression() {
    // 16 Levels' ends and a track's four sends go with the project (LVLE, SNDS), with the params version.
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    load_sound(project->tracks[2], EngineKind::Midi, 0);
    project->tracks[2].levels_ends[unsigned(LevelsKind::Tune)] = {-12, 24};
    project->tracks[2].mix.sends = {10, 20, 0, 30};
    auto bytes = save_project(*project, *bank);
    unsigned version = 9;
    auto back = std::make_unique<Project>();
    CHECK(load_project(bytes, *back, *bank, &version).empty() && version == params_version);
    CHECK(back->tracks[2].levels_ends[unsigned(LevelsKind::Tune)][0] == -12 && back->tracks[2].levels_ends[unsigned(LevelsKind::Tune)][1] == 24);
    CHECK(back->tracks[3].levels_ends == Track {}.levels_ends);
    const std::array<uint8_t, 4> four {10, 20, 0, 30}, none {};
    CHECK(back->tracks[2].mix.sends == four && back->tracks[3].mix.sends == none);
    UserSound us;
    us.engine = EngineKind::Midi;
    auto sound_bytes = save_sound(us);
    CHECK(load_sound_file(sound_bytes, us, &version) && version == params_version);
    sound_bytes.pop_back(); // as written before phase 1d
    CHECK(load_sound_file(sound_bytes, us, &version) && version == 0);
    // The arp's notes take the pressure of the pad holding their key, an octave up too: a synth hears it as each
    // note's own (poly) pressure.
    {
        auto p = std::make_unique<Project>();
        auto b = std::make_unique<PatternBank>();
        synth(p->tracks[0], 0);
        EngineHost host;
        host.load(*p, *b);
        auto tones = std::make_unique<Tones>();
        tones->offer(host, *p);
        host.send({Command::Type::Arp, 0, 0, 0, 1});
        host.send({Command::Type::Arp, 0, 0, 3, 2}); // two octaves: notes the pad never played
        host.send({Command::Type::ArpKey, 0, 0, 48, 100});
        host.send({Command::Type::Pressure, 0, 0, 48, 127});
        std::vector<int32_t> out(256);
        for (unsigned n = 0; n < 400; ++n) host.render(out.data(), 128, 1);
        const auto& heard = tones->at[0];
        CHECK(heard.count(0x90, 60) > 0 && heard.count(0xa0, 48) > 0 && heard.count(0xa0, 60) > 0);
    }
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
        synth(project.tracks[0], 0);
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
        project->song.place({0, 0, 1, 1, false});
        EngineHost host;
        host.load(*project, *bank);
        tones_for(host, *project);
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
        tones_for(host, *project);
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
    // Pad FX: a load sets each pad's chain and takes the last project's away, a pad that had one set by its project
    // or by a command alike (the host keeps which pads have a kind, so a load passes the others unread).
    {
        auto host = std::make_unique<EngineHost>();
        auto project = std::make_unique<Project>();
        auto patterns = std::make_unique<PatternBank>();
        project->tracks[2].pad_fx.at[5].slots[1] = EffectSlot(uint8_t(EffectKind::Tremolo));
        host->load(*project, *patterns);
        CHECK(host->pad_fx_kind(2, 5, 1) == EffectKind::Tremolo);
        std::vector<int32_t> out(256);
        host->send({Command::Type::PadFxKind, 7, 100, 2, int16_t(EffectKind::AutoPan)}); // bank G's pad 4
        host->render(out.data(), 128, 1);
        CHECK(host->pad_fx_kind(7, 100, 2) == EffectKind::AutoPan);
        auto empty = std::make_unique<Project>();
        host->load(*empty, *patterns);
        CHECK(host->pad_fx_kind(2, 5, 1) == EffectKind::None && host->pad_fx_kind(7, 100, 2) == EffectKind::None);
    }
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
        synth(project->tracks[0], 0);
        if (filter) { project->tracks[0].fx.slots[0].kind = uint8_t(EffectKind::Filter); project->tracks[0].fx.slots[0].params = {5, 0, 0, 0, 0, 0}; }
        if (echo) {
            project->tracks[0].mix.sends[0] = 100;
            project->buses[0].fx.slots[0].kind = uint8_t(EffectKind::Echo);
            project->buses[0].fx.slots[0].params = {60, 50, 100, 80, 0, 0};
        }
        EngineHost host;
        host.load(*project, *bank);
        tones_for(host, *project);
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
        auto host_held = std::make_unique<EngineHost>(); // on the heap: a test's frame stays under the 8 MB stack
        auto& host = *host_held;
        host.load(*back, *bank);
        tones_for(host, *back);
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
        auto host_held = std::make_unique<EngineHost>(); // on the heap: a test's frame stays under the 8 MB stack
        auto& host = *host_held;
        host.load(*project, *bank);
        tones_for(host, *project);
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
        synth(project->tracks[0], 0);
        put(bank->at[0][0], 0, 45, 110, 16);
        auto& slot = project->tracks[0].fx.slots[0];
        slot.kind = uint8_t(EffectKind::Filter);
        const auto& info = effect_info(EffectKind::Filter);
        for (unsigned q = 0; q < effect_params; ++q) slot.params[q] = info.params[q].label ? info.params[q].initial : 0;
        slot.params[0] = 100;
        bank->at[0][0].write_motion(point(8, motion_fx_param(0, 0), 0));
        CHECK(motion_param(project->tracks[0], int(motion_fx_param(0, 0))).info == &info.params[0]);
        CHECK(!motion_param(project->tracks[0], int(motion_fx_param(1, 0))).info); // no effect in slot 2
        UiState ui;
        auto lanes = motion_lanes(*project, ui, bank->at[0][0]);
        CHECK(lanes[0] == int(motion_fx_param(0, 0)));
        auto host_held = std::make_unique<EngineHost>(); // on the heap: a test's frame stays under the 8 MB stack
        auto& host = *host_held;
        host.load(*project, *bank);
        tones_for(host, *project);
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
        synth(t, 0);
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
    auto host_held = std::make_unique<EngineHost>(); // on the heap: a test's frame stays under the 8 MB stack
    auto& host = *host_held;
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
    synth(project->tracks[0], 0);
    EngineHost host;
    host.load(*project, *bank);
    tones_for(host, *project);
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
    std::snprintf(project->name, sizeof(project->name), "Project 1");
    project->tempo = 1245; project->swing = 58; project->root = 2; project->scale = 2;
    load_sound(project->tracks[0], EngineKind::Midi, 0);
    project->tracks[0].params[midi_track::Cc1] = 33;
    project->tracks[0].mix = {90, -20, 40, 2, true, false, {0, 0, 40, 0}};
    project->tracks[0].mods[3] = {1, 2, -45};
    project->tracks[0].fx.slots[0] = {uint8_t(EffectKind::Echo), true, {60, 50, 30, 70, 0, 0}};
    load_sound(project->tracks[4], EngineKind::Sampler, 0);
    project->tracks[4].pads[9].sample = 2; project->tracks[4].pads[9].start = 1234; project->tracks[4].pads[9].group = 3;
    std::snprintf(project->samples[2].path, sizeof(project->samples[2].path), "/media/card/Samples/Kick.wav");
    bank->at[0][3].length = 96 * 48; bank->at[0][3].division = Division::Eighth;
    bank->at[0][3].add({95 * 48 + 5, 40, 64, 77, 100, step_tie}); // five ticks late, Rec Quantize off
    bank->at[0][3].write_motion(point(7, midi_track::Cc2, 61));
    project->song.place({0, 3, 8, 4, true});
    project->song.step_marker(8);
    project->song.loop_start = 8; project->song.loop_end = 12;
    project->buses[1].level = 33; project->buses[1].fx.slots[0].kind = uint8_t(EffectKind::Room);
    project->master.slots[0].kind = uint8_t(EffectKind::Limiter);
    project->tracks[0].chain = {2, 0, 5}; project->tracks[0].chain_count = 3; project->tracks[0].chain_at = 1; // seq-sheet2 B (CHAN)
    project->tracks[1].chain = {4}; project->tracks[1].chain_count = 1; // one pattern: no chain, nothing written

    auto data = save_project(*project, *bank);
    auto copy = std::make_unique<Project>();
    auto copy_bank = std::make_unique<PatternBank>();
    CHECK(load_project(data, *copy, *copy_bank).empty());
    CHECK(std::string(copy->name) == "Project 1" && copy->tempo == 1245 && copy->swing == 58 && copy->scale == 2);
    CHECK(copy->tracks[0].engine == EngineKind::Midi && copy->tracks[0].params[midi_track::Cc1] == 33);
    CHECK(copy->tracks[0].mix.pan == -20 && copy->tracks[0].mix.sends[2] == 40 && copy->tracks[0].mix.sends[0] == 0 && copy->tracks[0].mix.mute);
    CHECK(copy->tracks[0].mods[3].amount == -45 && copy->tracks[0].fx.slots[0].bypass && copy->tracks[0].fx.slots[0].params[3] == 70);
    CHECK(copy->tracks[4].engine == EngineKind::Sampler && copy->tracks[4].pads[9].start == 1234 && copy->tracks[4].pads[9].group == 3);
    { // The pattern chain (seq-sheet2 B): kept in order, from its first; the next queued in the last bar of the one playing.
        const auto& ch = copy->tracks[0];
        CHECK(ch.chained() && ch.chain_count == 3 && ch.chain[0] == 2 && ch.chain[1] == 0 && ch.chain[2] == 5 && ch.chain_at == 0);
        CHECK(!copy->tracks[1].chained() && copy->tracks[1].chain_count == 0);
        CHECK(ch.chain_ahead(0) == 2 && ch.chain_ahead(1) == 0 && ch.chain_ahead(3) == 2); // round again
        constexpr uint32_t bar = 4 * ticks_per_quarter;
        CHECK(!chain_due(0, 2 * bar, bar) && !chain_due(int(bar) - 1, 2 * bar, bar) && chain_due(int(bar), 2 * bar, bar));
        CHECK(chain_due(0, bar, bar) && !chain_due(-1, bar, bar)); // a one-bar pattern: at once; stopped: never
    }
    CHECK(!copy->tracks[4].pads[8].used());
    CHECK(std::string(copy->samples[2].path) == "/media/card/Samples/Kick.wav");
    CHECK(copy_bank->at[0][3].length == 96 * 48 && copy_bank->at[0][3].division == Division::Eighth);
    CHECK(copy_bank->at[0][3].events.size() == 1 && copy_bank->at[0][3].events[0] == Event({95 * 48 + 5, 40, 64, 77, 100, step_tie}));
    CHECK(copy_bank->at[0][3].step(95).notes[0].key == 64 && copy_bank->at[0][3].step(95).notes[0].offset == 5);
    CHECK(copy_bank->at[0][3].motion.size() == 1 && copy_bank->at[0][3].motion[0].value == 61 && copy_bank->at[0][3] == bank->at[0][3]);
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
    track.chord_type = 1; track.transpose = 2; // the pads moved a tone, E Dorian: pad 2 is F#, its chord F#m7 (not Em7 under it)
    CHECK(chord_notes(project, track, 1, keys) == 4 && keys[0] == pad_note(project, track, 1).key && keys[0] == 42 && keys[3] == 52);
    CHECK(chord_name(project, track, 1) == "F#m7");
    track.transpose = 0;
    { // Any keys held together, named (stage-preset B): the shape over a root, the bass after a slash when it is not the root.
        const int dm7[] {38, 41, 45, 48}, c[] {48, 52, 55}, c_e[] {52, 55, 60}, one[] {60}, odd[] {60, 61}, gsus4[] {43, 48, 50};
        CHECK(name_chord(dm7, 4) == "Dm7" && name_chord(c, 3) == "C" && name_chord(c_e, 3) == "C/E");
        CHECK(name_chord(one, 1).empty() && name_chord(odd, 2).empty() && name_chord(gsus4, 3) == "Gsus4");
    }
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
    synth(p->tracks[0], 0);
    EngineHost host;
    host.load(*p, *bank);
    tones_for(host, *p);
    host.send({Command::Type::Arp, 0, 0, 0, 1});
    host.send({Command::Type::ArpKey, 0, 0, 60, 100});
    std::vector<int32_t> out(256);
    float peak = 0;
    for (unsigned n = 0; n < 100; ++n) { host.render(out.data(), 128, 1); for (auto s : out) peak = std::max(peak, std::fabs(s / 2147483648.0f)); }
    CHECK(!host.playing() && peak > 0.005f);
}
// Round 2, pad-fx C: punches kept with a pattern. What was heard becomes the pattern's punches where they fell,
// they go with the pattern through its speed, Double / Half Seq and Half / Double Speed, and the file keeps them
// (a pattern holding nothing else too); the host plays them on their ticks, several at once, each let go after its
// length, and quiet while a live punch sounds.
void kept_punches() {
    { // heard_punches: now 900, 132 ticks into the pass (begun at 768); the last pass is 516 .. 900.
        const PunchSpan spans[] {
            {100, 150, 4, 255, 90, 2, 1, false}, // before the last pass: gone
            {500, 600, 8, 2, 100, 1, 0, false},  // from before it: from where it begins
            {700, 0, 14, 255, 70, 3, 2, true},   // still sounding: until now
            {800, 800, 5, 255, 100, 2, 1, false}, // let go before its grid line: nothing heard
        };
        auto heard = heard_punches(spans, 4, 900, 132, 384);
        CHECK(heard.size() == 2);
        CHECK(heard[0] == PatternPunch({132, 84, 8, 2, 100, 1, 0}));
        CHECK(heard[1] == PatternPunch({316, 200, 14, 255, 70, 3, 2}));
        CHECK(heard_punches(spans, 4, 900, 132, 0).empty());
        auto later = heard_punches(spans + 1, 1, 5000, 0, 384); // Keep long after: the take is still the last one heard
        CHECK(later.size() == 1 && later[0].tick == 108 && later[0].length == 100);
        const PunchSpan two_takes[] {{100, 200, 4, 255, 100, 2, 1, false}, {1000, 1100, 8, 255, 100, 2, 1, false}};
        auto take = heard_punches(two_takes, 2, 2000, 0, 384); // a pass and more apart: the newer alone
        CHECK(take.size() == 1 && take[0].kind == 8);
        const PunchSpan long_one[] {{0, 0, 9, 255, 100, 2, 1, false}}; // held over two passes: one pass long
        auto whole = heard_punches(long_one, 1, 1000, 232, 384);
        CHECK(whole.size() == 1 && whole[0].tick == 232 && whole[0].length == 384);
        std::vector<PunchSpan> many;
        for (unsigned n = 0; n < 40; ++n) many.push_back({uint64_t(n * 8), uint64_t(n * 8 + 4), uint8_t(n % 16), 255, 100, 2, 1, false});
        CHECK(heard_punches(many.data(), 40, 384, 0, 384).size() == max_pattern_punches);
    }
    { // With the pattern: its speed, Double and Half Seq, Half and Double Speed.
        Pattern p;
        p.length = 384;
        p.punches = {{96, 48, 4, 255, 100, 2, 1}, {300, 24, 8, 255, 100, 2, 1}};
        auto fast = speed_pattern(p, 4); // x2: half the ticks
        CHECK(fast.length == 192 && fast.punches[0].tick == 48 && fast.punches[0].length == 24 && fast.punches[1].tick == 150 && fast.punches[1].length == 12);
        auto slow = speed_pattern(p, 0); // x1/2: twice
        CHECK(slow.punches[1].tick == 600 && slow.punches[1].length == 48);
        Pattern twice = p;
        CHECK(double_pattern(twice, 4 * 384) && twice.punches.size() == 4 && twice.punches[2].tick == 96 + 384 && twice.punches[3].tick == 300 + 384);
        Pattern bare = p;
        CHECK(double_pattern(bare, 4 * 384, false) && bare.punches.size() == 2);
        Pattern half = p;
        half_pattern(half);
        CHECK(half.length == 192 && half.punches.size() == 1 && half.punches[0].tick == 96);
        Pattern slower = p;
        scale_pattern(slower, true);
        CHECK(slower.punches.size() == 1 && slower.punches[0].tick == 192 && slower.punches[0].length == 96); // 600 falls past the end
        Pattern faster = p;
        scale_pattern(faster, false);
        CHECK(faster.punches.size() == 2 && faster.punches[1].tick == 150 && faster.punches[1].length == 12);
    }
    { // The file (KEPT): kept as they were, a pattern holding only punches too, beside the song's (PNCH); none
      // where there were none.
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        project->song.add_punch({2, 4, 8, 3, 255, 90});
        bank->at[3][5].punches = {{0, 96, 8, 255, 100, 2, 1}, {288, 48, 14, 3, 70, 3, 2}};
        auto data = save_project(*project, *bank);
        auto copy = std::make_unique<Project>();
        auto copy_bank = std::make_unique<PatternBank>();
        CHECK(load_project(data, *copy, *copy_bank).empty());
        CHECK(copy_bank->at[3][5] == bank->at[3][5] && copy_bank->at[3][5].punches.size() == 2);
        CHECK(copy->song.punch_count == 1 && copy->song.punches[0].bar == 2 && copy->song.punches[0].kind == 3);
        CHECK(copy_bank->at[0][0].punches.empty() && copy_bank->at[3][4].punches.empty());
    }
    { // The host: Low on the master from the bar's start for a sixteenth's two, Stutter on track 1 from the first
      // sixteenth's end for four; both at once, each let go after its length, again on the next pass; Stop lets go.
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        synth(project->tracks[0], 0);
        project->tempo = 1200;
        bank->at[0][0].length = project->bar_ticks();
        bank->at[0][0].punches = {{0, 48, uint8_t(PunchKind::Low), 255, 100, 2, 1}, {24, 96, uint8_t(PunchKind::Stutter), 0, 100, 2, 1}};
        auto host = std::make_unique<EngineHost>();
        host->load(*project, *bank);
        tones_for(*host, *project);
        std::vector<int32_t> out(256);
        host->send({Command::Type::Play});
        unsigned blocks = 0;
        auto run_to = [&](unsigned n) { for (; blocks < n; ++blocks) host->render(out.data(), 128, 1); }; // a tick is 229.7 samples
        run_to(1);
        CHECK(host->kept_punches() == 1);
        run_to(50); // tick 27
        CHECK(host->kept_punches() == 3);
        run_to(100); // tick 55: Low let go
        CHECK(host->kept_punches() == 2);
        run_to(230); // tick 128: Stutter let go
        CHECK(host->kept_punches() == 0);
        run_to(700); // the next pass
        CHECK(host->kept_punches() == 1);
        // A live punch wins: from its grid line (408) the kept ones are quiet, Stutter due there too; let go, both
        // come back for the rest of their length.
        Command on {Command::Type::PunchOn, 255, uint8_t(2 | 1 << 4), uint16_t(PunchKind::Gate), 100};
        on.position = 1;
        host->send(on);
        run_to(740); // tick 412
        CHECK(host->live_punches() == 1 && host->kept_punches() == 0);
        Command off {Command::Type::PunchOff, 255};
        off.position = 1;
        host->send(off);
        run_to(745); // tick 414: Low (to 432) and Stutter (to 504) again
        CHECK(host->live_punches() == 0 && host->kept_punches() == 3);
        host->send({Command::Type::Stop});
        run_to(746);
        CHECK(host->kept_punches() == 0);
    }
    { // Pad FX quantize (Shift + B3): playing, a live punch waits for its snap grid's next line (1/4 here: tick 96)
      // while a kept one sounds on; with 64 in its timing (quantize off) it starts at once, the kept one giving way.
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        synth(project->tracks[0], 0);
        project->tempo = 1200;
        bank->at[0][0].length = project->bar_ticks();
        bank->at[0][0].punches = {{0, project->bar_ticks(), uint8_t(PunchKind::Low), 255, 100, 2, 1}};
        auto host = std::make_unique<EngineHost>();
        host->load(*project, *bank);
        tones_for(*host, *project);
        std::vector<int32_t> out(256);
        host->send({Command::Type::Play});
        unsigned blocks = 0;
        auto run_to = [&](unsigned n) { for (; blocks < n; ++blocks) host->render(out.data(), 128, 1); }; // a tick is 229.7 samples
        run_to(20); // tick 11
        CHECK(host->kept_punches() == 1);
        Command on {Command::Type::PunchOn, 255, uint8_t(0 | 1 << 4), uint16_t(PunchKind::Gate), 100};
        on.position = 1;
        host->send(on);
        run_to(60); // tick 33: waiting for 96
        CHECK(host->live_punches() == 1 && host->kept_punches() == 1);
        run_to(180); // tick 100
        CHECK(host->kept_punches() == 0);
        Command off {Command::Type::PunchOff, 255};
        off.position = 1;
        host->send(off);
        run_to(183); // tick 102: Low back
        CHECK(host->live_punches() == 0 && host->kept_punches() == 1);
        on.pattern = uint8_t(0 | 1 << 4 | 64);
        host->send(on);
        run_to(186); // tick 103, the next quarter 192
        CHECK(host->live_punches() == 1 && host->kept_punches() == 0);
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
        const unsigned seam = unsigned(std::ceil(Punch::seam_frames(loop))); // each turn's end crossfades into its start
        for (unsigned n = 0; n + seam < loop; ++n) {
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
    // clear() (a project switch): what played before it reads as silence, though nothing is wiped; Echo repeats
    // what came two grid steps back, so without the clear it plays the noise again.
    for (bool cleared : {false, true}) {
        Punch punch;
        std::vector<float> l(128), r(128);
        unsigned at = 0;
        auto block = [&](bool sound) {
            for (unsigned f = 0; f < 128; ++f) l[f] = r[f] = sound ? signal(at + f) : 0.0f;
            punch.process(l.data(), r.data(), 128);
            at += 128;
        };
        for (unsigned b = 0; b < 200; ++b) block(true);
        if (cleared) punch.clear();
        for (unsigned b = 0; b < 4; ++b) block(false); // a little silence since
        punch.start(PunchKind::Echo, 1, bar, bar / 16, 0);
        float peak = 0;
        for (unsigned b = 0; b < 40; ++b) {
            block(false);
            for (unsigned f = 0; f < 128; ++f) peak = std::max(peak, std::fabs(l[f]));
        }
        CHECK(cleared ? peak == 0 : peak > 0.1f);
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
        project->punch_quantize = false;
        auto& pad = project->tracks[4].pads[9];
        pad.sample = 0; pad.punch = 15; pad.punch_rate = 1; pad.punch_depth = 33;
        std::strcpy(project->samples[0].path, "/x.wav");
        auto bytes = save_project(*project, *patterns);
        auto back = std::make_unique<Project>();
        CHECK(load_project(bytes, *back, *patterns).empty());
        CHECK(back->punch_target == 1 && back->punch_bank == 1 && back->punch_depth == 64 && back->punch_snap == 3 && back->punch_tail == 2);
        CHECK(!back->punch_quantize);
        project->punch_quantize = true;
        auto again = std::make_unique<Project>();
        CHECK(load_project(save_project(*project, *patterns), *again, *patterns).empty() && again->punch_quantize);
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
    // Capture's rows (capture C, no page): Auto (roadmap P5: the phrase), bars 1, 2, 4, 8, 16, 32, and as played,
    // 1/16 or 1/8; kept in the text.
    CHECK(c.value_text(Settings::CaptureBars) == "Auto" && c.value_text(Settings::CaptureQuantize) == "1/16");
    c.step(Settings::CaptureBars, 1); CHECK(c.capture_bars == 1 && c.value_text(Settings::CaptureBars) == "1 bar");
    c.step(Settings::CaptureBars, 1); CHECK(c.capture_bars == 2 && c.value_text(Settings::CaptureBars) == "2 bars");
    for (int n = 0; n < 6; ++n) c.step(Settings::CaptureBars, 1);
    CHECK(c.capture_bars == 32);
    for (int n = 0; n < 5; ++n) c.step(Settings::CaptureBars, -1);
    CHECK(c.capture_bars == 1);
    c.step(Settings::CaptureQuantize, -1); CHECK(c.capture_quantise == 0 && c.value_text(Settings::CaptureQuantize) == "As played");
    { Settings back; back.read(c.text()); CHECK(back.capture_bars == 1 && back.capture_quantise == 0); }
    { Settings odd; odd.read("capture_bars=5\ncapture_quantize=9\n"); CHECK(odd.capture_bars == 4 && odd.capture_quantise == 2); }
    { Settings none; none.read("capture_bars=0\n"); CHECK(none.capture_bars == 0); }
    CHECK(std::string(Settings::row_name(Settings::CaptureQuantize)) == "Capture quantize" && Settings::CaptureQuantize + 1 == Settings::Screen);
    // Favourites: starred sounds by engine, kept in the text; the Sounds page's FAVOURITES lists them.
    static_assert(Settings::engines == unsigned(EngineKind::Count), "a favourites row per engine");
    Settings f;
    f.toggle_favourite(unsigned(EngineKind::Midi), 3); f.toggle_favourite(unsigned(EngineKind::Midi), 9);
    f.toggle_favourite(unsigned(EngineKind::Sampler), 2); f.toggle_favourite(unsigned(EngineKind::Sampler), 2);
    Settings g;
    g.read(f.text());
    CHECK(g.favourite(unsigned(EngineKind::Midi), 3) && g.favourite(unsigned(EngineKind::Midi), 9) && !g.favourite(unsigned(EngineKind::Sampler), 2));
    auto track = std::make_unique<Track>();
    load_sound(*track, EngineKind::Midi, 0);
    UiState ui;
    ui.settings = g; ui.sounds_filter = 1; ui.user_sound_count = 10; // sounds of one's own, 3 and 9 starred
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
    load_sound(*track, EngineKind::Midi, 0);
    track->params[3] = -7; track->mods[1] = {2, 3, -40};
    auto bytes = save_sound(sound_of(*track, "BELL TREE"));
    UserSound back;
    CHECK(load_sound_file(bytes, back));
    CHECK(back.engine == EngineKind::Midi && std::string(back.name) == "BELL TREE" && back.params == track->params);
    CHECK(back.mods[1].source == 2 && back.mods[1].destination == 3 && back.mods[1].amount == -40);
    bytes[0] = 'X';
    CHECK(!load_sound_file(bytes, back));
    // The Sounds list counts sounds of one's own after the factory ones.
    UiState ui;
    ui.user_sound_count = 2;
    std::array<uint16_t, max_listed> list {};
    unsigned listed = sound_list(*track, ui, list);
    const unsigned factory = engine_info(EngineKind::Midi).sound_count;
    CHECK(listed == factory + 2 && list[listed - 1] == factory + 1);
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
    Track synth; synth.engine = EngineKind::Plugin; synth.last_note = 62;
    CHECK(synth.step_note() == 62);

    auto project = std::make_unique<Project>();
    project->tracks[0].engine = EngineKind::Plugin; project->tracks[0].transpose = -3;
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
}

// Phase 1b: Shift + Pad 1-16 as printed, their toggles and lights, and the
// buttons that glow while Shift is held.
void shift_pads() {
    const char* const printed[16] {"Full Level", "Half Seq", "Double Seq", "Count-In", "Compressor", "Half Speed",
                                   "Double Speed", "Settings", "Fader", "Rec Quantize", "Resample", "Song",
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
    project->tracks[0].engine = EngineKind::Plugin;
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
    project->tracks[0].engine = EngineKind::Plugin;
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
    ui.page = PageId::Tracks; ui.seq_held = true;
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

    // Faint lights: Chop only with a kit; Loop everywhere (held, the loop grid); Erase where it does something.
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
    CHECK(std::string(arp_rate_name(repeat_rate(1, true))) == "1/16T" && arp_rate_ticks(repeat_rate(2, true)) == 8); // 96 a quarter since P1
    for (unsigned r = 1; r < arp_rates; ++r) CHECK(arp_rate_ticks(r) < arp_rate_ticks(r - 1)); // slowest first, as K2 turns
    CHECK(arp_swing_ticks(4, 1, 75) == 12.0 && arp_swing_ticks(4, 2, 75) == 0 && arp_swing_ticks(5, 1, 75) == 0 && arp_swing_ticks(4, 1, 50) == 0);
    auto project = std::make_unique<Project>();
    UiState ui;
    ui.page = PageId::Keys;
    CHECK(!repeat_keys(*project, ui));
    project->tracks[0].arp = true;
    CHECK(repeat_keys(*project, ui));
    ui.page = PageId::Stage; // latched, Stage keeps its buttons; held, they are the rates
    CHECK(!repeat_keys(*project, ui));
    ui.repeat_held = true; CHECK(repeat_keys(*project, ui)); ui.repeat_held = false;
    ui.page = PageId::Song;
    CHECK(!repeat_keys(*project, ui)); // its own keys stay on Song
    Arp arp; arp.top = int(sampler_last_key);
    arp.press(150, 90);
    int velocity = 0;
    CHECK(arp.next(velocity) == 150 && velocity == 90); // a kit pad in bank H repeats as itself

    // Mute mode: kit pads where the pads play the kit (Stage too), tracks on Tracks.
    ui.page = PageId::Pads;
    CHECK(mute_on_kit(ui, true) && !mute_on_kit(ui, false));
    ui.page = PageId::Stage;
    CHECK(mute_on_kit(ui, true));
    ui.page = PageId::Tracks;
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
    project->tracks[1].engine = EngineKind::Plugin;
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
    synth(start->tracks[0], 0);
    host->load(*start, *start_bank);
    tones_for(*host, *start);
    std::vector<int32_t> out(256);
    Track synth;
    ::synth(synth, 0);
    std::array<Pattern, pattern_count> patterns {};
    put(patterns[0], 0, 48, 110);
    auto pack = std::make_unique<TrackPack>(2, synth, patterns);
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
        section("MOTN", {1, 0, 0, 3, 5, 0, uint8_t(engine_lane), 20, 0});
        Project project; auto bank = std::make_unique<PatternBank>();
        CHECK(load_project(bytes, project, *bank).empty());
        const auto& old = bank->at[0][3];
        CHECK(old.length == 32 * 48 && old.division == Division::Eighth && old.events.size() == 1);
        CHECK(old.events[0] == Event({5 * 48, 2 * 48, 60, 100, 100, step_accent}));
        CHECK(old.motion.size() == 1 && old.motion[0].tick == 10 * sixteenth_ticks); // eighth 5 is sixteenth 10, in its pattern
        // Saved again it is an event list (EVNT), and loads the same.
        project.signature = 12; project.seq_tempo = true; project.pattern_tempo[3] = 905;
        auto data = save_project(project, *bank);
        for (const char* tag : {"EVNT", "MOTK", "TIME"}) CHECK(std::search(data.begin(), data.end(), tag, tag + 4) != data.end());
        const char* patt = "PATT";
        CHECK(std::search(data.begin(), data.end(), patt, patt + 4) == data.end());
        Project again; auto again_bank = std::make_unique<PatternBank>();
        CHECK(load_project(data, again, *again_bank).empty());
        CHECK(again_bank->at[0][3] == old && again_bank->at[0][3].motion[0].tick == 10 * sixteenth_ticks);
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
        synth(project->tracks[0], 0);
        project->tempo = 1200;
        bank->at[0][0] = pattern;
        auto host = std::make_unique<EngineHost>();
        host->load(*project, *bank);
        tones_for(*host, *project);
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
    // The metronome's blip lasts as long at 96 kHz as at 44.1 kHz (it fell a fixed step a frame).
    {
        double lasted[2] {};
        for (unsigned k = 0; k < 2; ++k) {
            const float rate = k ? 96000.0f : 44100.0f;
            dsp::device_rate.store(rate);
            auto host = host_with(Pattern {});
            host->send({Command::Type::Play, 1}); // the count-in's first click, nothing else playing
            auto left = render(*host, unsigned(0.4f * rate) / 128);
            size_t last = 0;
            for (size_t n = 0; n < left.size(); ++n) if (std::fabs(left[n]) > 0.002f) last = n;
            lasted[k] = double(last) / rate;
        }
        dsp::device_rate.store(44100.0f);
        if (!(lasted[0] > 0.03 && std::fabs(lasted[0] - lasted[1]) < 0.002)) std::fprintf(stderr, "click %g s / %g s\n", lasted[0], lasted[1]);
        CHECK(lasted[0] > 0.03 && std::fabs(lasted[0] - lasted[1]) < 0.002);
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
        ui.page = PageId::TimeCorrect; five.feel.strength = 70; five.feel.swing = 60; five.feel.humanize = 20;
        auto tc = page_bindings(project, ui, five, false); // time-correct C: Q, then the strength and swing heard live
        CHECK(tc[0].kind == Binding::Kind::Division && tc[1].kind == Binding::Kind::FeelStrength && tc[1].value == 70
              && tc[2].kind == Binding::Kind::FeelSwing && tc[2].value == 60);
        auto tc_shift = page_bindings(project, ui, five, true); // Shift: humanize, its velocity, the groove's amount
        CHECK(tc_shift[0].kind == Binding::Kind::FeelHumanize && tc_shift[0].value == 20 && tc_shift[1].kind == Binding::Kind::FeelVelocity
              && tc_shift[2].kind == Binding::Kind::GrooveAmount);
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
    CHECK(level >= motion_fx_base + track_slots * 6 && pad_param_of(level) == int(PadParam::Level) && pad_param_of(engine_lane) < 0);
    Pattern m;
    CHECK(m.write_motion(point(0, level, 20, 9)) && m.write_motion(point(0, level, 90, 2)) && m.write_motion(point(8, level, 64, 9)));
    CHECK(m.write_motion(point(0, level, 30, 9)) && m.motion.size() == 3 && m.motion[1].value == 30); // the same place: replaced
    CHECK(m.has_lane(level, 9) && m.has_lane(level, 2) && !m.has_lane(level, 3) && !m.has_lane(tune, 9));
    m.clear_lane(level, 9);
    CHECK(m.motion.size() == 1 && m.motion[0].pad == 2);

    // The kit's lanes on Motion: the last pad's recorded ones first, then its tune, level and pan.
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    load_sound(project->tracks[4], EngineKind::Sampler, 0);
    auto& kit = project->tracks[4];
    kit.pads[9].sample = 0; kit.pads[9].level = 100; kit.pads[2].sample = 0;
    kit.last_pad = 9;
    bank->at[4][0].write_motion(point(4, level, 0, 9));
    bank->at[4][0].write_motion(point(2, motion_pad_param(PadParam::Attack), 50, 2)); // pad 3's: not shown while pad 10 is the last
    UiState kit_ui;
    kit_ui.track = 4;
    auto lanes = motion_lanes(*project, kit_ui, bank->at[4][0]);
    CHECK(lanes[0] == motion_lane(level, 9) && lanes[1] == motion_lane(tune, 9) && lanes[2] == motion_lane(motion_pad_param(PadParam::Pan), 9));
    CHECK(lane_name(kit, lanes[0]) == "A10 LEVEL" && motion_param(kit, lanes[0]).value == 100);
    auto card = lane_binding(kit, 4, lanes[0]);
    CHECK(card.kind == Binding::Kind::PadLevel && card.max == 127 && card.value == 100);
    CHECK(motion_param(kit, motion_lane(motion_pad_param(PadParam::VelocitySense), 9)).info); // parity phase 4 gave pads it
    CHECK(!motion_param(project->tracks[0], motion_lane(level, 9)).info);                      // not a kit
    kit.last_pad = 2;
    CHECK(motion_lanes(*project, kit_ui, bank->at[4][0])[0] == motion_lane(motion_pad_param(PadParam::Attack), 2));
    kit.last_pad = 9;

    // The file: each pattern's Motion in a MOTK of its own (roadmap P1), pad lanes with their pad.
    bank->at[0][0].write_motion(point(3, engine_lane, 77));
    auto data = save_project(*project, *bank);
    auto back = std::make_unique<Project>();
    auto back_bank = std::make_unique<PatternBank>();
    CHECK(load_project(data, *back, *back_bank).empty() && back_bank->at[4][0].motion.size() == 2);
    CHECK(back_bank->at[4][0].has_lane(level, 9) && back_bank->at[4][0].has_lane(motion_pad_param(PadParam::Attack), 2));
    CHECK(back_bank->at[0][0].has_lane(engine_lane, 0) && back_bank->at[0][0] == bank->at[0][0]);

    // The audio side: pad 10's level held at 0 from step 4 silences its hits there, and Stop gives it back.
    SampleData data_wav;
    data_wav.frames = 44100;
    data_wav.left.assign(44100, 0.5f); data_wav.right.assign(44100, 0.5f);
    for (unsigned n = 0; n < 44100; ++n) data_wav.left[n] = data_wav.right[n] = float(std::sin(n * 0.05) * 0.5);
    auto quiet = std::make_unique<Project>(*project);
    quiet->tracks[4].pads[9].end = 2000; quiet->tracks[4].pads[9].play = PadPlay::OneShot;
    auto& beat = bank->at[4][0];
    beat = Pattern {};
    beat.write_motion(point(4, level, 0, 9));
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

    // 16 Levels: Tune has the original on pad 4 at first, Filter sweeps the cutoff evenly; the two ends move (round 2).
    auto project = std::make_unique<Project>();
    load_sound(project->tracks[4], EngineKind::Sampler, 0);
    auto& kit = project->tracks[4];
    kit.pads[2].sample = 0; kit.pads[2].cutoff = 60; kit.levels_pad = 2;
    kit.levels_kind = uint8_t(LevelsKind::Tune);
    CHECK(levels_of(*project, kit, 3).semitones == 0 && levels_of(*project, kit, 0).semitones == -3 && levels_of(*project, kit, 15).semitones == 12);
    kit.levels_kind = uint8_t(LevelsKind::Filter);
    CHECK(levels_of(*project, kit, 0).cutoff == 0 && levels_of(*project, kit, 7).cutoff == 47 && levels_of(*project, kit, 15).cutoff == 100);
    project->tracks[0].levels_kind = uint8_t(LevelsKind::Filter);
    CHECK(levels_of(*project, project->tracks[0], 5).velocity == level_velocity(5)); // a synth: Filter is soft to loud
    kit.levels_ends[unsigned(LevelsKind::Filter)] = {48, 60};                       // a sweep over an octave of the cutoff
    CHECK(levels_of(*project, kit, 0).cutoff == 48 && levels_of(*project, kit, 15).cutoff == 60);
    kit.levels_kind = uint8_t(LevelsKind::Velocity);
    kit.levels_ends[unsigned(LevelsKind::Velocity)] = {40, 127};                    // sixteen levels between 40 and 127
    CHECK(levels_of(*project, kit, 0).velocity == 40 && levels_of(*project, kit, 15).velocity == 127 && levels_of(*project, kit, 8).velocity == 86);
    // Scale: up the key from its root, pad 1 the root, the sample a C: D major from D, A major from the A below.
    kit.levels_kind = uint8_t(LevelsKind::Scale);
    auto old_root = project->root, old_scale = project->scale;
    project->root = 2; project->scale = 0;
    CHECK(levels_of(*project, kit, 0).semitones == 2 + level_octave(kit) && levels_of(*project, kit, 2).semitones == 6 + level_octave(kit) &&
          levels_of(*project, kit, 7).semitones == 14 + level_octave(kit));
    kit.levels_ends[unsigned(LevelsKind::Scale)] = {-2, 3}; // an older file's ends change nothing
    CHECK(levels_of(*project, kit, 15).semitones == 28 + level_octave(kit));
    project->root = 9;
    CHECK(kit_root_shift(*project, kit) == -3 && levels_of(*project, kit, 0).semitones == -3 + level_octave(kit));
    kit.transpose = 3; // the track's key: A moved to C
    CHECK(levels_of(*project, kit, 0).semitones == level_octave(kit));
    kit.transpose = 0; project->root = old_root; project->scale = old_scale;
    kit.levels_ends = Track {}.levels_ends;
    CHECK(Track {}.levels_kind == uint8_t(LevelsKind::Scale)); // a new track's 16 Levels go up the scale first

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
    tones_for(*host, *project);
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
    for (unsigned n = 0; n < 5; ++n) host->render(out.data(), 128, 1, input.data()); // Monitor eases in and out (Ease)
    CHECK(host->monitoring() && out[0] > int32_t(0.3 * 2147483647.0)); // the line input heard (soft clipped)
    host->send({Command::Type::Monitor, 0, 0, 0, 0});
    for (unsigned n = 0; n < 5; ++n) host->render(out.data(), 128, 1, input.data());
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
    synth(project->tracks[0], 0);
    bank->at[0][0].length = project->bar_ticks();
    bank->at[0][0].add({0, 24, 57, 110, 100, 0});
    bank->at[0][0].add({192, 24, 60, 110, 100, 0});
    host = std::make_unique<EngineHost>();
    host->load(*project, *bank);
    tones_for(*host, *project);
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
    tones_for(*host, *project);
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
    CHECK(song_frames >= 2 * 88200 && song_frames <= 2 * 88200 + Bounce::tail_seconds * 44100 + 128);
    host->bounce().release();

    // Rec length Seq while playing: the take waits for the pattern to come round, then runs its length.
    host = std::make_unique<EngineHost>();
    project->song = Song {};
    host->load(*project, *bank);
    tones_for(*host, *project);
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
    synth(project->tracks[0], 0);
    auto energy = [&](const std::function<void(EngineHost&)>& set) {
        EngineHost host;
        host.load(*project, *bank);
        tones_for(host, *project);
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
            tones_for(host, *kit);
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
            tones_for(host, *kit);
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
        tones_for(host, *project);
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
        tones_for(host, *project);
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
        synth(two->tracks[2], 2);
        struct Heard { double energy; float reduction; };
        auto side = [&](bool on, bool key_plays, bool other_plays, bool key_muted) {
            EngineHost host;
            host.load(*two, *bank);
            tones_for(host, *two);
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
            tones_for(host, *kit);
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
        tones_for(host, *project);
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
    project->knob_fx.settle(); // as a load leaves it: A and B the values heard (Knob FX C)
    project->knob_fx.b = {1, 90, 10}; project->knob_fx.morph = 100;
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
    CHECK(k.a[1] == 70 && k.b[1] == 90 && k.b[2] == 10 && k.morph == 100 && k.editing == 0); // Knob FX C: A, B and the fader's place
    { // a KNFX from before Knob FX C (no A, B or morph after the pads): A and B are the values heard
        auto before_c = data;
        for (size_t n = 0; n + 8 <= before_c.size(); ++n) {
            if (std::memcmp(&before_c[n], "KNFX", 4) != 0) continue;
            uint32_t length = 0;
            std::memcpy(&length, &before_c[n + 4], 4);
            before_c.erase(before_c.begin() + long(n + 8 + length - 14), before_c.begin() + long(n + 8 + length));
            length -= 14;
            std::memcpy(&before_c[n + 4], &length, 4);
            break;
        }
        auto old = std::make_unique<Project>();
        CHECK(load_project(before_c, *old, *back_bank).empty());
        const auto& ok = old->knob_fx;
        CHECK(ok.kind == uint8_t(EffectKind::AmpSim) && ok.pad_on(1, 70) && ok.a[1] == 70 && ok.b[1] == 70 && ok.b[2] == 40 && ok.morph == 0);
    }
    { // A and B: the fader at 0 is A, at 127 B, between them in proportion
        KnobFx kf;
        kf.kind = uint8_t(EffectKind::Chorus); kf.params = {30, 50, 40, 0, 0, 0};
        kf.settle();
        CHECK(kf.a == kf.b && kf.a[2] == 40 && kf.morph == 0 && kf.editing == 0);
        kf.b = {80, 50, 0};
        kf.morph = 127; CHECK(kf.morphed(0) == 80 && kf.morphed(2) == 0);
        kf.morph = 0; CHECK(kf.morphed(0) == 30 && kf.morphed(2) == 40);
        kf.morph = 64; CHECK(kf.morphed(0) == 55 && kf.morphed(2) == 20 && kf.morphed(1) == 50);
    }
    const auto& c = back->compressor;
    CHECK(c.on && c.color && c.attack == 12 && c.release == 80 && c.amount == 66 && c.boost == 9 && c.sidechain && c.key == 5 && c.key_pad == 7);
    auto older = data;
    for (size_t n = 0; n + 4 <= older.size(); ++n)
        if (std::memcmp(&older[n], "KNFX", 4) == 0 || std::memcmp(&older[n], "CMPR", 4) == 0) std::memcpy(&older[n], "XXXX", 4);
    CHECK(load_project(older, *back, *back_bank).empty() && back->knob_fx.kind == 0 && !back->knob_fx.tracks && !back->compressor.on);

    // Bindings: with the mode on the pots are Knob FX's on any page, Shift its second row; Shift + K on a chain gives the other row.
    UiState ui;
    ui.page = PageId::Tracks;
    project->knob_fx.on = true; project->knob_fx.bypass = false;
    Pattern pattern;
    auto cards = page_bindings(*project, ui, pattern, false);
    CHECK(cards[0].kind == Binding::Kind::KnobParam && cards[0].id == 0 && cards[1].value == 70);
    cards = page_bindings(*project, ui, pattern, true);
    CHECK(cards[0].kind == Binding::Kind::KnobParam && cards[0].id == 3 && cards[2].value == 5);
    project->knob_fx.editing = 1; // Knob FX C: K1-K3 show the setting in hand, B here; Shift's row is the effect's own
    CHECK(page_bindings(*project, ui, pattern, false)[1].value == 90 && page_bindings(*project, ui, pattern, true)[2].value == 5);
    project->knob_fx.editing = 0;
    project->knob_fx.on = false;
    CHECK(page_bindings(*project, ui, pattern, false)[1].kind == Binding::Kind::Level);
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
        const unsigned in = unsigned(std::ceil(Punch::seam_seconds * dsp::sample_rate())); // it comes in over that first
        for (unsigned n = in; n < 256; ++n) CHECK(l[n] == 0 || std::fabs(std::fabs(l[n]) - 0.5f) < 1e-6f || std::fabs(std::fabs(l[n]) - 1) < 1e-6f);
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
    auto& synth = project->tracks[2]; // a track of notes: here a MIDI track, whose values are its own
    load_sound(synth, EngineKind::Midi, 0);
    // Defaults by engine; a kit target on a synth falls back to the mod wheel.
    CHECK(fader_target(kit) == FaderTarget::PadVolume && fader_target(synth) == FaderTarget::ModWheel);
    synth.fader_target = uint8_t(FaderTarget::PadPan);
    CHECK(fader_target(synth) == FaderTarget::ModWheel);
    // The track's mix goes on every track: a synth walks its mod wheel, level, pan and a send into each bus.
    CHECK(fader_takes(EngineKind::Midi, FaderTarget::TrackLevel) && fader_takes(EngineKind::Plugin, FaderTarget::SendD)
          && !fader_takes(EngineKind::Plugin, FaderTarget::KitVolume) && fader_takes(EngineKind::Sampler, FaderTarget::TrackPan));
    synth.mix.level = 80; synth.mix.pan = -12; synth.mix.sends[1] = 35;
    synth.fader_target = uint8_t(FaderTarget::TrackLevel);
    CHECK(fader_target(synth) == FaderTarget::TrackLevel);
    CHECK(fader_target_binding(synth, FaderTarget::TrackLevel).kind == Kind::Level && fader_target_binding(synth, FaderTarget::TrackLevel).value == 80);
    CHECK(fader_target_binding(synth, FaderTarget::TrackPan).kind == Kind::Pan && fader_target_binding(synth, FaderTarget::TrackPan).value == -12);
    auto send = fader_target_binding(synth, FaderTarget::SendB);
    CHECK(send.kind == Kind::Send && send.id == 1 && send.value == 35 && send.max == 100);
    {
        UiState at; at.track = 2; at.page = PageId::Stage; // the selected track's own mix, never a pad's value
        synth.fader_target = uint8_t(FaderTarget::TrackPan);
        auto mix = fader_job(*project, at);
        CHECK(mix.source == FaderJob::Source::Target && !mix.pad && mix.binding.kind == Kind::Pan && mix.centred);
        kit.fader_target = uint8_t(FaderTarget::SendD); at.track = 3;
        mix = fader_job(*project, at);
        CHECK(!mix.pad && mix.binding.kind == Kind::Send && mix.binding.id == 3);
        kit.fader_target = fader_by_engine;
    }
    synth.fader_target = fader_by_engine;
    // Pitch Bend: a plug-in's, or a MIDI track's own BEND, never a kit's; the wheel meets it beside the mod wheel.
    CHECK(fader_takes(EngineKind::Plugin, FaderTarget::PitchBend) && fader_takes(EngineKind::Midi, FaderTarget::PitchBend)
          && !fader_takes(EngineKind::Sampler, FaderTarget::PitchBend)
          && fader_order[fader_place(FaderTarget::ModWheel) + 1] == FaderTarget::PitchBend);
    synth.params[midi_track::Bend] = -20;
    auto bend = fader_target_binding(synth, FaderTarget::PitchBend);
    CHECK(bend.kind == Kind::Param && bend.id == midi_track::Bend && bend.value == -20 && bend.min == -64 && bend.max == 63);
    synth.params[midi_track::Bend] = 0;
    CHECK(!fader_target_binding(kit, FaderTarget::PitchBend).bound());
    kit.fader_target = uint8_t(FaderTarget::PitchBend);
    CHECK(fader_target(kit) == FaderTarget::PadVolume);
    kit.fader_target = fader_by_engine;
    {
        Track plug;
        ::synth(plug, 0);
        plug.bend = 12; plug.fader_target = uint8_t(FaderTarget::PitchBend);
        auto b = fader_target_binding(plug, fader_target(plug));
        CHECK(b.kind == Kind::PitchBend && b.value == 12 && b.min == -64 && b.max == 63 && b.at(64) == 0); // the fader's middle: no bend
        CHECK(binding_lane(plug, b) == motion_lane(motion_bend) && lane_name(plug, motion_lane(motion_bend)) == "PITCH BEND"
              && lane_binding(plug, 0, motion_lane(motion_bend)).kind == Kind::PitchBend);
        CHECK(lane_name(synth, motion_lane(motion_bend)) == "lane"); // a MIDI track bends by its BEND lane
        auto learned = fader_learnable(plug, 0, b); // fader C: learned as its lane, the card back from it
        CHECK(learned.what == FaderLearn::Lane && learned.lane == motion_bend);
        plug.fader_learned = learned;
        CHECK(fader_learned_binding(plug, 0).kind == Kind::PitchBend && fader_learned_binding(plug, 0).value == 12);
    }
    { // Its wide middle (Kerem): 54..74 is no bend, the rest spread so nothing jumps at the middle's edges; the ends reach the ends.
        FaderJob j; j.bend = true;
        CHECK(fader_reach(j, 64) == 64 && fader_reach(j, 54) == 64 && fader_reach(j, 74) == 64 && fader_reach(j, -1) == -1);
        Binding b {Kind::PitchBend, 0, -64, 63, 0};
        CHECK(b.at(fader_reach(j, 75)) == 1 && b.at(fader_reach(j, 53)) == -1 && b.at(fader_reach(j, 127)) == 63 && b.at(fader_reach(j, 0)) == -64);
        bool onward = true;
        for (int p = 1; p < 128; ++p) onward &= fader_reach(j, p) >= fader_reach(j, p - 1);
        CHECK(onward);
        j.bend = false;
        CHECK(fader_reach(j, 70) == 70);
        UiState at; at.track = 2; at.page = PageId::Stage; // a MIDI track's BEND is a bend too; pan is not
        synth.fader_target = uint8_t(FaderTarget::PitchBend);
        auto job = fader_job(*project, at);
        CHECK(job.bend && job.centred && job.binding.kind == Kind::Param && job.binding.id == midi_track::Bend);
        synth.fader_target = uint8_t(FaderTarget::TrackPan);
        CHECK(!fader_job(*project, at).bend);
        synth.fader_target = fader_by_engine;
    }
    { // The plug-in hears it as pitch bend: by hand, from its lane while playing, and back where the fader left it on Stop.
        auto proj = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        ::synth(proj->tracks[0], 0);
        bank->at[0][0].write_motion(point(0, motion_bend, -64));
        EngineHost host;
        host.load(*proj, *bank);
        auto tones = std::make_unique<Tones>();
        tones->offer(host, *proj);
        std::vector<int32_t> out(256);
        auto run = [&](unsigned blocks) { for (unsigned n = 0; n < blocks; ++n) host.render(out.data(), 128, 1); };
        auto last_bend = [&] {
            int v = -1;
            for (auto& m : tones->at[0].heard) if ((m.status & 0xf0) == 0xe0) v = m.a | m.b << 7;
            return v;
        };
        run(2);
        host.send({Command::Type::Bend, 0, 0, 0, int16_t(32 * 128)}); // +32 of the card's 63
        run(2);
        CHECK(last_bend() == 12288);
        host.send({Command::Type::Play});
        run(8);
        CHECK(last_bend() == 0); // the lane's -64: all the way down
        host.send({Command::Type::Stop});
        run(4);
        CHECK(last_bend() == 12288);
    }

    // What it drives: the track's target, or the page's own job.
    UiState ui; ui.track = 3; ui.page = PageId::Stage;
    auto job = fader_job(*project, ui);
    CHECK(job.source == FaderJob::Source::Target && job.binding.kind == Kind::PadLevel && job.binding.value == 90);
    ui.page = PageId::Tracks;
    job = fader_job(*project, ui);
    CHECK(job.source == FaderJob::Source::Page && job.binding.kind == Kind::Level);
    ui.page = PageId::Sends; CHECK(fader_job(*project, ui).binding.kind == Kind::Send);
    ui.page = PageId::StepEdit; CHECK(fader_job(*project, ui).source == FaderJob::Source::Target); // step entry: no nudge (round 2)
    ui.page = PageId::Compressor; CHECK(fader_job(*project, ui).binding.kind == Kind::CompAmount);
    ui.page = PageId::FlexBeat; CHECK(fader_job(*project, ui).binding.kind == Kind::FlexMix);
    ui.page = PageId::KnobFx; // no effect loaded: the track's target
    CHECK(fader_job(*project, ui).source == FaderJob::Source::Target);
    project->knob_fx.kind = uint8_t(EffectKind::Chorus); project->knob_fx.morph = 40;
    job = fader_job(*project, ui); // Knob FX C: from A to B, whatever the effect
    CHECK(job.source == FaderJob::Source::Page && job.binding.kind == Kind::KnobParam && unsigned(job.binding.id) == knob_morph
          && job.binding.value == 40 && job.binding.max == 127);
    project->knob_fx.kind = uint8_t(EffectKind::Filter);
    CHECK(unsigned(fader_job(*project, ui).binding.id) == knob_morph);
    // Off: only the page's own jobs remain.
    ui.page = PageId::Stage; kit.fader_on = false;
    CHECK(fader_job(*project, ui).source == FaderJob::Source::None && fader_light_level(fader_job(*project, ui)) == 0);
    ui.page = PageId::Tracks; CHECK(fader_job(*project, ui).source == FaderJob::Source::Page);
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
    { // Round 3 (fader readout B): the K card showing the fader's value says it moved; with none the header does.
        Pattern pattern;
        UiState at; at.track = 3; at.page = PageId::Tracks;
        CHECK(fader_twin(fader_job(*project, at), page_bindings(*project, at, pattern)) == 1); // LEVEL, K2
        at.page = PageId::Pads; // the last pad's PITCH, VOLUME, PAN
        CHECK(fader_twin(fader_job(*project, at), page_bindings(*project, at, pattern)) == -1); // Kit Volume: no card
        kit.fader_target = uint8_t(FaderTarget::PadVolume);
        CHECK(fader_twin(fader_job(*project, at), page_bindings(*project, at, pattern)) == 1);
        kit.fader_target = uint8_t(FaderTarget::PadPan);
        CHECK(fader_twin(fader_job(*project, at), page_bindings(*project, at, pattern)) == 2);
        kit.fader_on = false;
        CHECK(fader_twin(fader_job(*project, at), page_bindings(*project, at, pattern)) == -1);
        kit.fader_on = true; kit.fader_target = uint8_t(FaderTarget::KitVolume);
    }
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

    // Fader C (round 2): it learns a pot turned on its track, any of the track's values (PIN and the wheel are the shell's).
    {
        using L = FaderLearn;
        // What it can learn: an engine parameter, a pad's own value (whichever pad is hit last), the mix, the track's own effects.
        CHECK(fader_learnable(synth, 2, Binding {Kind::Param, midi_track::Cc1, 0, 127, 0}).what == L::Lane
              && fader_learnable(synth, 2, Binding {Kind::Param, midi_track::Cc1, 0, 127, 0}).lane == midi_track::Cc1);
        CHECK(fader_learnable(synth, 2, Binding {Kind::Tempo, 0, 300, 3000, 1200}).what == L::None);       // not the track's
        CHECK(fader_learnable(synth, 2, Binding {Kind::FxParam, 16 * 64 + 2, 0, 100, 0}).what == L::None); // its bus's effect
        CHECK(fader_learnable(synth, 2, Binding {Kind::FxParam, 3 * 64 + 2, 0, 100, 0}).what == L::None);  // another track's
        CHECK(fader_learnable(synth, 2, Binding {Kind::Send, 2, 0, 100, 0}).what == L::Send && fader_learnable(synth, 2, Binding {Kind::Send, 2, 0, 100, 0}).lane == 2);
        CHECK(fader_learnable(kit, 3, Binding {Kind::PadCutoff, 0, 0, 100, 0}).lane == motion_pad_param(PadParam::FilterCutoff));
        // The job: the learned card goes over the target, live.
        UiState at; at.page = PageId::Stage; at.track = 2;
        synth.params[midi_track::Cc1] = 70;
        synth.fader_learned = fader_learnable(synth, 2, Binding {Kind::Param, midi_track::Cc1, 0, 127, 0});
        auto learned = fader_job(*project, at);
        CHECK(learned.source == FaderJob::Source::Target && learned.learned && !learned.pad && learned.binding.kind == Kind::Param
              && learned.binding.id == midi_track::Cc1 && learned.binding.value == 70);
        at.page = PageId::Tracks; CHECK(fader_job(*project, at).source == FaderJob::Source::Page); // a page's own job still wins
        at.page = PageId::Stage;
        synth.fader_learned = fader_learnable(synth, 2, Binding {Kind::Pan, 0, -50, 50, 0});
        CHECK(fader_job(*project, at).binding.kind == Kind::Pan && fader_job(*project, at).centred);
        // One of the track's effects: until the effect goes, then the target again.
        synth.fx.slots[0] = {uint8_t(EffectKind::Chorus), false, {30, 50, 40, 0, 0, 0}};
        synth.fader_learned = fader_learnable(synth, 2, Binding {Kind::FxParam, 2 * 64 + 0 * 8 + 2, 0, 100, 40});
        CHECK(synth.fader_learned.lane == motion_fx_param(0, 2));
        learned = fader_job(*project, at);
        CHECK(learned.learned && learned.binding.kind == Kind::FxParam && learned.binding.id == 2 * 64 + 2 && learned.binding.value == 40);
        synth.fx.slots[0] = {};
        learned = fader_job(*project, at);
        CHECK(!learned.learned && learned.binding.kind == Kind::ModWheel);
        // Another engine: its parameter means nothing there, so the target again.
        synth.fader_learned = fader_learnable(synth, 2, Binding {Kind::Param, midi_track::Cc1, 0, 127, 0});
        auto learned_on = synth.fader_learned;
        ::synth(synth, 0);
        synth.fader_learned = learned_on;
        CHECK(!fader_job(*project, at).learned && fader_job(*project, at).binding.kind == Kind::ModWheel);
        load_sound(synth, EngineKind::Midi, 0);
        synth.fader_learned = learned_on;
        CHECK(fader_job(*project, at).learned);
        synth.fader_learned = {};
        // A pad's own value follows the last pad hit; an empty one has nothing to drive, but stays learned.
        at.track = 3; kit.last_pad = 8; kit.pads[8].cutoff = 33;
        kit.fader_learned = fader_learnable(kit, 3, Binding {Kind::PadCutoff, 0, 0, 100, 0});
        learned = fader_job(*project, at);
        CHECK(learned.learned && learned.pad && learned.binding.kind == Kind::PadCutoff && learned.binding.value == 33);
        CHECK(binding_lane(kit, learned.binding) == motion_lane(motion_pad_param(PadParam::FilterCutoff), 8)); // Seq Record: pad 9's lane
        kit.last_pad = 9;
        learned = fader_job(*project, at);
        CHECK(learned.learned && learned.pad && !learned.binding.bound());
        kit.last_pad = 8; kit.fader_learned = {};
    }

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
        kit.fader_learned = {FaderLearn::Lane, uint16_t(motion_pad_param(PadParam::FilterCutoff)), uint8_t(EngineKind::Sampler), 2};
        kit.fader_pin = true;
        synth.fader_learned = {FaderLearn::Send, 3, uint8_t(EngineKind::Midi), 0}; // learned, on its default target: FADR all the same
        patterns->at[2][0].write_motion(point(4, motion_wheel, 90)); patterns->at[2][0].write_motion(point(2, 3, 40));
        auto bytes = save_project(*project, *patterns);
        auto loaded = std::make_unique<Project>();
        CHECK(load_project(bytes, *loaded, *patterns).empty());
        CHECK(loaded->tracks[3].fader_target == uint8_t(FaderTarget::PadCutoff) && !loaded->tracks[3].fader_on);
        CHECK(loaded->tracks[2].fader_target == fader_by_engine && loaded->tracks[2].fader_on);
        const auto& kl = loaded->tracks[3].fader_learned;
        CHECK(kl.what == FaderLearn::Lane && kl.lane == motion_pad_param(PadParam::FilterCutoff) && kl.pot == 2 && loaded->tracks[3].fader_pin);
        CHECK(loaded->tracks[2].fader_learned.what == FaderLearn::Send && loaded->tracks[2].fader_learned.lane == 3 && !loaded->tracks[2].fader_pin);
        CHECK(!loaded->tracks[0].fader_learned.learned() && !loaded->tracks[0].fader_pin);
        CHECK(patterns->at[2][0].motion.size() == 2 && patterns->at[2][0].has_lane(motion_wheel, 0) && patterns->at[2][0].has_lane(3, 0));
        for (size_t n = 0; n + 4 <= bytes.size(); ++n)
            if (std::memcmp(&bytes[n], "FADR", 4) == 0 || std::memcmp(&bytes[n], "MOTW", 4) == 0) std::memcpy(&bytes[n], "XXXX", 4);
        auto older = std::make_unique<Project>();
        CHECK(load_project(bytes, *older, *patterns).empty());
        CHECK(older->tracks[3].fader_target == fader_by_engine && older->tracks[3].fader_on && fader_target(older->tracks[3]) == FaderTarget::PadVolume);
        CHECK(!older->tracks[3].fader_learned.learned() && !older->tracks[3].fader_pin);
        // A FADR from before fader C, its three bytes alone: the target and on / off, nothing learned.
        auto three = save_project(*project, *patterns);
        for (size_t n = 0; n + 8 <= three.size(); ++n) {
            if (std::memcmp(&three[n], "FADR", 4) != 0) continue;
            uint32_t length = 0;
            std::memcpy(&length, &three[n + 4], 4);
            if (length <= 3) continue;
            three.erase(three.begin() + long(n + 8 + 3), three.begin() + long(n + 8 + length));
            length = 3;
            std::memcpy(&three[n + 4], &length, 4);
        }
        auto before_c = std::make_unique<Project>();
        CHECK(load_project(three, *before_c, *patterns).empty());
        CHECK(before_c->tracks[3].fader_target == uint8_t(FaderTarget::PadCutoff) && !before_c->tracks[3].fader_on);
        CHECK(!before_c->tracks[3].fader_learned.learned() && !before_c->tracks[3].fader_pin && !before_c->tracks[2].fader_learned.learned());
        CHECK(patterns->at[2][0].motion.size() == 2); // the wheel's lane is in the pattern's MOTK with the rest (roadmap P1)
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
        CHECK(midi_row_value(set, track, ProgramChangeRow) == "Off" && std::string(midi_row_name(ResetRow)) == "Reset factory settings");
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
        Track synth; synth.engine = EngineKind::Plugin;
        CHECK(note_key(kit, 35) == -1 && note_key(kit, 36) == 36 && note_key(kit, 127) == 127);
        CHECK(note_key(synth, 12) == 12 && note_key(synth, 127) == 127);
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
        synth(project->tracks[0], 0);
        project->tempo = 1200;
        put(bank->at[0][0], 4, 45, 100);
        put(bank->at[1][0], 0, 60, 90); // track 2 has no engine: a MIDI track under Empty
        EngineHost host;
        host.load(*project, *bank);
        tones_for(host, *project);
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
        synth(project->tracks[0], 0);
        project->tempo = 1200;
        put(bank->at[0][0], 4, 45, 100);
        EngineHost host;
        host.load(*project, *bank);
        tones_for(host, *project);
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
// The wheel's pick on screens that offer B1-B3 as choices (shell/key_pick.h): a detent a key, held at the ends,
// stepping over keys with nothing.
void key_pick() {
    CHECK(key_pick_turn(0, 1) == 1 && key_pick_turn(1, 1) == 2 && key_pick_turn(2, 1) == 2);
    CHECK(key_pick_turn(2, -1) == 1 && key_pick_turn(0, -1) == 0 && key_pick_turn(0, 5) == 2 && key_pick_turn(2, -9) == 0);
    CHECK(key_pick_turn(0, 1, 0b101) == 2 && key_pick_turn(2, -1, 0b101) == 0 && key_pick_turn(2, 1, 0b101) == 2);
    CHECK(key_pick_turn(1, 0) == 1);
}

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
    card_press(ui, B::TopLeft);                                        // asked again: the wheel starts on CANCEL
    CHECK(ui.ask == UiState::Ask::TakeCardBack && ui.ask_pick == 0);
    CHECK(card_press(ui, B::WheelPush).request == CardRequest::None && ui.ask == UiState::Ask::None); // Push: CANCEL
    card_press(ui, B::TopLeft);
    ui.ask_pick = key_pick_turn(ui.ask_pick, 1, 0b101);                // a detent right: TAKE
    CHECK(ui.ask_pick == 2 && card_press(ui, B::WheelPush).request == CardRequest::Force);
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
    // B1 Trim -> Layers -> Mix -> Amp Env, B2 Tune -> Play -> Follow -> Cloud, B3 Filter -> Filt Env -> Kit filter; another group's key: its first.
    CHECK(sampler_next(PageId::Trim, 0) == PageId::LayersPage && sampler_next(PageId::LayersPage, 0) == PageId::PadMix);
    CHECK(sampler_next(PageId::PadMix, 0) == PageId::EnvelopePage);
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
    synth(project.tracks[0], 0);
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
        t.levels_kind = uint8_t(LevelsKind::Velocity);
        CHECK(levels_of(*p, t, 15).semitones == 0); // Velocity: no pitch
        t.levels_kind = uint8_t(LevelsKind::Scale); // up the key from its root in the track's octave: C Chromatic, pad 16 D#5
        CHECK(levels_of(*p, t, 0).key == 60 && levels_of(*p, t, 15).key == 75 && levels_of(*p, t, 15).semitones == 0);
        // Chords in Chromatic: a major triad on each pad's own semitone, not a cluster.
        std::array<int, notes_per_step> keys {};
        t.chord_type = 0; t.chord_voicing = 0; t.chord_invert = 0;
        CHECK(chord_notes(*p, t, 1, keys) == 3 && keys[0] == 61 && keys[1] == 65 && keys[2] == 68);
        CHECK(chord_name(*p, t, 1) == "C#");
        t.chord_type = 5;
        CHECK(chord_notes(*p, t, 0, keys) == 4 && keys[3] == 74); // Dia 9
    }
    // The project's key on a kit: Shift + K1 / K2 on Pads and Tempo; 16 Levels of type Scale on K2 / K3, even on an empty pad.
    {
        auto p = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        starting_project(*p, *bank);
        UiState ui;
        Pattern pattern;
        using K = Binding::Kind;
        for (PageId page : {PageId::Pads, PageId::Tempo}) {
            ui.page = page;
            auto shifted = page_bindings(*p, ui, pattern, true);
            CHECK(shifted[0].kind == K::Root && shifted[1].kind == K::Scale && shifted[1].max == 12);
        }
        ui.page = PageId::Levels; // K1 the type; of type Scale K2 the root and K3 the scale, of the others the two ends
        p->tracks[0].levels_kind = uint8_t(LevelsKind::Scale);
        auto levels = page_bindings(*p, ui, pattern, false);
        CHECK(levels[0].kind == K::LevelsType && levels[1].kind == K::Root && levels[2].kind == K::Scale && levels[2].max == 12);
        CHECK(page_bindings(*p, ui, pattern, true)[0].kind != K::Root); // Shift: the pad's own, not the key again
        p->tracks[0].levels_kind = uint8_t(LevelsKind::Tune);
        levels = page_bindings(*p, ui, pattern, false);
        CHECK(levels[1].kind == K::LevelsLow && levels[2].kind == K::LevelsHigh && levels[2].value == 12);
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
    synth(from->tracks[1], 1);
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
// Constants written a 44.1 kHz frame at a time follow the device's rate: at 96 kHz an effect keeps its timing, its
// corners and its density (dsp.h's fall_per_frame, step_per_frame, every_model_frames, kept), and at 44.1 kHz the
// constants are themselves.
void rate_constants() {
    dsp::device_rate.store(44100.0f);
    CHECK(dsp::fall_per_frame(0.93f) == 0.93f && dsp::step_per_frame(0.3f) == 0.3f && dsp::noise_level() == 1.0f);
    CHECK(dsp::fall_per_block(0.97f, 128) == 0.97f && dsp::rate_ratio().model == 1 && dsp::rate_ratio().device == 1);
    for (uint64_t k = 0; k < 5000; ++k) CHECK(dsp::every_model_frames(k, 12, dsp::rate_ratio()) == (k % 12 == 0));
    dsp::device_rate.store(96000.0f);
    {
        auto ratio = dsp::rate_ratio();
        CHECK(ratio.model == 147 && ratio.device == 320);
        unsigned ticks = 0; // every 4096 44.1 kHz frames: 107.7 times in ten seconds, the first on frame 0
        for (uint64_t k = 0; k < 960000; ++k) ticks += dsp::every_model_frames(k, 4096, ratio);
        CHECK(ticks == 108);
        const float fall = dsp::fall_per_frame(0.9999f); // a second of it falls as far as 44100 frames at 44.1 kHz
        float x = 1;
        for (unsigned n = 0; n < 96000; ++n) x *= fall;
        CHECK(std::fabs(x / std::pow(0.9999f, 44100.0f) - 1) < 0.01f);
    }
    // Each test below runs at 44.1 and at 96 kHz and compares the two in time.
    auto run = [](EffectKind kind, std::initializer_list<std::pair<unsigned, int>> params, float rate, float seconds, auto&& input) {
        dsp::device_rate.store(rate);
        DelayPool pool;
        auto effect = std::make_unique<Effect>();
        effect->set_kind(kind, pool);
        for (auto [index, value] : params) effect->set_param(index, value);
        std::vector<float> l(size_t(seconds * rate)), r;
        for (size_t n = 0; n < l.size(); ++n) l[n] = input(float(n) / rate);
        r = l;
        for (size_t f = 0; f < l.size(); f += 128) effect->process(l.data() + f, r.data() + f, unsigned(std::min<size_t>(128, l.size() - f)));
        effect->set_kind(EffectKind::None, pool);
        return l;
    };
    // Tremolo's square is smoothed (0.01 a 44.1 kHz frame): its edge falls from 90 % to 10 % in the same 5 ms.
    {
        double edge[2] {};
        for (unsigned k = 0; k < 2; ++k) {
            const float rate = k ? 96000.0f : 44100.0f;
            auto out = run(EffectKind::Tremolo, {{0, 90}, {1, 100}, {2, 1}}, rate, 0.05f, [](float) { return 0.5f; });
            size_t high = 0, low = 0;
            for (size_t n = 0; n < out.size() && !low; ++n) { if (!high && out[n] < 0.45f) high = n; if (high && out[n] < 0.05f) low = n; }
            edge[k] = double(low - high) / rate;
        }
        if (!(edge[0] > 0.004 && std::fabs(edge[1] / edge[0] - 1) < 0.03)) std::fprintf(stderr, "tremolo edge %g s / %g s\n", edge[0], edge[1]);
        CHECK(edge[0] > 0.004 && std::fabs(edge[1] / edge[0] - 1) < 0.03);
    }
    // Crush at RATE 100 holds each value 32 44.1 kHz frames: 1378 new values a second at either rate.
    // Pad FX Alias holds every twelfth 44.1 kHz frame: 3675 a second.
    {
        auto steps = [](const std::vector<float>& out, size_t from) {
            unsigned n = 0;
            for (size_t i = from + 1; i < out.size(); ++i) n += std::fabs(out[i] - out[i - 1]) > 1e-5f;
            return n;
        };
        auto ramp = [](float t) { return -0.9f + 0.9f * t; }; // never still: every hold is a new value
        unsigned crush[2], alias[2];
        for (unsigned k = 0; k < 2; ++k) {
            const float rate = k ? 96000.0f : 44100.0f;
            crush[k] = steps(run(EffectKind::Crush, {{0, 16}, {1, 100}, {2, 100}}, rate, 1.0f, ramp), 0);
            dsp::device_rate.store(rate);
            Punch punch;
            std::vector<float> l(static_cast<size_t>(rate)), r;
            for (size_t n = 0; n < l.size(); ++n) l[n] = ramp(float(n) / rate);
            r = l;
            punch.start(PunchKind::Alias, 1, 2 * rate, 2 * rate / 16, 1);
            for (size_t f = 0; f < l.size(); f += 128) punch.process(l.data() + f, r.data() + f, unsigned(std::min<size_t>(128, l.size() - f)));
            alias[k] = steps(l, size_t(0.01f * rate)); // after its 2 ms fade in
        }
        if (crush[0] < 1376 || crush[0] > 1380 || std::abs(int(crush[1]) - int(crush[0])) > 2 || std::abs(int(alias[1]) - int(alias[0])) > 2)
            std::fprintf(stderr, "crush %u / %u, alias %u / %u\n", crush[0], crush[1], alias[0], alias[1]);
        CHECK(crush[0] >= 1376 && crush[0] <= 1380 && std::abs(int(crush[1]) - int(crush[0])) <= 2);
        CHECK(alias[0] > 3600 && std::abs(int(alias[1]) - int(alias[0])) <= 2);
    }
    // Vinyl at CRACKLE 100: about 40 crackles a second at either rate (a chance taken each frame, kept to its share).
    {
        unsigned crackles[2];
        for (unsigned k = 0; k < 2; ++k) {
            const float rate = k ? 96000.0f : 44100.0f;
            auto out = run(EffectKind::Vinyl, {{0, 50}, {1, 100}, {2, 10}}, rate, 30.0f, [](float) { return 0.0f; });
            unsigned count = 0;
            size_t last = 0;
            for (size_t n = 0; n < out.size(); ++n)
                if (std::fabs(out[n]) > 0.02f) { if (!count || n - last > size_t(0.002f * rate)) ++count; last = n; }
            crackles[k] = count;
        }
        if (!(crackles[0] > 30 * 30 && crackles[0] < 45 * 30 && std::fabs(float(crackles[1]) / float(crackles[0]) - 1) < 0.15f))
            std::fprintf(stderr, "crackles %u / %u in 30 s\n", crackles[0], crackles[1]);
        CHECK(crackles[0] > 30 * 30 && crackles[0] < 45 * 30 && std::fabs(float(crackles[1]) / float(crackles[0]) - 1) < 0.15f);
    }
    dsp::device_rate.store(44100.0f);
}

void device_rates() {
    // The sequencer: a new rate waits, silent, for the project again; then at 48 kHz step 5 at 120 BPM starts 0.5 s in.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        synth(project->tracks[0], 0);
        project->tempo = 1200;
        put(bank->at[0][0], 4, 45, 100);
        auto host = std::make_unique<EngineHost>();
        host->load(*project, *bank);
        tones_for(*host, *project);
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
    // The Record meter falls in the same time at any rate: 0.95 a 128-frame block at 44.1 kHz.
    {
        dsp::device_rate.store(96000.0f);
        auto take = std::make_unique<TakeRecorder>();
        std::vector<float> loud(128, 1.0f), quiet(128, 0.0f);
        take->process(loud.data(), loud.data(), 128);
        for (unsigned b = 0; b < 75; ++b) take->process(quiet.data(), quiet.data(), 128); // 0.1 s
        CHECK(std::fabs(take->level() - std::pow(0.95f, 75 * 128 / (96000.0f / 44100.0f) / 128)) < 1e-3f);
    }
    // A take at 96 kHz is kept as it came, never converted on its way in: a second is 96000 frames of its own, 44100
    // as the project counts them. As a sample its 44.1 kHz frames are converted once, band-limited: 1 kHz stays
    // 1 kHz, 30 kHz (above what 44.1 kHz holds) is filtered out, not folded down; its own frames are what plays.
    {
        dsp::device_rate.store(96000.0f);
        for (float hz : {1000.0f, 30000.0f}) {
            auto take = std::make_unique<TakeRecorder>();
            CHECK(take->take_rate() == 96000); // made at the device's rate
            take->arm(-100, 0, 0);
            std::vector<float> l(128), r(128), heard;
            for (unsigned b = 0; b < 750; ++b) {
                for (unsigned f = 0; f < 128; ++f) l[f] = r[f] = 0.5f * std::sin(2 * 3.14159265f * hz * float(b * 128 + f) / 96000.0f);
                heard.insert(heard.end(), l.begin(), l.end());
                take->process(l.data(), r.data(), 128);
            }
            take->stop();
            CHECK(take->state() == TakeRecorder::State::Done && take->taken() == 96000 && take->take_rate() == 96000 && take->frames() == 44100);
            CHECK(std::equal(heard.begin(), heard.end(), take->left()));
            SampleData kept;
            kept.left.assign(take->left(), take->left() + take->taken());
            kept.right.assign(take->right(), take->right() + take->taken());
            CHECK(keep_capture(kept, take->take_rate(), "", 96000).empty());
            CHECK(kept.frames == 44100 && kept.plays_at(96000) && kept.plays_own() && kept.play_frames == 96000);
            if (hz < 2000) {
                unsigned crossings = 0;
                for (unsigned n = 1001; n < 41000; ++n) crossings += (kept.left[n - 1] < 0) != (kept.left[n] < 0);
                CHECK(std::fabs(crossings / 2.0f / (40000 / 44100.0f) - 1000) < 5);
                CHECK(peak_of(kept.left, 1000) > 0.45f);
            } else CHECK(peak_of(kept.left, 1000) < 0.03f);
        }
        // Resample at 96 kHz: a second rendered is 96000 frames of its own, 44100 as the project counts them.
        auto bounce = std::make_unique<Bounce>();
        bounce->cue(); bounce->begin(0);
        CHECK(bounce->rate() == 96000);
        std::vector<float> l(128, 0.25f), r(128, 0.25f), drained(4096 * 2);
        unsigned popped = 0;
        for (unsigned b = 0; b < 750; ++b) { bounce->process(l.data(), r.data(), 128); popped += bounce->pop(drained.data(), 4096); }
        CHECK(bounce->taken() == 96000 && bounce->frames() == 44100 && popped == bounce->taken() && drained[0] == 0.25f);
        // The device's rate changes meanwhile: the render is dropped, never kept with its rate changing partway.
        bounce->cue(); bounce->begin(0);
        bounce->process(l.data(), r.data(), 128);
        bounce->set_rate(48000);
        CHECK(bounce->state() == Bounce::State::Cancelled);
        // Audio Recall at 48 kHz: the ring keeps the source's own frames, 25 s of them; a new rate starts it again.
        dsp::device_rate.store(48000.0f);
        auto ring = std::make_unique<RecallRing>();
        CHECK(ring->rate() == 48000);
        std::vector<float> ramp_l(128), ramp_r(128);
        for (uint32_t block = 0; block < 27 * 48000 / 128; ++block) {
            for (unsigned f = 0; f < 128; ++f) { ramp_l[f] = float(block * 128 + f); ramp_r[f] = -ramp_l[f]; }
            ring->write(ramp_l.data(), ramp_r.data(), 128);
        }
        uint32_t written = 27 * 48000 / 128 * 128;
        CHECK(ring->filled() == 25 * 48000);
        std::vector<float> rl(25 * 48000), rr(25 * 48000);
        CHECK(ring->copy_last(25 * 48000, rl.data(), rr.data()));
        CHECK(rl.back() == float(written - 1) && rl.front() == float(written - 25 * 48000) && rr.back() == -rl.back());
        ring->set_rate(96000);
        CHECK(ring->filled() == 0 && ring->rate() == 96000);
    }
    // A take at 48 kHz (Settings' rate), round trip: saved as a 48 kHz file whose frames are what was heard (to the
    // 24-bit step), read back with its own frames playing, and its region and a cut made while it ran line up with
    // the sound, counted in the project's 44.1 kHz frames.
    {
        dsp::device_rate.store(48000.0f);
        auto take = std::make_unique<TakeRecorder>();
        take->arm(-100, 0, 3);
        const uint32_t hit = 48000, own = 1125 * 128; // a hit a second in, after a quiet hum; 1.5 s in all
        std::vector<float> l(128), r(128), heard_l, heard_r;
        uint32_t cut = 0;
        for (unsigned b = 0; b < 1125; ++b) {
            for (unsigned f = 0; f < 128; ++f) {
                uint32_t n = b * 128 + f;
                float hum = 0.01f * std::sin(2 * 3.14159265f * 220 * float(n) / 48000.0f);
                l[f] = hum + (n >= hit && n < hit + 4800 ? 0.8f * std::sin(2 * 3.14159265f * 1000 * float(n - hit) / 48000.0f) : 0.0f);
                r[f] = -0.5f * l[f];
            }
            if (b * 128 == hit) cut = take->frames(); // a pad tapped as the hit comes: the cut (Context::record_pad)
            heard_l.insert(heard_l.end(), l.begin(), l.end()); heard_r.insert(heard_r.end(), r.begin(), r.end());
            take->process(l.data(), r.data(), 128);
        }
        take->stop();
        CHECK(take->taken() == own && take->take_rate() == 48000 && take->frames() == 132300 && cut == 44100);
        std::string path = "/tmp/melodee-take48-test.wav";
        SampleData kept; // as SampleLibrary::land keeps it
        kept.left.assign(take->left(), take->left() + take->taken());
        kept.right.assign(take->right(), take->right() + take->taken());
        CHECK(keep_capture(kept, take->take_rate(), path, 48000).empty());
        CHECK(kept.frames == take->frames() && kept.plays_at(48000) && kept.plays_own() && kept.play_frames == own);
        SampleData read;
        WavInfo info;
        CHECK(read_wav(path, read, 60, &info, 48000).empty() && info.rate == 48000 && info.channels == 2);
        CHECK(read.plays_at(48000) && read.plays_own() && read.play_frames == own && read.play_left == kept.play_left
              && read.play_right == kept.play_right);
        float worst = 0;
        for (uint32_t n = 0; n < own; ++n)
            worst = std::max({worst, std::fabs(read.play_left[n] - heard_l[n]), std::fabs(read.play_right[n] - heard_r[n])});
        CHECK(worst <= 2.0f / 8388607);
        // Its 44.1 kHz frames read back the same (a new rate reads it again from the file), the hit where the cut was.
        CHECK(read.frames == kept.frames && read.left == kept.left && read.right == kept.right);
        std::array<Hit, max_hits> hits {};
        unsigned count = find_hits(read, hits);
        bool found = false;
        for (unsigned h = 1; h < count; ++h) found |= hits[h].frame + 220 >= cut && hits[h].frame <= cut + 220;
        CHECK(found);
        // A pad plays its own frames, a frame for each: the hit a second in, the whole region 1.5 s.
        SamplerEngine::Table table {};
        table[0] = &read;
        SamplerEngine engine;
        engine.set_samples(&table);
        engine.set_tempo(1200);
        PadSound sound; sound.sample = 0; sound.decay = 100;
        engine.set_pad(0, sound);
        engine.note_on(int(sampler_first_note), 120);
        std::vector<float> pl(128), pr(128), played;
        for (unsigned b = 0; b < 3000 && engine.sounding(); ++b) {
            std::fill(pl.begin(), pl.end(), 0.0f); std::fill(pr.begin(), pr.end(), 0.0f);
            engine.render_stereo(pl.data(), pr.data(), 128);
            played.insert(played.end(), pl.begin(), pl.end());
        }
        size_t first_loud = 0;
        while (first_loud < played.size() && std::fabs(played[first_loud]) < 0.2f) ++first_loud;
        if (!(std::fabs(float(played.size()) - float(own)) < 600 && first_loud + 64 >= hit && first_loud <= hit + 64))
            std::fprintf(stderr, "take48: %zu frames played, loud from %zu\n", played.size(), first_loud);
        CHECK(std::fabs(float(played.size()) - float(own)) < 600 && first_loud + 64 >= hit && first_loud <= hit + 64);
        // The mixdown's file says its rate too.
        WavStream wav;
        CHECK(wav.open(path, 48000) && wav.add(std::vector<float>(256, 0.1f).data(), 128) && wav.close());
        SampleData mix;
        CHECK(read_wav(path, mix, 60, &info, 48000).empty() && info.rate == 48000 && mix.play_frames == 128 && mix.plays_own());
        std::remove(path.c_str());
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
    synth(project->tracks[0], 0);
    auto host = std::make_unique<EngineHost>();
    host->load(*project, *bank);
    tones_for(*host, *project);
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
    tones_for(*host, *project);
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
        const uint32_t rate = 44100, pad = rate / 100;
        uint32_t a0 = rate / 2, a1 = a0 + rate / 2, b0 = a1 + 2 * rate, b1 = b0 + rate / 2, total = b1 + rate / 2;
        std::vector<float> l(total, 0.0f), r(total, 0.0f);
        for (uint32_t f = a0; f < a1; ++f) l[f] = 0.4f;
        for (uint32_t f = b0; f < b1; ++f) r[f] = -0.4f;
        RecallOptions o;
        uint32_t first = 0, end = 0;
        CHECK(recall_bounds(l.data(), r.data(), total, rate, o, first, end) && first == b0 - pad && end == b1 + pad);
        o.gap = 4;
        CHECK(recall_bounds(l.data(), r.data(), total, rate, o, first, end) && first == a0 - pad && end == b1 + pad);
        o.grab = 1; o.gap = 1;
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
        p.tracks[0].engine = EngineKind::Plugin; p.tracks[2].engine = EngineKind::Plugin;
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
        CHECK(edit_page_count(info) == 6 + unsigned(midi_track::slot_pages) && !is_mod_page(info, 0) && !is_mod_page(info, 1)); // roadmap P4: slots, pads, express, audio, SysEx
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
        CHECK(std::string(midi_row_name(PanicRow)) == "MIDI panic" && PanicRow + 1 == ResetRow);
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
        synth(project->tracks[1], 1);
        project->tempo = 1200;
        put(bank->at[0][0], 0, 60, 100, 2);
        bank->at[0][1].length = 4 * project->bar_ticks(); // pattern 2: four bars, one long note
        put(bank->at[0][1], 0, 62, 100, 64);
        EngineHost host;
        host.load(*project, *bank);
        tones_for(host, *project);
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
        host.send({Command::Type::Engine, 0, 0, uint16_t(EngineKind::Plugin)});
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
        tones_for(host, *project);
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
    // Every engine's table: three buttons of one to four pages (a kit's B1 five: LAYERS beside TRIM, roadmap P3),
    // each named, its parameters the engine's own.
    for (unsigned k = 0; k < unsigned(EngineKind::Count); ++k) {
        const auto& g = stage_groups(EngineKind(k));
        const auto& info = engine_info(EngineKind(k));
        for (const auto& group : g.groups) {
            CHECK(group.count >= 1 && group.count <= (EngineKind(k) == EngineKind::Sampler && &group == &g.groups[0] ? 5 : 4));
            for (unsigned n = 0; n < group.count; ++n) {
                const auto& page = group.pages[n];
                CHECK(page.key && std::strlen(page.key) > 0);
                for (unsigned c = 0; c < 3; ++c) CHECK(page.params[c] < int(info.param_count) && page.shift[c] < int(info.param_count));
                if (page.picture == StagePicture::Envelope) CHECK(page.params[0] >= 0 && page.params[1] >= 0 && page.params[2] >= 0 && page.shift[0] >= 0);
            }
        }
    }
    CHECK(stage_groups(EngineKind::Plugin).groups[0].pages[0].kind == StageKind::Mix);
    CHECK(stage_groups(EngineKind::Sampler).groups[0].pages[0].kind == StageKind::Trim && stage_groups(EngineKind::Sampler).groups[1].count == 4);
    CHECK(stage_groups(EngineKind::None).groups[2].pages[0].kind == StageKind::NewMidi);

    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    synth(project->tracks[0], 0);
    load_sound(project->tracks[1], EngineKind::Sampler, 0);
    synth(project->tracks[2], 2);
    load_sound(project->tracks[3], EngineKind::Midi, 0);
    const Pattern& pattern = bank->at[0][0];
    UiState ui;
    ui.page = PageId::Stage;
    auto key = [&] { return std::string(stage_page_now(ui, project->tracks[ui.track]).key); };
    auto cards = [&](bool shift) { return page_bindings(*project, ui, pattern, shift); };

    // B1 on a synth (a PLUGIN track): MIX -> SENDS -> MIX; B2 jumps to its own first page; each track keeps its own.
    CHECK(key() == "MIX");
    stage_press(ui, project->tracks[0], 0); CHECK(key() == "SENDS");
    stage_press(ui, project->tracks[0], 0); CHECK(key() == "MIX");
    stage_press(ui, project->tracks[0], 1); CHECK(key() == "ARP");
    stage_press(ui, project->tracks[0], 1); CHECK(key() == "CHORD");
    ui.track = 1; CHECK(key() == "TRIM");
    stage_press(ui, project->tracks[1], 2); CHECK(key() == "FILTER");
    ui.track = 0; CHECK(key() == "CHORD");
    stage_press(ui, project->tracks[0], 0); CHECK(key() == "MIX"); // another button: its first page

    // MIX: level, pan, send; Shift + K3 the bus.
    stage_go(ui, 0, 0);
    CHECK(cards(false)[0].kind == K::Level && cards(false)[1].kind == K::Pan && cards(false)[2].kind == K::Send && cards(false)[2].id == 0);
    CHECK(!shift_bindings(*project, ui, pattern)[2].bound()); // the quick send is A's; SENDS has all four
    // SENDS (B1 again): K1 the bus, K2 the send into it, K3 its return; every bus, D too, without Shift.
    stage_go(ui, 0, 1);
    ui.sends_bus = 0;
    CHECK(key() == "SENDS" && cards(false)[0].kind == K::SendBus && cards(false)[1].kind == K::Send && cards(false)[1].id == 0);
    CHECK(cards(false)[2].kind == K::BusReturn && cards(false)[2].id == 0 && !shift_bindings(*project, ui, pattern)[0].bound());
    ui.sends_bus = 3;
    CHECK(cards(false)[0].value == 3 && cards(false)[1].id == 3 && cards(false)[2].id == 3);
    // ARP: mode, rate, octaves; Shift + K1 gate, Shift + K3 hold.
    stage_go(ui, 1, 0);
    CHECK(cards(false)[0].kind == K::ArpMode && cards(false)[1].kind == K::ArpRate && cards(true)[0].kind == K::ArpGate && cards(true)[2].kind == K::ArpHold);
    // CHORD: type, voicing, inversion; Shift + K1 chords on or off, Shift + K3 hold.
    stage_go(ui, 1, 1);
    CHECK(key() == "CHORD" && cards(false)[0].kind == K::ChordType && cards(false)[2].kind == K::ChordInvert);
    CHECK(cards(true)[0].kind == K::ChordOn && cards(true)[2].kind == K::ArpHold);
    // FX: nothing without effects; with one, its first row and (Shift) its second.
    stage_go(ui, 2, 0);
    CHECK(!cards(false)[0].bound());
    project->tracks[0].fx.slots[0].kind = uint8_t(EffectKind::Echo);
    CHECK(cards(false)[0].kind == K::FxParam && cards(false)[0].id == 0 * 64 + 0 * 8 + 0);
    CHECK(!cards(true)[0].bound() || cards(true)[0].id == 3);

    // A kit: Akai's pages on the last pad hit. TRIM start, end, loop, zoom on Shift; MIX K2 the kit's volume;
    // AMP ENV Shift + K2 Decay From; TUNE Shift + K3 # Beats; PLAY Shift + K2 Pad Link.
    ui.track = 1;
    auto& kit = project->tracks[1];
    kit.pads[5].sample = 0; project->samples[0].frames = 44100; kit.last_pad = 5;
    stage_go(ui, 0, 0);
    CHECK(cards(false)[0].kind == K::PadStart && cards(false)[1].kind == K::PadEnd && cards(false)[2].kind == K::PadLoopPoint);
    CHECK(cards(true)[0].kind == K::TrimZoom);
    stage_go(ui, 0, 1); // LAYERS (roadmap P3): the layer in hand, its velocities; Shift + K1 how a hit picks
    CHECK(key() == "LAYERS" && cards(false)[0].kind == K::LayerPick && cards(false)[1].kind == K::LayerLow && cards(true)[0].kind == K::LayerPlayMode);
    stage_go(ui, 0, 2);
    CHECK(cards(false)[0].kind == K::PadLevel && cards(false)[1].kind == K::Param && cards(false)[1].id == sampler::Level && cards(false)[2].kind == K::PadPan);
    stage_go(ui, 0, 3);
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

    // An empty track: SYNTH (a plug-in: Melodee's synths are plug-ins), KIT (where its sample comes from), MIDI (port,
    // channel).
    ui.track = 9;
    CHECK(key() == "SYNTH");
    std::array<EngineKind, size_t(EngineKind::Count)> engines {};
    unsigned count = new_engines(engines);
    CHECK(count == 1 && engines[0] == EngineKind::Plugin);
    // Three doors on one page, whichever key is lit: K1 the engine, K2 the kit's source, K3 the MIDI port.
    for (unsigned door = 0; door < 3; ++door) {
        stage_go(ui, door, 0);
        auto doors = cards(false);
        CHECK(doors[0].kind == K::NewEngine && doors[1].kind == K::NewSource && doors[1].max == int(new_sources) - 1 && doors[2].kind == K::NewPort);
        CHECK(!shift_bindings(*project, ui, pattern)[0].bound()); // no Shift row on the doors
    }
    stage_go(ui, 0, 0);
    ui.last_pot = 2; // the pot touched last picks the door Push makes, on an empty track too
    CHECK(stage_focus(ui, project->tracks[9]) == 2 && stage_focus(ui, project->tracks[0]) == 2);
    ui.last_pot = -1;

    // Tracks: K1 the track, K2 its level, K3 its pan.
    ui.page = PageId::Tracks; ui.track = 0;
    CHECK(cards(false)[0].kind == K::TrackPick && cards(false)[1].kind == K::Level && cards(false)[2].kind == K::Pan);

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
    CHECK(tag != bytes.end() && tag[8] == 3 && tag[9] == 3); // version 3 since roadmap P4 (device, drum map, SysEx after it)
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
        tones_for(host, *p2);
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
// Roll opening on a line whose notes all lie outside the rows in view (a bass in octave 2 under a cursor on C4):
// the cursor's pitch goes to the note nearest its step, so the notes are in view; a note in view leaves it.
void roll_opens_on_notes() {
    Pattern p;
    p.length = 16 * sixteenth_ticks;
    auto add = [&](unsigned step, uint8_t key) { Event e; e.tick = step * sixteenth_ticks; e.key = key; CHECK(p.add(e)); };
    add(0, 38); add(4, 41); add(9, 45);
    UiState ui;
    ui.step_cursor = 8; ui.roll_key = 62; ui.roll_top = 65; // F4 down to A#3 in view: none of the notes
    roll_open(p, ui);
    CHECK(ui.roll_key == 45 && ui.roll_top >= 46 && ui.roll_top - int(roll_rows) + 1 <= 45); // step 9's A2, the nearest
    CHECK(roll_focus(p, ui) < 0);                                                              // the step stays: no focus forced
    unsigned top = ui.roll_top; ui.roll_key = 44;
    roll_open(p, ui); CHECK(ui.roll_key == 44 && ui.roll_top == top);                         // notes in view: as left
    Pattern empty; ui.roll_key = 80; ui.roll_top = 85;
    roll_open(empty, ui); CHECK(ui.roll_key == 80 && ui.roll_top == 85);                      // nothing to find
}

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
    { // Round 2: NUDGE on Shift + K1, the note in focus off its nearest grid line (1/16: -12 .. 11 ticks).
        Pattern n;
        n.length = p.length;
        Event late; late.tick = 20 * sixteenth_ticks + 5; late.key = 50; CHECK(n.add(late));
        Event early; early.tick = 30 * sixteenth_ticks - 4; early.key = 51; CHECK(n.add(early));
        CHECK(nudge_base(n, late) == 20 * sixteenth_ticks && nudge_base(n, early) == 30 * sixteenth_ticks);
        auto card = nudge_card(n, late);
        CHECK(card.kind == Binding::Kind::EventNudge && card.value == 5 && card.min == -12 && card.max == 11);
        CHECK(nudge_card(n, early).value == -4);
        Project project;
        UiState at; at.page = PageId::Roll; at.step_cursor = 20; at.roll_key = 50;
        auto shift = shift_bindings(project, at, n);
        CHECK(shift[0].kind == Binding::Kind::EventNudge && shift[0].value == 5 && !shift[1].bound());
        at.trig_cards = true; // the TRIG row keeps Shift + K1 free
        CHECK(!shift_bindings(project, at, n)[0].bound());
        // Step Edit as step entry: K1-K3 what a pad writes; Shift + K1-K3 the pattern's length, Q, the signature.
        UiState entry; entry.page = PageId::StepEdit; entry.entry_length = 2; entry.entry_velocity = 0; entry.entry_chance = 80;
        auto cards = base_bindings(project, entry, n);
        CHECK(cards[0].kind == Binding::Kind::EntryLength && cards[0].value == 2 && cards[1].kind == Binding::Kind::EntryVelocity
              && cards[1].value == 0 && cards[1].min == 0 && cards[2].kind == Binding::Kind::EntryChance && cards[2].value == 80);
        auto second = shift_bindings(project, entry, n);
        CHECK(second[0].kind == Binding::Kind::PatternLength && second[1].kind == Binding::Kind::Division && second[2].kind == Binding::Kind::TimeSignature);
    }
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
    synth(project.tracks[0], 0);
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
// The Looper at the device's rate: a take at 48 kHz keeps the frames as they came (no conversion), its memory
// made 64 s at that rate; at 96 kHz the loop plays on the beat from its own frames, an empty loop's memory is made
// again at 96 kHz and a take there is at 96 kHz; a 44.1 kHz loop (an older project's file) plays in time at 96 kHz.
void looper_rates() {
    auto lp = std::make_unique<Looper>();
    Looper& looper = *lp;
    float out_l[loop_tracks][looper::block], out_r[loop_tracks][looper::block];
    float src_l[looper::block] {}, src_r[looper::block] {};
    auto signal = [](uint64_t f) { return float(0.3 * std::sin(double(f) * 0.0137) + 0.1 * std::sin(double(f) * 0.29)); };
    double tick = 0;
    uint64_t frame = 0;
    bool restart = true;
    std::vector<float> heard;
    auto block = [&](float rate) {
        const double per = 96.0 * 240 / 60.0 / rate; // 240 BPM: a bar of 4/4 is a second
        Looper::Clock c;
        c.tick = tick; c.per_frame = per; c.bar = 384; c.beat = 96; c.running = true; c.restart = restart; c.rate = rate;
        restart = false;
        Looper::Sources s;
        s.frames = frame ? looper::block : 0;
        s.at[looper::source_input] = {src_l, src_r};
        looper.run(c, s, looper::block, out_l, out_r);
        for (unsigned f = 0; f < looper::block; ++f) {
            heard.push_back(out_l[0][f]);
            src_l[f] = signal(frame + f); src_r[f] = -src_l[f];
        }
        frame += looper::block;
        tick += looper::block * per;
        looper.service();
    };
    dsp::device_rate.store(48000.0f);
    looper.ask(looper::Do::Enable, 0, 1);
    for (unsigned n = 0; n < 20; ++n) block(48000);
    CHECK(looper.buffers_out() == 2 * loop_tracks);
    // A bar at 48 kHz: 48000 frames of its own, the tempo right, and played back frame for frame a bar later.
    looper.ask(looper::Do::Length, 0, 1);
    while (tick < 384 - 2 * looper::block * 96.0 * 4 / 48000) block(48000);
    looper.ask(looper::Do::Primary, 0, 0);
    while (tick < 768 + 4 * looper::block * 96.0 * 4 / 48000) block(48000);
    auto v = looper.view(0);
    CHECK(v.state == Looper::State::Playing && v.length == 48000 && v.rate == 48000 && v.tempo == 2400 && !v.stretched);
    const uint64_t take_start = 48000; // bar 2's frame
    while (tick < 1152 - 4 * looper::block * 96.0 * 4 / 48000) block(48000);
    float worst = 0;
    for (uint64_t f = take_start + 48000 + 2000; f < take_start + 96000 - 2000; ++f)
        worst = std::max(worst, std::fabs(heard[f] - signal(f - 48000)));
    if (worst > 1e-6f) std::fprintf(stderr, "looper 48k: worst %g\n", double(worst));
    CHECK(worst <= 1e-6f); // the frames themselves: nothing converted on the way in or out
    // The device goes to 96 kHz (the same project kept): the 48 kHz loop plays on the beat, a bar a second, not
    // stretched; empty loops' memory is made again at 96 kHz and the loop's own stays.
    dsp::device_rate.store(96000.0f);
    size_t from = heard.size();
    for (unsigned n = 0; n < 750; ++n) block(96000); // a second
    v = looper.view(0);
    CHECK(v.length == 48000 && v.rate == 48000 && !v.stretched && looper.buffers_out() == 2 * loop_tracks);
    // What it plays at 96 kHz is its 48 kHz frames read half a frame a frame: frame g is the take's g / 2.
    double phase = 0; // the loop's place at the first frame heard here, in its frames
    {
        double t0 = tick - 750.0 * looper::block * 96.0 * 4 / 96000; // the tick at `from`
        phase = std::fmod(t0, 384.0) / 384.0 * 48000;
    }
    worst = 0;
    for (size_t g = 4000; g < 90000; g += 7) {
        double at = std::fmod(phase + g * 0.5, 48000.0);
        if (at < 200 || at > 47800) continue; // clear of the seam's fade
        // the take's frame `at` is the source's frame take_start + at (as looper_engine finds it a bar on)
        double x = at;
        float a = signal(uint64_t(std::floor(x)) + take_start), b = signal(uint64_t(std::floor(x)) + 1 + take_start);
        float want = a + (b - a) * float(x - std::floor(x));
        worst = std::max(worst, std::fabs(heard[from + g] - want));
    }
    if (worst > 0.02f) std::fprintf(stderr, "looper 48k at 96k: worst %g\n", double(worst));
    CHECK(worst <= 0.02f);
    // A take on loop 2 now is at 96 kHz: a bar is 96000 frames.
    looper.ask(looper::Do::Length, 1, 1);
    looper.ask(looper::Do::Primary, 1, 0);
    for (unsigned n = 0; n < 4 * 750; ++n) block(96000);
    v = looper.view(1);
    CHECK(v.state == Looper::State::Playing && v.length == 96000 && v.rate == 96000 && v.tempo == 2400 && !v.stretched);
    // A loop saved at 44.1 kHz (a project from before) comes back and plays a bar a second at 96 kHz.
    auto* buffer = new LoopBuffer(looper::frames_at(96000));
    for (unsigned f = 0; f < 44100; ++f) buffer->left[f] = buffer->right[f] = 0.25f;
    LoopLoad item; item.buffer = buffer; item.loop = 2; item.frames = 44100; item.rate = 44100; item.ticks = 384; item.tempo = 2400;
    CHECK(looper.give_load(item));
    for (unsigned n = 0; n < 8; ++n) block(96000);
    v = looper.view(2);
    CHECK(v.state == Looper::State::Playing && v.length == 44100 && v.rate == 44100 && !v.stretched);
    looper.ask(looper::Do::Enable, 0, 0);
    for (unsigned n = 0; n < 8; ++n) block(96000);
    CHECK(looper.buffers_out() == 0);
    dsp::device_rate.store(44100.0f);
}

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
    auto* buffer = new LoopBuffer(looper::frames_at(44100));
    for (unsigned f = 0; f < 1000; ++f) buffer->left[f] = buffer->right[f] = 0.5f;
    LoopLoad item; item.buffer = buffer; item.loop = 3; item.frames = 1000; item.ticks = 96; item.tempo = 2400;
    CHECK(looper.give_load(item));
    for (unsigned n = 0; n < 4; ++n) block_at(per);
    v = looper.view(3);
    CHECK(v.state == Looper::State::Playing && v.length == 1000 && v.tempo == 2400 && looper.buffers_out() == 2 * loop_tracks);
    item.buffer = new LoopBuffer(looper::frames_at(44100)); item.stopped = true; // saved stopped: back stopped
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
    synth(project->tracks[0], 0);
    project->looper.on = true;
    project->looper.loops[0].source = looper::source_track(0);
    project->looper.loops[0].bars = 1;
    project->tempo = 2400;
    auto host = std::make_unique<EngineHost>();
    host->load(*project, *bank);
    tones_for(*host, *project);
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
        // roadmap P3: every articulation is a keyswitch's zone and a release sample plays on the key let go; a legato's is left out
        CHECK(samples(read) == S({"note.wav", "other.wav", "tail.wav", "first.wav"}));
        CHECK(read.left == 1 && read.left_by == std::set<std::string>({"trigger"}) && read.regions[2].zone.on_release);
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
    {   // a pick at random splits the dice; a release trigger's layers play on the key let go (roadmap P3)
        KeygroupProgram picked;
        picked.type = "Keygroup";
        KeygroupInstrument one;
        one.tags = {{"ZonePlay", "2"}};
        one.layers = {{{"SampleName", "x"}}, {{"SampleName", "y"}, {"Loop", "True"}}};
        KeygroupInstrument off = one;
        off.tags = {{"TriggerMode", "1"}};
        picked.instruments = {one, off};
        auto r = read_keygroup(picked);
        CHECK(r.regions.size() == 4 && r.left == 0 && !r.regions[1].zone.on_release && r.regions[2].zone.on_release && r.regions[3].zone.one_shot);
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
    CHECK(levels_of(*project, kit, 0).semitones == 2 - 36); // the sample a C, the root D, three octaves down
    kit.levels_kind = uint8_t(LevelsKind::Velocity);
    CHECK(levels_of(*project, kit, 0).semitones == 0);
    auto& synth = project->tracks[1];
    synth.engine = EngineKind::Plugin;
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
        const EffectKind chains[][2] {{EffectKind::Reverb, EffectKind::Echo}, {EffectKind::Chorus, EffectKind::Drive},
                                      {EffectKind::Phaser, EffectKind::Plate}, {EffectKind::Crush, EffectKind::Filter},
                                      {EffectKind::Compressor, EffectKind::Room}, {EffectKind::TapeEcho, EffectKind::Width},
                                      {EffectKind::Flanger, EffectKind::Shimmer}, {EffectKind::Tape, EffectKind::PingPong}};
        for (unsigned t = 0; t < 8; ++t) {
            if (t % 2) tone_kit(*p, t); else synth(p->tracks[t], t); // synths (plug-ins) and kits in turn
            p->tracks[t].mix.pan = int8_t(int(t) * 12 - 42);
            p->tracks[t].mix.sends[t % bus_count] = uint8_t(20 + t * 5);
            p->tracks[t].mix.sends[(t + 1) % bus_count] = uint8_t(10 + t * 3); // two sends a track: two buses fed
            for (unsigned k = 0; k < 2; ++k) p->tracks[t].fx.slots[k] = {uint8_t(chains[t][k]), false, {60, 50, 40, 50, 30, 0}};
        }
        p->buses[1].fx.slots[0] = {uint8_t(EffectKind::Reverb), false, {70, 50, 40, 50, 0, 0}};
        EngineHost host;
        host.set_render_threads(workers, 0);
        host.load(*p, *b);
        auto tones = std::make_unique<Tones>();
        tones->offer(host, *p);
        auto tone = tone_sample();
        CHECK(host.offer_sample(0, &tone));
        std::vector<int32_t> block(256), all;
        for (unsigned n = 0; n < 600; ++n) {
            if (n % 50 == 0)
                for (unsigned t = 0; t < 8; ++t) { // a kit's notes are its first four pads
                    auto key = [&](unsigned at) { return uint16_t(t % 2 ? sampler_first_note + (t + at) % 4 : 48 + t + at % 2 * 7); };
                    host.send({Command::Type::NoteOff, uint8_t(t), 0, key(n / 50 + 1), 0});
                    host.send({Command::Type::NoteOn, uint8_t(t), 0, key(n / 50), int16_t(90 + t)});
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

// Roadmap P2, one dispatch a block: a busy project (synths as plug-ins and kits of samples; ratchets with ramps,
// slides and ties, accents, Motion and locks on engine values, kit pads, effects and the wheel; an arp swung; notes,
// pads and values in by hand at their frames, mid-block) plays the same as the old way, where every track was cut
// wherever any track had a tick or a note and the workers were called for each run: the same to the sample, cut
// where the old way cut it or whole between each engine's own calls, with the workers or without.
void one_dispatch() {
    SampleData tone;
    tone.frames = 22050; tone.left.resize(22050); tone.right.resize(22050);
    for (unsigned n = 0; n < 22050; ++n) tone.left[n] = tone.right[n] = float(std::sin(n * 0.05) * 0.4 * std::exp(-double(n) / 6000.0));
    auto play = [&](bool whole, bool cut_as_split, unsigned workers, std::vector<float>* stems_out) {
        auto p = std::make_unique<Project>();
        auto b = std::make_unique<PatternBank>();
        for (unsigned t = 0; t < 8; ++t) {
            if (t == 4 || t == 5) tone_kit(*p, t); else synth(p->tracks[t], t);
            p->tracks[t].mix.pan = int8_t(int(t) * 12 - 42);
            p->tracks[t].mix.sends[t % bus_count] = uint8_t(20 + t * 5);
        }
        p->tracks[7].fx.slots[0] = {uint8_t(EffectKind::Filter), false, {60, 50, 40, 50, 30, 0}};
        p->tracks[1].fx.slots[0] = {uint8_t(EffectKind::Chorus), false, {60, 50, 40, 50, 30, 0}};
        p->buses[0].fx.slots[0] = {uint8_t(EffectKind::Reverb), false, {70, 50, 40, 50, 0, 0}};
        for (unsigned t : {4u, 5u})
            for (unsigned pad = 0; pad < 8; ++pad) { p->tracks[t].pads[pad].sample = 0; p->tracks[t].pads[pad].play = PadPlay::OneShot; }
        p->tempo = 1370;
        p->swing = 58;
        auto& bass = b->at[0][0]; // slides, ties and an accent
        put(bass, 0, 36, 100, 2, step_slide); put(bass, 2, 43, 90, 1, step_tie); put(bass, 3, 43, 80); put(bass, 6, 39, 110, 1, step_accent);
        put(bass, 9, 41, 70, 3); put(bass, 13, 48, 100, 1, step_slide); put(bass, 14, 36, 90);
        CHECK(bass.write_motion(point(1, engine_lane, 20)) && bass.write_motion(point(5, engine_lane, 90, 0, 6)) && bass.write_motion(point(10, engine_lane + 1, 70)));
        CHECK(bass.write_motion(point(7, motion_wheel, 120)) && bass.write_motion(point(12, motion_wheel, 10)));
        for (unsigned t : {1u, 3u, 6u}) {
            auto& q = b->at[t][0];
            for (unsigned s = 0; s < 16; s += 1 + t % 3) put(q, s, uint8_t(55 + (s * 5 + t) % 17), uint8_t(60 + s * 3), 1, s % 5 == 0 ? step_accent : 0);
            q.events[1].ratchet = 3; q.events[1].ramp = -40;
            q.events[3].ratchet = 5; q.events[3].ramp = 60;
        }
        CHECK(b->at[6][0].write_motion(point(2, engine_lane, 10, 0, 12)) && b->at[6][0].write_motion(point(9, engine_lane + 2, 80)));
        CHECK(b->at[7][0].write_motion(point(4, motion_fx_base + 0, 10)) && b->at[7][0].write_motion(point(11, motion_fx_base + 1, 90, 0, 24)));
        put(b->at[7][0], 0, 60, 100, 8); put(b->at[7][0], 8, 67, 100, 8);
        auto& drums = b->at[4][0]; // a kit's pads 1, 3 and 7
        for (unsigned s = 0; s < 16; ++s) {
            Step st;
            st.notes[0] = {uint8_t(sampler_first_note + (s % 4 == 0 ? 0 : s % 2 ? 6 : 2)), uint8_t(80 + s)};
            drums.set_step(s, st);
        }
        drums.events[2].ratchet = 4; drums.events[2].ramp = -60;
        auto& kit = b->at[5][0];
        for (unsigned s = 0; s < 16; s += 2) put(kit, s, uint8_t(sampler_first_note + s / 2 % 4), 100);
        kit.events[1].ratchet = 3; kit.events[1].pitch_ramp = 7;
        CHECK(kit.write_motion(point(3, motion_pad_base + unsigned(PadParam::Pitch), 5, 1)) &&
              kit.write_motion(point(6, motion_pad_base + unsigned(PadParam::FilterCutoff), 30, 0, 12)));
        EngineHost host;
        host.set_render_threads(workers, 0);
        host.set_one_dispatch(whole);
        host.set_split_cuts(cut_as_split);
        host.load(*p, *b);
        CHECK(host.offer_sample(0, &tone));
        auto tones = std::make_unique<Tones>();
        tones->offer(host, *p);
        host.send({Command::Type::Arp, 2, 0, 0, 1}); // the arp on a synth, its keys held
        host.send({Command::Type::Arp, 2, 0, 2, 3});
        for (uint16_t key : {60, 64, 67}) host.send({Command::Type::ArpKey, 2, 0, key, 100});
        host.send({Command::Type::Play});
        std::vector<int32_t> out(256);
        std::vector<float> all, stems(size_t(track_count) * EngineHost::block);
        auto timed = [&](Command c, uint64_t frame) { c.timed = true; c.at = uint32_t(frame); host.send(c); };
        for (unsigned n = 0; n < 700; ++n) {
            uint64_t at = uint64_t(n) * 128 + 128; // the next block's
            if (n % 9 == 3) { // by hand, at their frames inside the next block, and values behind them
                timed({Command::Type::NoteOn, 1, 0, uint16_t(70 + n % 5), 90}, at + 17 + n % 60);
                host.send({Command::Type::Param, 1, 0, uint16_t(n % 7), int16_t(n % 100)});
                timed({Command::Type::PadNote, 5, 64 + 3, 2, 110}, at + 40 + n % 50);
                timed({Command::Type::Pressure, 0, 0, 36, int16_t(n % 127)}, at + 70);
                timed({Command::Type::NoteOff, 1, 0, uint16_t(70 + (n + 4) % 5)}, at + 100);
                host.send({Command::Type::Wheel, 6, 0, 0, int16_t(n * 7 % 1000)});
                Command fx {Command::Type::PadFxParam, 5, 0, 0, int16_t(n % 90)};
                host.send(fx);
            }
            host.render(out.data(), 128, 1, nullptr, stems.data());
            for (unsigned f = 0; f < 256; ++f) all.push_back(out[f] / 2147483648.0f);
            if (stems_out) stems_out->insert(stems_out->end(), stems.begin(), stems.begin() + 8 * 128);
        }
        CHECK(host.calls_lost() == 0);
        return all;
    };
    std::vector<float> old_stems, new_stems;
    auto old_way = play(false, false, 0, &old_stems);
    float loudest = peak_of(old_way);
    CHECK(loudest > 0.1f);                                     // it plays
    for (unsigned t = 0; t < 8; ++t) { // every track sounds
        float peak = 0;
        for (size_t n = 0; n < old_stems.size() / 1024; ++n) peak = std::max(peak, peak_of(old_stems, n * 1024 + t * 128, n * 1024 + t * 128 + 128));
        CHECK(peak > 0.01f);
    }
    CHECK(play(false, false, 3, nullptr) == old_way);          // the old way on the workers
    CHECK(play(true, true, 0, nullptr) == old_way);            // each track cut where the old way cut it
    CHECK(play(true, true, 3, nullptr) == old_way);
    CHECK(play(true, false, 0, nullptr) == old_way);           // whole: each engine cut only at its own calls
    CHECK(play(true, false, 3, &new_stems) == old_way);        // on the workers, one dispatch a block
    CHECK(new_stems == old_stems);                             // and every track's stem the same, to the sample
}

// Roadmap P2, the overload guard: blocks near their time make the next ones lighter. Over 85 % the busiest track gives
// up its oldest voice each block (eight held notes gone in eight blocks, the oldest first, each fading in tens of ms,
// not a click); between 70 and 85 % no voice is stolen but a released note's long tail ends in tens of ms; under 70 %
// a tail rings as long as its release says.
// A project load parked (EngineHost::park): the audio thread fades out, plays silence touching nothing while the
// loader builds the next project on its own thread, then fades in on it, with what was sent meanwhile after it.
void parked_load() {
    { // DelayPool::scrub: a free block written since its clear is cleared, and the next take finds it clean
        DelayPool pool;
        bool clean = false;
        float* a = pool.take(clean);
        CHECK(a && clean);
        a[5] = 1;
        pool.give(a);
        CHECK(pool.scrub() == 1 && pool.scrub() == 0);
        float* b = pool.take(clean);
        CHECK(b && clean && b[5] == 0);
        pool.give(b);
    }
    auto a = std::make_unique<Project>();
    auto ab = std::make_unique<PatternBank>();
    synth(a->tracks[0], 0);
    a->tempo = 1200;
    a->tracks[0].fx.slots[0] = {uint8_t(EffectKind::Echo), false, {60, 50, 40, 50, 30, 0}};
    auto b = std::make_unique<Project>();
    auto bb = std::make_unique<PatternBank>();
    synth(b->tracks[0], 0);
    b->tempo = 900;
    b->tracks[0].fx.slots[0] = {uint8_t(EffectKind::Reverb), false, {70, 50, 40, 50, 0, 0}};
    put(bb->at[0][0], 0, 60, 100, 4);
    { // called off before the audio thread came to it: no fade, nothing waits
        EngineHost host;
        host.load(*a, *ab);
        host.set_parking(true);
        uint32_t serial = host.park();
        CHECK(serial != 0);
        host.resume(serial);
        host.send({Command::Type::Tempo, 0, 0, 0, 1111});
        std::vector<int32_t> out(256);
        for (unsigned n = 0; n < 8; ++n) host.render(out.data(), 128, 1);
        CHECK(host.parks() == 0 && !host.parked(serial) && host.tempo_now() == 1111);
    }
    EngineHost host;
    host.load(*a, *ab);
    tones_for(host, *a);
    host.set_parking(true);
    host.send({Command::Type::NoteOn, 0, 0, 60, 100}); // held: the fade is heard on it
    std::vector<float> heard;
    std::atomic<bool> stop {false};
    std::thread audio([&] { // the audio thread, a block at a time
        std::vector<int32_t> out(256);
        while (!stop.load()) {
            host.render(out.data(), 128, 1);
            for (unsigned f = 0; f < 128; ++f) heard.push_back(float(out[2 * f]) / 2147483648.0f);
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    uint32_t serial = host.park();
    CHECK(serial != 0);
    host.send({Command::Type::Tempo, 0, 0, 0, 1337}); // after the Park: lands on the new project, after its build
    bool parked = false;
    for (unsigned n = 0; n < 2000 && !(parked = host.parked(serial)); ++n) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(parked && host.parks() == 1);
    auto pack = std::make_unique<ProjectPack>(*b, *bb);
    host.reload(*pack, false); // here, on the loader's thread
    std::this_thread::sleep_for(std::chrono::milliseconds(30)); // parked a while longer: still silent
    host.send({Command::Type::Play}); // waits behind the Park too: B plays from the first block it fades in on
    host.resume(serial);
    pack.reset(); // the last project's patterns freed by the loader
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    stop = true;
    audio.join();
    CHECK(host.tempo_now() == 1337 && host.playing());
    // Heard: the held note, faded out over Ease::seconds into a stretch of exact silence, then the new project faded in.
    size_t run = 0, from = 0, longest = 0, longest_from = 0;
    for (size_t n = 0; n < heard.size(); ++n) {
        if (heard[n] == 0) { if (!run++) from = n; if (run > longest) { longest = run; longest_from = from; } }
        else run = 0;
    }
    const size_t fade = size_t(Ease::seconds * dsp::sample_rate());
    CHECK(longest >= size_t(0.025 * dsp::sample_rate())); // at least the 30 ms the loader held it
    size_t z = longest_from, e = longest_from + longest;
    CHECK(z > 3 * fade && e + 4000 < heard.size());
    float held = peak_of(heard, z - 3 * fade, z - fade);
    CHECK(held > 0.02f);
    CHECK(peak_of(heard, z - 32, z) < 0.25f * held);        // the fade's foot, nearly silent
    CHECK(peak_of(heard, z - fade, z) <= held * 1.05f);      // and nothing louder on the way down
    float after = peak_of(heard, e + 2 * fade, e + 4000);
    CHECK(after > 0.02f);
    CHECK(peak_of(heard, e, e + 16) < 0.25f * after);         // the new project faded in, not jumped to
}

void overload_guard() {
    auto p = std::make_unique<Project>();
    auto b = std::make_unique<PatternBank>();
    tone_kit(*p, 0); // a kit, Melodee's own engine with voices to give
    for (unsigned pad = 0; pad < 8; ++pad) {
        auto& sound = p->tracks[0].pads[pad];
        sound.sample = 0; sound.play = PadPlay::Gate; sound.release = 90; // held while held, a long tail
    }
    EngineHost host;
    host.set_overload_guard(true);
    host.load(*p, *b);
    auto tone = tone_sample();
    CHECK(host.offer_sample(0, &tone));
    std::vector<int32_t> out(256);
    auto loud = [&](unsigned blocks) {
        float most = 0;
        for (unsigned n = 0; n < blocks; ++n) { host.render(out.data(), 128, 1); for (auto x : out) most = std::max(most, std::fabs(x / 2147483648.0f)); }
        return most;
    };
    for (uint16_t pad = 0; pad < 8; ++pad) host.send({Command::Type::NoteOn, 0, 0, uint16_t(sampler_first_note + pad), 100});
    CHECK(loud(20) > 0.05f);
    host.set_guard_test_share(0.9f);
    loud(4);
    CHECK(host.guard_steals() == 4 && host.guard_blocks() == 4); // one voice a block, from the busiest (the only) track
    CHECK(loud(4) > 0.01f);                                        // four still held
    loud(8);
    CHECK(host.guard_steals() == 8);                              // every held voice gone, then nothing left to take
    CHECK(loud(10) < 1e-4f);                                       // faded out, quickly
    // Tails: a released note rings on (its release 90) unless the guard is easing the blocks: over 85 % as it is let
    // go, then 75 % (still easing, as it came down from over 85 %), nothing held to steal.
    auto tail = [&](float first, float then) {
        host.set_guard_test_share(0.5f);
        host.send({Command::Type::NoteOn, 0, 0, uint16_t(sampler_first_note), 100});
        loud(20);
        host.set_guard_test_share(first);
        host.send({Command::Type::NoteOff, 0, 0, uint16_t(sampler_first_note)});
        loud(1);
        host.set_guard_test_share(then);
        loud(11); // 1536 frames in all, 35 ms at 44.1 kHz
        return loud(1);
    };
    uint32_t stolen = host.guard_steals();
    CHECK(tail(0.9f, 0.75f) < 1e-4f && host.guard_steals() == stolen); // hurried, nothing stolen
    CHECK(tail(0.75f, 0.75f) > 0.01f);                                   // 75 % from below: no guard, the whole release
}

// Roadmap P2, samples at the device's rate: converted once, band-limited, never per voice. A copy lines up with its
// 44.1 kHz frames (an impulse lands where its time says), passes DC and the audible band whole and keeps images out;
// a file at the device's rate keeps its own frames; a voice at 48 kHz reads the copy a frame each frame, unpitched,
// and its taps come back in 44.1 kHz frames.
void sample_rates_p2() {
    // The converter: an impulse at 44.1 kHz frame 1000 lands at 48 kHz frame 1088 (1000 x 48000 / 44100 = 1088.4).
    std::vector<float> l(4410, 0.0f), r(4410, 0.0f), out_l, out_r;
    l[1000] = r[1000] = 1;
    resample(l.data(), r.data(), 4410, 44100, 48000, out_l, out_r);
    CHECK(out_l.size() == 4800 && out_r.size() == 4800);
    size_t peak = size_t(std::max_element(out_l.begin(), out_l.end()) - out_l.begin());
    CHECK(peak == 1088 || peak == 1089);
    // DC through whole; a 10 kHz tone kept at its level; 22 kHz (above 44.1's band) nothing to fold.
    auto tone = [&](double hz, unsigned from, unsigned to) {
        std::vector<float> a(from / 2), b, c;
        for (size_t n = 0; n < a.size(); ++n) a[n] = float(hz ? std::sin(2 * M_PI * hz * double(n) / from) : 1.0);
        resample(a.data(), a.data(), uint32_t(a.size()), from, to, b, c);
        double sum = 0;
        for (size_t n = b.size() / 4; n < b.size() * 3 / 4; ++n) sum += double(b[n]) * b[n];
        return std::sqrt(sum / double(b.size() / 2));
    };
    CHECK(std::fabs(tone(0, 44100, 48000) - 1.0) < 1e-3);
    CHECK(std::fabs(tone(10000, 44100, 48000) - std::sqrt(0.5)) < 0.01);
    CHECK(tone(30000, 96000, 44100) < 1e-3); // above 44.1 kHz's band: kept out, not folded down to 14.1 kHz
    // A sample's copy for 48 kHz, none at 44.1 kHz, none for an instrument.
    SampleData s;
    s.frames = 4410; s.left = l; s.right = r;
    prepare_playback(s, 48000);
    CHECK(s.plays_at(48000) && !s.plays_at(96000) && s.play_frames == 4800 && s.play_left[peak] == out_l[peak]);
    prepare_playback(s, 44100);
    CHECK(!s.play_frames && s.play_left.empty());
    // A 48 kHz WAV read for a 48 kHz device: the copy it plays is the file's own frames; its 44.1 kHz frames the time.
    const auto path = std::filesystem::temp_directory_path() / "melodee_p2_48k.wav";
    {
        std::ofstream file(path, std::ios::binary);
        auto put = [&](uint32_t v, unsigned bytes) { for (unsigned n = 0; n < bytes; ++n) file.put(char((v >> (8 * n)) & 255)); };
        const unsigned frames = 4800;
        file.write("RIFF", 4); put(36 + frames * 4, 4); file.write("WAVEfmt ", 8);
        put(16, 4); put(1, 2); put(2, 2); put(48000, 4); put(48000 * 4, 4); put(4, 2); put(16, 2);
        file.write("data", 4); put(frames * 4, 4);
        for (unsigned n = 0; n < frames; ++n) { int16_t v = int16_t((n % 100) * 300 - 15000); put(uint16_t(v), 2); put(uint16_t(v), 2); }
    }
    SampleData read;
    CHECK(read_wav(path.string(), read, 60, nullptr, 48000).empty());
    CHECK(read.frames == 4410 && read.plays_at(48000) && read.play_frames == 4800);
    for (unsigned n = 0; n < 4800; ++n) CHECK(read.play_left[n] == float(int16_t((n % 100) * 300 - 15000)) / 32768.0f);
    SampleData plain;
    CHECK(read_wav(path.string(), plain).empty() && !plain.play_frames && plain.frames == 4410); // none asked for
    std::filesystem::remove(path);
    // A voice at 48 kHz: the copy, a frame each device frame, unpitched; its taps in 44.1 kHz frames.
    const float was = dsp::device_rate.load();
    dsp::device_rate.store(48000);
    {
        SampleData ramp;
        ramp.frames = 44100; ramp.left.resize(44100); ramp.right.resize(44100);
        for (unsigned n = 0; n < 44100; ++n) ramp.left[n] = ramp.right[n] = float(std::sin(n * 0.01) * 0.5);
        prepare_playback(ramp, 48000);
        SamplerEngine::Table table {};
        table[0] = &ramp;
        SamplerEngine engine;
        engine.set_samples(&table);
        engine.set_tempo(1200);
        PadSound pad; pad.sample = 0; pad.play = PadPlay::OneShot; pad.start = 4410; // a tenth of a second in
        engine.set_pad(0, pad);
        engine.note_on(int(sampler_first_note), 127);
        std::vector<float> a(4800, 0.0f), b(4800, 0.0f);
        for (unsigned at = 0; at < 4800; at += 128) engine.render_stereo(a.data() + at, b.data() + at, std::min(128u, 4800 - at));
        TrackTaps taps;
        engine.taps(taps);
        CHECK(taps.count == 1 && taps.voice[0].from == 4410 && std::abs(int(taps.voice[0].at) - (4410 + 4410)) <= 1); // 0.1 s on
        CHECK(std::fabs(taps.voice[0].step - 44100.0f / 48000) < 1e-4f);
        // What it played is the copy's own frames from 4800 (0.1 s at 48 kHz), only shaped by the pad's level and pan.
        float ratio = 0;
        for (unsigned f = 2400; f < 4800; ++f) if (std::fabs(ramp.play_left[4800 + f]) > 0.2f) { ratio = a[f] / ramp.play_left[4800 + f]; break; }
        CHECK(ratio > 0.1f);
        for (unsigned f = 2400; f < 4800; ++f) CHECK(std::fabs(a[f] - ramp.play_left[4800 + f] * ratio) < 1e-4f);
    }
    dsp::device_rate.store(was);
}

// Roadmap P2: the Looper's four loops and their chains beside the tracks, then the buses' chains and Knob FX each a
// job of a second run: the same, to the sample, with the workers as without, and as the old way.
void loops_and_buses_parallel() {
    auto play = [](unsigned workers, bool whole) {
        auto p = std::make_unique<Project>();
        auto b = std::make_unique<PatternBank>();
        for (unsigned t = 0; t < 4; ++t) {
            synth(p->tracks[t], t);
            for (unsigned bus = 0; bus < bus_count; ++bus) p->tracks[t].mix.sends[bus] = uint8_t(10 + 10 * bus + t);
            put(b->at[t][0], t, uint8_t(50 + 3 * t), 100, 4);
        }
        const EffectKind buses[] {EffectKind::Reverb, EffectKind::Echo, EffectKind::Plate, EffectKind::Chorus};
        for (unsigned bus = 0; bus < bus_count; ++bus) p->buses[bus].fx.slots[0] = {uint8_t(buses[bus]), false, {70, 50, 40, 50, 0, 0}};
        p->looper.on = true;
        EngineHost host;
        host.set_render_threads(workers, 0);
        host.set_one_dispatch(whole);
        host.load(*p, *b);
        auto tones = std::make_unique<Tones>();
        tones->offer(host, *p);
        std::vector<int32_t> out(256), all;
        auto render = [&](unsigned blocks) {
            for (unsigned n = 0; n < blocks; ++n) {
                host.render(out.data(), 128, 1);
                host.looper().service();
                all.insert(all.end(), out.begin(), out.end());
            }
        };
        render(8); // the Looper's memory
        const EffectKind chains[] {EffectKind::Drive, EffectKind::Phaser, EffectKind::Echo, EffectKind::Flanger};
        for (unsigned n = 0; n < loop_tracks; ++n) {
            auto* buffer = new LoopBuffer(looper::frames_at(44100));
            for (unsigned f = 0; f < 4000; ++f) buffer->left[f] = buffer->right[f] = float(std::sin(f * (0.01 + 0.007 * n)) * 0.3);
            LoopLoad item; item.buffer = buffer; item.loop = uint8_t(n); item.frames = 4000; item.ticks = 96; item.tempo = 1200;
            CHECK(host.looper().give_load(item));
            host.send({Command::Type::FxKind, uint8_t(loop_chain(n)), 0, 0, int16_t(chains[n])});
            host.send({Command::Type::Send, uint8_t(track_count + n), 0, 0, 40}); // L1-L4 into a bus too
        }
        host.send({Command::Type::KnobFx, 0, 0, 0, int16_t(EffectKind::Filter)}); // Knob FX on track 2, heard
        Command knob {Command::Type::KnobFx, 0, 0, 3}; knob.position = 2; host.send(knob);
        host.send({Command::Type::KnobFx, 0, 0, 2, 1});
        host.send({Command::Type::Play});
        render(400);
        for (unsigned n = 0; n < loop_tracks; ++n) CHECK(host.looper().view(n).state == Looper::State::Playing && host.loop_meter(n) > 0.01f);
        return all;
    };
    auto alone = play(0, true);
    int64_t loudest = 0;
    for (int32_t v : alone) loudest = std::max<int64_t>(loudest, std::llabs(int64_t(v)));
    CHECK(loudest > 200000000); // it plays
    CHECK(play(3, true) == alone);
    CHECK(play(3, false) == alone);
    CHECK(play(0, false) == alone);
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

// Round 3 "instrument C": Stage's ZONES. The zone a key reaches, velocity layers counted, and a zone's trim (tune in
// cents, level in tenths of a dB) heard on its pad's instrument alone, kept in ZTRM and in a pad's undo state.
void instrument_zones_r3() {
    { // the zone in hand follows the key played; release samples and later round-robin turns are not it
        std::vector<ZoneShape> z(5);
        z[0].low_key = 0; z[0].high_key = 59;
        z[1].low_key = 60; z[1].high_key = 71; z[1].high_velocity = 64; z[1].round = 2;
        z[2].low_key = 60; z[2].high_key = 71; z[2].high_velocity = 64; z[2].round = 1;
        z[3].low_key = 60; z[3].high_key = 71; z[3].low_velocity = 65;
        z[4].low_key = 60; z[4].high_key = 71; z[4].flags = ZoneShape::Release;
        CHECK(zone_at(z, 40, 100) == 0 && zone_at(z, 64, 30) == 2 && zone_at(z, 64, 100) == 3 && zone_at(z, 100, 100) == -1);
        CHECK(zone_layers(z) == 2); // 1-64 and 65-127 on one key; turns of one range and the release count once
        CHECK(zone_layers({}) == 1);
    }
    { // the table: one trim a pad's zone, none when both are 0, full at max_zone_trims; stale ones dropped
        ZoneTrims trims {};
        CHECK(set_trim(trims, 8, 3, 5, 12, -15));
        const ZoneTrim* t = find_trim(trims, 8, 3, 5);
        CHECK(t && t->tune == 12 && t->level == -15 && !find_trim(trims, 8, 3, 6) && !find_trim(trims, 9, 3, 5));
        CHECK(set_trim(trims, 8, 3, 5, 500, -900)); // clamped
        CHECK(find_trim(trims, 8, 3, 5)->tune == zone_trim_cents && find_trim(trims, 8, 3, 5)->level == zone_trim_low);
        CHECK(set_trim(trims, 8, 3, 5, 0, 0) && !find_trim(trims, 8, 3, 5));
        for (unsigned n = 0; n < max_zone_trims; ++n) CHECK(set_trim(trims, 1, n, 5, 1, 0));
        CHECK(!set_trim(trims, 2, 0, 5, 1, 0) && set_trim(trims, 2, 0, 5, 0, 0)); // full: a new one refused, none is fine
        clear_trims(trims, 1);
        CHECK(std::none_of(trims.begin(), trims.end(), [](const ZoneTrim& z) { return z.used(); }));
        auto track = std::make_unique<Track>();
        track->pads[4].sample = 7;
        set_trim(track->zone_trims, 4, 0, 7, 10, 0);
        set_trim(track->zone_trims, 4, 1, 6, 10, 0); // made on another instrument
        track->drop_stale_trims();
        CHECK(find_trim(track->zone_trims, 4, 0, 7) && !find_trim(track->zone_trims, 4, 1, 6));
        // a pad's undo state carries its trims; set onto another pad they go with it
        PadState state = pad_state(*track, 4);
        CHECK(state.trims.size() == 1 && same_trims(state, pad_state(*track, 4)) && !same_trims(state, pad_state(*track, 5)));
        set_pad_state(*track, 9, state);
        CHECK(find_trim(track->zone_trims, 9, 0, 7) && find_trim(track->zone_trims, 4, 0, 7));
        set_pad_state(*track, 4, PadState {});
        CHECK(!find_trim(track->zone_trims, 4, 0, 7));
    }
    { // the engine hears a zone's trim on its pad's instrument, not another's
        ZoneRig r;
        r.part(zone_tone(220, 0.5f, 44100)); r.part(zone_tone(440, 0.5f, 44100));
        auto& low = r.zone(0); low.low_key = 0; low.high_key = 65; low.root = 60;
        auto& high = r.zone(1); high.low_key = 66; high.high_key = 127; high.root = 72;
        r.build();
        r.key(0);
        auto plain = r.run(8000);
        r.engine->silence();
        r.engine->set_trim(0, 0, 3, 100, -60); // zone 0 of slot 3 on pad 0: a semitone up, 6 dB down
        r.key(0);
        auto trimmed = r.run(8000);
        CHECK(std::fabs(zone_hz(trimmed, 800, 8000) - 220 * std::pow(2.0, 1 / 12.0)) < 8);
        CHECK(std::fabs(zone_rms(trimmed, 2000, 8000) / zone_rms(plain, 2000, 8000) - 0.501) < 0.03);
        r.engine->silence();
        r.key(12); // zone 1: untouched
        CHECK(std::fabs(zone_hz(r.run(8000), 800, 8000) - 440) < 10);
        r.engine->silence();
        r.engine->set_trim(0, UINT32_MAX, 0, 0, 0); // the pad's trims let go
        r.key(0);
        CHECK(std::fabs(zone_hz(r.run(8000), 800, 8000) - 220) < 8);
        r.engine->silence();
        r.engine->set_trim(0, 0, 4, 100, 0); // made on slot 4: the pad holds slot 3, so not heard
        r.key(0);
        CHECK(std::fabs(zone_hz(r.run(8000), 800, 8000) - 220) < 8);
        // the whole table at once, as a project or track load gives it
        r.engine->silence();
        ZoneTrims all {};
        set_trim(all, 0, 0, 3, -100, 0);
        r.engine->set_trims(all);
        r.key(0);
        CHECK(std::fabs(zone_hz(r.run(8000), 800, 8000) - 220 * std::pow(2.0, -1 / 12.0)) < 8);
        // as a command: index the pad, position the zone, micros the slot, pattern cents + 128, value tenths of a dB
        ZoneTrim z; z.pad = 9; z.zone = 300; z.slot = 12; z.tune = -37; z.level = -105;
        Command c = zone_trim_command(2, z);
        CHECK(c.type == Command::Type::ZoneTrim && c.track == 2 && c.index == 9 && c.position == 300 && c.micros == 12);
        CHECK(int(c.pattern) - 128 == -37 && c.value == -105);
    }
    { // a pot turns a card across 0 back to 0, however wide its range, and never goes back on itself
        for (auto range : {std::pair<int, int> {-zone_trim_cents, zone_trim_cents}, {zone_trim_low, zone_trim_high}, {-480, 120},
                           {-24, 24}, {-50, 50}, {-1, 1}, {-9600, 6000}}) {
            Binding b {Binding::Kind::ZoneTune, 0, range.first, range.second, 0};
            bool zero = false;
            for (int p = 0; p <= 127; ++p) {
                zero |= b.at(p) == 0;
                if (p) CHECK(b.at(p) >= b.at(p - 1));
            }
            CHECK(zero && b.at(0) == range.first && b.at(127) == range.second);
            CHECK(takeover_meets(int(b.fraction() * 127 + 0.5f), b)); // where 0 sits is where the pot meets it
        }
    }
    { // ZTRM: a track's trims kept; one made on a sample the pad no longer holds is not
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        load_sound(project->tracks[3], EngineKind::Sampler, 0);
        project->tracks[3].pads[8].sample = 5;
        set_trim(project->tracks[3].zone_trims, 8, 300, 5, -42, 35);
        set_trim(project->tracks[3].zone_trims, 8, 2, 6, 10, 0); // stale
        auto file = save_project(*project, *bank);
        auto back = std::make_unique<Project>();
        auto back_bank = std::make_unique<PatternBank>();
        CHECK(load_project(file, *back, *back_bank).empty());
        const ZoneTrim* t = find_trim(back->tracks[3].zone_trims, 8, 300, 5);
        CHECK(t && t->tune == -42 && t->level == 35 && !find_trim(back->tracks[3].zone_trims, 8, 2, 6));
        std::string text(file.begin(), file.end());
        file[text.find("ZTRM")] = 'X';
        CHECK(load_project(file, *back, *back_bank).empty() && !find_trim(back->tracks[3].zone_trims, 8, 300, 5));
    }
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

// The synths that left Melodee: a project saved while PROPHET, CZ and SID were built in opens their tracks as PLUGIN
// tracks on Melodee Poly, PD and Chip (the same voices, now Open MPC plug-ins), named from the card's top, their
// values and routes the plug-in's state in a free entry; FM6 and 909 tracks open empty with their patterns. Open MPC
// plug-ins run at 44.1 kHz (MPC OS's contract): at another device rate they are heard through a band-limited
// conversion, so they play in tune, and an effect passes what it is given.
// TONE3000's NAM blocks always lite (plugin/tone3000.h): each "slimSize" double set to 0 in its data and its
// presets on the card, and nothing else.
void tone3000_always_lite() {
    namespace fs = std::filesystem;
    auto size = [](double value) {
        std::vector<uint8_t> out = {'s', 'l', 'i', 'm', 'S', 'i', 'z', 'e', 0, 1, 9, 4};
        uint8_t bytes[8];
        std::memcpy(bytes, &value, 8); // JUCE writes it little-endian, as both hosts are
        out.insert(out.end(), bytes, bytes + 8);
        return out;
    };
    auto add = [](std::vector<uint8_t>& to, const std::vector<uint8_t>& more) { to.insert(to.end(), more.begin(), more.end()); };
    auto value_at = [](const std::vector<uint8_t>& data, size_t at) { double v; std::memcpy(&v, data.data() + at, 8); return v; };
    std::vector<uint8_t> preset = {'T', '3', 'K', 'H'};
    add(preset, size(1.0)); // full, at 4 + 12
    preset.push_back('x');
    add(preset, size(0.0)); // lite already, at 25 + 12
    add(preset, {'s', 'l', 'i', 'm', 'S', 'i', 'z', 'e', 0, 1, 5, 1, 1, 0, 0, 0}); // an int, not a size TONE3000 writes
    add(preset, size(0.5)); // at 61 + 12
    const std::vector<uint8_t> original = preset;
    std::vector<uint8_t> data = preset;
    CHECK(tone3000_lite(data.data(), data.size()) == 2);
    CHECK(value_at(data, 16) == 0.0 && value_at(data, 37) == 0.0 && value_at(data, 73) == 0.0);
    for (size_t i = 0; i < data.size(); ++i)
        if ((i < 16 || i >= 24) && (i < 73 || i >= 81)) CHECK(data[i] == original[i]);
    CHECK(tone3000_lite(data.data(), data.size()) == 0);
    std::vector<uint8_t> cut = size(1.0);
    cut.pop_back(); // the double cut short at the end: left alone
    CHECK(tone3000_lite(cut.data(), cut.size()) == 0);

    auto home = fs::temp_directory_path() / ("melodee-tone3000-" + std::to_string(getpid()));
    auto factory = home / ".config" / "TONE3000" / "Presets" / "Factory";
    fs::create_directories(factory);
    auto write = [](const fs::path& path, const std::vector<uint8_t>& bytes) {
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    };
    auto read = [](const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
    };
    write(factory / "Crunch.t3kpreset", original);
    write(home / ".config" / "TONE3000" / "Presets" / "Lite.t3kpreset", data);
    write(factory / "Crunch.t3kpreset.tmp", original); // not a preset
    CHECK(tone3000_lite_presets(home) == 1);
    CHECK(read(factory / "Crunch.t3kpreset") == data && read(factory / "Crunch.t3kpreset.tmp") == original);
    CHECK(tone3000_lite_presets(home) == 0);
    write(factory / "Crunch.t3kpreset", original); // copied over again: read again
    fs::last_write_time(factory / "Crunch.t3kpreset", fs::last_write_time(factory / "Crunch.t3kpreset") + std::chrono::seconds(2));
    CHECK(tone3000_lite_presets(home) == 1 && read(factory / "Crunch.t3kpreset") == data);
    CHECK(tone3000_lite_presets(home / "none") == 0 && tone3000_lite_presets({}) == 0);
    fs::remove_all(home);
}

void synths_as_plugins() {
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        for (unsigned t = 0; t < 5; ++t) load_sound(project->tracks[t], EngineKind::Midi, 0); // stand-ins, renumbered below
        for (unsigned p = 0; p < 53; ++p) project->tracks[0].params[p] = int16_t(p % 7);
        project->tracks[0].params[16] = 33;                                       // PROPHET's cutoff
        project->tracks[0].mods[2] = {uint8_t(ModSource::Lfo), uint8_t(ModDest::Cutoff), -45};
        project->tracks[0].set_name("FUNK BASS II");
        put(bank->at[1][0], 2, 60, 100);                                          // the FM6 track's notes
        synth(project->tracks[6], 0);                                             // a plug-in already in entry 0
        std::snprintf(project->plugins[0].path, plugin_path_length, "/media/card/Plugins/Other.vst3");
        auto bytes = save_project(*project, *bank);
        const uint8_t legacy[5] {legacy_engine::prophet, legacy_engine::fm6, legacy_engine::cz, legacy_engine::sid, legacy_engine::kit909};
        unsigned patched = 0;
        for (size_t at = 8; at + 10 <= bytes.size();) { // sections: a tag, a length, the contents
            uint32_t length = uint32_t(bytes[at + 4]) | uint32_t(bytes[at + 5]) << 8 | uint32_t(bytes[at + 6]) << 16 | uint32_t(bytes[at + 7]) << 24;
            if (!std::memcmp(&bytes[at], "TRAK", 4) && bytes[at + 8] < 5) { bytes[at + 9] = legacy[bytes[at + 8]]; ++patched; }
            if (!std::memcmp(&bytes[at], "MIDT", 4) && bytes[at + 8] < 5) std::memcpy(&bytes[at], "XXXX", 4); // they were never MIDI
            at += 8 + length;
        }
        CHECK(patched == 5);
        auto copy = std::make_unique<Project>();
        auto copy_bank = std::make_unique<PatternBank>();
        std::array<std::vector<uint8_t>, max_plugins> states;
        unsigned version = 0;
        CHECK(load_project(bytes, *copy, *copy_bank, &version, &states).empty() && version == params_version);
        auto text = [&](unsigned entry) { return std::string(states[entry].begin(), states[entry].end()); };
        const auto& poly = copy->tracks[0];
        CHECK(poly.engine == EngineKind::Plugin && poly.plugin == 1 && std::string(poly.name) == "FUNK BASS II");
        CHECK(std::string(copy->plugins[1].path) == "Plugins/Ellic Studio - VST - Melodee Poly" && std::string(copy->plugins[1].name) == "Melodee Poly");
        CHECK(text(1).compare(0, 5, std::string("MVS2\x01", 5)) == 0 && states[1].back() == 0);
        CHECK(text(1).find("melodee-poly 1\nmelodee=0,1,2,3,4,5,6,0,") != std::string::npos);
        auto values = text(1).substr(text(1).find("melodee=") + 8);
        values = values.substr(0, values.find('\n'));
        CHECK(std::count(values.begin(), values.end(), ',') == 52 && values.find(",33,") != std::string::npos); // 53 values
        CHECK(text(1).find("\nmelodee_mods=0:0:0,0:0:0,1:1:-45,0:0:0,") != std::string::npos);
        CHECK(copy->tracks[2].engine == EngineKind::Plugin && copy->tracks[2].plugin == 2
              && std::string(copy->plugins[2].path) == "Plugins/Ellic Studio - VST - Melodee PD" && text(2).find("melodee-pd 1\n") == 5);
        CHECK(copy->tracks[3].engine == EngineKind::Plugin && copy->tracks[3].plugin == 3 && text(3).find("melodee-chip 1\n") == 5);
        CHECK(copy->tracks[1].engine == EngineKind::None && copy->tracks[4].engine == EngineKind::None); // FM6, the 909
        CHECK(copy_bank->at[1][0].events.size() == 1 && copy_bank->at[1][0].events[0].key == 60);          // their notes kept
        CHECK(std::string(copy->plugins[0].path) == "/media/card/Plugins/Other.vst3" && states[0].empty());
        CHECK(plugin_used_by(*copy, 1) && plugin_used_by(*copy, 2) && plugin_used_by(*copy, 3));
        // Saved again, they are plug-in tracks like any other.
        auto again = save_project(*copy, *copy_bank, &states);
        auto back = std::make_unique<Project>();
        std::array<std::vector<uint8_t>, max_plugins> back_states;
        CHECK(load_project(again, *back, *copy_bank, nullptr, &back_states).empty());
        CHECK(back->tracks[0].engine == EngineKind::Plugin && back->tracks[0].plugin == 1 && back_states[1] == states[1]);
    }
    // The bridge: a plug-in at 44.1 kHz playing a 1 kHz tone, heard at 48 kHz, is a 1 kHz tone; at equal rates nothing
    // converts.
    {
        PluginRateBridge same;
        same.reset(44100, 44100, true);
        CHECK(!same.converting());
        PluginRateBridge bridge;
        bridge.reset(44100, 48000, true);
        CHECK(bridge.converting());
        double phase = 0;
        unsigned plugin_frames = 0;
        auto tone = [&](float* l, float* r, unsigned n) {
            for (unsigned f = 0; f < n; ++f) { l[f] = r[f] = float(0.5 * std::sin(phase)); phase += 2 * M_PI * 1000 / 44100; }
            plugin_frames += n;
        };
        std::vector<float> heard;
        std::array<float, 128> l {}, r {};
        const unsigned sizes[] {128, 37, 128, 1, 90, 128};
        for (unsigned n = 0; heard.size() < 48000; ++n) {
            unsigned size = sizes[n % 6];
            std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
            bridge.render(l.data(), r.data(), size, tone);
            heard.insert(heard.end(), l.begin(), l.begin() + size);
        }
        unsigned crossings = 0;
        for (size_t n = 9600; n + 1 < 48000; ++n) crossings += (heard[n] < 0) != (heard[n + 1] < 0);
        CHECK(crossings >= 2 * 798 && crossings <= 2 * 802); // 0.8 s of 1 kHz, two crossings a cycle
        CHECK(std::abs(int(plugin_frames) - 44100) < 200);    // as many of its frames as a second of its own
        CHECK(peak_of(heard, 9600) > 0.45f && peak_of(heard, 9600) < 0.55f);
    }
    // An effect at 44.1 kHz in a 96 kHz chain: what it passes comes out as it went in, later by a constant few ms.
    {
        PluginRateBridge bridge;
        bridge.reset(44100, 96000, false);
        std::vector<float> in(96000), out;
        for (size_t n = 0; n < in.size(); ++n) in[n] = float(0.4 * std::sin(2 * M_PI * 440 * double(n) / 96000));
        std::array<float, 128> l {}, r {};
        for (size_t at = 0; at < in.size(); at += 128) {
            std::copy(in.begin() + long(at), in.begin() + long(at + 128), l.begin());
            r = l;
            bridge.render(l.data(), r.data(), 128, [](float*, float*, unsigned) {}); // passes it as it is
            out.insert(out.end(), l.begin(), l.end());
        }
        double in_sum = 0, out_sum = 0;
        for (size_t n = 9600; n < 96000; ++n) { in_sum += double(in[n]) * in[n]; out_sum += double(out[n]) * out[n]; }
        CHECK(std::fabs(std::sqrt(out_sum / in_sum) - 1) < 0.01);
        size_t lag = 0; // where the output's first rise meets the input's
        while (lag < 1000 && std::fabs(out[lag]) < 0.01f) ++lag;
        CHECK(lag > 50 && lag < 600);
        double worst = 0;
        for (size_t n = 9600; n < 90000; ++n) worst = std::max(worst, double(std::fabs(out[n] - in[n - lag + 1])));
        CHECK(worst < 0.05);
    }
}

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
        copy->tracks[2].engine = EngineKind::Midi; // the entry stays in the table but nothing plays it
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

#include "melodee/shell/pad_lights.h"
#include "melodee/shell/step_lane.h"

// The pads light as what a press does there (shell/pad_context.h, shell/pad_lights.cpp), on five rungs.
void pad_lights() {
    using namespace pad_light;
    // The ladder: Faint keeps every channel of each track's colour lit; 0.07 left track 1 pure red.
    auto channels = [](Rgb c) { return unsigned(c.r > 0) + unsigned(c.g > 0) + unsigned(c.b > 0); };
    for (unsigned t = 0; t < 16; ++t) CHECK(channels(track(t, faint).rgb) == channels(track(t, full).rgb));
    CHECK(channels(track(0, 0.07f).rgb) == 1 && channels(track(0, full).rgb) == 3);
    CHECK(rung(0) == 0 && rung(faint) == 1 && rung(dim) == 2 && rung(mid) == 3 && rung(full) == 4 && rung(0.3f) == 1);

    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    EngineHost host;
    TrackTaps taps;
    UiState ui;
    auto& kit = project->tracks[1];
    kit.engine = EngineKind::Sampler;
    for (unsigned n = 0; n < 12; ++n) kit.pads[n].sample = uint16_t(n);
    kit.last_pad = 1;
    kit.pad_mutes[0] |= uint64_t(1) << 8; // pad 9
    project->tracks[0].engine = EngineKind::Plugin;
    project->tracks[5].engine = EngineKind::Plugin;
    ui.track = 1; ui.page = PageId::Stage;
    std::array<Look, 16> looks;
    PadFacts plays; plays.kit_pads = true;
    auto light = [&](PadFacts facts, uint32_t now = 1000, const std::array<uint32_t, 16>* let_go = nullptr) {
        PadScene scene {ui, *project, *bank, host, taps, facts, now, let_go, 0};
        pad_looks(scene, looks);
    };
    auto is = [&](unsigned pad, Hue hue, unsigned r) { return looks[pad].hue == hue && rung(looks[pad].level) == r; };

    // A kit: the last pad Full, a sample Dim, empty dark, muted Faint red on every play page; held white.
    light(plays);
    CHECK(is(1, Hue::Track, 4) && is(0, Hue::Track, 2) && is(12, Hue::None, 0) && is(8, Hue::Red, 1));
    ui.held_pads = 1u << 3; light(plays); CHECK(is(3, Hue::White, 4)); ui.held_pads = 0;
    kit.last_pad = 13; light(plays); // an empty pad tapped: selected, in the track's colour
    CHECK(is(13, Hue::Track, 4) && is(1, Hue::Track, 2) && is(12, Hue::None, 0));
    kit.last_pad = 1;
    std::array<uint32_t, 16> let_go {};
    let_go[0] = 1000 + 1; // pad 1 let go at 1000 ms: it flares, and a second later it is Dim again
    light(plays, 1000, &let_go); CHECK(is(0, Hue::Track, 4));
    light(plays, 2000, &let_go); CHECK(is(0, Hue::Track, 2));
    taps.track = 1; taps.count = 1; taps.voice[0].pad = 2; taps.voice[0].level = 0.5f; // a voice at half its envelope
    light(plays); CHECK(is(2, Hue::Track, 3));
    taps = TrackTaps {};
    // A synth: the key's roots in its colour (Mid), its other notes white Dim; chromatic, notes out of the key white Faint.
    ui.track = 0; light({});
    CHECK(is(0, Hue::Track, 3) && is(1, Hue::White, 2) && is(7, Hue::Track, 3));
    project->tracks[0].layout = PadLayout::Chromatic; light({});
    CHECK(is(0, Hue::Track, 3) && is(1, Hue::White, 1) && is(2, Hue::White, 2));
    project->tracks[0].transpose = 2; light({}); // the track's key goes with it: E major, G# in it, G out
    CHECK(is(0, Hue::Track, 3) && is(4, Hue::White, 2) && is(3, Hue::White, 1));
    project->tracks[0].transpose = 0;
    project->tracks[0].layout = PadLayout::Chords; ui.held_pads = 1u; light({}); // D's seventh held: F#, A, C# white Full
    CHECK(is(0, Hue::White, 4) && is(2, Hue::White, 4) && is(4, Hue::White, 4) && is(6, Hue::White, 4) && is(1, Hue::White, 2)
          && is(7, Hue::Track, 3));
    ui.held_pads = 0;
    project->tracks[0].layout = PadLayout::Scale;
    auto input = ui.settings.input; // Record with the synth as its source: its keys, as on its own pages
    ui.settings.input.source = InputSource::Track; ui.settings.input.track = 0;
    PadFacts keys_in_take; keys_in_take.record_keys = true; light(keys_in_take);
    CHECK(is(0, Hue::Track, 3) && is(1, Hue::White, 2));
    ui.settings.input = input;
    // 16 Levels: soft to loud; Tune and Scale the key's roots in the track's colour, its other notes white, out of it Faint.
    ui.track = 1; kit.levels = true; kit.levels_kind = uint8_t(LevelsKind::Velocity); light({});
    CHECK(is(0, Hue::Track, 1) && is(15, Hue::Track, 4) && looks[7].level < looks[8].level);
    kit.levels_kind = uint8_t(LevelsKind::Tune); light({}); // -3..12: the sample as recorded (pad 4) the root
    CHECK(is(3, Hue::Track, 3) && is(15, Hue::Track, 3) && is(0, Hue::White, 2) && is(1, Hue::White, 1));
    kit.levels = false;
    auto& synth = project->tracks[0]; // Scale: up the key from its root on pad 1, whatever the last note
    ui.track = 0; synth.levels = true; synth.levels_kind = uint8_t(LevelsKind::Scale); light({});
    CHECK(is(0, Hue::Track, 3) && is(7, Hue::Track, 3) && is(1, Hue::White, 2));
    synth.last_note = 45; synth.transpose = 2; light({}); // another last note, the key a tone up: the roots stay on pads 1, 8, 15
    CHECK(is(0, Hue::Track, 3) && is(7, Hue::Track, 3) && is(14, Hue::Track, 3) && is(6, Hue::White, 2));
    synth.transpose = 0; synth.levels_kind = uint8_t(LevelsKind::Tune); light({}); // Tune from the last note: F#2 up, D on pad 9, G# out of D major
    CHECK(is(8, Hue::Track, 3) && is(0, Hue::White, 2) && is(2, Hue::White, 1));
    synth.transpose = 2; light({}); // in E: E on pad 11, D out
    CHECK(is(10, Hue::Track, 3) && is(8, Hue::White, 1));
    synth.transpose = 0; synth.last_note = 50; synth.levels = false; synth.levels_kind = uint8_t(LevelsKind::Scale);
    ui.track = 1;

    // Tracks: picked Full, heard Dim, muted Faint red; a solo leaves the rest Faint.
    ui.page = PageId::Tracks; project->tracks[3].engine = EngineKind::Plugin; project->tracks[3].mix.mute = true; light({});
    CHECK(is(1, Hue::Track, 4) && is(0, Hue::Track, 2) && is(3, Hue::Red, 1) && is(9, Hue::None, 0));
    project->tracks[0].mix.solo = true; light({});
    CHECK(is(0, Hue::White, 2) && is(5, Hue::Track, 1));
    ui.page = PageId::Stage; ui.pad_bank = true; light(plays); // Pad Bank held: the tracks, on any page
    CHECK(is(1, Hue::Track, 4) && is(3, Hue::Red, 1));
    // + Erase: what Erase + pad would clear Faint red, an empty track dark (its patterns' notes count: they play MIDI out).
    ui.erase = true; light(plays);
    CHECK(is(1, Hue::Red, 1) && is(3, Hue::Red, 1) && is(9, Hue::None, 0));
    CHECK(track_clearable(project->tracks[1], bank->at[1]) && !track_clearable(project->tracks[9], bank->at[9]));
    put(bank->at[9][4], 0, 60, 100); light(plays);
    CHECK(is(9, Hue::Red, 1) && track_clearable(project->tracks[9], bank->at[9]));
    bank->at[9][4] = Pattern {};
    ui.erase = false; ui.pad_bank = false;
    // Mute mode: a kit's pads on a kit page, the tracks on Tracks; muted Dim red, a solo white.
    ui.mute = true; light(plays);
    CHECK(is(8, Hue::Red, 2) && is(0, Hue::Track, 3) && is(12, Hue::None, 0));
    ui.page = PageId::Tracks; light({});
    CHECK(is(3, Hue::Red, 2) && is(0, Hue::White, 3) && is(5, Hue::Track, 1));
    ui.mute = false; project->tracks[0].mix.solo = false;
    // Song: the lane picked, a clip under the cursor Mid, not in the song Faint.
    ui.page = PageId::Song; project->song.clips[0].track = 0; project->song.clips[0].bars = 2; project->song.count = 1; light({});
    CHECK(is(1, Hue::Track, 4) && is(0, Hue::Track, 3) && is(5, Hue::Track, 1));

    // Seq held: the pattern in play Full, with notes Dim, empty dark; + Erase red; + Shift a column; a copy.
    auto& p = bank->at[1][0];
    put(p, 0, uint8_t(sampler_first_note + 1), 127);
    put(p, 2, uint8_t(sampler_first_note), 100);
    put(bank->at[1][2], 0, uint8_t(sampler_first_note), 100);
    ui.page = PageId::Stage; ui.seq_held = true; light(plays);
    CHECK(is(0, Hue::Track, 4) && is(2, Hue::Track, 2) && is(5, Hue::None, 0));
    ui.erase = true; light(plays);
    CHECK(is(0, Hue::Red, 3) && is(2, Hue::Red, 2) && is(5, Hue::None, 0));
    ui.erase = false; ui.shift = true; light(plays);
    CHECK(is(0, Hue::White, 4) && is(2, Hue::White, 1));
    ui.shift = false; ui.copying = UiState::Copying::Patterns; ui.copy_from = 0; ui.copy_track = 1; light(plays);
    CHECK(is(0, Hue::White, 4) && is(2, Hue::Red, 1) && is(3, Hue::Track, 1));
    ui.copying = UiState::Copying::Off; ui.copy_from = -1; ui.seq_held = false;
    // Steps on a kit: the last pad's lane only, by velocity; past the pattern's end dark.
    ui.page = PageId::Steps; light(plays);
    CHECK(is(0, Hue::Track, 4) && is(2, Hue::Track, 1) && is(1, Hue::Track, 1));
    p.length = 12 * sixteenth_ticks; light(plays);
    CHECK(is(12, Hue::None, 0) && is(11, Hue::Track, 1));
    p.length = 16 * sixteenth_ticks;
    CHECK(steps_lane(kit) == 37 && steps_lane(project->tracks[0]) == -1);
    CHECK(step_velocity(p, 0, 37) == 127 && step_velocity(p, 2, 37) == 0 && step_velocity(p, 2, -1) == 100);
    CHECK(add_lane_note(p, 2, 37, 90) && step_velocity(p, 2, 37) == 90 && step_velocity(p, 2, 36) == 100); // beside the other pad's
    CHECK(clear_lane(p, 2, 37) && step_velocity(p, 2, 37) == 0 && step_velocity(p, 2, 36) == 100);        // only its lane goes
    CHECK(!clear_lane(p, 5, 37));
    CHECK(move_lane(p, 0, 37, 38) && step_velocity(p, 0, 38) == 127 && step_velocity(p, 0, 37) == 0 && move_lane(p, 0, 38, 37));
    // Step Edit: the chosen event white, a pad with nothing on the step Faint.
    ui.page = PageId::StepEdit; ui.step_cursor = 0; light({});
    CHECK(is(1, Hue::White, 4) && is(0, Hue::Track, 1) && is(12, Hue::None, 0));
    // Roll: the note in focus white, notes elsewhere Dim, none Faint.
    ui.page = PageId::Roll; ui.roll_key = 37; light({});
    CHECK(is(1, Hue::White, 4) && is(0, Hue::Track, 2) && is(5, Hue::Track, 1));
    // Time Correct: whose notes move.
    ui.page = PageId::TimeCorrect; ui.tc_all = true; light({});
    CHECK(is(0, Hue::Track, 4) && is(1, Hue::Track, 4) && is(5, Hue::None, 0));
    ui.tc_all = false; ui.tc_toggle(36); light({});
    CHECK(is(0, Hue::Track, 4) && is(1, Hue::Track, 2));

    // Chop: the slices, the one in hand Full; Manual armed, pad 1 flashes red.
    ui.page = PageId::Chop; ui.slices.count = 5; ui.chop_slice = 2; light({});
    CHECK(is(2, Hue::Track, 4) && is(0, Hue::Track, 2) && is(6, Hue::None, 0));
    ui.chop_mode = ChopMode::Manual; ui.chop_armed = true; light({}, 1000);
    CHECK(is(0, Hue::Red, 4) && is(1, Hue::None, 0));
    ui.chop_armed = false; ui.chop_mode = ChopMode::Threshold; ui.slices.count = 0;
    // Record: the take's pad red, a sample Dim, empty Faint (never 0.07, which drove tracks 1 and 2 red).
    ui.page = PageId::Record; ui.rec_track = 1; ui.rec_pad = 12; light({}, 1000);
    CHECK(is(12, Hue::Red, 4) && is(0, Hue::Track, 2) && is(13, Hue::Track, 1));
    ui.rec_pad = -1;
    ui.page = PageId::Stage; ui.resample_pick = true; light(plays);
    CHECK(is(0, Hue::Track, 2) && looks[12].hue == Hue::Track && looks[12].level <= dim);
    ui.resample_pick = false;
    // Knob FX and Flex Beat.
    ui.page = PageId::KnobFx; light({});
    CHECK(is(0, Hue::Track, 2) && is(12, Hue::None, 0));
    ui.page = PageId::FlexBeat; light({});
    CHECK(is(0, Hue::White, 4) && is(1, Hue::White, 2));
    // Pad FX with Erase held (round 2, pad-fx C): the pads whose punch the pattern keeps red, the rest dark.
    ui.page = PageId::Punch; project->punch_bank = 0; ui.erase = true;
    bank->at[1][kit.pattern].punches = {{0, 96, uint8_t(PunchKind::Low), 255, 100, 2, 1}};
    light({});
    CHECK(is(8, Hue::Red, 2) && is(4, Hue::None, 0));
    ui.erase = false; light({});
    CHECK(looks[8].hue != Hue::Red);
    bank->at[1][kit.pattern].punches.clear();
    ui.page = PageId::Stage;

    // Shift: the panel's jobs white, dark where they cannot act.
    ui.shift = true; kit.pattern = 1; light(plays); // an empty pattern on a kit
    CHECK(is(1, Hue::None, 0) && is(2, Hue::None, 0) && is(5, Hue::None, 0) && is(13, Hue::None, 0));
    CHECK(is(3, Hue::White, 2) && is(9, Hue::White, 4) && is(7, Hue::White, 2));
    kit.pattern = 0; light(plays);
    CHECK(is(1, Hue::White, 2) && is(5, Hue::White, 2) && is(10, Hue::White, 2));
    ui.track = 0; light({}); // kits only: Resample, Trim Sample, Warp
    CHECK(is(10, Hue::None, 0) && is(12, Hue::None, 0) && is(14, Hue::None, 0));
    ui.track = 1; ui.shift = false;
    // Erase held on a kit, stopped: the pads that would empty. A pad copy: from, then where.
    ui.erase = true; light(plays);
    CHECK(is(0, Hue::Red, 1) && is(12, Hue::None, 0));
    ui.erase = false; ui.copying = UiState::Copying::Pads; light(plays);
    CHECK(is(0, Hue::Track, 2) && is(12, Hue::None, 0));
    ui.copy_from = 4; light(plays);
    CHECK(is(4, Hue::White, 4) && is(0, Hue::Red, 1) && is(12, Hue::Track, 1));
    ui.copying = UiState::Copying::Off; ui.copy_from = -1;
    // The Name sheet: keys white, delete red, the one just typed bright for the retap window.
    ui.name.start(NameTarget::ProjectSave, "", 12, false); ui.name.last_pad = 6; ui.name.last_at = 900; light({}, 1000);
    CHECK(is(0, Hue::White, 2) && is(15, Hue::Red, 2) && is(6, Hue::White, 4));
    ui.name = NameEntry {};
    // A question: what it is about flashes red, the rest dark; Exit: every pad dark.
    ui.ask = UiState::Ask::ClearPattern; ui.ask_target = 2; ui.seq_held = true; light({}, 1000);
    CHECK(is(2, Hue::Red, 4) && is(0, Hue::None, 0));
    ui.seq_held = false; ui.pad_bank = true; ui.ask = UiState::Ask::ClearTrack; ui.ask_target = 5; light({}, 1000); // Pad Bank held + Erase + pad 6
    CHECK(is(5, Hue::Red, 4) && is(1, Hue::None, 0));
    ui.pad_bank = false;
    ui.ask = UiState::Ask::None; ui.seq_held = false; ui.exit_open = true; light(plays);
    for (unsigned n = 0; n < 16; ++n) CHECK(!looks[n].lit());
    ui.exit_open = false;
    // The Pad lights setting comes last: White, or Off.
    ui.settings.pad_lights = 1; light(plays);
    CHECK(looks[1].hue == Hue::White && looks[1].rgb.r == looks[1].rgb.g && looks[1].rgb.g == looks[1].rgb.b);
    ui.settings.pad_lights = 2; light(plays);
    for (unsigned n = 0; n < 16; ++n) CHECK(!looks[n].lit());
    ui.settings.pad_lights = 0;

    // One chooser, in Shell::hit's order: presses and lights agree.
    CHECK(pad_context(ui, kit, plays, 0) == PadContext::Page);
    ui.shift = true; ui.mute = true;
    CHECK(pad_context(ui, kit, plays, 0) == PadContext::Mute); // Shift + pad in mute mode solos
    ui.mute = false; ui.page = PageId::Looper;
    CHECK(pad_context(ui, kit, plays, 8) == PadContext::LoopNow && pad_context(ui, kit, plays, 7) == PadContext::Shift);
    ui.page = PageId::Stage; ui.shift = false; ui.seq_held = true; ui.name.start(NameTarget::Marker, "", 8, true);
    CHECK(pad_context(ui, kit, plays, 0) == PadContext::Name); // the sheet over the layer
    ui.name = NameEntry {}; ui.seq_held = false; ui.erase = true;
    CHECK(pad_context(ui, kit, plays, 0) == PadContext::PadErase && pad_context(ui, kit, {}, 0) == PadContext::Page);
    ui.erase = false;
    PadFacts taking = plays; taking.record_keys = true;
    CHECK(pad_context(ui, kit, taking, 0) == PadContext::RecordKeys);
    ui.shift = true; CHECK(pad_context(ui, kit, taking, 0) == PadContext::Shift); ui.shift = false; // Shift + pad jobs stay
    PadFacts card; card.card = true; ui.exit_open = true;
    CHECK(pad_context(ui, kit, card, 0) == PadContext::Page); // SD card access: the pads only play
    ui.exit_open = false;
}

// Track move (shell/track_swap.h, shell/pad_bank.h): two tracks trade places whole, and what names a track by its
// number follows them; the same swap undoes it. B1-B3 while the pads are the tracks start, switch, step back and
// end a move or copy; the pads light a move's targets without red (nothing is lost).
void track_move() {
    CHECK(swapped(3, 3, 7) == 7 && swapped(7, 3, 7) == 3 && swapped(5, 3, 7) == 5);
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    auto& p = *project;
    synth(p.tracks[2], 0);
    p.tracks[2].mix.level = 90; p.tracks[2].midi_channel = 3;
    p.tracks[6].engine = EngineKind::Sampler; p.tracks[6].pads[0].sample = 4;
    put(bank->at[2][0], 0, 60, 100);
    put(bank->at[6][1], 4, uint8_t(sampler_first_note), 90);
    Clip a; a.track = 2; a.bar = 0; a.bars = 2; CHECK(p.song.place(a));
    Clip b; b.track = 6; b.bar = 4; CHECK(p.song.place(b));
    Clip other; other.track = 9; other.bar = 1; CHECK(p.song.place(other));
    SongPunch on_track; on_track.target = 6; CHECK(p.song.add_punch(on_track));
    SongPunch on_master; on_master.target = 255; CHECK(p.song.add_punch(on_master));
    p.looper.loops[0].source = looper::source_track(2);
    p.looper.loops[1].source = looper::source_mix;
    p.knob_fx.set_track(2, true); p.knob_fx.set_pad(6, 0, true);
    p.compressor.key = 6;

    auto changed = swap_tracks(p, *bank, 2, 6);
    CHECK(p.tracks[6].engine == EngineKind::Plugin && p.tracks[6].mix.level == 90 && p.tracks[6].midi_channel == 3);
    CHECK(p.tracks[2].engine == EngineKind::Sampler && p.tracks[2].pads[0].sample == 4 && p.tracks[2].mix.level == 100);
    CHECK(!bank->at[6][0].events.empty() && bank->at[2][0].events.empty() && !bank->at[2][1].events.empty());
    CHECK(p.song.clip_at(6, 0) >= 0 && p.song.clip_at(2, 0) < 0 && p.song.clip_at(2, 4) >= 0 && p.song.clip_at(9, 1) >= 0);
    CHECK(p.song.punches[0].target == 2 && p.song.punches[1].target == 255);
    CHECK(p.looper.loops[0].source == looper::source_track(6) && p.looper.loops[1].source == looper::source_mix);
    CHECK(p.knob_fx.track_on(6) && !p.knob_fx.track_on(2) && p.knob_fx.pad_on(2, 0) && !p.knob_fx.pad_on(6, 0));
    CHECK(p.compressor.key == 2);
    CHECK(changed.song && changed.punches && changed.loops == 1 && changed.knob && changed.key);
    swap_tracks(p, *bank, 6, 2); // the undo: as it was
    CHECK(p.tracks[2].engine == EngineKind::Plugin && p.tracks[2].mix.level == 90 && p.tracks[6].pads[0].sample == 4);
    CHECK(!bank->at[2][0].events.empty() && p.song.clip_at(2, 0) >= 0 && p.song.clip_at(6, 4) >= 0 && p.song.punches[0].target == 6);
    CHECK(p.looper.loops[0].source == looper::source_track(2) && p.knob_fx.track_on(2) && p.knob_fx.pad_on(6, 0) && p.compressor.key == 6);
    // To an empty slot: a plain move, the slot it left empty; nothing that names neither changes.
    changed = swap_tracks(p, *bank, 2, 11);
    CHECK(p.tracks[11].engine == EngineKind::Plugin && !p.tracks[2].used() && p.song.clip_at(11, 0) >= 0 && p.song.clip_at(6, 4) >= 0);
    CHECK(changed.song && !changed.punches && changed.loops == 1 && changed.knob && !changed.key);
    changed = swap_tracks(p, *bank, 4, 4);
    CHECK(!changed.song && !changed.punches && !changed.loops && !changed.knob && !changed.key);

    // The screen's memory of each track goes with it, and the track in hand stays the one it was.
    UiState ui;
    ui.edit_page[2] = 3; ui.stage_group[6] = 1; ui.seq_editor[2] = 2; ui.track = 2;
    swap_track_views(ui, 2, 6);
    CHECK(ui.edit_page[6] == 3 && ui.edit_page[2] == 0 && ui.stage_group[2] == 1 && ui.seq_editor[6] == 2 && ui.track == 6);
    swap_track_views(ui, 0, 1);
    CHECK(ui.track == 6);

    // B1-B3 with Pad Bank held: B2 MOVE, B3 COPY; B1 steps back, then out; the active key again ends it.
    UiState held; held.pad_bank = true;
    CHECK(track_key(held, 0) == TrackKey::Nothing && held.copying == UiState::Copying::Off && held.pad_bank_used);
    CHECK(track_key(held, 1) == TrackKey::Started && held.copying == UiState::Copying::Tracks && held.moving && held.copy_from < 0);
    held.copy_from = 3;
    CHECK(track_key(held, 2) == TrackKey::Switched && !held.moving && held.copy_from == 3);
    CHECK(track_key(held, 0) == TrackKey::Back && held.copy_from < 0 && held.copying == UiState::Copying::Tracks);
    CHECK(track_key(held, 2) == TrackKey::Ended && held.copying == UiState::Copying::Off);
    held.pad_bank = false;
    CHECK(track_key(held, 1) == TrackKey::Ignored); // the page's keys answer
    start_track_pick(held, true); // Shift + B2 on Tracks
    CHECK(tracks_on_pads(held) && track_overlay_shown(held, 0) && held.moving);
    CHECK(track_key(held, 0) == TrackKey::Ended && !held.moving && !tracks_on_pads(held));
    held.copying = UiState::Copying::Pads; held.pad_bank = true;
    CHECK(track_key(held, 2) == TrackKey::Started && held.copying == UiState::Copying::Tracks && !held.moving); // a pad copy gives way
    held.moving = true; held.stop_copying();
    CHECK(!held.moving && held.copy_from < 0);

    // Shift on Tracks lights B2 and B3 (MOVE, COPY); elsewhere a synth track's B2 has no Shift job.
    UiState tracks; tracks.page = PageId::Tracks; tracks.track = 11;
    CHECK(button_has_shift_job(p, tracks, SurfaceButton::TopMiddle, false) && button_has_shift_job(p, tracks, SurfaceButton::TopRight, false));
    tracks.page = PageId::Keys;
    CHECK(!button_has_shift_job(p, tracks, SurfaceButton::TopMiddle, false) && !button_has_shift_job(p, tracks, SurfaceButton::TopRight, false));
    tracks.page = PageId::Punch; // Shift + B3 QUANTIZE on Pad FX
    CHECK(!button_has_shift_job(p, tracks, SurfaceButton::TopMiddle, false) && button_has_shift_job(p, tracks, SurfaceButton::TopRight, false));

    // The pads: the track picked white, a move's targets in their own colour (used Dim, empty Faint), a copy's used ones red.
    using namespace pad_light;
    EngineHost host;
    TrackTaps taps;
    std::array<Look, 16> looks;
    UiState pick; pick.pad_bank = true;
    start_track_pick(pick, true); pick.copy_from = 11;
    auto light = [&] { PadScene scene {pick, p, *bank, host, taps, PadFacts {}, 1000, nullptr, 0}; pad_looks(scene, looks); };
    light();
    CHECK(looks[11].hue == Hue::White && rung(looks[11].level) == 4);
    CHECK(looks[6].hue == Hue::Track && rung(looks[6].level) == 2 && looks[2].hue == Hue::Track && rung(looks[2].level) == 1);
    pick.moving = false; light();
    CHECK(looks[6].hue == Hue::Red && looks[2].hue == Hue::Track);
}

// Drums pads (model/scales.h, docs/MELODEE.md Keys): a drum plug-in's or MIDI drum machine's pads play as a kit's,
// 36 + 16 * bank + pad whatever the key, one lane a drum on Steps, 16 Levels in velocity alone; a drum plug-in says
// so in its library (plugin/drum_map.h) and the pads light as a kit's.
void drum_layout() {
    auto project = std::make_unique<Project>();
    project->root = 9; project->scale = 1; // A minor: a synth's pads skip and move; Drums pads do not
    auto& t = project->tracks[0];
    t.engine = EngineKind::Plugin; t.octave = 4; t.transpose = 3;
    CHECK(pad_note(*project, t, 0).key != 36 && !drum_pads(t));
    use_drum_pads(t);
    CHECK(drum_pads(t) && t.layout == PadLayout::Drums && std::string(layout_name(t.layout)) == "Drums");
    for (unsigned pad = 0; pad < 16; ++pad) CHECK(pad_note(*project, t, pad).key == int(36 + pad) && !pad_note(*project, t, pad).root);
    // The layout is the track's own: another track keeps its key and its Scale pads.
    auto& other = project->tracks[1];
    other.engine = EngineKind::Plugin; other.octave = 4;
    CHECK(other.layout == PadLayout::Scale && !drum_pads(other) && pad_note(*project, other, 0).key == 69 && pad_note(*project, other, 1).key == 71);
    t.bank = 3; CHECK(pad_note(*project, t, 0).key == 84 && pad_note(*project, t, 15).key == 99); // bank D: MIDI's to 99
    t.bank = 6; use_drum_pads(t); CHECK(t.bank == 2 && pad_note(*project, t, 0).key == 68);    // A-D only
    t.bank = 0;
    CHECK(drum_pad_of(36) == 0 && drum_pad_of(99) == 63 && drum_pad_of(35) < 0 && drum_pad_of(100) < 0);
    // A drum is no chord's root: one note, the pad's.
    t.chord = true; t.chord_type = 1;
    std::array<int, notes_per_step> keys {};
    CHECK(chord_notes(*project, t, 4, keys) == 1 && keys[0] == 40);
    // Steps: the last drum's lane (pad 1 of the bank before any); a synth's whole step.
    t.last_note = 42; CHECK(steps_lane(t) == 42);
    t.last_note = 20; CHECK(steps_lane(t) == 36);
    t.levels = true; CHECK(steps_lane(t) == -1); t.levels = false;
    auto synth = t; synth.layout = PadLayout::Scale; CHECK(steps_lane(synth) == -1 && !drum_pads(synth));
    auto empty = Track {}; empty.layout = PadLayout::Drums; CHECK(!drum_pads(empty)); // nothing to play
    auto kit = Track {}; kit.engine = EngineKind::Sampler; kit.layout = PadLayout::Drums; CHECK(!drum_pads(kit));
    // 16 Levels: Velocity alone, whatever type the track kept (another pitch is another drum).
    t.levels_kind = uint8_t(LevelsKind::Scale);
    use_drum_pads(t); CHECK(t.levels_kind == uint8_t(LevelsKind::Velocity));
    t.levels_kind = uint8_t(LevelsKind::Loop); use_drum_pads(t); CHECK(t.levels_kind == uint8_t(LevelsKind::Loop));
    t.levels_kind = uint8_t(LevelsKind::Tune);
    auto level = levels_of(*project, t, 15);
    CHECK(level.key < 0 && level.semitones == 0 && level.velocity > 0);
    std::array<LevelsKind, 4> types {};
    CHECK(levels_types(t, types) == 1 && types[0] == LevelsKind::Velocity && levels_types(synth, types) == 3);
    // K3 on Keys reaches Drums.
    UiState ui; ui.page = PageId::Keys; ui.track = 0;
    Pattern pattern;
    CHECK(page_bindings(*project, ui, pattern)[2].kind == Binding::Kind::Layout && page_bindings(*project, ui, pattern)[2].max == int(PadLayout::Drums));
    // The project keeps it: a MIDI track to a drum machine, on bank C.
    {
        auto saved = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        load_sound(saved->tracks[4], EngineKind::Midi, 0);
        use_drum_pads(saved->tracks[4]); saved->tracks[4].bank = 2;
        auto bytes = save_project(*saved, *bank);
        auto back = std::make_unique<Project>();
        CHECK(load_project(bytes, *back, *bank).empty());
        CHECK(back->tracks[4].layout == PadLayout::Drums && drum_bank(back->tracks[4]) == 2 && pad_note(*back, back->tracks[4], 1).key == 69);
    }

    // The library's maps: 8W8 carries GM's and Rack 36's; the pads play Rack 36's (all sixteen on bank A).
    std::string so = "\x7f" "ELF junk {\"pad_layout\":\"keys\",\"levels\":{\"a\":{\"name\":\"A\",\"note\":40}}}"; // not drums
    so.push_back('\0');
    so += R"({"pad_layout":"drums","focus_param":"x","levels":{"bd":{"name":"Bass Drum","knobs":["bd_tune",1,true,null],)"
          R"("params":[{"key":"bd_tune","label":"Tune"}],"note":36,"role":"kick"},"sd":{"name":"Snare","note":38},)"
          R"("rev":{"name":"Reverb","knobs":["rev_decay"]},"root":{"name":"8W8","params":[{"level":"bd"}]}}})";
    so.push_back('\0');
    so += R"({ "pad_layout" : "drums", "levels" : { "bd" : { "name" : "Bass Drum", "note" : 36 }, "sd" : { "name" : "Sn\"are", "note" : 37 },)"
          R"( "cp" : { "name" : "Hand Clap", "note" : 47.0 }, "bad" : { "name" : "Way Up", "note" : 200 } } })";
    so.push_back('\0');
    so += R"({"pad_layout":"drums","levels":{"bd":{"name":"Cut)"; // cut short: no map
    auto maps = drum_maps_in(so);
    CHECK(maps.size() == 2 && maps[0].size() == 2 && maps[0][1].key == 38 && maps[0][1].name == "Snare");
    CHECK(maps[1].size() == 3 && maps[1][1].name == "Sn\"are" && maps[1][2].key == 47);
    auto voices = drum_voices_in(so); // more on bank A wins: three against two
    CHECK(voices.size() == 3 && voices[2].key == 47);
    CHECK(drum_voices_in("no maps here").empty() && drum_voices_in("").empty());
    std::string rack = R"({"pad_layout":"drums","levels":{"a":{"name":"Lo","note":36},"b":{"name":"Hi","note":37},"c":{"name":"Up","note":38}}})";
    voices = drum_voices_in(so + '\0' + rack); // three each on bank A: the one spanning fewer keys (9W9's Rack over its GM)
    CHECK(voices.size() == 3 && voices[0].name == "Lo");

    // The pads light as a kit's: the drum hit last Full, the rest Dim; with the plug-in's voices, a pad without one dark.
    using namespace pad_light;
    auto bank = std::make_unique<PatternBank>();
    EngineHost host;
    TrackTaps taps;
    UiState lit; lit.track = 0; lit.page = PageId::Keys;
    t.layout = PadLayout::Drums; t.last_note = 38; t.chord = false;
    std::array<Look, 16> looks;
    auto light = [&] { PadScene scene {lit, *project, *bank, host, taps, PadFacts {}, 1000, nullptr, 0}; pad_looks(scene, looks); };
    auto is = [&](unsigned pad, Hue hue, unsigned r) { return looks[pad].hue == hue && rung(looks[pad].level) == r; };
    light();
    CHECK(is(2, Hue::Track, 4) && is(0, Hue::Track, 2) && is(15, Hue::Track, 2));
    lit.drum_voices.entry = 0;
    std::snprintf(lit.drum_voices.names[0].data(), 16, "Bass Drum");
    std::snprintf(lit.drum_voices.names[2].data(), 16, "Low Tom");
    CHECK(lit.drum_voices.any() && std::string(lit.drum_voices.at(36)) == "Bass Drum" && !lit.drum_voices.at(37) && !lit.drum_voices.at(200));
    light();
    CHECK(is(2, Hue::Track, 4) && is(0, Hue::Track, 2) && is(1, Hue::None, 0));
}

// The Looper's way in and its loop sheet (docs/LOOPER.md): Loop and 16 Levels pressed together, in either order,
// open it with neither's own job (shell/looper_chord.h); the sheet behind B3 MORE sets on the pots and acts on the
// keys, every job of the old list still there (shell/loop_sheet.h).
void looper_chord_and_sheet() {
    using B = SurfaceButton;
    // Loop first, then 16 Levels: the Looper; both releases do nothing (Loop's is a hold's, 16 Levels' is used).
    UiState ui;
    CHECK(looper_chord(ui, B::Loop, true) == ChordAct::Own);
    ui.loop_held = true; ui.loop_grip = false; // as loop_press leaves it: a tap so far
    CHECK(looper_chord(ui, B::SixteenLevels, true) == ChordAct::Looper);
    CHECK(ui.loop_grip && ui.levels_held && ui.levels_used);
    CHECK(looper_chord(ui, B::SixteenLevels, false) == ChordAct::Nothing && !ui.levels_held && !ui.levels_used);
    CHECK(looper_chord(ui, B::Loop, false) == ChordAct::Own && ui.loop_grip); // loop_press: a grip lets go doing nothing
    // 16 Levels first, then Loop: the same, and 16 Levels' toggle never comes.
    ui = UiState {};
    CHECK(looper_chord(ui, B::SixteenLevels, true) == ChordAct::Nothing && ui.levels_held && !ui.levels_used); // waits for its release
    CHECK(looper_chord(ui, B::Loop, true) == ChordAct::Looper && ui.loop_held && ui.loop_grip && !ui.loop_shift_held);
    CHECK(looper_chord(ui, B::Loop, false) == ChordAct::Own);
    CHECK(looper_chord(ui, B::SixteenLevels, false) == ChordAct::Nothing);
    // 16 Levels alone: its job on release; Shift + 16 Levels: the type as it goes down, its release nothing.
    ui = UiState {};
    CHECK(looper_chord(ui, B::SixteenLevels, true) == ChordAct::Nothing);
    CHECK(looper_chord(ui, B::SixteenLevels, false) == ChordAct::Levels && !ui.levels_held);
    ui.shift = true;
    CHECK(looper_chord(ui, B::SixteenLevels, true) == ChordAct::Levels && ui.levels_used);
    CHECK(looper_chord(ui, B::SixteenLevels, false) == ChordAct::Nothing);
    // Shift + Loop down, then 16 Levels: still the Looper, and no Reverse or Looper on / off after it.
    ui = UiState {}; ui.shift = true; ui.loop_shift_held = true;
    CHECK(looper_chord(ui, B::SixteenLevels, true) == ChordAct::Looper && !ui.loop_shift_held && ui.loop_held && ui.loop_grip);
    // 16 Levels held with Shift, then Loop: the Looper too.
    ui = UiState {}; ui.shift = true;
    CHECK(looper_chord(ui, B::SixteenLevels, true) == ChordAct::Levels);
    ui.loop_shift_held = false;
    CHECK(looper_chord(ui, B::Loop, true) == ChordAct::Looper && ui.loop_grip);
    // Loop alone is Loop's, and other buttons are not the chord's.
    ui = UiState {};
    CHECK(looper_chord(ui, B::Loop, true) == ChordAct::Own && looper_chord(ui, B::Loop, false) == ChordAct::Own);
    CHECK(looper_chord(ui, B::Chop, true) == ChordAct::Own && !ui.levels_held);

    // The sheet's keys: B1 BACK, B2 COMMIT, B3 PLAY / STOP on the boundary; Shift: CLEAR, + NOTE, at once.
    CHECK(loop_job(0, false) == LoopJob::Back && loop_job(1, false) == LoopJob::Commit && loop_job(2, false) == LoopJob::Toggle);
    CHECK(loop_job(0, true) == LoopJob::Clear && loop_job(1, true) == LoopJob::CommitNote && loop_job(2, true) == LoopJob::Now);
    // What each does in each state; BACK always.
    using S = Looper::State;
    auto view = [](S state, uint32_t length) { Looper::View v; v.state = state; v.length = length; return v; };
    auto can = [&](LoopJob job, S state, uint32_t length, bool kit = true) { return loop_job_can(job, view(state, length), kit); };
    for (auto state : {S::Empty, S::Armed, S::Recording, S::Playing, S::Overdub, S::Stopped}) CHECK(can(LoopJob::Back, state, 0));
    // Empty: only BACK (and the cards).
    CHECK(!can(LoopJob::Commit, S::Empty, 0) && !can(LoopJob::Toggle, S::Empty, 0) && !can(LoopJob::Clear, S::Empty, 0)
          && !can(LoopJob::CommitNote, S::Empty, 0) && !can(LoopJob::Now, S::Empty, 0));
    // Armed and the first take: no audio yet, so no PLAY / STOP on the boundary; STOP NOW cancels or closes; CLEAR asks.
    CHECK(!can(LoopJob::Toggle, S::Armed, 0) && can(LoopJob::Now, S::Armed, 0) && can(LoopJob::Clear, S::Armed, 0));
    CHECK(!can(LoopJob::Toggle, S::Recording, 0) && can(LoopJob::Now, S::Recording, 0) && !can(LoopJob::Commit, S::Recording, 0));
    // Playing or stopped: everything; COMMIT and + NOTE only with a kit pad to land on; never while overdubbing.
    for (auto job : {LoopJob::Commit, LoopJob::Toggle, LoopJob::Clear, LoopJob::CommitNote, LoopJob::Now}) {
        CHECK(can(job, S::Playing, 44100) && can(job, S::Stopped, 44100));
    }
    CHECK(!can(LoopJob::Commit, S::Playing, 44100, false) && !can(LoopJob::CommitNote, S::Stopped, 44100, false));
    CHECK(!can(LoopJob::Commit, S::Overdub, 44100) && can(LoopJob::Toggle, S::Overdub, 44100) && can(LoopJob::Now, S::Overdub, 44100));
    // STOP or PLAY: a loop going on stops (Halt), a stopped one plays at once.
    CHECK(loop_going(view(S::Armed, 0)) && loop_going(view(S::Recording, 0)) && loop_going(view(S::Playing, 1)) && loop_going(view(S::Overdub, 1)));
    CHECK(!loop_going(view(S::Stopped, 1)) && !loop_going(view(S::Empty, 0)));
    // The pad a commit lands on: the kit's next empty one after its last, round; none on a synth or a full kit.
    Track kit; kit.engine = EngineKind::Sampler;
    for (unsigned n = 0; n < 16; ++n) kit.pads[n].sample = uint16_t(n);
    kit.last_pad = 8;
    CHECK(commit_pad(kit) == 16);
    kit.last_pad = sampler_pads - 1;
    CHECK(commit_pad(kit) == 16);
    kit.pads[sampler_pads - 1].sample = 3; kit.last_pad = 2;
    CHECK(commit_pad(kit) == 16);
    for (auto& p : kit.pads) p.sample = 1;
    CHECK(commit_pad(kit) == -1);
    Track synth; synth.engine = EngineKind::Plugin;
    CHECK(commit_pad(synth) == -1);

    // K1 SOURCE turns through what Push used to step: the input, the tracks there are, the other loops, the mix,
    // the hardware inputs that do not clash; its own source always among them.
    auto project = std::make_unique<Project>();
    project->tracks[0].engine = EngineKind::Plugin; project->tracks[2].engine = EngineKind::Sampler;
    std::array<uint8_t, looper::source_count> sources {};
    unsigned at = 99, count = loop_sources(*project, 0, sources, at);
    CHECK(count == 1 + 2 + 3 + 1 + unsigned(looper::direct_inputs.size()) && at == 0);
    CHECK(sources[0] == looper::source_input && sources[1] == looper::source_track(0) && sources[2] == looper::source_track(2));
    CHECK(sources[3] == looper::source_loop(1) && sources[5] == looper::source_loop(3) && sources[6] == looper::source_mix);
    CHECK(sources[7] == looper::source_direct(0) && sources[count - 1] == looper::source_of(InputSource::Mic));
    for (unsigned n = 0; n < count; ++n) CHECK(sources[n] != looper::source_loop(0)); // never itself
    // L2 on the mic: L1 can no longer take the rear inputs (one converter), but the mic and USB still.
    project->looper.loops[1].source = looper::source_of(InputSource::Mic);
    count = loop_sources(*project, 0, sources, at);
    CHECK(count == 7 + 4);
    for (unsigned n = 0; n < count; ++n) CHECK(!loop_source_clashes(project->looper, 0, sources[n]));
    CHECK(sources[count - 1] == looper::source_of(InputSource::Mic));
    // A source that would be skipped stays while it is the loop's own (a track emptied since): K1 starts on it.
    project->looper.loops[0].source = looper::source_track(5);
    count = loop_sources(*project, 0, sources, at);
    CHECK(sources[at] == looper::source_track(5) && at == 3 && count == 12);

    // The cards: the sheet keeps the page's but for K1, the source in place of the loop; Shift as on the page,
    // from LOOPS or MIX alike; the sheet shut, the page's own (MIX its strip); the Looper off, none.
    project->looper.on = true;
    project->looper.loops[1].bars = 4;
    project->loop_mix[1].level = 90;
    UiState lp; lp.page = PageId::Looper; lp.loop = 1; lp.loop_sheet = true;
    Pattern pattern;
    auto cards = page_bindings(*project, lp, pattern);
    count = loop_sources(*project, 1, sources, at);
    CHECK(cards[0].kind == Binding::Kind::LoopSource && cards[0].id == 1 && cards[0].max == int(count) - 1 && cards[0].value == int(at));
    CHECK(sources[size_t(cards[0].value)] == looper::source_of(InputSource::Mic));
    CHECK(cards[1].kind == Binding::Kind::LoopLength && looper::lengths[size_t(cards[1].value)] == 4);
    CHECK(cards[2].kind == Binding::Kind::LoopLevel && cards[2].value == 90);
    lp.looper_view = 1;
    CHECK(page_bindings(*project, lp, pattern)[0].kind == Binding::Kind::LoopSource);
    lp.shift = true;
    auto second = page_bindings(*project, lp, pattern);
    CHECK(second[0].kind == Binding::Kind::LoopFeedback && second[1].kind == Binding::Kind::LoopQuantize && second[2].kind == Binding::Kind::LoopThen);
    lp.shift = false; lp.loop_sheet = false;
    CHECK(page_bindings(*project, lp, pattern)[0].kind == Binding::Kind::LoopLevel); // MIX
    lp.looper_view = 0;
    CHECK(page_bindings(*project, lp, pattern)[0].kind == Binding::Kind::LoopPick);
    project->looper.on = false; lp.loop_sheet = true;
    CHECK(!page_bindings(*project, lp, pattern)[0].bound());
    // Note Repeat latched leaves the Looper's keys alone (its pads repeat nothing there).
    project->tracks[0].arp = true; lp.track = 0;
    CHECK(!repeat_keys(*project, lp));
    lp.page = PageId::Keys;
    CHECK(repeat_keys(*project, lp));
}

// Roadmap P0: a hit lands in Seq Record where it was heard, and plays at its own frame, a block late.
void hits_on_time() {
    // The place's arithmetic: counted back from the playhead and wrapped; a step snapped onto still to come or not.
    CHECK(wrap_tick(-3, 384) == 381 && wrap_tick(390, 384) == 6 && wrap_tick(5, 384) == 5);
    CHECK(still_ahead(100, 120, 102, 384));  // the playhead two ticks on: the step is still to come, skipped once
    CHECK(!still_ahead(100, 120, 121, 384)); // it played already
    CHECK(!still_ahead(100, 96, 102, 384));  // snapped back: behind the hit
    CHECK(still_ahead(380, 0, 382, 384));    // onto the first step, next time round
    CHECK(!still_ahead(3, 0, 5, 384));       // back onto the first step: behind
    CHECK(still_ahead(100, 101, 99, 384));   // the hit rounded up to a tick the playhead has not played

    // Where a hit was heard: its stamp less the output latency (three periods), on the nearest tick, though the
    // playhead has moved on since.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        synth(project->tracks[0], 0);
        project->tempo = 1200; // 120 BPM: a tick every 229.6875 frames at 44.1 kHz
        EngineHost host;
        host.load(*project, *bank);
        tones_for(host, *project);
        host.set_audio_format(44100, 384);
        std::vector<int32_t> out(256);
        host.send({Command::Type::Play}); // the patterns from tick 0, its first tick on the first frame
        for (int b = 0; b < 40; ++b) host.render(out.data(), 128, 1);
        const auto& clock = host.audio_clock();
        CHECK(clock.known() && host.position_of(0) > 20);
        const double tick = 44100 * 60.0 / 120 / 96;
        const int64_t latency_ns = int64_t(384 * 1e9 / 44100);
        int position = -1;
        uint32_t ticks = 0;
        // In as tick 10's frame reached the jacks: tick 10.
        CHECK(host.heard_place(0, uint64_t(clock.heard_at(uint64_t(10 * tick))), position, ticks) && position == 10 && ticks == 11);
        // In as that frame was rendered: heard 384 frames (1.67 ticks) before, tick 8. The playhead's own place would be 22.
        CHECK(host.heard_place(0, uint64_t(clock.heard_at(uint64_t(10 * tick)) - latency_ns), position, ticks) && position == 8);
        // The nearest tick: four tenths of a tick on, the same; six tenths, the next.
        CHECK(host.place_at(0, uint64_t(10.4 * tick), position, ticks) && position == 10);
        CHECK(host.place_at(0, uint64_t(10.6 * tick), position, ticks) && position == 11 && ticks == 12);
        CHECK(host.place_at(0, 0, position, ticks) && position == 0); // no tick of the run before its first
        CHECK(!host.heard_place(0, 0, position, ticks));              // no stamp: none (the playhead, in Context::record)
        host.send({Command::Type::Stop});
        host.render(out.data(), 128, 1);
        CHECK(!host.place_at(0, uint64_t(10 * tick), position, ticks)); // stopped: none
    }

    // Two hits 5 ms apart (220.5 frames) come out 5 ms apart, each a block after the frame it came in at, not on
    // block starts; one that came in after its frame (a late MIDI thread) plays at once and is counted.
    {
        using K = MidiOutEvent::Kind;
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        synth(project->tracks[0], 0);
        synth(project->tracks[1], 1);
        EngineHost host;
        host.load(*project, *bank);
        tones_for(host, *project);
        host.set_audio_format(44100, 384);
        host.send({Command::Type::MidiTrack, 0, 0, 5, 1}); // MIDI out too, to the frame
        host.send({Command::Type::MidiTrack, 1, 0, 6, 1});
        std::vector<int32_t> out(256);
        std::vector<float> stems(size_t(track_count) * EngineHost::block);
        std::array<std::vector<float>, 2> heard;
        auto render = [&] {
            host.render(out.data(), 128, 1, nullptr, stems.data());
            for (unsigned t = 0; t < 2; ++t) heard[t].insert(heard[t].end(), stems.begin() + t * 128, stems.begin() + (t + 1) * 128);
        };
        for (int b = 0; b < 4; ++b) render();
        const auto& clock = host.audio_clock();
        // In 1 ms into rendering frame 512's block, and 5 ms after.
        const int64_t first = clock.heard_at(512) - int64_t(384 * 1e9 / 44100) + 1000000, second = first + 5000000;
        const uint64_t f1 = clock.frame_at(first) + 128, f2 = clock.frame_at(second) + 128;
        CHECK(f2 - f1 >= 220 && f2 - f1 <= 221 && f1 % 128 != 0 && f2 % 128 != 0);
        host.send_at({Command::Type::NoteOn, 0, 0, 60, 100}, uint64_t(first));
        host.send({Command::Type::Param, 1, 0, 0, project->tracks[1].params[0]}); // untimed: waits behind, still applied
        host.send_at({Command::Type::NoteOn, 1, 0, 60, 100}, uint64_t(second));
        for (int b = 0; b < 8; ++b) render();
        std::vector<uint64_t> on;
        MidiOutEvent e;
        while (host.take_midi(e)) if (e.kind == K::NoteOn) on.push_back(e.frame);
        CHECK(on.size() == 2 && on[0] == f1 && on[1] == f2);
        auto onset = [&](const std::vector<float>& v) { for (size_t n = 0; n < v.size(); ++n) if (v[n] != 0) return n; return v.size(); };
        CHECK(onset(heard[0]) >= f1 && onset(heard[0]) <= f1 + 2);
        CHECK(onset(heard[1]) >= f2 && onset(heard[1]) <= f2 + 2);
        CHECK(host.timed_kept() == 2 && host.timed_missed() == 0);
        // Late: its frame went by before it came (the MIDI thread woke late). It plays at the next block's start.
        Command late {Command::Type::NoteOn, 0, 0, 62, 100};
        late.timed = true; late.at = uint32_t(f1);
        host.send(late);
        render();
        on.clear();
        while (host.take_midi(e)) if (e.kind == K::NoteOn) on.push_back(e.frame);
        CHECK(on.size() == 1 && on[0] == 12 * 128 && host.timed_missed() == 1 && host.timed_worst_frames() == 12 * 128 - f1);
    }
}

// The audio audit's fixes (2026-11-12): nothing steps where the sound should run on, nothing comes back that
// had gone, and nothing loud is clipped on its way to a file.
void audio_quality() {
    dsp::device_rate.store(44100.0f);
    auto largest_step = [](const std::vector<float>& y, size_t from, size_t to) {
        float most = 0;
        for (size_t k = std::max<size_t>(from, 1); k < std::min(to, y.size()); ++k) most = std::max(most, std::fabs(y[k] - y[k - 1]));
        return most;
    };
    auto sine = [](unsigned frames, double hz, float level) {
        SampleData s;
        s.frames = frames; s.left.resize(frames); s.right.resize(frames);
        for (unsigned n = 0; n < frames; ++n) s.left[n] = s.right[n] = float(level * std::sin(2 * M_PI * hz * n / 44100));
        return s;
    };
    auto play = [](SamplerEngine& e, std::vector<float>& l, unsigned frames) {
        l.assign(frames, 0.0f);
        std::vector<float> r(frames, 0.0f);
        for (unsigned at = 0; at < frames; at += 128) e.render_stereo(l.data() + at, r.data() + at, std::min(128u, frames - at));
    };
    // A kit pad's seamless one-cycle loop plays in tune: 100 frames a turn, not 99.
    {
        SampleData cycle = sine(100, 441, 1);
        SamplerEngine::Table table {};
        table[0] = &cycle;
        SamplerEngine e;
        e.set_samples(&table);
        PadSound pad; pad.sample = 0; pad.loop = true;
        e.set_pad(0, pad);
        e.note_on(int(sampler_first_note), 127);
        std::vector<float> l;
        play(e, l, 44100);
        unsigned rises = 0;
        double first = -1, last = 0;
        for (size_t k = 22051; k < l.size(); ++k)
            if (l[k - 1] < 0 && l[k] >= 0) { double t = double(k - 1) + l[k - 1] / (l[k - 1] - l[k]); if (first < 0) first = t; last = t; ++rises; }
        CHECK(rises > 1 && std::fabs((rises - 1) / ((last - first) / 44100) - 441) < 0.5); // it played 445.45 Hz
    }
    // A full kit: the 33rd voice fades the oldest out (a choke's fade), never cuts it dead.
    {
        SampleData tone = sine(88200, 200, 0.5f), quiet = sine(88200, 200, 0);
        SamplerEngine::Table table {};
        table[0] = &tone; table[1] = &quiet;
        SamplerEngine e;
        e.set_samples(&table);
        PadSound a; a.sample = 0;
        e.set_pad(0, a);
        PadSound b; b.sample = 1;
        for (unsigned p = 1; p < 40; ++p) e.set_pad(p, b);
        std::vector<float> l(128 * 40, 0.0f), r(128 * 40, 0.0f);
        unsigned at = 0;
        auto run = [&](unsigned blocks) { for (unsigned k = 0; k < blocks; ++k, at += 128) e.render_stereo(l.data() + at, r.data() + at, 128); };
        e.note_on(int(sampler_first_note), 127);
        run(4);
        for (unsigned p = 1; p < 32; ++p) { e.note_on(int(sampler_first_note + p), 127); run(1); }
        float natural = largest_step(l, 1, at);
        unsigned steal = at;
        e.note_on(int(sampler_first_note + 32), 127);
        run(4);
        CHECK(largest_step(l, steal - 1, at) < 2 * natural); // it jumped 25 times the sine's own step
    }
    // Cloud grains read between frames, and never off their region's end (round to the file's start, as they did).
    {
        SampleData high = sine(44100, 5000, 0.5f);
        SamplerEngine::Table table {};
        table[0] = &high;
        SamplerEngine e;
        e.set_samples(&table);
        e.set_param(sampler::Cloud, 1); e.set_param(sampler::Spread, 0); e.set_param(sampler::Size, 60); e.set_param(sampler::Density, 20);
        PadSound pad; pad.sample = 0; pad.pitch = 1; // a semitone up: every grain reads between frames
        e.set_pad(0, pad);
        e.note_on(int(sampler_first_note), 127);
        std::vector<float> l;
        play(e, l, 44100);
        double hz = 5000 * std::pow(2.0, 1 / 12.0), tone = 0, worst = -300;
        auto level = [&](double f) {
            double re = 0, im = 0, sum = 0;
            for (size_t k = 0; k < 16384; ++k) {
                double w = 0.5 - 0.5 * std::cos(2 * M_PI * k / 16383), ph = 2 * M_PI * f * k / 44100;
                re += l[8192 + k] * w * std::cos(ph); im -= l[8192 + k] * w * std::sin(ph); sum += w;
            }
            return 20 * std::log10(std::max(2 * std::sqrt(re * re + im * im) / sum, 1e-12));
        };
        tone = level(hz);
        for (double f = 300; f < 20000; f += 25) if (std::fabs(f - hz) > 150) worst = std::max(worst, level(f));
        CHECK(tone - worst > 30); // nearest-frame reads left a spur 20 dB under it
        SampleData edged = sine(44100, 200, 0);
        for (unsigned n = 0; n < 44100; ++n) if (n < 10000 || n >= 30000) edged.left[n] = edged.right[n] = (n % 7 < 3 ? 0.9f : -0.9f);
        table[0] = &edged;
        SamplerEngine cut;
        cut.set_samples(&table);
        cut.set_param(sampler::Cloud, 1); cut.set_param(sampler::Spread, 0); cut.set_param(sampler::Size, 80); cut.set_param(sampler::Density, 50);
        cut.set_param(sampler::Position, 98);
        PadSound region; region.sample = 0; region.start = 10000; region.end = 30000; // silent inside, loud outside
        cut.set_pad(0, region);
        cut.note_on(int(sampler_first_note), 127);
        play(cut, l, 88200);
        CHECK(peak_of(l) == 0);
    }
    // An echo bypassed and back: what it held when it was bypassed never plays again.
    {
        DelayPool pool;
        Effect echo;
        echo.set_kind(EffectKind::Echo, pool);
        std::vector<float> l(128), r(128);
        float after = 0;
        for (unsigned block = 0; block < 400; ++block) {
            std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
            if (block == 0) l[0] = r[0] = 1.0f;
            if (block == 20) echo.set_bypass(true);  // before its first echo
            if (block == 200) echo.set_bypass(false);
            echo.process(l.data(), r.data(), 128);
            if (block >= 200) for (float x : l) after = std::max(after, std::fabs(x));
        }
        CHECK(after < 1e-6f); // it played the old impulse at 0.3
    }
    // Punch over a running sound: a repeat's turns, Stutter's edges and Upper's windows never step.
    for (PunchKind kind : {PunchKind::Loop32, PunchKind::Stutter, PunchKind::Upper, PunchKind::RevStepper}) {
        Punch punch;
        std::vector<float> l(128), r(128);
        unsigned n = 0;
        float worst = 0, before = 0;
        for (unsigned block = 0; block < 1000; ++block) {
            for (unsigned f = 0; f < 128; ++f, ++n) l[f] = r[f] = float(0.5 * std::sin(2 * M_PI * 220.0 * n / 44100));
            if (block == 100) punch.start(kind, 1.0f, 88200, 88200 / 16, 0);
            punch.process(l.data(), r.data(), 128);
            for (unsigned f = 0; f < 128; ++f) { if (block >= 100) worst = std::max(worst, std::fabs(l[f] - before)); before = l[f]; }
        }
        float own = float(0.5 * 2 * M_PI * 220 / 44100) * (kind == PunchKind::Upper ? 2 : 1); // Upper plays it an octave up
        if (worst > 2 * own) std::fprintf(stderr, "%s step %f\n", punch_name(kind), worst);
        CHECK(worst < 2 * own); // Loop 32 stepped 0.085, Stutter 0.39, Rev Stepper 0.41, Upper 0.97
    }
    // A track's level and mute, and main volume, ease: no step bigger than the sound's own.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        load_sound(project->tracks[0], EngineKind::Sampler, 0);
        project->tracks[0].pads[0].sample = 0; project->tracks[0].pads[0].loop = true;
        SampleData tone = sine(44100, 200, 0.5f); // 200 whole cycles: a seamless loop
        EngineHost host;
        host.load(*project, *bank);
        CHECK(host.offer_sample(0, &tone));
        std::vector<int32_t> out(256);
        std::vector<float> heard;
        float volume = 1;
        auto run = [&](unsigned blocks) {
            for (unsigned b = 0; b < blocks; ++b) {
                host.render(out.data(), 128, volume);
                for (unsigned f = 0; f < 128; ++f) heard.push_back(out[2 * f] / 2147483648.0f);
            }
        };
        host.send({Command::Type::NoteOn, 0, 0, uint16_t(sampler_first_note), 127});
        run(20);
        float natural = largest_step(heard, heard.size() - 128 * 10, heard.size());
        CHECK(natural > 0.005f);
        auto eased = [&](const Command& c, float to_volume) {
            size_t from = heard.size();
            host.send(c);
            volume = to_volume;
            run(8);
            return largest_step(heard, from - 1, heard.size()) < 1.5f * natural;
        };
        CHECK(eased({Command::Type::Level, 0, 0, 0, 50}, 1));   // 0 dB to -12 dB
        CHECK(eased({Command::Type::Level, 0, 0, 0, 100}, 1));
        CHECK(eased({Command::Type::Mute, 0, 0, 0, 1}, 1));     // a mute cut it in one sample
        CHECK(peak_of(heard, heard.size() - 128) == 0);           // and it is silent once eased out
        CHECK(eased({Command::Type::Mute, 0, 0, 0, 0}, 1));
        CHECK(eased({Command::Type::Pan, 0, 0, 0, -50}, 1));
        CHECK(eased({Command::Type::Pan, 0, 0, 0, 0}, 0.25f)); // main volume a quarter at once
    }
    // A mixdown past full scale reads back whole (32-bit float), not clipped at 0 dBFS; a hot sample saved too.
    {
        std::string path = "/tmp/melodee-loud-test.wav";
        WavStream wav;
        CHECK(wav.open(path));
        std::vector<float> loud(2 * 100);
        for (unsigned f = 0; f < 100; ++f) { loud[2 * f] = 1.5f * std::sin(f * 0.1f); loud[2 * f + 1] = -loud[2 * f]; }
        CHECK(wav.add(loud.data(), 100) && wav.close());
        SampleData read;
        CHECK(read_wav(path, read).empty() && read.frames == 100 && std::fabs(read.left[15] - loud[30]) < 1e-6f);
        SampleData hot;
        hot.frames = 3; hot.left.resize(3); hot.right.resize(3);
        hot.left[0] = 1.25f; hot.left[1] = -0.5f; hot.right[0] = -1.75f; hot.right[1] = 0.25f;
        CHECK(write_wav(path, hot).empty() && read_wav(path, read).empty() && std::fabs(read.left[0] - 1.25f) < 1e-6f
              && std::fabs(read.right[0] + 1.75f) < 1e-6f);
        std::remove(path.c_str());
    }
}

// Roadmap P1: what a pattern's notes play, beyond once on their tick. The note-on frames and velocities a track sends
// out over MIDI while its pattern `p` plays for `blocks` blocks, at 120 BPM and 44.1 kHz.
std::vector<MidiOutEvent> played_notes(const Pattern& p, unsigned blocks, EngineKind engine = EngineKind::Midi,
                                       const std::function<void(EngineHost&, unsigned)>& each = {}) {
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    load_sound(project->tracks[0], engine, 0);
    if (engine == EngineKind::Midi) { project->tracks[0].params[midi_track::Port] = midi_track::UsbC; project->tracks[0].params[midi_track::Channel] = 1; }
    project->tempo = 1200;
    bank->at[0][0] = p;
    EngineHost host;
    host.load(*project, *bank);
    host.set_audio_format(44100, 384);
    if (engine != EngineKind::Midi) host.send({Command::Type::MidiTrack, 0, 0, 1, 1}); // Pad MIDI Out: always
    host.send({Command::Type::Play});
    std::vector<int32_t> out(256);
    std::vector<MidiOutEvent> notes;
    for (unsigned b = 0; b < blocks; ++b) {
        if (each) each(host, b);
        host.render(out.data(), 128, 1);
        MidiOutEvent e;
        while (host.take_midi(e)) if (e.kind == MidiOutEvent::Kind::NoteOn) notes.push_back(e);
    }
    return notes;
}

void ratchets() {
    const double tick = 44100 * 60.0 / 120 / 96; // 229.6875 frames
    // The hits' velocities: the ramp ends the last at so much of the note's, the hits between evenly.
    Event e;
    e.velocity = 100; e.ratchet = 4; e.ramp = -50;
    CHECK(e.hits() == 4 && e.hit_velocity(100, 0) == 100 && e.hit_velocity(100, 1) == 83 && e.hit_velocity(100, 3) == 50);
    e.ramp = 100;
    CHECK(e.hit_velocity(100, 3) == 127 && e.hit_velocity(40, 3) == 80); // up to twice, at most 127
    e.pitch_ramp = 7;
    CHECK(e.hit_pitch(0) == 0 && e.hit_pitch(1) == 2 && e.hit_pitch(3) == 7);
    e.pitch_ramp = -5;
    CHECK(e.hit_pitch(2) == -3);
    e.ratchet = 0; CHECK(e.hits() == 1); e.ratchet = 40; CHECK(e.hits() == max_ratchet);

    // A step rewritten (Steps' wheel) keeps each note's ratchet; a new note plays once.
    Pattern p;
    put(p, 2, 60, 100);
    p.events[0].ratchet = 3; p.events[0].ramp = 20;
    Step s = p.step(2);
    CHECK(s.ratchet == 3 && s.ramp == 20);
    s.notes[0].key = 62; s.notes[1] = {64, 90};
    p.set_step(2, s);
    CHECK(p.events.size() == 2 && p.events[0].key == 62 && p.events[0].ratchet == 3 && p.events[0].ramp == 20 && p.events[1].ratchet == 3);
    put(p, 5, 60, 100);
    CHECK(p.events.back().tick == 5 * sixteenth_ticks && p.events.back().ratchet == 1);

    // Played: four hits over a sixteenth, a frame apart as six ticks are, ramped down to half; a note of one hit
    // between; out over MIDI as heard.
    {
        Pattern q;
        Event r;
        r.tick = 0; r.length = sixteenth_ticks; r.key = 60; r.velocity = 100; r.ratchet = 4; r.ramp = -50;
        q.add(r);
        Event one;
        one.tick = 2 * sixteenth_ticks; one.key = 62; one.velocity = 90;
        q.add(one);
        auto notes = played_notes(q, 100); // 12800 frames: past step 3 (11025)
        CHECK(notes.size() == 5);
        const int velocities[] {100, 83, 67, 50};
        for (unsigned k = 0; k < 4; ++k) {
            CHECK(notes[k].a == 60 && notes[k].b == velocities[k]);
            CHECK(std::llabs(int64_t(notes[k].frame - notes[0].frame) - std::llround(k * 6 * tick)) <= 1); // on its frame, not a block start
        }
        CHECK(notes[4].a == 62 && notes[4].b == 90 && std::llabs(int64_t(notes[4].frame - notes[0].frame) - std::llround(48 * tick)) <= 1);
    }
    // Stopped in the middle: the hits still to come go with it.
    {
        Pattern q;
        Event r;
        r.length = 4 * sixteenth_ticks; r.key = 60; r.ratchet = 8; // a hit every 12 ticks (2756 frames)
        q.add(r);
        auto notes = played_notes(q, 60, EngineKind::Midi, [](EngineHost& host, unsigned b) { if (b == 30) host.send({Command::Type::Stop}); });
        CHECK(notes.size() == 2); // 0 and 2756; 5512 comes after the stop at 3840
    }
    // A kit: the hits play on the sampler too (and go out on its MIDI channel).
    {
        Pattern q;
        Event r;
        r.key = uint8_t(sampler_first_note + 2); r.length = sixteenth_ticks; r.ratchet = 2; r.pitch_ramp = 12;
        q.add(r);
        auto notes = played_notes(q, 60, EngineKind::Sampler);
        CHECK(notes.size() == 2 && notes[0].a == sampler_first_note + 2 && notes[1].a == sampler_first_note + 2);
    }

    // The file: ratchets in RTCH by their place in EVNT; a file without it plays every note once.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        auto& q = bank->at[2][5];
        put(q, 0, 40, 100); put(q, 4, 42, 100); put(q, 8, 44, 100);
        q.events[1].ratchet = 5; q.events[1].ramp = -30; q.events[1].pitch_ramp = -12;
        auto data = save_project(*project, *bank);
        auto back = std::make_unique<Project>();
        auto back_bank = std::make_unique<PatternBank>();
        CHECK(load_project(data, *back, *back_bank).empty() && back_bank->at[2][5] == q);
        const auto& b = back_bank->at[2][5].events;
        CHECK(b[1].ratchet == 5 && b[1].ramp == -30 && b[1].pitch_ramp == -12 && b[0].ratchet == 1 && b[2].ratchet == 1);
        std::string text(data.begin(), data.end());
        auto at = text.find("RTCH");
        CHECK(at != std::string::npos);
        data[at] = 'X'; // an older build's view: a section it skips
        CHECK(load_project(data, *back, *back_bank).empty() && back_bank->at[2][5].events[1].ratchet == 1);
    }

    // The cards: the TRIG row of Steps (B1 again) and Roll (B2 again), K1 hits and K2 the ramp; Shift + K2 a kit
    // pad's pitch ramp. Roll's Shift row is otherwise left alone.
    {
        auto project = std::make_unique<Project>();
        synth(project->tracks[0], 0);
        load_sound(project->tracks[3], EngineKind::Sampler, 0);
        UiState ui;
        Pattern q;
        put(q, 1, 60, 100);
        q.events[0].ratchet = 3; q.events[0].ramp = -40;
        ui.page = PageId::Steps; ui.step_cursor = 1;
        CHECK(page_bindings(*project, ui, q, false)[0].kind == Binding::Kind::StepLength); // the main row: its length
        ui.trig_cards = true;
        auto trig = page_bindings(*project, ui, q, false);
        CHECK(trig[0].kind == Binding::Kind::StepRatchet && trig[0].value == 3 && trig[0].min == 1 && trig[0].max == 8);
        CHECK(trig[1].kind == Binding::Kind::StepRamp && trig[1].value == -40);
        CHECK(!shift_bindings(*project, ui, q)[1].bound()); // a synth has no pitch ramp
        ui.page = PageId::Roll; ui.step_cursor = 1; ui.roll_key = 60;
        CHECK(page_bindings(*project, ui, q, false)[0].kind == Binding::Kind::EventRatchet);
        ui.trig_cards = false;
        CHECK(page_bindings(*project, ui, q, false)[0].kind == Binding::Kind::EventLength
              && shift_bindings(*project, ui, q)[0].kind == Binding::Kind::EventNudge); // Shift + K1 off the TRIG row: NUDGE (round 2)
        ui.track = 3; ui.page = PageId::Steps; ui.trig_cards = true;
        Pattern kit;
        Event hit;
        hit.tick = sixteenth_ticks; hit.key = uint8_t(sampler_first_note); hit.pitch_ramp = 5;
        kit.add(hit);
        project->tracks[3].last_pad = 0;
        CHECK(page_bindings(*project, ui, kit, true)[1].kind == Binding::Kind::StepPitchRamp && page_bindings(*project, ui, kit, true)[1].value == 5);
        // Steps on a kit: the cards are the last pad's note in the step, not the step's first.
        Event other = hit;
        other.key = uint8_t(sampler_first_note + 4); other.velocity = 30; other.pitch_ramp = 0;
        Event first = hit;
        first.key = uint8_t(sampler_first_note + 1); first.velocity = 50;
        kit.add(other); kit.add(first);
        project->tracks[3].last_pad = 4;
        ui.page = PageId::Steps; ui.trig_cards = false;
        CHECK(page_bindings(*project, ui, kit, false)[1].value == 30);
    }
}

// Trig conditions (roadmap P1): which passes a note plays on, beside its chance.
void trig_conditions() {
    // The byte: always, 1:2 .. 8:8, then the rest, named as the cards show them.
    unsigned n = 0, m = 0, count = 0;
    for (unsigned every = 2; every <= 8; ++every)
        for (unsigned which = 1; which <= every; ++which) {
            unsigned c = trig_of(which, every);
            CHECK(c >= 1 && c <= trig_passes && trig_pass(c, n, m) && n == which && m == every);
            ++count;
        }
    CHECK(count == trig_passes && trig_of(1, 2) == 1 && trig_of(8, 8) == trig_passes && trig_of(3, 2) == trig_always);
    CHECK(std::string(trig_name(trig_of(3, 4))) == "3:4" && std::string(trig_name(trig_not_nei)) == "NOT NEI" && std::string(trig_name(0)) == "ALWAYS");
    CHECK(trig_met(trig_of(1, 2), 0, false, false, false) && !trig_met(trig_of(1, 2), 1, false, false, false) && trig_met(trig_of(1, 2), 4, false, false, false));
    CHECK(trig_met(trig_of(2, 3), 1, false, false, false) && trig_met(trig_of(2, 3), 4, false, false, false) && !trig_met(trig_of(2, 3), 2, false, false, false));
    CHECK(trig_met(trig_first, 0, false, false, false) && !trig_met(trig_first, 1, false, false, false) && trig_met(trig_not_first, 3, false, false, false));
    CHECK(trig_met(trig_fill, 2, true, false, false) && !trig_met(trig_fill, 2, false, false, false) && trig_met(trig_not_fill, 2, false, false, false));
    CHECK(trig_met(trig_pre, 0, false, true, false) && trig_met(trig_not_nei, 0, false, true, false) && !trig_met(trig_nei, 0, false, true, false));
    CHECK(trig_weighed(trig_first, 100) && !trig_weighed(trig_pre, 100) && !trig_weighed(trig_always, 100) && trig_weighed(trig_always, 50));

    // Played over four passes of a beat-long pattern (172 blocks a pass), Fill held from the fourth.
    {
        Pattern p;
        p.length = ticks_per_quarter;
        auto note = [&](uint32_t tick, uint8_t key, unsigned condition) {
            Event e;
            e.tick = tick; e.length = 6; e.key = key; e.condition = uint8_t(condition);
            p.add(e);
        };
        note(0, 60, trig_of(1, 2)); note(12, 61, trig_pre); note(24, 62, trig_first); note(48, 64, trig_not_first);
        note(72, 65, trig_fill); note(84, 66, trig_not_pre);
        const double pass_frames = 44100 * 0.5;
        auto notes = played_notes(p, 680, EngineKind::Midi, [&](EngineHost& host, unsigned b) {
            if (b == unsigned(3 * pass_frames / 128) - 2) host.send({Command::Type::Fill, 0, 0, 0, 1});
        });
        std::map<int, std::vector<int>> passes;
        for (const auto& e : notes) passes[e.a].push_back(int((e.frame - notes[0].frame + 64) / pass_frames));
        CHECK((passes[60] == std::vector<int> {0, 2}) && (passes[61] == std::vector<int> {0, 2})); // 1:2, and PRE after it
        CHECK((passes[62] == std::vector<int> {0}) && (passes[64] == std::vector<int> {1, 2, 3}));  // FIRST, NOT FIRST
        CHECK((passes[65] == std::vector<int> {3}) && (passes[66] == std::vector<int> {0, 1, 2}));  // FILL, NOT PRE after it
    }
    // NEI: a kit pad's lane follows the pad below's; that pad's first lane, the track before (none: as if played).
    {
        Pattern p;
        p.length = ticks_per_quarter;
        Event a;
        a.key = uint8_t(sampler_first_note + 4); a.length = 6; a.condition = uint8_t(trig_of(2, 2));
        p.add(a);
        Event b = a;
        b.key = uint8_t(sampler_first_note + 5); b.condition = uint8_t(trig_nei);
        p.add(b);
        Event c = a;
        c.key = uint8_t(sampler_first_note + 7); c.condition = uint8_t(trig_nei); // pad 7's below (6) has none: as played
        p.add(c);
        auto notes = played_notes(p, 360, EngineKind::Sampler);
        std::map<int, int> heard;
        for (const auto& e : notes) ++heard[e.a];
        CHECK(heard[sampler_first_note + 4] == 1 && heard[sampler_first_note + 5] == 1 && heard[sampler_first_note + 7] == 3);
    }

    // The file: conditions in COND by their place in EVNT; a file without it plays every note on every pass.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        auto& q = bank->at[1][2];
        put(q, 0, 40, 100); put(q, 4, 42, 100);
        q.events[1].condition = uint8_t(trig_not_fill);
        auto data = save_project(*project, *bank);
        auto back = std::make_unique<Project>();
        auto back_bank = std::make_unique<PatternBank>();
        CHECK(load_project(data, *back, *back_bank).empty() && back_bank->at[1][2] == q && back_bank->at[1][2].events[1].condition == trig_not_fill);
        std::string text(data.begin(), data.end());
        data[text.find("COND")] = 'X';
        CHECK(load_project(data, *back, *back_bank).empty() && back_bank->at[1][2].events[1].condition == trig_always);
    }
    // The TRIG row's K3 on Steps and Roll; a step rewritten keeps each note's condition.
    {
        auto project = std::make_unique<Project>();
        synth(project->tracks[0], 0);
        UiState ui;
        Pattern q;
        put(q, 1, 60, 100);
        q.events[0].condition = uint8_t(trig_of(3, 4));
        ui.page = PageId::Steps; ui.step_cursor = 1; ui.trig_cards = true;
        auto cards = page_bindings(*project, ui, q, false);
        CHECK(cards[2].kind == Binding::Kind::StepCondition && cards[2].value == int(trig_of(3, 4)) && cards[2].max == int(trig_count) - 1);
        ui.page = PageId::Roll; ui.roll_key = 60;
        CHECK(page_bindings(*project, ui, q, false)[2].kind == Binding::Kind::EventCondition);
        Step s = q.step(1);
        s.notes[0].key = 61;
        q.set_step(1, s);
        CHECK(q.events[0].key == 61 && q.events[0].condition == trig_of(3, 4));
    }
}

// Motion as each pattern's own at tick resolution, and locks (roadmap P1).
void pattern_motion() {
    using namespace midi_track;
    const double tick = 44100 * 60.0 / 120 / 96;
    // A MIDI track's CC lane shows Motion as it plays: a point on tick 5, a lock on tick 48 for a sixteenth over the
    // point's value (back to it after), and a lock on another CC lane with nothing recorded (back to the track's own).
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        load_sound(project->tracks[0], EngineKind::Midi, 0);
        auto& t = project->tracks[0];
        t.params[Port] = UsbC; t.params[Channel] = 1; t.params[CcNumber1] = 74; t.params[CcNumber2] = 71; t.params[Cc2] = 33;
        project->tempo = 1200;
        auto& p = bank->at[0][0];
        p.length = ticks_per_quarter;
        MotionPoint a; a.tick = 5; a.param = uint8_t(Cc1); a.value = 20;
        MotionPoint lock = point(2, Cc1, 100, 0, sixteenth_ticks);
        MotionPoint other = point(3, Cc2, 90, 0, 6);
        CHECK(p.write_motion(a) && p.write_motion(lock) && p.write_motion(other));
        CHECK(p.lock_at(48, Cc1, 0) && !p.lock_at(5, Cc1, 0) && p.lane_value(60, Cc1, 0, -1) == 20 && p.lane_value(4, Cc1, 0, -1) == -1);
        EngineHost host;
        host.load(*project, *bank);
        tones_for(host, *project);
        host.set_audio_format(44100, 384);
        host.send({Command::Type::Play});
        std::vector<int32_t> out(256);
        std::vector<MidiOutEvent> ccs;
        for (unsigned b = 0; b < 170; ++b) { // a beat (22050 frames)
            host.render(out.data(), 128, 1);
            MidiOutEvent e;
            while (host.take_midi(e)) if (e.kind == MidiOutEvent::Kind::Control) ccs.push_back(e);
        }
        auto at = [&](uint8_t cc, uint8_t value) {
            for (const auto& e : ccs) if (e.a == cc && e.b == value) return int64_t(e.frame);
            return int64_t(-1);
        };
        int64_t start = at(74, 20) - std::llround(5 * tick); // the point on tick 5 sets the run's frames
        CHECK(at(74, 20) > 0 && std::llabs(at(74, 100) - start - std::llround(48 * tick)) <= 2);
        CHECK(std::llabs(at(74, 20) - start - std::llround(5 * tick)) <= 2);
        unsigned back20 = 0;
        for (const auto& e : ccs) back20 += e.a == 74 && e.b == 20;
        CHECK(back20 == 2); // tick 5, then the lock's end at tick 72
        CHECK(at(71, 90) > 0 && at(71, 33) > at(71, 90) && std::llabs(at(71, 33) - at(71, 90) - std::llround(6 * tick)) <= 2);
    }
    // A pattern's edits carry its Motion: doubled with the notes, halved, at half and double speed.
    {
        Pattern p;
        p.length = 16 * sixteenth_ticks;
        p.write_motion(point(2, engine_lane, 10)); p.write_motion(point(12, engine_lane, 90, 0, 12));
        CHECK(double_pattern(p, 128 * 384) && p.motion.size() == 4 && p.motion[2].tick == 18 * sixteenth_ticks && p.motion[3].hold == 12);
        half_pattern(p);
        CHECK(p.motion.size() == 2);
        scale_pattern(p, true); // half speed: twice as far apart; past the end goes
        CHECK(p.motion.size() == 1 && p.motion[0].tick == 4 * sixteenth_ticks);
        p.write_motion(point(10, engine_lane, 50, 0, 8));
        scale_pattern(p, false);
        CHECK(p.motion.size() == 2 && p.motion[1].tick == 5 * sixteenth_ticks && p.motion[1].hold == 4);
    }
    // The file: a pattern's points in MOTK with their ticks and holds; Motion from before (MOT2, MOTP, MOTW on
    // sixteenths of the project's one list) lands in its patterns.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        MotionPoint fine; fine.tick = 7; fine.param = engine_lane; fine.value = 66;
        bank->at[3][2].write_motion(fine); bank->at[3][2].write_motion(point(4, motion_pad_param(PadParam::Level), 30, 9, 24));
        auto data = save_project(*project, *bank);
        auto back = std::make_unique<Project>();
        auto back_bank = std::make_unique<PatternBank>();
        CHECK(load_project(data, *back, *back_bank).empty() && back_bank->at[3][2] == bank->at[3][2] && back_bank->at[3][2].motion[1].hold == 24);
        std::vector<uint8_t> old(data.begin(), data.begin() + 8); // the magic, then sections as builds before P1 wrote them
        auto section = [&](const char* tag, const std::vector<uint8_t>& body) {
            old.insert(old.end(), tag, tag + 4);
            for (int k = 0; k < 4; ++k) old.push_back(uint8_t(body.size() >> (8 * k)));
            old.insert(old.end(), body.begin(), body.end());
        };
        section("MOT2", {1, 0, 2, 5, 6, 0, uint8_t(engine_lane + 1), 44, 0});       // track 3, pattern 6, sixteenth 6
        section("MOTP", {1, 0, 2, 5, 1, 0, 9, uint8_t(PadParam::Pitch), 3, 0});       // pad 10's tune on sixteenth 1
        section("MOTW", {1, 0, 2, 5, 3, 0, 100, 0});                                  // the mod wheel on sixteenth 3
        auto legacy = std::make_unique<PatternBank>();
        CHECK(load_project(old, *back, *legacy).empty());
        const auto& m = legacy->at[2][5].motion;
        CHECK(m.size() == 3 && m[0].tick == sixteenth_ticks && m[0].param == motion_pad_param(PadParam::Pitch) && m[0].pad == 9 && m[0].value == 3);
        CHECK(m[1].tick == 3 * sixteenth_ticks && m[1].param == motion_wheel && m[1].value == 100);
        CHECK(m[2].tick == 6 * sixteenth_ticks && m[2].param == engine_lane + 1 && m[2].value == 44 && !m[2].lock());
    }
    // The cards while a step is held on Steps (or a note's pad on Roll): the Motion page's lanes, each its lock there
    // or the track's own value; the lock holds as long as the step's longest note on the lane, a step at least.
    {
        auto project = std::make_unique<Project>();
        load_sound(project->tracks[0], EngineKind::Midi, 0); // a track whose values are Melodee's (a MIDI track's)
        const auto& info = engine_info(EngineKind::Midi);
        UiState ui;
        Pattern q;
        put(q, 3, 60, 100, 2);
        q.write_motion(point(3, unsigned(info.favourites[0]), 1, 0, 48));
        ui.page = PageId::Steps; ui.held_step = 3; ui.step_cursor = 3;
        auto cards = page_bindings(*project, ui, q, false);
        CHECK(cards[0].kind == Binding::Kind::Lock && cards[0].id == motion_lane(unsigned(info.favourites[0])) && cards[0].value == 1);
        CHECK(cards[1].kind == Binding::Kind::Lock && cards[1].id == motion_lane(unsigned(info.favourites[1])));
        CHECK(cards[1].value == project->tracks[0].params[size_t(info.favourites[1])]);
        uint32_t tick = 0, hold = 0;
        CHECK(lock_place(*project, ui, q, tick, hold) && tick == 3 * sixteenth_ticks && hold == 2 * sixteenth_ticks);
        ui.held_step = -1;
        CHECK(page_bindings(*project, ui, q, false)[0].kind == Binding::Kind::StepLength && !lock_place(*project, ui, q, tick, hold));
        ui.page = PageId::Roll; ui.roll_key = 60; ui.held_note = 4;
        CHECK(page_bindings(*project, ui, q, false)[0].kind == Binding::Kind::Lock && lock_place(*project, ui, q, tick, hold) && hold == 48);
    }
}

// Kit lanes of their own length (roadmap P1): polymeter inside a kit.
void kit_lanes() {
    // The model: a lane shorter than the pattern loops alone; as long, or longer, it has none of its own.
    Pattern p;
    p.length = 16 * sixteenth_ticks;
    uint8_t a = uint8_t(sampler_first_note), b = uint8_t(sampler_first_note + 1);
    p.set_lane_length(b, 3 * sixteenth_ticks);
    CHECK(p.own_lane(b) && !p.own_lane(a) && p.lane_length(b) == 3 * sixteenth_ticks && p.lane_length(a) == p.length);
    p.set_lane_length(b, p.length);
    CHECK(!p.own_lane(b) && p.lanes.empty());
    p.set_lane_length(b, 12 * sixteenth_ticks);
    half_pattern(p); // 8 steps: a 12-step lane is no longer shorter
    CHECK(p.lanes.empty());
    p.set_lane_length(b, 3 * sixteenth_ticks);
    scale_pattern(p, true); // half speed: the lane twice as long, as its notes are
    CHECK(p.lane_length(b) == 6 * sixteenth_ticks);
    // Played: A01 once a bar, A02 on a lane of three steps every three, its condition counting its own passes.
    {
        Pattern q;
        q.length = 16 * sixteenth_ticks;
        Event one;
        one.key = a; one.length = 6;
        q.add(one);
        Event two = one;
        two.key = b;
        q.add(two);
        Event three = one;
        three.key = uint8_t(sampler_first_note + 2); three.condition = uint8_t(trig_of(1, 2));
        q.add(three);
        q.set_lane_length(b, 3 * sixteenth_ticks);
        q.set_lane_length(three.key, 3 * sixteenth_ticks);
        auto notes = played_notes(q, 680); // one bar: 88200 frames
        std::map<int, std::vector<int64_t>> on;
        const double step = 44100 * 60.0 / 120 / 4;
        for (const auto& e : notes) on[e.a].push_back(std::llround((e.frame - notes[0].frame) / step));
        CHECK((on[a] == std::vector<int64_t> {0}));
        CHECK((on[b] == std::vector<int64_t> {0, 3, 6, 9, 12, 15}));
        CHECK((on[three.key] == std::vector<int64_t> {0, 6, 12})); // 1:2 of its own passes
    }
    // The file: LANE after the pattern's notes; without it every key loops with the pattern.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        auto& q = bank->at[3][1];
        put(q, 0, a, 100);
        q.set_lane_length(b, 5 * sixteenth_ticks);
        auto data = save_project(*project, *bank);
        auto back = std::make_unique<Project>();
        auto back_bank = std::make_unique<PatternBank>();
        CHECK(load_project(data, *back, *back_bank).empty() && back_bank->at[3][1] == q && back_bank->at[3][1].lane_length(b) == 5 * sixteenth_ticks);
        std::string text(data.begin(), data.end());
        data[text.find("LANE")] = 'X';
        CHECK(load_project(data, *back, *back_bank).empty() && back_bank->at[3][1].lanes.empty());
    }
    // Steps on a kit: Shift + K1 the last pad's lane's length in steps (the pattern's: All).
    {
        auto project = std::make_unique<Project>();
        load_sound(project->tracks[3], EngineKind::Sampler, 0);
        project->tracks[3].last_pad = 1;
        UiState ui;
        ui.track = 3; ui.page = PageId::Steps;
        Pattern q;
        q.set_lane_length(b, 6 * sixteenth_ticks);
        auto shifted = shift_bindings(*project, ui, q);
        CHECK(shifted[0].kind == Binding::Kind::LaneLength && shifted[0].id == b && shifted[0].value == 6 && shifted[0].max == 16);
        project->tracks[3].last_pad = 0;
        CHECK(shift_bindings(*project, ui, q)[0].value == 16);
        synth(project->tracks[0], 0);
        ui.track = 0;
        CHECK(!shift_bindings(*project, ui, q)[0].bound()); // a synth's Steps: the whole step, no lanes
    }
}

// Note Repeat (roadmap P1): 1/4 and 1/64, the pad's pressure as the repeats' velocity, and its notes kept by Seq Record.
void note_repeat_p1() {
    // B1 and B3 again step to 1/4 and 1/64 (with Shift their triplets) and back; B2 stays.
    CHECK(std::string(arp_rate_name(repeat_next(repeat_rate(0, false), 0, false))) == "1/4" && repeat_next(0, 0, false) == repeat_rate(0, false));
    CHECK(std::string(arp_rate_name(repeat_next(6, 2, false))) == "1/64" && std::string(arp_rate_name(repeat_next(7, 2, true))) == "1/64T");
    CHECK(repeat_next(4, 1, false) == 4 && repeat_next(2, 1, false) == 4 && repeat_next(8, 0, false) == 2); // another key: its first
    CHECK(repeat_shown(8, 2, false) == 8 && repeat_shown(4, 2, false) == 6 && arp_rate_ticks(8) == 6 && arp_rate_ticks(9) == 4);
    using namespace midi_track;
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    load_sound(project->tracks[0], EngineKind::Midi, 0);
    project->tracks[0].params[Port] = UsbC; project->tracks[0].params[Channel] = 1;
    project->tempo = 1200;
    EngineHost host;
    host.load(*project, *bank);
    host.set_audio_format(44100, 384);
    host.send({Command::Type::Arp, 0, 0, 2, 4}); // 1/16
    host.send({Command::Type::Arp, 0, 0, 0, 1});
    host.send({Command::Type::SongRecord, 0, 0, 0, 1});
    host.send({Command::Type::Play});
    host.send({Command::Type::ArpKey, 0, 0, 60, 100});
    std::vector<int32_t> out(256);
    std::vector<MidiOutEvent> ons;
    auto play = [&](unsigned blocks) {
        for (unsigned b = 0; b < blocks; ++b) {
            host.render(out.data(), 128, 1);
            MidiOutEvent e;
            while (host.take_midi(e)) if (e.kind == MidiOutEvent::Kind::NoteOn) ons.push_back(e);
        }
    };
    play(100);                                                // two sixteenths, as struck
    host.send({Command::Type::Pressure, 0, 0, 60, 64});       // pressed half way
    play(100);
    host.send({Command::Type::Arp, 0, 0, 5, 0});              // Full Level: the pressure is no velocity
    play(100);
    CHECK(ons.size() == 7 && ons[0].b == 100 && ons[1].b == 100); // a sixteenth is 5512.5 frames: two in each phase, three in the last
    bool half = false, full_again = false;
    for (size_t n = 1; n < ons.size(); ++n) {
        if (ons[n].b == 64) half = true;
        if (half && ons[n].b == 100) full_again = true;
    }
    CHECK(half && full_again);
    // Seq Record kept each repeat where it fell, a sixteenth apart, as long as the arp held it, as hard as it played.
    EngineHost::ArpWritten w;
    std::vector<EngineHost::ArpWritten> kept;
    while (host.take_arp_written(w)) kept.push_back(w);
    CHECK(kept.size() == ons.size());
    for (size_t n = 0; n < kept.size(); ++n) {
        CHECK(kept[n].track == 0 && kept[n].pattern == 0 && kept[n].key == 60 && kept[n].tick == n * sixteenth_ticks % 384);
        CHECK(kept[n].velocity == ons[n].b && kept[n].length == sixteenth_ticks * 60 / 100);
    }
}

// A pattern's feel (roadmap P1): strength toward Q, swing, humanize and a groove from another pattern, heard live.
void feel_p1() {
    auto note = [](Pattern& p, uint32_t tick, uint8_t key, uint8_t velocity) {
        Event e;
        e.tick = tick; e.key = key; e.velocity = velocity; e.length = 6;
        p.add(e);
    };
    Pattern p;
    note(p, 5, 60, 100); note(p, 30, 62, 100); note(p, 50, 64, 100);
    CHECK(feel_pattern(p) == p); // plain: as written
    p.feel.strength = 100;
    auto played = feel_pattern(p);
    CHECK(played.events[0].tick == 0 && played.events[1].tick == 24 && played.events[2].tick == 48);
    CHECK(p.events[0].tick == 5); // the notes as written stay
    p.feel.strength = 50;
    played = feel_pattern(p);
    CHECK(played.events[0].tick == 3 && played.events[1].tick == 27 && played.events[2].tick == 49);
    p.feel.strength = 100; p.feel.swing = 75; // every second sixteenth half a sixteenth late
    played = feel_pattern(p);
    CHECK(played.events[1].tick == 36 && played.events[2].tick == 48);
    // Humanize: each note a little early or late (at most a quarter of a step) and softer or harder (at most 20),
    // the same each time it is laid, until its amount changes.
    Pattern h;
    for (unsigned n = 0; n < 16; ++n) note(h, n * 24 + 12, uint8_t(40 + n), 100);
    h.feel.humanize = 100; h.feel.humanize_velocity = 100;
    auto a = feel_pattern(h), b = feel_pattern(h);
    CHECK(a == b);
    unsigned moved = 0, louder = 0, softer = 0;
    for (const auto& e : a.events) {
        int off = int(e.tick) - int((e.key - 40) * 24 + 12);
        CHECK(std::abs(off) <= 6 && std::abs(int(e.velocity) - 100) <= 20);
        moved += off != 0; louder += e.velocity > 100; softer += e.velocity < 100;
    }
    CHECK(moved >= 8 && louder >= 2 && softer >= 2);
    // A groove taken from a pattern: its second sixteenth four ticks late and soft, laid on another's notes.
    Pattern source;
    for (unsigned n = 0; n < 16; ++n) note(source, n * 24 + (n % 2 ? 4 : 0), 36, n % 2 ? 60 : 120);
    Pattern bass;
    note(bass, 0, 40, 100); note(bass, 24, 41, 100);
    groove_of(source, bass.feel);
    CHECK(bass.feel.groove_offsets.size() == 16 && bass.feel.groove_offsets[1] == 4 && bass.feel.groove_offsets[0] == 0);
    CHECK(bass.feel.groove_levels[0] == 133 && bass.feel.groove_levels[1] == 67);
    played = feel_pattern(bass);
    CHECK(played.events[0].tick == 0 && played.events[0].velocity == 127 && played.events[1].tick == 28 && played.events[1].velocity == 67);
    bass.feel.groove_amount = 50;
    played = feel_pattern(bass);
    CHECK(played.events[1].tick == 26 && played.events[1].velocity == 84);
    // Heard: the audio thread plays the feel (a note written on tick 5, at strength 100 on tick 0's frame).
    {
        Pattern q;
        note(q, 5, 60, 100); note(q, 2 * 24, 62, 100);
        q.feel.strength = 100;
        auto notes = played_notes(q, 120);
        const double tick = 44100 * 60.0 / 120 / 96;
        CHECK(notes.size() == 2 && std::llabs(int64_t(notes[1].frame - notes[0].frame) - std::llround(48 * tick)) <= 2);
    }
    // The cards Time Correct's page lays out.
    CHECK(feel_cards(bass, 0)[0].kind == Binding::Kind::FeelStrength && feel_cards(bass, 1)[1].kind == Binding::Kind::GrooveFrom &&
          feel_cards(bass, 1)[2].value == 50 && feel_cards(bass, 1)[1].max == int(track_count * pattern_count));
    // The file: FEEL with its groove; without it the notes play as written.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        bank->at[5][3] = bass;
        bank->at[5][3].feel.groove_from = 1 + 2 * pattern_count + 7; bank->at[5][3].feel.humanize = 30; bank->at[5][3].feel.swing = 66;
        auto data = save_project(*project, *bank);
        auto back = std::make_unique<Project>();
        auto back_bank = std::make_unique<PatternBank>();
        CHECK(load_project(data, *back, *back_bank).empty() && back_bank->at[5][3] == bank->at[5][3]);
        std::string text(data.begin(), data.end());
        data[text.find("FEEL")] = 'X';
        CHECK(load_project(data, *back, *back_bank).empty() && back_bank->at[5][3].feel.plain());
    }
}

// Generators (roadmap P1): Euclid on a kit lane, and a pattern's variations.
void generators() {
    auto text = [](const std::vector<bool>& r) { std::string s; for (bool b : r) s += b ? 'x' : '.'; return s; };
    CHECK(text(euclid(3, 8)) == "x..x..x." && text(euclid(5, 8)) == "x.xx.xx." && text(euclid(3, 4)) == "x.xx");
    CHECK(text(euclid(3, 8, 1)) == ".x..x..x" && text(euclid(0, 4)) == "...." && text(euclid(9, 4)) == "xxxx");
    CHECK(euclid(4, 200).size() == euclid_max_steps);
    // Laid on a kit lane: its notes as the rhythm's hits, a step of Q each, the lane looping on its own; the other
    // lanes left alone; the rhythm kept for the cards.
    Pattern p;
    uint8_t hat = uint8_t(sampler_first_note + 2), kick = uint8_t(sampler_first_note);
    put(p, 0, kick, 120);
    put(p, 5, hat, 90); // the hat's old note goes
    CHECK(lay_euclid(p, hat, 3, 8, 0));
    unsigned hats = 0;
    for (const auto& e : p.events) if (e.key == hat) { CHECK(e.tick % (3 * sixteenth_ticks) == 0 && e.tick < 8 * sixteenth_ticks); ++hats; }
    CHECK(hats == 3 && p.events.size() == 4 && p.lane_length(hat) == 8 * sixteenth_ticks && !p.own_lane(kick));
    const EuclidLane* e = euclid_of(p, hat);
    CHECK(e && e->hits == 3 && e->steps == 8 && e->rotate == 0 && !euclid_of(p, kick));
    CHECK(lay_euclid(p, hat, 16, 16, 3) && p.lanes.empty() && euclid_of(p, hat)->hits == 16 && euclid_of(p, hat)->rotate == 3);
    // The cards: Steps' EUCLID row on a kit.
    {
        auto project = std::make_unique<Project>();
        load_sound(project->tracks[3], EngineKind::Sampler, 0);
        project->tracks[3].last_pad = 2;
        UiState ui;
        ui.track = 3; ui.page = PageId::Steps; ui.euclid_cards = true;
        auto cards = page_bindings(*project, ui, p, false);
        CHECK(cards[0].kind == Binding::Kind::EuclidHits && cards[0].value == 16 && cards[1].kind == Binding::Kind::EuclidSteps && cards[2].value == 3);
        ui.sheet_open = true; ui.euclid_cards = false;
        CHECK(page_bindings(*project, ui, p, false)[0].kind == Binding::Kind::VaryAmount);
    }
    // Variations.
    VaryPlace place;
    place.root = 0; place.degree_count = scale_degrees(0, place.degrees);
    Pattern line;
    for (unsigned s : {0u, 3u, 4u, 7u, 8u, 12u}) put(line, s, uint8_t(60 + s), uint8_t(s == 0 ? 120 : 80));
    auto thin = vary_pattern(line, Variation::Thinner, 0.5, 1, place);
    CHECK(thin.events.size() == 3 && thin.events[0].tick == 0); // the weakest half go, the bar's first stays
    auto busy = vary_pattern(line, Variation::Busier, 1.0, 1, place);
    CHECK(busy.events.size() > line.events.size());
    for (const auto& n : busy.events) CHECK(n.tick < line.length);
    auto loose = vary_pattern(line, Variation::Humanised, 1.0, 7, place);
    CHECK(loose.events.size() == line.events.size() && loose != line);
    for (size_t n = 0; n < loose.events.size(); ++n) CHECK(std::abs(int(loose.events[n].tick) - int(line.events[n].tick)) <= 6);
    CHECK(vary_pattern(line, Variation::Humanised, 1.0, 7, place) == loose); // the same dice, the same pattern
    auto back = vary_pattern(line, Variation::Reversed, 1.0, 1, place);
    CHECK(back.events.size() == line.events.size() && back.events.front().tick == 3 * sixteenth_ticks && back.events.back().tick == 15 * sixteenth_ticks);
    CHECK(vary_pattern(back, Variation::Reversed, 1.0, 1, place).events == line.events); // twice is as it was
    auto up = vary_pattern(line, Variation::OctaveUp, 1.0, 1, place);
    CHECK(up.events[0].key == 72 && vary_pattern(line, Variation::OctaveDown, 1.0, 1, place).events[0].key == 48);
    Pattern kit;
    put(kit, 0, kick, 120); put(kit, 4, kick, 120); put(kit, 8, kick, 120);
    VaryPlace drums = place;
    drums.kit = true;
    CHECK(vary_pattern(kit, Variation::OctaveUp, 1.0, 1, drums) == kit); // a kit's keys are pads
    auto fills = vary_pattern(kit, Variation::Busier, 1.0, 3, drums);
    CHECK(fills.events.size() > kit.events.size());
    for (const auto& n : fills.events) CHECK(n.key == kick && (n.velocity == 120 || n.velocity == 60));
    // The file: EUCL keeps the rhythms laid.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        bank->at[3][4] = p;
        auto data = save_project(*project, *bank);
        auto copy = std::make_unique<Project>();
        auto copy_bank = std::make_unique<PatternBank>();
        CHECK(load_project(data, *copy, *copy_bank).empty() && copy_bank->at[3][4] == p);
    }
}

// A track's playback speed (roadmap P1): x1/2 .. x2, its notes as written.
void track_speed() {
    Pattern p;
    put(p, 0, 60, 100); put(p, 4, 62, 100, 2);
    p.write_motion(point(8, engine_lane, 20, 0, 24));
    p.set_lane_length(64, 6 * sixteenth_ticks);
    auto fast = speed_pattern(p, 4); // x2: every tick half as far
    CHECK(fast.length == 8 * sixteenth_ticks && fast.events[1].tick == 2 * sixteenth_ticks && fast.events[1].length == 2 * sixteenth_ticks / 2);
    CHECK(fast.motion[0].tick == 4 * sixteenth_ticks && fast.motion[0].hold == 12 && fast.lane_length(64) == 3 * sixteenth_ticks);
    auto slow = speed_pattern(p, 0); // x1/2
    CHECK(slow.length == 32 * sixteenth_ticks && slow.events[1].tick == 8 * sixteenth_ticks);
    auto three = speed_pattern(p, 3); // x3/2: sixteenths become sixteenth triplets
    CHECK(three.length == 256 && three.events[1].tick == 64);
    CHECK(speed_pattern(p, speed_normal) == p && played_pattern(p, speed_normal) == p);
    CHECK(std::string(speed_name(4)) == "\xc3\x97" "2" && speed_of(1).num == 3 && speed_of(1).den == 4);
    // Heard: a track at x2 plays its sixteenths as thirty-seconds; its place is still read in the pattern as written.
    {
        using namespace midi_track;
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        load_sound(project->tracks[0], EngineKind::Midi, 0);
        project->tracks[0].params[Port] = UsbC; project->tracks[0].params[Channel] = 1; project->tracks[0].speed = 4;
        project->tempo = 1200;
        put(bank->at[0][0], 0, 60, 100); put(bank->at[0][0], 4, 62, 100);
        EngineHost host;
        host.load(*project, *bank);
        tones_for(host, *project);
        host.set_audio_format(44100, 384);
        host.send({Command::Type::Play});
        std::vector<int32_t> out(256);
        std::vector<MidiOutEvent> ons;
        for (unsigned b = 0; b < 100; ++b) {
            host.render(out.data(), 128, 1);
            MidiOutEvent e;
            while (host.take_midi(e)) if (e.kind == MidiOutEvent::Kind::NoteOn) ons.push_back(e);
        }
        const double tick = 44100 * 60.0 / 120 / 96;
        CHECK(ons.size() >= 2 && ons[0].a == 60 && ons[1].a == 62 && std::llabs(int64_t(ons[1].frame - ons[0].frame) - std::llround(48 * tick)) <= 2);
        int at = host.position_of(0); // 12800 frames: 55 ticks played, 110 of the pattern as written
        CHECK(at >= 108 && at <= 112);
    }
    // The file: TSPD; without it every track at x1.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        project->tracks[6].speed = 1;
        auto data = save_project(*project, *bank);
        auto back = std::make_unique<Project>();
        auto back_bank = std::make_unique<PatternBank>();
        CHECK(load_project(data, *back, *back_bank).empty() && back->tracks[6].speed == 1 && back->tracks[5].speed == speed_normal);
        std::string text(data.begin(), data.end());
        data[text.find("TSPD")] = 'X';
        CHECK(load_project(data, *back, *back_bank).empty() && back->tracks[6].speed == speed_normal);
    }
    // Tracks' Shift + K1.
    {
        auto project = std::make_unique<Project>();
        synth(project->tracks[2], 2);
        project->tracks[2].speed = 3;
        UiState ui;
        ui.page = PageId::Tracks; ui.track = 2;
        Pattern none;
        auto cards = shift_bindings(*project, ui, none);
        CHECK(cards[0].kind == Binding::Kind::TrackSpeed && cards[0].value == 3 && cards[0].id == 2 && cards[0].max == int(speed_count) - 1);
    }
}

// Roadmap P3: up to four samples a pad, picked by velocity, in turn or at random; kept in LAYR and bank files;
// an MPC drum pad of two to four layers mapped onto them.
void pad_layers_p3() {
    std::array<SampleData, 3> data;
    for (unsigned n = 0; n < 3; ++n) {
        data[n].frames = 44100;
        data[n].left.assign(data[n].frames, 0.1f * (n + 1)); data[n].right.assign(data[n].frames, 0.1f * (n + 1));
    }
    SamplerEngine::Table table {};
    table[4] = &data[0]; table[5] = &data[1]; table[6] = &data[2];
    std::vector<float> l(128, 0), r(128, 0);
    auto block = [&](SamplerEngine& e) { std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f); e.render_stereo(l.data(), r.data(), 128); };
    // The slots the voices started by one hit play (the newest `count`), lowest first.
    auto hit = [&](SamplerEngine& e, int velocity) {
        e.note_on(int(sampler_first_note) + 2, velocity);
        block(e);
        TrackTaps t; t.track = 0; e.taps(t);
        std::vector<uint16_t> slots;
        uint32_t newest = t.count ? t.voice[0].serial : 0;
        for (unsigned n = 0; n < t.count; ++n)
            if (newest - t.voice[n].serial < 4 && !(t.voice[n].flags & tap::Choked)) slots.push_back(t.voice[n].slot);
        std::sort(slots.begin(), slots.end());
        e.all_notes_off(); e.silence();
        return slots;
    };
    PadSound pad;
    pad.sample = 4; pad.low_velocity = 1; pad.high_velocity = 63;
    pad.layers[0].sample = 5; pad.layers[0].low = 64; pad.layers[0].high = 127;
    pad.layers[1].sample = 6; pad.layers[1].low = 100; pad.layers[1].high = 127;
    CHECK(pad.layered() && pad.plays(6) && !pad.plays(7) && !pad.plain_layers());
    {
        SamplerEngine e;
        e.set_samples(&table);
        e.set_pad(2, pad);
        CHECK((hit(e, 40) == std::vector<uint16_t> {4}));
        CHECK((hit(e, 80) == std::vector<uint16_t> {5}));
        CHECK((hit(e, 110) == std::vector<uint16_t> {5, 6})); // two ranges reach it: both play
        PadSound mono = pad;
        mono.mono = true;
        e.set_pad(2, mono);
        CHECK((hit(e, 110) == std::vector<uint16_t> {5, 6})); // a mono pad's layers never cut each other
    }
    {
        SamplerEngine e;
        e.set_samples(&table);
        PadSound cycle = pad;
        cycle.layer_play = LayerPlay::Cycle;
        cycle.layers[0] = PadLayer {}; // a gap: Cycle goes round the layers that hold a sample
        cycle.layers[1].low = cycle.layers[1].high = 127;
        e.set_pad(2, cycle);
        CHECK((hit(e, 10) == std::vector<uint16_t> {4}) && (hit(e, 10) == std::vector<uint16_t> {6}) && (hit(e, 10) == std::vector<uint16_t> {4}));
        PadSound random = pad;
        random.layer_play = LayerPlay::Random;
        e.set_pad(2, random);
        std::array<unsigned, 3> seen {};
        for (unsigned n = 0; n < 60; ++n) {
            auto slots = hit(e, 64);
            CHECK(slots.size() == 1);
            if (slots.size() == 1) ++seen[slots[0] - 4u];
        }
        CHECK(seen[0] > 5 && seen[1] > 5 && seen[2] > 5);
    }
    {   // A layer's offsets go over the pad's; its region and loop point are its own.
        PadSound one = pad;
        one.pitch = 2; one.gain = 100; one.pan = 10; one.start = 500; one.loop_lock = false; one.loop_start = 900;
        one.layers[1].pitch = -3; one.layers[1].gain = -600; one.layers[1].pan = -20; one.layers[1].start = 50;
        PadSound as = layer_sound(one, 2);
        CHECK(as.sample == 6 && as.start == 50 && as.pitch == -1 && as.gain == -500 && as.pan == -10 && as.loop_point() == 50 && !as.layered());
        CHECK(layer_sound(one, 0).sample == 4 && layer_sound(one, 0).loop_point() == 900);
    }
    {   // The project file: LAYR; without it every pad plays its one sample.
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        load_sound(project->tracks[3], EngineKind::Sampler, 0);
        PadSound saved = pad;
        saved.layer_play = LayerPlay::Random;
        saved.layers[1].pitch = -7; saved.layers[1].fine = 12; saved.layers[1].gain = -350; saved.layers[1].pan = 9; saved.layers[1].end = 2000;
        project->tracks[3].pads[70] = saved;
        project->tracks[3].pads[1].sample = 4; // plain: no LAYR record
        auto file = save_project(*project, *bank);
        auto back = std::make_unique<Project>();
        auto back_bank = std::make_unique<PatternBank>();
        CHECK(load_project(file, *back, *back_bank).empty());
        const auto& b = back->tracks[3].pads[70];
        CHECK(b.layer_play == LayerPlay::Random && b.low_velocity == 1 && b.high_velocity == 63);
        CHECK(b.layers[0].sample == 5 && b.layers[0].low == 64 && b.layers[1].sample == 6 && b.layers[1].pitch == -7 && b.layers[1].fine == 12);
        CHECK(b.layers[1].gain == -350 && b.layers[1].pan == 9 && b.layers[1].end == 2000 && !b.layers[2].used());
        CHECK(back->tracks[3].pads[1].plain_layers());
        std::string text(file.begin(), file.end());
        file[text.find("LAYR")] = 'X';
        CHECK(load_project(file, *back, *back_bank).empty() && !back->tracks[3].pads[70].layered() && back->tracks[3].pads[70].sample == 4);
        // A bank file carries them: the layers' files after the pads' own.
        std::snprintf(project->samples[4].path, sizeof(project->samples[4].path), "a.wav");
        std::snprintf(project->samples[5].path, sizeof(project->samples[5].path), "b.wav");
        std::snprintf(project->samples[6].path, sizeof(project->samples[6].path), "c.wav");
        auto bank_file = bank_of(*project, 3, 4, "LAYERED"); // bank E: pads 64-79
        CHECK(bank_file.pads[6].sample == 0 && bank_file.pads[6].layers[0].sample == bank_pads && bank_file.pads[6].layers[1].sample == bank_pads + 1);
        BankFile loaded;
        CHECK(load_bank(save_bank(bank_file), loaded));
        CHECK(loaded.pads[6].layers[1].pitch == -7 && std::string(loaded.samples[bank_pads + 1].path) == "c.wav");
        auto into_held = std::make_unique<Track>();
        auto& into = *into_held;
        load_sound(into, EngineKind::Sampler, 0);
        std::map<std::string, int> given {{"a.wav", 10}, {"b.wav", 11}, {"c.wav", 12}};
        auto got = import_bank(loaded, 1, into, [&](const SampleRef& ref) { return given[ref.path]; });
        CHECK(got.pads == 1 && into.pads[22].sample == 10 && into.pads[22].layers[0].sample == 11 && into.pads[22].layers[1].sample == 12);
    }
    {   // The Layers page: K1 the layer, K2 / K3 its velocities under Velocity alone; Shift + K1 how a hit picks.
        auto held = std::make_unique<Project>();
        auto& project = *held;
        load_sound(project.tracks[1], EngineKind::Sampler, 0);
        project.tracks[1].pads[9] = pad;
        project.tracks[1].last_pad = 9;
        UiState ui;
        ui.track = 1; ui.page = PageId::LayersPage; ui.pad_layer = 2;
        Pattern none;
        auto cards = base_bindings(project, ui, none);
        CHECK(cards[0].kind == Binding::Kind::LayerPick && cards[0].value == 2 && cards[1].kind == Binding::Kind::LayerLow && cards[1].value == 100);
        auto shifted = shift_bindings(project, ui, none);
        CHECK(shifted[0].kind == Binding::Kind::LayerPlayMode && shifted[1].kind == Binding::Kind::LayerPitch && shifted[2].kind == Binding::Kind::LayerGain);
        project.tracks[1].pads[9].layer_play = LayerPlay::Cycle;
        CHECK(!base_bindings(project, ui, none)[1].bound());
        CHECK(sampler_next(PageId::Trim, 0) == PageId::LayersPage && sampler_next(PageId::LayersPage, 0) == PageId::PadMix);
        CHECK(stage_kit_page(StageKind::Layers) == PageId::LayersPage);
    }
    {   // An MPC drum pad of three velocity layers onto the pad's own layers.
        KeygroupInstrument snare;
        snare.number = 2;
        snare.tags = {{"ZonePlay", "1"}, {"TriggerMode", "0"}, {"Volume", "0.707946"}, {"Pan", "0.5"}, {"TuneCoarse", "1"}};
        const char* names[] {"soft", "mid", "hard"};
        const char* volumes[] {"0.5", "1.0", "1.0"};
        for (unsigned n = 0; n < 3; ++n)
            snare.layers.push_back({{"Active", "True"}, {"SampleName", names[n]}, {"VelStart", std::to_string(n * 43)}, {"VelEnd", std::to_string(n * 43 + 42)},
                                    {"Volume", volumes[n]}, {"Pan", n == 2 ? "0.7" : "0.5"}, {"TuneCoarse", n == 1 ? "2" : "0"}, {"SliceStart", "100"},
                                    {"SliceEnd", "0"}});
        PadSound mapped;
        keygroup_pad(snare, 1.0, mapped);
        CHECK(keygroup_pad_layers(snare, {20, 21, 22, 0}, {1.0, 2.0, 1.0, 1.0}, mapped));
        CHECK(mapped.sample == 20 && mapped.start == 100 && mapped.pitch == 1 && mapped.layer_play == LayerPlay::Velocity);
        CHECK(mapped.low_velocity == 1 && mapped.high_velocity == 42 && mapped.gain == int16_t(std::lround(20 * std::log10(0.5) * 100)));
        CHECK(mapped.layers[0].sample == 21 && mapped.layers[0].start == 200 && mapped.layers[0].pitch == 2 && mapped.layers[0].low == 43 && mapped.layers[0].high == 85);
        CHECK(mapped.layers[0].gain == -mapped.gain && mapped.layers[1].pan == 20 && !mapped.layers[2].used());
        snare.tags["ZonePlay"] = "0";
        PadSound cycled;
        CHECK(keygroup_pad_layers(snare, {1, 2, 3, 0}, {1.0, 1.0, 1.0, 1.0}, cycled) && cycled.layer_play == LayerPlay::Cycle);
        snare.tags["TriggerMode"] = "1";
        PadSound off;
        CHECK(!keygroup_pad_layers(snare, {1, 2, 3, 0}, {1.0, 1.0, 1.0, 1.0}, off) && !off.used());
        KeygroupInstrument five = snare;
        five.tags["TriggerMode"] = "0";
        for (unsigned n = 0; n < 2; ++n) five.layers.push_back(five.layers[0]);
        CHECK(!keygroup_pad_layers(five, {1, 2, 3, 4}, {1.0, 1.0, 1.0, 1.0}, off)); // five: streamed as an instrument still
    }
}

// Roadmap P3: Pad Link through more than one pad, round the bank once; a muted pad links on.
void pad_link_chains() {
    SampleData data;
    data.frames = 44100;
    data.left.assign(data.frames, 0.3f); data.right = data.left;
    SamplerEngine::Table table {};
    table[1] = &data;
    auto engine = std::make_unique<SamplerEngine>();
    engine->set_samples(&table);
    std::vector<float> l(128), r(128);
    auto pads_heard = [&](int hit) {
        engine->note_on(int(sampler_first_note) + hit, 100);
        std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
        engine->render_stereo(l.data(), r.data(), 128);
        TrackTaps t; t.track = 0; engine->taps(t);
        std::vector<int> pads;
        for (unsigned n = 0; n < t.count; ++n) pads.push_back(t.voice[n].pad);
        std::sort(pads.begin(), pads.end());
        engine->silence();
        return pads;
    };
    PadSound pad;
    pad.sample = 1;
    PadSound a = pad, b = pad, c = pad;
    a.link = 2; b.link = 3; c.link = 1; // A01 -> A02 -> A03 -> back to A01: each once
    engine->set_pad(0, a); engine->set_pad(1, b); engine->set_pad(2, c);
    CHECK((pads_heard(0) == std::vector<int> {0, 1, 2}));
    CHECK((pads_heard(1) == std::vector<int> {0, 1, 2}));
    engine->set_pad_mute(1, true); // A02 silent, A03 still reached through it
    CHECK((pads_heard(0) == std::vector<int> {0, 2}));
    engine->set_pad_mute(1, false);
    PadSound d = pad; d.link = 2; // B01 -> B02: a link stays in its own bank
    engine->set_pad(16, d); engine->set_pad(17, pad);
    CHECK((pads_heard(16) == std::vector<int> {16, 17}));
}


// Roadmap P3: a loop's seam crossfaded over a time (not a count of frames): with audio before the loop point, after
// the end, or (neither) a dip; never the jump an unfaded seam makes.
void loop_seams() {
    SampleData data;
    data.frames = 44100;
    data.left.resize(data.frames);
    for (uint32_t n = 0; n < data.frames; ++n) data.left[n] = float(n) / float(data.frames); // a ramp: every seam a jump
    data.right = data.left;
    SamplerEngine::Table table {};
    table[1] = &data;
    auto biggest_step = [&](PadSound pad) {
        auto engine = std::make_unique<SamplerEngine>();
        engine->set_samples(&table);
        engine->set_pad(0, pad);
        engine->note_on(int(sampler_first_note), 127);
        std::vector<float> l(128), r(128);
        float last = 0, most = 0;
        for (unsigned b = 0; b < 600; ++b) {
            std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
            engine->render_stereo(l.data(), r.data(), 128);
            for (unsigned f = 0; f < 128; ++f) {
                if (b * 128 + f > 2000) most = std::max(most, std::fabs(l[f] - last)); // past the attack
                last = l[f];
            }
        }
        return most;
    };
    PadSound pad;
    pad.sample = 1; pad.loop = true; pad.decay = 100; pad.velocity_sense = 0;
    // Before the loop point there is audio: the end fades into what precedes the loop point.
    PadSound before = pad;
    before.end = 20000; before.loop_lock = false; before.loop_start = 10000;
    const float jump = biggest_step(before);
    before.loop_xfade = 10;
    CHECK(jump > 0.1f && biggest_step(before) < jump / 100);
    // None before it (the loop from the start), but audio after the end: the turn fades out of what follows the end.
    PadSound after = pad;
    after.end = 20000;
    const float jump2 = biggest_step(after);
    after.loop_xfade = 10;
    CHECK(jump2 > 0.2f && biggest_step(after) < jump2 / 50);
    // The whole file: a dip to nothing and back.
    PadSound whole = pad;
    const float jump3 = biggest_step(whole);
    whole.loop_xfade = 20;
    CHECK(jump3 > 0.5f && biggest_step(whole) < jump3 / 50);
    // Reversed, before its loop point there is the end's far side: no jump either.
    PadSound back = before;
    back.reverse = true;
    back.loop_xfade = 0;
    const float jump4 = biggest_step(back);
    back.loop_xfade = 10;
    CHECK(jump4 > 0.1f && biggest_step(back) < jump4 / 50);
    // PLAY's Shift + K3, both rows; the file's PXFD (older files: none).
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    project->tracks[2].engine = EngineKind::Sampler;
    project->tracks[2].pads[5] = before;
    project->tracks[2].last_pad = 5;
    UiState ui;
    ui.track = 2; ui.page = PageId::PlayPage;
    Pattern none;
    auto shifted = shift_bindings(*project, ui, none);
    CHECK(shifted[1].kind == Binding::Kind::PadLink && shifted[2].kind == Binding::Kind::PadLoopXfade && shifted[2].value == 10);
    ui.sampler_row = 1;
    CHECK(!shift_bindings(*project, ui, none)[1].bound() && shift_bindings(*project, ui, none)[2].kind == Binding::Kind::PadLoopXfade);
    auto file = save_project(*project, *bank);
    auto loaded = std::make_unique<Project>();
    auto loaded_bank = std::make_unique<PatternBank>();
    CHECK(load_project(file, *loaded, *loaded_bank).empty() && loaded->tracks[2].pads[5].loop_xfade == 10);
    std::string text(file.begin(), file.end());
    file[text.find("PXFD")] = 'X';
    CHECK(load_project(file, *loaded, *loaded_bank).empty() && loaded->tracks[2].pads[5].loop_xfade == 0);
}


// Roadmap P3, instruments: release samples, crossfades over velocity, keyswitches.
void instrument_p3() {
    { // xfin / xfout: a straight gain across the range, the power curve its root
        SampleZone z;
        z.xfin_lovel = 0; z.xfin_hivel = 64; z.xfout_lovel = 64; z.xfout_hivel = 127; z.vel_power = false;
        CHECK(zone_crossfade(z, 60, 0) == 0.0f && std::fabs(zone_crossfade(z, 60, 32) - 0.5f) < 0.01f && zone_crossfade(z, 60, 64) == 1.0f);
        CHECK(std::fabs(zone_crossfade(z, 60, 127)) < 1e-6f);
        z.vel_power = true;
        CHECK(std::fabs(zone_crossfade(z, 60, 32) - std::sqrt(0.5f)) < 0.01f);
        SampleZone keys;
        keys.xfin_lokey = 48; keys.xfin_hikey = 60;
        CHECK(zone_crossfade(keys, 40, 100) == 0.0f && zone_crossfade(keys, 70, 100) == 1.0f);
    }
    { // a zone faded out at this velocity does not sound at all
        ZoneRig r;
        r.part(zone_tone(440, 0.5f, 44100));
        auto& z = r.zone(0); z.xfin_lovel = 50; z.xfin_hivel = 100;
        r.pad.velocity_sense = 0;
        r.build();
        r.key(0, 40);
        CHECK(zone_rms(r.run(4000), 500, 4000) < 1e-6);
        r.key(0, 110);
        CHECK(zone_rms(r.run(4000), 500, 4000) > 0.1);
    }
    { // a release sample plays when the key is let go, quieter the longer it was held
        auto played = [](unsigned held_blocks) {
            ZoneRig r;
            r.part(zone_tone(440, 0.5f, 44100)); r.part(zone_tone(880, 0.5f, 44100));
            r.zone(0);
            auto& tail = r.zone(1); tail.on_release = true; tail.rt_decay = 6;
            r.multi->has_release = true;
            r.pad.velocity_sense = 0; r.pad.release = 0;
            r.build(PadPlay::Gate);
            r.key(0, 100);
            auto pressed = r.run(4000);
            if (held_blocks) r.run(held_blocks * 128);
            r.off(0);
            auto after = r.run(8000);
            return std::make_tuple(zone_hz(pressed, 500, 4000), zone_hz(after, 3000, 8000), zone_rms(after, 3000, 8000));
        };
        auto [first, second, loud] = played(0);
        CHECK(std::fabs(first - 440) < 10 && std::fabs(second - 880) < 15); // the press plays its zone alone; the release the other
        auto [a, b, quiet] = played(345); // about a second more held: 6 dB down
        (void)a; (void)b;
        CHECK(quiet < loud * 0.62 && quiet > loud * 0.38);
    }
    { // keyswitches: the last one pressed picks the zones; it sounds nothing itself
        ZoneRig r;
        r.part(zone_tone(440, 0.5f, 44100)); r.part(zone_tone(880, 0.5f, 44100));
        auto& legato = r.zone(0); legato.switch_key = 24; legato.root = 60;
        auto& staccato = r.zone(1); staccato.switch_key = 25; staccato.root = 60; staccato.keytrack = 100;
        r.multi->switch_low = 24; r.multi->switch_high = 25; r.multi->switch_default = 24;
        r.build();
        r.key(0);
        CHECK(std::fabs(zone_hz(r.run(8000), 800, 8000) - 440) < 8);
        r.engine->silence();
        r.key(25 - 60);
        CHECK(zone_rms(r.run(2000), 200, 2000) < 1e-6); // the switch itself: silent
        r.key(0);
        CHECK(std::fabs(zone_hz(r.run(8000), 800, 8000) - 880) < 12);
    }
    { // SFZ: what the reader takes of them
        auto read = read_sfz("<global> sw_lokey=24 sw_hikey=25 sw_default=24\n<region> sample=note.wav key=60 sw_last=24\n<region> sample=other.wav key=60 sw_last=25\n"
                             "<region> sample=tail.wav key=60 trigger=release rt_decay=3 sw_last=24\n<region> sample=soft.wav key=61 xfin_lovel=0 xfin_hivel=40 xf_velcurve=gain\n"
                             "<region> sample=slur.wav key=61 trigger=legato\n");
        CHECK(read.regions.size() == 4 && read.left == 1 && read.left_by == std::set<std::string>({"trigger"}));
        CHECK(read.switch_low == 24 && read.switch_high == 25 && read.switch_default == 24);
        CHECK(read.regions[0].zone.switch_key == 24 && read.regions[1].zone.switch_key == 25 && !read.regions[0].zone.on_release);
        CHECK(read.regions[2].zone.on_release && read.regions[2].zone.rt_decay == 3.0f);
        CHECK(read.regions[3].zone.xfin_hivel == 40 && !read.regions[3].zone.vel_power && read.regions[3].zone.switch_key == -1);
        CHECK(read.ignored.empty());
    }
}

// A SoundFont made here: one preset, its instrument two sounds panned apart (a stereo pair), looped.
std::vector<uint8_t> tiny_sf2() {
    std::vector<uint8_t> out;
    auto u8 = [](std::vector<uint8_t>& v, unsigned x) { v.push_back(uint8_t(x)); };
    auto u16 = [&](std::vector<uint8_t>& v, unsigned x) { u8(v, x & 0xff); u8(v, (x >> 8) & 0xff); };
    auto u32 = [&](std::vector<uint8_t>& v, uint32_t x) { u16(v, x & 0xffff); u16(v, x >> 16); };
    auto name = [&](std::vector<uint8_t>& v, const char* text, size_t size) { for (size_t n = 0; n < size; ++n) u8(v, n < std::strlen(text) ? uint8_t(text[n]) : 0); };
    auto chunk = [&](const char* id, const std::vector<uint8_t>& body) {
        std::vector<uint8_t> c(id, id + 4);
        u32(c, uint32_t(body.size()));
        c.insert(c.end(), body.begin(), body.end());
        if (body.size() % 2) c.push_back(0);
        return c;
    };
    auto list = [&](const char* kind, std::vector<std::vector<uint8_t>> parts) {
        std::vector<uint8_t> body(kind, kind + 4);
        for (auto& p : parts) body.insert(body.end(), p.begin(), p.end());
        return chunk("LIST", body);
    };
    std::vector<uint8_t> ifil; u16(ifil, 2); u16(ifil, 1);
    std::vector<uint8_t> smpl;
    for (unsigned n = 0; n < 2000; ++n) u16(smpl, uint16_t(int16_t(n < 1000 ? 8000 : -8000)));
    std::vector<uint8_t> phdr, pbag, pmod(10, 0), pgen, inst, ibag, imod(10, 0), igen, shdr;
    name(phdr, "Tiny", 20); u16(phdr, 5); u16(phdr, 0); u16(phdr, 0); u32(phdr, 0); u32(phdr, 0); u32(phdr, 0);
    name(phdr, "EOP", 20); u16(phdr, 0); u16(phdr, 0); u16(phdr, 1); u32(phdr, 0); u32(phdr, 0); u32(phdr, 0);
    u16(pbag, 0); u16(pbag, 0); u16(pbag, 1); u16(pbag, 0);
    u16(pgen, 41); u16(pgen, 0); u16(pgen, 0); u16(pgen, 0);
    name(inst, "Pair", 20); u16(inst, 0);
    name(inst, "EOI", 20); u16(inst, 2);
    u16(ibag, 0); u16(ibag, 0); u16(ibag, 4); u16(ibag, 0); u16(ibag, 8); u16(ibag, 0);
    for (int side = 0; side < 2; ++side) {
        u16(igen, 43); u16(igen, 60 | 72 << 8);
        u16(igen, 17); u16(igen, uint16_t(int16_t(side ? 500 : -500)));
        u16(igen, 54); u16(igen, 1);
        u16(igen, 53); u16(igen, unsigned(side));
    }
    u16(igen, 0); u16(igen, 0);
    for (int side = 0; side < 2; ++side) {
        name(shdr, side ? "Tone R" : "Tone L", 20);
        u32(shdr, side * 1000); u32(shdr, side * 1000 + 1000); u32(shdr, side * 1000 + 100); u32(shdr, side * 1000 + 900); u32(shdr, 22050);
        u8(shdr, 64); u8(shdr, 0); u16(shdr, unsigned(1 - side)); u16(shdr, side ? 2 : 4);
    }
    name(shdr, "EOS", 20); for (int n = 0; n < 26; ++n) u8(shdr, 0);
    auto info = list("INFO", {chunk("ifil", ifil)});
    auto sdta = list("sdta", {chunk("smpl", smpl)});
    auto pdta = list("pdta", {chunk("phdr", phdr), chunk("pbag", pbag), chunk("pmod", pmod), chunk("pgen", pgen), chunk("inst", inst),
                              chunk("ibag", ibag), chunk("imod", imod), chunk("igen", igen), chunk("shdr", shdr)});
    std::vector<uint8_t> body {'s', 'f', 'b', 'k'};
    for (auto* part : {&info, &sdta, &pdta}) body.insert(body.end(), part->begin(), part->end());
    out = chunk("RIFF", body);
    return out;
}

// Roadmap P3: SF2 and DecentSampler instruments read as zones.
void instrument_formats_p3() {
    {
        auto bytes = tiny_sf2();
        Sf2 file;
        CHECK(file.open(bytes.data(), bytes.size()).empty());
        CHECK(file.presets().size() == 1 && file.presets()[0].program == 5 && file.presets()[0].name == "Tiny" && file.plays(file.presets()[0]));
        auto read = file.read(file.presets()[0]);
        CHECK(read.regions.size() == 1 && read.left == 0); // two sides, one stereo sound
        const auto& r = read.regions[0];
        CHECK(r.sample == "0+1" && r.zone.low_key == 60 && r.zone.high_key == 72 && r.zone.root == 64 && r.zone.pan == 0);
        CHECK(r.zone.loop == ZoneLoop::On && r.zone.loop_start == 100 && r.zone.loop_end == 900);
        std::vector<float> left, right;
        unsigned rate = 0;
        int64_t from = 0, to = 0;
        CHECK(file.sound(r.sample, left, right, rate, from, to));
        CHECK(left.size() == 1000 && right.size() == 1000 && rate == 22050 && from == 100 && to == 900);
        CHECK(left[10] > 0.2f && right[10] < -0.2f);
        CHECK(!file.sound("7", left, right, rate, from, to));
        std::string named;
        unsigned preset = 0;
        CHECK(split_sf2_preset("/media/card/Samples/Piano.SF2#12", named, preset) && named == "/media/card/Samples/Piano.SF2" && preset == 12);
        CHECK(!split_sf2_preset("/media/card/Samples/Kit.xpm#2", named, preset));
        SampleRef ref;
        std::snprintf(ref.path, sizeof(ref.path), "/media/card/Piano.sf2#3");
        CHECK(ref.instrument());
        std::snprintf(ref.path, sizeof(ref.path), "/media/card/Strings.dspreset");
        CHECK(ref.instrument());
        std::vector<uint8_t> broken(bytes.begin(), bytes.begin() + 40);
        Sf2 bad;
        CHECK(!bad.open(broken.data(), broken.size()).empty());
    }
    {
        DsElement root {"DecentSampler", {}, {}};
        DsElement groups {"groups", {{"attack", "0.01"}, {"release", "0.5"}}, {}};
        DsElement group {"group", {{"volume", "-6dB"}, {"seqMode", "round_robin"}}, {}};
        group.children.push_back({"sample", {{"path", "Samples/C3 a.wav"}, {"rootNote", "48"}, {"loNote", "46"}, {"hiNote", "50"}, {"seqPosition", "1"}}, {}});
        group.children.push_back({"sample", {{"path", "Samples/C3 b.wav"}, {"rootNote", "48"}, {"loNote", "46"}, {"hiNote", "50"}, {"seqPosition", "2"}}, {}});
        DsElement release_group {"group", {{"trigger", "release"}}, {}};
        release_group.children.push_back({"sample", {{"path", "Samples/C3 rel.wav"}, {"rootNote", "48"}, {"loopEnabled", "true"}, {"loopStart", "10"}, {"loopEnd", "99"}}, {}});
        DsElement off_group {"group", {{"enabled", "false"}}, {}};
        off_group.children.push_back({"sample", {{"path", "Samples/off.wav"}}, {}});
        groups.children = {group, release_group, off_group};
        root.children.push_back(groups);
        root.children.push_back({"effects", {}, {{"effect", {{"type", "reverb"}}, {}}}});
        auto read = read_dspreset(root);
        CHECK(read.regions.size() == 3 && read.left == 1 && read.left_by.count("enabled"));
        CHECK(read.regions[0].sample == "Samples/C3 a.wav" && read.regions[0].zone.root == 48 && read.regions[0].zone.low_key == 46);
        CHECK(read.regions[0].zone.sequence == 1 && read.regions[1].zone.sequence == 2 && read.regions[1].zone.sequence_length == 2);
        CHECK(std::fabs(read.regions[0].zone.gain + 6) < 0.01f && read.ignored.count("effects"));
        CHECK(read.regions[2].zone.on_release && read.regions[2].zone.loop == ZoneLoop::On && read.regions[2].zone.loop_end == 100);
        CHECK(read.envelope.release && std::fabs(*read.envelope.release - 0.5) < 1e-9);
        std::string why;
        read_dspreset(DsElement {"NotAPreset", {}, {}}, &why);
        CHECK(!why.empty());
        CHECK(dspreset_decibels("0.5") && std::fabs(*dspreset_decibels("0.5") + 6.02) < 0.01);
    }
}

// Roadmap P3: Chop's slices kept with the project (CHOP; older files: none, a damaged one dropped).
void chop_kept() {
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    std::snprintf(project->samples[7].path, sizeof(project->samples[7].path), "break.wav");
    auto& c = project->chops[7];
    c.from = 1000; c.to = 90000; c.mode = ChopMode::Manual; c.amount = 0;
    c.slices.count = 3; c.slices.start = {}; c.slices.start[0] = 1000; c.slices.start[1] = 20000; c.slices.start[2] = 51000; c.slices.last = 90000;
    project->chops[9] = c; // a chop of a slot holding nothing is not kept
    auto file = save_project(*project, *bank);
    auto back = std::make_unique<Project>();
    auto back_bank = std::make_unique<PatternBank>();
    CHECK(load_project(file, *back, *back_bank).empty());
    const auto& k = back->chops[7];
    CHECK(k.used() && k.from == 1000 && k.to == 90000 && k.mode == ChopMode::Manual && k.slices.count == 3 && k.slices.start[2] == 51000);
    CHECK(k.slices.last == 90000 && k.slices.end(2, 100000) == 90000 && !back->chops[9].used());
    std::string text(file.begin(), file.end());
    auto damaged = file;
    size_t at = text.find("CHOP") + 8 + 1 + 4 + 4 + 1 + 2 + 1 + 4; // to its first start
    damaged[at + 4] = 0xff; damaged[at + 5] = 0xff; damaged[at + 6] = 0xff; damaged[at + 7] = 0x7f; // the second start before the first
    CHECK(load_project(damaged, *back, *back_bank).empty() && !back->chops[7].used());
    file[text.find("CHOP")] = 'X';
    CHECK(load_project(file, *back, *back_bank).empty() && !back->chops[7].used());
}

// Roadmap P3: the slots in use as a save frees the rest: pads and their layers, and whatever an undo or redo holds.
void samples_used() {
    auto project = std::make_unique<Project>();
    load_sound(project->tracks[0], EngineKind::Sampler, 0);
    project->tracks[0].pads[3].sample = 2;
    project->tracks[0].pads[3].layers[1].sample = 5;
    History edits;
    Edit load;
    load.pad_before.sound.sample = 11; // a pad loaded over: undo brings 11 back
    load.pad_after.sound.sample = 2;
    edits.push(std::move(load));
    Edit chop;
    chop.chop_before = std::make_unique<ChopState>();
    chop.chop_before->sample = 20;
    edits.push(std::move(chop));
    auto used = samples_in_use(*project, edits);
    for (unsigned n = 0; n < sample_slots; ++n) CHECK(used[n] == (n == 2 || n == 5 || n == 11 || n == 20));
    edits.undo(); // a redo keeps them too
    CHECK(samples_in_use(*project, edits)[20]);
}

// Roadmap P3: a 16 Levels Filter note kept by Seq Record plays at its cutoff (LVCF), the pad's own filter otherwise.
void levels_cutoff_kept() {
    CHECK(pad_note(40, 0, 0) == (40 | 1 << 16) && pad_note(40, -2, 50) == (40 | 0xfe << 8 | 51 << 16) && pad_note(40, 3, -1) == pad_note(40, 3));
    SampleData data;
    data.frames = 44100;
    data.left.resize(data.frames);
    uint32_t seed = 1;
    for (auto& x : data.left) { seed = seed * 1664525u + 1013904223u; x = float(int32_t(seed)) / 2147483648.0f * 0.5f; } // noise: every band
    data.right = data.left;
    SamplerEngine::Table table {};
    table[1] = &data;
    auto heard = [&](int key) {
        auto engine = std::make_unique<SamplerEngine>();
        engine->set_samples(&table);
        PadSound pad; pad.sample = 1; pad.velocity_sense = 0;
        engine->set_pad(0, pad);
        engine->note_on(key, 100);
        std::vector<float> l(128), r(128);
        double sum = 0;
        for (unsigned b = 0; b < 60; ++b) {
            std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
            engine->render_stereo(l.data(), r.data(), 128);
            if (b > 10) for (float x : l) sum += double(x) * x;
        }
        engine->note_off(key & 0xffff); // its note off needs no cutoff
        return std::sqrt(sum);
    };
    const int key = int(sampler_first_note);
    double open = heard(key), closed = heard(pad_note(key, 0, 5)), wide = heard(pad_note(key, 0, 100));
    CHECK(closed < open * 0.3 && wide > open * 0.6);
    // The file: LVCF by each note's place; older files play every note at its pad's own.
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    auto& pattern = bank->at[4][2];
    Event a; a.tick = 0; a.key = uint8_t(key); a.cutoff = 31;
    Event b; b.tick = 96; b.key = uint8_t(key + 1);
    pattern.add(a); pattern.add(b);
    auto file = save_project(*project, *bank);
    auto back = std::make_unique<Project>();
    auto back_bank = std::make_unique<PatternBank>();
    CHECK(load_project(file, *back, *back_bank).empty());
    CHECK(back_bank->at[4][2].events.size() == 2 && back_bank->at[4][2].events[0].cutoff == 31 && back_bank->at[4][2].events[1].cutoff == 0);
    std::string text(file.begin(), file.end());
    file[text.find("LVCF")] = 'X';
    CHECK(load_project(file, *back, *back_bank).empty() && back_bank->at[4][2].events[0].cutoff == 0);
}

// Roadmap P4, plug-in delay compensation. A plug-in that is late by its latency: an instrument sounds a note that many
// frames after it came; an effect gives back what it was given that many frames later.
struct LatePlugin final : PluginRunner {
    bool instrument = true;
    unsigned late = 0;
    int countdown = -1;
    std::array<float, 8192> line_l {}, line_r {};
    unsigned at = 0;
    void midi(uint8_t status, uint8_t, uint8_t data2) override { if ((status & 0xf0) == 0x90 && data2) countdown = int(late); }
    void silence() override {}
    void render(float* left, float* right, unsigned frames) override {
        for (unsigned f = 0; f < frames; ++f) {
            if (instrument) {
                if (countdown == 0) { left[f] += 0.5f; right[f] += 0.5f; }
                if (countdown >= 0) --countdown;
            } else {
                line_l[at & 8191] = left[f]; line_r[at & 8191] = right[f];
                left[f] = line_l[(at - late) & 8191]; right[f] = line_r[(at - late) & 8191];
                ++at;
            }
        }
    }
    unsigned latency() const override { return late; }
};

void delay_compensation_p4() {
    // The rate bridge's own lag, measured once: an Open MPC plug-in at 44.1 kHz heard at 48 kHz. An instrument is
    // run ahead for what is asked, so it is not late; an effect is late by the margin its output starts with.
    {
        unsigned instrument = PluginRateBridge::measure(44100, 48000, true), effect = PluginRateBridge::measure(44100, 48000, false);
        CHECK(instrument <= 2 && effect >= 100 && effect < 260);
        CHECK(PluginRateBridge::measure(48000, 48000, true) == 0);
        // And what it says is where an impulse comes out.
        auto bridge = std::make_unique<PluginRateBridge>();
        bridge->reset(44100, 48000, false);
        CHECK(bridge->latency() == effect);
        float most = 0; unsigned where = 0, frame = 0;
        for (unsigned b = 0; b < 12; ++b) {
            std::array<float, 128> l {}, r {};
            if (b == 4) l[0] = r[0] = 1; // at frame 512
            bridge->render(l.data(), r.data(), 128, [](float*, float*, unsigned) {});
            for (unsigned f = 0; f < 128; ++f, ++frame) if (std::fabs(l[f]) > most) { most = std::fabs(l[f]); where = frame; }
        }
        CHECK(where == 512 + effect && most > 0.8f);
    }
    // A Limiter looking ahead: LOOKAHEAD ms late, never over its ceiling, and reports it.
    {
        DelayPool pool;
        Effect limiter;
        limiter.set_kind(EffectKind::Limiter, pool);
        CHECK(limiter.latency() == 0);
        limiter.set_param(0, -6);
        limiter.set_param(2, 5);
        unsigned late = unsigned(std::lround(5 * dsp::sample_rate() / 1000));
        CHECK(limiter.latency() == late);
        std::vector<float> l(4096, 0.0f), r(4096, 0.0f);
        for (unsigned f = 1000; f < 4096; ++f) l[f] = r[f] = 0.9f * float(std::sin(f * 0.05));
        unsigned first = 0;
        float most = 0;
        for (unsigned at = 0; at < 4096; at += 128) {
            limiter.process(l.data() + at, r.data() + at, 128);
            for (unsigned f = at; f < at + 128; ++f) { if (!first && std::fabs(l[f]) > 1e-6f) first = f; most = std::max(most, std::fabs(l[f])); }
        }
        CHECK(first >= 1000 + late && first <= 1000 + late + 2);
        CHECK(most <= std::pow(10.0f, -6 / 20.0f) + 1e-4f && most > 0.4f);
        limiter.set_param(2, 0); // back to none: no delay, its line given back
        CHECK(limiter.latency() == 0);
        limiter.set_bypass(true);
    }
    // Two PLUGIN tracks, one 300 frames late: both heard together, 300 frames on; a stem a track shows it.
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    for (unsigned t = 0; t < 2; ++t) { project->tracks[t].engine = EngineKind::Plugin; project->tracks[t].plugin = uint8_t(t); }
    auto host = std::make_unique<EngineHost>();
    host->load(*project, *bank);
    LatePlugin on_time, slow;
    slow.late = 300;
    CHECK(host->offer_plugin(0, &on_time) && host->offer_plugin(1, &slow));
    std::vector<int32_t> out(256);
    std::vector<float> stems(size_t(track_count) * EngineHost::block);
    uint64_t rendered = 0; // frames
    auto play = [&](unsigned blocks, std::vector<float>* mix, std::array<std::vector<float>, 2>* stem) {
        for (unsigned n = 0; n < blocks; ++n) {
            host->render(out.data(), 128, 1, nullptr, stems.data());
            rendered += 128;
            for (unsigned f = 0; f < 128; ++f) {
                if (mix) mix->push_back(out[2 * f] / 2147483648.0f);
                if (stem) for (unsigned t = 0; t < 2; ++t) (*stem)[t].push_back(stems[t * EngineHost::block + f]);
            }
        }
    };
    auto peak_at = [](const std::vector<float>& x) {
        unsigned best = 0; float most = 0;
        for (unsigned f = 0; f < x.size(); ++f) if (std::fabs(x[f]) > most) { most = std::fabs(x[f]); best = f; }
        return std::make_pair(best, most);
    };
    play(2, nullptr, nullptr);
    CHECK(host->compensation() == 300 && host->track_latency(1) == 300 && host->track_latency(0) == 0);
    play(4, nullptr, nullptr); // the lines settled
    host->send({Command::Type::NoteOn, 0, 0, 60, 100});
    host->send({Command::Type::NoteOn, 1, 0, 60, 100});
    std::array<std::vector<float>, 2> heard;
    play(6, nullptr, &heard);
    auto a = peak_at(heard[0]), b = peak_at(heard[1]);
    CHECK(a.second > 0.1f && b.second > 0.1f && a.first == 300 && b.first == 300);
    // A bus 200 frames late: the direct mix waits for it, so a track sent to both is heard once, not twice.
    host->send({Command::Type::NoteOff, 0, 0, 60, 0});
    host->send({Command::Type::TrackPlugin, 1, 0, 0, 255}); // the slow track lets go of its plug-in
    LatePlugin bus_late;
    bus_late.instrument = false; bus_late.late = 200;
    host->send({Command::Type::FxKind, uint8_t(bus_chain(0)), 0, 0, int16_t(EffectKind::Plugin)});
    host->send({Command::Type::FxParam, uint8_t(bus_chain(0)), 0, 0, 5});
    CHECK(host->offer_plugin(5, &bus_late));
    host->send({Command::Type::Send, 0, 0, 0, 100});
    host->send({Command::Type::Return, 0, 0, 0, 100});
    play(8, nullptr, nullptr);
    CHECK(host->compensation() == 200 && host->bus_latency(0) == 200);
    host->send({Command::Type::NoteOn, 0, 0, 60, 100});
    std::vector<float> mix;
    play(6, &mix, nullptr);
    auto m = peak_at(mix);
    unsigned loud = 0;
    for (float x : mix) loud += std::fabs(x) > m.second * 0.3f;
    CHECK(m.first == 200 && loud == 1);
    // The master's chain late by 50 more: MIDI out (a MIDI track's notes) goes out as late as what is heard.
    LatePlugin master_late;
    master_late.instrument = false; master_late.late = 50;
    host->send({Command::Type::FxKind, uint8_t(master_chain), 0, 0, int16_t(EffectKind::Plugin)});
    host->send({Command::Type::FxParam, uint8_t(master_chain), 0, 0, 6});
    CHECK(host->offer_plugin(6, &master_late));
    host->send({Command::Type::Engine, 2, 0, uint16_t(EngineKind::Midi)});
    host->send({Command::Type::Param, 2, 0, uint16_t(midi_track::Channel), 1});
    play(2, nullptr, nullptr);
    CHECK(host->compensation() == 250 && host->master_latency() == 50);
    MidiOutEvent e;
    while (host->take_midi(e)) {}
    host->send({Command::Type::NoteOn, 2, 0, 64, 100});
    uint64_t before = rendered; // the note plays at the next block's first frame
    play(1, nullptr, nullptr);
    bool found = false;
    while (host->take_midi(e)) if (e.kind == MidiOutEvent::Kind::NoteOn && e.a == 64) { found = true; CHECK(e.frame == before + 250); }
    CHECK(found);
    // Taken away, every line comes back to none.
    host->send({Command::Type::FxKind, uint8_t(master_chain), 0, 0, 0});
    host->send({Command::Type::FxKind, uint8_t(bus_chain(0)), 0, 0, 0});
    play(3, nullptr, nullptr);
    CHECK(host->compensation() == 0);
    CHECK(host->offer_plugin(0, nullptr) && host->offer_plugin(5, nullptr) && host->offer_plugin(6, nullptr) && host->offer_plugin(1, nullptr));
    play(1, nullptr, nullptr);
}

// Roadmap P4: a Compressor or Gate keyed by another track or bus. FakePlugin tracks: a steady tone while a note is held.
void keyed_dynamics_p4() {
    // The file: FXKY keeps each keyed slot's key; an older file's slots hear themselves.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        project->tracks[3].fx.slots[1] = EffectSlot {uint8_t(EffectKind::Compressor), false, {-30, 8, 4, 5, 30, 100}, fx_key_track(0)};
        project->buses[2].fx.slots[0] = EffectSlot {uint8_t(EffectKind::Gate), false, {-36, 20, 0, 0, 0, 0}, fx_key_bus(1)};
        auto data = save_project(*project, *bank);
        auto copy = std::make_unique<Project>();
        auto copy_bank = std::make_unique<PatternBank>();
        CHECK(load_project(data, *copy, *copy_bank).empty());
        CHECK(copy->tracks[3].fx.slots[1].key == fx_key_track(0) && copy->buses[2].fx.slots[0].key == fx_key_bus(1));
        CHECK(copy->tracks[3].fx.slots[0].key == 0 && fx_key_is_bus(fx_key_bus(3)) && !fx_key_is_track(fx_key_bus(0)));
        // A track move (shell/track_swap.h): keys follow their tracks, in every chain.
        project->tracks[5].fx.slots[0] = EffectSlot {uint8_t(EffectKind::Gate), false, {-36, 20, 0, 0, 0, 0}, fx_key_track(7)};
        project->buses[0].fx.slots[0] = EffectSlot {uint8_t(EffectKind::Compressor), false, {-30, 8, 4, 5, 30, 100}, fx_key_track(2)};
        auto changes = swap_tracks(*project, *bank, 2, 7);
        CHECK(changes.fx_keys && project->tracks[5].fx.slots[0].key == fx_key_track(2) && project->buses[0].fx.slots[0].key == fx_key_track(7));
        CHECK(project->buses[2].fx.slots[0].key == fx_key_bus(1) && project->tracks[3].fx.slots[1].key == fx_key_track(0));
    }
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    for (unsigned t = 0; t < 2; ++t) { project->tracks[t].engine = EngineKind::Plugin; project->tracks[t].plugin = uint8_t(t); }
    auto host = std::make_unique<EngineHost>();
    host->load(*project, *bank);
    FakePlugin tone, key;
    CHECK(host->offer_plugin(0, &tone) && host->offer_plugin(1, &key));
    std::vector<int32_t> out(256);
    std::vector<float> stems(size_t(track_count) * EngineHost::block);
    auto level = [&](unsigned track, unsigned blocks) {
        float most = 0;
        for (unsigned n = 0; n < blocks; ++n) {
            host->render(out.data(), 128, 1, nullptr, stems.data());
            for (unsigned f = 0; f < 128; ++f) most = std::max(most, std::fabs(stems[track * EngineHost::block + f]));
        }
        return most;
    };
    // A Gate on track 1's chain keyed by track 2: closed while the key is silent, open while it plays.
    host->send({Command::Type::FxKind, 0, 0, 0, int16_t(EffectKind::Gate)});
    host->send({Command::Type::FxParam, 0, 0, 0, -40});
    host->send({Command::Type::FxKey, 0, 0, 0, int16_t(fx_key_track(1))});
    host->send({Command::Type::NoteOn, 0, 0, 60, 100});
    level(0, 40);
    CHECK(level(0, 4) < 0.01f);
    host->send({Command::Type::NoteOn, 1, 0, 60, 100});
    level(0, 20);
    float open = level(0, 4);
    CHECK(open > 0.1f);
    host->send({Command::Type::NoteOff, 1, 0, 60, 0});
    level(0, 400); // the gate's release
    CHECK(level(0, 4) < 0.01f);
    // Unkeyed, the same gate hears the track itself: open.
    host->send({Command::Type::FxKey, 0, 0, 0, 0});
    level(0, 20);
    CHECK(level(0, 4) > open * 0.9f);
    // A Compressor keyed by track 2: the tone ducks while the key plays, and comes back after.
    host->send({Command::Type::FxKind, 0, 0, 0, int16_t(EffectKind::Compressor)});
    for (auto [q, v] : {std::pair<int, int>{0, -40}, {1, 20}, {2, 0}, {3, 0}, {4, 10}}) host->send({Command::Type::FxParam, 0, uint8_t(q), 0, int16_t(v)});
    host->send({Command::Type::FxKey, 0, 0, 0, int16_t(fx_key_track(1))});
    level(0, 20);
    float alone = level(0, 4);
    host->send({Command::Type::NoteOn, 1, 0, 60, 100});
    level(0, 40);
    float ducked = level(0, 4);
    CHECK(alone > 0.1f && ducked < alone * 0.3f);
    // Keyed by bus A (the block before): track 2 sends into it.
    host->send({Command::Type::FxKey, 0, 0, 0, int16_t(fx_key_bus(0))});
    host->send({Command::Type::Send, 1, 0, 0, 100});
    level(0, 40);
    CHECK(level(0, 4) < alone * 0.3f);
    host->send({Command::Type::Send, 1, 0, 0, 0});
    level(0, 400);
    CHECK(level(0, 4) > alone * 0.8f);
    CHECK(host->offer_plugin(0, nullptr) && host->offer_plugin(1, nullptr));
    level(0, 1);
}

// Roadmap P4: SYNC on the delays. An echo at a note value of the song's clock, following it when the tempo changes.
void delay_sync_p4() {
    CHECK(delay_sync_param(EffectKind::Echo) == 4 && delay_sync_param(EffectKind::DiffDelay) == 5 && delay_sync_param(EffectKind::Reverb) < 0);
    CHECK(delay_sync_quarters(0) == 0 && delay_sync_quarters(9) == 1 && std::string(delay_sync_name(7)) == "1/8D");
    double quarter = effect_clock.quarter;
    for (auto kind : {EffectKind::Echo, EffectKind::DiffDelay}) {
        DelayPool pool;
        Effect echo;
        echo.set_kind(kind, pool);
        echo.set_param(1, 0);   // no feedback: one echo
        echo.set_param(2, 100); // all wet
        if (kind == EffectKind::DiffDelay) echo.set_param(3, 0); // no smear: the echo where it lands
        echo.set_param(unsigned(delay_sync_param(kind)), 9); // 1/4
        effect_clock.quarter = 11025; // 240 BPM at 44.1 kHz frames
        auto echo_at = [&](unsigned length) {
            std::vector<float> l(length, 0.0f), r(length, 0.0f);
            l[0] = r[0] = 1;
            for (unsigned at = 0; at < length; at += 128) echo.process(l.data() + at, r.data() + at, std::min(128u, length - at));
            unsigned best = 0; float most = 0;
            for (unsigned f = 0; f < length; ++f) if (std::fabs(l[f]) > most) { most = std::fabs(l[f]); best = f; }
            return best;
        };
        unsigned first = echo_at(16384);
        CHECK(first >= 11024 && first <= 11026);
        effect_clock.quarter = 5512.5; // the tempo doubled: the echo glides to an eighth of a second
        echo_at(44100);
        echo.clear();
        while (echo.clearing()) { std::array<float, 128> l {}, r {}; echo.process(l.data(), r.data(), 128); } // its line cleared a slice a block
        unsigned second = echo_at(16384);
        CHECK(second >= 5511 && second <= 5514);
        echo.set_param(unsigned(delay_sync_param(kind)), 0); // Off: TIME again
        CHECK(echo_at(32768) != second);
    }
    effect_clock.quarter = quarter;
}

// Roadmap P4: Motion lanes on the mix. A track's level, pan and sends and the master's values (its chain, the returns)
// written as points of the track's pattern and played as other Motion; Stop puts them back where they were set.
void mix_motion_p4() {
    // The lanes as the pages name them.
    {
        auto project = std::make_unique<Project>();
        auto& track = project->tracks[0];
        track.mix.level = 90; track.mix.sends[1] = 40;
        project->master.slots[0] = EffectSlot {uint8_t(EffectKind::Width), false, {80, 0, 0, 0, 0, 0}};
        project->buses[2].level = 55;
        CHECK(lane_name(track, int(motion_level)) == "LEVEL" && motion_param(track, int(motion_level)).value == 90);
        CHECK(lane_name(track, int(motion_send(1))) == "SEND B" && motion_param(track, int(motion_send(1))).value == 40);
        CHECK(!motion_param(track, int(motion_return(2))).info); // the master's need the project
        CHECK(motion_param(track, int(motion_return(2)), project.get()).value == 55);
        CHECK(lane_name(track, int(motion_master_fx(0, 0)), project.get()) == "MASTER " + std::string(effect_info(EffectKind::Width).params[0].label));
        auto b = lane_binding(track, 0, int(motion_master_fx(0, 0)), project.get());
        CHECK(b.kind == Binding::Kind::FxParam && b.id == int(master_chain * 64) && b.value == 80);
        CHECK(lane_binding(track, 0, int(motion_send(3)), project.get()).kind == Binding::Kind::Send);
        CHECK(is_mix_lane(motion_level) && is_master_lane(motion_return(0)) && !is_mix_lane(motion_wheel) && pad_param_of(motion_level) < 0);
        // The file keeps them as any lane.
        auto bank = std::make_unique<PatternBank>();
        MotionPoint m; m.tick = 48; m.param = uint8_t(motion_return(1)); m.value = 30;
        bank->at[0][0].write_motion(m);
        auto data = save_project(*project, *bank);
        auto copy = std::make_unique<Project>();
        auto copy_bank = std::make_unique<PatternBank>();
        CHECK(load_project(data, *copy, *copy_bank).empty() && copy_bank->at[0][0].motion.size() == 1 && copy_bank->at[0][0].motion[0] == m);
    }
    // The host: a held tone whose level lane is 0 for the first half bar and 100 after.
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    project->tracks[0].engine = EngineKind::Plugin; project->tracks[0].plugin = 0;
    project->tracks[0].mix.sends[0] = 100;
    project->buses[0].level = 100;
    auto& pattern = bank->at[0][0];
    pattern.length = 4 * ticks_per_quarter;
    Event e; e.tick = 0; e.length = uint16_t(4 * ticks_per_quarter - 4); e.key = 60;
    pattern.add(e);
    for (auto [tick, value] : {std::pair<uint32_t, int>{0, 0}, {2 * ticks_per_quarter, 100}}) {
        MotionPoint m; m.tick = tick; m.param = uint8_t(motion_level); m.value = int16_t(value);
        pattern.write_motion(m);
    }
    { MotionPoint m; m.tick = 0; m.param = uint8_t(motion_return(0)); m.value = 0; pattern.write_motion(m); } // the return shut
    auto host = std::make_unique<EngineHost>();
    host->load(*project, *bank);
    FakePlugin tone;
    CHECK(host->offer_plugin(0, &tone));
    std::vector<int32_t> out(256);
    std::vector<float> stems(size_t(track_count) * EngineHost::block);
    float early = 0, late = 0, mix_late = 0;
    host->send({Command::Type::Play, 0, 0, 0, 0});
    for (unsigned n = 0; n < 400; ++n) {
        host->render(out.data(), 128, 1, nullptr, stems.data());
        int at = host->position_of(0);
        float stem = 0, mix = 0;
        for (unsigned f = 0; f < 128; ++f) { stem = std::max(stem, std::fabs(stems[f])); mix = std::max(mix, std::fabs(out[2 * f] / 2147483648.0f)); }
        if (at > 20 && at < 180) early = std::max(early, stem);
        if (at > 220 && at < 370) { late = std::max(late, stem); mix_late = std::max(mix_late, mix); }
    }
    CHECK(early < 0.01f && late > 0.1f);
    CHECK(mix_late < late * 1.2f); // the return held at 0: the bus not heard beside the track
    // Stop: the level and the return back to the track's own; the bus heard again.
    host->send({Command::Type::Stop});
    host->send({Command::Type::NoteOn, 0, 0, 60, 100});
    float stem = 0, mix = 0;
    for (unsigned n = 0; n < 20; ++n) {
        host->render(out.data(), 128, 1, nullptr, stems.data());
        for (unsigned f = 0; f < 128; ++f) { stem = std::max(stem, std::fabs(stems[f])); mix = std::max(mix, std::fabs(out[2 * f] / 2147483648.0f)); }
    }
    CHECK(stem > 0.1f && mix > mix_late * 1.2f); // the bus beside the track again (the mix soft clips)
    CHECK(host->offer_plugin(0, nullptr));
    host->render(out.data(), 128, 1);
}

// A project's load (every chain's effects set again) or a new kind never clears a 1.1 MB delay block on the audio
// thread at once (about 0.2 ms each on the MPC: twenty of them a dropout): a block never written since it was cleared
// is taken first, and one that was written is cleared a slice a block, the effect sounding as with an empty line.
void delay_pool_clears() {
    auto pool = std::make_unique<DelayPool>();
    Effect a, b;
    a.set_kind(EffectKind::Echo, *pool);
    CHECK(!a.clearing() && pool->free_blocks() == DelayPool::blocks - 1);
    a.set_param(2, 100); // all wet
    std::array<float, 128> l {}, r {};
    l[0] = r[0] = 1;
    a.process(l.data(), r.data(), 128);
    for (unsigned n = 0; n < 400; ++n) { l.fill(0); r.fill(0); a.process(l.data(), r.data(), 128); } // its line written
    a.set_kind(EffectKind::Echo, *pool); // the same block, cleared a slice at a time
    CHECK(a.clearing());
    unsigned calls = 0;
    float most = 0;
    while (a.clearing() && calls < 100) {
        l.fill(calls ? 0.0f : 0.5f); r.fill(calls ? 0.0f : 0.5f); // a sound while it clears: its dry share alone
        a.process(l.data(), r.data(), 128);
        ++calls;
        for (float x : l) most = std::max(most, std::fabs(x));
    }
    CHECK(calls <= (DelayPool::block_floats + 16383) / 16384 && most < 0.5f * 0.75f); // the dry share only (MIX 30 % after set_kind)
    for (unsigned n = 0; n < 2000; ++n) { // nothing of the old echoes comes back
        l.fill(0); r.fill(0);
        a.process(l.data(), r.data(), 128);
        for (float x : l) CHECK(std::fabs(x) < 1e-6f);
    }
    // Given back, a written block is taken last; when it is the only one left it is cleared before it sounds.
    a.set_kind(EffectKind::None, *pool);
    b.set_kind(EffectKind::Echo, *pool);
    CHECK(!b.clearing());
    std::vector<std::unique_ptr<Effect>> rest;
    while (pool->free_blocks() > 1) { rest.push_back(std::make_unique<Effect>()); rest.back()->set_kind(EffectKind::Reverb, *pool); CHECK(!rest.back()->clearing()); }
    Effect last;
    last.set_kind(EffectKind::Echo, *pool);
    CHECK(last.clearing() && pool->free_blocks() == 0);
    // A project's load sets each effect's values at once, worked out once: it sounds as set one at a time.
    auto twice = std::make_unique<DelayPool>();
    for (auto kind : {EffectKind::Compressor, EffectKind::Reverb, EffectKind::Limiter, EffectKind::Formant, EffectKind::DiffDelay}) {
        Effect one, all;
        const std::array<int16_t, effect_params> values {-20, 6, 3, 40, 60, 80};
        one.set_kind(kind, *twice);
        for (unsigned q = 0; q < effect_params; ++q) one.set_param(q, values[q]);
        all.set_kind(kind, *twice, false);
        all.set_params(values);
        for (unsigned n = 0; n < 20; ++n) {
            std::array<float, 128> l1 {}, r1 {}, l2 {}, r2 {};
            for (unsigned f = 0; f < 128; ++f) l1[f] = r1[f] = l2[f] = r2[f] = 0.5f * float(std::sin((n * 128 + f) * 0.031));
            one.process(l1.data(), r1.data(), 128); all.process(l2.data(), r2.data(), 128);
            CHECK(l1 == l2 && r1 == r2);
        }
        one.set_kind(EffectKind::None, *twice); all.set_kind(EffectKind::None, *twice);
    }
}

// Roadmap P4: modulators (an LFO, free or on the clock, an envelope follower) on any lane, and macros.
void modulators_p4() {
    // Shapes and the swing around a lane.
    CHECK(std::fabs(lfo_value(LfoShape::Sine, 0.25) - 1) < 1e-5f && std::fabs(lfo_value(LfoShape::Triangle, 0.75) + 1) < 1e-5f);
    CHECK(lfo_value(LfoShape::Square, 0.2) == 1 && lfo_value(LfoShape::SawUp, 0) == -1 && lfo_value(LfoShape::Hold, 0.3, 0.1f, -0.4f) == -0.4f);
    ModTarget g; g.track = 0; g.param = uint8_t(motion_level); g.low = 0; g.high = 127; g.depth = 50;
    CHECK(modulated(g, 100, 1, false) == 127 && modulated(g, 64, -1, false) == 32 && modulated(g, 64, 1, true) == 127);
    CHECK(Macro::at(MacroTarget {0, 0, 0, 20, 100}, 127) == 100 && Macro::at(MacroTarget {0, 0, 0, 100, 20}, 0) == 100);
    // The file: MODS and MACR.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        auto& m = project->modulators[2];
        m.kind = ModKind::Lfo; m.shape = LfoShape::Drift; m.sync = true; m.division = 6; m.rate = 33;
        m.targets[1] = ModTarget {4, uint8_t(motion_send(2)), 0, -40, 0, 100, 0};
        auto& f = project->modulators[5];
        f.kind = ModKind::Follower; f.source = 18; f.attack = 5; f.release = 70;
        f.targets[0] = ModTarget {1, uint8_t(motion_pad_param(PadParam::Pitch)), 9, 30, -48, 48, 0};
        auto& macro = project->macros[3];
        macro.value = 99;
        macro.targets[0] = MacroTarget {2, uint8_t(motion_level), 0, 40, 120};
        macro.targets[5] = MacroTarget {7, uint8_t(motion_master_fx(1, 2)), 0, 100, 0};
        auto data = save_project(*project, *bank);
        auto copy = std::make_unique<Project>();
        auto copy_bank = std::make_unique<PatternBank>();
        CHECK(load_project(data, *copy, *copy_bank).empty());
        CHECK(copy->modulators[2] == m && copy->modulators[5] == f && copy->modulators[0].kind == ModKind::Off);
        CHECK(copy->macros[3].value == 99 && copy->macros[3].targets[0] == macro.targets[0] && copy->macros[3].target_count() == 2);
        CHECK(lane_own(*project, 4, motion_send(2), 0) == 0 && lane_own(*project, 2, motion_level, 0) == 100);
    }
    // The host: a held tone, its level swung by an LFO, then ducked by a follower of another track.
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    for (unsigned t = 0; t < 2; ++t) { project->tracks[t].engine = EngineKind::Plugin; project->tracks[t].plugin = uint8_t(t); }
    auto host = std::make_unique<EngineHost>();
    host->load(*project, *bank);
    FakePlugin tone, key;
    CHECK(host->offer_plugin(0, &tone) && host->offer_plugin(1, &key));
    std::vector<int32_t> out(256);
    std::vector<float> stems(size_t(track_count) * EngineHost::block);
    auto levels = [&](unsigned blocks, float& low, float& high) {
        low = 1e9f; high = 0;
        for (unsigned n = 0; n < blocks; ++n) {
            host->render(out.data(), 128, 1, nullptr, stems.data());
            float most = 0;
            for (unsigned f = 0; f < 128; ++f) most = std::max(most, std::fabs(stems[f]));
            low = std::min(low, most); high = std::max(high, most);
        }
    };
    host->send({Command::Type::NoteOn, 0, 0, 60, 100});
    float low = 0, high = 0;
    levels(20, low, high);
    float steady = high;
    CHECK(steady > 0.1f && low > steady * 0.95f);
    Modulator lfo;
    lfo.kind = ModKind::Lfo; lfo.shape = LfoShape::Sine; lfo.rate = 70; // about 2.5 Hz
    lfo.targets[0] = ModTarget {0, uint8_t(motion_level), 0, 100, 0, 127, 100};
    host->send(pack_mod_target(0, 0, lfo.targets[0]));
    host->send(pack_modulator(0, lfo));
    levels(400, low, high); // over a second: the level swings
    CHECK(low < steady * 0.3f && high > steady * 0.9f && std::fabs(host->modulator_value(0)) <= 1);
    // Its lane's own value moves while it swings: the swing follows it.
    host->send({Command::Type::Level, 0, 0, 0, 60}); // around 60 now: up to 123 (half the range each way)
    levels(400, low, high);
    CHECK(high > steady * 1.3f && low < steady * 0.05f);
    // Off: the lane where it was set (60).
    lfo.kind = ModKind::Off;
    host->send(pack_modulator(0, lfo));
    levels(20, low, high); // eased there
    levels(10, low, high);
    CHECK(std::fabs(high - low) < 1e-3f && high < steady * 0.6f && high > steady * 0.2f);
    // A follower of track 2 pulls track 1's level down while it plays.
    host->send({Command::Type::Level, 0, 0, 0, 100});
    Modulator follower;
    follower.kind = ModKind::Follower; follower.source = 1; follower.attack = 0; follower.release = 20;
    follower.targets[0] = ModTarget {0, uint8_t(motion_level), 0, -100, 0, 127, 100};
    host->send(pack_modulator(1, follower));
    host->send(pack_mod_target(1, 0, follower.targets[0]));
    levels(20, low, high);
    CHECK(high > steady * 0.9f);
    host->send({Command::Type::NoteOn, 1, 0, 60, 100});
    levels(10, low, high); // the follower rises, the level eases down
    levels(20, low, high);
    CHECK(high < steady * 0.2f && host->modulator_value(1) > 0.4f);
    host->send({Command::Type::NoteOff, 1, 0, 60, 0});
    levels(200, low, high);
    CHECK(high > steady * 0.9f);
    CHECK(host->offer_plugin(0, nullptr) && host->offer_plugin(1, nullptr));
    host->render(out.data(), 128, 1);
}

// Roadmap P4, MIDI tracks M2-M5 (docs/MIDI_TRACKS.md): slots of every kind with CC thinning, bend, pressure, the mod
// wheel, each pattern's own program at its launch, the track's delay, Drum pads' map, USB devices by name, an input
// as the track's sound, and MIDT's version 3.
// A project file with some sections taken out (a file as an older build wrote it): magic, then tag, length, body.
std::vector<uint8_t> without_sections(const std::vector<uint8_t>& file, std::initializer_list<const char*> tags) {
    std::vector<uint8_t> out(file.begin(), file.begin() + 8);
    for (size_t at = 8; at + 8 <= file.size();) {
        uint32_t n = uint32_t(file[at + 4]) | uint32_t(file[at + 5]) << 8 | uint32_t(file[at + 6]) << 16 | uint32_t(file[at + 7]) << 24;
        bool drop = false;
        for (auto* t : tags) drop |= std::memcmp(&file[at], t, 4) == 0;
        if (!drop) out.insert(out.end(), file.begin() + long(at), file.begin() + long(at + 8 + n));
        at += 8 + n;
    }
    return out;
}

void midi_tracks_p4() {
    using namespace midi_track;
    using E = MidiOutEvent::Kind;
    CHECK(slot_kind(-1) == SlotKind::Off && slot_kind(74) == SlotKind::Cc && slot_kind(address_cc14 + 1) == SlotKind::Cc14 &&
          slot_number(address_cc14 + 1) == 1 && slot_kind(address_bend) == SlotKind::PitchBend && slot_kind(address_nrpn + 300) == SlotKind::Nrpn &&
          slot_number(address_nrpn + 300) == 300 && slot_label(74) == "CC 74" && slot_label(address_bend) == "BEND");
    CHECK(slot_value(23) < SlotAddress4 && slot_address(23) < PressureOut && Count <= int(max_params) && slot_of_address(slot_address(10)) == 10);
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    auto& tr = project->tracks[0];
    tr.engine = EngineKind::Midi;
    const auto& info = engine_info(EngineKind::Midi);
    for (unsigned p = 0; p < info.param_count; ++p) tr.params[p] = info.params[p].initial;
    tr.params[Channel] = 5;
    tr.params[slot_address(3)] = int16_t(address_cc14 + 2);
    tr.params[slot_address(4)] = int16_t(address_nrpn + 300);
    tr.params[slot_address(5)] = int16_t(address_pressure);
    bank->at[0][1].program = 21; // pattern 2's own program (Pattern::program)
    bank->at[0][70].program = 33; // and one in bank E
    tr.params[Program] = 5;
    // The file: MIDT 3 keeps the device's name, the drum map and the SysEx file.
    std::snprintf(tr.midi.device, sizeof(tr.midi.device), "%s", "Minilogue XD");
    tr.midi.drum_note[17] = 42;
    std::snprintf(tr.midi.sysex, sizeof(tr.midi.sysex), "%s", "bank-a.syx");
    {
        auto bytes = save_project(*project, *bank);
        auto back = std::make_unique<Project>();
        auto back_bank = std::make_unique<PatternBank>();
        CHECK(load_project(bytes, *back, *back_bank).empty());
        const auto& b = back->tracks[0];
        CHECK(b.engine == EngineKind::Midi && std::string(b.midi.device) == "Minilogue XD" && b.midi.drum_note[17] == 42 && b.midi.drum_note[0] == 36 &&
              std::string(b.midi.sysex) == "bank-a.syx" && b.params[slot_address(4)] == address_nrpn + 300 && b.params[PatternProgram0 + 1] == 0 &&
              back_bank->at[0][1].program == 21 && back_bank->at[0][70].program == 33 && back_bank->at[0][2].program == 0);
        // A file from before the banks: P1-P16's programs in TRAK's values and no PPRG; they move into the patterns.
        auto old = without_sections(bytes, {"PPRG"});
        auto back2 = std::make_unique<Project>();
        auto back2_bank = std::make_unique<PatternBank>();
        CHECK(old.size() < bytes.size() && load_project(old, *back2, *back2_bank).empty());
        CHECK(back2_bank->at[0][1].program == 21 && back2_bank->at[0][70].program == 0 && back2->tracks[0].params[PatternProgram0 + 1] == 0);
    }
    // Drum pads play the map; a USB device by name, offline when it is gone.
    {
        Track d = tr;
        use_drum_pads(d);
        d.bank = 1;
        CHECK(drum_key(d, 1) == 42 && drum_key(d, 0) == 36 + 16);
        UiState ui;
        std::snprintf(ui.usb_devices[0].data(), 32, "%s", "TR-8S");
        ui.usb_device_count = 1;
        d.params[Port] = UsbDevice;
        CHECK(midi_port_count(ui, d) == 5 && midi_port_index(ui, d) == 3); // External, USB-C, TR-8S, Minilogue XD (offline), All USB
        bool offline = false;
        CHECK(midi_port_text(ui, d, 3, &offline) == "Minilogue XD" && offline && midi_port_text(ui, d, 2) == "TR-8S" && midi_port_text(ui, d, 4) == "All USB");
    }
    auto host = std::make_unique<EngineHost>();
    host->load(*project, *bank);
    std::vector<int32_t> out(256);
    std::vector<MidiOutEvent> got;
    uint64_t rendered = 0;
    auto run = [&](unsigned blocks, const int32_t* input = nullptr, float* stems = nullptr) {
        for (unsigned n = 0; n < blocks; ++n) { host->render(out.data(), 128, 1, input, stems); rendered += 128; }
        MidiOutEvent e;
        while (host->take_midi(e)) got.push_back(e);
    };
    run(2);
    got.clear();
    // Slots: a 14-bit CC, NRPN, channel pressure; two turns within 2 ms: the second waits, then goes.
    host->send({Command::Type::Param, 0, 0, uint16_t(slot_value(3)), 100});
    host->send({Command::Type::Param, 0, 0, uint16_t(slot_value(4)), 127});
    host->send({Command::Type::Param, 0, 0, uint16_t(slot_value(5)), 40});
    host->send({Command::Type::Param, 0, 0, uint16_t(slot_value(0)), 10});
    host->send({Command::Type::Param, 0, 0, uint16_t(slot_value(0)), 11});
    run(1);
    auto find = [&](E kind) { for (const auto& e : got) if (e.kind == kind) return &e; return static_cast<const MidiOutEvent*>(nullptr); };
    const auto* cc14 = find(E::Cc14);
    const auto* nrpn = find(E::Nrpn);
    const auto* press = find(E::ChannelPressure);
    CHECK(cc14 && cc14->a == 2 && cc14->value == 100 * 129 && cc14->channel == 5);
    CHECK(nrpn && (nrpn->value >> 14) == 300 && (nrpn->value & 0x3fff) == 16383 && press && press->b == 40);
    std::vector<MidiOutEvent> controls; // the second turn waited its 2 ms, then went (here, inside the same block)
    for (const auto& e : got) if (e.kind == E::Control && e.a == 74) controls.push_back(e);
    CHECK(controls.size() == 2 && controls[0].b == 10 && controls[1].b == 11 &&
          controls[1].frame >= controls[0].frame + uint64_t(dsp::sample_rate() * 0.002));
    got.clear();
    run(2);
    // Bend, pressure (poly, then channel, then off) and the mod wheel.
    host->send({Command::Type::Param, 0, 0, uint16_t(Bend), 0});
    host->send({Command::Type::Pressure, 0, 0, 60, 90});
    host->send({Command::Type::Wheel, 0, 0, 0, 500});
    run(1);
    CHECK(find(E::PitchBend) && find(E::PitchBend)->value == 8192 && find(E::PolyPressure) && find(E::PolyPressure)->a == 60 && find(E::PolyPressure)->b == 90);
    CHECK(find(E::Control) && find(E::Control)->a == 1 && find(E::Control)->b == 64);
    got.clear();
    host->send({Command::Type::Param, 0, 0, uint16_t(PressureOut), PressureOff});
    host->send({Command::Type::Pressure, 0, 0, 60, 30});
    run(1);
    CHECK(!find(E::PolyPressure) && !find(E::ChannelPressure));
    // A pattern's own program at its launch; back to the track's.
    got.clear();
    host->send({Command::Type::Pattern, 0, 0, 0, 1});
    run(1);
    CHECK(find(E::Program) && find(E::Program)->a == 20);
    got.clear();
    host->send({Command::Type::Pattern, 0, 0, 0, 0});
    run(1);
    CHECK(find(E::Program) && find(E::Program)->a == 4);
    // DELAY: +10 ms later; -50 ms no earlier than the output's latency.
    got.clear();
    host->send({Command::Type::Param, 0, 0, uint16_t(Delay), 10});
    uint64_t at = rendered;
    host->send({Command::Type::NoteOn, 0, 0, 60, 100});
    run(1);
    CHECK(find(E::NoteOn) && find(E::NoteOn)->frame == at + uint64_t(std::lround(10 * dsp::sample_rate() / 1000.0)));
    got.clear();
    host->send({Command::Type::NoteOff, 0, 0, 60, 0});
    host->send({Command::Type::Param, 0, 0, uint16_t(Delay), -50});
    run(1);
    got.clear();
    at = rendered;
    host->send({Command::Type::NoteOn, 0, 0, 62, 100});
    run(1);
    CHECK(find(E::NoteOn) && find(E::NoteOn)->frame == at - host->audio_clock().latency_frames());
    // An input as the track's sound: the rear inputs into its stem, at its LEVEL.
    host->send({Command::Type::MidiAudio, 0, 0, 0, AudioRear});
    std::vector<int32_t> input(256, int32_t(0.25 * 2147483647.0));
    std::vector<float> stems(size_t(track_count) * EngineHost::block);
    run(4, input.data(), stems.data());
    float most = 0;
    for (unsigned f = 0; f < 128; ++f) most = std::max(most, std::fabs(stems[f]));
    CHECK(most > 0.15f);
    host->send({Command::Type::MidiAudio, 0, 0, 0, AudioNone});
    run(2, input.data(), stems.data());
    most = 0;
    for (unsigned f = 0; f < 128; ++f) most = std::max(most, std::fabs(stems[f]));
    CHECK(most < 1e-4f);
}

// Roadmap P4: exports and imports. Standard MIDI Files out and in, a mixdown in integer PCM dithered, stems beside it.
void exports_p4() {
    // A MIDI file written and read back: the notes, their channel and the tempo.
    {
        std::vector<SmfTrack> tracks(2);
        tracks[0].name = "BASS"; tracks[0].channel = 2;
        tracks[0].notes = {{0, 48, 36, 100}, {96, 24, 43, 90}, {96, 96, 48, 80}};
        tracks[1].name = "KIT"; tracks[1].channel = 10;
        tracks[1].notes = {{0, 12, 36, 127}, {48, 12, 38, 110}};
        auto bytes = write_smf(tracks, 1284, 3, 4);
        std::vector<SmfTrack> back;
        std::string why;
        unsigned tempo = 0;
        CHECK(read_smf(bytes, back, why, &tempo) && back.size() == 2 && tempo == 1284);
        CHECK(back[0].name == "BASS" && back[0].channel == 2 && back[0].notes.size() == 3 && back[0].notes[1].tick == 96 &&
              back[0].notes[1].key == 43 && back[0].notes[1].length == 24 && back[0].notes[2].length == 96 && back[0].notes[1].velocity == 90);
        CHECK(back[1].channel == 10 && back[1].notes[1].key == 38 && back[1].notes[1].tick == 48);
        // Another division, running status and a note on at velocity 0 as its off: 480 a quarter becomes 96.
        std::vector<uint8_t> other {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xe0,
                                    'M', 'T', 'r', 'k', 0, 0, 0, 16,
                                    0x00, 0x91, 60, 100, 0x83, 0x60, 60, 0, 0x00, 62, 90, 0x81, 0x70, 62, 0, 0x00};
        // (the last byte is padding the length covers)
        CHECK(read_smf(other, back, why) && back.size() == 1 && back[0].channel == 2 && back[0].notes.size() == 2);
        CHECK(back[0].notes[0].tick == 0 && back[0].notes[0].length == 96 && back[0].notes[1].tick == 96 && back[0].notes[1].length == 48);
        CHECK(!read_smf({1, 2, 3}, back, why) && why == "not a MIDI file");
    }
    // A project's tracks as a MIDI file (no song: the patterns playing), and one laid on a kit's pattern.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        project->tracks[0].engine = EngineKind::Sampler; project->tracks[0].set_name("DRUMS");
        project->tracks[3].engine = EngineKind::Plugin; project->tracks[3].midi_channel = 5;
        Event kick; kick.tick = 0; kick.key = uint8_t(sampler_first_note); kick.length = 12;
        Event snare = kick; snare.tick = 96; snare.key = uint8_t(sampler_first_note + 2);
        bank->at[0][0].add(kick); bank->at[0][0].add(snare);
        Event note = kick; note.key = 64; note.length = 192;
        bank->at[3][0].add(note);
        auto tracks = smf_tracks(*project, *bank);
        CHECK(tracks.size() == 2 && tracks[0].channel == 10 && tracks[0].name == "DRUMS" && tracks[0].notes[1].key == 38 && tracks[1].channel == 5);
        Pattern laid;
        CHECK(smf_pattern(tracks, project->tracks[0], project->bar_ticks(), laid));
        CHECK(laid.length == project->bar_ticks() && laid.events.size() == 3 && laid.events[0].key == sampler_first_note); // the 64 too: pad 29
        Track empty_kit; empty_kit.engine = EngineKind::Sampler;
        std::vector<SmfTrack> high(1);
        high[0].notes = {{0, 10, 20, 100}};
        CHECK(!smf_pattern(high, empty_kit, 384, laid)); // below the kit's first pad
    }
    // A mixdown in 16-bit PCM: dithered (a step either way at most around silence), the format says 16 bits.
    {
        auto dir = std::filesystem::temp_directory_path() / "melodee-p4-dither.wav";
        WavStream wav;
        CHECK(wav.open(dir.string(), 48000, 16));
        std::vector<float> frames(2 * 4800, 0.0f);
        CHECK(wav.add(frames.data(), 4800) && wav.close() && wav.frames() == 4800);
        std::ifstream in(dir, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(bytes.size() == 44 + 4800 * 4 && bytes[20] == 1 && bytes[34] == 16 && bytes[24] == uint8_t(48000 & 0xff));
        int lowest = 0, highest = 0;
        unsigned nonzero = 0;
        for (size_t at = 44; at + 1 < bytes.size(); at += 2) {
            int v = int16_t(uint16_t(bytes[at] | bytes[at + 1] << 8));
            lowest = std::min(lowest, v); highest = std::max(highest, v); nonzero += v != 0;
        }
        CHECK(lowest >= -1 && highest <= 1 && nonzero > 1000); // TPDF: noise of a step, never more
        std::filesystem::remove(dir);
    }
    // Stems beside the mixdown: each track as it joins the mix, the same frames.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        project->tracks[0].engine = EngineKind::Plugin; project->tracks[0].plugin = 0;
        auto& p = bank->at[0][0];
        p.length = 4 * ticks_per_quarter;
        auto host = std::make_unique<EngineHost>();
        host->load(*project, *bank);
        FakePlugin tone;
        CHECK(host->offer_plugin(0, &tone));
        auto ring = std::make_unique<StemRing>();
        ring->arm(1u << 0 | 1u << 5);
        host->set_stems(ring.get());
        Command arm {Command::Type::BounceArm, 0, 0, 0, 0};
        arm.position = ticks_per_quarter / 2; // an eighth: a quarter of a second at 120
        host->send(arm);
        host->send({Command::Type::Play, 0});
        host->send({Command::Type::NoteOn, 0, 0, 60, 100});
        std::vector<int32_t> out(256);
        for (unsigned n = 0; n < 200; ++n) host->render(out.data(), 128, 1);
        CHECK(host->bounce().state() == Bounce::State::Done && host->bounce().taken() > 0);
        std::vector<float> got(size_t(StemRing::stem_count) * 2 * 65536);
        unsigned frames = ring->pop(got.data(), 65536);
        CHECK(frames == host->bounce().taken() && !ring->failed());
        float loud = 0, quiet = 0;
        for (unsigned f = 0; f < frames; ++f) { loud = std::max(loud, std::fabs(got[2 * f])); quiet = std::max(quiet, std::fabs(got[size_t(5) * 2 * 65536 + 2 * f])); }
        CHECK(loud > 0.1f && quiet == 0);
        host->set_stems(nullptr);
        CHECK(host->offer_plugin(0, nullptr));
        host->render(out.data(), 128, 1);
    }
}

// Roadmap P5: scenes (a column's name and tempo) and follow actions (next, previous, any, again, stop; after bars or
// plays; two actions on odds), brought by the audio thread on their tick; a chained track's wait.
// Banks of patterns (freeze plan F1): 128 a track, A-H of sixteen, a file older builds still read.
void banks_f1() {
    CHECK(pattern_count == 128 && pattern_pads == 16 && pattern_banks == 8);
    UiState ui;
    ui.pattern_bank = 2;
    CHECK(ui.pad_pattern(5) == 37 && ui.pad_pattern(21) == 37);
    // A groove's source: as before the banks where that reaches it, GRVB where it does not.
    CHECK(groove_track(groove_source(2, 7)) == 2 && groove_pattern(groove_source(2, 7)) == 7);
    CHECK(legacy_groove(groove_source(2, 7)) == 1 + 2 * 16 + 7 && groove_of_legacy(1 + 2 * 16 + 7) == groove_source(2, 7));
    CHECK(legacy_groove(groove_source(2, 20)) == 0 && legacy_groove(0) == 0 && groove_of_legacy(0) == 0);
    // Follow actions go round across the banks.
    FollowDice dice;
    PatternSet filled;
    filled.set(3); filled.set(100);
    CHECK(follow_target(unsigned(FollowAction::Next), 3, filled, dice) == 100 && follow_target(unsigned(FollowAction::Next), 100, filled, dice) == 3);
    CHECK(follow_target(unsigned(FollowAction::Previous), 3, filled, dice) == 100 && follow_target(unsigned(FollowAction::Any), 3, filled, dice) == 100);
    // The file: patterns past P16, their tempos, a groove from one; TIME still holds sixteen tempos.
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    project->tracks[3].engine = EngineKind::Sampler;
    project->seq_tempo = true; project->pattern_tempo[2] = 905; project->pattern_tempo[90] = 1305;
    project->tracks[3].pattern = 100;
    Event e; e.tick = 48; e.key = 40;
    bank->at[3][100].add(e);
    bank->at[3][100].length = 64 * sixteenth_ticks;
    bank->at[3][5].feel.groove_from = groove_source(1, 3); bank->at[3][5].feel.groove_offsets = {2}; bank->at[3][5].feel.groove_levels = {110};
    bank->at[3][6].feel.groove_from = groove_source(3, 100); bank->at[3][6].feel.groove_offsets = {1}; bank->at[3][6].feel.groove_levels = {90};
    project->scenes[77].tempo = 1400;
    auto bytes = save_project(*project, *bank);
    auto back = std::make_unique<Project>();
    auto back_bank = std::make_unique<PatternBank>();
    CHECK(load_project(bytes, *back, *back_bank).empty());
    CHECK(back->tracks[3].pattern == 100 && back_bank->at[3][100].events.size() == 1 && back_bank->at[3][100].length == 64 * sixteenth_ticks);
    CHECK(back->pattern_tempo[2] == 905 && back->pattern_tempo[90] == 1305 && back->scenes[77].tempo == 1400);
    CHECK(back_bank->at[3][5].feel.groove_from == groove_source(1, 3) && back_bank->at[3][6].feel.groove_from == groove_source(3, 100));
    CHECK(back->tempo_of(90) == 1305 && back->tempo_of(91) == back->tempo);
    // As an older build saved it (no TIM2, no GRVB): P1-P16's tempos from TIME, a groove from past P16 gone.
    auto older = without_sections(bytes, {"TIM2", "GRVB"});
    CHECK(load_project(older, *back, *back_bank).empty());
    CHECK(back->pattern_tempo[2] == 905 && back->pattern_tempo[90] == 0 && back_bank->at[3][5].feel.groove_from == groove_source(1, 3) &&
          back_bank->at[3][6].feel.groove_from == 0);
}

void scenes_p5() {
    using FA = FollowAction;
    FollowDice dice;
    uint32_t filled = 0b1011; // patterns 1, 2 and 4
    CHECK(follow_target(unsigned(FA::Next), 1, filled, dice) == 3 && follow_target(unsigned(FA::Next), 3, filled, dice) == 0);
    CHECK(follow_target(unsigned(FA::Previous), 0, filled, dice) == 3 && follow_target(unsigned(FA::Previous), 3, filled, dice) == 1);
    CHECK(follow_target(unsigned(FA::Again), 1, filled, dice) == 1 && follow_target(unsigned(FA::Stop), 1, filled, dice) == -1);
    CHECK(follow_target(unsigned(FA::Next), 1, 0, dice) == -1 && follow_target(unsigned(FA::Any), 2, 0b100, dice) == 2);
    unsigned seen[16] {};
    for (unsigned n = 0; n < 300; ++n) ++seen[follow_target(unsigned(FA::Any), 1, filled, dice)];
    CHECK(!seen[1] && seen[0] > 60 && seen[3] > 60 && seen[0] + seen[3] == 300);
    PatternFollow f;
    unsigned action = 0, stretches = 0;
    CHECK(!f.on() && !follow_plan(f, dice, action, stretches));
    f.action = uint8_t(FA::Next);
    CHECK(follow_plan(f, dice, action, stretches) && action == unsigned(FA::Next) && stretches == 1);
    f.chance = 0; CHECK(!follow_plan(f, dice, action, stretches)); // no odds, no second: never
    f.chance = 25; // the first on a quarter of rolls, else the pattern plays on another stretch
    unsigned later = 0;
    for (unsigned n = 0; n < 400; ++n) { CHECK(follow_plan(f, dice, action, stretches) && action == unsigned(FA::Next)); later += stretches > 1; }
    CHECK(later > 220 && later < 380);
    f.other = uint8_t(FA::Previous); f.chance = 75; // two actions on their odds
    unsigned firsts = 0;
    for (unsigned n = 0; n < 1000; ++n) { follow_plan(f, dice, action, stretches); CHECK(stretches == 1); firsts += action == unsigned(FA::Next); }
    CHECK(firsts > 680 && firsts < 820);
    f.count = 4; CHECK(follow_after(f, 200, 384) == 4 * 384);
    f.plays = true; f.count = 3; CHECK(follow_after(f, 200, 384) == 600);
    CHECK(follow_after_card(f) == 64 + 2);
    PatternFollow g; follow_after_of_card(g, 5); CHECK(!g.plays && g.count == 6);
    follow_after_of_card(g, 127); CHECK(g.plays && g.count == 64);
    CHECK(std::string(follow_name(unsigned(FA::Any))) == "ANY");
    // The file: FOLW a pattern, SCNE a scene; without them none.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        put(bank->at[3][5], 0, 60, 100);
        bank->at[3][5].follow = f;
        project->scenes[2].set_name("CHORUS"); project->scenes[2].tempo = 1285; project->scenes[9].tempo = 900;
        auto data = save_project(*project, *bank);
        auto back = std::make_unique<Project>();
        auto back_bank = std::make_unique<PatternBank>();
        CHECK(load_project(data, *back, *back_bank).empty() && back_bank->at[3][5].follow == f && !back_bank->at[3][4].follow.on());
        CHECK(std::string(back->scenes[2].name) == "CHORUS" && back->scenes[2].tempo == 1285 && back->scenes[9].tempo == 900 && back->scenes[0].plain());
        std::string text(data.begin(), data.end());
        data[text.find("FOLW")] = 'X'; data[text.find("SCNE")] = 'X';
        CHECK(load_project(data, *back, *back_bank).empty() && !back_bank->at[3][5].follow.on() && back->scenes[2].plain());
    }
    // SCENES' cards: the selected track's pattern's follow action; Shift + K1 the second.
    {
        auto project = std::make_unique<Project>();
        synth(project->tracks[1], 2);
        UiState ui;
        ui.page = PageId::Scenes; ui.track = 1;
        Pattern p;
        p.follow = f;
        auto cards = base_bindings(*project, ui, p);
        CHECK(cards[0].kind == Binding::Kind::FollowAction && cards[0].value == int(FA::Next) && cards[1].kind == Binding::Kind::FollowAfter &&
              cards[1].value == 66 && cards[2].kind == Binding::Kind::FollowChance && cards[2].value == 75);
        auto shifted = shift_bindings(*project, ui, p);
        CHECK(shifted[0].kind == Binding::Kind::FollowOther && shifted[0].value == int(FA::Previous) && !shifted[1].bound());
    }
    // Heard: a MIDI track's patterns 1-3 (one note each), P1 Next after a bar, P2 (half a bar) Next after two plays,
    // P3 Stop after a bar: 60, 62 62, 64, then nothing, the track stopped; three follow actions came.
    auto run = [](bool chained, std::vector<std::pair<int, uint64_t>>& ons, EngineHost*& out_host) {
        using namespace midi_track;
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        load_sound(project->tracks[0], EngineKind::Midi, 0);
        project->tracks[0].params[Port] = UsbC; project->tracks[0].params[Channel] = 1;
        project->tempo = 1200;
        put(bank->at[0][0], 0, 60, 100); put(bank->at[0][1], 0, 62, 100); put(bank->at[0][2], 0, 64, 100);
        bank->at[0][1].length = 8 * sixteenth_ticks;
        bank->at[0][0].follow.action = uint8_t(FollowAction::Next);
        bank->at[0][1].follow.action = uint8_t(FollowAction::Next); bank->at[0][1].follow.count = 2; bank->at[0][1].follow.plays = true;
        bank->at[0][2].follow.action = uint8_t(FollowAction::Stop);
        auto* host = new EngineHost;
        host->load(*project, *bank);
        tones_for(*host, *project);
        host->set_audio_format(44100, 384);
        if (chained) host->send({Command::Type::Chained, 0, 0, 0, 1});
        host->send({Command::Type::Play});
        std::vector<int32_t> buffer(256);
        for (unsigned b = 0; b < 2900; ++b) { // five bars at 120 BPM
            host->render(buffer.data(), 128, 1);
            MidiOutEvent e;
            while (host->take_midi(e)) if (e.kind == MidiOutEvent::Kind::NoteOn) ons.push_back({e.a, e.frame});
        }
        out_host = host;
    };
    {
        std::vector<std::pair<int, uint64_t>> ons;
        EngineHost* host = nullptr;
        run(false, ons, host);
        const double bar = 44100 * 2.0;
        CHECK(ons.size() == 4 && ons[0].first == 60 && ons[1].first == 62 && ons[2].first == 62 && ons[3].first == 64);
        if (ons.size() == 4) {
            CHECK(std::llabs(int64_t(ons[1].second - ons[0].second) - std::llround(bar)) <= 2);
            CHECK(std::llabs(int64_t(ons[2].second - ons[1].second) - std::llround(bar / 2)) <= 2);
            CHECK(std::llabs(int64_t(ons[3].second - ons[1].second) - std::llround(bar)) <= 2);
        }
        CHECK(host->follows_of(0) == 3 && (host->stopped_tracks() & 1) && host->position_of(0) < 0);
        host->send({Command::Type::Pattern, 0, 0, 0, 0}); // a launch brings it back on the next bar
        std::vector<int32_t> buffer(256);
        bool back = false;
        for (unsigned b = 0; b < 800 && !back; ++b) {
            host->render(buffer.data(), 128, 1);
            MidiOutEvent e;
            while (host->take_midi(e)) back |= e.kind == MidiOutEvent::Kind::NoteOn && e.a == 60;
        }
        CHECK(back && !(host->stopped_tracks() & 1));
        delete host;
    }
    {
        std::vector<std::pair<int, uint64_t>> ons;
        EngineHost* host = nullptr;
        run(true, ons, host); // a chain is the track's order: its follow actions wait
        CHECK(ons.size() == 5 && host->follows_of(0) == 0);
        for (auto& on : ons) CHECK(on.first == 60);
        delete host;
    }
    // A scene's tempo takes over on the next bar while playing (at once while stopped).
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        project->tempo = 1200;
        auto host = std::make_unique<EngineHost>();
        host->load(*project, *bank);
        host->set_audio_format(44100, 384);
        host->send({Command::Type::SceneTempo, 0, 0, 0, 1000});
        std::vector<int32_t> buffer(256);
        host->render(buffer.data(), 128, 1);
        CHECK(host->tempo_now() == 1000);
        host->send({Command::Type::Play});
        for (unsigned b = 0; b < 100; ++b) host->render(buffer.data(), 128, 1);
        host->send({Command::Type::SceneTempo, 0, 0, 0, 1500});
        for (unsigned b = 0; b < 100; ++b) host->render(buffer.data(), 128, 1);
        CHECK(host->tempo_now() == 1000);
        for (unsigned b = 0; b < 900; ++b) host->render(buffer.data(), 128, 1); // past the bar line
        CHECK(host->tempo_now() == 1500);
    }
}

// Roadmap P5: always-on capture. Every track's notes kept in a ring made once, Record on or not; Shift + Seq Record lays
// the phrase (or so many bars) into patterns after the fact, the same bars for every track, on the bar lines while the
// transport ran, from the first note when stopped.
void capture_p5() {
    auto ring = std::make_unique<CaptureRing>();
    const uint32_t bar = 4 * ticks_per_quarter;
    // Played while the transport ran (run 3): track 2 a kick on bars 5 and 6, track 7 a bass note in bar 6; a gap of
    // three bars before them (bar 1's note on track 2 is another phrase).
    ring->press(2, 36, 100, 0, 3, 0 * bar + 10); ring->release(2, 36, 100, 0 * bar + 30);
    ring->press(2, 36, 110, 0, 3, 4 * bar); ring->release(2, 36, 100, 4 * bar + 24);
    ring->press(7, 40 | 2 << 8, 90, 0, 3, 5 * bar + 3 * sixteenth_ticks + 2); // still held
    ring->press(2, 36, 120, 0, 3, 5 * bar + 8 * sixteenth_ticks - 3); ring->release(2, 36, 100, 5 * bar + 8 * sixteenth_ticks + 20);
    CHECK(ring->count() == 4 && ring->played(2) == 3 && ring->played(7) == 1);
    CaptureAsk ask;
    ask.bar = bar; ask.grid = sixteenth_ticks; ask.ticks = 5 * bar + 12 * sixteenth_ticks;
    auto w = capture_window(*ring, ask);
    CHECK(w.any && w.run == 3 && w.start == 4 * bar && w.length == 2 * bar); // the phrase: bars 5 and 6
    Pattern kick, bass;
    CHECK(lay_capture(*ring, 2, w, ask, kick) && kick.length == 2 * bar && kick.events.size() == 2);
    CHECK(kick.events[0].tick == 0 && kick.events[0].velocity == 110 && kick.events[0].length == sixteenth_ticks);
    CHECK(kick.events[1].tick == bar + 8 * sixteenth_ticks && kick.events[1].velocity == 120); // onto its sixteenth
    CHECK(lay_capture(*ring, 7, w, ask, bass) && bass.events.size() == 1 && bass.events[0].tick == bar + 3 * sixteenth_ticks &&
          bass.events[0].key == 40 && bass.events[0].pitch == 2 && bass.events[0].length == 9 * sixteenth_ticks); // held until now
    Pattern none;
    CHECK(!lay_capture(*ring, 5, w, ask, none));
    ask.grid = 1; // as played
    CHECK(lay_capture(*ring, 2, w, ask, kick) && kick.events[1].tick == bar + 8 * sixteenth_ticks - 3 && kick.events[1].length == 23);
    ask.bars = 4; // four bars back from the bar after the last note: bars 3-6, bar 1's note still out
    w = capture_window(*ring, ask);
    CHECK(w.start == 2 * bar && w.length == 4 * bar && lay_capture(*ring, 2, w, ask, kick) && kick.events.size() == 2 && kick.events[0].tick == 2 * bar);
    ask.bars = 8; // reaching past the transport's start: from it
    w = capture_window(*ring, ask);
    CHECK(w.start == 0 && w.length == 8 * bar && lay_capture(*ring, 2, w, ask, kick) && kick.events.size() == 3 && kick.events[0].tick == 10);
    ask.tracks = 1u << 2; // only track 2 looked at: its own phrase
    ask.bars = capture_auto;
    w = capture_window(*ring, ask);
    CHECK(w.start == 4 * bar && w.length == 2 * bar);
    // Played stopped (run 0), at 120 BPM: the phrase's first note is its downbeat; a gap of more than two bars ends it.
    auto loose = std::make_unique<CaptureRing>();
    CaptureAsk still;
    still.bar = bar; still.grid = sixteenth_ticks; still.tick_ms = 60000.0 / 120 / ticks_per_quarter; // a bar is 2000 ms
    loose->press(4, 60, 100, 1000, 0, 0); loose->release(4, 60, 1100, 0);       // long before: not this phrase
    loose->press(4, 62, 100, 9000, 0, 0); loose->release(4, 62, 9240, 0);       // the downbeat
    loose->press(4, 64, 80, 9500 + 3, 0, 0); loose->release(4, 64, 9750, 0);    // a beat later, 3 ms late
    loose->press(4, 67, 90, 12100, 0, 0); loose->release(4, 67, 12300, 0);      // in the second bar
    still.now = 14000;
    auto sw = capture_window(*loose, still);
    CHECK(sw.any && sw.run == 0 && sw.start == 9000 && sw.length == 2 * bar);
    Pattern melody;
    CHECK(lay_capture(*loose, 4, sw, still, melody) && melody.events.size() == 3 && melody.events[0].tick == 0 &&
          melody.events[1].tick == ticks_per_quarter && melody.events[2].tick == bar + 9 * sixteenth_ticks); // 3100 ms in: 595 ticks, the nearest sixteenth
    CHECK(melody.events[0].length == 2 * sixteenth_ticks && melody.events[1].length == 2 * sixteenth_ticks);
    still.bars = 1; // a bar back from where the last note ended: the second bar's note alone, its own downbeat
    sw = capture_window(*loose, still);
    CHECK(sw.start == 12100 && sw.length == bar && lay_capture(*loose, 4, sw, still, melody) && melody.events.size() == 1 && melody.events[0].tick == 0);
    still.bars = 2; still.now = 30000; // asked long after: still the last two bars played, not two bars back from now
    sw = capture_window(*loose, still);
    CHECK(sw.start == 9000 && sw.length == 2 * bar && lay_capture(*loose, 4, sw, still, melody) && melody.events.size() == 3);
    // The ring goes round: minutes of notes, the oldest let go, nothing made as it plays.
    auto full = std::make_unique<CaptureRing>();
    for (unsigned n = 0; n < capture_size + 100; ++n) full->press(n % 3, 60, 100, n, 1, n * 24);
    CHECK(full->count() == capture_size && full->newest(0).at == capture_size + 99 && full->newest(capture_size - 1).at == 100);
    static_assert(sizeof(CaptureRing) < 256 * 1024, "the ring stays small");
}

// Roadmap P5: the song's tempo lane, stepped or ramped; the audio thread follows it a tick at a time while the song
// plays, so everything on the clock does; kept in TMPL; a MIDI file carries it.
void tempo_lane_p5() {
    TempoLane lane;
    CHECK(lane.empty() && lane.at(5, 1200) == 1200);
    CHECK(lane.set(8, 1400, true) && lane.set(4, 1000, false) && lane.set(12, 900, false) && lane.count == 3);
    CHECK(lane.points[0].bar == 4 && lane.points[1].bar == 8 && lane.points[2].bar == 12);
    CHECK(lane.set(8, 1600, true) && lane.count == 3 && lane.points[1].tempo == 1600); // the bar's point replaced
    CHECK(lane.at(3.9, 1200) == 1200 && lane.at(4, 1200) == 1000 && lane.at(6, 1200) == 1300 && lane.at(8, 1200) == 1600 &&
          lane.at(11.5, 1200) == 1600 && lane.at(12, 1200) == 900 && lane.at(400, 1200) == 900);
    CHECK(lane.at_bar(8) == 1 && lane.at_bar(9) == -1 && lane.erase(8) && !lane.erase(8) && lane.count == 2);
    CHECK(lane.at(6, 1200) == 1000); // with the ramp gone, the step's tempo holds
    TempoLane first; first.set(2, 1800, true); // a ramp from bar 1 at the project's tempo
    CHECK(std::fabs(first.at(1, 1200) - 1500) < 1e-9);
    // Seconds: four bars of 4/4 at 120 BPM are 8 s; a ramp 120 -> 240 over two bars takes 4 s * ln 2.
    TempoLane plain;
    CHECK(std::fabs(plain.seconds(4, 4, 1200) - 8) < 1e-9);
    TempoLane ramp; ramp.set(2, 2400, true);
    CHECK(std::fabs(ramp.seconds(2, 4, 1200) - 4 * std::log(2.0)) < 1e-6 && std::fabs(ramp.seconds(3, 4, 1200) - (4 * std::log(2.0) + 1)) < 1e-6);
    CHECK(std::fabs(ramp.bars_at(ramp.seconds(1.37, 4, 1200), 4, 1200) - 1.37) < 1e-6);
    // The file: TMPL; without it the project's tempo throughout.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        project->song.tempo.set(16, 1335, true); project->song.tempo.set(3, 900, false);
        auto data = save_project(*project, *bank);
        auto back = std::make_unique<Project>();
        auto back_bank = std::make_unique<PatternBank>();
        CHECK(load_project(data, *back, *back_bank).empty() && back->song.tempo == project->song.tempo);
        std::string text(data.begin(), data.end());
        data[text.find("TMPL")] = 'X';
        CHECK(load_project(data, *back, *back_bank).empty() && back->song.tempo.empty());
    }
    // Song's Shift + K1 / K2: the point at the cursor (none: the lane's tempo there, no shape).
    {
        auto project = std::make_unique<Project>();
        project->tempo = 1200;
        project->song.tempo.set(4, 1500, true);
        UiState ui;
        ui.page = PageId::Song; ui.song_cursor = 4;
        Pattern none;
        auto cards = shift_bindings(*project, ui, none);
        CHECK(cards[0].kind == Binding::Kind::SongTempo && cards[0].value == 150 && cards[0].min == 29 &&
              cards[1].kind == Binding::Kind::SongTempoShape && cards[1].value == 1);
        ui.song_cursor = 2;
        cards = shift_bindings(*project, ui, none);
        CHECK(cards[0].value == 135 && !cards[1].bound());
    }
    // Heard: a MIDI track's note each bar of a four-bar clip, the lane stepping to 60 BPM on bar 2 and ramping to 240
    // over bars 3 and 4. Bar 1 lasts 2 s, bar 2 4 s; the click's clock (the effects') follows; the tempo shown too.
    {
        using namespace midi_track;
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        load_sound(project->tracks[0], EngineKind::Midi, 0);
        project->tracks[0].params[Port] = UsbC; project->tracks[0].params[Channel] = 1;
        project->tempo = 1200;
        put(bank->at[0][0], 0, 60, 100);
        project->song.place({0, 0, 0, 6, false});
        project->song.tempo.set(1, 600, false);
        project->song.tempo.set(2, 600, false);
        project->song.tempo.set(4, 2400, true);
        auto host = std::make_unique<EngineHost>();
        host->load(*project, *bank);
        tones_for(*host, *project);
        host->set_audio_format(44100, 384);
        Command play {Command::Type::Play, 0, 0, 0, 1};
        host->send(play);
        std::vector<int32_t> buffer(256);
        std::vector<uint64_t> ons;
        double quarter_in_bar2 = 0;
        unsigned tempo_in_bar2 = 0, tempo_mid_ramp = 0;
        for (unsigned b = 0; b < 4200 && ons.size() < 5; ++b) {
            host->render(buffer.data(), 128, 1);
            MidiOutEvent e;
            while (host->take_midi(e)) if (e.kind == MidiOutEvent::Kind::NoteOn) ons.push_back(e.frame);
            if (host->song_bar() == 1 && host->ticks() % 384 >= 100 && !tempo_in_bar2) { tempo_in_bar2 = host->tempo_now(); quarter_in_bar2 = effect_clock.quarter; }
            if (host->song_bar() == 2 && host->ticks() % 384 == 192) tempo_mid_ramp = host->tempo_now();
        }
        CHECK(ons.size() >= 4);
        if (ons.size() >= 4) {
            CHECK(std::llabs(int64_t(ons[1] - ons[0]) - 88200) <= 2);  // bar 1 at 120 BPM
            CHECK(std::llabs(int64_t(ons[2] - ons[1]) - 176400) <= 2); // bar 2 at 60
            const auto& lane = project->song.tempo; // bar 3, the first half of the ramp 60 -> 240 over bars 3 and 4
            double ramp = (lane.seconds(3, 4, 1200) - lane.seconds(2, 4, 1200)) * 44100;
            CHECK(std::fabs(double(ons[3] - ons[2]) - ramp) < 0.002 * ramp); // within the ramp's tenths' steps
        }
        CHECK(tempo_in_bar2 == 600 && std::fabs(quarter_in_bar2 - 44100) < 1);
        CHECK(tempo_mid_ramp >= 1000 && tempo_mid_ramp <= 1400); // half way through bar 3: 60 + 180 / 4 = 105 BPM
        host->send({Command::Type::Stop});
        host->render(buffer.data(), 128, 1);
        CHECK(host->tempo_now() == 1200); // the patterns: the project's
    }
    // A MIDI file of the song: a tempo change where the lane steps, and as the ramp moves.
    {
        auto project = std::make_unique<Project>();
        project->tempo = 1200;
        project->song.place({0, 0, 0, 4, false});
        project->song.tempo.set(1, 900, false);
        project->song.tempo.set(3, 1200, true);
        auto tempos = smf_tempos(*project);
        CHECK(!tempos.empty() && tempos[0].first == 384 && tempos[0].second == 900 && tempos.back().second == 1200 && tempos.size() > 10);
        auto bytes = write_smf({}, 1200, 4, 4, tempos);
        unsigned count = 0;
        for (size_t n = 0; n + 2 < bytes.size(); ++n) count += bytes[n] == 0xff && bytes[n + 1] == 0x51 && bytes[n + 2] == 3;
        CHECK(count == tempos.size() + 1);
    }
}

// A pad's values reach what it plays now, gliding there without a step: a knob (a Pad command), Motion, the kit's
// values, the tempo, an instrument's trim, the pad's region. A new sample on the pad leaves what plays alone.
void live_pad_values() {
    // A second of DC on the left (each frame is the voice's gain heard), a fast sine on the right (a low pass shows).
    SampleData data;
    data.frames = 44100;
    data.left.assign(44100, 0.5f); data.right.resize(44100);
    for (unsigned n = 0; n < 44100; ++n) data.right[n] = float(std::sin(n * 1.0) * 0.5);
    SampleData other = data;
    SamplerEngine::Table table {};
    table[3] = &data; table[4] = &other;
    const int key = int(sampler_first_note);
    struct Heard { float left = 0, right = 0, jump = 0; std::vector<float> l; };
    // Renders `blocks` of 128: the last block's peaks and the largest step from frame to frame on the left.
    auto run = [&](SamplerEngine& e, unsigned blocks) {
        Heard h;
        std::vector<float> l(128), r(128);
        float last = -1;
        for (unsigned b = 0; b < blocks; ++b) {
            std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
            e.render_stereo(l.data(), r.data(), 128);
            h.left = h.right = 0;
            for (unsigned f = 0; f < 128; ++f) {
                CHECK(std::isfinite(l[f]) && std::isfinite(r[f]));
                if (last >= 0) h.jump = std::max(h.jump, std::fabs(l[f] - last));
                last = l[f];
                h.left = std::max(h.left, std::fabs(l[f])); h.right = std::max(h.right, std::fabs(r[f]));
                h.l.push_back(l[f]);
            }
        }
        return h;
    };
    auto tap = [](const SamplerEngine& e) { TrackTaps t; e.taps(t); return t; };
    PadSound held;
    held.sample = 3; held.play = PadPlay::Gate; held.velocity_sense = 0; held.attack = 0; held.release = 0;
    auto engine_with = [&](const PadSound& pad) {
        auto e = std::make_unique<SamplerEngine>();
        e->set_samples(&table);
        e->set_pad(0, pad);
        e->note_on(key, 127);
        return e;
    };
    { // the level turned down as it plays: down to nothing while held, gliding there
        auto e = engine_with(held);
        auto before = run(*e, 10);
        CHECK(before.left > 0.3f);
        PadSound quiet = held; quiet.level = 0;
        e->set_pad(0, quiet);
        auto after = run(*e, 30);
        CHECK(e->sounding() && after.left < 1e-3f);
        CHECK(after.jump < 0.01f); // no step: 0.35 down over 8 ms
        CHECK(after.l[64] > 0.2f && after.l[64] < before.left); // under way after 64 frames, not there yet
    }
    { // the pad's filter closed as it plays: by a knob, then by Motion; the high side goes, the DC stays
        PadSound open = held; open.filter = PadFilter::Lpf2; open.cutoff = 100;
        auto e = engine_with(open);
        auto before = run(*e, 10);
        PadSound shut = open; shut.cutoff = 25;
        e->set_pad(0, shut);
        auto after = run(*e, 30);
        CHECK(before.right > 0.2f && after.right < 0.25f * before.right);
        CHECK(std::fabs(after.left - before.left) < 0.02f && after.jump < 0.01f);
        e->move_pad(0, PadParam::FilterCutoff, 100); // Motion opens it again on the same note
        CHECK(run(*e, 30).right > 0.8f * before.right);
        e->restore_pads(); // the pattern's start: the kit's own cutoff, heard at once
        CHECK(run(*e, 30).right < 0.25f * before.right);
    }
    { // pitch: the pad's Semi, then the kit's TUNE, retune the note held
        auto e = engine_with(held);
        run(*e, 4);
        CHECK(std::fabs(tap(*e).voice[0].step - 1.0f) < 1e-4f);
        PadSound up = held; up.pitch = 12;
        e->set_pad(0, up);
        run(*e, 1);
        const float gliding = tap(*e).voice[0].step;
        CHECK(gliding > 1.0f && gliding < 2.0f); // on its way
        run(*e, 30);
        CHECK(std::fabs(tap(*e).voice[0].step - 2.0f) < 1e-3f);
        e->set_param(sampler::Tune, -12);
        run(*e, 30);
        CHECK(std::fabs(tap(*e).voice[0].step - 1.0f) < 1e-3f);
    }
    { // pan and the kit's level, heard on the note held
        auto e = engine_with(held);
        auto centre = run(*e, 10);
        PadSound left = held; left.pan = -50;
        e->set_pad(0, left);
        auto panned = run(*e, 30);
        CHECK(panned.right < 1e-3f && panned.left > 1.35f * centre.left && panned.jump < 0.01f);
        e->set_param(sampler::Level, 0);
        CHECK(run(*e, 30).left < 1e-3f && e->sounding());
    }
    { // a sustain turned while held: the envelope goes there at its decay, no step
        PadSound sus = held; sus.decay = 0; sus.sustain = 100;
        auto e = engine_with(sus);
        auto full = run(*e, 10);
        PadSound half = sus; half.sustain = 50;
        e->set_pad(0, half);
        auto after = run(*e, 30);
        CHECK(std::fabs(after.left - 0.5f * full.left) < 0.01f && after.jump < 0.01f);
    }
    { // a new sample on the pad: what plays goes on as it was, the next hit plays the new one
        auto e = engine_with(held);
        run(*e, 4);
        PadSound swapped = held; swapped.sample = 4; swapped.pitch = 12; swapped.level = 0;
        e->set_pad(0, swapped);
        auto still = run(*e, 30);
        auto t = tap(*e);
        CHECK(t.count == 1 && t.voice[0].slot == 3 && std::fabs(t.voice[0].step - 1.0f) < 1e-4f && still.left > 0.3f);
    }
    { // a one shot whose end is trimmed behind where it plays fades out at once, without a click
        PadSound shot = held; shot.play = PadPlay::OneShot;
        auto e = engine_with(shot);
        run(*e, 20);
        PadSound trimmed = shot; trimmed.end = 1000;
        e->set_pad(0, trimmed);
        auto gone = run(*e, 4);
        CHECK(!e->sounding() && gone.jump < 0.02f);
    }
    { // a loop's end moved as it loops: it goes round its new loop
        PadSound looped = held; looped.loop = true; looped.end = 4410;
        auto e = engine_with(looped);
        run(*e, 20);
        PadSound shorter = looped; shorter.end = 2205;
        e->set_pad(0, shorter);
        run(*e, 60);
        auto t = tap(*e);
        CHECK(e->sounding() && t.count == 1 && t.voice[0].to == 2205 && t.voice[0].at < 2205);
    }
    { // Follow: a tempo change speeds a note that follows it, stretched from where it is, without a dip
        PadSound follow = held; follow.own_tempo = 1200;
        auto e = std::make_unique<SamplerEngine>();
        e->set_samples(&table);
        e->set_param(sampler::Follow, 1);
        e->set_param(sampler::FollowHow, 1); // Tones: windows that sum to one
        e->set_tempo(1200);
        e->set_pad(0, follow);
        e->note_on(key, 127);
        auto before = run(*e, 10);
        CHECK(!(tap(*e).voice[0].flags & tap::Stretch));
        e->set_tempo(1800);
        auto after = run(*e, 40);
        auto t = tap(*e);
        CHECK((t.voice[0].flags & tap::Stretch) && std::fabs(t.voice[0].step - 1.5f) < 1e-3f);
        CHECK(std::fabs(after.left - before.left) < 0.02f && after.jump < 0.01f);
    }
    { // ZONES' trim on an instrument's zone, heard on the note held
        ZoneRig r;
        r.part(zone_tone(440, 0.5f, 44100));
        r.zone(0).root = 60;
        r.build(PadPlay::Gate);
        r.key(0);
        CHECK(std::fabs(zone_hz(r.run(8000), 800, 8000) - 440) < 8);
        r.engine->set_trim(0, 0, 3, 100, 0);
        CHECK(std::fabs(zone_hz(r.run(8000), 800, 8000) - 440 * std::pow(2.0, 1 / 12.0)) < 8);
        r.pad.cutoff = 100; r.pad.pitch = 12; // the pad's Semi moves the note held an octave
        r.engine->set_pad(0, r.pad);
        CHECK(std::fabs(zone_hz(r.run(8000), 800, 8000) - 880 * std::pow(2.0, 1 / 12.0)) < 14);
    }
}

// Freeze plan F2a: prints. A print host (EngineHost::Lean) holding one track of a project plays it as the whole
// project's host plays that track, sample for sample, after its chain; the hash moves with what made the print and
// nothing else; PRNT keeps the records; the ticks fall where tick_frame() says; a plug-in's latency is taken out.
void prints_f2a() {
    const float rate = dsp::sample_rate();
    // Two kits: track 1 through an Echo and a Pumper (tempo-synced) with a send into a reverb bus, playing pattern 4;
    // track 2 its own beat. Track 1 at unity and centred, as a print plays it.
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    tone_kit(*project, 0); tone_kit(*project, 1);
    auto& t1 = project->tracks[0];
    t1.fx.slots[0] = {uint8_t(EffectKind::Echo), false, {40, 50, 50, 50, 0, 0}};
    t1.fx.slots[1] = {uint8_t(EffectKind::Pumper), false, {4, 50, 60, 20, 20, 50}};
    t1.mix.sends[0] = 40;
    t1.pattern = 3;
    project->buses[0].fx.slots[0] = {uint8_t(EffectKind::Reverb), false, {70, 50, 40, 50, 0, 0}};
    project->swing = 58;
    project->tempo = 1333;
    for (unsigned s = 0; s < 16; s += 3) put(bank->at[0][3], s, uint8_t(sampler_first_note + s % 4), uint8_t(70 + s));
    for (unsigned s = 0; s < 16; s += 2) put(bank->at[1][0], s, uint8_t(sampler_first_note + 1), 90);
    put(bank->at[0][0], 0, uint8_t(sampler_first_note), 100); // another pattern of track 1: not played
    auto tone = tone_sample();
    const uint32_t ticks = played_pattern(bank->at[0][3], t1.speed).length;

    // The whole project, track 1 taken after its chain as the Track source takes it.
    auto live = std::make_unique<EngineHost>();
    live->load(*project, *bank);
    CHECK(live->offer_sample(0, &tone));
    live->set_print_tap(0, true);
    std::vector<int32_t> out(256);
    live->send({Command::Type::Play}); // the sample taken in at the first block's start, before Play
    std::vector<float> heard_l, heard_r;
    live->render(out.data(), 128, 1);
    const uint64_t pass = live->tick_frame(ticks);
    CHECK(pass > 44100 && pass < 3 * 44100);
    for (unsigned f = 0; f < 128; ++f) { heard_l.push_back(live->print_left()[f]); heard_r.push_back(live->print_right()[f]); }
    while (heard_l.size() < 2 * pass + 256) {
        live->render(out.data(), 128, 1);
        for (unsigned f = 0; f < 128; ++f) { heard_l.push_back(live->print_left()[f]); heard_r.push_back(live->print_right()[f]); }
    }
    CHECK(peak_of(heard_l, size_t(pass), size_t(2 * pass)) > 0.05f);

    auto print = [&](PrintTail tail, PrintTap tap = PrintTap::PostFx) {
        auto p = std::make_unique<Project>(*project);
        auto b = std::make_unique<PatternBank>(*bank);
        print_project(*p, *b, 0, 3);
        CHECK(!p->tracks[1].used() && b->at[0][0].empty() && !b->at[0][3].empty() && p->tracks[0].pattern == 3);
        CHECK(p->buses[0].fx.slots[0].kind == 0 && p->tracks[0].mix.sends[0] == 0);
        auto host = std::make_unique<EngineHost>(EngineHost::Lean {2});
        CHECK(host->lean() && host->delay_blocks_free() == 2);
        host->load(*p, *b);
        CHECK(host->offer_sample(0, &tone));
        return render_print(*host, 0, ticks, tap, tail);
    };
    auto loop = print(PrintTail::Loop);
    CHECK(loop.error.empty() && loop.pass == pass && loop.latency == 0 && loop.left.size() == pass);
    uint64_t differ = 0;
    for (uint64_t f = 0; f < pass; ++f) differ += loop.left[f] != heard_l[pass + f] || loop.right[f] != heard_r[pass + f];
    CHECK(differ == 0); // the second pass, with the first's echoes in it, as heard
    auto cut = print(PrintTail::Cut);
    differ = 0;
    for (uint64_t f = 0; f < pass; ++f) differ += cut.left[f] != heard_l[f] || cut.right[f] != heard_r[f];
    CHECK(cut.left.size() == pass && differ == 0);
    auto ring = print(PrintTail::Ring); // the sequencer stops where the pass ends: the echoes ring out, then quiet
    CHECK(ring.left.size() > pass + uint64_t(0.3 * rate) && ring.left.size() < pass + uint64_t(9 * rate));
    CHECK(peak_of(ring.left, size_t(pass + uint64_t(0.1 * rate))) > 0.01f);
    for (uint64_t f = 0; f < pass - pass % 128; ++f) CHECK(ring.left[f] == cut.left[f]); // its last block is split
    auto pre = print(PrintTail::Cut, PrintTap::PreFx); // the engine alone: no echo after the last hit
    CHECK(pre.left.size() == pass && peak_of(pre.left) > 0.05f && pre.left != cut.left);
    CHECK(model_frames_of(pass, rate) == uint32_t(std::llround(double(pass) * 44100 / rate)));

    // A print host's own effect clock: rendering one beside the live host on the same thread, at another tempo, with
    // a Pumper of its own, leaves what the live host plays as it was.
    {
        auto again = std::make_unique<EngineHost>();
        again->load(*project, *bank);
        CHECK(again->offer_sample(0, &tone));
        auto other = std::make_unique<Project>(*project);
        auto ob = std::make_unique<PatternBank>(*bank);
        print_project(*other, *ob, 0, 3);
        other->tempo = 900;
        auto beside = std::make_unique<EngineHost>(EngineHost::Lean {2});
        beside->load(*other, *ob);
        CHECK(beside->offer_sample(0, &tone));
        again->set_print_tap(0, true);
        std::vector<int32_t> o(256);
        again->send({Command::Type::Play}); beside->send({Command::Type::Play});
        differ = 0;
        for (uint64_t at = 0; at < 2 * pass; at += 128) {
            beside->render(o.data(), 128, 1);
            again->render(o.data(), 128, 1);
            for (unsigned f = 0; f < 128; ++f) differ += again->print_left()[f] != heard_l[at + f];
        }
        CHECK(differ == 0);
    }

    // Each tick on the frame tick_frame() says: one frame a render, the tick count watched.
    {
        auto p = std::make_unique<Project>();
        auto b = std::make_unique<PatternBank>();
        tone_kit(*p, 0);
        p->tempo = 1777; p->swing = 63;
        b->at[0][0].length = 4 * sixteenth_ticks;
        auto host = std::make_unique<EngineHost>();
        host->load(*p, *b);
        host->send({Command::Type::Play});
        std::vector<int32_t> o(2);
        std::vector<uint64_t> at;
        for (uint64_t f = 0; at.size() < 40; ++f) {
            uint32_t before = host->ticks();
            host->render(o.data(), 1, 1);
            if (host->ticks() > before) at.push_back(f);
        }
        for (uint32_t k = 0; k < at.size(); ++k) CHECK(host->tick_frame(k) == at[k]);
    }

    // The hash: what makes the print moves it, the mix, other tracks and other patterns do not.
    auto states = std::array<std::vector<uint8_t>, max_plugins> {};
    auto hash = [&](const Project& p, const PatternBank& b, PrintTap tap = PrintTap::PostFx, PrintTail tail = PrintTail::Loop,
                    unsigned at = 48000) {
        auto q = std::make_unique<Project>(p);
        auto k = std::make_unique<PatternBank>(b);
        print_project(*q, *k, 0, 3);
        return print_hash(*q, *k, states, tap, tail, at);
    };
    const uint64_t h = hash(*project, *bank);
    CHECK(h == hash(*project, *bank));
    auto changed = [&](const std::function<void(Project&, PatternBank&)>& change) {
        auto p = std::make_unique<Project>(*project);
        auto b = std::make_unique<PatternBank>(*bank);
        change(*p, *b);
        return hash(*p, *b) != h;
    };
    CHECK(!changed([](Project& p, PatternBank&) { p.tracks[0].mix.level = 64; p.tracks[0].mix.pan = -30; p.tracks[0].mix.sends[1] = 80; }));
    CHECK(!changed([](Project& p, PatternBank&) { p.tracks[0].mix.mute = true; p.tracks[1].mix.solo = true; }));
    CHECK(!changed([](Project& p, PatternBank&) { p.tracks[0].set_name("Drums"); p.tracks[0].last_pad = 7; p.name[0] = 'X'; }));
    CHECK(!changed([](Project&, PatternBank& b) { put(b.at[1][0], 1, uint8_t(sampler_first_note), 50); }));     // another track's notes
    CHECK(!changed([](Project&, PatternBank& b) { put(b.at[0][0], 5, uint8_t(sampler_first_note), 50); }));     // another pattern
    CHECK(!changed([](Project& p, PatternBank&) { p.buses[0].fx.slots[0].params[0] = 10; p.master.slots[0] = {uint8_t(EffectKind::Drive), false, {}}; }));
    CHECK(!changed([](Project&, PatternBank& b) { b.at[0][3].write_motion(point(2, motion_level, 40)); })); // a mix lane stays live
    CHECK(changed([](Project&, PatternBank& b) { put(b.at[0][3], 1, uint8_t(sampler_first_note), 50); }));      // its notes
    CHECK(changed([](Project&, PatternBank& b) { b.at[0][3].length = 8 * sixteenth_ticks; }));
    CHECK(changed([](Project& p, PatternBank&) { p.tracks[0].pads[0].pitch = 3; }));                          // its sound
    CHECK(changed([](Project& p, PatternBank&) { p.tracks[0].fx.slots[0].params[1] = 77; }));                 // its chain
    CHECK(changed([](Project& p, PatternBank&) { p.tempo = 1200; }));
    CHECK(changed([](Project& p, PatternBank&) { p.tracks[0].speed = 3; }));
    CHECK(changed([](Project& p, PatternBank&) { std::snprintf(p.samples[0].path, sizeof(p.samples[0].path), "/media/card/Other.wav"); }));
    CHECK(hash(*project, *bank, PrintTap::PreFx) != h && hash(*project, *bank, PrintTap::PostFx, PrintTail::Ring) != h);
    CHECK(hash(*project, *bank, PrintTap::PostFx, PrintTail::Loop, 44100) != h); // printed at another rate
    {   // a plug-in's state counts only where the track uses it
        auto p = std::make_unique<Project>(*project);
        std::snprintf(p->plugins[2].path, sizeof(p->plugins[2].path), "Plugins/Some Effect.vst3");
        uint64_t unused = hash(*p, *bank);
        states[2] = {1, 2, 3};
        CHECK(hash(*p, *bank) == unused);
        p->tracks[0].fx.slots[2] = {plugin_effect_kind, false, {2, 0, 0, 0, 0, 0}};
        uint64_t used = hash(*p, *bank);
        states[2] = {1, 2, 4};
        CHECK(hash(*p, *bank) != used && used != unused);
        states[2].clear();
    }

    // PRNT: the records go and come back with the project; a file without them loads with none.
    PrintTable table;
    PrintRecord a; a.track = 0; a.pattern = 3; a.frames = 88200; a.pass = 88200; a.hash = h; a.file = print_file_name(0, 3);
    PrintRecord b2; b2.track = 5; b2.pattern = 127; b2.tap = PrintTap::PreFx; b2.tail = PrintTail::Ring; b2.frames = 99999; b2.pass = 44100;
    b2.rate = 96000;
    b2.hash = 0xfedcba9876543210ull; b2.file = print_file_name(5, 127);
    table.put(b2); table.put(a);
    CHECK(table.records.size() == 2 && table.records[0].track == 0 && table.find(5, 127) && !table.find(5, 126));
    CHECK(a.file == "Track 1 A4.wav" && b2.file == "Track 6 H16.wav");
    auto bytes = save_project(*project, *bank, nullptr, &table);
    auto back = std::make_unique<Project>();
    auto back_bank = std::make_unique<PatternBank>();
    PrintTable read;
    CHECK(load_project(bytes, *back, *back_bank, nullptr, nullptr, &read).empty() && read == table);
    CHECK(load_project(bytes, *back, *back_bank).empty() && back->tracks[0].pattern == 3); // as an older build reads it
    CHECK(load_project(save_project(*project, *bank), *back, *back_bank, nullptr, nullptr, &read).empty() && read.records.empty());
    CHECK(prints_folder("/media/card/Projects/Song.mldp") == "/media/card/Projects/Song Prints" && prints_folder("").empty());

    // A plug-in 300 frames late: its print starts on the pattern's first tick all the same.
    {
        auto p = std::make_unique<Project>();
        auto b = std::make_unique<PatternBank>();
        synth(p->tracks[0], 0);
        put(b->at[0][0], 0, 60, 100);
        auto host = std::make_unique<EngineHost>(EngineHost::Lean {0});
        host->load(*p, *b);
        LatePlugin slow;
        slow.late = 300;
        CHECK(host->offer_plugin(0, &slow));
        auto made = render_print(*host, 0, played_pattern(b->at[0][0], speed_normal).length, PrintTap::PostFx, PrintTail::Cut);
        CHECK(made.error.empty() && made.latency == 300 && std::fabs(made.left[0] - 0.5f) < 1e-4f);
        for (unsigned f = 1; f < 2000; ++f) CHECK(made.left[f] == 0.0f);
    }
    std::puts("prints ok");
}

int main() {
    tempo_lane_p5();
    capture_p5();
    scenes_p5();
    banks_f1();
    prints_f2a();
    exports_p4();
    midi_tracks_p4();
    modulators_p4();
    delay_pool_clears();
    mix_motion_p4();
    delay_sync_p4();
    keyed_dynamics_p4();
    delay_compensation_p4();
    chop_kept();
    samples_used();
    levels_cutoff_kept();
    pad_link_chains();
    loop_seams();
    instrument_p3();
    instrument_formats_p3();
    pad_layers_p3();
    track_speed();
    generators();
    feel_p1();
    note_repeat_p1();
    ratchets();
    trig_conditions();
    pattern_motion();
    kit_lanes();
    plugins();
    tone3000_always_lite();
    synths_as_plugins();
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
    one_dispatch();
    loops_and_buses_parallel();
    overload_guard();
    parked_load();
    sample_rates_p2();
    sampler_zones();
    instrument_zones_r3();
    live_pad_values();
    note_pitches();
    track_source();
    zero_snap();
    looper_engine();
    looper_rates();
    roll();
    roll_opens_on_notes();
    stage();
    midi_tracks();
    device_rates();
    rate_constants();
    key_pick();
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
    hits_on_time();
    pad_expression();
    parity_1c();
    shift_pads();
    pad_lights();
    track_move();
    drum_layout();
    button_remap();
    looper_chord_and_sheet();
    parity_basics();
    history();
    names();
    settings();
    punches();
    kept_punches();
    helpers();
    project_files();
    recording();
    sampler_kit();
    effects();
    arrange();
    motion();
    layouts();
    bindings();
    host_sequencer();
    sequenced_legato();
    audio_quality();
    std::puts("melodee ok");
}
