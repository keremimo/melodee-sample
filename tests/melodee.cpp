// SPDX-License-Identifier: AGPL-3.0-only
// Melodee's model and audio side on the host: pad layouts, knob bindings,
// the engine host's sequencer and mixer, and synth tracks as plug-ins.
#include "drivers/direct_monitor.h"
#include "melodee/engine/effects.h"
#include "melodee/model/slice_banks.h"
#include "melodee/model/slice_hits.h"
#include "melodee/model/song_steps.h"
#include "melodee/model/jam_log.h"
#include "melodee/engine/macros.h"
#include <sstream>
#include <deque>
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
#include "melodee/engine/onsets.h"
#include "melodee/engine/root_find.h"
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
#include "melodee/shell/usage_log.h"
#include "melodee/shell/controls_rules.h"
#include "melodee/shell/light_show.h"
#include "melodee/shell/xpm_write.h"
#include "melodee/engine/auto_probe.h"
#include "melodee/engine/external_notes.h"
#include "melodee/engine/release_fit.h"
#include "melodee/model/card_folders.h"
#include "melodee/shell/stems_find.h"
#include "melodee/shell/audio_monitor.h"
#include "melodee/shell/auto_sample.h"
#include "melodee/engine/loop_find.h"
#include "melodee/engine/engine_host.h"
#include "melodee/engine/print_player.h"
#include "melodee/engine/print_render.h"
#include "melodee/model/print.h"
#include "melodee/engine/plugin_engine.h"
#include "melodee/plugin/plugin_sleep.h"
#include "melodee/plugin/rate_bridge.h"
#include "melodee/plugin/idle_gate.h"
#include "melodee/engine/punch.h"
#include "melodee/engine/pad_fx_info.h"
#include "melodee/model/mod_matrix.h"
#include "melodee/engine/arp.h"
#include "melodee/model/note_edits.h"
#include "melodee/model/chords.h"
#include "melodee/model/progression.h"
#include "melodee/model/project_file.h"
#include "melodee/model/keygroup_track.h"
#include "melodee/model/generators.h"
#include "melodee/model/smf.h"
#include "melodee/model/lane_value.h"
#include "melodee/model/scales.h"
#include "melodee/shell/bindings.h"
#include "melodee/shell/step_sounds.h"
#include "melodee/shell/new_track.h"
#include "melodee/shell/pad_colour.h"
#include "melodee/shell/zoom_view.h"
#include "melodee/shell/browse_trim.h"
#include "melodee/shell/settings.h"
#include "melodee/shell/sample_lists.h"
#include "melodee/shell/card_paths.h"
#include "melodee/shell/history_list.h"
#include "melodee/shell/capture_ring.h"
#include "melodee/shell/place_bar.h"
#include "melodee/shell/playheads.h"
#include "melodee/shell/record_place.h"
#include "melodee/engine/voice_tap.h"
#include "melodee/shell/input_choices.h"
#include "melodee/shell/input_route.h"
#include "melodee/shell/comp_trail.h"
#include "melodee/shell/fader_job.h"
#include "melodee/shell/midi_route.h"
#include "melodee/shell/mtc_in.h"
#include "melodee/shell/midi_config_rows.h"
#include "melodee/shell/program_in.h"
#include "melodee/plugin/program_jobs.h"
#include "melodee/shell/scale_keys.h"
#include "melodee/engine/midi_out.h"
#include "melodee/engine/midi_engine.h"
#include "melodee/shell/takeover.h"
#include "melodee/shell/midi_ports.h"
#include "melodee/model/fader.h"
#include "melodee/engine/effect_info.h"
#include "melodee/shell/history.h"
#include "melodee/shell/mixer_channels.h"
#include "melodee/shell/spectrum_view.h"
#include "melodee/shell/name_entry.h"
#include "melodee/shell/name_suggest.h"
#include "melodee/model/sound_file.h"
#include "melodee/shell/sound_list.h"
#include "melodee/shell/key_pick.h"
#include "melodee/ui/card_rows.h"
#include "melodee/shell/button_jobs.h"
#include "melodee/shell/page_stack.h"
#include "melodee/shell/tap_hold.h"
#include "melodee/shell/autosave.h"
#include "melodee/shell/features.h"
#include "melodee/shell/lesson.h"
#include "melodee/shell/levels_keys.h"
#include "melodee/shell/progression_tree.h"
#include "melodee/shell/track_colours.h"
#include "melodee/shell/time_correct.h"
#include "melodee/shell/pad_context.h"
#include "melodee/shell/pad_light.h"
#include "melodee/shell/menu.h"
#include "melodee/shell/stems.h"
#include "melodee/shell/stems_room.h"
#include "melodee/shell/song_take.h"
#include "melodee/shell/sample_memory.h"
#include "melodee/shell/stream_holds.h"
#include "melodee/engine/stream_pager.h"
#include "melodee/shell/sequences.h"
#include "melodee/shell/free_length.h"
#include "melodee/shell/note_preview.h"
#include "melodee/shell/pad_job.h"
#include "melodee/shell/project_kits.h"
#include "melodee/shell/pad_gap.h"
#include "melodee/shell/mix_hub.h"
#include "melodee/shell/sample_mode.h"
#include "melodee/shell/sampler_map.h"
#include "melodee/shell/zone_shapes.h"
#include "melodee/shell/screen_view.h"
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
#include "melodee/plugin/plugin_files.h"
#include "melodee/plugin/xpl_preset.h"
#include "melodee/plugin/page_file.h"
#include "melodee/plugin/companion_package.h"
#include "melodee/plugin/page_library.h"
#include "melodee/plugin/nam.h"
#include "melodee/plugin/preset_rows.h"
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
    std::snprintf(project.samples.edit(0).path, sizeof(SampleRef::path), "/media/card/Samples/Tone.wav");
    project.samples.edit(0).frames = 44100;
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

void bindings() {
    Project project;
    load_sound(project.tracks[0], EngineKind::Midi, 0);
    UiState ui;
    Pattern pattern;
    ui.stage_sub[0] = 2; // a MIDI track's Stage B1 CC: its three controllers
    auto stage = page_bindings(project, ui, pattern);
    CHECK(stage[0].kind == Binding::Kind::Param && stage[0].id == midi_track::Cc1);
    CHECK(stage[0].at(0) == 0 && stage[0].at(127) == 127 && stage[0].at(64) == 64);
    ui.stage_sub[0] = 0;
    ui.page = PageId::Mixer; // the mixer's SENDS: A, B, C; Shift: pan, output, D
    auto mix = page_bindings(project, ui, pattern);
    CHECK(mix[0].kind == Binding::Kind::Send && mix[0].id == 0 && mix[2].kind == Binding::Kind::Send && mix[2].id == 2);
    auto mix_shift = page_bindings(project, ui, pattern, true);
    CHECK(mix_shift[0].kind == Binding::Kind::Pan && mix_shift[0].at(0) == -50 && mix_shift[0].at(127) == 50);
    { // Held keys: Pad Bank held puts the selected track's strip on K1-K3 on any page, over Knob FX too.
        UiState held = ui; held.page = PageId::Roll; held.pad_bank = true;
        auto strip = page_bindings(project, held, pattern);
        CHECK(strip[0].kind == Binding::Kind::Level && strip[1].kind == Binding::Kind::Pan && strip[2].kind == Binding::Kind::Send && strip[2].id == 0);
        CHECK(!shift_bindings(project, held, pattern)[0].bound()); // one row
        held.pad_bank = false; held.copying = UiState::Copying::Tracks; // a track move picking its tracks: the same
        CHECK(page_bindings(project, held, pattern)[0].kind == Binding::Kind::Level);
    }
    ui.page = PageId::StepEdit; ui.step_pads = true; // Step Edit's pads on the steps
    CHECK(!page_bindings(project, ui, pattern)[0].bound()); // an empty step has nothing on the cards
    put(pattern, 0, 50, 90);
    CHECK(page_bindings(project, ui, pattern)[1].value == 90);
    ui.step_pads = false;
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
        auto host_held = std::make_unique<EngineHost>(); // on the heap: a test's frame stays under the 8 MB stack
        auto& host = *host_held;
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
void put32(std::ofstream& f, uint32_t v) { for (int n = 0; n < 4; ++n) f.put(char(v >> (8 * n))); }
void put16(std::ofstream& f, uint16_t v) { f.put(char(v)); f.put(char(v >> 8)); }

void sampler_kit() {
    // A 16-bit stereo WAV at 22.05 kHz: four clicks a quarter second apart, kept at its rate.
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
    CHECK(data.frames == 22050 && data.rate == 22050 && data.left.size() == 22050 && data.model_frames() == 44100);
    CHECK(!read_wav("/nonexistent.wav", data).empty());
    Overview overview;
    make_overview(data, overview);
    if (!(overview[0] > 50 && overview[60] == 0)) std::fprintf(stderr, "overview %d %d frames %u\n", overview[0], overview[60], data.frames);
    CHECK(overview[0] > 50 && overview[60] == 0);
    std::array<Hit, max_hits> hits {};
    unsigned found = find_hits(data, hits);
    CHECK(found >= 4 && hits[0].frame == 0);
    // Chop: hits at the clicks, even parts, beats at 120 BPM (22050 frames each).
    auto even = chop(ChopMode::Regions, 4, data.model_frames(), hits, found, 1200);
    CHECK(even.count == 4 && even.start[2] == 22050);
    auto beats = chop(ChopMode::Beats, 4, data.model_frames(), hits, found, 1200);
    CHECK(beats.count == 2 && beats.start[1] == 22050);
    auto clicks = chop(ChopMode::Threshold, 0, data.model_frames(), hits, found, 1200);
    CHECK(clicks.count >= 4);
    for (unsigned n = 1; n < 4; ++n) CHECK(std::abs(int(clicks.start[n]) - int(n * 11024)) < 600);
    Slices edited = even;
    CHECK(edited.add(1000, data.model_frames()) && edited.count == 5 && edited.start[1] == 1000);
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
    // The short loop for plain runs (SamplerEngine::run_plain()) sounds exactly as the full loop: one shots, slices
    // with their edge fades, reversed, looped, filtered, tails, bends gliding, chokes and pads changed mid-note.
    {
        SampleData noise;
        noise.frames = 44100;
        noise.left.resize(44100); noise.right.resize(44100);
        uint32_t seed = 7;
        for (unsigned n = 0; n < 44100; ++n) {
            seed = seed * 1664525u + 1013904223u;
            noise.left[n] = float(int32_t(seed) >> 8) / float(1 << 23) * 0.5f;
            noise.right[n] = 0.4f * float(std::sin(n * 0.02));
        }
        SamplerEngine::Table kit_table {};
        kit_table[1] = &noise;
        auto play = [&](bool plain) {
            SamplerEngine::plain_runs = plain;
            SamplerEngine e;
            e.set_samples(&kit_table);
            std::array<PadSound, 8> pads {};
            for (auto& p : pads) p.sample = 1;
            pads[1].start = 1000; pads[1].end = 9000;                                // edges
            pads[2].start = 2000; pads[2].end = 12000; pads[2].reverse = true;
            pads[3].play = PadPlay::Gate; pads[3].loop = true; pads[3].loop_lock = false; pads[3].loop_start = 3000;
            pads[3].start = 500; pads[3].end = 6000;
            pads[4].filter = PadFilter::Lpf2; pads[4].cutoff = 40; pads[4].resonance = 50; pads[4].filter_amount = 60;
            pads[5].pitch = 7; pads[5].loop = true; pads[5].reverse = true; pads[5].play = PadPlay::Gate; pads[5].end = 9000;
            pads[6].filter = PadFilter::Classic; pads[6].cutoff = 60; pads[6].decay = 30;
            pads[7].play = PadPlay::Gate; pads[7].release = 60; pads[7].group = 2; pads[6].group = 2;
            pads[0].decay = 40;
            for (unsigned p = 0; p < 8; ++p) e.set_pad(p, pads[p]);
            std::vector<float> out;
            std::vector<float> l(128), r(128);
            const unsigned sizes[] {128, 64, 128, 17, 128, 100};
            for (unsigned b = 0; b < 420; ++b) {
                if (b % 9 == 0) e.note_on(int(sampler_first_note) + int(b / 9 % 8), 60 + int(b % 60));
                if (b % 13 == 5) e.note_off(int(sampler_first_note) + int(b / 13 % 8));
                if (b == 120) e.set_bend(0.7f);
                if (b == 160) e.set_bend(-0.3f);
                if (b == 200) { pads[4].cutoff = 80; pads[3].pitch = -5; e.set_pad(4, pads[4]); e.set_pad(3, pads[3]); }
                if (b == 260) { pads[0].filter = PadFilter::Hpf4; e.set_pad(0, pads[0]); }
                unsigned n = sizes[b % 6];
                std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
                e.render_stereo(l.data(), r.data(), n);
                out.insert(out.end(), l.begin(), l.begin() + n);
                out.insert(out.end(), r.begin(), r.begin() + n);
            }
            SamplerEngine::plain_runs = true;
            return out;
        };
        SamplerEngine::plain_taken = 0;
        const auto full = play(false);
        CHECK(SamplerEngine::plain_taken == 0);
        const auto plain = play(true);
        CHECK(SamplerEngine::plain_taken > 500); // most runs took the short loop
        CHECK(full.size() == plain.size());
        size_t differ = 0;
        float loudest = 0;
        for (size_t n = 0; n < full.size(); ++n) { differ += full[n] != plain[n]; loudest = std::max(loudest, std::fabs(full[n])); }
        if (differ) std::fprintf(stderr, "run_plain: %zu samples differ\n", differ);
        CHECK(differ == 0 && loudest > 0.05f);
    }
    // Warped pads (stretch()) the same way: Tones' window weights from the engine's table and the tail check made once a
    // run sound exactly as working each out frame by frame, in Beats and in Tones, looped, reversed, sliced, filtered.
    {
        SampleData noise;
        noise.frames = 44100;
        noise.left.resize(44100); noise.right.resize(44100);
        uint32_t seed = 11;
        for (unsigned n = 0; n < 44100; ++n) {
            seed = seed * 1664525u + 1013904223u;
            noise.left[n] = float(int32_t(seed) >> 8) / float(1 << 23) * 0.5f;
            noise.right[n] = 0.4f * float(std::sin(n * 0.017));
        }
        SamplerEngine::Table warp_table {};
        warp_table[1] = &noise;
        auto play = [&](bool plain, int how) {
            SamplerEngine::plain_runs = plain;
            SamplerEngine e;
            e.set_samples(&warp_table);
            e.set_param(sampler::FollowHow, how);
            std::array<PadSound, 6> pads {};
            for (auto& p : pads) { p.sample = 1; p.warp = 150; p.play = PadPlay::Gate; }
            pads[1].warp = 70; pads[1].start = 3000; pads[1].end = 20000;               // a slice, its edges
            pads[2].reverse = true; pads[2].end = 30000;
            pads[3].loop = true; pads[3].loop_lock = false; pads[3].loop_start = 5000; pads[3].end = 15000;
            pads[4].filter = PadFilter::Lpf4; pads[4].cutoff = 50; pads[4].decay = 30; pads[4].play = PadPlay::OneShot;
            pads[5].warp = 120; pads[5].pitch = 5; pads[5].play = PadPlay::OneShot; pads[5].end = 9000; pads[5].decay = 50;
            for (unsigned p = 0; p < 6; ++p) e.set_pad(p, pads[p]);
            std::vector<float> out;
            std::vector<float> l(128), r(128);
            const unsigned sizes[] {128, 64, 128, 19, 128};
            for (unsigned b = 0; b < 400; ++b) {
                if (b % 11 == 0) e.note_on(int(sampler_first_note) + int(b / 11 % 6), 70 + int(b % 50));
                if (b % 17 == 8) e.note_off(int(sampler_first_note) + int(b / 17 % 6));
                if (b == 150) e.set_bend(0.4f);
                if (b == 230) { pads[0].warp = 90; e.set_pad(0, pads[0]); }
                unsigned n = sizes[b % 5];
                std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
                e.render_stereo(l.data(), r.data(), n);
                out.insert(out.end(), l.begin(), l.begin() + n);
                out.insert(out.end(), r.begin(), r.begin() + n);
            }
            SamplerEngine::plain_runs = true;
            return out;
        };
        for (int how : {0, 1}) {
            const auto full = play(false, how);
            SamplerEngine::plain_taken = 0;
            const auto plain = play(true, how);
            CHECK(how == 0 || SamplerEngine::plain_taken > 300); // Tones: its runs took the table
            size_t differ = 0;
            float loudest = 0;
            for (size_t n = 0; n < std::min(full.size(), plain.size()); ++n) { differ += full[n] != plain[n]; loudest = std::max(loudest, std::fabs(full[n])); }
            if (differ) std::fprintf(stderr, "stretch (how %d): %zu samples differ\n", how, differ);
            CHECK(full.size() == plain.size() && differ == 0 && loudest > 0.05f);
        }
    }
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

    // MIDI in: a keyboard's pitch wheel bends the kit's voices (two semitones each way, gliding there), and the
    // sustain pedal (CC 64) holds a note let go until it comes up.
    {
        SamplerEngine kit;
        kit.set_samples(&table);
        PadSound pad; pad.sample = 3; pad.play = PadPlay::Gate;
        kit.set_pad(4, pad);
        const int key = int(sampler_first_note) + 4;
        kit.note_on(key, 120);
        std::vector<float> l(128, 0), r(128, 0);
        TrackTaps taps;
        kit.render_stereo(l.data(), r.data(), 128); kit.taps(taps);
        CHECK(taps.count == 1);
        const float straight = taps.voice[0].step;
        kit.set_bend(1.0f);
        for (unsigned b = 0; b < 40; ++b) kit.render_stereo(l.data(), r.data(), 128);
        kit.taps(taps);
        CHECK(std::fabs(taps.voice[0].step / straight - float(std::exp2(2.0 / 12))) < 0.01f);
        kit.set_bend(0.0f);
        for (unsigned b = 0; b < 40; ++b) kit.render_stereo(l.data(), r.data(), 128);
        kit.taps(taps);
        CHECK(std::fabs(taps.voice[0].step / straight - 1.0f) < 0.01f);
        kit.control(64, 127);
        kit.note_off(key);
        for (unsigned b = 0; b < 100; ++b) kit.render_stereo(l.data(), r.data(), 128);
        kit.taps(taps);
        CHECK(kit.sounding() && taps.voice[0].level > 0.5f); // held on by the pedal
        kit.control(64, 0);
        for (unsigned b = 0; b < 100; ++b) kit.render_stereo(l.data(), r.data(), 128);
        CHECK(!kit.sounding());
        kit.control(64, 127); // a key pressed again under the pedal is not let go when it comes up
        kit.note_on(key, 120);
        kit.note_off(key);
        kit.note_on(key, 120);
        kit.control(64, 0);
        for (unsigned b = 0; b < 100; ++b) kit.render_stereo(l.data(), r.data(), 128);
        CHECK(kit.sounding());
    }

    // Cloud: a held pad becomes grains, and sounds while held.
    {
        SamplerEngine cloud;
        cloud.set_samples(&table);
        PadSound pad; pad.sample = 3; pad.play = PadPlay::Gate; pad.cloud = true; pad.cloud_density = 60;
        cloud.set_pad(2, pad);
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
        PadSound pad; pad.sample = 3; pad.start = 0; pad.end = 4410; pad.play = PadPlay::OneShot; pad.cloud = true; pad.cloud_density = 60;
        cloud.set_pad(2, pad);
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
    // Tones (WSOLA): a held tone pitched up a fifth and kept to length holds its level (its windows add in phase, no
    // beating) from its first frame; clicks an octave up, or down, are each heard once.
    {
        auto play = [](const SampleData& data, int semitones) {
            SamplerEngine::Table table {};
            table[0] = &data;
            auto engine = std::make_unique<SamplerEngine>();
            engine->set_samples(&table);
            engine->set_param(sampler::FollowHow, 1);
            PadSound sound; sound.sample = 0; sound.pitch = int8_t(semitones); sound.decay = 100; sound.warp = 100;
            engine->set_pad(0, sound);
            engine->note_on(int(sampler_first_note), 127);
            std::vector<float> l(128), r(128), all;
            for (unsigned b = 0; b < 1000 && engine->sounding(); ++b) {
                std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
                engine->render_stereo(l.data(), r.data(), 128);
                all.insert(all.end(), l.begin(), l.end());
            }
            return all;
        };
        auto chord = std::make_unique<SampleData>();
        chord->frames = 44100;
        chord->left.resize(44100); chord->right.resize(44100);
        for (unsigned n = 0; n < 44100; ++n)
            chord->left[n] = chord->right[n] = float(0.3 * std::sin(2 * 3.14159265 * 220 * n / 44100.0) + 0.2 * std::sin(2 * 3.14159265 * 440 * n / 44100.0)
                                                    + 0.1 * std::sin(2 * 3.14159265 * 660 * n / 44100.0));
        const auto held = play(*chord, 7);
        auto rms = [&](size_t at) { double e = 0; for (size_t n = at; n < at + 512; ++n) e += held[n] * held[n]; return std::sqrt(e / 512); };
        double low = 1e9, high = 0, sum = 0;
        unsigned count = 0;
        for (size_t at = 4096; at + 512 < std::min<size_t>(held.size(), 36000); at += 256) { const double x = rms(at); low = std::min(low, x); high = std::max(high, x); sum += x; ++count; }
        const double mean = count ? sum / count : 0;
        const bool steady = count > 100 && mean > 0.05 && (high - low) / mean < 0.06 && rms(0) > 0.7 * mean;
        if (!steady) std::fprintf(stderr, "tones held: mean %f, swing %f, first %f\n", mean, mean > 0 ? (high - low) / mean : 0.0, rms(0));
        CHECK(steady);
        auto clicks = std::make_unique<SampleData>();
        clicks->frames = 44100;
        clicks->left.assign(44100, 0.0f); clicks->right.assign(44100, 0.0f);
        for (unsigned c = 0; c < 4; ++c)
            for (unsigned n = 0; n < 400; ++n)
                clicks->left[4000 + c * 10000 + n] = clicks->right[4000 + c * 10000 + n] = float(0.8 * std::exp(-double(n) / 60.0) * std::sin(n * 0.9));
        for (int semitones : {12, -12}) {
            const auto out = play(*clicks, semitones);
            unsigned hits = 0;
            float before = 0;
            for (size_t at = 0; at + 64 <= out.size(); at += 64) {
                float e = 0;
                for (size_t n = at; n < at + 64; ++n) e += std::fabs(out[n]);
                if (e > 4 * before && e > 64 * 0.02f) ++hits;
                before = e;
            }
            if (hits != 4) std::fprintf(stderr, "tones clicks %+d: %u hits\n", semitones, hits);
            CHECK(hits == 4);
        }
        // Round a kit pad's loop too: two clicks in a loop of 20000 frames, each heard once a lap, on time.
        auto looped = [&](int semitones) {
            SamplerEngine::Table table {};
            table[0] = clicks.get();
            auto engine = std::make_unique<SamplerEngine>();
            engine->set_samples(&table);
            engine->set_param(sampler::FollowHow, 1);
            PadSound sound; sound.sample = 0; sound.pitch = int8_t(semitones); sound.decay = 100; sound.warp = 100;
            sound.loop = true; sound.loop_lock = false; sound.loop_start = 2000; sound.end = 22000; sound.play = PadPlay::Gate;
            engine->set_pad(0, sound);
            engine->note_on(int(sampler_first_note), 127);
            std::vector<float> l(128), r(128), all;
            for (unsigned b = 0; b < 600; ++b) { // 76800 frames: three laps and more
                std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
                engine->render_stereo(l.data(), r.data(), 128);
                all.insert(all.end(), l.begin(), l.end());
            }
            std::vector<size_t> at;
            float before = 0;
            for (size_t n = 0; n + 64 <= all.size(); n += 64) {
                float e = 0;
                for (size_t k = n; k < n + 64; ++k) e += std::fabs(all[k]);
                if (e > 4 * before && e > 64 * 0.02f) at.push_back(n);
                before = std::max(e, before * 0.97f);
            }
            return at;
        };
        for (int semitones : {12, -7}) {
            const auto at = looped(semitones);
            // The clicks at 4000 and 14000 (2000 and 12000 into the loop), every 20000 frames.
            bool on_time = at.size() == 8;
            for (size_t k = 0; k < at.size() && on_time; ++k) {
                const long want = 4000 + long(k % 2) * 10000 + long(k / 2) * 20000;
                on_time = std::labs(long(at[k]) - want) <= 128;
            }
            if (!on_time) {
                std::fprintf(stderr, "tones loop clicks %+d:", semitones);
                for (size_t n : at) std::fprintf(stderr, " %zu", n);
                std::fprintf(stderr, "\n");
            }
            CHECK(on_time);
        }
        // Read faster than a frame a frame, band-limited: 13 kHz an octave up (26 kHz) is gone, not folded back to
        // 18 kHz; 3 kHz a fifth up keeps its level.
        auto tone = [&](double hz, int semitones) {
            auto data = std::make_unique<SampleData>();
            data->frames = 44100;
            data->left.resize(44100); data->right.resize(44100);
            for (unsigned n = 0; n < 44100; ++n) data->left[n] = data->right[n] = float(0.5 * std::sin(2 * 3.14159265 * hz * n / 44100.0));
            const auto out = play(*data, semitones);
            double e = 0;
            unsigned count = 0;
            for (size_t n = 4096; n < std::min<size_t>(out.size(), 36000); ++n) { e += out[n] * out[n]; ++count; }
            return count ? std::sqrt(e / count) : 0.0;
        };
        const double own = tone(3000, 0), folded = tone(13000, 12) / own, kept = tone(3000, 7) / own;
        if (!(folded < 0.003 && kept > 0.9 && kept < 1.1)) std::fprintf(stderr, "tones band: folded %f, kept %f\n", folded, kept);
        CHECK(folded < 0.003 && kept > 0.9 && kept < 1.1);
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
    // A punch written up to the song's end (or past it) lets go where the song ends; it sounded on until Stop.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        project->song.place({0, 0, 0, 1, false});
        project->song.add_punch({0, 8, 16, uint8_t(PunchKind::Gate), 255, 100});
        auto host = std::make_unique<EngineHost>();
        host->load(*project, *bank);
        host->send({Command::Type::Play, 0, 0, 0, 1});
        std::vector<int32_t> out(256);
        for (unsigned n = 0; n < 500; ++n) host->render(out.data(), 128, 1); // the bar's third beat
        CHECK(host->playing() && host->punching());
        for (unsigned n = 0; n < 250; ++n) host->render(out.data(), 128, 1); // past the bar's end
        CHECK(!host->playing() && !host->punching());
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
    // An audio track's take on Song (shell/song_take.h): armed, it waits while its lane has no clip, starts with the
    // clip's first bar (its pattern as long as a pattern can be, so no pass ends it), and ends where -2 says, kept.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        load_sound(project->tracks[2], EngineKind::Audio, 0);
        bank->at[2][4].length = project->max_length();
        project->song.place({2, 4, 1, 2, false}); // bars 2-3; bar 1 silent on its lane
        auto host_held = std::make_unique<EngineHost>();
        auto& host = *host_held;
        host.load(*project, *bank);
        host.send({Command::Type::AudioRecord, 2, 0, 0, 0});
        host.send({Command::Type::SongRecord, 0, 0, 0, 1});
        host.send({Command::Type::Play, 0, 0, 0, 1});
        std::vector<int32_t> out(256);
        for (unsigned b = 0; b < 88200 / 2 / 128; ++b) host.render(out.data(), 128, 1); // half of bar 1
        CHECK(host.audio_take_track() == 2 && host.audio_take().cued() && host.audio_take_pattern() < 0);
        for (unsigned b = 0; b < 88200 / 128; ++b) host.render(out.data(), 128, 1); // into bar 2
        CHECK(host.song_bar() == 1 && host.audio_take().running() && host.audio_take_pattern() == 4);
        for (unsigned b = 0; b < 88200 / 128; ++b) host.render(out.data(), 128, 1); // into bar 3: no pass ends it
        CHECK(host.song_bar() == 2 && host.audio_take().running());
        host.send({Command::Type::AudioRecord, 2, 0, 0, -2});
        host.render(out.data(), 128, 1);
        CHECK(host.audio_take_track() == -1 && !host.audio_take().running() && !host.audio_take().cued());
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
// Chord progressions (the Prog layout, model/progression.h): an MPC 3 file's chords on the pads, moved by K1 KEY,
// voiced by K2, stored from held keys, kept by the project (PROG).
void progressions() {
    auto owned = std::make_unique<Project>();
    auto& project = *owned;
    auto& t = project.tracks[0];
    load_sound(t, EngineKind::Midi, 0);
    t.layout = PadLayout::Prog;
    t.prog.root = 4; // E
    const uint8_t am9[] {45, 67, 71, 72, 76}, cm9[] {49, 56, 64, 68, 71, 75};
    t.prog.chords[0].count = 6; t.prog.chords[0].root = true;
    for (unsigned k = 0; k < 6; ++k) t.prog.chords[0].notes[k] = cm9[k];
    std::snprintf(t.prog.chords[0].name, sizeof(t.prog.chords[0].name), "C#min9");
    t.prog.chords[17].count = 5;
    for (unsigned k = 0; k < 5; ++k) t.prog.chords[17].notes[k] = am9[k];
    std::snprintf(t.prog.chords[17].name, sizeof(t.prog.chords[17].name), "F#Maj(add2)/A#");
    t.prog.count = 18;
    CHECK(prog_pads(t));
    std::array<int, notes_per_step> keys {};
    // Pad 1 of bank A: the file's notes as they are, at OCT 2; the Root chord is the pad's root.
    CHECK(chord_notes(project, t, 0, keys) == 6 && keys[0] == 49 && keys[5] == 75 && pad_note(project, t, 0).root);
    CHECK(chord_notes(project, t, 1, keys) == 0 && pad_note(project, t, 1).key == -1); // no chord there
    t.bank = 1; // Pad Bank: chords 17-32
    CHECK(chord_notes(project, t, 1, keys) == 5 && keys[0] == 45 && prog_index(t, 1) == 17);
    // The voicings of A1 G3 B3 C4 E4 (MIDI 45 67 71 72 76).
    t.prog_voicing = unsigned(ProgVoicing::Close);
    CHECK(prog_keys(t, 17, keys) == 5 && keys[0] == 45 && keys[1] == 47 && keys[2] == 48 && keys[3] == 52 && keys[4] == 55);
    t.prog_voicing = unsigned(ProgVoicing::Open);
    CHECK(prog_keys(t, 17, keys) == 5 && keys[0] == 45 && keys[1] == 47 && keys[2] == 52 && keys[3] == 60 && keys[4] == 67);
    t.prog_voicing = unsigned(ProgVoicing::Spread);
    CHECK(prog_keys(t, 17, keys) == 5 && keys[0] == 33 && keys[1] == 67);
    t.prog_voicing = 0;
    // K1 KEY: E to G moves every chord up three semitones and its name with it; the octave moves by twelve.
    UiState ui;
    ui.page = PageId::Keys;
    Pattern pattern;
    auto cards = page_bindings(project, ui, pattern);
    CHECK(cards[0].kind == Binding::Kind::ProgKey && cards[0].value == 4 && cards[1].kind == Binding::Kind::ProgVoicing && cards[2].kind == Binding::Kind::Layout);
    t.prog_shift = 3;
    CHECK(prog_key(t) == 7 && prog_keys(t, 17, keys) == 5 && keys[0] == 48);
    CHECK(prog_pad_name(t, 17).chord == "Aadd2" && prog_pad_name(t, 17).bass == "C#");
    t.octave = 3;
    CHECK(prog_keys(t, 17, keys) == 5 && keys[0] == 60);
    t.octave = 2; t.prog_shift = 0;
    // Pad names, short.
    CHECK(short_chord_name("C#min9") == "C#m9" && short_chord_name("EMaj9") == "Emaj9" && short_chord_name("C7(#9#5)") == "C7#5#9");
    CHECK(short_chord_name("A9(13)") == "A13" && short_chord_name("C#min9(11)") == "C#m11" && short_chord_name("DMaj") == "D");
    CHECK(pad_chord_name("B6/9").bass.empty() && pad_chord_name("AMaj(add2)/Db").chord == "Aadd2");
    CHECK(transpose_chord_name("AMaj(add2)/Db", 0) == "AMaj(add2)/C#" && transpose_chord_name("F#min9(b5)", 2) == "G#min9(b5)");
    // A chord stored from held keys: lowest first, each key once, named.
    Progression mine;
    const int held[] {60, 64, 67, 64, 71};
    CHECK(store_prog_chord(mine, 12, held, 5) && mine.count == 13 && mine.chords[12].count == 4);
    CHECK(mine.chords[12].notes[0] == 60 && mine.chords[12].notes[3] == 71 && std::string(mine.chords[12].name) == "Cmaj7");
    CHECK(!store_prog_chord(mine, prog_chords, held, 5));
    // The project keeps the progression, its key and voicing.
    t.prog_shift = -2; t.prog_voicing = 3;
    std::snprintf(t.prog.name, sizeof(t.prog.name), "Prisms 2-A Lydian");
    std::snprintf(t.prog.file, sizeof(t.prog.file), "A Lydian");
    auto bank = std::make_unique<PatternBank>();
    auto bytes = save_project(project, *bank);
    auto back = std::make_unique<Project>();
    CHECK(load_project(bytes, *back, *bank).empty());
    const auto& b = back->tracks[0];
    CHECK(b.layout == PadLayout::Prog && b.prog == t.prog && b.prog_shift == -2 && b.prog_voicing == 3);
    CHECK(back->tracks[1].prog.empty());
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
    // SCREEN's Palette and Font: NIGHT and Rubik first, stopping at the ends, kept in the text, a number out of range kept in.
    CHECK(c.value_text(Settings::Palette) == "NIGHT" && c.value_text(Settings::Font) == "Rubik");
    c.step(Settings::Palette, -1); CHECK(c.palette == 0);
    c.step(Settings::Palette, 1); CHECK(c.value_text(Settings::Palette) == "DAY");
    for (int n = 0; n < 20; ++n) c.step(Settings::Font, 1);
    CHECK(c.font + 1u == ui::face_choices && c.value_text(Settings::Font) == "VT323");
    { Settings back; back.read(c.text()); CHECK(back.palette == 1 && back.font + 1u == ui::face_choices); }
    { Settings lcd; lcd.palette = uint8_t(ui::palettes.size() - 1); CHECK(lcd.value_text(Settings::Palette) == "LCD" && lcd.value_text(Settings::Font) == "Melodee Pixel"); }
    { Settings odd; odd.read("palette=99\nfont=99\n"); CHECK(odd.palette + 1u == ui::palettes.size() && odd.font + 1u == ui::face_choices); }
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
    // SEQ > Follow: Off, Page, Step, stopping at the ends; kept in the text.
    CHECK(c.seq_follow == 0 && c.value_text(Settings::SeqFollow) == "Off" && std::string(Settings::row_name(Settings::SeqFollow)) == "Follow");
    c.step(Settings::SeqFollow, -1); CHECK(c.seq_follow == 0);
    c.step(Settings::SeqFollow, 1); CHECK(c.value_text(Settings::SeqFollow) == "Page");
    c.step(Settings::SeqFollow, 1); c.step(Settings::SeqFollow, 1); CHECK(c.seq_follow == 2 && c.value_text(Settings::SeqFollow) == "Step");
    { Settings back; back.read(c.text()); CHECK(back.seq_follow == 2); }
    { Settings odd; odd.read("seq_follow=7\n"); CHECK(odd.seq_follow == 2); }
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
    // Their names come from their own list, never from past the end of the engine's table (an AUDIO track with
    // two sounds of one's own crashed the Sounds page: its table has one sound).
    const auto& audio = engine_info(EngineKind::Audio);
    const std::vector<std::string> own {"TAPE IN", "VOX"};
    CHECK(audio.sound_count == 1 && std::string(sound_name(audio, 0, own)) == "AUDIO");
    CHECK(std::string(sound_name(audio, 1, own)) == "TAPE IN" && std::string(sound_name(audio, 2, own)) == "VOX");
    CHECK(sound_name(audio, 3, own) == nullptr && sound_name(audio, 1, {}) == nullptr);
}
} // namespace

// Undo and redo: edits move between the lists, a new one drops the redos,
// and only the last 512 are kept, fewer when they hold more than the budget.
void history() {
    Track t;
    t.pads[3].sample = 7; t.pads[3].pitch = 5; t.pad_fx.at[3].slots[0].kind = 2;
    PadState copied = pad_state(t, 3);
    set_pad_state(t, 20, copied);
    CHECK(t.pads[20].sample == 7 && t.pads[20].pitch == 5 && t.pad_fx.at[20].slots[0].kind == 2 && t.pad_fx.at[20].size == pad_slots);
    CHECK(pad_text("Delete", 20) == "Delete B05");

    History h;
    CHECK(!h.undo() && !h.redo());
    const int pushed = int(History::depth) + 8;
    for (int n = 0; n < pushed; ++n) { Edit e; e.pad = int16_t(n); h.push(std::move(e)); }
    int undone = 0;
    while (Edit* e = h.undo()) { CHECK(e->pad == pushed - 1 - undone); ++undone; }
    CHECK(History::depth == 512 && undone == int(History::depth));
    Edit* again = h.redo();
    CHECK(again && again->pad == 8);   // the oldest kept
    CHECK(h.can_redo());
    Edit fresh; fresh.pad = 99;
    h.push(std::move(fresh));          // a new edit: nothing to redo
    CHECK(!h.can_redo() && h.undo()->pad == 99 && h.undo()->pad == 8 && !h.can_undo());
    { // The memory budget: steps that each hold a 4 MB pattern copy keep 16 of them, the newest; the count follows.
        History big;
        for (int n = 0; n < 40; ++n) {
            Edit e; e.pad = int16_t(n);
            e.pattern_before = std::make_unique<Pattern>();
            e.pattern_before->events.resize((size_t(4) << 20) / sizeof(Event)); // about 4 MB of notes
            big.push(std::move(e));
        }
        CHECK(big.size() < 40 && big.bytes() <= History::budget && big.last()->pad == 39);
        CHECK(big.size() * (4u << 20) <= History::budget + (4u << 20));
        while (big.undo()) {}
        big.push(Edit {});                 // undone steps go with the next edit, and their memory with them
        CHECK(big.size() == 1 && big.bytes() < 4096);
    }

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
    for (unsigned n : {0u, 3u, 9u}) CHECK(shift_pad_job(n).kind == K::Toggle); // Full Level, Count-In, Rec Quantize (one press, as the MPC)
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
    CHECK(shift_pad_level(ui, 9) == 1); // off: the pad faint
    // Rec Quantize has no grid of its own (the pattern's Q, as the MPC): a file from when it had one still reads.
    Settings old; old.read("rec_grid=3\nrec_quantize=0\n");
    CHECK(!old.rec_quantize && old.text().find("rec_grid") == std::string::npos);
    { // Settings' SEQ row: Rec quantize, as Shift + Pad 10
        Settings g;
        CHECK(g.value_text(Settings::RecQuantize) == "On" && std::string(Settings::row_name(Settings::RecQuantize)) == "Rec quantize");
        g.step(Settings::RecQuantize, 1);
        CHECK(!g.rec_quantize && g.value_text(Settings::RecQuantize) == "Off");
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
    Pattern pattern; pattern.length = 3 * project->bar_ticks(); pattern.free = false; // a length set by hand
    UiState ui; ui.track = 0;
    ui.page = PageId::Patterns;
    auto cards = page_bindings(*project, ui, pattern);
    CHECK(cards[0].kind == Binding::Kind::PatternLength && pattern_lengths[cards[0].value] == 3); // every bar to 16 (issue 52)
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
    ui.page = PageId::Mixer; ui.seq_held = true;
    cards = page_bindings(*project, ui, pattern);
    CHECK(cards[0].kind == Binding::Kind::PatternLength && cards[1].kind == Binding::Kind::Division && cards[2].kind == Binding::Kind::Swing);
    cards = page_bindings(*project, ui, pattern, true);
    CHECK(cards[0].kind == Binding::Kind::PatternSteps && cards[1].kind == Binding::Kind::TimeSignature && cards[2].kind == Binding::Kind::TrackSpeed);
    project->knob_fx.on = true; project->knob_fx.kind = 1;
    CHECK(page_bindings(*project, ui, pattern)[0].kind == Binding::Kind::PatternLength);
    ui.seq_held = false;
    CHECK(page_bindings(*project, ui, pattern)[0].kind != Binding::Kind::PatternLength); // Knob FX's again
    project->knob_fx.on = false; project->knob_fx.kind = 0;

    // Back: Stage -> Sounds -> Patterns -> Roll -> Motion -> Song; Back walks Song, the SEQ group, Sounds, Stage.
    UiState nav;
    auto go = [&](PageId page) { push_back_stack(nav, page); nav.page = page; };
    auto back = [&] { PageId to = back_target(nav); go(to); };
    go(PageId::Sounds); go(PageId::Patterns); go(PageId::Roll); go(PageId::Motion); go(PageId::Song);
    CHECK(nav.back_depth == 3);
    back(); CHECK(nav.page == PageId::Motion);
    back(); CHECK(nav.page == PageId::Sounds); // Patterns, Roll and Motion are one stop
    back(); CHECK(nav.page == PageId::Stage && nav.back_depth == 0);
    back(); CHECK(nav.page == PageId::Stage);
    // Opening a page already on the stack goes back to it instead of growing it.
    go(PageId::Sounds); go(PageId::Patterns); go(PageId::Sounds);
    CHECK(nav.back_depth == 1 && nav.back_stack[0] == PageId::Stage);
    // The pads chip (shell/pad_job.h): what the pads do when they do not play the track; "" while they play.
    {
        UiState j; j.track = 0;
        auto job = [&] { return std::string(pad_job(*project, j)); };
        j.page = PageId::Stage; CHECK(job().empty());
        j.page = PageId::Scenes; CHECK(job() == "SEQUENCES");
        j.page = PageId::StepEdit; CHECK(job().empty()); j.step_pads = true; CHECK(job() == "STEPS"); j.step_pads = false;
        j.page = PageId::Roll; j.seq_held = true; CHECK(job() == "PATTERNS"); // Seq held: the track's patterns
        j.shift = true; CHECK(job() == "SEQUENCES");                          // + Shift: the sequences, every track
        j.erase = true; CHECK(job() == "PATTERNS"); j.erase = false;          // Erase + pad clears the track's pattern
        j.seq_print = true; CHECK(job() == "PATTERNS"); j.seq_print = false;  // freezing: the track's patterns
        j.page = PageId::Patterns; CHECK(job() == "SEQUENCES"); j.shift = false; // on Patterns too
        CHECK(job() == "PATTERNS");
        j.page = PageId::Roll;
        j.pad_bank = true; CHECK(job() == "TRACKS"); // Pad Bank held wins
        j.menu = menu_go_view; CHECK(job().empty()); // a sheet draws its own pads
        j.menu = menu_closed; j.copying = UiState::Copying::Tracks; CHECK(job().empty()); // a move's prompt has the header
    }
    // Sequences first (shell/sequences.h): Seq held's pads are the track's patterns, the sequences with Shift held;
    // a launch takes every used track; an empty kept pattern takes the sequence's length; a track off what the
    // sequence in hand keeps for it is marked.
    {
        UiState q; q.track = 0; q.page = PageId::Stage;
        CHECK(!sequences_on_pads(q));
        q.seq_held = true; CHECK(!sequences_on_pads(q) && !sequences_lit(q));
        q.shift = true; CHECK(sequences_on_pads(q) && sequences_lit(q));
        q.erase = true; CHECK(sequences_on_pads(q) && !sequences_lit(q)); q.erase = false;
        q.seq_print = true; CHECK(!sequences_on_pads(q)); q.seq_print = false;
        q.copying = UiState::Copying::Patterns; CHECK(!sequences_on_pads(q)); q.copying = UiState::Copying::Off;
        q.page = PageId::Patterns; CHECK(sequences_on_pads(q)); q.page = PageId::Stage;
        q.seq_held = false; CHECK(!sequences_on_pads(q)); q.shift = false; // Shift alone: its printed jobs
        // Settings > SEQ > Seq + pad on Sequences (issue 50): Seq held's pads the sequences, Shift + pad the patterns;
        // Patterns' own pads (Seq not held) stay as they are.
        q.settings.seq_pads_sequences = true;
        CHECK(!pad_launches_sequence(q) && !sequences_on_pads(q));
        q.shift = true; CHECK(pad_launches_sequence(q) && !sequences_on_pads(q)); q.shift = false; // Patterns' Shift + pad
        q.seq_held = true; CHECK(pad_launches_sequence(q) && sequences_on_pads(q) && sequences_lit(q));
        q.shift = true; CHECK(!pad_launches_sequence(q) && !sequences_on_pads(q) && !sequences_lit(q));
        q.shift = false; q.seq_print = true; CHECK(!sequences_on_pads(q)); q.seq_print = false;
        q.erase = true; CHECK(sequences_on_pads(q) && !sequences_lit(q)); q.erase = false;
        q.seq_held = false; q.settings.seq_pads_sequences = false;
        {
            Settings fresh;
            CHECK(!fresh.seq_pads_sequences && fresh.value_text(Settings::SeqPads) == "Patterns" &&
                  std::string(Settings::row_name(Settings::SeqPads)) == "Seq + pad");
            Settings swapped = fresh;
            swapped.step(Settings::SeqPads, 1);
            CHECK(swapped.seq_pads_sequences && swapped.value_text(Settings::SeqPads) == "Sequences");
            Settings back; back.read(swapped.text());
            CHECK(back.seq_pads_sequences);
            Settings old; old.read("note_mutes=1\n"); // a file from before the row: Patterns
            CHECK(!old.seq_pads_sequences);
        }
        auto p = std::make_unique<Project>();
        auto b = std::make_unique<PatternBank>();
        Track used = project->tracks[0]; // a track with a sound
        CHECK(used.used() && sequence_takes(used) && !sequence_takes(Track {}));
        p->tracks[0] = used; p->tracks[1] = used; p->tracks[2] = used;
        p->scene = 3;
        for (unsigned t = 0; t < 3; ++t) p->tracks[t].in_scene[3] = 4; // SEQ 4 keeps P4 on each
        p->tracks[0].pattern = 3; p->tracks[1].pattern = 3; p->tracks[2].pattern = 5;
        CHECK(sequence_in_view(*p, q) == 3 && !own_pattern(*p, q, 0) && !own_pattern(*p, q, 1) && own_pattern(*p, q, 2) && !own_pattern(*p, q, 7));
        int first = -1;
        CHECK(own_patterns(*p, q, first) == 1 && first == 2);
        p->tracks[2].pattern = 3; CHECK(own_patterns(*p, q, first) == 0 && first == -1);
        p->tracks[1].pattern = 9; p->tracks[1].in_scene[3] = 0; CHECK(!own_pattern(*p, q, 1)); // it keeps nothing: plays on
        p->tracks[0].off = true; CHECK(own_pattern(*p, q, 0));                                   // off where it keeps P4
        p->tracks[0].in_scene[3] = scene_off; CHECK(!own_pattern(*p, q, 0));                      // kept off
        p->tracks[0].off = false; CHECK(own_pattern(*p, q, 0));                                   // playing where it keeps off
        p->tracks[0].in_scene[3] = 4; p->tracks[1].in_scene[3] = 4; p->tracks[1].pattern = 3;
        // The length: the selected track's kept pattern, else the longest kept with notes, else the fallback.
        for (unsigned t = 0; t < 3; ++t) p->tracks[t].in_scene[4] = 5; // SEQ 5 keeps P5 on each
        CHECK(sequence_length(*p, *b, 0, 4, 1920) == 1920);
        b->at[1][4].length = 960; b->at[1][4].add({0, 60, 100, 120});
        b->at[2][4].length = 3840; b->at[2][4].add({0, 62, 100, 120});
        CHECK(!b->at[1][4].empty() && sequence_length(*p, *b, 0, 4, 1920) == 3840);
        b->at[0][4].length = 480; b->at[0][4].add({0, 64, 100, 120});
        CHECK(sequence_length(*p, *b, 0, 4, 1920) == 480);
        p->tracks[2] = Track {}; b->at[0][4] = Pattern {}; // an unused track's pattern does not count
        CHECK(sequence_length(*p, *b, 0, 4, 1920) == 960);
    }
    // Basic and Full (shell/features.h, Settings::full_features): what Basic leaves out, and who starts on it.
    {
        Settings fresh; CHECK(fresh.full_features); // the code's own start: Full (tests, previews)
        fresh.read(""); CHECK(fresh.full_features);                        // no settings rows (a first install, or a file with only last=): Full
        Settings before; before.read("pad_response=2\nscreen=80\n"); CHECK(before.full_features); // a file from before the row: Full
        Settings basic; basic.read("features=0\npad_response=2\n"); CHECK(!basic.full_features);
        Settings back; back.read(basic.text()); CHECK(!back.full_features); // written and read back
        basic.reset_factory(); CHECK(!basic.full_features);                 // Reset Factory Settings keeps it
        { Settings rowless = basic; rowless.read(""); CHECK(rowless.full_features); } // no row read: Full, whatever came before
        UiState f; f.track = 0; f.page = PageId::Stage;
        CHECK(full_features(f) && menu_tile_shown(f, 7) && !menu_tile_shown(f, 9) && looper_shown(f) && follow_shown(f) && new_pick_count(f) == 5);
        f.settings.full_features = false;
        // GO: MOTION and LOOPER are left out (pad 10 is free since the mixer); the other tiles keep their pads; the
        // wheel passes over them.
        CHECK(!menu_tile_shown(f, 7) && !menu_tile_shown(f, 9) && !menu_tile_shown(f, 11) && menu_tile_shown(f, 8) && menu_tile_shown(f, 6) && menu_tile_shown(f, 15));
        CHECK(menu_tile_turn(f, 6, 1) == 0 && menu_tile_turn(f, 0, -1) == 6); // STEP EDIT, then the bottom row: MOTION is passed over
        CHECK(menu_tile_turn(f, 8, 1) == 10 && menu_tile_turn(f, 10, 1) == 4 && menu_tile_turn(f, 4, -1) == 10); // MIXER, EFFECTS, then the row below
        CHECK(menu_tile_turn(f, 12, -1) == 12 && menu_tile_turn(f, 3, 1) == 3 && menu_tile_turn(f, 12, 15) == 3);
        f.settings.full_features = true; CHECK(menu_tile_turn(f, 6, 1) == 7 && menu_tile_turn(f, 8, 1) == 10); f.settings.full_features = false;
        // HERE: no LOOPER, no FREEZE, no FOLLOW TO ALL.
        std::array<MenuItem, menu_here_max> items {};
        f.page = PageId::Scenes;
        unsigned n = menu_here(*project, f, items);
        CHECK(n == 4);
        for (unsigned k = 0; k < n; ++k) CHECK(items[k].job != MenuJob::SequenceFollowAll);
        f.page = PageId::Mixer; n = menu_here(*project, f, items);
        for (unsigned k = 0; k < n; ++k) CHECK(items[k].job != MenuJob::Looper && items[k].job != MenuJob::Freeze);
        // Seq's MOTION: only for a pattern that holds motion. The loop grid: Full's.
        Pattern plain, moved;
        moved.motion.push_back(MotionPoint {});
        CHECK(!motion_shown(f, plain) && motion_shown(f, moved));
        f.page = PageId::Stage; f.loop_held = true; CHECK(!loop_grid_held(f) && std::string(pad_job(*project, f)).empty());
        f.settings.full_features = true; CHECK(loop_grid_held(f) && std::string(pad_job(*project, f)) == "LOOPS");
        f.page = PageId::Looper; CHECK(!loop_grid_held(f)); f.loop_held = false; f.page = PageId::Stage;
        f.settings.full_features = false;
        // A new track: every type, MIDI and Audio too.
        CHECK(new_pick_count(f) == 5 && NewType(4) == NewType::Audio);
        { UiState pick = f; pick.new_pick = 3; new_pick_turn(pick, 1, 1000); CHECK(pick.new_pick == 4); } // on to Audio, the end
        // Stage: a kit pad has the stock app's seven pages; SENDS goes on any track.
        Track kit; kit.engine = EngineKind::Sampler;
        const auto& g = stage_groups(EngineKind::Sampler);
        StageSkip skip = stage_skip(*project, f, kit);
        CHECK(stage_shown(g, 0, skip) == 3 && stage_shown(g, 1, skip) == 1 && stage_shown(g, 2, skip) == 3);
        UiState full_ui = f; full_ui.settings.full_features = true;
        StageSkip none = stage_skip(*project, full_ui, kit);
        CHECK(stage_shown(g, 0, none) == 4 && stage_shown(g, 1, none) == 4 && stage_shown(g, 2, none) == 5);
        UiState st = f; st.track = 0;
        stage_press(st, kit, 0, skip); CHECK(stage_page_now(st, kit, skip).kind == StageKind::Tune);   // TRIM, then TUNE: LAYERS is passed over
        stage_press(st, kit, 0, skip); CHECK(stage_page_now(st, kit, skip).kind == StageKind::AmpEnv);
        stage_press(st, kit, 0, skip); CHECK(stage_page_now(st, kit, skip).kind == StageKind::Trim);
        stage_press(st, kit, 2, skip); CHECK(stage_page_now(st, kit, skip).kind == StageKind::PadMix);
        stage_press(st, kit, 2, skip); CHECK(stage_page_now(st, kit, skip).kind == StageKind::Filter); // MIX, then FILTER: SENDS is passed over
        stage_press(st, kit, 2, skip); CHECK(stage_page_now(st, kit, skip).kind == StageKind::FiltEnv);
        stage_press(st, kit, 2, skip); CHECK(stage_page_now(st, kit, skip).kind == StageKind::PadMix); // KIT FILT too
        stage_go(st, 1, 3); CHECK(stage_page_now(st, kit, none).kind == StageKind::Cloud && stage_page_now(st, kit, skip).kind == StageKind::Play); // left on CLOUD in Full: PLAY in Basic
        // The cards: Settings' K1 FEATURES and K2 USAGE LOG on the doors; Sequences has the pattern's length, Q and swing.
        Pattern pat;
        UiState b = f; b.page = PageId::SettingsPage;
        auto cards = base_bindings(*project, b, pat);
        CHECK(cards[0].kind == Binding::Kind::Features && cards[0].value == 0);
        CHECK(cards[1].kind == Binding::Kind::UsageLog && cards[1].value == 0 && cards[1].max == 1 && !cards[2].bound());
        // Behind a door K1 is the row under the cursor, its values as a list; Palette's cards take the wheel instead.
        b.settings_in = true; b.settings_cursor = Settings::PadLights; b.settings.pad_lights = 1;
        cards = base_bindings(*project, b, pat);
        CHECK(cards[0].kind == Binding::Kind::SettingRow && cards[0].id == Settings::PadLights && cards[0].max == 2 && cards[0].value == 1 && !cards[1].bound());
        b.settings_wait_row = int8_t(Settings::SampleRate); b.settings_wait_value = 2; b.settings_cursor = Settings::SampleRate;
        CHECK(base_bindings(*project, b, pat)[0].value == 2); // a value waiting for K1 to rest
        b.settings_wait_row = -1;
        b.settings_cursor = Settings::Palette; b.settings_look = true; CHECK(!base_bindings(*project, b, pat)[0].bound());
        b.settings_look = false; b.settings_in = false;
        // MIDI Config: K1 the row; MIDI Panic and Reset have none.
        b.page = PageId::MidiConfig; b.midi_cursor = OutChannelRow; b.settings.midi.out_channel = 10;
        cards = base_bindings(*project, b, pat);
        CHECK(cards[0].kind == Binding::Kind::MidiRow && cards[0].max == 15 && cards[0].value == 9);
        b.midi_cursor = PanicRow; CHECK(!base_bindings(*project, b, pat)[0].bound());
        b.midi_cursor = ResetRow; CHECK(!base_bindings(*project, b, pat)[0].bound());
        b.page = PageId::SettingsPage;
        b.settings_in = false; b.page = PageId::Scenes;
        CHECK(base_bindings(*project, b, pat)[0].kind == Binding::Kind::PatternLength);
        b.settings.full_features = true;
        CHECK(base_bindings(*project, b, pat)[0].kind == Binding::Kind::FollowAction || !base_bindings(*project, b, pat)[0].bound());
    }
    // Lessons (shell/lesson.h): one control a step; any other press does nothing; the step's light blinks.
    {
        using B = SurfaceButton;
        UiState u; u.page = PageId::Stage;
        CHECK(!lesson_step(u) && !lesson_finished(u) && !lesson_takes_pad(u, 0));
        for (unsigned n = 0; n < lesson_count; ++n) { // every lesson: a name, a first step that waits for the kit, words for every step
            const Lesson& l = lesson_at(n);
            CHECK(l.name[0] && l.count >= 5 && l.steps[0].kind == LessonKind::Wait);
            for (unsigned k = 0; k < l.count; ++k) CHECK(l.steps[k].words[0] && std::strlen(l.steps[k].words) <= 16);
        }
        // First beat: the kit loads, pad 1, pad 2, Seq Record, Play ...
        u.lesson = 1; u.lesson_step = 0;
        CHECK(lesson_step(u)->kind == LessonKind::Wait && !lesson_takes_pad(u, 0) && !lesson_takes_button(u, B::Play));
        CHECK(lesson_takes_button(u, B::WheelPush)); // the encoder always: held, the MENU, the way out
        lesson_next(u, 100);
        CHECK(lesson_takes_pad(u, 0) && !lesson_takes_pad(u, 1) && !lesson_takes_button(u, B::Play) && !lesson_takes_knob(u, 0) && !lesson_takes_wheel(u));
        CHECK(lesson_lights_pad(u, 0) && !lesson_lights_pad(u, 1) && !lesson_lights_button(u, B::Play));
        lesson_pad(u, 3, 200); CHECK(u.lesson_step == 1); // another pad: nothing
        lesson_pad(u, 0, 200); CHECK(u.lesson_step == 2 && lesson_takes_pad(u, 1));
        lesson_pad(u, 1, 300); CHECK(u.lesson_step == 3 && lesson_takes_button(u, B::SeqRecord) && lesson_lights_button(u, B::SeqRecord));
        lesson_button(u, B::Play, true, 400); CHECK(u.lesson_step == 3);
        lesson_button(u, B::SeqRecord, true, 400); CHECK(u.lesson_step == 4 && lesson_takes_button(u, B::Play));
        u.menu = menu_go_view; // the MENU over it: B1 and pad 16 (LEAVE) only
        CHECK(!lesson_takes_button(u, B::Play) && lesson_takes_button(u, B::TopLeft) && lesson_takes_pad(u, 15) && !lesson_takes_pad(u, 0));
        u.menu = menu_closed;
        lesson_button(u, B::Play, true, 450); // Play: then the count-in, where nothing acts
        CHECK(u.lesson_step == 5 && lesson_step(u)->kind == LessonKind::Count && !lesson_takes_pad(u, 0) && !lesson_takes_button(u, B::Stop) && !lesson_lights_pad(u, 0));
        lesson_next(u, 460); CHECK(lesson_takes_pad(u, 0));
        u.lesson_step = lesson_at(0).count; CHECK(lesson_finished(u) && !lesson_step(u) && !lesson_takes_button(u, B::Stop));
        // Sequences: Seq opens its page (the step is done once the page is open); Seq held + pad 2.
        u.lesson = 2; u.lesson_step = 2; u.page = PageId::Stage;
        CHECK(lesson_step(u)->page == PageId::Scenes && lesson_takes_button(u, B::Seq));
        lesson_button(u, B::Seq, true, 500); lesson_page(u, 500); CHECK(u.lesson_step == 2); // pressed, the page not open yet
        u.page = PageId::Scenes; lesson_page(u, 600); CHECK(u.lesson_step == 3);
        u.lesson_step = 6; u.page = PageId::Stage;
        CHECK(lesson_step(u)->with == int8_t(B::Seq) && lesson_takes_button(u, B::Seq) && !lesson_takes_pad(u, 1)); // not before Seq is held
        CHECK(lesson_lights_button(u, B::Seq) && lesson_lights_pad(u, 1));
        lesson_button(u, B::Seq, true, 700); CHECK(u.lesson_step == 6 && u.lesson_with == int8_t(B::Seq) && lesson_takes_pad(u, 1));
        lesson_pad(u, 1, 800); CHECK(u.lesson_step == 7);
        lesson_button(u, B::Seq, false, 900); CHECK(u.lesson_with == -1);
        // Mix: Pad FX held + Knob FX; a knob's step is done once it has moved; B1-B3 and the knob cards pulse on screen.
        u.lesson = 3; u.lesson_step = 1; u.page = PageId::Stage;
        CHECK(lesson_takes_button(u, B::PadFx) && !lesson_takes_button(u, B::KnobFx));
        lesson_button(u, B::PadFx, true, 1000); CHECK(lesson_takes_button(u, B::KnobFx));
        lesson_button(u, B::KnobFx, true, 1000); CHECK(u.lesson_step == 1); // its page ends the step
        u.page = PageId::Mixer; lesson_page(u, 1100); CHECK(u.lesson_step == 2 && lesson_knob_card(u) == 0 && lesson_soft_key(u) == -1);
        CHECK(lesson_takes_knob(u, 0) && !lesson_takes_knob(u, 1));
        lesson_knob(u, 0, 60, 1200); lesson_knob(u, 0, 64, 1200); CHECK(u.lesson_step == 2); // a nudge is not a turn
        lesson_knob(u, 0, 60 + lesson_knob_travel, 1300); CHECK(u.lesson_step == 3 && lesson_takes_pad(u, 1) && lesson_knob_card(u) == -1);
        u.lesson_step = 5; CHECK(lesson_soft_key(u) == 0 && lesson_knob_card(u) == -1); // PRESS B1: back from the effects
        // The settings keep the lessons done; a lesson shows Basic's pages.
        Settings done; done.lessons_done = 5;
        Settings read_back; read_back.read(done.text()); CHECK(read_back.lessons_done == 5);
        done.reset_factory(); CHECK(done.lessons_done == 5);
        UiState full; CHECK(full_features(full)); full.lesson = 1; CHECK(!full_features(full) && new_pick_count(full) == 5);
        // GO's pad 16 is LEARN.
        CHECK(std::string(menu_places()[15].name) == "LEARN" && menu_tile_shown(full, 15));
    }
    // 16 Levels FILE's folders (issue 65, shell/progression_tree.h): each file's place, a folder's rows (its folders
    // by name first, then its files in the list's order), and the way up.
    {
        const std::string card = "/media/card";
        const std::vector<std::string> paths {card + "/Melodee/Progressions/Mine 1.progression", card + "/Melodee/Progressions/jazz/ii-V-I.progression",
                                              card + "/Melodee/Progressions/Hip Hop/Boom.progression", card + "/Melodee/Progressions/Hip Hop/Dusty/Lo.progression",
                                              card + "/Progressions/Top.progression", card + "/Melodee/Samples/Expansions/Organs/Progressions/Gospel.progression",
                                              card + "/Samples/Expansions/Old/Blue.progression"};
        std::vector<std::string> places;
        for (const auto& p : paths) places.push_back(prog_place(card, p));
        CHECK(places[0] == "Mine 1.progression" && places[1] == "jazz/ii-V-I.progression" && places[3] == "Hip Hop/Dusty/Lo.progression");
        CHECK(places[4] == "Progressions/Top.progression" && places[5] == "Expansions/Organs/Progressions/Gospel.progression" && places[6] == "Expansions/Old/Blue.progression");
        auto top = prog_folder_rows(places, "");
        CHECK(top.size() == 5 && top[0].name == "Expansions" && top[0].file < 0 && top[1].name == "Hip Hop" && top[2].name == "jazz" &&
              top[3].name == "Progressions" && top[4].name == "Mine 1" && top[4].file == 0);
        auto hip = prog_folder_rows(places, "Hip Hop");
        CHECK(hip.size() == 2 && hip[0].name == "Dusty" && hip[0].file < 0 && hip[1].name == "Boom" && hip[1].file == 2);
        CHECK(prog_folder_rows(places, "Hip Hop/Dusty").size() == 1 && prog_folder_rows(places, "Hip Hop/Dusty")[0].file == 3);
        CHECK(prog_folder_rows(places, "Expansions").size() == 2 && prog_folder_rows(places, "Hip").empty()); // a name's start is not a folder
        CHECK(prog_place_folder(places[3]) == "Hip Hop/Dusty" && prog_place_folder(places[0]).empty());
        CHECK(prog_folder_up("Hip Hop/Dusty") == "Hip Hop" && prog_folder_up("Hip Hop").empty() && prog_folder_name("Expansions/Organs") == "Organs");
        CHECK(prog_place(card, "/elsewhere/Loose.progression") == "Loose.progression");
    }
    // 16 Levels held's keys (shell/levels_keys.h): SCALE and CHORDS by the type; on Chords FILE and SET; SET's chords.
    {
        Track kit; kit.engine = EngineKind::Sampler;
        auto keys = levels_keys(kit);
        CHECK(keys.key[0] == LevelsKey::Scale && keys.key[1] == LevelsKey::Chords && keys.key[2] == LevelsKey::None && keys.lit == -1);
        kit.levels = true; kit.levels_kind = uint8_t(LevelsKind::Scale);
        CHECK(levels_keys(kit).lit == 0 && !levels_chords_on(kit)); // Scale in use: its key lit
        kit.levels_kind = uint8_t(LevelsKind::Chords);
        keys = levels_keys(kit);
        CHECK(keys.key[0] == LevelsKey::Scale && keys.key[1] == LevelsKey::File && keys.key[2] == LevelsKey::Set && keys.lit == -1 && levels_chords_on(kit));
        CHECK(std::string(levels_key_name(keys.key[1])) == "FILE" && std::string(levels_key_name(keys.key[2])) == "SET");
        kit.levels = false; CHECK(!levels_chords_on(kit) && levels_keys(kit).key[1] == LevelsKey::File); // the type stays while off
        CHECK(levels_keys(Track {}).key[0] == LevelsKey::None); // an empty track: no keys
        Track audio; audio.engine = EngineKind::Audio; CHECK(levels_keys(audio).key[0] == LevelsKey::None);
        // The pads chip says CHORDS.
        UiState j; j.track = 0; j.page = PageId::Stage;
        auto with = std::make_unique<Project>(*project);
        with->tracks[0].levels = true; with->tracks[0].levels_kind = uint8_t(LevelsKind::Chords);
        CHECK(std::string(pad_job(*with, j)) == "CHORDS");
        with->tracks[0].levels_kind = uint8_t(LevelsKind::Scale); CHECK(std::string(pad_job(*with, j)) == "LEVELS");
        // FILE's rows: the row in hand two from the top where the list allows.
        CHECK(prog_rows_first(0, 3) == 0 && prog_rows_first(2, 20) == 0 && prog_rows_first(5, 20) == 3 && prog_rows_first(19, 20) == 14 && prog_rows_first(4, 6) == 0);
        // SET: D minor 7 as the cards give it, named as progression files name chords; inverted, a slash bass.
        ProgChord dm7 = build_prog_chord(2, 4, 0);
        CHECK(dm7.count == 4 && dm7.notes[0] == 50 && dm7.notes[1] == 53 && dm7.notes[2] == 57 && dm7.notes[3] == 60 && std::string(dm7.name) == "Dmin7");
        CHECK(pad_chord_name(dm7.name).chord == "Dm7" && pad_chord_name(dm7.name).bass.empty());
        ProgChord first = build_prog_chord(2, 4, 1);
        CHECK(first.notes[0] == 53 && first.notes[3] == 62 && std::string(first.name) == "Dmin7/F" && pad_chord_name(first.name).chord == "Dm7" && pad_chord_name(first.name).bass == "F");
        CHECK(build_prog_chord(0, 0, 0).count == 3 && std::string(build_prog_chord(0, 0, 0).name) == "CMaj" && pad_chord_name("CMaj").chord == "C");
        CHECK(build_prog_chord(11, 11, 3).count == 6 && build_prog_chord(11, 11, 9).notes[0] > 48); // an inversion past its notes stops one short
        for (unsigned root = 0; root < 12; ++root) // every chord of the table in root position is found again as itself
            for (unsigned type = 0; type < built_chord_count; ++type) {
                BuiltPick pick;
                CHECK(find_built_chord(build_prog_chord(root, type, 0), pick) && pick.root == root && pick.type == type && pick.inversion == 0);
                CHECK(std::strlen(build_prog_chord(root, type, 3).name) < prog_chord_name);
            }
        BuiltPick none;
        ProgChord odd; odd.count = 3; odd.notes = {60, 61, 62};
        CHECK(!find_built_chord(odd, none) && !find_built_chord(ProgChord {}, none));
        // The cards: SET's chord on K1-K3 over any page, and nothing under FILE's list.
        Pattern pat;
        UiState b; b.track = 0; b.page = PageId::Stage; b.levels_sheet = levels_sheet_chord; b.chord_root = 2; b.chord_type = 4; b.chord_invert = 1;
        auto cards = base_bindings(*project, b, pat);
        CHECK(cards[0].kind == Binding::Kind::BuildRoot && cards[0].value == 2 && cards[1].kind == Binding::Kind::BuildChord && cards[1].value == 4 && cards[2].kind == Binding::Kind::BuildInvert && cards[2].value == 1);
        b.levels_sheet = levels_sheet_files; CHECK(!base_bindings(*project, b, pat)[0].bound());
        // The MENU's HERE on Stage: CHORDS and CHORD FILE first.
        std::array<MenuItem, menu_here_max> items {};
        UiState m; m.track = 0; m.page = PageId::Stage;
        unsigned count = menu_here(*with, m, items);
        CHECK(count >= 4 && items[0].job == MenuJob::Chords && items[1].job == MenuJob::ChordFile);
        m.page = PageId::Mixer; menu_here(*with, m, items); CHECK(items[0].job == MenuJob::TrackMove);
    }
    // The MENU (shell/menu.h): the encoder held opens it, a tap stays the page's Push; GO's tiles and HERE's cards.
    {
        EncoderHold e;
        encoder_press(e, 1000);
        CHECK(!encoder_tick(e, 1000 + menu_hold_ms - 1) && encoder_release(e)); // let go soon: a tap
        encoder_press(e, 2000);
        CHECK(encoder_tick(e, 2000 + menu_hold_ms) && !encoder_tick(e, 9000)); // held: the MENU, once
        CHECK(!encoder_release(e) && !encoder_release(e)); // its release is no tap
        CHECK(menu_tile_of(PageId::Mixer) == 8 && menu_tile_of(PageId::Scenes) == 4 && menu_tile_of(PageId::Master) == 10 && menu_tile_of(PageId::Edit) == 0);
        CHECK(menu_tile_of(PageId::Patterns) == 4 && menu_places()[4].page == PageId::Scenes && std::string(menu_places()[4].name) == "SEQUENCE"); // Seq's home
        CHECK(menu_tile_turn(12, 1) == 13 && menu_tile_turn(15, 1) == 8 && menu_tile_turn(8, -1) == 15); // across a row, then the row below
        CHECK(menu_tile_turn(12, -1) == 12 && menu_tile_turn(3, 1) == 3 && menu_tile_turn(12, 15) == 3); // clamped at both ends
        std::array<MenuItem, menu_here_max> items {};
        UiState m; m.track = 0;
        m.page = PageId::Scenes;
        CHECK(menu_here(*project, m, items) == 5 && items[0].job == MenuJob::SequenceCapture && items[4].danger);
        m.page = PageId::Mixer;
        unsigned count = menu_here(*project, m, items);
        CHECK(count >= 4 && items[0].job == MenuJob::TrackMove && items[count - 1].job == MenuJob::TrackClear);
        m.page = PageId::Tempo;
        CHECK(menu_here(*project, m, items) == 0 && std::string(menu_page_name(PageId::Tempo)).empty());
    }
    // Track colour (issue 69, shell/track_colours.h): its place's until one is picked; kept in TCOL (none written while
    // every track is in its place's); the screen, the pads and the Live Set export follow; the MENU offers it on the mixer.
    {
        auto coloured = std::make_unique<Project>();
        coloured->tracks[3].engine = EngineKind::Sampler;
        CHECK(track_colour_slot(coloured->tracks[3], 3) == 3 && track_colour_slot(coloured->tracks[9], 9) == 9);
        auto bank = std::make_unique<PatternBank>();
        auto bytes = save_project(*coloured, *bank);
        auto has_tcol = [](const std::vector<uint8_t>& b) {
            for (size_t n = 0; n + 4 <= b.size(); ++n) if (std::memcmp(&b[n], "TCOL", 4) == 0) return true;
            return false;
        };
        CHECK(!has_tcol(bytes));
        coloured->tracks[3].colour = 5; // Sky
        CHECK(track_colour_slot(coloured->tracks[3], 3) == 4);
        bytes = save_project(*coloured, *bank);
        CHECK(has_tcol(bytes));
        auto back = std::make_unique<Project>();
        auto back_bank = std::make_unique<PatternBank>();
        CHECK(load_project(bytes, *back, *back_bank).empty() && back->tracks[3].colour == 5 && back->tracks[4].colour == 0);
        show_track_colours(*back);
        CHECK(ui::track_slot(3) == 4 && ui::track_slot(4) == 4 && ui::track_slot(0) == 0); // two tracks in Sky now
        CHECK(pad_light::track(3, 1.0f).colour == pad_light::colour_slot(4, 1.0f).colour && pad_light::track(0, 1.0f).colour != pad_light::colour_slot(4, 1.0f).colour);
        auto live = live_track_colours(*back);
        CHECK(live[3] == ui::track_live_colours[4] && live[0] == ui::track_live_colours[0]);
        CHECK(colour_slot_step(15, 1) == 0 && colour_slot_step(0, -1) == 15 && colour_slot_step(4, 3) == 7);
        show_track_colours(Project {}); // back as the other tests expect
        CHECK(ui::track_slot(3) == 3);
        std::array<MenuItem, menu_here_max> items {};
        UiState m; m.track = 3; m.page = PageId::Mixer;
        unsigned count = menu_here(*coloured, m, items);
        bool offered = false;
        for (unsigned n = 0; n < count; ++n) offered |= items[n].job == MenuJob::TrackColour;
        CHECK(offered);
        m.track = 7; count = menu_here(*coloured, m, items); // an empty track: nothing to colour
        for (unsigned n = 0; n < count; ++n) CHECK(items[n].job != MenuJob::TrackColour);
        m.colour_sheet = true; CHECK(pad_context(m, coloured->tracks[3], PadFacts {}, 0) == PadContext::ColourPick);
        m.ask = UiState::Ask::ClearTrack; CHECK(pad_context(m, coloured->tracks[3], PadFacts {}, 0) == PadContext::Sheet);
    }
    // An audio track's take on Song (shell/song_take.h): the first empty pattern no clip plays; the clip reaches the bar
    // after the one playing, what was there giving way; closed on the last bar heard.
    {
        Song song;
        auto bank = std::make_unique<PatternBank>();
        bank->at[3][0].events.push_back({}); // P1 holds a note
        song.place({3, 1, 8, 2, false});      // P2 empty, but on the lane
        CHECK(song_take_pattern(song, *bank, 3) == 2);
        song.place({3, 5, 2, 4, false});      // T4 bars 3-6, P6
        song.place({3, 2, 1, 1, false});      // the take's clip at bar 2, P3
        CHECK(song_take_pattern(song, *bank, 3) == 3);
        CHECK(!song_take_grow(song, 3, 2, 1, 0)); // the count before it: it holds bar 2 already
        CHECK(song_take_grow(song, 3, 2, 1, 1) && song.clips[size_t(song_take_clip(song, 3, 2, 1))].bars == 2);
        const int p6 = song.clip_at(3, 3);
        CHECK(p6 >= 0 && song.clips[size_t(p6)].bar == 3 && song.clips[size_t(p6)].bars == 3); // gave way a bar
        CHECK(song_take_grow(song, 3, 2, 1, 2) && song.clips[size_t(song_take_clip(song, 3, 2, 1))].end() == 4);
        CHECK(!song_take_grow(song, 3, 2, 1, 2)); // once a bar
        CHECK(song_take_close(song, 3, 2, 1, 2) == 2 && song.clips[size_t(song_take_clip(song, 3, 2, 1))].end() == 3);
        CHECK(song.clip_at(3, 3) < 0 && song.clip_at(3, 4) >= 0 && song.clips[size_t(song.clip_at(3, 4))].pattern == 5); // what gave way stays cut
        CHECK(song_take_close(song, 3, 2, 1, 0) == 1); // never under a bar
        CHECK(song_take_close(song, 3, 7, 1, 4) == 0); // gone
        Song long_one;
        long_one.place({0, 0, 10, 1, false});
        for (unsigned bar = 10; bar < 10 + max_bars + 4; ++bar) song_take_grow(long_one, 0, 0, 10, bar);
        CHECK(long_one.clips[0].bars == max_bars);
        Song looped; // the loop ahead: the clip stops at its end, the clip after it keeps its first bar
        looped.loop_start = 0; looped.loop_end = 4;
        looped.place({1, 0, 2, 1, false});
        looped.place({1, 1, 4, 2, false});
        CHECK(song_take_grow(looped, 1, 0, 2, 2) && looped.clips[size_t(song_take_clip(looped, 1, 0, 2))].end() == 4);
        CHECK(!song_take_grow(looped, 1, 0, 2, 3) && looped.clip_at(1, 4) >= 0 && looped.clips[size_t(looped.clip_at(1, 4))].pattern == 1);
    }
    // Song (issue 60, shell/song_range.h): the loop range copied, pasted as an insert and taken out, on one lane or
    // every lane with the markers and tempo points; K2's list of the patterns that hold notes and the next empty.
    {
        Song song;
        song.place({0, 0, 0, 4, false});  // T1 bars 1-4
        song.place({0, 1, 4, 4, false});  // T1 bars 5-8
        song.place({1, 2, 2, 4, false});  // T2 bars 3-6
        song.step_marker(4);
        song.tempo.set(6, 1300, false);
        auto section = song_copy_range(song, 2, 6, song_lanes(false, 0)); // T1's bars 3-6
        CHECK(section.bars == 4 && section.clips.size() == 2 && section.clips[0].bar == 0 && section.clips[0].bars == 2 &&
              section.clips[1].bar == 2 && section.clips[1].bars == 2 && section.clips[1].pattern == 1);
        auto every = song_copy_range(song, 2, 6, song_all_lanes);
        CHECK(every.clips.size() == 3 && every.lanes == song_all_lanes);
        Song one = song;
        CHECK(song_paste(one, section, 2)); // T1 alone: what is at bar 3 on moves 4 bars on, cut where the cursor is
        CHECK(one.clip_at(0, 1) >= 0 && one.clips[size_t(one.clip_at(0, 1))].bars == 2);              // bars 1-2 stay
        CHECK(one.clip_at(0, 2) >= 0 && one.clips[size_t(one.clip_at(0, 2))].pattern == 0);           // the paste's first part
        CHECK(one.clip_at(0, 4) >= 0 && one.clips[size_t(one.clip_at(0, 4))].pattern == 1);           // its second
        CHECK(one.clip_at(0, 6) >= 0 && one.clips[size_t(one.clip_at(0, 6))].pattern == 0 && one.length() == 12); // the tail of the first clip moved on
        CHECK(one.clip_at(1, 2) >= 0 && one.clips[size_t(one.clip_at(1, 2))].bar == 2);               // T2 not moved
        CHECK(one.markers[0].bar == 4 && one.tempo.points[0].bar == 6);                              // one lane: markers stay
        Song all = song;
        CHECK(song_paste(all, every, 2));
        CHECK(all.clip_at(1, 6) >= 0 && all.clips[size_t(all.clip_at(1, 6))].bar == 6 && all.markers[0].bar == 8 && all.tempo.points[0].bar == 10);
        Song out = song;
        CHECK(song_remove_bars(out, 2, 6, song_all_lanes)); // bars 3-6 out of every lane
        CHECK(out.length() == 4 && out.clip_at(0, 0) >= 0 && out.clips[size_t(out.clip_at(0, 0))].bars == 2);
        CHECK(out.clip_at(0, 2) >= 0 && out.clips[size_t(out.clip_at(0, 2))].pattern == 1 && out.clips[size_t(out.clip_at(0, 2))].bars == 2);
        CHECK(out.clip_at(1, 2) < 0 && out.marker_count == 0 && out.tempo.points[0].bar == 2);
        Song full;
        for (unsigned n = 0; n < song_clips; ++n) full.place({uint8_t(n % 16), 0, uint16_t(n / 16 * 2), 2, false});
        CHECK(!song_paste(full, every, 1)); // no room: nothing done
        std::array<uint64_t, 2> used {};
        used[0] = (1u << 0) | (1u << 3); used[1] = uint64_t(1) << (70 - 64);
        auto list = song_pattern_list(used, 0);
        CHECK((list == std::vector<uint8_t> {0, 3, 70, 71}) && song_pattern_index(list, 70) == 2);
        list = song_pattern_list(used, 20); // a clip on an empty pattern keeps it in the list
        CHECK((list == std::vector<uint8_t> {0, 3, 20, 70, 71}) && song_pattern_index(list, 20) == 2);
        CHECK((song_pattern_list({}, 0) == std::vector<uint8_t> {0}));
        CHECK(print_pattern_name(17) == "B2");
        // LOOP: start and end at the cursor over clips or empty time; twice on one bar of a clip, its bars; then off.
        Song looped = song;
        int16_t waiting = -1;
        CHECK(song_loop_tap(looped, waiting, 1, looped.clip_at(0, 1)) == SongLoopTap::Start && waiting == 1 && !looped.looping()); // on a clip: a start
        CHECK(song_loop_tap(looped, waiting, 5, looped.clip_at(0, 5)) == SongLoopTap::Set && looped.loop_start == 1 && looped.loop_end == 6 && waiting == -1);
        CHECK(song_loop_tap(looped, waiting, 3, -1) == SongLoopTap::Off && !looped.looping());
        CHECK(song_loop_tap(looped, waiting, 6, looped.clip_at(0, 6)) == SongLoopTap::Start);
        CHECK(song_loop_tap(looped, waiting, 6, looped.clip_at(0, 6)) == SongLoopTap::Set && looped.loop_start == 4 && looped.loop_end == 8); // the clip's bars
        CHECK(song_loop_tap(looped, waiting, 9, -1) == SongLoopTap::Off);
        CHECK(song_loop_tap(looped, waiting, 9, -1) == SongLoopTap::Start && song_loop_tap(looped, waiting, 7, -1) == SongLoopTap::Set &&
              looped.loop_start == 7 && looped.loop_end == 10); // either way round
        auto long_song = std::make_unique<Project>(); // a clip longer than a pattern (an MPC song's) keeps its length
        long_song->song.place({0, 0, 0, 200, true});
        auto bank = std::make_unique<PatternBank>();
        auto back = std::make_unique<Project>();
        CHECK(load_project(save_project(*long_song, *bank), *back, *bank).empty() && back->song.clips[0].bars == 200 && back->song.clips[0].muted);
    }
    // Stems (shell/stems_jobs.cpp): a kit pad that holds a sample offers STEMS in the MENU on Stage and on Shift + B3
    // of its TRIM, in Basic as in Full; an empty pad and an instrument do not.
    {
        auto kit = std::make_unique<Project>();
        auto& track = kit->tracks[0];
        track.engine = EngineKind::Sampler;
        track.last_pad = 0;
        std::array<MenuItem, menu_here_max> items {};
        UiState m; m.track = 0; m.page = PageId::Stage;
        auto offered = [&] {
            unsigned count = menu_here(*kit, m, items);
            for (unsigned n = 0; n < count; ++n) if (items[n].job == MenuJob::Stems) return true;
            return false;
        };
        CHECK(stage_page_now(m, track, StageSkip {}).kind == StageKind::Trim); // Stage opens on a kit pad's TRIM
        CHECK(!offered() && !button_has_shift_job(*kit, m, SurfaceButton::TopRight, true)); // nothing on the pad
        track.pads[0].sample = 0;
        std::snprintf(kit->samples.edit(0).path, sizeof(SampleRef::path), "/card/Samples/LOOP.wav");
        kit->samples.edit(0).frames = 4 * 44100;
        CHECK(offered() && button_has_shift_job(*kit, m, SurfaceButton::TopRight, true));
        m.page = PageId::Mixer; CHECK(!offered()); // the pad's job: on Stage alone
        m.page = PageId::Stage;
        m.settings.full_features = false; CHECK(offered() && button_has_shift_job(*kit, m, SurfaceButton::TopRight, true)); // Basic too
        m.settings.full_features = true;
        std::snprintf(kit->samples.edit(0).path, sizeof(SampleRef::path), "/card/Samples/PIANO.sfz");
        CHECK(!offered() && !button_has_shift_job(*kit, m, SurfaceButton::TopRight, true)); // an instrument: nothing to separate
        // A job's room (shell/stems_room.h): four empty pads after the pad. How many samples the project holds is
        // no part of it: 61, three short of the 64 a project once held, leave room.
        auto room = stems_room(*kit, 0, 0);
        CHECK(room.pads == 4 && room.enough());
        for (unsigned n = 1; n < 61; ++n) std::snprintf(kit->samples.edit(n).path, sizeof(SampleRef::path), "/card/Samples/S%u.wav", n);
        CHECK(stems_room(*kit, 0, 0).enough());
        for (unsigned n = 1; n < sampler_pads - 3; ++n) track.pads[n].sample = 0;
        room = stems_room(*kit, 0, 0);
        CHECK(room.pads == 3 && !room.enough()); // three empty pads left on the track
        for (unsigned n = 1; n < sampler_pads; ++n) track.pads[n] = PadSound {};
        CHECK(stem_count == 4 && std::string(stem_name(0)) == "DRUMS" && std::string(stem_name(1)) == "BASS" && std::string(stem_name(2)) == "MUSIC" && std::string(stem_name(3)) == "VOCALS");
    }
    // Background save (shell/autosave.h): a changed project, nothing running, the controls quiet for a moment.
    {
        AutosaveFacts f;
        f.dirty = true; f.last_input = 1000; f.now = 1000 + autosave_idle_ms - 1;
        CHECK(!autosave_due(f)); // a hand was on the controls a moment ago
        f.now = 1000 + autosave_idle_ms;
        CHECK(autosave_due(f));
        f.dirty = false; CHECK(!autosave_due(f)); f.dirty = true;
        f.playing = true; CHECK(!autosave_due(f)); f.playing = false;
        f.recording = true; CHECK(!autosave_due(f)); f.recording = false;
        f.sheet = true; CHECK(!autosave_due(f)); f.sheet = false;
        f.loading = true; CHECK(!autosave_due(f)); f.loading = false;
        f.editing = true; CHECK(!autosave_due(f)); f.editing = false;
        f.failed = true; f.last_try = f.now - autosave_retry_ms + 1;
        CHECK(!autosave_due(f)); // no card a moment ago: not again yet
        f.last_try = f.now - autosave_retry_ms;
        CHECK(autosave_due(f));
        f.enabled = false; CHECK(!autosave_due(f)); // Settings > SEQ > Autosave Off (issue 18): only SAVE writes it
    }
    // Issue 18: the Autosave row, On unless the file says Off; Reset Factory Settings turns it back On.
    {
        Settings fresh;
        CHECK(fresh.autosave && fresh.value_text(Settings::Autosave) == "On" && std::string(Settings::row_name(Settings::Autosave)) == "Autosave");
        Settings off = fresh;
        off.step(Settings::Autosave, 1);
        CHECK(!off.autosave && off.value_text(Settings::Autosave) == "Off" && off.choice(Settings::Autosave) == 0);
        Settings back; back.read(off.text());
        CHECK(!back.autosave);
        Settings old; old.read("tap_light=0\n"); // a file from before the row: On
        CHECK(old.autosave);
        off.reset_factory();
        CHECK(off.autosave);
    }
    // Settings > SEQ > Mute (issue 27): Audio unless the file says Notes.
    {
        Settings fresh;
        CHECK(!fresh.note_mutes && fresh.value_text(Settings::MuteKind) == "Audio" && fresh.choice(Settings::MuteKind) == 0 &&
              std::string(Settings::row_name(Settings::MuteKind)) == "Mute" && fresh.choices(Settings::MuteKind) == 2);
        Settings notes = fresh;
        notes.step(Settings::MuteKind, 1);
        CHECK(notes.note_mutes && notes.value_text(Settings::MuteKind) == "Notes");
        Settings back; back.read(notes.text());
        CHECK(back.note_mutes);
        Settings old; old.read("seq_follow=1\n"); // a file from before the row: Audio
        CHECK(!old.note_mutes);
        notes.set_choice(Settings::MuteKind, 0);
        CHECK(!notes.note_mutes);
    }
    // Settings > SEQ > Note preview (shell/note_preview.h): On unless the file says Off; how long a note sounds and
    // which of a list are heard.
    {
        Settings fresh;
        CHECK(fresh.note_preview && fresh.value_text(Settings::NotePreview) == "On" &&
              std::string(Settings::row_name(Settings::NotePreview)) == "Note preview");
        Settings off = fresh;
        off.step(Settings::NotePreview, 1);
        CHECK(!off.note_preview && off.value_text(Settings::NotePreview) == "Off" && off.choice(Settings::NotePreview) == 0);
        Settings back; back.read(off.text());
        CHECK(!back.note_preview);
        Settings old; old.read("seq_follow=1\n"); // a file from before the row: On
        CHECK(old.note_preview && old.seq_follow == 1);
        off.reset_factory();
        CHECK(off.note_preview);

        Event e; // a 16th at 120 BPM: 240 ticks of 125 ms, held a tenth of a step short
        e.length = 240;
        CHECK(preview_ms(e, 240, 1200) == 112);
        e.length = 960 * 16; CHECK(preview_ms(e, 240, 1200) == preview_longest_ms); // four bars: a second
        e.length = 1; CHECK(preview_ms(e, 240, 1200) == preview_shortest_ms);
        e.length = 960; e.ratchet = 4; CHECK(preview_ms(e, 240, 1200) == 112); // a ratchet: its first hit
        e.ratchet = 1; e.length = 240; CHECK(preview_ms(e, 240, 600) == 225); // half the tempo: twice as long

        std::array<Event, preview_notes_max> out {};
        std::vector<Event> notes;
        CHECK(preview_chord(notes, out) == 0);
        auto at = [](uint32_t tick, uint8_t key, uint8_t flags = 0) { Event n; n.tick = tick; n.key = key; n.flags = flags; return n; };
        notes = {at(48, 60), at(24, 64, step_mute), at(48, 67), at(96, 72), at(48, 71, step_mute)};
        unsigned heard = preview_chord(notes, out); // the earliest not muted: the chord on 48, its muted note left out
        CHECK(heard == 2 && out[0].key == 60 && out[1].key == 67);
        notes.clear();
        for (unsigned n = 0; n < 12; ++n) notes.push_back(at(0, uint8_t(48 + n)));
        CHECK(preview_chord(notes, out) == preview_notes_max); // a chord past eight notes: its first eight
        notes = {at(0, 60, step_mute)};
        CHECK(preview_chord(notes, out) == 0); // only muted: nothing
    }
    // Tap Tempo held (shell/tap_hold.h): a turn of the encoder shows the Tempo page until it is let go; held alone,
    // the page opens to stay; held on the page, it closes.
    {
        TapHold tap;
        tap_press(tap, 1000);
        CHECK(tap_tick(tap, 1000 + tap_hold_ms, false) == TapDo::Nothing); // not yet
        CHECK(tap_turn(tap, false) == TapDo::Open && tap_turn(tap, true) == TapDo::Nothing); // the first turn opens it
        CHECK(tap_tick(tap, 5000, true) == TapDo::Nothing); // a turn under way never closes it
        CHECK(tap_release(tap, true) == TapDo::Close && !tap.down);
        tap_press(tap, 2000);
        CHECK(tap_tick(tap, 2000 + tap_hold_ms + 1, false) == TapDo::Open && tap_tick(tap, 9000, true) == TapDo::Nothing);
        CHECK(tap_release(tap, true) == TapDo::Nothing); // opened by holding alone: it stays
        tap_press(tap, 3000);
        CHECK(tap_tick(tap, 3000 + tap_hold_ms + 1, true) == TapDo::Close); // its opener closes it
        CHECK(tap_release(tap, false) == TapDo::Nothing);
        tap_press(tap, 4000);
        CHECK(tap_turn(tap, true) == TapDo::Nothing && tap_release(tap, true) == TapDo::Nothing); // on the page already: it stays
        CHECK(tap_turn(tap, false) == TapDo::Nothing); // not down: the encoder is the page's
    }
    // The mixer and its chains are one stop: Mixer -> Stage, Back lands on the mixer; Mixer -> Rack -> Bus is one stop.
    {
        UiState m; m.page = PageId::Patterns;
        auto to = [&](PageId page) { push_back_stack(m, page); m.page = page; };
        to(PageId::Mixer); to(PageId::Stage);
        CHECK(back_target(m) == PageId::Mixer);
        to(PageId::Mixer); to(PageId::Rack); to(PageId::Bus);
        CHECK(back_target(m) == PageId::Patterns);
    }
    // Seq, the SEQUENCE hub (docs/HUBS_PLAN.md): a tap opens the hub's page last open on the track (Patterns until
    // another is), per track; inside the hub home, Patterns; Step Edit goes back to the page under it.
    {
        UiState q; q.track = 0;
        q.page = PageId::Stage; CHECK(seq_tap_page(q) == PageId::Scenes); // Seq's home: Sequences, every track
        q.page = PageId::Song; CHECK(seq_tap_page(q) == PageId::Scenes);
        remember_editor(q, PageId::Motion);
        q.page = PageId::Stage; CHECK(seq_tap_page(q) == PageId::Motion);
        q.track = 1; CHECK(seq_tap_page(q) == PageId::Scenes); // each track its own
        q.track = 0; remember_editor(q, PageId::Stage); CHECK(seq_editor_page(q) == PageId::Motion); // only the hub's pages count
        q.page = PageId::Motion; CHECK(seq_tap_page(q) == PageId::Scenes); // home
        q.page = PageId::Patterns; CHECK(seq_tap_page(q) == PageId::Scenes && seq_home == PageId::Scenes);
        q.page = PageId::ModPage; CHECK(seq_tap_page(q) == PageId::Scenes && menu_tile_of(PageId::ModPage) == 7); // MOD and MACRO: Motion's next pages, in the hub
        q.page = PageId::StepEdit; CHECK(seq_tap_page(q) == PageId::Motion);
        CHECK(seq_editor(PageId::Roll) && !seq_editor(PageId::StepEdit) && seq_editor(PageId::Patterns));
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
    l.page = PageId::Patterns; // Erase tapped clears the pattern in hand, Shift + Erase copies patterns (shell/erase_copy.h)
    CHECK(button_has_job(*project, l, B::Erase, false, false) && button_has_shift_job(*project, l, B::Erase, false));
    l.page = PageId::Edit; // a synth's sound: Erase + pot only, nothing to copy
    CHECK(!button_has_job(*project, l, B::Erase, false, false) && !button_has_shift_job(*project, l, B::Erase, false));
    l.seq_held = true; // Seq held: Erase + pad clears a pattern, Shift + Erase copies one, on any page
    l.page = PageId::Stage;
    CHECK(button_has_job(*project, l, B::Erase, false, false) && button_has_shift_job(*project, l, B::Erase, false));
    l.seq_held = false;
    l.track = 1;
    CHECK(button_has_job(*project, l, B::Loop, true, false) && !button_has_job(*project, l, B::Chop, true, false)); // the last pad is empty
    project->tracks[1].pads[0].sample = 0;
    CHECK(button_has_job(*project, l, B::Chop, true, false));
    { // a PLUGIN track's B1 pages on Stage light Shift + B1 PLUG-INS, B2 PRESETS (when it has some), B3 TRANSPORT
        auto plug = std::make_unique<Project>();
        plug->tracks[0].engine = EngineKind::Plugin; // none picked yet
        UiState u; u.page = PageId::Stage; u.track = 0;
        CHECK(stage_plugin_page(stage_page_now(u, plug->tracks[0], stage_skip(*plug, u, plug->tracks[0])).kind));
        CHECK(button_has_shift_job(*plug, u, B::TopLeft, false) && !button_has_shift_job(*plug, u, B::TopMiddle, false)
              && !button_has_shift_job(*plug, u, B::TopRight, false));
        plug->tracks[0].plugin = 0;
        CHECK(button_has_shift_job(*plug, u, B::TopRight, false) && !button_has_shift_job(*plug, u, B::TopMiddle, false)); // no presets yet
        u.plugin_panel.entry = 0; u.plugin_panel.programs = 12;
        CHECK(button_has_shift_job(*plug, u, B::TopMiddle, false));
        CHECK(stage_go(u, EngineKind::Plugin, StageKind::PluginEdit) && button_has_shift_job(*plug, u, B::TopLeft, false));
        CHECK(stage_go(u, EngineKind::Plugin, StageKind::Arp)); // B2 ARP: Shift + B1-B3 do nothing there
        CHECK(!button_has_shift_job(*plug, u, B::TopLeft, false) && !button_has_shift_job(*plug, u, B::TopRight, false));
    }
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
    project->tracks[0].repeat = true;
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
    { // Stopped, the first pad held plays at once, not on the next step of a clock that ran on before it (1/8 here)
        auto p = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        tone_kit(*p, 0);
        auto host_held = std::make_unique<EngineHost>(); // on the heap: a test's frame stays under the 8 MB stack
        auto& host = *host_held;
        host.load(*p, *bank);
        auto tone = tone_sample();
        CHECK(host.offer_sample(0, &tone));
        host.send({Command::Type::Repeat, 0, 0, 0, 1});
        host.send({Command::Type::Repeat, 0, 0, 2, 2});
        std::vector<int32_t> out(256);
        for (unsigned n = 0; n < 37; ++n) host.render(out.data(), 128, 1);
        host.send({Command::Type::RepeatKey, 0, 0, uint16_t(sampler_first_note), 100});
        host.render(out.data(), 128, 1);
        float peak = 0;
        for (auto s : out) peak = std::max(peak, std::fabs(s / 2147483648.0f));
        CHECK(peak > 0.01f);
    }
    { // Two pads held under Note Repeat sound together on each step, not one after the other (the arp's way)
        auto steps = [](bool both) {
            auto p = std::make_unique<Project>();
            auto bank = std::make_unique<PatternBank>();
            tone_kit(*p, 0);
            p->tracks[0].pads[1] = p->tracks[0].pads[0];
            auto host_held = std::make_unique<EngineHost>(); // on the heap: a test's frame stays under the 8 MB stack
            auto& host = *host_held;
            host.load(*p, *bank);
            auto tone = tone_sample();
            host.offer_sample(0, &tone);
            host.send({Command::Type::Repeat, 0, 0, 0, 1});
            std::vector<int32_t> out(256);
            for (unsigned n = 0; n < 37; ++n) host.render(out.data(), 128, 1);
            host.send({Command::Type::RepeatKey, 0, 0, uint16_t(sampler_first_note), 100});
            if (both) host.send({Command::Type::RepeatKey, 0, 0, uint16_t(sampler_first_note + 1), 100});
            std::vector<float> peaks; // each block's
            for (unsigned n = 0; n < 200; ++n) {
                host.render(out.data(), 128, 1);
                float peak = 0;
                for (auto s : out) peak = std::max(peak, std::fabs(s / 2147483648.0f));
                peaks.push_back(peak);
            }
            return peaks;
        };
        auto one = steps(false), two = steps(true);
        CHECK(one[0] > 0.01f && two[0] > 1.6f * one[0]); // both on the first step
        float one_max = *std::max_element(one.begin() + 80, one.end()), two_max = *std::max_element(two.begin() + 80, two.end());
        CHECK(two_max > 1.6f * one_max);                 // and on the later ones
        Project kit;
        CHECK(!kit.tracks[0].repeat && !kit.tracks[0].arp && NoteRepeat().gate == 60 && !NoteRepeat().pressure_velocity);
    }
    { // 16 Levels: a pad's key with its semitones and Filter cutoff repeats as it is; its release names it without the cutoff
        Arp levels; levels.top = int(sampler_last_key);
        int up = pad_note(40, 5, 30), down = pad_note(40, -3), v = 0;
        levels.press(up, 100); levels.press(down, 80);
        CHECK(levels.next(v) == down && v == 80 && levels.next(v) == up && v == 100); // lowest pitch first
        levels.release(pad_note(40, 5));
        CHECK(levels.count == 1 && levels.next(v) == down);
        levels.octaves = 2; levels.position = 1;
        CHECK(levels.next(v) == pad_note(52, -3)); // the octave on the key, its semitones kept
    }

    // Mute mode: kit pads where the pads play the kit (Stage too), tracks on the mixer.
    ui.page = PageId::Pads;
    CHECK(mute_on_kit(ui, true) && !mute_on_kit(ui, false));
    ui.page = PageId::Stage;
    CHECK(mute_on_kit(ui, true));
    ui.page = PageId::Mixer;
    CHECK(!mute_on_kit(ui, true));
    ui.mute_latch = true;
    CHECK(ui.mute_mode());
    Track kit; kit.engine = EngineKind::Sampler;
    kit.pads[3].sample = 0; kit.pads[100].sample = 0;
    kit.set_pad_muted(100, true);
    CHECK(kit.pad_muted(100) && !kit.pad_muted(3) && kit.any_pad_muted() && pad_soloed(kit, 3) && !pad_soloed(kit, 100));
    kit.set_pad_muted(100, false);
    CHECK(!kit.any_pad_muted() && !pad_soloed(kit, 3));
    { // Issue 32: a kit pad's mute lane. Motion lists it for the last pad hit and names it for its pad; its card is the
      // pad's mute; while the pattern plays, the mute heard follows the lane (before its first point, as set).
        auto lanes_project = std::make_unique<Project>();
        auto& drums = lanes_project->tracks[0];
        drums.engine = EngineKind::Sampler; drums.pads[4].sample = 0; drums.pads[5].sample = 0; drums.last_pad = 4;
        Pattern p;
        MotionPoint on; on.tick = 4 * sixteenth_ticks; on.param = uint8_t(motion_pad_mute); on.pad = 4; on.value = 1;
        MotionPoint off = on; off.tick = 8 * sixteenth_ticks; off.value = 0;
        CHECK(p.write_motion(on) && p.write_motion(off));
        UiState mui; mui.page = PageId::Motion;
        auto lanes = motion_lanes(*lanes_project, mui, p);
        CHECK(lanes[0] == motion_lane(motion_pad_mute, 4) && lane_name(drums, lanes[0]) == "A05 MUTE");
        auto cards = page_bindings(*lanes_project, mui, p);
        CHECK(cards[0].kind == Binding::Kind::PadMute && cards[0].id == 4 && cards[0].min == 0 && cards[0].max == 1 && cards[0].value == 0);
        drums.set_pad_muted(4, true);
        CHECK(page_bindings(*lanes_project, mui, p)[0].value == 1 && motion_param(drums, lanes[0]).value == 1);
        drums.last_pad = 5; // another pad's lane waits for that pad
        CHECK(motion_lanes(*lanes_project, mui, p)[0] != lanes[0]);
        drums.set_pad_muted(4, false);
        CHECK(!pad_muted_now(drums, p, -1, 4) && !pad_muted_now(drums, p, 0, 4) && pad_muted_now(drums, p, 5 * sixteenth_ticks, 4));
        CHECK(!pad_muted_now(drums, p, 9 * sixteenth_ticks, 4) && !pad_muted_now(drums, p, 5 * sixteenth_ticks, 5));
        drums.set_pad_muted(4, true);
        CHECK(pad_muted_now(drums, p, 0, 4) && pad_muted_now(drums, p, -1, 4) && !pad_muted_now(drums, p, 9 * sixteenth_ticks, 4));
    }

    // Pad Bank held or a track copy: the pads are the tracks; Shift + Erase there copies.
    UiState held; held.pad_bank = true;
    CHECK(tracks_on_pads(held));
    // The pads are the tracks; the screen stays the page's (Pad FX + Knob FX shows the tracks).
    CHECK(!track_overlay_shown(held));
    project->tracks[1].engine = EngineKind::Plugin;
    held.track = 1;
    CHECK(button_has_shift_job(*project, held, SurfaceButton::Erase, false));
    held.pad_bank = false; held.copying = UiState::Copying::Tracks;
    CHECK(tracks_on_pads(held) && track_overlay_shown(held)); // a track copy: at once

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
    p.add({300, 400, 60, 100, 100, 0}); // step 1 of 1/16, 60 ticks late, held 400 ticks
    p.add({310, 240, 62, 90, 100, 0});
    for (uint8_t k = 0; k <= notes_per_step; ++k) p.add({240 * 3, 240, uint8_t(70 + k), 100, 100, 0}); // one note more than a step shows
    Step one = p.step(1);
    CHECK(one.notes[0].key == 60 && one.notes[0].offset == 60 && one.notes[1].offset == 70 && one.length == 2);
    one.notes[1].velocity = 50; // a velocity changed on Steps: places and lengths stay
    p.set_step(1, one);
    CHECK(p.events[0] == Event({300, 400, 60, 100, 100, 0}) && p.events[1].velocity == 50 && p.events[1].tick == 310);
    Step three = p.step(3);
    CHECK(three.notes[notes_per_step - 1].key == 70 + notes_per_step - 1);
    p.set_step(3, Step {}); // the notes shown go; the one past them stays
    CHECK(p.events.size() == 3 && p.events.back().key == 70 + notes_per_step);
    CHECK(p.steps() == 16 && !p.empty());
    // Q is only the grid: a coarser one sees the same notes in fewer, longer steps.
    p.division = Division::Quarter;
    CHECK(p.steps() == 4 && p.step(0).notes[0].offset == 300);
    p.division = Division::Sixteenth;
    CHECK(division_ticks(Division::SixtyFourth) == 60 && division_ticks(Division::QuarterTriplet) == 640 && std::string(division_name(Division::ThirtySecondTriplet)) == "1/32T");

    // Rec Quantize: the nearest step, the last step's second half onto the first.
    Pattern q;
    q.length = 3840;
    CHECK(quantise_tick(q, 110) == 0 && quantise_tick(q, 130) == 240 && quantise_tick(q, 3800) == 0);
    q.division = Division::EighthTriplet;
    CHECK(quantise_tick(q, 400) == 320);
    // Onto a grid given in ticks, whatever the pattern's Q: 1/32, 1/32T, 1/64, a quarter; past the end, the first.
    CHECK(quantise_tick(q, 130, division_ticks(Division::ThirtySecond)) == 120 && quantise_tick(q, 170, division_ticks(Division::ThirtySecond)) == 120);
    CHECK(quantise_tick(q, 180, division_ticks(Division::ThirtySecond)) == 240);
    CHECK(quantise_tick(q, 110, division_ticks(Division::ThirtySecondTriplet)) == 80 && quantise_tick(q, 120, division_ticks(Division::ThirtySecondTriplet)) == 160);
    CHECK(quantise_tick(q, 40, division_ticks(Division::SixtyFourth)) == 60 && quantise_tick(q, 1400, division_ticks(Division::Quarter)) == 960);
    CHECK(quantise_tick(q, 3820, division_ticks(Division::SixtyFourth)) == 0 && quantise_tick(q, 3780, division_ticks(Division::SixtyFourth)) == 3780);
    CHECK(quantise_tick(q, 400, division_ticks(q.division)) == quantise_tick(q, 400)); // the pattern's Q

    // Double / Half Seq, Half / Double Speed.
    Pattern d;
    d.add({0, 240, 36, 100, 100, 0}); d.add({1000, 240, 38, 100, 100, 0}); d.add({3840, 240, 40, 100, 100, 0}); // the last is past the end
    CHECK(double_pattern(d, 128 * 3840) && d.length == 7680 && d.events.size() == 4 && d.events[2].tick == 3840 && d.events[3].tick == 4840);
    CHECK(!double_pattern(d, 7680 + 1));
    { // Settings' Double Seq "Empty": twice as long, the new half bare
        Pattern e;
        e.add({0, 240, 36, 100, 100, 0}); e.add({1000, 240, 38, 100, 100, 0}); e.add({3840, 240, 40, 100, 100, 0});
        CHECK(double_pattern(e, 128 * 3840, false) && e.length == 7680 && e.events.size() == 2 && e.events[1].tick == 1000);
    }
    half_pattern(d);
    CHECK(d.length == 3840 && d.events.size() == 2);
    scale_pattern(d, true); // half speed: twice as far apart; 1000 -> 2000
    CHECK(d.events.size() == 2 && d.events[1].tick == 2000 && d.events[1].length == 480 && d.length == 3840);
    scale_pattern(d, true); // again: 4000 falls past the end
    CHECK(d.events.size() == 1);
    d.add({3000, 480, 38, 100, 100, 0});
    scale_pattern(d, false); // double speed
    CHECK(d.events.size() == 2 && d.events[1].tick == 1500 && d.events[1].length == 240);

    // Time Correct: the chosen keys onto Q, the second sixteenth of each eighth late by the swing, then shifted.
    Pattern t;
    t.length = 3840;
    t.add({50, 240, 36, 100, 100, 0}); t.add({270, 240, 36, 100, 100, 0}); t.add({270, 240, 42, 100, 100, 0}); t.add({3700, 240, 36, 100, 100, 0}); // step 15 (3700 + 120 rounds down), swung to 3720
    std::array<uint64_t, 3> keys {};
    keys[0] |= uint64_t(1) << 36;
    time_correct_all(t, &keys, 75, 0); // 75 %: half a sixteenth late on the off sixteenths
    CHECK(t.events.size() == 4 && t.events[0].tick == 0 && t.events[0].key == 36);
    CHECK(t.events[1].tick == 270 && t.events[1].key == 42); // not chosen: where it was
    CHECK(t.events[2].tick == 360 && t.events[2].key == 36); // step 1 + 120 of swing
    time_correct_all(t, nullptr, 50, -20); // every note, 20 ticks early; the first wraps to the end
    CHECK(t.events.back().tick == 3820 && t.events.front().tick == 220);

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
        CHECK(old.length == 32 * 480 && old.division == Division::Eighth && old.events.size() == 1);
        CHECK(old.events[0] == Event({5 * 480, 2 * 480, 60, 100, 100, step_accent}));
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
        CHECK(again.signature == 12 && again.time_signature().beats == 7 && again.bar_ticks() == 3360 && again.seq_tempo && again.pattern_tempo[3] == 905);
        CHECK(again.tempo_of(3) == 905 && again.tempo_of(2) == again.tempo);
    }
    // 128 bars of sixteenths: 2048 notes, saved and loaded whole.
    {
        Project project; auto bank = std::make_unique<PatternBank>();
        auto& big = bank->at[2][0];
        big.length = project.max_length();
        for (unsigned n = 0; n < 2048; ++n) big.events.push_back({n * 240, 120, uint8_t(36 + n % 24), 100, 100, 0});
        auto data = save_project(project, *bank);
        Project copy; auto copy_bank = std::make_unique<PatternBank>();
        CHECK(load_project(data, copy, *copy_bank).empty() && copy_bank->at[2][0] == big && big.length == 128 * 3840);
    }

    // The engine: a note 60 ticks off the grid sounds 60 ticks late.
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
        late.add({4 * 240 + 60, 240, 45, 110, 100, 0}); // tick 1020: 23428 samples at 120 BPM
        auto host = host_with(late);
        host->send({Command::Type::Play});
        auto left = render(*host, 240);
        CHECK(peak_of(left, 0, 23300) == 0 && peak_of(left, 23500, 30000) > 0.005f);
    }
    // Count-In: a bar of clicks first, the pattern from its start after it.
    {
        Pattern first;
        first.add({0, 240, 45, 110, 100, 0});
        auto host = host_with(first);
        host->send({Command::Type::Play, 1});
        render(*host, 2);
        CHECK(host->count_in() == 1 && host->position_of(0) < 0);
        CHECK(host->count_in_from() == 0 && host->count_in_ticks() > 3 * ticks_per_quarter && host->count_in_ticks() < 4 * ticks_per_quarter);
        render(*host, 22050 / 128 + 2); // the second beat
        CHECK(host->count_in() == 2);
        render(*host, 3 * 22050 / 128 - 6); // just short of the bar's end: Seq Record keeps a hit here, on the first tick
        CHECK(host->count_in() == 4 && host->count_in_ticks() > 0 && host->count_in_ticks() <= sixteenth_ticks);
        render(*host, 22050 / 128); // past the bar
        CHECK(host->count_in() == 0 && host->count_in_ticks() == 0 && host->position_of(0) >= 0);
        // Played again with a count-in: ticks() reads the start, not where the last run stopped (an early hit's length).
        host->send({Command::Type::Stop});
        render(*host, 2);
        host->send({Command::Type::Play, 1});
        render(*host, 2);
        CHECK(host->count_in() == 1 && host->ticks() == host->count_in_from());
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
        host->send({Command::Type::Signature, 0, 0, ticks_per_quarter, 3 * ticks_per_quarter});
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
        Pattern four; four.length = 4 * 3840;
        auto host = host_with(four);
        host->send({Command::Type::Play});
        render(*host, 4);
        Command to {Command::Type::Locate};
        to.position = 2 * 3840;
        host->send(to);
        render(*host, 1);
        CHECK(host->position_of(0) >= 2 * 3840 && host->position_of(0) < 2 * 3840 + 10);
        host->send({Command::Type::Stop});
        Command from {Command::Type::Play};
        from.position = 3840 + 960;
        host->send(from);
        render(*host, 1);
        CHECK(host->position_of(0) >= 3840 + 960 && host->position_of(0) < 3840 + 970);
    }
    // A note written just ahead of the playhead (Rec Quantize) was heard live: skipped once, then played.
    {
        Pattern ahead;
        ahead.add({0, 240, 45, 110, 100, 0});
        auto host = host_with(ahead);
        Command wrote {Command::Type::Recorded, 0, 0, 45};
        wrote.position = 0;
        host->send(wrote);
        host->send({Command::Type::Play});
        auto first_bar = render(*host, 88200 / 128 - 30);
        auto second = render(*host, 100);
        CHECK(peak_of(first_bar, 0, 40000) == 0 && peak_of(second) > 0.005f);
    }

    // Patterns' cards: K1 by 1, 2, 3 .. 16, 24 .. 128 bars, Shift + K1 in sixteenths up to four bars (issue 27), Shift + K2 the
    // time signature.
    {
        Project project;
        UiState ui; ui.page = PageId::Patterns;
        Pattern five; five.length = 5 * 3840; five.free = false; // a length set by hand (Free: shell/free_length.h)
        auto base = page_bindings(project, ui, five, false);
        CHECK(base[0].kind == Binding::Kind::PatternLength && base[0].max == int(pattern_length_count) - 1 && pattern_lengths[base[0].value] == 5);
        CHECK(pattern_lengths[base[0].max] == 128 && pattern_lengths[15] == 16 && pattern_lengths[16] == 24); // every bar to 16 (issue 52)
        auto shifted = page_bindings(project, ui, five, true);
        CHECK(shifted[0].kind == Binding::Kind::PatternSteps && shifted[0].value == 64 && shifted[0].min == 1 && shifted[0].max == 64);
        Pattern seven; seven.length = 7 * sixteenth_ticks; seven.free = false; // 7/16 against a bar on another track
        CHECK(page_bindings(project, ui, seven, true)[0].value == 7 && pattern_lengths[page_bindings(project, ui, seven, false)[0].value] == 1);
        CHECK(shifted[1].kind == Binding::Kind::TimeSignature && shifted[1].value == int(common_time));
        project.tracks[0].engine = EngineKind::Sampler; // a used track: its speed on Shift + K3 since the mixer (the click level is on Tempo)
        CHECK(page_bindings(project, ui, five, true)[2].kind == Binding::Kind::TrackSpeed);
        project.tracks[0].engine = EngineKind::None;
        ui.page = PageId::TimeCorrect; five.feel.strength = 70; five.feel.swing = 60; five.feel.humanize = 20;
        auto tc = page_bindings(project, ui, five, false); // time-correct C: Q, then the strength and swing heard live
        CHECK(tc[0].kind == Binding::Kind::Division && tc[1].kind == Binding::Kind::FeelStrength && tc[1].value == 70
              && tc[2].kind == Binding::Kind::FeelSwing && tc[2].value == 60);
        auto tc_shift = page_bindings(project, ui, five, true); // Shift: humanize, its velocity, the groove's amount
        CHECK(tc_shift[0].kind == Binding::Kind::FeelHumanize && tc_shift[0].value == 20 && tc_shift[1].kind == Binding::Kind::FeelVelocity
              && tc_shift[2].kind == Binding::Kind::GrooveAmount);
        int start = -1; // Erase + pot on a feel card: back to a new pattern's
        CHECK(feel_reset_value(tc[2], start) && start == 50 && feel_reset_value(tc[1], start) && start == 0);
        CHECK(feel_reset_value(tc_shift[2], start) && start == 100 && erase_resets(tc_shift[0]) && !feel_reset_value(tc[0], start));
        CHECK(shift_pad_job(1).kind == ShiftPadJob::Kind::Action && shift_pad_job(13).kind == ShiftPadJob::Kind::Open);
    }
}

// Parity phase 3: Step Edit's view of a step, and Motion on a kit pad's own values.
void step_edit_and_pad_motion() {
    // The events starting in a step of Q, and the selection kept inside it.
    Pattern p;
    p.length = 4 * ticks_per_quarter;
    for (auto e : {Event {0, 240, 36, 100, 100, 0}, Event {240, 240, 38, 90, 100, 0}, Event {240, 120, 37, 80, 100, 0}, Event {310, 60, 40, 70, 100, 0}, Event {480, 240, 36, 60, 100, 0}})
        p.add(e);
    auto in = step_events(p, 1);
    CHECK(in.first == 1 && in.count == 3 && p.events[1].key == 37 && p.events[3].tick == 310);
    CHECK(step_events(p, 3).count == 0 && step_events(p, 2).count == 1);
    UiState ui;
    ui.step_cursor = 1; ui.step_event = 9; // past the step's last: its last
    CHECK(selected_event(p, ui) == 3);
    ui.step_cursor = 3;
    CHECK(selected_event(p, ui) == -1);
    p.division = Division::Eighth; // Q 480: step 0 holds four
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
    auto host_held = std::make_unique<EngineHost>(); // on the heap: a test's frame stays under the 8 MB stack
    auto& host = *host_held;
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
    { // Mono (a new pad's default): a second hit stops the first; Poly lets them sum. 16 Levels' notes at two
      // pitches: a mono pad keeps only the last, a poly pad plays the chord.
        PadSound pad = base; pad.end = 40000;
        CHECK(pad.mono);
        pad.mono = false;
        auto poly = engine_with(pad);
        poly->note_on(key, 127); poly->note_on(key, 127);
        float both = run(*poly, 8, 2).first;
        pad.mono = true;
        auto mono = engine_with(pad);
        mono->note_on(key, 127); mono->note_on(key, 127);
        float one = run(*mono, 8, 2).first;
        CHECK(both > one * 1.7f && one > 0.1f);
        auto chord = [&](bool is_mono, bool both_notes) {
            PadSound p = pad; p.mono = is_mono;
            auto e = engine_with(p);
            if (both_notes) e->note_on(pad_note(key, 4), 127);
            e->note_on(pad_note(key, 7), 127);
            return run(*e, 8, 2).first;
        };
        const float last_only = chord(false, false);
        CHECK(std::fabs(chord(true, true) - last_only) < 1e-4f); // mono: the second pitch cuts the first
        CHECK(chord(false, true) > last_only * 1.2f);           // poly: both sound
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
        // Issue 64: at the sustain a pad has until it is turned (100) a one shot from the start still falls; Note On
        // keeps its sustain.
        pad.sustain = 100;
        auto full = engine_with(pad);
        full->note_on(key, 127);
        CHECK(run(*full, 90, 80).first < middle * 0.3f);
        pad.play = PadPlay::Gate; pad.sustain = 50;
        auto gate = engine_with(pad);
        gate->note_on(key, 127);
        float held = run(*gate, 90, 80).first;
        CHECK(held > middle * 0.35f && held < middle * 0.65f);
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
    kit.last_pad = 2; kit.pads[2].end = 30000; project->samples.edit(0).frames = 44100;
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

// Trim and Chop zoomed in (shell/zoom_view.h): the zoom 0-100% smooth from all of the span down to
// zoom_closest frames; the view kept while its handle moves inside it and carried along past its edges;
// the wheel's step a 1024th of the view; Zero Snap's wheel cut onto the next crossing the way it turns;
// Trim's cards in frames across the view on screen.
void zoom_and_fine_edit() {
    CHECK(zoom_width(441000, 0) == 441000 && zoom_width(441000, zoom_top) == zoom_closest && zoom_width(200, 80) == 200);
    uint32_t last = 441000;
    for (int z = 1; z <= zoom_top; ++z) { // every step closer, never a jump bigger than the step before it by much
        uint32_t w = zoom_width(441000, z);
        CHECK(w < last && double(last) / w < 1.1);
        last = w;
    }
    // Chop's wheel closes the view in on the cut it moves, about follow_view across (none where the region is shorter),
    // and the next pad opens it again; a zoom set by Shift + K is the player's and stays.
    CHECK(zoom_for_width(4000, follow_view) == 0 && zoom_for_width(176400, 200) == zoom_top);
    for (uint32_t span : {22050u, 176400u, 44100u * 60}) {
        const int z = zoom_for_width(span, follow_view);
        const uint32_t w = zoom_width(span, z);
        CHECK(z > 0 && z < zoom_top && w > follow_view * 9 / 10 && w < follow_view * 11 / 10);
    }
    {
        UiState z;
        chop_follow(z, 176400);
        CHECK(z.chop_zoom == zoom_for_width(176400, follow_view) && z.chop_zoom_auto);
        z.chop_view = {10, 20, 3};
        chop_follow(z, 176400); // the next detent: the view it carries along stays
        CHECK(z.chop_view.key == 3);
        chop_unfollow(z);
        CHECK(z.chop_zoom == 0 && !z.chop_zoom_auto && z.chop_view.key == 0);
        z.chop_zoom = 30; // Shift + K's
        chop_follow(z, 176400);
        chop_unfollow(z);
        CHECK(z.chop_zoom == 30 && !z.chop_zoom_auto);
        chop_follow(z, 2000); // shorter than the view: nothing to close in on
        CHECK(z.chop_zoom == 30);
        z.chop_zoom = 0; chop_follow(z, 2000);
        CHECK(z.chop_zoom == 0 && !z.chop_zoom_auto);
    }
    FrameView edge = zoom_view(441000, 0, 441000, 100, 60); // round a handle near the start: kept inside the file
    CHECK(edge.from == 0 && edge.width() == zoom_width(441000, 60));
    edge = zoom_view(441000, 0, 441000, 440990, 60);
    CHECK(edge.to == 441000);
    CHECK(fine_step({0, 256}) == 1 && fine_step({0, 441000}) == 430);
    // Spun fast the wheel goes further a detent; slow, a pause or the other way, one step.
    WheelPace pace;
    CHECK(wheel_gain(pace, 1000, 1) == 1 && wheel_gain(pace, 1200, 1) == 1); // 5 a second
    unsigned gain = 1;
    for (uint32_t t = 1210; t < 1400; t += 10) gain = wheel_gain(pace, t, 1); // 100 a second
    CHECK(gain >= 16 && gain <= wheel_most);
    CHECK(wheel_gain(pace, 1410, -1) == 1); // the other way: from the start
    for (uint32_t t = 1420; t < 1700; t += 4) gain = wheel_gain(pace, t, -1);
    CHECK(gain == wheel_most);
    CHECK(wheel_gain(pace, 2500, -1) == 1); // after a pause
    ZoomView kept;
    zoom_follow(kept, 7, {1000, 2000}, 1500, 441000, 40); // inside: the view stays
    CHECK(kept.from == 1000 && kept.to == 2000 && kept.key == 7);
    CHECK(zoom_now(kept, 7, 441000, 0, 441000, 1999, 40).from == 1000); // the same handle and zoom: kept
    CHECK(zoom_now(kept, 9, 441000, 0, 441000, 1999, 40).from != 1000); // another: round the handle again
    zoom_follow(kept, 7, {1000, 2000}, 2300, 441000, 40); // past the edge: carried along
    CHECK(kept.to == 2300 && kept.from == 1300);

    // Zero Snap's wheel: the next crossing the way it turns, never back past where it starts.
    std::vector<float> l(1000), r(1000, 0.0f);
    for (unsigned i = 0; i < l.size(); ++i) l[i] = std::sin(float(i) * 2 * 3.14159265f / 100.0f) + 0.01f; // crossings near 50, 100, 150..
    uint32_t up = zero_crossing_toward(l.data(), r.data(), 1000, 120, 1), down = zero_crossing_toward(l.data(), r.data(), 1000, 120, -1);
    CHECK(up >= 120 && up <= 152 && down <= 120 && down >= 98);
    uint32_t next = zero_crossing_toward(l.data(), r.data(), 1000, up + 3, 1); // a few frames on: the crossing after (near 200)
    CHECK(next >= 196 && next <= 204);
    std::vector<float> flat(1000, 0.5f);
    CHECK(zero_crossing_toward(flat.data(), flat.data(), 1000, 300, 1) == 300); // none: where it was

    // A handle's pot puts it in focus; its zoom too; Chop's wheel then edits it.
    UiState ui;
    CHECK(focus_handle(ui, Binding {Binding::Kind::PadEnd, 0, 0, 1, 0}) && ui.trim_focus == 1);
    CHECK(!focus_handle(ui, Binding {Binding::Kind::PadEnd, 0, 0, 1, 0}));
    CHECK(focus_handle(ui, Binding {Binding::Kind::TrimZoom, 2, 0, zoom_top, 0}) && ui.trim_focus == 2);
    CHECK(focus_handle(ui, Binding {Binding::Kind::ChopZoom, 1, 0, zoom_top, 0}) && ui.chop_focus == 1 && ui.chop_fine);
    CHECK(!focus_handle(ui, Binding {Binding::Kind::PadLevel, 0, 0, 127, 0}));

    // Trim's cards in frames: unzoomed the whole file, zoomed the view on screen round the handle in focus.
    Project project;
    auto& kit = project.tracks[0];
    load_sound(kit, EngineKind::Sampler, 0);
    project.samples.edit(0).frames = 441000; std::snprintf(project.samples.edit(0).path, sizeof(SampleRef::path), "/x.wav");
    kit.last_pad = 0; kit.pads[0].sample = 0; kit.pads[0].start = 44100; kit.pads[0].end = 0;
    UiState trim;
    trim.track = 0; trim.page = PageId::Trim;
    Pattern none;
    auto cards = page_bindings(project, trim, none, false);
    CHECK(cards[0].kind == Binding::Kind::PadStart && cards[0].value == 44100 && cards[0].min == 0 && cards[0].max == 440999);
    CHECK(cards[1].kind == Binding::Kind::PadEnd && cards[1].value == 441000 && cards[1].min == 44101 && cards[1].max == 441000);
    trim.trim_zoom = zoom_top;
    FrameView view = trim_view(project, trim);
    cards = page_bindings(project, trim, none, false);
    CHECK(view.width() == zoom_closest && view.from <= 44100 && view.to >= 44100);
    CHECK(cards[0].min == int(view.from) && cards[0].max == int(view.to));
    CHECK(page_bindings(project, trim, none, true)[0].max == zoom_top);
}

// Samples' trim sheet (shell/browse_trim.h): K2 START and K3 END on an audio file under the cursor, none on a folder
// or a kit; not read yet, cards that move nothing; read, START and END in frames across the whole file, the view about
// follow_view round the end turned last and going along with it; Shift + K2 / K3 a zoom of one's own, and then the
// pot turns across the view; the file's end kept as 0; what a moved end plays once.
void browse_trim_sheet() {
    using K = Binding::Kind;
    Project project;
    load_sound(project.tracks[0], EngineKind::Sampler, 0);
    UiState ui;
    ui.track = 0; ui.page = PageId::Samples; ui.files_count = 9; ui.files_cursor = 2;
    Pattern none;
    auto cards = page_bindings(project, ui, none, false);
    CHECK(cards[0].kind == K::FileJump && !cards[1].bound() && !cards[2].bound()); // a folder or a kit: K1 alone
    ui.files_audio = true;
    cards = page_bindings(project, ui, none, false);
    CHECK(cards[1].kind == K::BrowseStart && cards[1].id == 1 && cards[2].kind == K::BrowseEnd && cards[2].id == 1);
    CHECK(page_bindings(project, ui, none, true)[1].kind == K::BrowseZoom && page_bindings(project, ui, none, true)[2].id == 1);
    ui.sheet_open = true; // LOAD KIT's cards over the list: no trim
    CHECK(!page_bindings(project, ui, none, false)[1].bound());
    ui.sheet_open = false;

    ui.browse.slot = 5; // read into slot 5, not loaded yet: nothing moves
    CHECK(!browse_set(ui.browse, project, K::BrowseStart, 100) && !browse_view(project, ui.browse).width());
    project.samples.edit(5).frames = 441000; std::snprintf(project.samples.edit(5).path, sizeof(SampleRef::path), "/x.wav");
    cards = page_bindings(project, ui, none, false);
    CHECK(cards[1].kind == K::BrowseStart && cards[1].id == 0 && cards[1].min == 0 && cards[1].max == 440999 && cards[1].value == 0);
    CHECK(cards[2].kind == K::BrowseEnd && cards[2].min == 1 && cards[2].max == 441000 && cards[2].value == 441000);
    FrameView view = browse_view(project, ui.browse);
    CHECK(view.from == 0 && view.width() == zoom_width(441000, zoom_for_width(441000, follow_view)));
    CHECK(browse_set(ui.browse, project, K::BrowseStart, 22050) && ui.browse.start == 22050);
    view = browse_view(project, ui.browse);
    CHECK(view.from < 22050 && view.to > 22050); // round START as it moves
    CHECK(browse_set(ui.browse, project, K::BrowseEnd, 10) && ui.browse.end == 22051); // never before START
    CHECK(browse_set(ui.browse, project, K::BrowseEnd, 500000) && ui.browse.end == 0); // the file's end: kept as 0
    CHECK(!browse_set(ui.browse, project, K::BrowseEnd, 441000));
    CHECK(browse_set(ui.browse, project, K::BrowseEnd, 300000) && browse_set(ui.browse, project, K::BrowseStart, 400000) && ui.browse.start == 299999);
    CHECK(focus_handle(ui, page_bindings(project, ui, none, false)[2]) && ui.browse.focus == 1);
    view = browse_view(project, ui.browse);
    CHECK(view.from < 300000 && view.to > 300000); // round END now
    CHECK(!focus_handle(ui, page_bindings(project, ui, none, false)[2]));
    CHECK(focus_handle(ui, page_bindings(project, ui, none, true)[1]) && ui.browse.focus == 0);
    CHECK(handle_kind(K::BrowseStart) && handle_kind(K::BrowseEnd) && !handle_kind(K::BrowseZoom));

    // Shift + K2: a zoom of one's own; the pot then turns across the view on screen, kept while START moves inside it.
    CHECK(browse_set(ui.browse, project, K::BrowseEnd, 441000) && ui.browse.end == 0);
    ui.browse.own_zoom = true; ui.browse.zoom = zoom_top; ui.browse.view = {};
    view = browse_view(project, ui.browse);
    cards = page_bindings(project, ui, none, false);
    CHECK(view.width() == zoom_closest && cards[1].min == int(view.from) && cards[1].max == int(view.to));
    CHECK(browse_set(ui.browse, project, K::BrowseStart, int(view.from) + 3) && browse_view(project, ui.browse).from == view.from);
    CHECK(browse_set(ui.browse, project, K::BrowseStart, int(view.to) + 100)); // past its edge: the view goes along
    CHECK(browse_view(project, ui.browse).to >= ui.browse.start && browse_view(project, ui.browse).from > view.from);

    // Heard once still: START plays the part kept; END its last second.
    BrowseHandles h {441000, 100000, 300000};
    CHECK(browse_heard(h, 0) == std::make_pair(100000u, 300000u) && browse_heard(h, 1) == std::make_pair(300000u - heard_tail, 300000u));
    h.start = 290000;
    CHECK(browse_heard(h, 1).first == 290000);
}

// Chop's Auto type (engine/auto_chop.cpp, shell/chop.h auto_chop()): a groove of drums reads as DRUMS and is cut on
// its beats, every other beat or in eighths by SLICES (never on a ghost note, no slice under 80% of an eighth); HITS
// keeps 150 ms between cuts; a chord loop reads as MUSIC and is cut on its beats too; a row of single sounds with silence between reads as SHOTS, one slice a sound,
// each ending where its sound does, and hand cuts keep those gaps; KIT lays one pad a sound; SLICES on Shift + K3.
void auto_chop_kinds() {
    struct Made { std::array<Hit, max_hits> hits {}; unsigned count = 0; BeatGrid grid; AutoRead autos; uint32_t frames = 0; };
    auto analysed = [](const std::vector<float>& wave) {
        Made m;
        m.frames = uint32_t(wave.size());
        std::vector<float> mono(wave.size());
        for (size_t n = 0; n < wave.size(); ++n) mono[n] = 2 * wave[n];
        m.count = find_hits_in(mono, m.hits, m.grid, &m.autos);
        return m;
    };
    uint32_t noise = 11;
    auto add_kick = [](std::vector<float>& w, double at, double level = 0.8) {
        for (uint32_t n = uint32_t(at * 44100), end = std::min<uint32_t>(uint32_t(w.size()), n + 13230); n < end; ++n) {
            double u = (n - at * 44100) / 44100;
            w[n] += float(level * std::sin(2 * M_PI * (55 + 150 * std::exp(-u / 0.015)) * u) * std::exp(-u / 0.06));
        }
    };
    auto add_noise = [&](std::vector<float>& w, double at, double level, double decay) {
        for (uint32_t n = uint32_t(at * 44100), end = std::min<uint32_t>(uint32_t(w.size()), n + uint32_t(decay * 6 * 44100)); n < end; ++n) {
            noise = noise * 1664525u + 1013904223u;
            w[n] += float(level * ((noise >> 8) / 8388608.0 - 1) * std::exp(-(n - at * 44100) / 44100 / decay));
        }
    };
    auto near_one = [](uint32_t cut, const std::vector<double>& at) {
        for (double t : at) if (std::fabs(double(cut) - t * 44100) <= 882) return true;
        return false;
    };

    // Four bars at 100 BPM: kicks on 1 and 3, a snare on 2 and 4, hats on the eighths, ghost notes on the last
    // sixteenth of each beat.
    const double beat = 0.6;
    std::vector<float> wave(size_t(16 * beat * 44100), 0.0f);
    std::vector<double> hits_at, ghosts_at, strong_at; // strong: the kicks and snares
    for (unsigned b = 0; b < 16; ++b) {
        if (b % 2 == 0) add_kick(wave, b * beat); else add_noise(wave, b * beat, 0.5, 0.05);
        add_noise(wave, b * beat, 0.15, 0.008);
        add_noise(wave, (b + 0.5) * beat, 0.15, 0.008);
        add_noise(wave, (b + 0.75) * beat, 0.03, 0.02);
        if (b) { hits_at.push_back(b * beat); strong_at.push_back(b * beat); }
        hits_at.push_back((b + 0.5) * beat);
        ghosts_at.push_back((b + 0.75) * beat);
    }
    auto groove = analysed(wave);
    CHECK(groove.autos.kind == SoundKind::Drums && groove.autos.sounds.size() == groove.count);
    CHECK(groove.grid.found() && std::abs(int(groove.grid.tempo) - 1000) <= 10);
    // SLICES 16 over 16 beats: one slice a beat, each cut on its kick or snare.
    Slices s = auto_chop(groove.autos, 16, 0, groove.frames, groove.frames, groove.hits, groove.count, groove.grid);
    CHECK(s.count == 16);
    for (unsigned n = 1; n < s.count; ++n) CHECK(near_one(s.start[n], strong_at));
    // SLICES 32: eighths, the hats between the beats; never the ghost notes, and no slice under 80% of an eighth.
    s = auto_chop(groove.autos, 32, 0, groove.frames, groove.frames, groove.hits, groove.count, groove.grid);
    CHECK(s.count == 32);
    for (unsigned n = 0; n < s.count; ++n) {
        if (n) CHECK(near_one(s.start[n], hits_at) && !near_one(s.start[n], ghosts_at));
        CHECK(s.end(n, groove.frames) - s.start[n] >= uint32_t(0.8 * beat / 2 * 44100));
    }
    // SLICES 8: every other beat; 64 asks for more than eighths give, and gets the eighths.
    CHECK(auto_chop(groove.autos, 8, 0, groove.frames, groove.frames, groove.hits, groove.count, groove.grid).count == 8);
    CHECK(auto_chop(groove.autos, 64, 0, groove.frames, groove.frames, groove.hits, groove.count, groove.grid).count == 32);
    // HITS keeps 150 ms between cuts.
    AutoRead as_hits = groove.autos;
    as_hits.kind = SoundKind::Hits;
    s = auto_chop(as_hits, 64, 0, groove.frames, groove.frames, groove.hits, groove.count, groove.grid);
    for (unsigned n = 0; n < s.count; ++n) CHECK(s.end(n, groove.frames) - s.start[n] >= auto_least);

    // A row of single sounds, silence between, at no steady spacing.
    std::vector<float> row(size_t(3.2 * 44100), 0.0f);
    const std::vector<double> starts {0.05, 0.42, 0.95, 1.31, 1.9, 2.55};
    for (size_t k = 0; k < starts.size(); ++k) {
        if (k % 2 == 0) add_kick(row, starts[k], 0.5 + 0.1 * double(k)); else add_noise(row, starts[k], 0.4, 0.03);
    }
    auto shots = analysed(row);
    CHECK(shots.autos.kind == SoundKind::Shots && shots.autos.spans.size() == starts.size());
    s = auto_chop(shots.autos, 8, 0, shots.frames, shots.frames, shots.hits, shots.count, shots.grid);
    CHECK(s.count == starts.size());
    for (unsigned n = 0; n < s.count; ++n) {
        CHECK(std::fabs(double(s.start[n]) - starts[n] * 44100) <= 441);
        if (n + 1 < s.count) CHECK(!s.touches(n, shots.frames) && s.end(n, shots.frames) < s.start[n + 1]); // the silence left out
    }
    // A hand cut inside the third sound: it splits that sound in two, and the gaps stay round it.
    HandCuts hand;
    hand.pin(uint32_t(1.12 * 44100));
    const Slices kept = with_hand(s, hand, shots.frames);
    CHECK(kept.count == s.count + 1 && !kept.touches(1, shots.frames) && kept.touches(2, shots.frames) && !kept.touches(3, shots.frames)
          && kept.end(3, shots.frames) == s.end(2, shots.frames));

    // Four chords, two beats each at 110 BPM, played in eighths over a bass note on each beat: MUSIC, and SLICES 4
    // cuts every two beats, where they change.
    {
        const double b = 60 / 110.0;
        std::vector<float> keys(size_t(8 * b * 44100), 0.0f);
        const double roots[] {261.63, 349.23, 392.0, 220.0}; // C, F, G, A: major, major, major, minor
        for (int c = 0; c < 4; ++c)
            for (int e = 0; e < 4; ++e) {
                const double at = (2 * c + 0.5 * e) * b;
                for (double ratio : {1.0, c == 3 ? 1.1892 : 1.2599, 1.4983})
                    for (uint32_t n = uint32_t(at * 44100), end = std::min<uint32_t>(uint32_t(keys.size()), n + uint32_t(b / 2 * 44100)); n < end; ++n) {
                        const double u = (n - at * 44100) / 44100;
                        keys[n] += float((e == 0 ? 0.18 : e == 2 ? 0.14 : 0.08) * std::sin(2 * M_PI * roots[c] * ratio * u) * std::exp(-u / 0.25) * std::min(1.0, u / 0.003));
                    }
                if (e % 2 == 0) // a bass note on each beat, the chord's root two octaves down
                    for (uint32_t n = uint32_t(at * 44100), end = std::min<uint32_t>(uint32_t(keys.size()), n + uint32_t(b * 44100)); n < end; ++n) {
                        const double u = (n - at * 44100) / 44100;
                        keys[n] += float(0.3 * std::sin(2 * M_PI * roots[c] / 4 * u) * std::exp(-u / 0.12) * std::min(1.0, u / 0.003));
                    }
            }
        auto chords = analysed(keys);
        CHECK(chords.autos.found == SoundKind::Music && chords.autos.kind == SoundKind::Music);
        const Slices four = auto_chop(chords.autos, 4, 0, chords.frames, chords.frames, chords.hits, chords.count, chords.grid);
        CHECK(four.count == 4); // every two beats, from the region's start: where the chords change
        for (unsigned n = 1; n < four.count; ++n) CHECK(std::fabs(double(four.start[n]) - 2 * n * b * 44100) <= 882);
    }

    // KIT: the three kicks are one sound, kicks first; the three bursts of noise another. Each pad takes the longest.
    const auto kit = kit_sounds(s, shots.frames, shots.hits, shots.count, shots.autos);
    CHECK(kit.size() == 2 && kit[0].kind == KitKind::Kick && kit[0].count == 3 && kit[1].count == 3 && kit[1].kind != KitKind::Kick);
    for (const auto& k : kit)
        for (unsigned n = k.slice % 2; n < s.count; n += 2) CHECK(s.end(n, shots.frames) - s.start[n] <= s.end(k.slice, shots.frames) - s.start[k.slice]);
    CHECK(std::string(slice_play_name(SlicePlay::Kit)) == "KIT" && unsigned(SlicePlay::Count) == 4);
    std::array<int, max_slices> sound_of; // each slice to its sound, for TO PADS' pattern
    kit_sounds(s, shots.frames, shots.hits, shots.count, shots.autos, &sound_of);
    for (unsigned n = 0; n < s.count; ++n) CHECK(sound_of[n] == (n % 2 == kit[0].slice % 2 ? 0 : 1));
    CHECK(sound_of[s.count] == -1);

    // SLICES: 8, 16, 32, 64 on Shift + K3 (16 to start); Auto is the fifth type.
    CHECK(auto_slice_count(0) == 8 && auto_slice_count(int(auto_default_count)) == 16 && auto_slice_count(3) == 64 && auto_slice_count(9) == 64);
    CHECK(std::string(chop_mode_name(ChopMode::Auto)) == "Auto" && std::string(sound_kind_name(SoundKind::Shots)) == "SHOTS");
    Project project;
    UiState ui;
    Pattern pattern;
    ui.page = PageId::Chop; ui.chop_mode = ChopMode::Auto;
    const auto cards = shift_bindings(project, ui, pattern);
    CHECK(cards[2].kind == Binding::Kind::ChopAmount && cards[2].min == 0 && cards[2].max == 3 && cards[2].value == 1);
}

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
    auto beats = chop(ChopMode::Beats, 4, 1000, 0, frames, hits, found, 1200);
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
    // Slices with a gap between them (as an MPC file keeps them): edges across the gap move alone and stop at the
    // neighbour, a cut in the gap is refused, and Merge takes the gap into the slice before.
    Slices gap;
    gap.whole(1000, 20000); gap.add(10000, frames);
    gap.stop[0] = 8000;
    CHECK(gap.end(0, frames) == 8000 && gap.start[1] == 10000 && !gap.touches(0, frames));
    CHECK(!gap.add(9000, frames));
    gap.move_end(0, 15000, frames); // up to the next slice, no further
    CHECK(gap.end(0, frames) == 10000 && gap.touches(0, frames));
    gap.move_end(0, 8000, frames); // touching: the next slice's start comes too
    CHECK(gap.start[1] == 8000);
    gap.stop[0] = 6000; gap.move_start(1, 7000, frames); // a gap again: the next slice's start moves alone
    CHECK(gap.start[1] == 7000 && gap.end(0, frames) == 6000);
    gap.move_start(1, 5000, frames); // and stops where the slice before ends; touching from then on
    CHECK(gap.start[1] == 6000 && gap.touches(0, frames));
    gap.move_start(1, 9000, frames);
    CHECK(gap.start[1] == 9000 && gap.end(0, frames) == 9000);
    gap.remove(1);
    CHECK(gap.count == 1 && gap.start[0] == 1000 && gap.end(0, frames) == 20000);
    Slices tiny = chop(ChopMode::Manual, 0, 0, 1500, frames, hits, found, 1200);
    CHECK(!tiny.split(0, frames));
    // A tap a little after a hit lands on it; one far from any stays put.
    CHECK(snap_to_hit(41200, hits, found) == 40000);
    CHECK(snap_to_hit(45000, hits, found) == 45000);
    CHECK(snap_to_hit(39800, hits, found) == 40000); // up to 10 ms early

    // The cards: K1 start, K2 end, K3 type; Shift: zoom at each, the type's amount (none for Manual).
    Project project;
    load_sound(project.tracks[0], EngineKind::Sampler, 0);
    project.samples.edit(0).frames = 441000; std::snprintf(project.samples.edit(0).path, sizeof(SampleRef::path), "/x.wav");
    UiState ui;
    Pattern pattern;
    ui.page = PageId::Chop; ui.chop_sample = 0;
    ui.slices = chop(ChopMode::Regions, 4, 0, 0, 441000, hits, 0, 1200);
    ui.chop_mode = ChopMode::Regions; ui.chop_slice = 1;
    auto cards = page_bindings(project, ui, pattern);
    // In frames, a slice no shorter than min_slice.
    CHECK(cards[0].kind == Binding::Kind::ChopStart && cards[0].value == 110250 && cards[0].min == 882 && cards[0].max == 220500 - 882);
    CHECK(cards[1].kind == Binding::Kind::ChopEnd && cards[1].value == 220500 && cards[1].min == 110250 + 882 && cards[1].max == 330750 - 882);
    CHECK(cards[2].kind == Binding::Kind::ChopMode && cards[2].max == 4); // Threshold, Regions, Beats, Manual, Auto
    ui.chop_zoom = 50; // half way: sqrt(441000 * 256) frames round the slice's start; the pot turns across exactly that
    FrameView view = chop_view(project, ui);
    CHECK(view.width() == 10625 && view.from == 110250 - 5312);
    cards = page_bindings(project, ui, pattern);
    CHECK(cards[0].min == int(view.from) && cards[0].max == int(view.to) && cards[0].value == 110250);
    CHECK(cards[1].value == 220500 && cards[1].min == 110250 + 882); // the end, outside the view: all of its range
    ui.chop_focus = 1; // K2 touched: the view round the end
    view = chop_view(project, ui);
    CHECK(view.from <= 220500 && view.to >= 220500 && page_bindings(project, ui, pattern)[1].max == int(view.to));
    ui.chop_focus = 0;
    auto shifted = page_bindings(project, ui, pattern, true);
    CHECK(shifted[0].kind == Binding::Kind::ChopZoom && shifted[0].id == 0 && shifted[1].id == 1);
    CHECK(shifted[2].kind == Binding::Kind::ChopAmount && shifted[2].max == int(max_slices));
    ui.chop_mode = ChopMode::Beats; // Shift + K3 on Beats: the division, 1/16 to 4 bars, from 1/4
    shifted = page_bindings(project, ui, pattern, true);
    CHECK(shifted[2].min == 0 && shifted[2].max == int(beat_divisions) - 1 && shifted[2].value == int(quarter_division));
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

// The mic and the rear inputs share the ADC: a loop or MIDI track on the rear inputs takes it, else one on the mic,
// else Input Config's source. A source on the other side is IN USE, and the engine hears silence for it, not the
// other input under its name; USB-C and Melodee's own sound are never taken.
void input_taken() {
    using R = InputRoute;
    const auto route = [](InputSource s, unsigned needs) { return std::string(R::adc_route(s, needs)); };
    CHECK(route(InputSource::Mic, 0) == "mic" && route(InputSource::Mic, R::need_line) == "line" && route(InputSource::Rear, R::need_mic) == "mic"
          && route(InputSource::RearL, R::need_line | R::need_mic) == "line" && route(InputSource::Usb, 0) == "off"
          && route(InputSource::Usb, R::need_mic | R::need_usb) == "mic");
    CHECK(!R::adc_taken(InputSource::Mic, 0) && !R::adc_taken(InputSource::Rear, 0) && !R::adc_taken(InputSource::Mic, R::need_mic | R::need_usb));
    CHECK(R::adc_taken(InputSource::Mic, R::need_line) && R::adc_taken(InputSource::Mic, R::need_line | R::need_mic));
    CHECK(R::adc_taken(InputSource::RearL, R::need_mic) && R::adc_taken(InputSource::Rear, R::need_mic | R::need_usb));
    CHECK(!R::adc_taken(InputSource::RearR, R::need_line | R::need_mic) && !R::adc_taken(InputSource::Rear, R::need_line));
    for (auto s : {InputSource::Usb, InputSource::UsbL, InputSource::UsbR, InputSource::Resample, InputSource::Track})
        CHECK(!R::adc_taken(s, R::need_line | R::need_mic | R::need_usb));

    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    for (auto& t : project->tracks) t.engine = EngineKind::None;
    std::vector<int32_t> input(256), out(256);
    std::vector<float> usb(256, 0.5f);
    for (unsigned f = 0; f < 128; ++f) { input[2 * f] = int32_t(0.5 * 2147483647.0); input[2 * f + 1] = int32_t(0.25 * 2147483647.0); }
    // The mic taken: nothing on its meter or heard through Monitor, though the ADC brings the rear inputs' sound.
    auto host = std::make_unique<EngineHost>();
    host->load(*project, *bank);
    host->send({Command::Type::RecordSource, uint8_t(InputSource::Mic)});
    host->send({Command::Type::Monitor, 0, 0, 0, 1});
    host->send({Command::Type::InputTaken, 0, 0, 0, 1});
    for (unsigned n = 0; n < 4; ++n) host->render(out.data(), 128, 1, input.data(), nullptr, usb.data());
    CHECK(host->input_taken() && host->in_level(0) == 0 && host->in_level(1) == 0 && host->out_level(0) == 0);
    // Free again: heard at once (the mic is both ADC sides' average).
    host->send({Command::Type::InputTaken, 0, 0, 0, 0});
    for (unsigned n = 0; n < 4; ++n) host->render(out.data(), 128, 1, input.data(), nullptr, usb.data());
    CHECK(!host->input_taken() && std::fabs(host->in_level(0) - 0.375f) < 1e-3f && std::fabs(host->in_level(1) - 0.375f) < 1e-3f);
    // USB-C does not use the ADC: heard whatever the flag says.
    host = std::make_unique<EngineHost>();
    host->load(*project, *bank);
    host->send({Command::Type::RecordSource, uint8_t(InputSource::Usb)});
    host->send({Command::Type::InputTaken, 0, 0, 0, 1});
    for (unsigned n = 0; n < 4; ++n) host->render(out.data(), 128, 1, input.data(), nullptr, usb.data());
    CHECK(std::fabs(host->in_level(0) - 0.5f) < 1e-3f);
}


// Parity phase 6: Input Config, the input path, Sample Record's Seq take,
// Audio Recall's ring, renders of the mix and Song -> new pattern.
// The meter lights beside the screen, as stock lights them: the engine's left and right levels (the input source,
// and the mix before main volume), up at once and falling 60 dB a second, the LEDs a strip lights for them, read
// every 50 ms.
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
    // Monitor at a centred pad's pan law, as the take will play back from a kit pad at unity; main volume 2 unheard.
    const float centre = std::cos(dsp::pi / 4);
    CHECK(std::fabs(host->out_level(0) - 0.5f * centre) < 1e-3f && std::fabs(host->out_level(1) - 0.25f * centre) < 1e-3f);
    host->send({Command::Type::Monitor, 0, 0, 0, 0});
    for (unsigned n = 0; n < unsigned(dsp::sample_rate()) / 128; ++n) host->render(out.data(), 128, 2, silent.data());
    float fell = 20 * std::log10(host->in_level(0) / 0.5f), fell_out = 20 * std::log10(host->out_level(1) / (0.25f * centre));
    CHECK(fell < -59 && fell > -61 && fell_out < -59 && fell_out > -61);
    for (unsigned n = 0; n < unsigned(dsp::sample_rate()) / 128; ++n) host->render(out.data(), 128, 2, silent.data());
    CHECK(host->in_level(0) == 0 && host->out_level(0) == 0); // silent below -100 dBFS

    // A bar up to the level, nothing held, the same for the mix and the input: green past -24.1, -12.1 and -6.1
    // dBFS, yellow past -3.1, red only past -0.1.
    auto at = [](float db) { return std::pow(10.0f, db / 20); };
    CHECK(meter_lit(0) == 0 && meter_lit(-0.5f) == 0 && meter_lit(at(-30)) == 0 && meter_lit(at(-24.2f)) == 0);
    CHECK(meter_lit(at(-24)) == 0b00001 && meter_lit(at(-12)) == 0b00011 && meter_lit(at(-6)) == 0b00111);
    CHECK(meter_lit(at(-3)) == 0b01111 && meter_lit(at(-0.5f)) == 0b01111 && meter_lit(at(-0.2f)) == 0b01111);
    CHECK(meter_lit(at(-0.05f)) == surface_meter_all && meter_lit(at(0)) == surface_meter_all && meter_lit(at(6)) == surface_meter_all);
    // Read every 50 ms, and at once when the source changes.
    MeterReads reads;
    float level = at(0);
    auto now_lit = [&](uint32_t now, bool input) { return reads.at(now, input, [&](unsigned) { return level; })[0]; };
    CHECK(now_lit(1000, false) == surface_meter_all);
    level = at(-12);
    CHECK(now_lit(1049, false) == surface_meter_all && now_lit(1050, false) == 0b00011);
    level = at(-30);
    CHECK(now_lit(1060, false) == 0b00011 && now_lit(1061, true) == 0);
    level = at(-6);
    CHECK(now_lit(1110, true) == 0 && now_lit(1111, true) == 0b00111);
    reads = {}; // the ms clock wrapping
    CHECK(now_lit(uint32_t(-20), false) == 0b00111);
    level = at(0);
    CHECK(now_lit(10, false) == 0b00111 && now_lit(30, false) == surface_meter_all);

    // The master's limit: straight to -1 dBFS (a full-scale sample at -1.5 untouched), then round to full scale.
    CHECK(dsp::master_clip(0.5f) == 0.5f && dsp::master_clip(-0.85f) == -0.85f);
    CHECK(dsp::master_clip(1.0f) > 0.95f && dsp::master_clip(1.0f) < 1.0f && dsp::master_clip(4.0f) <= 1.0f);
    CHECK(dsp::master_clip(-1.0f) == -dsp::master_clip(1.0f));
}

// Light show (Sample + Mute, shell/light_show.h): each style keeps time with the beat and stays still between
// beats; Chase walks the pads in sixteenths; Sweep crosses the panel; Ripple's rings come only from presses and
// beat 1; Levels follows the mix; a press flashes its pad.
void light_show_lights() {
    using B = SurfaceButton;
    using S = ShowStyle;
    auto peak = [](Rgb c) { return std::max({c.r, c.g, c.b}); };
    auto total = [&](const ShowFrame& f) { unsigned sum = 0; for (auto c : f.lights.pads) sum += peak(c); return sum; };
    ShowClock clock;
    clock.playing = true; clock.bar_beats = 4; clock.now = 10000;
    auto at = [&](LightShow& show, S style, double beats, uint32_t now = 10000) { clock.beats = beats; clock.now = now; return show.frame(clock, style); };
    for (unsigned n = 0; n < unsigned(S::Count); ++n) CHECK(show_style_names[n] && *show_style_names[n]);

    // Pulse: the beat's hit falls through the beat; beat 1 red on white / red buttons, never on red-only ones.
    LightShow show;
    auto hit = at(show, S::Pulse, 0), mid = at(show, S::Pulse, 0.5), beat2 = at(show, S::Pulse, 1);
    CHECK(total(hit) > total(mid) && total(hit) > total(beat2) && total(beat2) > total(mid));
    CHECK(hit.lights.buttons[size_t(B::Chop)] == ButtonLight::RedBright && beat2.lights.buttons[size_t(B::Chop)] != ButtonLight::RedBright);
    CHECK(hit.lights.buttons[size_t(B::SampleRecord)] != ButtonLight::RedBright);
    CHECK(hit.lights.buttons[size_t(B::Play)] == ButtonLight::Bright);
    // The same moment twice is the same frame: nothing random.
    auto again = at(show, S::Pulse, 0.5);
    CHECK(std::memcmp(&again.lights, &mid.lights, sizeof mid.lights) == 0);
    for (unsigned eighth = 0; eighth < 8; ++eighth) CHECK(at(show, S::Pulse, eighth * 0.5 + 0.1).lights.banks[eighth]);
    clock.playing = false; // stopped: half as bright, no red
    auto stopped = at(show, S::Pulse, 0);
    CHECK(total(stopped) < total(hit) && stopped.lights.buttons[size_t(B::Chop)] != ButtonLight::RedBright);
    clock.playing = true;

    // Chase: the brightest pad is the bar's sixteenth.
    for (unsigned step = 0; step < 16; ++step) {
        auto f = at(show, S::Chase, step * 0.25 + 0.01);
        unsigned best = 0;
        for (unsigned p = 1; p < 16; ++p) if (peak(f.lights.pads[p]) > peak(f.lights.pads[best])) best = p;
        CHECK(best == step);
    }
    // Sweep: left buttons lit early in beat 1, right buttons late; back the other way in beat 2.
    auto left = at(show, S::Sweep, 0.02), right = at(show, S::Sweep, 0.98), back = at(show, S::Sweep, 1.98);
    CHECK(left.lights.buttons[size_t(B::Sample)] != ButtonLight::Off && left.lights.buttons[size_t(B::Mute)] == ButtonLight::Off);
    CHECK(right.lights.buttons[size_t(B::Mute)] != ButtonLight::Off && right.lights.buttons[size_t(B::Sample)] == ButtonLight::Off);
    CHECK(back.lights.buttons[size_t(B::Sample)] != ButtonLight::Off);
    CHECK(peak(at(show, S::Sweep, 0.5).lights.pads[5]) > peak(left.lights.pads[5]));

    // Ripple: a press on pad 1 rings out to pad 16; with no press and off beat 1, nothing moves.
    LightShow ripple;
    auto calm = at(ripple, S::Ripple, 1.5, 20000), calm2 = at(ripple, S::Ripple, 1.5, 20400);
    CHECK(std::memcmp(&calm.lights, &calm2.lights, sizeof calm.lights) == 0);
    ripple.tap(0, 1, 20000, S::Ripple);
    CHECK(ripple.newest() == 20001);
    auto pressed = at(ripple, S::Ripple, 1.5, 20000);
    CHECK(peak(pressed.lights.pads[0]) > peak(calm.lights.pads[0]) + 40); // the flash
    auto reach = at(ripple, S::Ripple, 1.5, 20000 + unsigned(std::sqrt(18.0f) / show_light::ring_speed));
    CHECK(peak(reach.lights.pads[15]) > peak(calm.lights.pads[15]) + 10);
    LightShow other; // other styles: a press only flashes its pad
    other.tap(0, 1, 20000, S::Pulse);
    CHECK(other.newest() == 0);
    // Levels: a hit fills the inner columns at once and the outer ones a moment later, then falls within a
    // third of a second; a quiet song fills them as high as a loud one once it has played a while.
    {
        LightShow levels;
        auto play = [&](float level, uint32_t from, uint32_t to) {
            ShowFrame f;
            for (uint32_t t = from; t <= to; t += 5) { clock.level = {level, level}; f = at(levels, S::Levels, 0.5, t); }
            return f;
        };
        auto lit_rows = [&](const ShowFrame& f, unsigned col) { unsigned n = 0; for (unsigned r = 0; r < 4; ++r) n += peak(f.lights.pads[r * 4 + col]) > 40; return n; };
        auto quiet = play(0, 40000, 40500);
        CHECK(lit_rows(quiet, 1) == 0 && !quiet.lights.banks[0]);
        auto hit = play(0.5f, 40505, 40505);
        CHECK(lit_rows(hit, 1) >= 3 && lit_rows(hit, 2) >= 3 && lit_rows(hit, 0) == 0); // inner now, outer not yet
        CHECK(hit.lights.banks[0] && peak(hit.lights.pads[13]) > peak(quiet.lights.pads[13]));
        auto spread = play(0.5f, 40510, 40505 + show_light::level_spread_ms + 10);
        CHECK(lit_rows(spread, 0) >= 3);
        auto fallen = play(0, 40505 + show_light::level_spread_ms + 15, 40505 + show_light::level_spread_ms + 400);
        auto body = [&](const ShowFrame& f, unsigned col) { unsigned n = 0; for (unsigned r = 0; r < 3; ++r) n += peak(f.lights.pads[r * 4 + col]) > 40; return n; };
        CHECK(body(fallen, 1) == 0 && body(fallen, 0) == 0); // the column down; its peak dot still up top
        CHECK(peak(fallen.lights.pads[13]) > 40);
        // The rows differ in colour: bottom and top are not the same drive.
        auto rgb_of = [](Rgb c) { return uint32_t(c.r) << 16 | uint32_t(c.g) << 8 | c.b; };
        CHECK(rgb_of(hit.lights.pads[1]) != rgb_of(hit.lights.pads[13]));
        // A quiet song (-30 dBFS hits on silence): after some seconds it fills as high as the loud one did.
        ShowFrame soft;
        for (uint32_t t = 50000; t < 60000; t += 250) { play(0.03f, t, t + 20); soft = play(0.001f, t + 25, t + 245); }
        soft = play(0.03f, 60000, 60000);
        CHECK(lit_rows(soft, 1) >= 3);
        // A dense song (between -20 and -15 dBFS, never quiet): the columns still rise and fall with it.
        ShowFrame low, high;
        for (uint32_t t = 80000; t < 90000; t += 400) { high = play(0.18f, t, t + 100); low = play(0.1f, t + 105, t + 395); }
        CHECK(lit_rows(high, 1) >= 3 && body(low, 1) <= 1);
        // Turned on in silence, then a dense song starts: it moves from the first second, not once the range has
        // climbed from silence.
        LightShow late;
        clock.level = {0, 0};
        for (uint32_t t = 100000; t < 102000; t += 5) at(late, S::Levels, 0.5, t);
        ShowFrame late_low, late_high;
        for (uint32_t t = 102000; t < 103000; t += 400) {
            for (uint32_t u = t; u <= t + 100; u += 5) { clock.level = {0.18f, 0.18f}; late_high = at(late, S::Levels, 0.5, u); }
            for (uint32_t u = t + 105; u <= t + 395; u += 5) { clock.level = {0.1f, 0.1f}; late_low = at(late, S::Levels, 0.5, u); }
        }
        CHECK(lit_rows(late_high, 1) >= 3 && body(late_low, 1) <= 1);
        // One side alone: its columns only.
        LightShow side;
        clock.level = {0.5f, 0};
        auto left_only = at(side, S::Levels, 0.5, 70000);
        CHECK(lit_rows(left_only, 1) >= 3 && lit_rows(left_only, 2) == 0);
        clock.level = {};
    }
    // Meters: the mix as it is.
    clock.meters = {0b00011, 0b00001};
    CHECK(at(show, S::Pulse, 0.5).meters[0] == 0b00011);
    clock.meters = {};
    // Turned on: dark first, then up over the intro.
    LightShow fresh;
    fresh.start(30000);
    CHECK(total(at(fresh, S::Pulse, 0.5, 30000)) == 0);
    CHECK(total(at(fresh, S::Pulse, 0.5, 30000 + show_light::intro_ms)) > 16 * 5);
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
    // Picking the mic turns Monitor off; leaving it puts Monitor back, even after a turn through other sources.
    in.monitor = Monitor::On; in.set_source(InputSource::Mic, true);
    CHECK(in.monitor == Monitor::Off && !in.mutes_speaker(true));
    in.set_source(InputSource::Rear, true); CHECK(in.monitor == Monitor::On);
    in.set_source(InputSource::Usb, true); CHECK(in.monitor == Monitor::On);
    in.monitor = Monitor::Auto; in.set_source(InputSource::Mic, true); in.monitor = Monitor::On; // turned on by hand: kept while on the mic
    in.set_source(InputSource::Mic, true); CHECK(in.monitor == Monitor::On);
    in.set_source(InputSource::Track, true); CHECK(in.monitor == Monitor::Auto);
    // Headphones or line out in: the mic leaves Monitor alone, and leaving the mic changes nothing.
    in.monitor = Monitor::On; in.set_source(InputSource::Mic, false); CHECK(in.monitor == Monitor::On);
    in.monitor = Monitor::Auto; in.set_source(InputSource::Rear, true); CHECK(in.monitor == Monitor::Auto);
    CHECK(std::string(input_route(InputSource::Mic)) == "mic" && std::string(input_route(InputSource::RearR)) == "line"
          && std::string(input_route(InputSource::UsbL)) == "off" && std::string(input_route(InputSource::Resample)) == "off");
    CHECK(rec_bars(rec_free) == 0 && rec_bars(rec_seq) == 0 && rec_bars(2) == 1 && rec_bars(17) == 16 && rec_bars(18) == 24 && rec_bars(rec_lengths - 1) == 128);
    // Kept with the settings; an older settings file (no input keys) gets the defaults.
    Settings saved;
    saved.input.source = InputSource::UsbR; saved.input.monitor = Monitor::On; saved.input.before_mic = int8_t(Monitor::Off); saved.input.threshold = -72;
    saved.input.length = rec_seq; saved.input.fx = true; saved.input.gain = -6;
    Settings back; back.read(saved.text());
    CHECK(back.input.source == InputSource::UsbR && back.input.monitor == Monitor::On && back.input.before_mic == int8_t(Monitor::Off) && back.input.threshold == -72
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
        auto host_held = std::make_unique<EngineHost>(); // on the heap: a test's frame stays under the 8 MB stack
        auto& host = *host_held;
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
            auto host_held = std::make_unique<EngineHost>(); // on the heap: a test's frame stays under the 8 MB stack
            auto& host = *host_held;
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
            auto host_held = std::make_unique<EngineHost>(); // on the heap: a test's frame stays under the 8 MB stack
            auto& host = *host_held;
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
        auto host_held = std::make_unique<EngineHost>(); // on the heap: a test's frame stays under the 8 MB stack
        auto& host = *host_held;
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
        auto host_held = std::make_unique<EngineHost>(); // on the heap: a test's frame stays under the 8 MB stack
        auto& host = *host_held;
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
            auto host_held = std::make_unique<EngineHost>(); // on the heap: a test's frame stays under the 8 MB stack
            auto& host = *host_held;
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
            auto host_held = std::make_unique<EngineHost>(); // on the heap: a test's frame stays under the 8 MB stack
            auto& host = *host_held;
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
        auto host_held = std::make_unique<EngineHost>(); // on the heap: a test's frame stays under the 8 MB stack
        auto& host = *host_held;
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
    ui.page = PageId::Mixer;
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
    CHECK(page_bindings(*project, ui, pattern, false)[0].kind != Binding::Kind::KnobParam); // the mixer's own again
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
    { // Issue 57: a target chosen with the encoder holds; FILTER, ECHO and SPACE no longer take it, the Fader page's
      // own knobs still do, and B2 lets every knob take it again.
        Track plug;
        ::synth(plug, 0);
        plug.fader_learned = fader_learnable(plug, 0, Binding {Kind::Param, plugin_track::Macro1, -100, 100, 0});
        assign_fader_target(plug, FaderTarget::PitchBend);
        CHECK(plug.fader_pin && plug.fader_on && !plug.fader_learned.learned() && fader_target(plug) == FaderTarget::PitchBend);
        for (int m = plugin_track::Macro1; m <= plugin_track::Macro3; ++m) {
            Binding macro {Kind::Param, m, -100, 100, 0};
            CHECK(fader_learnable(plug, 0, macro).learned()); // a macro is one of the track's values
            CHECK(!fader_follow(plug, 0, macro, false).learned() && fader_follow(plug, 0, macro, true).learned());
        }
        plug.fader_pin = false; // B2
        CHECK(fader_follow(plug, 0, Binding {Kind::Param, plugin_track::Macro1, -100, 100, 0}, false).learned());
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
        auto host_held = std::make_unique<EngineHost>(); // on the heap: a test's frame stays under the 8 MB stack
        auto& host = *host_held;
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
    ui.page = PageId::Mixer;
    job = fader_job(*project, ui);
    CHECK(job.source == FaderJob::Source::Page && job.binding.kind == Kind::Level);
    ui.page = PageId::StepEdit; CHECK(fader_job(*project, ui).source == FaderJob::Source::Target); // step entry: no nudge (round 2)
    ui.page = PageId::Compressor; CHECK(fader_job(*project, ui).binding.kind == Kind::CompAmount);
    ui.page = PageId::FlexBeat; CHECK(fader_job(*project, ui).binding.kind == Kind::FlexMix);
    // The sample browser on a kit: its preview's level, kept with the settings (100 as the file is); on a track of
    // notes the track's own target.
    ui.page = PageId::Samples; ui.settings.preview_level = 70;
    job = fader_job(*project, ui);
    CHECK(job.source == FaderJob::Source::Page && job.binding.kind == Kind::PreviewLevel && job.binding.value == 70
          && job.binding.min == 0 && job.binding.max == 127 && !job.centred && !job.bend);
    ui.track = 2; CHECK(fader_job(*project, ui).source == FaderJob::Source::Target);
    ui.track = 3; ui.settings.preview_level = 100;
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
    ui.page = PageId::Mixer; CHECK(fader_job(*project, ui).source == FaderJob::Source::Page);
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
        UiState at; at.track = 3; at.page = PageId::Compressor;
        CHECK(fader_twin(fader_job(*project, at), page_bindings(*project, at, pattern)) == 2); // AMOUNT, K3
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
        at.page = PageId::Mixer; CHECK(fader_job(*project, at).source == FaderJob::Source::Page); // a page's own job still wins
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
        set.midi.sync_in = MidiSync::Clock; set.midi.sync_out = MidiSync::Mtc; set.midi.thru = true; set.midi.program_change = program_plugin;
        set.midi.cv_out = true; set.midi.cv_base = 4; set.midi.cv_division = 24; set.midi.pad_in = false;
        set.midi.preset_cc = 74; set.midi.preset_knob = knob_signed_bit_2; set.midi.send_program = true;
        Settings back; back.read(set.text());
        CHECK(back.midi == set.midi && back.midi.cv_ppqn() == 96);
        Settings older; older.read("pad_response=2\nscreen=80\n");
        CHECK(older.midi == MidiConfig {} && older.pad_response == 2);
        Settings bad; bad.read("cv_base=40\ncv_division=0\nmidi_out_channel=0\nmidi_in_channel=99\nprogram_change=9\n");
        CHECK(bad.midi.cv_base == 8 && bad.midi.cv_division == 1 && bad.midi.out_channel == 1 && bad.midi.in_channel == 16);
        CHECK(bad.midi.program_change == program_plugin);
        Settings knob; knob.read("remote_preset_cc=200\nremote_preset_knob=9\n");
        CHECK(knob.midi.preset_cc == 127 && knob.midi.preset_knob == knob_signed_bit_2);
        CHECK(MidiConfig {}.preset_cc == 0 && MidiConfig {}.preset_knob == knob_binary_offset); // Off; Saadet's and the MiniLab 3's Relative1
        CHECK(!MidiConfig {}.send_program && back.midi.send_program);
        Settings on; on.read("program_change=1\n"); // an older file's On: Sequence, as before
        CHECK(on.midi.program_change == program_sequence);
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
        midi_row_step(set, track, TrackInRow, 1); CHECK(track.midi_in == TrackMidiIn::External && midi_row_value(set, track, TrackInRow) == "External");
        midi_row_step(set, track, TrackInRow, -1); CHECK(midi_row_value(set, track, TrackInRow) == "Any");
        CHECK(midi_row_value(set, track, RemoteChannelRow) == "Off" && midi_row_value(set, track, SceneCcRow) == "104");
        midi_row_step(set, track, RemoteChannelRow, 1); CHECK(set.midi.remote_channel == 1);
        for (int n = 0; n < 200; ++n) midi_row_step(set, track, TrackCcRow, -1);
        CHECK(midi_row_value(set, track, TrackCcRow) == "Off");
        set.midi.remote_channel = 0; set.midi.track_cc = 102;
        midi_row_step(set, track, PadOutRow, -1); CHECK(set.midi.pad_out == PadMidiOut::Empty);
        midi_row_step(set, track, SyncOutRow, 1); CHECK(midi_row_value(set, track, SyncOutRow) == "MIDI Clock");
        midi_row_step(set, track, FaderTakeoverRow, 1); CHECK(set.fader_takeover == Takeover::Scaled);
        CHECK(midi_row_value(set, track, ProgramChangeRow) == "Off" && std::string(midi_row_name(ResetRow)) == "Reset factory settings");
        // Receive program change: Off, Sequence, Plug-in, round; K1's list the same three.
        CHECK(midi_row_choices(ProgramChangeRow) == 3);
        midi_row_step(set, track, ProgramChangeRow, 1); CHECK(midi_row_value(set, track, ProgramChangeRow) == "Sequence");
        midi_row_step(set, track, ProgramChangeRow, 1); CHECK(midi_row_value(set, track, ProgramChangeRow) == "Plug-in");
        CHECK(midi_row_choice(set, track, ProgramChangeRow) == 2);
        midi_row_step(set, track, ProgramChangeRow, 1); CHECK(set.midi.program_change == program_off);
        midi_row_step(set, track, ProgramChangeRow, -1); CHECK(set.midi.program_change == program_plugin);
        midi_row_set(set, track, ProgramChangeRow, 1); CHECK(set.midi.program_change == program_sequence);
        midi_row_set(set, track, ProgramChangeRow, 0);
        // Send program change: On / Off, after Receive program change.
        CHECK(std::string(midi_row_name(SendProgramRow)) == "Send program change" && SendProgramRow == ProgramChangeRow + 1);
        CHECK(midi_row_value(set, track, SendProgramRow) == "Off" && midi_row_choices(SendProgramRow) == 2);
        midi_row_step(set, track, SendProgramRow, 1); CHECK(set.midi.send_program && midi_row_value(set, track, SendProgramRow) == "On");
        midi_row_set(set, track, SendProgramRow, 0); CHECK(!set.midi.send_program && midi_row_choice(set, track, SendProgramRow) == 0);
        // The preset knob's rows, after Remote sequence CC; Push learns the four CC rows, not the knob's mode.
        CHECK(std::string(midi_row_name(PresetCcRow)) == "Remote preset CC" && midi_row_value(set, track, PresetCcRow) == "Off");
        CHECK(midi_row_choices(PresetCcRow) == 128 && midi_row_choices(PresetKnobRow) == 5);
        CHECK(midi_row_value(set, track, PresetKnobRow) == "Binary offset");
        midi_row_step(set, track, PresetKnobRow, 1); CHECK(midi_row_value(set, track, PresetKnobRow) == "2's complement");
        midi_row_step(set, track, PresetKnobRow, -1); midi_row_step(set, track, PresetKnobRow, -1);
        CHECK(midi_row_value(set, track, PresetKnobRow) == "Absolute");
        midi_row_set(set, track, PresetKnobRow, 4); CHECK(set.midi.preset_knob == knob_signed_bit_2 && midi_row_choice(set, track, PresetKnobRow) == 4);
        midi_row_set(set, track, PresetCcRow, 74); CHECK(midi_row_value(set, track, PresetCcRow) == "74");
        CHECK(midi_row_learns(TrackCcRow) && midi_row_learns(PatternCcRow) && midi_row_learns(SceneCcRow) && midi_row_learns(PresetCcRow));
        CHECK(!midi_row_learns(PresetKnobRow) && !midi_row_learns(RemoteChannelRow) && !midi_row_learns(PanicRow));
        set.midi.preset_cc = 0; set.midi.preset_knob = knob_binary_offset;
        // Scale on white keys: off at first, a toggle, kept in the settings file, off again after Reset.
        CHECK(std::string(midi_row_name(ScaleKeysRow)) == "Scale on white keys" && midi_row_choices(ScaleKeysRow) == 2);
        CHECK(!set.scale_keys && midi_row_value(set, track, ScaleKeysRow) == "Off" && midi_row_choice(set, track, ScaleKeysRow) == 0);
        midi_row_step(set, track, ScaleKeysRow, 1); CHECK(set.scale_keys && midi_row_value(set, track, ScaleKeysRow) == "On");
        Settings back; back.read(set.text()); CHECK(back.scale_keys);
        Settings older; older.read("pad_response=2\n"); CHECK(!older.scale_keys);
        midi_row_set(set, track, ScaleKeysRow, 0); CHECK(!set.scale_keys);
        set.scale_keys = true; set.reset_factory(); CHECK(!set.scale_keys);
    }
    // Scale on white keys: the white keys play the scale in order, C4 the root nearest it, black keys nothing.
    {
        auto project = std::make_unique<Project>();
        project->root = 0; project->scale = 0; // C Major: every white key as it is
        for (int k : {0, 59, 60, 62, 64, 65, 67, 69, 71, 72, 127}) CHECK(scale_key_note(*project, k) == k);
        for (int k : {1, 61, 63, 66, 68, 70, 126}) CHECK(scale_key_note(*project, k) == -1);
        CHECK(scale_key_note(*project, -1) == -1 && scale_key_note(*project, 128) == -1);
        project->root = 2; // D Major: C4 is D4, B3 the C#4 below it, the top key past 127
        CHECK(scale_key_note(*project, 60) == 62 && scale_key_note(*project, 62) == 64 && scale_key_note(*project, 64) == 66);
        CHECK(scale_key_note(*project, 65) == 67 && scale_key_note(*project, 59) == 61 && scale_key_note(*project, 72) == 74);
        CHECK(scale_key_note(*project, 127) == -1);
        project->root = 9; project->scale = 1; // A Minor: the root the nearest way, down to A3
        CHECK(scale_key_note(*project, 60) == 57 && scale_key_note(*project, 62) == 59 && scale_key_note(*project, 64) == 60);
        CHECK(scale_key_note(*project, 72) == 69 && scale_key_note(*project, 0) == -1);
        project->root = 0; project->scale = 10; // C Minor pent: five notes go on up the white keys, a note a key
        CHECK(std::string(scales()[10].name) == "Minor pent");
        CHECK(scale_key_note(*project, 60) == 60 && scale_key_note(*project, 62) == 63 && scale_key_note(*project, 67) == 70);
        CHECK(scale_key_note(*project, 69) == 72 && scale_key_note(*project, 71) == 75 && scale_key_note(*project, 72) == 77);
        CHECK(scale_key_note(*project, 59) == 58 && scale_key_note(*project, 57) == 55);
        // Where it plays: a synth, keygroup or MIDI track on Scale or Chords, in any scale but Chromatic.
        Track synth; synth.engine = EngineKind::Plugin;
        CHECK(scale_keys_on(*project, synth));
        synth.layout = PadLayout::Chords; CHECK(scale_keys_on(*project, synth));
        synth.layout = PadLayout::Chromatic; CHECK(!scale_keys_on(*project, synth));
        synth.layout = PadLayout::Drums; CHECK(!scale_keys_on(*project, synth));
        synth.layout = PadLayout::Prog; CHECK(!scale_keys_on(*project, synth));
        synth.layout = PadLayout::Scale;
        Track kit; kit.engine = EngineKind::Sampler;
        Track keygroup; keygroup.engine = EngineKind::Keygroup;
        Track midi; midi.engine = EngineKind::Midi;
        Track empty;
        CHECK(!scale_keys_on(*project, kit) && scale_keys_on(*project, keygroup) && scale_keys_on(*project, midi) && !scale_keys_on(*project, empty));
        project->scale = chromatic_scale; CHECK(!scale_keys_on(*project, synth));
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
        // Each track on its own input: a channel of its own counts only from the ports the track listens on; the
        // selected track only from MIDI Config's port.
        config.in_channel = 0;
        CHECK(track_hears(TrackMidiIn::Any, "SerialMIDI", false) && track_hears(TrackMidiIn::Any, "MIDI Gadget", false));
        CHECK(track_hears(TrackMidiIn::External, "SerialMIDI", false) && !track_hears(TrackMidiIn::External, "USB-Audio", false));
        CHECK(track_hears(TrackMidiIn::Usb, "USB-Audio", false) && !track_hears(TrackMidiIn::Usb, "SerialMIDI", false));
        CHECK(!track_hears(TrackMidiIn::Any, "Surface", true));
        uint16_t heard = 1u << 9; // track 10 listens here, track 6 does not
        CHECK(note_tracks(config, own, heard, 2, 3, false) == (1u << 9));
        CHECK(note_tracks(config, own, 0, 2, 3, false) == 0);       // nobody listens, not MIDI Config's port
        CHECK(note_tracks(config, own, 0, 2, 3, true) == (1u << 2)); // MIDI Config's port: the selected track
        CHECK(note_tracks(config, own, heard, 2, 1, false) == 0);
        // Receive program change, Plug-in: the place in the preset list, past 128 with the Bank Select CCs before it.
        ProgramBanks banks;
        CHECK(banks.place(1, 0) == 0 && banks.place(1, 127) == 127);
        banks.control(1, 32, 1); CHECK(banks.place(1, 0) == 128 && banks.place(1, 4) == 132);
        banks.control(1, 0, 1); CHECK(banks.place(1, 0) == (128 + 1) * 128);
        banks.control(1, 7, 99); banks.control(17, 0, 5); banks.control(0, 32, 5);
        CHECK(banks.place(1, 0) == (128 + 1) * 128 && banks.place(2, 5) == 5); // another channel: its own bank
        banks.control(1, 0, 0); banks.control(1, 32, 0); CHECK(banks.place(1, 9) == 9);
        // The plug-ins it reaches: each PLUGIN track's once, never a frozen track's, an empty entry's or a kit's.
        {
            auto p = std::make_unique<Project>();
            std::snprintf(p->plugins[3].path, plugin_path_length, "/media/card/Plugins/Synth.vst3");
            std::snprintf(p->plugins[5].path, plugin_path_length, "/media/card/Plugins/Bass.clap");
            for (unsigned t : {0u, 1u, 2u, 4u}) p->tracks[t].engine = EngineKind::Plugin;
            p->tracks[0].plugin = 3; p->tracks[1].plugin = 3; p->tracks[2].plugin = 5; p->tracks[4].plugin = 7;
            p->tracks[3].engine = EngineKind::Sampler; p->tracks[3].plugin = 5;
            CHECK(program_entries(*p, 0xffff, 0) == ((1u << 3) | (1u << 5)));
            CHECK(program_entries(*p, 1u << 1, 0) == (1u << 3));        // track 2 alone: the entry it shares with track 1
            CHECK(program_entries(*p, 0xffff, 1u << 2) == (1u << 3));   // track 3 frozen
            CHECK(program_entries(*p, (1u << 3) | (1u << 4), 0) == 0);   // a kit, a PLUGIN track with no plug-in
        }
        // Send program change: each PLUGIN track playing the plug-in, on its own channel else MIDI Config's Out, each
        // channel once; Bank Select MSB and LSB, then the program, counted as Receive program change counts them.
        {
            auto p = std::make_unique<Project>();
            MidiConfig out; out.out_channel = 5;
            for (unsigned t : {0u, 1u, 2u, 3u}) { p->tracks[t].engine = EngineKind::Plugin; p->tracks[t].plugin = 3; }
            p->tracks[1].midi_channel = 9; p->tracks[2].midi_channel = 9; p->tracks[3].plugin = 4;
            p->tracks[4].engine = EngineKind::Sampler; p->tracks[4].plugin = 3; p->tracks[4].midi_channel = 12;
            CHECK(program_channels(*p, out, 3) == ((1u << 4) | (1u << 8))); // tracks 1 (Out 5), 2 and 3 (9, once)
            CHECK(program_channels(*p, out, 4) == (1u << 4) && program_channels(*p, out, 7) == 0);
            CHECK((program_bytes(1, 5) == std::array<uint8_t, 8> {0xb0, 0, 0, 0xb0, 32, 0, 0xc0, 5}));
            CHECK((program_bytes(16, 129) == std::array<uint8_t, 8> {0xbf, 0, 0, 0xbf, 32, 1, 0xcf, 1}));
            CHECK((program_bytes(2, (128 + 1) * 128 + 9) == std::array<uint8_t, 8> {0xb1, 0, 1, 0xb1, 32, 1, 0xc1, 9}));
            ProgramBanks back; // what Receive program change makes of it: the same place
            back.control(2, 0, 1); back.control(2, 32, 1); CHECK(back.place(2, 9) == (128 + 1) * 128 + 9);
        }
        // The preset knob: steps a message, + next, - previous, in each of the five ways knobs send.
        CHECK(knob_steps(knob_binary_offset, 65, -1) == 1 && knob_steps(knob_binary_offset, 63, -1) == -1 && knob_steps(knob_binary_offset, 68, -1) == 4);
        CHECK(knob_steps(knob_binary_offset, 64, -1) == 0);
        CHECK(knob_steps(knob_twos_complement, 1, -1) == 1 && knob_steps(knob_twos_complement, 127, -1) == -1 && knob_steps(knob_twos_complement, 124, -1) == -4);
        CHECK(knob_steps(knob_signed_bit, 1, -1) == 1 && knob_steps(knob_signed_bit, 65, -1) == -1 && knob_steps(knob_signed_bit, 67, -1) == -3);
        CHECK(knob_steps(knob_signed_bit_2, 65, -1) == 1 && knob_steps(knob_signed_bit_2, 1, -1) == -1);
        CHECK(knob_steps(knob_absolute, 40, -1) == 0 && knob_steps(knob_absolute, 41, 40) == 1 && knob_steps(knob_absolute, 30, 41) == -1);
        CHECK(knob_steps(knob_absolute, 127, 127) == 0); // at its end: nothing more
        // Learn: turned right, rising values are a knob's position; the same value again a relative knob.
        CHECK(guess_knob({40, 41, 43}) == knob_absolute && guess_knob({65, 65, 65}) == knob_binary_offset);
        CHECK(guess_knob({1, 1, 2}) == knob_twos_complement && guess_knob({127, 127, 127}) == knob_binary_offset);
        CHECK(!learnable_cc(0) && !learnable_cc(32) && !learnable_cc(6) && !learnable_cc(99) && !learnable_cc(123) && learnable_cc(74) && learnable_cc(1));
        // A wait that ran out leaves nothing behind (MPC, 2026-11-27: two values of a first Push and one of a second took
        // the knob, and the next two stepped a plug-in's presets); the control just taken is quiet while it moves on.
        {
            LearnTake take;
            CHECK(!take.take(100, 1, 3, 65, learn_values, 0) && !take.take(100, 1, 3, 65, learn_values, 10));   // the first Push's two
            CHECK(!take.take(200, 1, 3, 65, learn_values, 20000) && !take.take(200, 1, 3, 65, learn_values, 20010)); // a new Push: from 0
            CHECK(take.take(200, 1, 3, 65, learn_values, 20020) && take.channel == 1 && take.cc == 3 && take.values[2] == 65);
            CHECK(take.quiet(20100, 1, 3) && take.quiet(20650, 1, 3));  // still turning: dropped, its rest starts again
            CHECK(!take.quiet(20700, 1, 4) && !take.quiet(20700, 2, 3)); // another control, another channel
            CHECK(!take.quiet(21300, 1, 3));                              // rested: it steps
            CHECK(!take.take(300, 1, 5, 1, learn_values, 30000) && !take.take(300, 1, 6, 1, learn_values, 30010)); // another control: over
            CHECK(take.take(300, 9, 102, 0, 1, 40000) && take.channel == 9 && take.cc == 102); // a track row: the first CC takes it
        }
        // Program changes in quick succession: one waiting per plug-in, the last asked; another plug-in's, a load and
        // a replaced plug-in's (another generation) left as they are.
        {
            struct Job { enum class Kind { Load, Program } kind; unsigned entry; uint32_t generation; int32_t id; };
            std::deque<Job> waiting;
            auto ask = [&](Job j) { if (!replace_waiting_program(waiting, j)) waiting.push_back(j); };
            ask({Job::Kind::Load, 2, 1, 0});
            for (int32_t n = 0; n < 30; ++n) ask({Job::Kind::Program, 2, 1, n});
            ask({Job::Kind::Program, 4, 1, 7});
            ask({Job::Kind::Program, 2, 2, 9});
            CHECK(waiting.size() == 4 && waiting[1].kind == Job::Kind::Program && waiting[1].entry == 2 && waiting[1].id == 29);
            CHECK(waiting[0].kind == Job::Kind::Load && waiting[2].entry == 4 && waiting[2].id == 7 && waiting[3].id == 9);
            waiting.pop_front(); waiting.pop_front();       // the rack took them: the next ask queues again
            ask({Job::Kind::Program, 2, 1, 3});
            CHECK(waiting.size() == 3 && waiting.back().id == 3);
        }
        // Remote: CCs on its channel select a track, launch a pattern or a scene; the rest go on to the tracks.
        MidiConfig remote;
        CHECK(remote_cc(remote, 1, remote.track_cc, 3).kind == RemoteJob::Kind::None); // Off
        remote.remote_channel = 16;
        auto job = remote_cc(remote, 16, remote.track_cc, 3);
        CHECK(job.kind == RemoteJob::Kind::Track && job.index == 3);
        job = remote_cc(remote, 16, remote.pattern_cc, 17);
        CHECK(job.kind == RemoteJob::Kind::Pattern && job.index == 17);
        job = remote_cc(remote, 16, remote.scene_cc, 2);
        CHECK(job.kind == RemoteJob::Kind::Scene && job.index == 2);
        CHECK(remote_cc(remote, 16, remote.track_cc, 16).kind == RemoteJob::Kind::Ignored); // past T16: taken, nothing done
        CHECK(remote_cc(remote, 15, remote.track_cc, 3).kind == RemoteJob::Kind::None);
        CHECK(remote_cc(remote, 16, 7, 3).kind == RemoteJob::Kind::None);
        // The transport's (issue 7): Off until learned; a press (64 up) plays or records, its let go is taken.
        CHECK(remote.play_cc == 0 && remote.record_cc == 0);
        remote.play_cc = 64; remote.record_cc = 65;
        CHECK(remote_cc(remote, 16, 64, 127).kind == RemoteJob::Kind::Play && remote_cc(remote, 16, 64, 64).kind == RemoteJob::Kind::Play);
        CHECK(remote_cc(remote, 16, 64, 0).kind == RemoteJob::Kind::Ignored && remote_cc(remote, 16, 64, 63).kind == RemoteJob::Kind::Ignored);
        CHECK(remote_cc(remote, 16, 65, 127).kind == RemoteJob::Kind::Record && remote_cc(remote, 16, 65, 0).kind == RemoteJob::Kind::Ignored);
        CHECK(remote_cc(remote, 15, 64, 127).kind == RemoteJob::Kind::None); // another channel: the tracks' sustain pedal
        remote.track_cc = 0;
        CHECK(remote_cc(remote, 16, 0, 3).kind == RemoteJob::Kind::None); // a CC row Off
        std::ostringstream text;
        write_midi_config(text, remote);
        MidiConfig back;
        std::istringstream lines(text.str());
        for (std::string line; std::getline(lines, line);) {
            auto eq = line.find('=');
            CHECK(eq != std::string::npos && read_midi_config(back, line.substr(0, eq), std::stoi(line.substr(eq + 1))));
        }
        CHECK(back == remote);
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
    // The tempo clock in sets, held through its wobble (an SMK-37 at 120 read 119.9 and 119.8 now and then, and the
    // header shook); readings on one side for four beats are taken, half a BPM or more at once, a pause starts over.
    {
        TempoSteady t;
        CHECK(t.take(0) == 0 && t.take(1200) == 1200);
        for (unsigned n = 0; n < 500; ++n) CHECK(t.take(n % 7 == 3 ? 1198 : n % 11 == 5 ? 1202 : n % 5 == 1 ? 1199 : 1200) == 1200);
        for (unsigned n = 0; n < 60; ++n) CHECK(t.take(n % 2 ? 1201 : 1199) == 1200); // either side: never taken
        CHECK(t.take(1200) == 1200);       // back on it: the count starts over
        for (unsigned n = 1; n < TempoSteady::steady_pulses; ++n) CHECK(t.take(n % 3 ? 1202 : 1203) == 1200);
        CHECK(t.take(1202) == 1202);       // a slow ramp: taken after four beats on one side
        CHECK(t.take(1207) == 1207);       // half a BPM: at once
        CHECK(t.take(0) == 1207);          // no reading: kept
        t.reset(); CHECK(t.take(900) == 900);
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
    // Out at once when played by hand: when a frame is rendered, heard_at() less the latency.
    {
        AudioClock clock;
        clock.set_format(48000, 384);
        clock.stamp(1000000000, 0);
        CHECK(clock.rendered_at(480) == 1000000000 + 10000000 && clock.heard_at(480) - clock.rendered_at(480) == 8000000);
    }
    // MIDI out waiting for its time, sorted; a note played by hand goes ahead of the same key's sequenced notes waiting.
    {
        using K = MidiOutEvent::Kind;
        auto note = [](K kind, uint8_t key, bool live = false, uint8_t channel = 3) {
            MidiOutEvent e; e.kind = kind; e.channel = channel; e.a = key; e.live = live; e.dest = MidiDest::External; e.track = 0;
            return e;
        };
        MidiOutQueue q;
        q.add(note(K::NoteOn, 60), 100);
        q.add(note(K::NoteOn, 62), 100);
        q.add(note(K::NoteOff, 60), 300);
        q.add(note(K::NoteOn, 64), 200);
        q.add(note(K::NoteOn, 60, false, 4), 100); // another channel
        CHECK(q.size() == 5 && q[0].event.a == 60 && q[1].event.a == 62 && q[2].event.channel == 4 && q[3].event.a == 64 && q[4].event.kind == K::NoteOff);
        // A hand's Note On for 60: the sequenced On waiting is dropped (nothing would end it), its Off goes first.
        auto hand = note(K::NoteOn, 60, true);
        CHECK(q.supersede(hand, 50) == 2);
        q.add(hand, 50);
        CHECK(q.size() == 5 && q[0].event.kind == K::NoteOff && q[0].event.a == 60 && q[0].due == 50 && q[1].event.live && q[1].event.kind == K::NoteOn);
        CHECK(q[2].event.a == 62 && q[3].event.channel == 4 && q[4].event.a == 64);
        q.drop_front(2);
        // A hand's Note Off for 62: its sequenced On and Off waiting both go (its own Off ends the key); a hand's note
        // still waiting (a DELAY) stays.
        q.add(note(K::NoteOff, 62), 250);
        q.add(note(K::NoteOn, 62, true), 400);
        CHECK(q.supersede(note(K::NoteOff, 62, true), 60) == 2 && q.size() == 3 && q[2].event.live && q[2].event.a == 62);
        // A MIDI track's USB device by its name: only that track's.
        MidiOutQueue usb;
        auto device = [&](K kind, uint8_t track, bool live) { auto e = note(kind, 70, live); e.dest = MidiDest::UsbDevice; e.track = track; return e; };
        usb.add(device(K::NoteOn, 1, false), 100);
        usb.add(device(K::NoteOn, 2, false), 100);
        CHECK(usb.supersede(device(K::NoteOn, 2, true), 10) == 1 && usb.size() == 1 && usb[0].event.track == 1);
        usb.keep_if([](const MidiOutEvent& e) { return e.kind != K::NoteOn; });
        CHECK(usb.size() == 0);
        // A hand's Note Off ends its own track's notes (another MIDI track on the same port and channel plays on); its
        // Note On any track's, as the engine ends the note it replaces.
        MidiOutQueue two;
        auto on_track = [&](K kind, uint8_t track, bool live) { auto e = note(kind, 72, live); e.track = track; return e; };
        two.add(on_track(K::NoteOn, 1, false), 100);
        CHECK(two.supersede(on_track(K::NoteOff, 2, true), 10) == 0 && two.size() == 1);
        CHECK(two.supersede(on_track(K::NoteOn, 2, true), 10) == 1 && two.size() == 0);
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
        auto host_held = std::make_unique<EngineHost>(); // on the heap: a test's frame stays under the 8 MB stack
        auto& host = *host_held;
        host.load(*project, *bank);
        tones_for(host, *project);
        host.set_audio_format(44100, 384);
        std::vector<int32_t> out(256);
        auto drain = [&] { std::vector<MidiOutEvent> e; MidiOutEvent one; while (host.take_midi(one)) e.push_back(one); return e; };
        int woken = 0;
        host.set_hand_waker([](void* n) { ++*static_cast<int*>(n); }, &woken);
        host.send({Command::Type::MidiTrack, 0, 0, 5, 1}); // track 1: channel 5, always
        host.send({Command::Type::MidiTrack, 1, 0, 2, 2}); // track 2: channel 2, empty
        host.send({Command::Type::MidiSync, 0, 0, 0, 1});  // MIDI clock out
        host.render(out.data(), 128, 1);
        CHECK(drain().empty());
        host.send({Command::Type::NoteOn, 0, 0, 50, 99});  // a pad
        host.send({Command::Type::NoteOn, 0, 1, 51, 99});  // a note from MIDI in: never echoed
        host.render(out.data(), 128, 1);
        auto pads = drain();
        // Played by hand: out at once, at the frame it was played (a block before the one it plays at), not when heard.
        CHECK(pads.size() == 1 && pads[0].kind == K::NoteOn && pads[0].channel == 5 && pads[0].a == 50 && pads[0].b == 99 && pads[0].frame == 0 && pads[0].live);
        CHECK(woken == 1); // the MIDI thread woken for it
        host.send({Command::Type::NoteOff, 0, 0, 50});
        host.render(out.data(), 128, 1);
        pads = drain();
        CHECK(pads.size() == 1 && pads[0].kind == K::NoteOff && pads[0].a == 50 && pads[0].frame == 128 && pads[0].live && woken == 2);
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
        for (auto& e : all) CHECK(!e.live); // the pattern's notes and the clock: when heard
        CHECK(woken == 2);
        CHECK(clocks >= 33 && clocks <= 34); // 30720 frames at 918.75 a pulse
        host.send({Command::Type::Stop});
        host.render(out.data(), 128, 1);
        auto stopped = drain();
        CHECK(!stopped.empty() && stopped[0].kind == K::Stop);
        // MIDI clock in: Play waits for the first pulse, then 40 ticks a pulse (a 24th of a quarter), each let through at its frame.
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
        CHECK(host.ticks() >= 1 && host.ticks() <= 40);
        for (unsigned n = 0; n < 20; ++n) host.render(out.data(), 128, 1);
        CHECK(host.ticks() == 40); // no more pulses: it waits
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
        project->tracks[6].midi_channel = 2; project->tracks[6].midi_in = TrackMidiIn::External;
        auto bytes = save_project(*project, *patterns);
        auto loaded = std::make_unique<Project>();
        CHECK(load_project(bytes, *loaded, *patterns).empty());
        CHECK(loaded->tracks[4].midi_channel == 11 && loaded->tracks[3].midi_channel == 0);
        CHECK(loaded->tracks[4].midi_in == TrackMidiIn::Any && loaded->tracks[6].midi_in == TrackMidiIn::External);
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
    // The card sheets: three rows of two in view; the rows scroll once the highlight passes the third.
    using ui::card_sheet_top;
    CHECK(card_sheet_top(0, 4) == 0 && card_sheet_top(3, 4) == 0);          // Roll's four: never scrolls
    CHECK(card_sheet_top(5, 6) == 0);                                         // an effect's six: never scrolls
    CHECK(card_sheet_top(0, 8) == 0 && card_sheet_top(5, 8) == 0);          // a project's eight: rows 1-3 ...
    CHECK(card_sheet_top(6, 8) == 1 && card_sheet_top(7, 8) == 1);          // ... then 2-4 for the last two
    CHECK(card_sheet_top(2, 8) == 0);                                         // back up: the top again
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
    CHECK(std::string(card_title(ui.card)) == "Ejected on the computer");

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
    // B1 Trim -> Layers -> Tune -> Amp Env, B2 Play -> Follow -> Cloud, B3 Mix -> Filter -> Filt Env -> Kit filter (as
    // Stage's kit, hubs plan S-B); another group's key: its first.
    CHECK(sampler_next(PageId::Trim, 0) == PageId::LayersPage && sampler_next(PageId::LayersPage, 0) == PageId::PadPage);
    CHECK(sampler_next(PageId::PadPage, 0) == PageId::EnvelopePage);
    CHECK(sampler_next(PageId::EnvelopePage, 0) == PageId::Trim);
    CHECK(sampler_next(PageId::Trim, 1) == PageId::PlayPage && sampler_next(PageId::PlayPage, 1) == PageId::FollowPage);
    CHECK(sampler_next(PageId::FollowPage, 1) == PageId::CloudPage && sampler_next(PageId::CloudPage, 1) == PageId::PlayPage);
    CHECK(sampler_next(PageId::PadMix, 2) == PageId::PadFilterPage && sampler_next(PageId::PadFilterPage, 2) == PageId::FilterEnvPage);
    CHECK(sampler_next(PageId::FilterEnvPage, 2) == PageId::FilterPage && sampler_next(PageId::FilterPage, 2) == PageId::PadMix);
    CHECK(sampler_next(PageId::Pads, 1) == PageId::PlayPage); // from outside the ten: the group's first
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
    CHECK(scope(K::Tempo).kind == S::Tempo && scope_name(project, ui, scope(K::Tempo)) == "Tempo"); // K1 on TEMPO: one step a turn
    project.tempo = 1200; project.pattern_tempo[3] = 900; project.seq_tempo = true;
    CHECK((tempo_of(project) == TempoState {1200, project.pattern_tempo, true}));
    CHECK(scope(K::ClickLevel).none() && scope(K::PatternLength).none() && scope(K::FileJump).none());
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
    ui.chop_mode = ChopMode::Manual; ui.slices.count = 1; CHECK(press_scope(project, ui, B::Count, true).none()); // Manual's take
    ui.slices.count = 4; CHECK(press_scope(project, ui, B::Count, true).none()); // a pad picks and plays a slice: no step
    ui.erase = true; CHECK(press_scope(project, ui, B::Count, true).kind == S::Chop); ui.erase = false; // Erase + pad: a step
    ui.chop_mode = ChopMode::Threshold; ui.slices.count = 0;
    ui.page = PageId::PlayPage; CHECK(press_scope(project, ui, B::Count, true).kind == S::Pad);
    CHECK(scope_name(project, ui, UndoScope {S::Pad, 2, 17}) == "B02");
    CHECK(scope_name(project, ui, UndoScope {S::Chain, 0, uint16_t(bus_chain(1))}) == "Bus B FX");
    CHECK(scope_name(project, ui, UndoScope {S::Chain, 0, 4}) == "T5 FX");
    CHECK(scope_name(project, ui, UndoScope {S::Chain, 2, uint16_t(pad_chain(2, 3))}) == "A04 FX");
    // Roll R0: on Roll, Steps and Step Edit a press keeps the selected track's pattern, and so do the note cards.
    project.tracks[2].pattern = 3;
    ui.page = PageId::Roll; CHECK(press_scope(project, ui, B::Count, true) == (UndoScope {S::Pattern, 2, 3}));
    ui.page = PageId::StepEdit; CHECK(press_scope(project, ui, B::TopRight, false).kind == S::Pattern);
    CHECK(press_scope(project, ui, B::Play, false).none()); // the transport is not an edit
    CHECK(scope(K::StepVelocity) == (UndoScope {S::Pattern, 2, 3}) && scope(K::StepLength).kind == S::Pattern);
    CHECK(scope(K::StepChance).kind == S::Pattern && scope(K::EventVelocity).kind == S::Pattern && scope(K::EventLength).kind == S::Pattern);
    CHECK(scope(K::Division).none()); // the grid moves no note
    CHECK(scope_name(project, ui, pattern_scope(project, ui)) == "T3 P4");
    ui.page = PageId::Patterns; CHECK(press_scope(project, ui, B::Count, true).none()); // a pad there launches, it writes nothing
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

    // A new project: no tracks (the track types open on track 1), C Chromatic, no notes; every pad its own semitone.
    {
        auto p = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        starting_project(*p, *bank);
        CHECK(p->used_tracks() == 0 && !p->tracks[0].used() && kit_track(*p) == -1);
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
        load_sound(p->tracks[0], EngineKind::Sampler, 0); // a new project's track 1 made a kit
        UiState ui;
        Pattern pattern;
        using K = Binding::Kind;
        for (PageId page : {PageId::Pads, PageId::Tempo}) {
            ui.page = page;
            auto shifted = page_bindings(*p, ui, pattern, true);
            CHECK(shifted[0].kind == K::Root && shifted[1].kind == K::Scale && shifted[1].max == int(scales().size()) - 1);
        }
        // 16 Levels held, on any page (no page of its own): K1 the type; of type Scale K2 the root and K3 the scale,
        // of the others the two ends. Let go, the page's own cards; of type Loop, none of its own.
        ui.page = PageId::Stage; ui.levels_held = true;
        p->tracks[0].levels_kind = uint8_t(LevelsKind::Scale);
        auto levels = page_bindings(*p, ui, pattern, false);
        CHECK(levels[0].kind == K::LevelsType && levels[1].kind == K::Root && levels[2].kind == K::Scale && levels[2].max == int(scales().size()) - 1);
        p->tracks[0].levels_kind = uint8_t(LevelsKind::Tune);
        levels = page_bindings(*p, ui, pattern, false);
        CHECK(levels[1].kind == K::LevelsLow && levels[2].kind == K::LevelsHigh && levels[2].value == 12);
        p->tracks[0].levels_kind = uint8_t(LevelsKind::Chords); // the progression's key and voicing
        levels = page_bindings(*p, ui, pattern, false);
        CHECK(levels[0].kind == K::LevelsType && levels[1].kind == K::ProgKey && levels[2].kind == K::ProgVoicing);
        p->tracks[0].levels_kind = uint8_t(LevelsKind::Tune);
        ui.levels_held = false;
        CHECK(page_bindings(*p, ui, pattern, false)[0].kind != K::LevelsType);
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
        std::snprintf(from->samples.edit(slot).path, sizeof(SampleRef::path), "/card/Samples/%s.wav", name);
        std::snprintf(from->samples.edit(slot).name, sizeof(SampleRef::name), "%s", name);
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

    // LOAD KIT's banks: one bank of a kit into one bank of the track, only its filled pads; the rest stays.
    auto counts = kit_bank_pads(kit);
    CHECK(counts[0] == 3 && counts[1] == 0 && counts[2] == 4 && counts[7] == 0);
    Track mine = other;
    mine.pads[48].sample = 50; mine.pads[49].sample = 51; mine.pads[63].sample = 52; mine.set_pad_muted(48, true);
    mine.pads[0].sample = 53; mine.params[sampler::Filter] = 12;
    asked = 0;
    auto in = import_kit_bank(*from, 6, 2, 3, mine, [&](const SampleRef& r) { ++asked; return std::string(r.name) == "KICK" ? 60 : 61; });
    CHECK(asked == 2 && in.pads == 3 && in.missing == 1 && in.replaced == 3); // pad 34's slot has no file
    CHECK(mine.pads[48].sample == 61 && mine.pads[48].pitch == -3 && mine.pad_fx.at[48].slots[0].kind == uint8_t(EffectKind::Drive));
    CHECK(mine.pads[49].sample == 61 && mine.pads[63].sample == 60 && mine.pads[63].group == 3 && !mine.pad_muted(48));
    CHECK(!mine.pads[50].used() && mine.pads[0].sample == 53 && mine.params[sampler::Filter] == 12); // the rest as it was
    Track same = mine;
    in = import_kit_bank(*from, 6, 1, 0, same, [&](const SampleRef&) { return 70; }); // an empty bank of the kit
    CHECK(in.pads == 0 && in.replaced == 0 && same.pads[0].sample == 53);

    // Erase + an empty pad: the pads after it move down one through its bank and the next; the bank after stays.
    {
        Track g {};
        g.engine = EngineKind::Sampler;
        for (unsigned n = 0; n < 18; ++n) g.pads[n].sample = uint16_t(n + 1); // A01-A16, B01, B02
        g.pads[32].sample = 99;                                    // C01
        g.pads[4] = PadSound {};                                  // A05 deleted
        g.pad_fx.at[6].slots[0].kind = uint8_t(EffectKind::Drive); // A07's chain
        g.set_pad_muted(9, true);                                 // A10 muted
        g.last_pad = 15; g.levels_pad = 2;
        g.zone_trims[0].pad = 6; g.zone_trims[0].slot = 7; g.zone_trims[0].tune = 3;
        CHECK(gap_end(4) == 32 && gap_end(20) == 48 && gap_end(100) == 128 && gap_end(120) == 128);
        CHECK(gap_closed(4, 4) == -1 && gap_closed(5, 4) == 4 && gap_closed(16, 4) == 15 && gap_closed(31, 4) == 30);
        CHECK(gap_closed(32, 4) == 32 && gap_closed(3, 4) == 3 && gap_closed(0, 120) == 0 && gap_closed(127, 120) == 126);
        CHECK(!gap_closes(g, 5) && gap_closes(g, 4) && gap_closes(g, 20) && !gap_closes(g, 40) && !gap_closes(g, 120)); // B05 takes C01; C05 has nothing after it in C and D
        std::array<Pattern, pattern_count> ps {};
        auto note = [](uint32_t tick, unsigned pad) { Event e; e.tick = tick; e.key = uint8_t(sampler_first_note + pad); return e; };
        ps[0].events = {note(0, 4), note(0, 5), note(96, 15), note(96, 16), note(96, 32), note(192, 3)}; // A05's left-over note goes
        MotionPoint m; m.tick = 48; m.param = uint8_t(motion_pad_param(PadParam(0))); m.pad = 6; ps[0].motion.push_back(m);
        MotionPoint track_lane; track_lane.tick = 48; track_lane.param = 1; track_lane.pad = 4; ps[0].motion.push_back(track_lane); // not a pad lane
        ps[3].lanes = {LaneLength {uint8_t(sampler_first_note + 4), 96}, LaneLength {uint8_t(sampler_first_note + 8), 192}};
        close_pad_gap(g, ps, 4);
        CHECK(g.pads[4].sample == 6 && g.pads[14].sample == 16 && g.pads[15].sample == 17 && g.pads[16].sample == 18 && g.pads[3].sample == 4);
        CHECK(!g.pads[17].used() && !g.pads[31].used() && g.pads[32].sample == 99); // B shifted too; C stays
        CHECK(g.pad_fx.at[5].slots[0].kind == uint8_t(EffectKind::Drive) && !g.pad_fx.at[6].count() && g.pad_muted(8) && !g.pad_muted(9));
        CHECK(g.last_pad == 14 && g.levels_pad == 2 && g.zone_trims[0].pad == 5 && g.zone_trims[0].tune == 3);
        CHECK(ps[0].events.size() == 5 && ps[0].events[0].key == sampler_first_note + 4 && ps[0].events[1].key == sampler_first_note + 14);
        CHECK(ps[0].events[2].key == sampler_first_note + 15 && ps[0].events[3].key == sampler_first_note + 32 && ps[0].events[4].key == sampler_first_note + 3);
        CHECK(ps[0].motion.size() == 2 && ps[0].motion[0].param == 1 && ps[0].motion[0].pad == 4 && ps[0].motion[1].pad == 5);
        CHECK(ps[3].lanes.size() == 1 && ps[3].lanes[0].key == sampler_first_note + 7 && ps[3].lanes[0].length == 192);
        // Bank H's gap stays in H: A is never pulled round.
        Track h {};
        h.engine = EngineKind::Sampler;
        h.pads[0].sample = 1; h.pads[121].sample = 2; h.pads[127].sample = 3;
        std::array<Pattern, pattern_count> none {};
        CHECK(gap_closes(h, 120));
        close_pad_gap(h, none, 120);
        CHECK(h.pads[120].sample == 2 && h.pads[126].sample == 3 && !h.pads[127].used() && h.pads[0].sample == 1);
        auto proj = std::make_unique<Project>();
        proj->knob_fx.set_pad(2, 5, true); proj->knob_fx.set_pad(2, 15, true); proj->knob_fx.set_pad(2, 16, true);
        proj->knob_fx.set_pad(2, 32, true); proj->knob_fx.set_pad(3, 5, true);
        proj->compressor.key = 2; proj->compressor.key_pad = 4;
        proj->modulators[0].targets[0].track = 2; proj->modulators[0].targets[0].param = uint8_t(motion_pad_param(PadParam(0)));
        proj->modulators[0].targets[0].pad = 9;
        close_pad_gap_names(*proj, 2, 4);
        CHECK(proj->knob_fx.pad_on(2, 4) && !proj->knob_fx.pad_on(2, 5) && proj->knob_fx.pad_on(2, 14) && proj->knob_fx.pad_on(2, 15));
        CHECK(!proj->knob_fx.pad_on(2, 16) && proj->knob_fx.pad_on(2, 32) && proj->knob_fx.pad_on(3, 5)); // C, other tracks
        CHECK(proj->compressor.key_pad == MasterComp::whole_kit && proj->modulators[0].targets[0].pad == 8);
    }
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
    // A take at 96 kHz is kept as it came, never converted: a second is 96000 frames of its own, 44100 as the project
    // counts them, and as a sample it is those frames, at 96 kHz (no copy at another rate).
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
            CHECK(keep_capture(kept, take->take_rate(), "").empty());
            CHECK(kept.frames == 96000 && kept.rate == 96000 && kept.model_frames() == 44100 && kept.left.size() == 96000);
            float worst = 0; // its frames are what was heard, on 24-bit steps
            for (uint32_t n = 0; n < 96000; ++n) worst = std::max(worst, std::fabs(kept.left[n] - heard[n]));
            CHECK(worst <= 1.0f / 8388607);
            if (hz < 2000) {
                unsigned crossings = 0;
                for (unsigned n = 1001; n < 90000; ++n) crossings += (kept.left[n - 1] < 0) != (kept.left[n] < 0);
                CHECK(std::fabs(crossings / 2.0f / (88999 / 96000.0f) - 1000) < 5);
            }
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
    // 24-bit step), read back as those frames, and its region and a cut made while it ran line up with the sound,
    // counted in the project's 44.1 kHz frames.
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
        CHECK(keep_capture(kept, take->take_rate(), path).empty());
        CHECK(kept.frames == own && kept.rate == 48000 && kept.model_frames() == take->frames());
        SampleData read;
        WavInfo info;
        CHECK(read_wav(path, read, 60, &info).empty() && info.rate == 48000 && info.channels == 2);
        CHECK(read.rate == 48000 && read.frames == own && read.left == kept.left && read.right == kept.right);
        float worst = 0;
        for (uint32_t n = 0; n < own; ++n)
            worst = std::max({worst, std::fabs(read.left[n] - heard_l[n]), std::fabs(read.right[n] - heard_r[n])});
        CHECK(worst <= 2.0f / 8388607);
        // The hit where the cut was, as the project counts it.
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
        CHECK(read_wav(path, mix, 60, &info).empty() && info.rate == 48000 && mix.frames == 128 && mix.rate == 48000);
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
    // Screen Mirror: off, 1280 x 960 at 30 fps until asked for; kept and read back; the size stops at the list's
    // ends; a size past the list (an older build's 1920 x 1440 or 2560 x 1920) reads as its last.
    {
        Settings a;
        CHECK(!a.mirror && a.value_text(Settings::Mirror) == "Off" && a.value_text(Settings::MirrorSize) == "1280 x 960");
        CHECK(a.value_text(Settings::MirrorRate) == "30 fps" && a.mirror_mode() == (ScreenMirrorMode {false, 1280, 960, 30}));
        a.step(Settings::Mirror, 1); a.step(Settings::MirrorSize, -1); a.step(Settings::MirrorRate, 1);
        CHECK(a.mirror && a.value_text(Settings::Mirror) == "On" && a.mirror_mode() == (ScreenMirrorMode {true, 1280, 720, 60}));
        CHECK(a.value_text(Settings::MirrorRate) == "60 fps");
        for (int n = 0; n < 5; ++n) a.step(Settings::MirrorSize, -1);
        CHECK(a.value_text(Settings::MirrorSize) == "320 x 240");
        for (int n = 0; n < 5; ++n) a.step(Settings::MirrorSize, 1);
        CHECK(a.value_text(Settings::MirrorSize) == "1280 x 960");
        Settings b;
        b.read(a.text());
        CHECK(b.mirror_mode() == (ScreenMirrorMode {true, 1280, 960, 60}));
        b.read("mirror_size=5\nscreen_mirror=0\nmirror_fps=45\n");
        CHECK(!b.mirror && b.mirror_size == 3 && b.mirror_fps == 30);
        CHECK(std::string(Settings::row_name(Settings::Mirror)) == "Screen mirror" && std::string(Settings::row_name(Settings::MirrorSize)) == "Resolution");
        CHECK(std::string(Settings::row_name(Settings::MirrorRate)) == "Frame rate" && std::string(Settings::row_name(Settings::Screen)) == "Brightness");
        b.mirror = true; b.mirror_fps = 60; b.reset_factory();
        CHECK(!b.mirror && b.mirror_size == 3 && b.mirror_fps == 30);
    }
    // File sharing: on until turned off, kept and read back; a file from before the row reads as on.
    {
        Settings a;
        CHECK(a.file_sharing && a.value_text(Settings::FileSharingRow) == "On" && std::string(Settings::row_name(Settings::FileSharingRow)) == "File sharing");
        a.step(Settings::FileSharingRow, 1);
        CHECK(!a.file_sharing && a.value_text(Settings::FileSharingRow) == "Off" && a.text().find("\nfile_sharing=0\n") != std::string::npos);
        Settings b;
        b.read(a.text()); CHECK(!b.file_sharing);
        Settings c;
        c.read("screen_mirror=1\n"); CHECK(c.file_sharing);
        b.reset_factory(); CHECK(b.file_sharing);
    }
    // Screensaver (shell/screensaver.h): Off until picked, kept and read back; it waits the minutes after the last
    // control and never while busy; the logo stays on the screen and counts its bounces.
    {
        Settings a;
        CHECK(a.screensaver == 0 && a.value_text(Settings::ScreensaverRow) == "Off" && std::string(Settings::row_name(Settings::ScreensaverRow)) == "Screensaver");
        a.step(Settings::ScreensaverRow, 1);
        CHECK(a.value_text(Settings::ScreensaverRow) == "1 min" && a.text().find("\nscreensaver=1\n") != std::string::npos);
        for (int n = 0; n < 9; ++n) a.step(Settings::ScreensaverRow, 1);
        CHECK(a.value_text(Settings::ScreensaverRow) == "30 min");
        Settings b;
        b.read(a.text()); CHECK(b.screensaver == 5);
        b.read("screensaver=99\n"); CHECK(b.screensaver == 5);
        b.reset_factory(); CHECK(b.screensaver == 0);
        CHECK(!screensaver::due(0, 1u << 31, 0, false));
        CHECK(!screensaver::due(1, 59999, 0, false) && screensaver::due(1, 60000, 0, false) && !screensaver::due(1, 60000, 0, true));
        CHECK(!screensaver::due(1, 0xffffff00u + 59999u, 0xffffff00u, false) && screensaver::due(1, 0xffffff00u + 60000u, 0xffffff00u, false)); // past the clock's wrap
        // A pad hit stamped on the MIDI thread after the frame read its clock: just touched, not four billion ms ago.
        CHECK(!screensaver::due(1, 500000, 500001, false) && !screensaver::due(5, 500000, 500040, false));
        // The backlight while it shows: the MPC's six steps down to the second, never up, back when it ends.
        CHECK(screensaver::backlight(6, 6, true) == 2 && screensaver::backlight(3, 6, true) == 2);
        CHECK(screensaver::backlight(2, 6, true) == 2 && screensaver::backlight(1, 6, true) == 1);
        CHECK(screensaver::backlight(6, 6, false) == 6 && screensaver::backlight(-1, 6, true) == -1 && screensaver::backlight(1, 1, true) == 1);
        auto p = screensaver::place(0, 135, 23, 320, 240, 400, 30);
        CHECK(p.x == 400 % 185 && p.y == 30 && p.bounces == 0);
        p = screensaver::place(5442, 135, 23, 320, 240, 0, 0); // just past the right edge: back by 28 milli-px
        CHECK(p.x == 184 && p.y == 141 && p.bounces == 1);
        p = screensaver::place(10882, 135, 23, 320, 240, 0, 0);
        CHECK(p.x == 0 && p.y == 151 && p.bounces == 2); // back at the left, and off the bottom once
        unsigned bounces = 0;
        bool inside = true, rising = true;
        for (uint32_t ms = 0; ms < 600000; ms += 17) {
            auto at = screensaver::place(ms, 135, 23, 320, 240, 77, 190);
            inside = inside && at.x >= 0 && at.x <= 185 && at.y >= 0 && at.y <= 217;
            rising = rising && at.bounces >= bounces;
            bounces = at.bounces;
        }
        CHECK(inside && rising && bounces > 150);
        CHECK(screensaver::place(1000, 320, 240, 320, 240, 5, 5).x == 0);
    }
    // LIGHTS: Pad fade 100 ms and Tap Tempo light on until changed; kept and read back; the fade stops at the list's ends.
    {
        Settings a;
        CHECK(a.fade_ms() == 100 && a.value_text(Settings::PadFade) == "100 ms" && a.tap_light && a.value_text(Settings::TapLight) == "On");
        a.step(Settings::PadFade, -1); a.step(Settings::PadFade, -1); a.step(Settings::TapLight, 1);
        CHECK(a.fade_ms() == 60 && !a.tap_light && a.value_text(Settings::TapLight) == "Off");
        for (int n = 0; n < 6; ++n) a.step(Settings::PadFade, 1);
        CHECK(a.value_text(Settings::PadFade) == "250 ms");
        Settings b;
        b.read(a.text());
        CHECK(b.fade_ms() == 250 && !b.tap_light);
        b.read("pad_fade=9\n");
        CHECK(b.fade_ms() == 250);
        CHECK(std::string(Settings::row_name(Settings::PadFade)) == "Pad fade" && std::string(Settings::row_name(Settings::TapLight)) == "Tap Tempo light");
    }
    { // Repeat velocity: Strike by default; Pressure kept in the file, and off with Full Level or Pad pressure off
        Settings a;
        CHECK(!a.repeats_by_pressure() && a.value_text(Settings::RepeatVelocity) == "Strike");
        a.step(Settings::RepeatVelocity, 1);
        CHECK(a.repeats_by_pressure() && a.value_text(Settings::RepeatVelocity) == "Pressure");
        Settings b;
        b.read(a.text());
        CHECK(b.repeats_by_pressure() && std::string(Settings::row_name(Settings::RepeatVelocity)) == "Repeat velocity");
        b.toggle_full_level();
        CHECK(!b.repeats_by_pressure());
        b.toggle_full_level(); b.pressure = false;
        CHECK(!b.repeats_by_pressure());
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
    // Settings' and MIDI Config's rows as lists for K1: every value of every row is reached, read back and named.
    {
        Settings set;
        for (unsigned row = 0; row < Settings::Rows; ++row) {
            CHECK(set.choices(row) >= 2);
            for (unsigned n = 0; n < set.choices(row); ++n) {
                set.set_choice(row, n);
                CHECK(set.choice(row) == n && !set.value_text(row).empty());
            }
            set.set_choice(row, 99); CHECK(set.choice(row) == set.choices(row) - 1); // past the end: the last
        }
        Settings fresh;
        CHECK(fresh.choice(Settings::Screen) == 8 && fresh.choice(Settings::SampleRate) == 0); // 100 %, 44.1 kHz
        fresh.set_choice(Settings::Screen, 0); CHECK(fresh.screen == 20);
        CHECK(Settings::reopens(Settings::SampleRate) && Settings::reopens(Settings::Mirror) && !Settings::reopens(Settings::Palette));
        Track track;
        for (unsigned row = 0; row < MidiRows; ++row) {
            unsigned count = midi_row_choices(row);
            CHECK((count == 0) == (row == PanicRow || row == ResetRow));
            for (unsigned n = 0; n < count; ++n) {
                midi_row_set(set, track, row, n);
                CHECK(midi_row_choice(set, track, row) == n && !midi_row_value(set, track, row).empty());
            }
        }
        Settings m; Track t;
        midi_row_set(m, t, CvBaseRow, 0); midi_row_set(m, t, CvDivisionRow, 23); midi_row_set(m, t, TrackChannelRow, 5);
        CHECK(m.midi.cv_base == 1 && m.midi.cv_division == 24 && t.midi_channel == 5);
        midi_row_set(m, t, FaderTakeoverRow, 2); CHECK(m.fader_takeover == Takeover::Instant); // the takeover rows are Settings' own
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
        auto host_held = std::make_unique<EngineHost>(); // on the heap: a test's frame stays under the 8 MB stack
        auto& host = *host_held;
        host.load(*project, *bank);
        tones_for(host, *project);
        host.set_audio_format(44100, 384);
        std::vector<int32_t> out(256);
        auto drain = [&] { std::vector<MidiOutEvent> e; MidiOutEvent one; while (host.take_midi(one)) e.push_back(one); return e; };
        uint64_t rendered = 0;
        auto block = [&] { host.render(out.data(), 128, 1); rendered += 128; return drain(); };
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
        CHECK(pads[0].live && pads[1].live);                // played by hand, Pad MIDI Out too: out at once
        // A new channel ends the held note on the old one, when heard (a sequenced Note On may still wait for it).
        host.send({Command::Type::Param, 0, 0, midi_track::Channel, 11});
        auto moved = block();
        CHECK(moved.size() == 1 && moved[0].kind == K::NoteOff && moved[0].channel == 10 && moved[0].a == 48 && moved[0].dest == MidiDest::UsbC && moved[0].track == 0);
        CHECK(!moved[0].live);
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
        // Its pattern plays on its port and channel; the next pattern launched ends what the old one held. A negative
        // DELAY brings its sequenced notes earlier, by no more than the output's latency.
        host.send({Command::Type::Param, 0, 0, midi_track::Port, midi_track::AllUsb});
        host.send({Command::Type::Param, 0, 0, midi_track::Delay, -50});
        block();
        uint64_t play_at = rendered;
        host.send({Command::Type::Play});
        std::vector<MidiOutEvent> all;
        for (unsigned n = 0; n < 120; ++n) for (auto& e : block()) all.push_back(e); // two sixteenths: 11025 frames
        bool on = false, off = false;
        const uint64_t early = 2205; // 50 ms at 44.1 kHz: past the output's latency, the sequencer plays it ahead (play_ahead())
        for (auto& e : all) {
            if (e.kind == K::NoteOn && e.a == 60) on = e.channel == 11 && e.dest == MidiDest::AllUsb && !e.live && e.frame == (play_at > early ? play_at - early : 0);
            if (e.kind == K::NoteOff && e.a == 60) off = true;
        }
        CHECK(on && off);
        host.send({Command::Type::Param, 0, 0, midi_track::Delay, 0});
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
        auto host_held = std::make_unique<EngineHost>(); // on the heap: a test's frame stays under the 8 MB stack
        auto& host = *host_held;
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
    // Every engine's table: three buttons of one to five pages (a MIDI track's B3 EDIT opens the Edit page), each named,
    // its parameters the engine's own.
    for (EngineKind kind : {EngineKind::None, EngineKind::Sampler, EngineKind::Midi, EngineKind::Plugin}) {
        const auto& g = stage_groups(kind);
        const auto& info = engine_info(kind);
        for (const auto& group : g.groups) {
            CHECK(group.count >= 1 && group.count <= 5);
            for (unsigned n = 0; n < group.count; ++n) {
                const auto& page = group.pages[n];
                CHECK(page.key && std::strlen(page.key) > 0);
                for (unsigned c = 0; c < 3; ++c) CHECK(page.params[c] < int(info.param_count) && page.shift[c] < int(info.param_count));
                if (page.picture == StagePicture::Envelope) CHECK(page.params[0] >= 0 && page.params[1] >= 0 && page.params[2] >= 0 && page.shift[0] >= 0);
            }
        }
    }
    CHECK(stage_groups(EngineKind::Plugin).groups[0].pages[0].kind == StageKind::PluginScreen && stage_groups(EngineKind::Plugin).groups[0].pages[1].kind == StageKind::PluginSound);
    CHECK(stage_groups(EngineKind::Plugin).groups[2].pages[0].kind == StageKind::Mix && stage_groups(EngineKind::Sampler).groups[2].pages[0].kind == StageKind::PadMix);
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
    auto key = [&] { return std::string(stage_page_now(ui, project->tracks[ui.track], stage_skip(*project, ui, project->tracks[ui.track])).key); };
    auto cards = [&](bool shift) { return page_bindings(*project, ui, pattern, shift); };

    // Every track gives B1-B3 the same jobs (hubs plan, S-B): B1 the source, B2 how it plays, B3 where it goes.
    // A synth (a PLUGIN track): B1 PRESETS -> EDIT, B2 ARP -> CHORD, B3 MIX -> SENDS -> FX; another button jumps to
    // its own first page; each track keeps its own.
    CHECK(key() == "PRESETS");
    stage_press(ui, project->tracks[0], 0, stage_skip(*project, ui, project->tracks[0])); CHECK(key() == "EDIT");
    stage_press(ui, project->tracks[0], 0, stage_skip(*project, ui, project->tracks[0])); CHECK(key() == "PRESETS");
    { // A plug-in that draws its own screen (shell/screen_view.h): B1 PANEL first, then PRESETS, EDIT; Plug-in screens
      // off, no PANEL.
        const uint8_t own = project->tracks[0].plugin;
        ui.plugin_panel.entry = own; ui.plugin_panel.status = PluginStatus::Ready; ui.plugin_panel.has_screen = true;
        CHECK(key() == "PRESETS"); // the page the track was on is kept
        stage_press(ui, project->tracks[0], 0, stage_skip(*project, ui, project->tracks[0])); CHECK(key() == "EDIT");
        CHECK(plugin_screen_place(*project, ui).entry < 0); // EDIT: Melodee's pages
        stage_press(ui, project->tracks[0], 0, stage_skip(*project, ui, project->tracks[0])); CHECK(key() == "PANEL");
        CHECK(plugin_screen_place(*project, ui).entry == int(own) && plugin_screen_place(*project, ui).area == 0);
        stage_press(ui, project->tracks[0], 0, stage_skip(*project, ui, project->tracks[0])); CHECK(key() == "PRESETS");
        stage_press(ui, project->tracks[0], 1, stage_skip(*project, ui, project->tracks[0])); CHECK(key() == "ARP");
        stage_press(ui, project->tracks[0], 0, stage_skip(*project, ui, project->tracks[0])); CHECK(key() == "PANEL"); // B1 from elsewhere: its first page
        ui.stage_group[0] = ui.stage_sub[0] = 0; CHECK(key() == "PANEL");                                                 // and a track's first look
        // Picked from Stage (Context::choose_plugin): EDIT while it opens, then PANEL once it says it draws its own screen.
        stage_go(ui, EngineKind::Plugin, StageKind::PluginEdit);
        ui.plugin_screen_due = own; ui.plugin_panel.status = PluginStatus::Loading; ui.plugin_panel.has_screen = false;
        plugin_screen_first(*project, ui); CHECK(key() == "EDIT" && ui.plugin_screen_due == own);
        ui.plugin_panel.status = PluginStatus::Ready; ui.plugin_panel.has_screen = true;
        plugin_screen_first(*project, ui); CHECK(key() == "PANEL" && ui.plugin_screen_due == no_plugin);
        // One without a screen of its own stays on EDIT; a page chosen while it opened is kept; another track: not awaited.
        stage_go(ui, EngineKind::Plugin, StageKind::PluginEdit);
        ui.plugin_screen_due = own; ui.plugin_panel.has_screen = false;
        plugin_screen_first(*project, ui); CHECK(key() == "EDIT" && ui.plugin_screen_due == no_plugin);
        stage_go(ui, EngineKind::Plugin, StageKind::PluginSound);
        ui.plugin_screen_due = own; ui.plugin_panel.has_screen = true;
        plugin_screen_first(*project, ui); CHECK(key() == "PRESETS" && ui.plugin_screen_due == no_plugin);
        ui.plugin_screen_due = own; ui.track = 1;
        plugin_screen_first(*project, ui); CHECK(ui.plugin_screen_due == no_plugin);
        ui.track = 0;
        stage_go(ui, EngineKind::Plugin, StageKind::PluginEdit);
        ui.settings.plugin_skins = false;
        stage_press(ui, project->tracks[0], 0, stage_skip(*project, ui, project->tracks[0])); CHECK(key() == "PRESETS");
        stage_press(ui, project->tracks[0], 0, stage_skip(*project, ui, project->tracks[0])); CHECK(key() == "EDIT");
        stage_press(ui, project->tracks[0], 0, stage_skip(*project, ui, project->tracks[0])); CHECK(key() == "PRESETS");
        ui.settings.plugin_skins = true;
        ui.plugin_panel = {};
    }
    stage_press(ui, project->tracks[0], 1, false); CHECK(key() == "ARP");
    stage_press(ui, project->tracks[0], 1, false); CHECK(key() == "CHORD");
    ui.track = 1; CHECK(key() == "TRIM");
    stage_press(ui, project->tracks[1], 2, false); CHECK(key() == "MIX");
    ui.track = 0; CHECK(key() == "CHORD");
    const auto synth_skip = stage_skip(*project, ui, project->tracks[0]); // MIX and SENDS are the mixer's since step 6
    stage_press(ui, project->tracks[0], 2, synth_skip); CHECK(key() == "FX"); // another button: its first page shown
    stage_press(ui, project->tracks[0], 2, synth_skip); CHECK(key() == "FX"); // its only one
    // PRESETS: the performance knobs, the plug-in open or not; EDIT: its own row of three once it is open (none
    // before). No Shift row on either.
    CHECK(stage_go(ui, EngineKind::Plugin, StageKind::PluginSound) && cards(false)[0].kind == K::Param && !shift_bindings(*project, ui, pattern)[0].bound());
    CHECK(stage_go(ui, EngineKind::Plugin, StageKind::PluginEdit) && !cards(false)[0].bound());
    CHECK(stage_go(ui, EngineKind::Plugin, StageKind::PluginSound));
    ui.plugin_panel.entry = project->tracks[0].plugin; ui.plugin_panel.status = PluginStatus::Ready; ui.plugin_panel.params[0].used = true;
    { // PRESETS: the three performance knobs, whatever the plug-in (engine/macros.h); a slot's kind 0 is its default
        auto knobs = cards(false);
        CHECK(knobs[0].kind == K::Param && knobs[0].id == plugin_track::Macro1 && knobs[0].min == -100 && knobs[0].max == 100);
        CHECK(knobs[1].id == plugin_track::Macro2 && knobs[1].min == 0 && knobs[2].id == plugin_track::Macro3 && knobs[2].max == 100);
        auto& pt = project->tracks[0];
        CHECK(std::string(track_param_info(pt.engine, pt.params, 0).label) == "FILTER" &&
              std::string(track_param_info(pt.engine, pt.params, 1).label) == "ECHO" && std::string(track_param_info(pt.engine, pt.params, 2).label) == "SPACE");
        pt.params[plugin_track::Kind3] = int16_t(MacroKind::Echo); // a slot set to another kind: its card and lane follow
        CHECK(std::string(track_param_info(pt.engine, pt.params, 2).label) == "ECHO" && lane_name(pt, plugin_track::Macro3) == "ECHO");
        pt.params[plugin_track::Kind3] = 0;
        CHECK(show(macro_param(MacroKind::Filter), -40).value == "LP 40" && show(macro_param(MacroKind::Filter), 0).value == "Off" &&
              show(macro_param(MacroKind::Filter), 25).value == "HP 25");
        CHECK(!shift_bindings(*project, ui, pattern)[0].bound());
    }
    CHECK(stage_go(ui, EngineKind::Plugin, StageKind::PluginEdit) && key() == "EDIT" && cards(false)[0].kind == K::PluginParam);
    {
        // Shift + wheel: a parameter with 401 values (x42 Fat1's Tuning, 400-480 Hz) moves one of them a detent, so
        // 440 Hz (500) is reached and a detent is never rounded back; a smooth one moves a thousandth.
        const auto b = cards(false)[0]; // a copy: cards() gives its array by value
        CHECK(plugin_card(ui.plugin_panel, b.id) == &ui.plugin_panel.params[0]);
        PluginPanelParam tuning; tuning.used = true; tuning.steps = 401;
        CHECK(plugin_param_step(tuning, 500, 1) == 503 && plugin_param_step(tuning, 503, -1) == 500 &&
              plugin_param_step(tuning, 498, 1) == 500 && plugin_param_step(tuning, 1000, 1) == 1000);
        PluginPanelParam smooth; smooth.used = true;
        CHECK(plugin_param_step(smooth, 500, 1) == 501 && plugin_param_step(smooth, 0, -1) == 0);
        ui.plugin_panel.row = 1; CHECK(!plugin_card(ui.plugin_panel, b.id)); // another row on K1-K3
        // Its own screen (plugin/plugin_screen.h) names its three: bound apart from the rows, each once.
        ui.plugin_panel.row = 0;
        ui.plugin_panel.screen = true;
        ui.plugin_panel.params[1].used = ui.plugin_panel.params[2].used = true;
        const auto own = cards(false);
        CHECK(own[0].kind == K::PluginParam && plugin_binding_shown(own[0].id) == plugin_screen_slot && plugin_binding_k(own[2].id) == 2);
        CHECK(plugin_card(ui.plugin_panel, own[1].id) == &ui.plugin_panel.params[1]);
        CHECK(!plugin_card(ui.plugin_panel, b.id)); // a row's binding while its screen is in view: none
        ui.plugin_panel.screen = false;
        CHECK(!plugin_card(ui.plugin_panel, own[1].id) && plugin_card(ui.plugin_panel, b.id) == &ui.plugin_panel.params[0]);
    }
    ui.plugin_panel = {};

    // MIX and SENDS are the mixer's since step 6: a synth's B3 shows FX, and left on either, Stage shows FX.
    stage_go(ui, 2, 0);
    CHECK(key() == "FX");
    stage_go(ui, 2, 1);
    CHECK(key() == "FX" && !cards(false)[0].bound());
    // ARP: mode, rate, octaves; Shift + K1 gate, Shift + K3 hold.
    stage_go(ui, 1, 0);
    CHECK(cards(false)[0].kind == K::ArpMode && cards(false)[1].kind == K::ArpRate && cards(true)[0].kind == K::ArpGate && cards(true)[2].kind == K::ArpHold);
    // CHORD: type, voicing, inversion; Shift + K1 chords on or off, Shift + K3 hold.
    stage_go(ui, 1, 1);
    CHECK(key() == "CHORD" && cards(false)[0].kind == K::ChordType && cards(false)[2].kind == K::ChordInvert);
    CHECK(cards(true)[0].kind == K::ChordOn && cards(true)[2].kind == K::ArpHold);
    // FX: nothing without effects; with one, its first row and (Shift) its second.
    stage_go(ui, 2, 2);
    CHECK(!cards(false)[0].bound());
    project->tracks[0].fx.slots[0].kind = uint8_t(EffectKind::Echo);
    CHECK(cards(false)[0].kind == K::FxParam && cards(false)[0].id == 0 * 64 + 0 * 8 + 0);
    CHECK(!cards(true)[0].bound() || cards(true)[0].id == 3);

    // A kit, on the last pad hit: B1 TRIM LAYERS TUNE AMP ENV, B2 PLAY FOLLOW CLOUD, B3 MIX SENDS FILTER FILT ENV
    // KIT FILT. TRIM start, end, loop, zoom on Shift; MIX K2 the kit's volume;
    // AMP ENV Shift + K2 Decay From; TUNE Shift + K3 # Beats; PLAY Shift + K2 Pad Link.
    ui.track = 1;
    auto& kit = project->tracks[1];
    kit.pads[5].sample = 0; project->samples.edit(0).frames = 44100; kit.last_pad = 5;
    stage_go(ui, 0, 0);
    CHECK(cards(false)[0].kind == K::PadStart && cards(false)[1].kind == K::PadEnd && cards(false)[2].kind == K::PadLoopPoint);
    CHECK(cards(true)[0].kind == K::TrimZoom);
    stage_go(ui, 0, 1); // LAYERS (roadmap P3): the layer in hand, its velocities; Shift + K1 how a hit picks
    CHECK(key() == "LAYERS" && cards(false)[0].kind == K::LayerPick && cards(false)[1].kind == K::LayerLow && cards(true)[0].kind == K::LayerPlayMode);
    stage_go(ui, 2, 0);
    CHECK(key() == "MIX" && cards(false)[0].kind == K::PadLevel && cards(false)[1].kind == K::Param && cards(false)[1].id == sampler::Level && cards(false)[2].kind == K::PadPan);
    stage_go(ui, 0, 3);
    CHECK(key() == "AMP ENV" && cards(false)[0].kind == K::PadAttack && cards(true)[1].kind == K::PadDecayFrom);
    stage_go(ui, 0, 2);
    CHECK(key() == "TUNE" && cards(false)[0].kind == K::PadPitch && cards(true)[2].kind == K::PadBeats);
    stage_go(ui, 1, 0);
    CHECK(key() == "PLAY" && cards(false)[0].kind == K::PadMono && cards(true)[1].kind == K::PadLink);
    stage_go(ui, 2, 1);
    CHECK(key() == "SENDS" && cards(false)[1].kind == K::SendBus);
    stage_go(ui, 2, 2);
    CHECK(key() == "FILTER" && cards(false)[2].kind == K::PadFilterType);
    stage_go(ui, 2, 3);
    CHECK(key() == "FILT ENV" && cards(false)[2].kind == K::PadFAmount);
    // B2 PLAY -> COLOUR -> FOLLOW -> CLOUD -> PLAY; B3 ends on KIT FILT.
    stage_go(ui, 1, 0);
    stage_press(ui, kit, 1, false); CHECK(key() == "COLOUR" && cards(false)[0].kind == K::PadColour && !cards(false)[1].bound() && cards(true)[0].kind == K::PadColour);
    stage_press(ui, kit, 1, false); CHECK(key() == "FOLLOW" && cards(false)[0].id == sampler::Follow && cards(false)[1].id == sampler::FollowHow);
    stage_press(ui, kit, 1, false);
    CHECK(key() == "CLOUD" && cards(false)[0].kind == K::PadCloud && cards(false)[0].id == 1 && cards(false)[2].id == 3);
    CHECK(cards(true)[0].kind == K::PadCloud && cards(true)[0].id == 4 && cards(true)[2].id == 0);
    stage_press(ui, kit, 1, false); CHECK(key() == "PLAY");
    // A pad holding an instrument: its zones are its layers, so B1 is ZONES -> TUNE -> AMP ENV, three dots. A track
    // left on LAYERS shows ZONES on an instrument pad and LAYERS again on a sample pad.
    std::snprintf(project->samples.edit(1).path, sizeof(SampleRef::path), "%s", "Strings.sfz");
    project->samples.edit(1).zones = 6; // read
    kit.pads[7].sample = 1; kit.last_pad = 7;
    CHECK(stage_instrument(*project, kit) && !stage_instrument(*project, project->tracks[0]));
    stage_go(ui, 0, 0);
    CHECK(cards(false)[0].kind == K::ZonePick);
    stage_press(ui, kit, 0, true); CHECK(key() == "TUNE");
    stage_press(ui, kit, 0, true); CHECK(key() == "AMP ENV");
    stage_press(ui, kit, 0, true); CHECK(key() == "TRIM");
    stage_go(ui, 0, 1);
    CHECK(stage_at(ui, kit, true).sub == 0 && key() == "TRIM" && cards(false)[0].kind == K::ZonePick);
    stage_press(ui, kit, 0, true); CHECK(key() == "TUNE");
    stage_go(ui, 0, 1); kit.last_pad = 5;
    CHECK(key() == "LAYERS" && cards(false)[0].kind == K::LayerPick);
    const auto& kit_groups = stage_groups(EngineKind::Sampler);
    CHECK(stage_shown(kit_groups, 0, true) == 3 && stage_shown(kit_groups, 0, false) == 4 && stage_shown_at(kit_groups, 0, 2, true) == 1);
    kit.pads[7] = PadSound {}; project->samples.edit(1) = SampleRef {};
    stage_go(ui, 2, 4);
    CHECK(key() == "KIT FILT" && cards(false)[0].id == sampler::Filter && cards(false)[2].id == sampler::FilterType);
    kit.last_pad = 6; // a pad without a sample: nothing to turn, but the kit's own values still are
    CHECK(cards(false)[0].bound() && cards(false)[0].id == sampler::Filter);
    stage_go(ui, 2, 3);
    CHECK(!cards(false)[0].bound());
    // The old sampler pages keep the same groups (shell/sampler_map.h).
    CHECK(sampler_next(PageId::Trim, 0) == PageId::LayersPage && sampler_next(PageId::LayersPage, 0) == PageId::PadPage);
    CHECK(sampler_next(PageId::Trim, 1) == PageId::PlayPage && sampler_next(PageId::CloudPage, 1) == PageId::PlayPage);
    CHECK(sampler_next(PageId::Trim, 2) == PageId::PadMix && sampler_next(PageId::FilterPage, 2) == PageId::PadMix);
    CHECK(stage_kit_page(StageKind::AmpEnv) == PageId::EnvelopePage && stage_kit_page(StageKind::Preset) == PageId::Count);
    {   // Sample held (the SOUND hub, docs/HUBS_PLAN.md H-b): its pads after sample_sheet_ms, but on Steps and Roll the
        // editor stays and the header names the held sound.
        UiState held;
        held.page = PageId::Stage; held.sample_held = true; held.sample_down = 1000;
        CHECK(!sample_sheet_shown(held, 1000 + sample_sheet_ms - 1) && sample_sheet_shown(held, 1000 + sample_sheet_ms));
        held.page = PageId::StepEdit;
        CHECK(!sample_sheet_shown(held, 5000) && sample_held_named(held, 5000));
        held.page = PageId::Roll;
        CHECK(!sample_sheet_shown(held, 5000) && sample_held_named(held, 5000));
        held.page = PageId::Patterns;
        CHECK(sample_sheet_shown(held, 5000) && !sample_held_named(held, 5000));
        held.sample_held = false;
        CHECK(!sample_sheet_shown(held, 5000) && !sample_held_named(held, 5000));
        // K1-K3 are the pads' while it is held: Keys' root, scale and layout on a synth; on Steps the step's stay.
        UiState u; u.page = PageId::Stage; u.track = 0; u.sample_held = true;
        auto keys = page_bindings(*project, u, pattern, false);
        CHECK(keys[0].kind == K::Root && keys[1].kind == K::Scale && keys[2].kind == K::Layout);
        CHECK(sample_held_page(*project, u) == PageId::Keys);
        u.track = 1; CHECK(sample_held_page(*project, u) == PageId::Pads);
        u.page = PageId::StepEdit; CHECK(sample_held_page(*project, u) == PageId::Count);
        u.page = PageId::Stage; u.track = 9; CHECK(sample_held_page(*project, u) == PageId::Count); // empty: nothing
    }

    // MIDI: B1 DEVICE, PROGRAM (bank and program, Off at first), CC (values; their numbers on Shift); B2 ARP, CHORD;
    // B3 EDIT opens the Edit page (every MIDI page), so Stage never shows it.
    ui.track = 3;
    auto& midi = project->tracks[3];
    stage_go(ui, 0, 1);
    CHECK(key() == "PROGRAM" && cards(false)[2].id == midi_track::Program && midi.params[midi_track::Program] == 0);
    CHECK(show(engine_info(EngineKind::Midi).params[midi_track::Program], 0).value == "Off" && show(engine_info(EngineKind::Midi).params[midi_track::BankMsb], 1).value == "0");
    stage_go(ui, 0, 2);
    CHECK(key() == "CC" && cards(false)[0].id == midi_track::Cc1 && cards(true)[0].id == midi_track::CcNumber1 && midi.params[midi_track::CcNumber1] == 74);
    CHECK(std::string(midi_track::cc_name(74)) == "Brightness" && !midi_track::cc_name(3));
    CHECK(binding_lane(midi, cards(false)[0]) == motion_lane(midi_track::Cc1)); // a CC records as motion
    stage_press(ui, midi, 2, false); CHECK(key() == "CC" && stage_opens_edit(midi, 2) && !stage_opens_edit(midi, 0)); // B3 EDIT opens Edit (stage_page.cpp)
    stage_press(ui, midi, 1, false); CHECK(key() == "ARP");
    ui.stage_group[3] = 2; CHECK(key() == "DEVICE" && stage_at(ui, midi, false).group == 0); // left on B3: B1's

    // An empty track: SYNTH (a plug-in: Melodee's synths are plug-ins), KIT (where its sample comes from), MIDI (port,
    // channel).
    ui.track = 9;
    CHECK(key() == "SYNTH");
    std::array<EngineKind, size_t(EngineKind::Count)> engines {};
    unsigned count = new_engines(engines);
    CHECK(count == 1 && engines[0] == EngineKind::Plugin);
    // No knob cards, whichever key is lit: the empty track's Stage is a carousel the wheel and Push work (shell/new_track.h).
    for (unsigned key = 0; key < 3; ++key) {
        stage_go(ui, key, 0);
        auto none = cards(false);
        CHECK(!none[0].bound() && !none[1].bound() && !none[2].bound());
        CHECK(!shift_bindings(*project, ui, pattern)[0].bound()); // no Shift row either
    }
    // Its carousel as on the MPC: the types (Drum, Keygroup, Plugin, MIDI, Audio), then the opened type's sources.
    {
        UiState u;
        CHECK(new_pick_count(u) == 5 && new_pick_choice(u) == NewChoice::Count); // the types: nothing made yet
        new_pick_turn(u, 9, 1);
        CHECK(u.new_pick == 4);                // the wheel stops at the last type (Audio)
        new_pick_turn(u, -9, 2);
        CHECK(u.new_pick == 0);                // and at the first (Drum)
        u.new_sources = true; u.new_type = uint8_t(NewType::Drum); u.new_pick = 0;
        CHECK(new_pick_count(u) == 3 && new_pick_choice(u) == NewChoice::Samples);
        new_pick_turn(u, 1, 3); CHECK(new_pick_choice(u) == NewChoice::Kit);
        new_pick_turn(u, 5, 4); CHECK(new_pick_choice(u) == NewChoice::Record);
        u.new_type = uint8_t(NewType::Midi); u.new_pick = type_sources(NewType::Midi).first;
        CHECK(new_pick_choice(u) == NewChoice::MidiUsb); // MIDI opens on USB-C
        CHECK(type_sources(NewType::Keygroup).count == 1 && type_sources(NewType::Keygroup).items[0] == NewChoice::Keygroup);
        CHECK(type_sources(NewType::Plugin).count == 1 && type_sources(NewType::Plugin).items[0] == NewChoice::Plugin);
        CHECK(type_sources(NewType::Audio).count == 1 && type_sources(NewType::Audio).items[0] == NewChoice::Audio);
    }
    stage_go(ui, 0, 0);
    ui.last_pot = 2; // the pot touched last picks the door Push makes, on an empty track too
    CHECK(stage_focus(ui, project->tracks[9]) == 2 && stage_focus(ui, project->tracks[0]) == 2);
    ui.last_pot = -1;

    // The mixer: SENDS A-C on K1-K3 (Tracks and Stage's MIX are gone, docs/MIXER_PLAN.md step 6).
    ui.page = PageId::Mixer; ui.track = 0;
    CHECK(cards(false)[0].kind == K::Send && cards(false)[1].kind == K::Send && cards(false)[2].kind == K::Send);

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
        auto host_held = std::make_unique<EngineHost>(); // on the heap: a test's frame stays under the 8 MB stack
        auto& host = *host_held;
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
    roll_step(p, ui, 2); CHECK(ui.step_cursor == 8 && ui.roll_key == 57);     // the wheel: past the chord's top, a step on; onto step 8's note
    roll_step(p, ui, 1); CHECK(ui.step_cursor == 9 && ui.roll_key == 57 && roll_focus(p, ui) < 0); // an empty step keeps the pitch
    roll_step(p, ui, 31); CHECK(ui.step_cursor == 40 && ui.roll_key == 69);   // a detent a step over empty bars
    roll_step(p, ui, 100); CHECK(ui.step_cursor == 63 && ui.roll_key == 69);  // stops at the pattern's end
    { // Round 2: NUDGE on Shift + K1, the note in focus off its nearest grid line (1/16: -120 .. 119 ticks).
        Pattern n;
        n.length = p.length;
        Event late; late.tick = 20 * sixteenth_ticks + 5; late.key = 50; CHECK(n.add(late));
        Event early; early.tick = 30 * sixteenth_ticks - 4; early.key = 51; CHECK(n.add(early));
        CHECK(nudge_base(n, late) == 20 * sixteenth_ticks && nudge_base(n, early) == 30 * sixteenth_ticks);
        auto card = nudge_card(n, late);
        CHECK(card.kind == Binding::Kind::EventNudge && card.value == 5 && card.min == -120 && card.max == 119);
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
    // The wheel through a chord a note at a time: in on its lowest going on, its highest going back.
    ui.step_cursor = 5; ui.roll_key = 70;
    roll_step(p, ui, 1); CHECK(ui.step_cursor == 6 && ui.roll_key == 60 && roll_focus(p, ui) >= 0);
    roll_step(p, ui, 1); CHECK(ui.step_cursor == 6 && ui.roll_key == 64 && roll_focus(p, ui) >= 0);
    roll_step(p, ui, 1); CHECK(ui.step_cursor == 7 && ui.roll_key == 64 && roll_focus(p, ui) < 0);
    roll_step(p, ui, 1); CHECK(ui.step_cursor == 8 && ui.roll_key == 57);
    roll_step(p, ui, -1); CHECK(ui.step_cursor == 7 && ui.roll_key == 57);
    roll_step(p, ui, -1); CHECK(ui.step_cursor == 6 && ui.roll_key == 64);
    roll_step(p, ui, -1); CHECK(ui.step_cursor == 6 && ui.roll_key == 60);
    roll_step(p, ui, -1); CHECK(ui.step_cursor == 5 && ui.roll_key == 60 && roll_focus(p, ui) < 0);
    roll_step(p, ui, -5); CHECK(ui.step_cursor == 0 && ui.roll_key == 62);
    roll_step(p, ui, -1); CHECK(ui.step_cursor == 0 && ui.roll_key == 62);    // stops at the start
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

// Roll's selection: Shift + wheel marks notes along the wheel's walk; the marks then move, change pitch, mute,
// ratchet and go together.
void roll_marks() {
    Pattern p;
    p.length = 4 * 4 * ticks_per_quarter; // 64 steps of 1/16
    auto add = [&](unsigned step, uint8_t key) { Event e; e.tick = step * sixteenth_ticks; e.key = key; CHECK(p.add(e)); };
    add(0, 62); add(6, 60); add(6, 64); add(8, 57); add(10, 64);
    const uint32_t s = sixteenth_ticks;
    UiState ui;
    ui.track = 3; ui.step_cursor = 5; ui.roll_key = 70;
    roll_mark_walk(p, ui, 1, 2);                                  // nothing at the start; onto step 6's C4
    CHECK(ui.roll_marking && ui.roll_marked == 1 && ui.roll_mark_track == 3 && ui.roll_mark_pattern == 2);
    roll_mark_walk(p, ui, 2, 2);                                  // E4, then the empty step 7
    CHECK(ui.roll_marked == 2 && ui.step_cursor == 7);
    roll_mark_walk(p, ui, 1, 2);                                  // step 8's A3
    CHECK(ui.roll_marked == 3 && roll_marked_events(p, ui).size() == 3);
    CHECK(roll_selection(ui, 2) && !roll_selected(ui, 2));        // still marking
    CHECK(!roll_selection(ui, 1));                                // another pattern: none
    ui.roll_marking = false;
    CHECK(roll_selected(ui, 2));
    // The wheel: a step of Q later, the cursor with them.
    CHECK(roll_marks_move(p, ui, 1) && ui.step_cursor == 9);
    CHECK(p.events.size() == 5 && roll_marked_events(p, ui).size() == 3);
    CHECK(p.events[size_t(roll_marked_events(p, ui).front())].tick == 7 * s);
    CHECK(roll_marks_move(p, ui, 1) && roll_marks_move(p, ui, 1));
    { auto m = roll_marked_events(p, ui); CHECK(m.size() == 3 && p.events[m.front()].tick == 9 * s); }
    // The marked E4 on step 9 would land on step 10's E4: the notes hop over it to step 11 together.
    CHECK(roll_marks_move(p, ui, 1) && ui.step_cursor == 13);
    CHECK(p.events.size() == 5);
    { auto m = roll_marked_events(p, ui); CHECK(m.size() == 3 && p.events[m.front()].tick == 11 * s); }
    { int e4 = 0; for (const auto& e : p.events) e4 += e.key == 64; CHECK(e4 == 2); } // the unmarked E4 kept
    CHECK(!roll_marks_move(p, ui, 100));                          // past the end: nothing moves
    CHECK(p.events.size() == 5);
    // Shift + wheel: a row up.
    CHECK(roll_marks_pitch(p, ui, 1, -1, 127));
    { auto m = roll_marked_events(p, ui); CHECK(m.size() == 3); int sum = 0; for (auto n : m) sum += p.events[n].key; CHECK(sum == 61 + 65 + 58); }
    CHECK(!roll_marks_pitch(p, ui, 200, -1, 127));
    // Mute: all on, then all off.
    CHECK(roll_marks_mute(p, ui));
    for (auto n : roll_marked_events(p, ui)) CHECK(p.events[n].flags & step_mute);
    CHECK(!roll_marks_mute(p, ui));
    for (auto n : roll_marked_events(p, ui)) CHECK(!(p.events[n].flags & step_mute));
    // Note Repeat: 2, 3, 4, 6, 8, then off.
    CHECK(roll_marks_ratchet(p, ui) == 2 && roll_marks_ratchet(p, ui) == 3 && roll_marks_ratchet(p, ui) == 4);
    CHECK(roll_marks_ratchet(p, ui) == 6 && roll_marks_ratchet(p, ui) == 8 && roll_marks_ratchet(p, ui) == 1);
    // Erase: the marked go, the rest stay, the marks cleared.
    CHECK(roll_marks_erase(p, ui) == 3 && p.events.size() == 2 && !ui.roll_marked);
    CHECK(p.events[0].key == 62 && p.events[1].key == 64 && p.events[1].tick == 10 * s);
}

// Roll's K2 over marked notes: every velocity moved by as much, the spacing kept; the card their lowest to highest.
void roll_marks_velocity_shift() {
    Pattern p;
    p.length = 4 * 4 * ticks_per_quarter;
    auto add = [&](unsigned step, uint8_t key, uint8_t velocity) {
        Event e; e.tick = step * sixteenth_ticks; e.key = key; e.velocity = velocity; CHECK(p.add(e));
    };
    add(0, 60, 60); add(2, 62, 85); add(4, 64, 100); add(6, 65, 40); // the last one stays unmarked
    UiState ui;
    ui.page = PageId::Roll; ui.track = 0; ui.step_cursor = 0; ui.roll_key = 60;
    roll_mark_walk(p, ui, 1, 0);                  // C4, then the empty step 1
    roll_mark_walk(p, ui, 2, 0);                  // D4, the empty step 3
    roll_mark_walk(p, ui, 1, 0);                  // E4
    ui.roll_marking = false;
    CHECK(roll_marked_events(p, ui).size() == 3);
    auto velocities = [&] { std::vector<int> v; for (const auto& e : p.events) v.push_back(e.velocity); return v; };
    int low = 0, high = 0;
    CHECK(roll_marks_span(p, ui, low, high) && low == 60 && high == 100);
    // +10: 60, 85, 100 to 70, 95, 110; the unmarked note keeps 40.
    CHECK(roll_marks_velocity(p, ui, 70));
    CHECK(velocities() == (std::vector<int> {70, 95, 110, 40}));
    // Down again, back as they were.
    CHECK(roll_marks_velocity(p, ui, 60) && velocities() == (std::vector<int> {60, 85, 100, 40}));
    // Held at the ends: the highest stops at 127, the lowest at 1, and the spacing stays.
    CHECK(roll_marks_velocity(p, ui, 120) && velocities() == (std::vector<int> {87, 112, 127, 40}));
    CHECK(!roll_marks_velocity(p, ui, 127));
    CHECK(roll_marks_velocity(p, ui, -5) && velocities() == (std::vector<int> {1, 26, 41, 40}));
    CHECK(roll_marks_velocity(p, ui, 60));
    // The card: K2 is the marks' lowest, room for the whole group; K1 and K3 still the cards that set them all.
    Project project;
    synth(project.tracks[0], 0);
    project.tracks[0].pattern = 0;
    ui.roll_key = 70;                             // no note in focus
    auto cards = page_bindings(project, ui, p);
    CHECK(cards[0].kind == Binding::Kind::EventLength && cards[2].kind == Binding::Kind::EventChance);
    CHECK(cards[1].kind == Binding::Kind::MarksVelocity && cards[1].value == 60 && cards[1].min == 1 && cards[1].max == 87);
    ui.roll_key = 64;                             // a marked note in focus: K2 still moves them all
    CHECK(page_bindings(project, ui, p)[1].kind == Binding::Kind::MarksVelocity);
    CHECK(binding_scope(project, ui, cards[1]).kind == UndoScope::Kind::Pattern);
    ui.roll_marking = true;                       // still marking: the note in focus alone
    CHECK(page_bindings(project, ui, p)[1].kind == Binding::Kind::EventVelocity);
    ui.roll_marking = false; ui.held_note = 2;    // a pad held: its locks, not the marks
    CHECK(page_bindings(project, ui, p)[1].kind != Binding::Kind::MarksVelocity);
}

// Roll on a kit with 16 Levels on: a row for each pitch of the levels pad (60 + its semitones, as Seq Record keeps
// them), that pad's notes only; the cursor, focus, chord, jumps, opening and the cards go by those rows.
void roll_levels() {
    auto project = std::make_unique<Project>();
    auto& kit = project->tracks[0];
    load_sound(kit, EngineKind::Sampler, 0);
    CHECK(roll_lane(kit) == -1);                                  // a kit's roll: a row a pad
    kit.levels = true; kit.levels_pad = 2;
    const int lane = roll_lane(kit);
    CHECK(lane == int(sampler_first_note) + 2);
    Pattern p;
    p.length = 16 * sixteenth_ticks;
    auto add = [&](unsigned step, uint8_t key, int8_t pitch) { Event e; e.tick = step * sixteenth_ticks; e.key = key; e.pitch = pitch; CHECK(p.add(e)); };
    add(0, uint8_t(lane), 0); add(0, uint8_t(lane), 4); add(0, uint8_t(lane), 7); // a chord: one pad, three pitches, all kept
    add(0, uint8_t(sampler_first_note), 0);                                    // pad 1's note: not on this roll
    add(4, uint8_t(lane), 2); add(9, uint8_t(lane), -5);
    CHECK(p.events.size() == 6);
    CHECK(roll_row(p.events[1], lane) == 60 && roll_row(p.events[0], lane) == -1 && roll_row(p.events[0], -1) == int(sampler_first_note));
    UiState ui;
    ui.step_cursor = 0; ui.roll_key = 64;
    int at = roll_focus(p, ui, lane);
    CHECK(at >= 0 && p.events[size_t(at)].key == lane && p.events[size_t(at)].pitch == 4);
    CHECK(roll_focus(p, ui) < 0);                                 // read as pad rows, nothing sits on 64
    ui.roll_key = int(sampler_first_note); CHECK(roll_focus(p, ui, lane) < 0); // pad 1's note is no row here
    ui.roll_key = 60;
    roll_pitch(p, ui, 1, lane); CHECK(ui.roll_key == 64);         // the chord up
    roll_pitch(p, ui, 1, lane); CHECK(ui.roll_key == 67);
    roll_pitch(p, ui, 1, lane); CHECK(ui.roll_key == 68 && roll_focus(p, ui, lane) < 0); // then a semitone
    roll_step(p, ui, 4, lane); CHECK(ui.step_cursor == 4 && ui.roll_key == 62); // onto step 4's D4
    roll_step(p, ui, 5, lane); CHECK(ui.step_cursor == 9 && ui.roll_key == 55);
    roll_step(p, ui, 1, lane); CHECK(ui.step_cursor == 10 && ui.roll_key == 55);
    roll_step(p, ui, -6, lane); CHECK(ui.step_cursor == 4 && ui.roll_key == 62);
    roll_step(p, ui, -4, lane); CHECK(ui.step_cursor == 0 && ui.roll_key == 67); // the chord's top going back
    roll_step(p, ui, -1, lane); CHECK(ui.step_cursor == 0 && ui.roll_key == 64); // then down through it
    roll_select(p, ui, lane);
    CHECK(selected_event(p, ui) == roll_focus(p, ui, lane));      // Step Edit's selection: the note in focus
    UiState far; far.step_cursor = 9; far.roll_key = 100; far.roll_top = 110;
    roll_open(p, far, lane); CHECK(far.roll_key == 55);           // opening on the pitches, not on pad 1's key
    // Roll's cards follow the row in focus.
    ui.page = PageId::Roll; ui.track = 0; ui.step_cursor = 0; ui.roll_key = 64;
    auto cards = page_bindings(*project, ui, p);
    CHECK(cards[0].kind == Binding::Kind::EventLength && cards[1].kind == Binding::Kind::EventVelocity);
    ui.roll_key = 65; CHECK(!page_bindings(*project, ui, p)[0].bound());
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

    // Always on now: an older settings file that turned it off reads as on.
    Settings fresh;
    CHECK(fresh.zero_snap);
    Settings off; off.zero_snap = false;
    Settings back; back.read(off.text());
    CHECK(back.zero_snap);
    // STEMS' BEST likewise: on at first, off kept.
    CHECK(fresh.stems_best);
    Settings fast; fast.stems_best = false;
    back.read(fast.text());
    CHECK(!back.stems_best);
    back.read(fresh.text());
    CHECK(back.stems_best);
    // The sample browser's PREVIEW: off at first, on kept; its level (the fader's on Samples) 100 at first, kept,
    // and held to 0..127.
    CHECK(!fresh.preview && fresh.preview_level == 100);
    Settings hearing; hearing.preview = true; hearing.preview_level = 64;
    back.read(hearing.text());
    CHECK(back.preview && back.preview_level == 64);
    back.read(fresh.text());
    CHECK(!back.preview && back.preview_level == 100);
    back.read("preview_level=300\n");
    CHECK(back.preview_level == 127);

    // TO PADS as Simpler's Playback: POLY as before, MONO in one mute group, THRU that and on to the region's end;
    // the pick kept with the settings, POLY at first.
    {
        Slices sl;
        sl.whole(1000, 40000); sl.add(10000, 50000); sl.add(20000, 50000);
        PadSound parent;
        parent.sample = 3;
        auto poly = slice_pad(parent, sl, 1, 50000, SlicePlay::Poly, 4);
        CHECK(poly.start == 10000 && poly.end == 20000 && poly.group == 0);
        auto mono = slice_pad(parent, sl, 1, 50000, SlicePlay::Mono, 4);
        CHECK(mono.start == 10000 && mono.end == 20000 && mono.group == 4);
        auto thru = slice_pad(parent, sl, 0, 50000, SlicePlay::Thru, 4);
        CHECK(thru.start == 1000 && thru.end == 40000 && thru.group == 4);
        std::array<PadSound, 4> pads {};
        pads[0].sample = 1; pads[0].group = 1; pads[1].sample = 2; pads[1].group = 2; pads[2].group = 3; // pad 3 empty: its group free
        CHECK(slice_group(parent, pads) == 3);
        parent.group = 7;
        CHECK(slice_group(parent, pads) == 7 && slice_pad(parent, sl, 2, 50000, SlicePlay::Poly, 4).group == 7);
        CHECK(std::string(slice_play_name(SlicePlay::Thru)) == "THRU" && fresh.slice_play == 0);
        Settings picked; picked.slice_play = 2;
        back.read(picked.text());
        CHECK(back.slice_play == 2);
    }
    // TO PADS' pattern (Live's Slice to New MIDI Track): the slices' notes where they fall, as the pads play them.
    {
        PadSound plain;
        auto ticks = [](double per_frame, uint32_t frames) { return std::lround(per_frame * frames); };
        CHECK(ticks(slice_ticks_per_frame(plain, false, 88200, 1200), 44100) == 1920);  // as recorded: a second at 120 BPM, 2 beats
        CHECK(ticks(slice_ticks_per_frame(plain, false, 88200, 900), 44100) == 1440);   // the same second at 90 BPM
        PadSound follows = plain; follows.own_tempo = 900;
        CHECK(ticks(slice_ticks_per_frame(follows, true, 88200, 1200), 44100) == 1440); // Follow: at its own 90, whatever the pattern's
        CHECK(ticks(slice_ticks_per_frame(follows, false, 88200, 1200), 44100) == 1920); // Follow off: as recorded
        PadSound seq = plain; seq.warp = warp_seq; seq.beats = 8;                       // 8 beats in 4 s: 120 BPM of its own
        CHECK(ticks(slice_ticks_per_frame(seq, false, 176400, 900), 44100) == 1920);
        PadSound twice = plain; twice.warp = 200;                                       // 200 %: twice as long
        CHECK(ticks(slice_ticks_per_frame(twice, false, 88200, 1200), 44100) == 3840);
        // Four slices of a bar at 120 BPM (2 s) on pads A01-A03 and A05: a bar with a note on each beat, as long as its slice.
        Slices sl;
        sl.whole(0, 88200); sl.add(22050, 88200); sl.add(44100, 88200); sl.add(66150, 88200);
        std::array<int, max_slices> keys;
        keys.fill(-1);
        keys[0] = 36; keys[1] = 37; keys[2] = 38; keys[3] = 40;
        const double per = slice_ticks_per_frame(plain, false, 88200, 1200);
        Pattern made = slice_pattern(sl, 88200, keys, per, 3840, 3840 * 64);
        CHECK(made.length == 3840 && made.events.size() == 4 && !made.free);
        CHECK(made.events[0].tick == 0 && made.events[1].tick == 960 && made.events[3].tick == 2880 && made.events[3].key == 40);
        CHECK(made.events[2].length == 960 && made.events[2].velocity == 127);
        keys[1] = -1; // a slice with no pad: no note
        CHECK(slice_pattern(sl, 88200, keys, per, 3840, 3840 * 64).events.size() == 3);
        keys[1] = 36; // KIT: two slices of one sound, one pad
        made = slice_pattern(sl, 88200, keys, per, 3840, 3840 * 64);
        CHECK(made.events.size() == 4 && made.events[0].key == 36 && made.events[1].key == 36);
        // Near a bar's end it is that bar; past an eighth more, the next whole one; never past `most`.
        Slices near;
        near.whole(0, 86900);
        CHECK(slice_pattern(near, 86900, keys, per, 3840, 3840 * 64).length == 3840);
        Slices longer;
        longer.whole(0, 132300); // a bar and a half
        CHECK(slice_pattern(longer, 132300, keys, per, 3840, 3840 * 64).length == 7680);
        CHECK(slice_pattern(longer, 132300, keys, per, 3840, 3840).length == 3840);
        // PATTERN on at first, off kept.
        Settings none; none.slice_pattern = false;
        back.read(none.text());
        CHECK(fresh.slice_pattern && !back.slice_pattern);
        back.read(fresh.text());
        CHECK(back.slice_pattern);
    }
    // Chop's K3: the type picked last is kept with the settings, Threshold at first; a sample with no chop opens on it.
    {
        Settings fresh_chop;
        CHECK(fresh_chop.chop_mode == uint8_t(ChopMode::Threshold));
        Settings picked; picked.chop_mode = uint8_t(ChopMode::Auto);
        Settings read_back; read_back.read(picked.text());
        CHECK(read_back.chop_mode == uint8_t(ChopMode::Auto));
        read_back.read("chop_mode=9\n");
        CHECK(read_back.chop_mode == uint8_t(ChopMode::Auto)); // out of range: the last type
    }

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

// The auto sampler on an instrument outside (engine/external_notes.h), with one made up: it answers 300 frames after a
// note is sent, with a sine that stops over 50 ms once its key is let go, over a little noise. What is kept of a note
// starts on its sound, its key's release where it was asked, its quiet end left out.
struct MadeUpInstrument final : ExternalIo {
    float rate = 48000;
    uint64_t clock = 0;
    uint32_t delay = 300;
    float noise = 0.0005f, level = 0.5f, fall = 0.05f;
    bool mute = false;
    std::vector<uint64_t> spikes; // frames heard at full scale whatever plays
    struct Voice { uint8_t key; float gain; double phase; uint64_t on, off; };
    std::vector<Voice> voices;
    std::vector<std::pair<uint64_t, Command>> posted;
    std::vector<float> heard;
    uint32_t seed = 9;
    unsigned ons = 0, offs = 0, all_offs = 0;
    uint64_t now() override { return clock; }
    void post(const Command& c, uint64_t at) override { posted.push_back({at, c}); }
    void forget() override { for (auto& p : posted) p.second.type = Command::Type::Reload; }
    bool copy(uint64_t from, uint32_t frames, float* l, float* r) override {
        if (from + frames > heard.size()) return false;
        for (uint32_t f = 0; f < frames; ++f) l[f] = r[f] = heard[from + f];
        return true;
    }
    void wait(unsigned ms) override {
        const uint64_t until = clock + uint64_t(ms * rate / 1000);
        for (; clock < until; ++clock) {
            for (auto& p : posted) { // each command as its frame comes, heard `delay` frames on
                if (p.first > clock || p.second.type == Command::Type::Reload) continue;
                const auto& c = p.second;
                if (c.type == Command::Type::NoteOn) { voices.push_back({uint8_t(c.index), level * float(c.value) / 100.0f, 0.0, clock + delay, UINT64_MAX}); ++ons; }
                else if (c.type == Command::Type::NoteOff) { for (auto& v : voices) if (v.key == c.index && v.off == UINT64_MAX) v.off = clock + delay; ++offs; }
                else if (c.type == Command::Type::AllNotesOff) { for (auto& v : voices) if (v.off == UINT64_MAX) v.off = clock + delay; ++all_offs; }
                p.second.type = Command::Type::Reload; // done
            }
            seed = seed * 1664525u + 1013904223u;
            double x = double(int32_t(seed)) / 2147483648.0 * noise;
            for (auto& v : voices) {
                if (clock < v.on || mute) continue;
                const double since = double(clock - v.on) / rate, hz = 440.0 * std::pow(2.0, (v.key - 69) / 12.0);
                double g = v.gain * std::min(1.0, since / 0.002);
                if (clock >= v.off) g *= std::exp(-double(clock - v.off) / rate / fall);
                v.phase += 2 * M_PI * hz / rate;
                x += g * std::sin(v.phase);
            }
            if (std::find(spikes.begin(), spikes.end(), clock) != spikes.end()) x = 1.0;
            heard.push_back(float(std::clamp(x, -1.0, 1.0)));
        }
    }
};

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

// The pager pins ahead of a voice and lets go behind; the audio thread sees only what is safe.
std::vector<std::pair<uintptr_t, bool>>* pin_log = nullptr;
bool pin_fails = false;
bool logged_pin(const void* address, size_t, bool lock) {
    if (lock && pin_fails) return false;
    if (pin_log) pin_log->push_back({reinterpret_cast<uintptr_t>(address), lock});
    return true;
}

// A platform's worker setup (engine/render_pool.h): run once on each worker's own thread, before its first job.
std::thread::id setup_caller;
std::atomic<int> setups_run {0};
std::atomic<int> setup_on_worker {0};
void count_setup() {
    setups_run.fetch_add(1);
    if (std::this_thread::get_id() != setup_caller) setup_on_worker.fetch_add(1);
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
    project->tracks[2].repeat = true;
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
    PadSound pad = [] { PadSound p; p.mono = false; return p; }(); // an instrument loads Poly (shell/sample_files.cpp)
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
    void key(int semitones, int velocity = 127) { engine->note_on(int(sampler_first_note) | (uint8_t(int8_t(semitones)) << 8), velocity); }
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

void keygroup_tracks() {
    // A KEYGROUP track's sampler is keyed: MIDI note k plays its instrument at key k (+ the pad's pitch, its tune).
    {
        ZoneRig r;
        r.part(zone_tone(220, 0.5f, 44100)); r.part(zone_tone(880, 0.5f, 44100));
        auto& low = r.zone(0); low.low_key = 0; low.high_key = 65; low.root = 60;
        auto& high = r.zone(1); high.low_key = 66; high.high_key = 127; high.root = 72;
        r.build();
        r.engine->set_keyed(true);
        r.engine->note_on(60, 127);
        CHECK(std::fabs(zone_hz(r.run(8000), 800, 8000) - 220) < 8);
        r.engine->silence();
        r.engine->note_on(65, 127);
        CHECK(std::fabs(zone_hz(r.run(8000), 800, 8000) - 220 * std::pow(2.0, 5 / 12.0)) < 8);
        r.engine->silence();
        r.engine->note_on(72, 127); // the other zone, at its own pitch
        CHECK(std::fabs(zone_hz(r.run(8000), 800, 8000) - 880) < 12);
    }
    { // the pad's pitch is the track's tune: note 60 sounds key 72
        ZoneRig r;
        r.part(zone_tone(220, 0.5f, 44100)); r.part(zone_tone(880, 0.5f, 44100));
        auto& low = r.zone(0); low.low_key = 0; low.high_key = 65; low.root = 60;
        auto& high = r.zone(1); high.low_key = 66; high.high_key = 127; high.root = 72;
        r.pad.pitch = 12;
        r.build();
        r.engine->set_keyed(true);
        r.engine->note_on(60, 127);
        CHECK(std::fabs(zone_hz(r.run(8000), 800, 8000) - 880) < 12);
    }
    { // a note let go under the sustain pedal ends when the pedal comes up, not before (its key turned once)
        ZoneRig r;
        r.part(zone_tone(440, 0.5f, 88200));
        r.zone(0).root = 60;
        r.pad.release = 0;
        r.build(PadPlay::Gate);
        r.engine->set_keyed(true);
        r.engine->control(64, 127);
        r.engine->note_on(64, 127);
        r.run(2000);
        r.engine->note_off(64);
        CHECK(peak_of(r.run(4000), 2000) > 0.1f); // held by the pedal
        r.engine->control(64, 0);
        r.run(8000);
        CHECK(peak_of(r.run(2000), 0) < 0.01f);
    }
    // A keygroup track keeps its kind, its instrument on pad 1 and its MIDI notes through a file.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        std::strcpy(project->samples.edit(4).path, "/card/Keys/Piano.sfz");
        auto& tr = project->tracks[2];
        tr.engine = EngineKind::Keygroup; tr.set_name("PIANO");
        tr.pads[0].sample = 4; tr.pads[0].pitch = -3;
        Event e; e.tick = 24; e.key = 67; e.velocity = 90;
        bank->at[2][0].add(e);
        auto bytes = save_project(*project, *bank);
        auto back = std::make_unique<Project>();
        auto back_bank = std::make_unique<PatternBank>();
        CHECK(load_project(bytes, *back, *back_bank).empty());
        const auto& t2 = back->tracks[2];
        CHECK(t2.engine == EngineKind::Keygroup && std::string(t2.name) == "PIANO" && t2.pads[0].sample == 4 && t2.pads[0].pitch == -3);
        CHECK(back_bank->at[2][0].events.size() == 1 && back_bank->at[2][0].events[0].key == 67);
    }
    // Instrument pads on kits become keygroup tracks: a kit of more moves the pad, its notes (60 + their semitones),
    // its mute and its clips to the first empty track; a kit of nothing else turns into the keygroup itself.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        std::strcpy(project->samples.edit(0).path, "/card/Kick.wav");
        std::strcpy(project->samples.edit(1).path, "/card/Keys/Piano.sfz");
        std::strcpy(project->samples.edit(1).name, "PIANO");
        std::strcpy(project->samples.edit(2).path, "/card/Keys/Strings.exs");
        std::strcpy(project->samples.edit(3).path, "/card/Projects/Song.xpj#1.3"); // an MPC drum pad of layers: a kit's
        project->tracks[0].pads[9].sample = 3;
        auto& kit = project->tracks[0];
        load_sound(kit, EngineKind::Sampler, 0); kit.set_name("BEAT");
        kit.mix.level = 90;
        kit.pads[0].sample = 0;
        kit.pads[3].sample = 1; kit.pads[3].pitch = 2;
        kit.set_pad_muted(3, true);
        auto note = [](uint32_t tick, unsigned pad, int pitch) { Event e; e.tick = tick; e.key = uint8_t(sampler_first_note + pad); e.pitch = int8_t(pitch); return e; };
        bank->at[0][0].add(note(0, 0, 0)); bank->at[0][0].add(note(96, 3, 5)); bank->at[0][0].add(note(192, 3, -2));
        bank->at[0][1].add(note(0, 3, 0));
        Clip clip; clip.track = 0; clip.pattern = 0; clip.bar = 0; clip.bars = 4;
        project->song.place(clip);
        auto& lone = project->tracks[1];
        load_sound(lone, EngineKind::Sampler, 0); lone.set_name("PAD");
        lone.pads[5].sample = 2;
        bank->at[1][2].add(note(48, 5, 0));
        auto moved = migrate_instrument_pads(*project, *bank);
        CHECK(moved.moved == 2 && moved.left == 0);
        // the kit keeps its kick and its note; the piano went to track 3 (the first empty)
        CHECK(project->tracks[0].engine == EngineKind::Sampler && !project->tracks[0].pads[3].used() && !project->tracks[0].pad_muted(3));
        CHECK(project->tracks[0].pads[9].sample == 3); // the MPC drum pad stays
        CHECK(bank->at[0][0].events.size() == 1 && bank->at[0][0].events[0].key == sampler_first_note && bank->at[0][1].events.empty());
        const auto& piano = project->tracks[2];
        CHECK(piano.engine == EngineKind::Keygroup && std::string(piano.name) == "PIANO" && piano.pads[0].sample == 1 && piano.pads[0].pitch == 2);
        CHECK(piano.pad_muted(0) && piano.mix.level == 90);
        const auto& keys = bank->at[2][0].events;
        CHECK(keys.size() == 2 && keys[0].tick == 96 && keys[0].key == 65 && keys[0].pitch == 0 && keys[1].key == 58);
        CHECK(bank->at[2][1].events.size() == 1 && bank->at[2][1].events[0].key == 60);
        bool clip_moved = false;
        for (unsigned c = 0; c < project->song.count; ++c) clip_moved |= project->song.clips[c].track == 2 && project->song.clips[c].bars == 4;
        CHECK(clip_moved);
        // the lone pad's kit is now its keygroup, in its place and name, its note at 60
        const auto& pad = project->tracks[1];
        CHECK(pad.engine == EngineKind::Keygroup && std::string(pad.name) == "PAD" && pad.pads[0].sample == 2 && !pad.pads[5].used());
        CHECK(bank->at[1][2].events.size() == 1 && bank->at[1][2].events[0].key == 60);
        CHECK(migrate_instrument_pads(*project, *bank).moved == 0); // once is all
    }
    { // a file saved since keygroup tracks keeps an instrument on a kit pad where it was put (KGVR: no migration)
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        std::strcpy(project->samples.edit(1).path, "/card/Keys/Piano.sfz");
        load_sound(project->tracks[0], EngineKind::Sampler, 0);
        project->tracks[0].pads[0].sample = 1;
        Event e; e.key = uint8_t(sampler_first_note); e.pitch = 7;
        bank->at[0][0].add(e);
        auto bytes = save_project(*project, *bank);
        auto back = std::make_unique<Project>();
        auto back_bank = std::make_unique<PatternBank>();
        CHECK(load_project(bytes, *back, *back_bank).empty());
        CHECK(back->tracks[0].engine == EngineKind::Sampler && back->tracks[0].pads[0].sample == 1 && !back->tracks[1].used());
    }
    { // a keygroup's fader: its sound's volume first, pitch bend too; Motion's bend lane; a MIDI file's notes its keys
        CHECK(fader_default(EngineKind::Keygroup) == FaderTarget::PadVolume && fader_takes(EngineKind::Keygroup, FaderTarget::PitchBend)
              && fader_takes(EngineKind::Keygroup, FaderTarget::KitVolume) && !fader_takes(EngineKind::Plugin, FaderTarget::PadVolume));
        Track keys;
        keys.engine = EngineKind::Keygroup;
        keys.bend = 20;
        keys.pads[0].sample = 1; keys.pads[0].level = 90;
        CHECK(fader_target_binding(keys, FaderTarget::PadVolume).kind == Binding::Kind::PadLevel && fader_target_binding(keys, FaderTarget::PadVolume).value == 90);
        CHECK(motion_param(keys, motion_lane(motion_bend)).value == 20 && lane_binding(keys, 0, motion_lane(motion_bend)).kind == Binding::Kind::PitchBend);
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        project->tracks[1] = keys;
        project->tracks[1].set_name("KEYS");
        Event e; e.key = 64; e.velocity = 100;
        bank->at[1][0].add(e);
        auto tracks = smf_tracks(*project, *bank);
        CHECK(tracks.size() == 1 && tracks[0].channel != 10 && tracks[0].notes.size() == 1 && tracks[0].notes[0].key == 64);
    }
    { // every track in use: the pad stays on its kit
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        std::strcpy(project->samples.edit(0).path, "/card/Kick.wav");
        std::strcpy(project->samples.edit(1).path, "/card/Keys/Piano.sfz");
        for (auto& t : project->tracks) load_sound(t, EngineKind::Sampler, 0);
        project->tracks[0].pads[0].sample = 0;
        project->tracks[0].pads[1].sample = 1;
        auto moved = migrate_instrument_pads(*project, *bank);
        CHECK(moved.moved == 0 && moved.left == 1 && project->tracks[0].pads[1].used());
    }
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
        PadSound pad; pad.sample = 2; pad.start = 1000; pad.end = 20000; pad.mono = false; // Poly: two hits sound
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
        PadSound pad; pad.sample = 2; pad.play = PadPlay::Gate; pad.cloud = true; pad.cloud_density = 80; pad.cloud_position = 50;
        e.set_pad(2, pad);
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
        e.move_pad(2, PadParam::CloudDensity, 100); e.move_pad(2, PadParam::CloudSize, 100);
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
        auto host_held = std::make_unique<EngineHost>(); // on the heap: a test's frame stays under the 8 MB stack
        auto& host = *host_held;
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
        // A pad with layers: the voice of any layer's sample is the pad's (the picture shows its first layer); a
        // pad without layers keeps to its own sample.
        {
            HeadTrail layered;
            TrackTaps lt;
            lt.track = 3; lt.count = 1;
            auto& lv = lt.voice[0];
            lv.serial = 9; lv.pad = 8; lv.slot = 28; lv.from = 0; lv.to = 20000; lv.at = 10000; lv.step = 1; lv.level = 1; lv.age = 10000;
            layered.update(lt, 0, 5000);
            PadSound two; two.sample = 27; two.layers[0].sample = 28;
            PadSound one; one.sample = 27;
            auto heads = layered.heads();
            CHECK(pad_voices(heads, 8, uint16_t(27)).count == 0);          // by its first sample alone: nothing
            auto both = pad_voices(heads, 8, two);
            CHECK(both.count == 1 && both.sounding == 1 && both.head[0]->slot == 28 && std::abs(head_through(*both.head[0]) - 0.5f) < 0.01f);
            CHECK(pad_voices(heads, 8, one).count == 0 && pad_voices(heads, 9, two).count == 0);
        }
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
    sl.whole(1000, 12000); sl.add(5000, 20000); sl.add(9000, 20000);
    PlayHead h;
    h.serial = 3; h.pad = 128; h.slot = 4; h.from = 5000; h.to = 9000; h.at = 6000; h.step = 1; h.level = 0.8f; h.wake = 1;
    CHECK(chop_sound(&h, sl, 20000, false).slice == 1 && !chop_sound(&h, sl, 20000, false).take);
    CHECK(chop_sound(nullptr, sl, 20000, false).slice == -1 && chop_sound(&h, Slices {}, 20000, false).slice == -1);
    h.from = 9000; h.to = 12000; // the last slice, up to its own end
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
    sl.whole(1000, 12000); h.level = 0.8f; // one slice, the whole region: the take while armed, else that slice
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
// PRESETS' folders (plugin/preset_rows.h): a folder's folders first, each with how many presets it holds at any
// depth, then its own presets; the one picked last and the folders holding it marked; where PRESETS opens on it, and
// the row of the folder B1 comes back out of. Presets with no folder list at the top, as before.
void preset_folders() {
    PluginProgramList list({{0, "Init", ""}, {1, "Deep", "Factory/Basses"}, {2, "Sub", "Factory/Basses"},
                            {3, "Glass", "Factory/Keys"}, {4, "Rough", "Factory/Basses/Dirty"},
                            {5, "Mine", "User"}, {6, "Bad", "Factorya"}});
    auto top = preset_rows(list, "", 4);
    CHECK(top.size() == 4);
    CHECK(top[0].folder && top[0].name == "Factory" && top[0].count == 4 && top[0].at == 1 && top[0].current);
    CHECK(top[1].folder && top[1].name == "User" && top[1].count == 1 && !top[1].current);
    CHECK(top[2].folder && top[2].name == "Factorya" && top[2].count == 1);
    CHECK(!top[3].folder && top[3].name == "Init" && top[3].at == 0);
    auto factory = preset_rows(list, "Factory", 4);
    CHECK(factory.size() == 2 && factory[0].name == "Basses" && factory[0].count == 3 && factory[0].current);
    CHECK(factory[1].name == "Keys" && factory[1].count == 1 && !factory[1].current);
    auto basses = preset_rows(list, "Factory/Basses", 4);
    CHECK(basses.size() == 3 && basses[0].folder && basses[0].name == "Dirty" && basses[0].current);
    CHECK(!basses[1].folder && basses[1].name == "Deep" && basses[1].at == 1 && !basses[2].current);
    CHECK(preset_rows(list, "Nowhere", 4).empty());

    auto place = preset_place(list, 2);
    CHECK(place.folder == "Factory/Basses" && place.row == 2); // after Dirty and Deep
    CHECK(preset_place(list, -1).folder.empty() && preset_place(list, -1).row == 0);
    CHECK(preset_folder_up("Factory/Basses/Dirty") == "Factory/Basses" && preset_folder_up("Factory").empty());
    CHECK(preset_folder_name("Factory/Basses") == "Basses" && preset_folder_name("User") == "User");
    CHECK(preset_row_of_folder(list, "Factory/Keys", -1) == 1 && preset_row_of_folder(list, "User", -1) == 1);

    // A CLAP location's name as its folder: "/" a level (Melodee Modules: a synth, then its Factory and SysEx files).
    CHECK(preset_location_folder(" PPG / 2.2 factory ") == "PPG/2.2 factory" && preset_location_folder("/a//b/") == "a/b");
    CHECK(preset_location_folder("Factory Presets") == "Factory Presets" && preset_location_folder(" / ").empty());
    PluginProgramList synths({{0, "INIT", preset_location_folder("PPG/Factory")}, {1, "PROGRAM 00", "PPG/2.2 factory"},
                              {2, "FAT BASS", preset_location_folder("TRIO")}});
    auto by_synth = preset_rows(synths, "", 2);
    CHECK(by_synth.size() == 2 && by_synth[0].name == "PPG" && by_synth[0].count == 2 && by_synth[1].name == "TRIO" &&
          by_synth[1].current);
    auto ppg = preset_rows(synths, "PPG", 2);
    CHECK(ppg.size() == 2 && ppg[0].folder && ppg[0].name == "Factory" && ppg[1].name == "2.2 factory");

    PluginProgramList flat({{0, "One", ""}, {3, "Four", ""}}); // a VST3's programs: no folders
    auto rows = preset_rows(flat, "", 3);
    CHECK(rows.size() == 2 && !rows[0].folder && rows[1].name == "Four" && rows[1].current && rows[1].at == 1);
    PluginProgramList none;
    CHECK(none.empty() && preset_rows(none, "", -1).empty() && none.begin() == none.end());
}

// Neural Amp Modeler (plugin/nam.h): its pages from Melodee's library, Model, Cab and Output first, the tone stack
// next, the rest in its own order, Slim among them; long names shorter on the cards; its two folders made under the home.
void nam_page() {
    namespace fs = std::filesystem;
    const std::vector<std::string> names {"Input", "Threshold", "Bass", "Middle", "Treble", "Output", "NoiseGateActive",
                                          "ToneStack", "IRToggle", "CalibrateInput", "InputCalibrationLevel", "OutputMode",
                                          "Slim", "Model", "Cab"};
    // Its pages: Melodee's library page file (plugin/page_library.h), the order and short names the cards had before.
    PageFile file;
    std::string why;
    CHECK(read_page_file(page_library[0], file, why));
    PluginIdentity who; who.name = "NeuralAmpModeler";
    CHECK(page_file_matches(file, who));
    std::vector<int> shown;
    std::vector<std::string> ids;
    for (size_t i = 0; i < names.size(); ++i) { shown.push_back(int(i)); ids.push_back(std::to_string(i)); }
    std::vector<std::string> problems;
    auto layout = lay_out(resolve_page_file(file, shown, names, ids, problems));
    CHECK(problems.empty());
    CHECK((layout.slots == std::vector<int> {13, 14, 5, 2, 3, 4, 0, 1, 6, 7, 8, 11, 9, 10, 12})); // Slim last, with the rest
    CHECK((layout.names == std::vector<std::string> {"AMP", "TONE", "INPUT", "SWITCHES", "MORE"}));
    CHECK(layout.labels[8] == "Gate" && layout.labels[13] == "Cal level" && layout.labels[0].empty());
    CHECK(is_nam("NeuralAmpModeler") && !is_nam("TONE3000"));
    auto home = fs::temp_directory_path() / ("melodee-nam-" + std::to_string(getpid()));
    nam_folders(home);
    CHECK(fs::is_directory(home / "Neural Amp Modeler" / "Models") && fs::is_directory(home / "Neural Amp Modeler" / "IRs"));
    nam_folders(home); // there already
    nam_folders({});
    fs::remove_all(home);
}

// The card's plug-in folder (plugin/plugin_files.h): Melodee/Plugins. A path saved in the old top-level Plugins folder,
// relative or absolute, opens from Melodee/Plugins once it is there, else where it was; any other path as saved.
void card_plugin_paths() {
    namespace fs = std::filesystem;
    auto top = fs::temp_directory_path() / ("melodee-plugins-" + std::to_string(getpid()));
    const std::string t = top.string();
    fs::remove_all(top);
    fs::create_directories(top / "Plugins" / "Old.vst3");
    fs::create_directories(top / "Melodee" / "Plugins" / "New.vst3");
    CHECK(plugin_folder(t) == t + "/Melodee/Plugins" && plugin_folder("").empty());
    CHECK(card_plugin_path(t, "Plugins/Old.vst3") == t + "/Plugins/Old.vst3");                  // not moved yet
    CHECK(card_plugin_path(t, t + "/Plugins/Old.vst3") == t + "/Plugins/Old.vst3");
    CHECK(card_plugin_path(t, "Plugins/New.vst3") == t + "/Melodee/Plugins/New.vst3");          // moved
    CHECK(card_plugin_path(t, t + "/Plugins/New.vst3") == t + "/Melodee/Plugins/New.vst3");
    CHECK(card_plugin_path(t, "Plugins/Gone.vst3") == t + "/Melodee/Plugins/Gone.vst3");        // on neither: the new place
    CHECK(card_plugin_path(t, "Melodee/Plugins/New.vst3") == t + "/Melodee/Plugins/New.vst3");
    CHECK(card_plugin_path(t, "/elsewhere/Plugins/X.vst3") == "/elsewhere/Plugins/X.vst3");
    CHECK(card_plugin_path("", "Plugins/X.vst3") == "Plugins/X.vst3");                          // no card
    fs::create_directories(top / "Melodee" / "Plugins" / "Old.vst3");                           // both: the new one
    CHECK(card_plugin_path(t, "Plugins/Old.vst3") == t + "/Melodee/Plugins/Old.vst3");
    fs::remove_all(top);
}

// Files saved on another card (shell/card_paths.h, issue 72): a card mounts at /media/<its volume id>, so a project,
// bank or settings file from another card names its files under a folder that is not there. They are looked for at
// the same place on the card in now. A path on this card, off the mounts, relative, or still there stays as saved.
void card_paths_moved() {
    namespace fs = std::filesystem;
    auto mounts = fs::temp_directory_path() / ("melodee-mounts-" + std::to_string(getpid()));
    fs::remove_all(mounts);
    const std::string m = mounts.string(), top = m + "/NEW-CARD", old = m + "/3265-3932";
    const std::string kick = "/Melodee/Samples/Kit/Kick.wav";
    fs::create_directories(top + "/Melodee/Samples/Kit");
    CHECK(on_card_now(old + kick, top) == top + kick);
    CHECK(on_card_now(old + kick, top + "/") == top + kick);
    CHECK(on_card_now(old + "/Melodee/../Melodee/Samples/Kit/Kick.wav", top) == top + kick);
    CHECK(on_card_now(top + kick, top) == top + kick);                                  // this card
    CHECK(on_card_now("/content/Samples/Kick.wav", top) == "/content/Samples/Kick.wav"); // the internal drive
    CHECK(on_card_now("Melodee/Plugins/X.vst3", top) == "Melodee/Plugins/X.vst3");       // relative
    CHECK(on_card_now("(memory) Take 1", top) == "(memory) Take 1");
    CHECK(on_card_now(old, top) == old);                                                 // the mount itself
    CHECK(on_card_now(m + "2/x.wav", top) == m + "2/x.wav");                              // beside the mounts
    CHECK(on_card_now(old + kick, "") == old + kick);                                     // no card
    CHECK(on_card_now("", top).empty());
    CHECK(on_card_now("/x.wav", "/card") == "/x.wav");                                    // a card at the top: no mounts
    fs::create_directories(m + "/USB-STICK/Drums");
    { std::ofstream(m + "/USB-STICK/Drums/Snare.wav") << "x"; }
    CHECK(on_card_now(m + "/USB-STICK/Drums/Snare.wav", top) == m + "/USB-STICK/Drums/Snare.wav"); // another drive in

    Project project;
    std::snprintf(project.samples.edit(3).path, sizeof(SampleRef::path), "%s", (old + kick).c_str());
    std::snprintf(project.samples.edit(700).path, sizeof(SampleRef::path), "%s", (top + "/a.wav").c_str());
    std::snprintf(project.plugins[1].path, sizeof(project.plugins[1].path), "%s", (old + "/Melodee/Plugins/Surge XT.vst3").c_str());
    std::snprintf(project.plugins[2].path, sizeof(project.plugins[2].path), "%s", "Melodee/Plugins/Mine");
    CHECK(samples_on_card_now(project, top));
    CHECK(std::string(project.samples[3].path) == top + kick);
    CHECK(std::string(project.samples[700].path) == top + "/a.wav");
    CHECK(!project.samples[4].used());
    CHECK(std::string(project.plugins[1].path) == top + "/Melodee/Plugins/Surge XT.vst3");
    CHECK(std::string(project.plugins[2].path) == "Melodee/Plugins/Mine");
    CHECK(!samples_on_card_now(project, top)); // already on this card
    CHECK(!samples_on_card_now(project, ""));
    Project full; // 255 characters saved; on a card whose folder name is longer it would not fit: kept as saved
    const std::string longest = old + "/" + std::string(sizeof(SampleRef::path) - old.size() - 2, 'x');
    std::snprintf(full.samples.edit(5).path, sizeof(SampleRef::path), "%s", longest.c_str());
    CHECK(std::string(full.samples[5].path) == longest);
    CHECK(!samples_on_card_now(full, m + "/A-LONGER-CARD"));
    CHECK(std::string(full.samples[5].path) == longest);

    BankFile bank;
    std::snprintf(bank.samples[0].path, sizeof(SampleRef::path), "%s", (old + kick).c_str());
    std::snprintf(bank.samples[17].path, sizeof(SampleRef::path), "%s", "/content/Samples/Kick.wav");
    samples_on_card_now(bank, top);
    CHECK(std::string(bank.samples[0].path) == top + kick);
    CHECK(std::string(bank.samples[17].path) == "/content/Samples/Kick.wav");
    CHECK(!bank.samples[1].used());
    fs::remove_all(mounts);
}

// An Open MPC skin's Q-Link pages (plugin/plugin_files.h): a "Column" page (every toolkit skin) is read in its
// layout's slot order, Q-Links 13, 9, 5, 1, 14, ... 4, so each three are one K1-K3 row as the skin shows them; any
// other direction in Q-Link number order. -1 skipped, each parameter once.
void qlink_pages() {
    // Page 1 like Melodee Poly's OSC page: slots 0-5 FREQ PW SAW PULSE NOISE B SAW are parameters 0-5, on Q-Links
    // 13 9 5 1 14 10; Q-Link 6 unused (-1); 2 again on Q-Link 2; its direction after its Q-Links. Page 2 "Row":
    // number order. Page 3 no direction: number order.
    const char* skin = R"({"Screen Mode Q-Links": {"name": "Poly", "map": [
        {"Tab": 1, "SubTab": 1, "Q-Links": {"Q-Link 1": 3, "Q-Link 2": 2, "Q-Link 5": 2, "Q-Link 6": -1, "Q-Link 9": 1,
            "Q-Link 10": 5, "Q-Link 13": 0, "Q-Link 14": 4, "Q-Link 16": 6}, "Bank Direction": "Column"},
        {"Tab": 2, "SubTab": 1, "Bank Direction": "Row", "Q-Links": {"Q-Link 1": 9, "Q-Link 2": 8, "Q-Link 13": 7}},
        {"Tab": 3, "SubTab": 1, "Q-Links": {"Q-Link 4": 11, "Q-Link 3": 10, "Q-Link 1": 0, "Q-Link 17": 12}}]}})";
    CHECK((qlink_order_in(skin) == std::vector<int> {0, 1, 2, 3, 4, 5, 6, 9, 8, 7, 10, 11}));
    CHECK(qlink_order_in("{\"Screen Mode Q-Links\": {\"map\": [").empty() && qlink_order_in("").empty()); // cut short
    // Page by page, with the tab each is on; a parameter may be on two pages (0 on pages 1 and 3).
    auto pages = qlink_pages_in(skin);
    CHECK(pages.size() == 3);
    CHECK(pages.size() == 3 && pages[0].tab == 1 && pages[1].tab == 2 && pages[2].tab == 3);
    CHECK(pages.size() == 3 && (pages[0].params == std::vector<int> {0, 1, 2, 3, 4, 5, 6}));
    CHECK(pages.size() == 3 && (pages[1].params == std::vector<int> {9, 8, 7}) && (pages[2].params == std::vector<int> {0, 10, 11}));
    // Named after TUI.json's tabs (Melodee Poly's: OSC, FILTER, ...); a tab's second page " 2"; no name: Q-LINKS n.
    const char* tui = R"({"pageData": {"version": 1, "componentDefinitions": {"tabName": "not this"},
        "tabs": [{"tabName": "OSC", "components": [{"tabName": "nor this"}]}, {"tabName": "FILTER"}, {"other": 1}]}})";
    auto tabs = skin_tab_names_in(tui);
    CHECK((tabs == std::vector<std::string> {"OSC", "FILTER", ""}));
    CHECK(skin_tab_names_in("{\"pageData\": {\"tabs\": [").empty() && skin_tab_names_in("").empty());
    std::vector<QLinkPage> raw {{1, {0, 1}}, {2, {2}}, {1, {3}}, {3, {4}}, {0, {5}}};
    auto named = named_qlink_pages(raw, tabs);
    CHECK(named.size() == 5);
    CHECK(named.size() == 5 && named[0].name == "OSC" && named[1].name == "FILTER" && named[2].name == "OSC 2"
          && named[3].name == "Q-LINKS 4" && named[4].name == "Q-LINKS 5");
    CHECK(named.size() == 5 && (named[2].params == std::vector<int> {3}));
}

// Akai's plugin.json in an MPC plug-in folder (plugin/plugin_files.h): name, instrument or effect, and the libraries
// by build, each kept inside the folder.
void plugin_json() {
    // OxZygen 0.7.7's -mpc-sdk.zip (an Open MPC release in the SDK's folder): both builds at the folder's top.
    auto ox = plugin_json_in(R"({"name": "OxZygen", "manufacturer": "oxzygen", "version": "0.7.7", "kind": "instrument",
        "uid": "0x4f785a79", "numInputs": 0, "numOutputs": 2, "file64": "oxzygen-arm64.so", "file32": "oxzygen-arm32.so"})");
    CHECK(ox.name == "OxZygen" && ox.instrument == 1);
    CHECK(ox.file64 == "oxzygen-arm64.so" && ox.file32 == "oxzygen-arm32.so" && ox.file.empty());
    // An SDK build: libraries in VST/; isInstrument wins over kind.
    auto sdk = plugin_json_in(R"({"name": "Drive", "kind": "instrument", "isInstrument": false, "file64": "VST/drive-arm64.so"})");
    CHECK(sdk.instrument == 0 && sdk.file64 == "VST/drive-arm64.so");
    CHECK(plugin_json_in(R"({"kind": "Effect", "file": "x.so"})").instrument == 0);
    CHECK(plugin_json_in(R"({"isInstrument": 1})").instrument == 1);
    CHECK(plugin_json_in(R"({"kind": "midi"})").instrument == -1); // not said: opened once to find out
    // Paths out of the folder are dropped; a backslash reads as a slash.
    auto out = plugin_json_in(R"({"file64": "../other/x.so", "file32": "/usr/lib/x.so", "file": "C:\\VST\\x.so"})");
    CHECK(out.file64.empty() && out.file32.empty() && out.file.empty());
    CHECK(folder_path_in("VST\\x.so") == "VST/x.so" && folder_path_in("a/../x.so").empty() && folder_path_in("..").empty());
    CHECK(folder_path_in("x..so") == "x..so" && folder_path_in("").empty());
    // Not an object: nothing.
    CHECK(plugin_json_in("[1]").name.empty() && plugin_json_in("{\"name\": \"cut").name.empty() && plugin_json_in("").instrument == -1);
}

// MPC plug-in presets (plugin/xpl_preset.h): JUCE's base64 and standard base64, VST2 bank and program files turned
// into Melodee's VST2 state, and an .xpl's name and ids.
void xpl_presets() {
    auto text = [](const std::vector<uint8_t>& b) { return std::string(b.begin(), b.end()); };
    // JUCE's base64, vectors from an encoder written apart from this one; spaces passed over, a cut short one zeroed.
    CHECK(text(juce_base64_decode("4.CMlaKA")) == "CcnK" && text(juce_base64_decode("8.MUFauQVYkEB")) == "Melodee!");
    CHECK(text(juce_base64_decode(" 8.MUFa\nuQVYkEB ")) == "Melodee!" && text(juce_base64_decode("8.MUFauQVYkEB", 3)) == "Mel");
    CHECK(juce_base64_decode("MUFauQVYkEB").empty() && juce_base64_decode("x.MUF").empty() && juce_base64_decode("").empty());
    CHECK((juce_base64_decode("2.") == std::vector<uint8_t> {0, 0}));
    CHECK(text(base64_decode("TWVsb2RlZSE=")) == "Melodee!" && text(base64_decode("TWVs\nb2Rl ZSE=", 4)) == "Melo");
    CHECK(base64_decode("TWVs*b2RlZSE=").empty());
    // VST2 bank and program files, built here as JUCE's host writes them (big-endian).
    auto be = [](std::vector<uint8_t>& b, uint32_t v) { for (int s = 24; s >= 0; s -= 8) b.push_back(uint8_t(v >> s)); };
    auto word = [](std::vector<uint8_t>& b, const char* w) { b.insert(b.end(), w, w + 4); };
    auto head = [&](const char* kind, uint32_t version, uint32_t count) {
        std::vector<uint8_t> b;
        word(b, "CcnK"); be(b, 0); word(b, kind); be(b, version); word(b, "OxZy"); be(b, 740); be(b, count);
        return b;
    };
    auto pad = [](std::vector<uint8_t>& b, size_t to) { b.resize(to, 0); };
    auto program = [&](std::vector<float> values) { // FxCk: name at 28, values at 56
        auto b = head("FxCk", 1, uint32_t(values.size()));
        pad(b, 56);
        for (float v : values) { uint32_t w; std::memcpy(&w, &v, 4); be(b, w); }
        return b;
    };
    std::string chunk = "master=94;preset=1;";
    auto bank = head("FBCh", 1, 1); // numPrograms at 24, current at 28, 124 spare, chunk size at 156
    pad(bank, 156); be(bank, uint32_t(chunk.size())); bank.insert(bank.end(), chunk.begin(), chunk.end());
    uint32_t id = 0;
    auto state = vst2_state_from_fx(bank, &id);
    CHECK(text(state) == std::string("MVS2\x01", 5) + chunk && id == 0x4f785a79);
    auto one = head("FPCh", 1, 1);
    pad(one, 56); be(one, 3); word(one, "abc!");
    CHECK(text(vst2_state_from_fx(one)) == std::string("MVS2\x03", 5) + "abc"); // only the chunk's 3 bytes
    auto values = vst2_state_from_fx(program({0.25f, 0.5f}));
    CHECK(values.size() == 13 && values[4] == vst2::state::parameters);
    if (values.size() == 13) {
        float a, b;
        std::memcpy(&a, values.data() + 5, 4); std::memcpy(&b, values.data() + 9, 4);
        CHECK(a == 0.25f && b == 0.5f);
    }
    auto params = head("FxBk", 2, 2); // version 2: on program 2 of 2
    be(params, 1); pad(params, 156);
    for (auto p : {program({0.1f}), program({0.75f})}) params.insert(params.end(), p.begin(), p.end());
    auto second = vst2_state_from_fx(params);
    CHECK(second.size() == 9);
    if (second.size() == 9) { float v; std::memcpy(&v, second.data() + 5, 4); CHECK(v == 0.75f); }
    CHECK(text(vst2_state_from_fx({'r', 'a', 'w'})) == std::string("MVS2\x01raw", 8)); // no header: the chunk
    auto cut = bank;
    cut.resize(cut.size() - 2);
    CHECK(vst2_state_from_fx(cut).empty() && vst2_state_from_fx({}).empty());
    auto odd = head("FxQQ", 1, 0);
    CHECK(vst2_state_from_fx(odd).empty());
    // An .xpl as MPC OS writes one: its name (entities undone), its plug-in's uid, the state's id; JUCE's base64.
    auto juce64 = [](const std::vector<uint8_t>& b) {
        static const char* a = ".ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+";
        std::string out = std::to_string(b.size()) + ".";
        for (size_t bit = 0; bit < b.size() * 8; bit += 6) {
            unsigned v = 0;
            for (unsigned k = 0; k < 6; ++k)
                if (bit + k < b.size() * 8 && (b[(bit + k) >> 3] >> ((bit + k) & 7) & 1)) v |= 1u << k;
            out += a[v];
        }
        return out;
    };
    std::string xpl = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<pluginstate>\n  <version file=\"1\"/>\n"
                      "  <PLUGIN name=\"OxZygen\" format=\"VST\" file=\"%payload-path%/oxzygen - VST - OxZygen/oxzygen.so\" uid=\"4f785a79\" isInstrument=\"1\"/>\n"
                      "  <preset> VOX Angel &amp; Lead&#33; </preset>\n  <state>" + juce64(bank) + "</state>\n</pluginstate>\n";
    auto p = xpl_preset_in(xpl);
    CHECK(p.name == "VOX Angel & Lead!" && p.uid == 0x4f785a79 && p.fx_id == 0x4f785a79 && p.state == state);
    CHECK(p.for_plugin(0x4f785a79) && !p.for_plugin(0x4d64506c));
    auto listed = xpl_preset_in(xpl, false); // listing: no state, ids still read
    CHECK(listed.state.empty() && listed.name == p.name && listed.fx_id == 0x4f785a79);
    // <state64>, standard base64 ("TWVsb2RlZSE=" is "Melodee!"): no header, so the chunk itself; no uid said.
    auto sdk = xpl_preset_in("<pluginstate><PLUGIN name=\"X\"/><preset>Init</preset><state64>TWVsb2RlZSE=</state64></pluginstate>");
    CHECK(sdk.name == "Init" && sdk.uid == 0 && sdk.fx_id == 0 && text(sdk.state) == std::string("MVS2\x01Melodee!", 13));
    CHECK(sdk.for_plugin(12345));
    CHECK(xpl_preset_in("<pluginstate><PLUGIN uid=\"zz\"/></pluginstate>").uid == 0 && xpl_preset_in("").state.empty());
    // A program the plug-in lists already: the same name, or cut to a VST2 program name's 23 characters.
    CHECK(same_preset_name("VOX Angel Lead", " vox angel lead ") && same_preset_name("SYN Analog Brass Sectio", "SYN Analog Brass Section"));
    CHECK(!same_preset_name("Bass", "Bass 2") && !same_preset_name("VOX Angel Lead 2", "VOX Angel Lead") && !same_preset_name("", "Init"));
    CHECK(!same_preset_name("SYN Analog Brass Sectiq", "SYN Analog Brass Section"));
}

// A plug-in's pages (plugin/plugin_pages.h): each page's parameters in rows of three on K1-K3, a page's last row
// filled with -1, so no row holds two pages; groups make pages, with MAIN for the parameters outside any group.
void plugin_page_layout() {
    // Remote-controls-like pages: 8, then 2, then one left empty by keep_only, then 3 with a repeat.
    std::vector<PluginPage> pages {{"OSC", {0, 1, 2, 3, 4, 5, 6, 7}}, {"AMP", {8, 9}}, {"HIDDEN", {20}}, {"FX", {10, 11, 10, 12}}};
    keep_only(pages, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12});
    CHECK(pages[2].params.empty());
    auto layout = lay_out(pages);
    CHECK(layout.pages() == 3 && (layout.names == std::vector<std::string> {"OSC", "AMP", "FX"}));
    CHECK((layout.slots == std::vector<int> {0, 1, 2, 3, 4, 5, 6, 7, -1, 8, 9, -1, 10, 11, 12}));
    CHECK(layout.rows() == 5 && (layout.first_rows == std::vector<uint16_t> {0, 3, 4}));
    CHECK(layout.page_of(0) == 0 && layout.page_of(2) == 0 && layout.page_of(3) == 1 && layout.page_of(4) == 2 && layout.page_of(9) == 2);
    CHECK(layout.rows_of(0) == 3 && layout.rows_of(1) == 1 && layout.rows_of(2) == 1 && layout.rows_of(3) == 0);
    CHECK(layout.at(8) == -1 && layout.at(14) == 12 && layout.at(15) == -1);
    CHECK(lay_out({}).rows() == 0 && lay_out({}).page_of(5) == 0);

    // Groups: none named, one list; named, MAIN first for the rest, in the order the groups first appear.
    auto flat = group_pages({3, 4, 5}, {"", "", ""});
    CHECK(flat.size() == 1 && flat[0].name.empty() && (flat[0].params == std::vector<int> {3, 4, 5}));
    auto grouped = group_pages({0, 1, 2, 3, 4, 5}, {"Osc 1", "", "Filter", "Osc 1", "Amp/Env", ""});
    CHECK(grouped.size() == 4);
    CHECK(grouped.size() == 4 && grouped[0].name == "MAIN" && (grouped[0].params == std::vector<int> {1, 5}));
    CHECK(grouped.size() == 4 && grouped[1].name == "Osc 1" && (grouped[1].params == std::vector<int> {0, 3}));
    CHECK(grouped.size() == 4 && grouped[2].name == "Filter" && grouped[3].name == "Env");
    auto no_main = group_pages({0, 1}, {"A", "B"}); // every parameter in a group: no MAIN
    CHECK(no_main.size() == 2 && no_main[0].name == "A");
    auto same_leaf = group_pages({0, 1}, {"Osc 1/Env", "Filter/Env"}); // two groups ending alike: their whole paths
    CHECK(same_leaf.size() == 2 && same_leaf[0].name == "Osc 1 Env" && same_leaf[1].name == "Filter Env");

    // The plug-in's own pages first, then its groups for the rest (Surge XT: 5 remote pages hold 36 of 775).
    std::vector<int> shown {0, 1, 2, 3, 4, 5};
    std::vector<std::string> groups {"Osc", "Osc", "Filter", "", "Filter", "Env"};
    auto both = pages_with_groups({{"Macros", {2, 0}}, {"Empty", {}}}, shown, groups);
    CHECK(both.size() == 5); // Macros, then MAIN, Osc, Filter, Env for the five it leaves out
    CHECK(both.size() == 5 && both[0].name == "Macros" && (both[0].params == std::vector<int> {2, 0}));
    CHECK(both.size() == 5 && both[1].name == "MAIN" && (both[1].params == std::vector<int> {3}));
    CHECK(both.size() == 5 && both[2].name == "Osc" && (both[2].params == std::vector<int> {1}));
    CHECK(both.size() == 5 && both[3].name == "Filter" && (both[3].params == std::vector<int> {4}));
    CHECK(both.size() == 5 && both[4].name == "Env" && (both[4].params == std::vector<int> {5}));
    auto none = pages_with_groups({{"Q", {1, 2}}}, shown, std::vector<std::string>(6)); // no groups: only its own
    CHECK(none.size() == 1 && none[0].name == "Q");
    auto all_own = pages_with_groups({{"All", {0, 1, 2, 3, 4, 5}}}, shown, groups); // nothing left over
    CHECK(all_own.size() == 1);
    auto rest_ungrouped = pages_with_groups({{"A", {0, 1, 2, 4, 5}}}, shown, groups); // the rest outside every group
    CHECK(rest_ungrouped.size() == 2 && rest_ungrouped[1].name == "MAIN" && (rest_ungrouped[1].params == std::vector<int> {3}));
    auto no_own = pages_with_groups({}, shown, groups);
    CHECK(no_own.size() == 4 && no_own[0].name == "MAIN");

    // Pictures from names: an envelope of one prefix, a filter's cutoff with its resonance.
    std::vector<std::string> names {"Amp Attack", "Amp Decay", "Amp Sustain", "Amp Release", "Filter Attack", "Cutoff",
                                    "Resonance", "Drive", "A Filter 1 Cutoff", "A Filter 2 Resonance", "A Filter 1 Resonance"};
    auto pictured = lay_out({{"AMP", {4, 0, 1, 2, 3}}, {"FILTER", {5, 7, 6}}, {"SURGE", {8, 9, 10}}, {"NONE", {7, 4}}});
    find_pictures(pictured, names);
    CHECK(pictured.pictures.size() == 4);
    auto first = [&](unsigned page) { return pictured.pictures[page].empty() ? PagePictureSlots {} : pictured.pictures[page][0]; };
    CHECK(pictured.pictures.size() == 4 && pictured.pictures[0].size() == 1 && first(0).kind == PagePicture::Envelope); // Filter Attack alone: no envelope
    CHECK(pictured.pictures.size() == 4 && first(0).slots[0] == 1 && first(0).slots[1] == 2 && first(0).slots[2] == 3 && first(0).slots[3] == 4);
    CHECK(pictured.pictures.size() == 4 && first(1).kind == PagePicture::Filter && first(1).slots[0] == 6 && first(1).slots[1] == 8);
    CHECK(pictured.pictures.size() == 4 && first(2).kind == PagePicture::Filter && first(2).slots[0] == 9 && first(2).slots[1] == 11); // the same prefix's resonance
    CHECK(pictured.pictures.size() == 4 && pictured.pictures[3].empty() && picture_for(pictured, 3, 7).kind == PagePicture::None);
    // A page of both (Melodee Poly's FILTER tab): the one its row in hand turns.
    std::vector<std::string> poly {"Cutoff", "Resonance", "Filter Env Amount", "Filter Attack", "Filter Decay", "Filter Sustain", "Filter Release"};
    auto both_kinds = lay_out({{"FILTER", {0, 1, 2, 3, 4, 5, 6}}});
    find_pictures(both_kinds, poly);
    CHECK(both_kinds.pictures.size() == 1 && both_kinds.pictures[0].size() == 2);
    CHECK(picture_for(both_kinds, 0, 0).kind == PagePicture::Filter);   // Cutoff, Resonance, Env Amount in hand
    CHECK(picture_for(both_kinds, 0, 1).kind == PagePicture::Envelope); // Attack, Decay, Sustain in hand
    CHECK(picture_for(both_kinds, 0, 2).kind == PagePicture::Envelope); // Release: still the envelope's
    auto poly_tab = lay_out({{"FILTER", {0, 1, 2, 7, 8, 9, 3, 4, 5, 6}}}); // Key Track's row between: nothing on it, the filter nearest
    std::vector<std::string> poly2 = poly;
    poly2.insert(poly2.end(), {"Key Track", "Vel > Filter", "Vel > Amp"});
    find_pictures(poly_tab, poly2);
    CHECK(picture_for(poly_tab, 0, 1).kind == PagePicture::Filter);
    CHECK(picture_for(poly_tab, 0, 2).kind == PagePicture::Envelope);

    // Units from a value's text.
    double v = 0;
    CHECK(value_in("12.5 ms", false, v) && std::abs(v - 0.0125) < 1e-9);
    CHECK(value_in("1.2 s", false, v) && std::abs(v - 1.2) < 1e-9);
    CHECK(value_in("350ms", false, v) && std::abs(v - 0.35) < 1e-9);
    CHECK(value_in(" 2,5 sec", false, v) && std::abs(v - 2.5) < 1e-9);
    CHECK(value_in("2 kHz", true, v) && std::abs(v - 2000) < 1e-9);
    CHECK(value_in("440 Hz", true, v) && std::abs(v - 440) < 1e-9);
    CHECK(!value_in("45", false, v) && !value_in("62 %", true, v) && !value_in("Off", false, v) && !value_in("2 kHz", false, v));
}

// Companion screens' screen.json (plugin/companion_package.h): read, checked against a plug-in by its ID and every
// parameter's name at its index, refused with a reason, and none picked when two fit.
void companion_packages() {
    const char* json = R"({"name": "Panel", "sdk": 1, "plugin": {"vst2": "Pft5"},
        "params": [{"index": 0, "name": "PM Filt Env"}, {"index": 2, "name": " A Frequency ", "steps": 60}]})";
    CompanionManifest m;
    std::string why;
    CHECK(read_companion_manifest(json, m, why));
    CHECK(m.library == "screen-aarch64.so" && m.params.size() == 2 && m.params[1].name == "A Frequency" && m.params[1].steps == 60);
    CompanionTarget t;
    t.who.vst2 = int32_t(0x50667435); // 'Pft5'
    t.vst2 = true;
    t.names = {"PM Filt Env", "PM Osc B", "A Frequency"};
    CHECK(companion_fit(m, t, why) == CompanionFit::Fits);
    t.names[2] = "A Freq";
    CHECK(companion_fit(m, t, why) == CompanionFit::Wrong && why.find("parameter 2 is \"A Freq\"") != std::string::npos);
    t.names.resize(1);
    CHECK(companion_fit(m, t, why) == CompanionFit::Wrong && why.find("no parameter 2") != std::string::npos);
    t.who.vst2 = 1;
    CHECK(companion_fit(m, t, why) == CompanionFit::Other);
    t.who.vst2 = int32_t(0x50667435);
    t.vst2 = false;
    CHECK(companion_fit(m, t, why) == CompanionFit::Wrong); // not a VST2: not yet
    CHECK(read_companion_manifest(R"({"name": "x", "sdk": 1, "library": "custom.so", "plugin": {"name": "P"}, "params": [{"index": 0, "name": "A"}]})", m, why)
          && m.library == "custom.so");
    CHECK(!read_companion_manifest(R"({"name": "x", "sdk": 2, "plugin": {"name": "P"}, "params": [{"index": 0, "name": "A"}]})", m, why)
          && why.find("SDK 2") != std::string::npos);
    CHECK(!read_companion_manifest(R"({"name": "x", "sdk": 1, "library": "../x.so", "plugin": {"name": "P"}, "params": [{"index": 0, "name": "A"}]})", m, why));
    CHECK(!read_companion_manifest(R"({"name": "x", "sdk": 1, "plugin": {"name": "P"}, "params": []})", m, why));
    CHECK(!read_companion_manifest(R"({"name": "x", "sdk": 1, "plugin": {"name": "P"}, "params": [{"name": "A"}]})", m, why));
    CHECK(!read_companion_manifest(R"({"sdk": 1, "plugin": {"name": "P"}, "params": [{"index": 0, "name": "A"}]})", m, why));
    CHECK(!read_companion_manifest("{not json", m, why));
    std::string many = R"({"name": "x", "sdk": 1, "plugin": {"name": "P"}, "params": [)";
    for (unsigned n = 0; n <= companion_params_most; ++n) many += (n ? "," : "") + std::string(R"({"index": 0, "name": "A"})");
    CHECK(!read_companion_manifest(many + "]}", m, why) && why.find("more than") != std::string::npos);
    CHECK(pick_companion({CompanionFit::Other, CompanionFit::Fits, CompanionFit::Wrong}).package == 1);
    auto two = pick_companion({CompanionFit::Fits, CompanionFit::Fits});
    CHECK(two.package == -1 && two.ambiguous);
    CHECK(pick_companion({CompanionFit::Wrong}).package == -1 && !pick_companion({}).ambiguous);
    CHECK(plugin_binding_indexed(plugin_binding(3, plugin_index_slot + 58)) && !plugin_binding_indexed(plugin_binding(3, plugin_screen_slot)));
    CHECK(!plugin_binding_indexed(plugin_binding(3, 5)));
}

// Page files (plugin/page_file.h): which plug-in, its pages by names or IDs, labels, a last page of the rest,
// pictures; files that cannot be used say why.
void page_files() {
    const char* nam = R"({"melodee-pages": 1, "plugin": {"name": "NeuralAmpModeler"},
        "pages": [
          {"name": "AMP", "params": ["Model", "Cab", "Output"]},
          {"name": "INPUT", "params": ["Input", {"name": "NoiseGateActive", "label": "Gate"}, {"id": "7"}],
           "picture": {"kind": "value", "params": ["Input", "Output", "Bass", "Middle"]}},
          {"name": "EQ", "params": ["Bass"], "picture": {"kind": "eq", "bands": [{"freq": "Low Freq", "gain": "Bass", "q": "Gone"}]}},
          {"name": "MORE", "rest": true}]})";
    PageFile file;
    std::string why;
    CHECK(read_page_file(nam, file, why));
    CHECK(file.name == "NeuralAmpModeler" && file.pages.size() == 4 && file.pages[3].rest && !file.pages[0].rest);
    CHECK(file.pages.size() == 4 && file.pages[1].params.size() == 3 && file.pages[1].params[1].label == "Gate" && file.pages[1].params[2].id == "7");
    CHECK(file.pages.size() == 4 && file.pages[1].picture == PagePicture::Value && file.pages[1].parts.size() == 3); // values: three at most
    CHECK(file.pages.size() == 4 && file.pages[2].picture == PagePicture::Eq && file.pages[2].parts.size() == 3);

    PluginIdentity who; who.name = "NeuralAmpModeler"; who.vendor = "Steven Atkinson";
    CHECK(page_file_matches(file, who));
    who.name = "Other";
    CHECK(!page_file_matches(file, who));
    PageFile by_id; by_id.clap = "org.surge-synth-team.surge-xt"; by_id.vst2 = "NAMp";
    PluginIdentity surge; surge.clap = "org.surge-synth-team.surge-xt";
    PluginIdentity coded; coded.vst2 = int32_t('N' << 24 | 'A' << 16 | 'M' << 8 | 'p');
    CHECK(page_file_matches(by_id, surge) && page_file_matches(by_id, coded) && vst2_code(coded.vst2) == "NAMp" && vst2_code(5) == "5");
    PageFile vendored; vendored.name = "Reverb"; vendored.vendor = "Acme";
    PluginIdentity reverb; reverb.name = "Reverb"; reverb.vendor = "Other";
    CHECK(!page_file_matches(vendored, reverb));

    // Resolved against the plug-in: names by index, IDs by index; 9 is hidden (not shown).
    std::vector<std::string> names {"Model", "Cab", "Output", "Bass", "Middle", "Treble", "Input", "Threshold", "NoiseGateActive", "Slim"};
    std::vector<std::string> ids {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
    std::vector<int> shown {0, 1, 2, 3, 4, 5, 6, 7, 8};
    std::vector<std::string> problems;
    auto pages = resolve_page_file(file, shown, names, ids, problems);
    CHECK(pages.size() == 4);
    CHECK(pages.size() == 4 && (pages[1].params == std::vector<int> {6, 8, 7}) && pages[1].labels.size() == 3 && pages[1].labels[1] == "Gate");
    CHECK(pages.size() == 4 && (pages[3].params == std::vector<int> {4, 5})); // the rest: Middle, Treble (Slim is hidden)
    CHECK(pages.size() == 4 && (pages[1].picture_params == std::vector<int> {6, 2, 3}));
    CHECK(pages.size() == 4 && (pages[2].picture_params == std::vector<int> {-1, 3, -1}));
    CHECK((problems == std::vector<std::string> {"no parameter \"Low Freq\"", "no parameter \"Gone\""}));
    auto layout = lay_out(pages);
    CHECK(layout.labels.size() == layout.slots.size() && layout.labels[4] == "Gate"); // INPUT's second slot
    CHECK(layout.named.size() == 4 && !layout.named[0] && layout.named[1] && layout.named[2] && !layout.named[3]);
    CHECK(layout.pictures.size() == 4 && layout.pictures[1].size() == 1 && layout.pictures[1][0].kind == PagePicture::Value);
    CHECK(layout.pictures.size() == 4 && layout.pictures[1][0].slots[0] == 3 && layout.pictures[1][0].slots[1] == -1); // Output: not on INPUT
    CHECK(layout.pictures.size() == 4 && layout.pictures[1][0].params[1] == 2);
    find_pictures(layout, names); // the file's pictures stay
    CHECK(layout.pictures[1].size() == 1 && layout.pictures[1][0].kind == PagePicture::Value && picture_for(layout, 1, 1).kind == PagePicture::Value);

    // Files that cannot be used.
    PageFile bad;
    CHECK(!read_page_file("{\"pages\": [", bad, why) && why == "not JSON Melodee can read");
    CHECK(!read_page_file(R"({"plugin": {"name": "X"}, "pages": [{"name": "A"}]})", bad, why) && why == "no \"melodee-pages\": 1");
    CHECK(!read_page_file(R"({"melodee-pages": 2, "plugin": {"name": "X"}, "pages": [{}]})", bad, why) && why == "version 2 is newer than this Melodee");
    CHECK(!read_page_file(R"({"melodee-pages": 1, "pages": [{}]})", bad, why) && why == "names no plug-in");
    CHECK(!read_page_file(R"({"melodee-pages": 1, "plugin": {"lv2": "urn:x"}, "pages": []})", bad, why) && why == "has no pages");
    CHECK(read_page_file(R"({"melodee-pages": 1, "plugin": {"vst2": 1234}, "pages": [{"name": "A", "picture": {"kind": "spiral"}}], "skin": {}})", bad, why)
          && bad.vst2 == "1234" && bad.pages[0].picture == PagePicture::None); // unknown keys and kinds: skipped

    // A skin: the font, pages by name, each control's kind, box and part; a nested label is a control of its own;
    // a kind it does not know (a later version's) is skipped; the strip for an effect.
    const char* skinned = R"({"melodee-pages": 1, "plugin": {"name": "Melodee Poly"}, "pages": [{"name": "FILTER", "params": ["Cutoff"]}],
      "skin": {"font": "fonts/panel.ttf",
        "pages": {"FILTER": {"background": "art/filter.png", "controls": [
          {"param": "Cutoff", "kind": "knob", "strip": "art/knob.png", "frames": 128, "x": 136, "y": 22, "size": 28,
           "label": {"text": "CUTOFF", "x": 150, "y": 76, "colour": "#ECE3CF", "size": 9, "align": "left"}},
          {"param": {"id": "17"}, "kind": "switch", "image": "art/slide3.png", "frames": 3, "x": 148, "y": 101},
          {"param": "Amp Attack", "kind": "slider", "image": "art/track.png", "cap": "art/cap.png", "x": 170, "y": 20, "w": 16, "h": 84},
          {"param": "Cutoff", "kind": "value", "x": 120, "y": 60, "w": 60, "size": 13, "colour": "FFB347", "align": "right"},
          {"kind": "curve", "of": "filter", "x": 8, "y": 22, "w": 104, "h": 108, "colour": "FFB347"},
          {"kind": "canvas", "x": 0, "y": 0, "w": 10, "h": 10},
          {"kind": "scope", "x": 124, "y": 21, "w": 186, "h": 16}]}},
        "strip": {"background": "art/strip.png", "controls": [{"kind": "meter", "of": "out", "image": "art/vu.png", "frames": 32, "x": 4, "y": 4}]}}})";
    PageFile skin_file;
    CHECK(read_page_file(skinned, skin_file, why));
    const auto& sk = skin_file.skin;
    CHECK(sk.present && sk.font == "fonts/panel.ttf" && sk.pages.size() == 1 && sk.pages[0].first == "FILTER" && sk.has_strip);
    const auto& ctl = sk.pages.empty() ? SkinPageSpec {} : sk.pages[0].second;
    CHECK(ctl.background == "art/filter.png" && ctl.controls.size() == 7); // knob, its label, switch, slider, value, curve, scope
    if (ctl.controls.size() == 7) {
        const auto& knob = ctl.controls[0];
        CHECK(knob.kind == SkinKind::Knob && knob.param.name == "Cutoff" && knob.image == "art/knob.png" && knob.frames == 128
              && knob.x == 136 && knob.y == 22 && knob.w == 28 && knob.h == 28);
        const auto& label = ctl.controls[1];
        CHECK(label.kind == SkinKind::Label && label.text == "CUTOFF" && label.coloured && label.colour == 0xECE3CF && label.text_size == 11
              && label.align == -1 && label.x == 150); // 9 px is drawn at 11
        CHECK(ctl.controls[2].kind == SkinKind::Switch && ctl.controls[2].param.id == "17" && ctl.controls[2].frames == 3);
        CHECK(ctl.controls[3].kind == SkinKind::Slider && ctl.controls[3].cap == "art/cap.png" && ctl.controls[3].h == 84 && !ctl.controls[3].horizontal);
        CHECK(ctl.controls[4].kind == SkinKind::Value && ctl.controls[4].text_size == 13 && ctl.controls[4].w == 60 && ctl.controls[4].align == 1);
        CHECK(ctl.controls[5].kind == SkinKind::Curve && ctl.controls[5].of == "filter" && ctl.controls[5].colour == 0xFFB347);
        CHECK(ctl.controls[6].kind == SkinKind::Scope && !ctl.controls[6].coloured);
    }
    CHECK(sk.strip.background == "art/strip.png" && sk.strip.controls.size() == 1 && sk.strip.controls[0].kind == SkinKind::Meter
          && sk.strip.controls[0].of == "out" && sk.strip.controls[0].frames == 32);
    CHECK(!file.skin.present); // NAM's file above has none

    double db = 0;
    CHECK(decibels_in("-3.5 dB", db) && std::abs(db + 3.5) < 1e-9 && decibels_in("+6dB", db) && std::abs(db - 6) < 1e-9 && !decibels_in("6", db));
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
        CHECK(std::string(copy->plugins[1].path) == "Melodee/Plugins/Ellic Studio - VST - Melodee Poly" && std::string(copy->plugins[1].name) == "Melodee Poly");
        CHECK(text(1).compare(0, 5, std::string("MVS2\x01", 5)) == 0 && states[1].back() == 0);
        CHECK(text(1).find("melodee-poly 1\nmelodee=0,1,2,3,4,5,6,0,") != std::string::npos);
        auto values = text(1).substr(text(1).find("melodee=") + 8);
        values = values.substr(0, values.find('\n'));
        CHECK(std::count(values.begin(), values.end(), ',') == 52 && values.find(",33,") != std::string::npos); // 53 values
        CHECK(text(1).find("\nmelodee_mods=0:0:0,0:0:0,1:1:-45,0:0:0,") != std::string::npos);
        CHECK(copy->tracks[2].engine == EngineKind::Plugin && copy->tracks[2].plugin == 2
              && std::string(copy->plugins[2].path) == "Melodee/Plugins/Ellic Studio - VST - Melodee PD" && text(2).find("melodee-pd 1\n") == 5);
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
    // The bridge's converter (plugin/polyphase.h) gives what the base's SincResampler gives, frame for frame, at every
    // rate pair the bridge meets, and one too fine for a table (played through a SincResampler).
    {
        const std::pair<unsigned, unsigned> pairs[] {{44100, 48000}, {48000, 44100}, {44100, 96000}, {96000, 44100}, {44100, 47999}};
        for (auto [from, to] : pairs) {
            ::SincResampler<2> slow;
            PolyphaseResampler fast;
            slow.reset(from, to);
            fast.reset(from, to);
            std::vector<float> a, b;
            uint32_t seed = 1;
            for (unsigned n = 0; n < 20000; ++n) {
                seed = seed * 1664525u + 1013904223u;
                float l = float(int32_t(seed) >> 8) / float(1 << 23), r = 0.5f * float(std::sin(n * 0.03));
                slow.feed(l, r, [&](float x, float y) { a.push_back(x); a.push_back(y); });
                fast.feed(l, r, [&](float x, float y) { b.push_back(x); b.push_back(y); });
            }
            CHECK(a.size() == b.size());
            float worst = 0;
            for (size_t n = 0; n < std::min(a.size(), b.size()); ++n) worst = std::max(worst, std::fabs(a[n] - b[n]));
            CHECK(worst < 1e-5f);
        }
    }
    // Idle sleep (plugin/idle_gate.h): an instrument sleeps after half a second of silence with no key held and the
    // song not playing; a key, the pedal or the song keeps it awake; any MIDI wakes it. An effect sleeps once its input
    // and output are quiet, whatever the song does, and wakes on sound in its input.
    {
        const std::array<float, 128> silent {}, tone = [] { std::array<float, 128> t {}; for (unsigned f = 0; f < 128; ++f) t[f] = 0.3f * float(std::sin(f * 0.1)); return t; }();
        auto quiet_for = [&](IdleGate& g, unsigned blocks) { for (unsigned b = 0; b < blocks; ++b) g.heard(silent.data(), silent.data(), 128); };
        IdleGate g;
        g.reset(true, 48000);
        CHECK(!g.asleep(false, false, nullptr, nullptr, 128)); // not quiet for long enough yet
        quiet_for(g, 186);                                        // 0.496 s
        CHECK(!g.asleep(false, false, nullptr, nullptr, 128));
        quiet_for(g, 2);
        CHECK(g.asleep(false, false, nullptr, nullptr, 128));
        CHECK(!g.asleep(false, true, nullptr, nullptr, 128));     // the song playing: awake
        CHECK(!g.asleep(true, false, nullptr, nullptr, 128));     // MIDI for it: awake, and the quiet counts again
        CHECK(!g.asleep(false, false, nullptr, nullptr, 128));
        g.midi(0x90, 60, 100);
        quiet_for(g, 400);
        CHECK(!g.asleep(false, false, nullptr, nullptr, 128));    // a key held, its sound or not
        g.midi(0x90, 60, 0);                                      // note on at 0 lets go
        CHECK(g.keys_held() == 0 && g.asleep(false, false, nullptr, nullptr, 128));
        g.midi(0xb0, 64, 127);
        CHECK(!g.asleep(false, false, nullptr, nullptr, 128));    // the pedal down
        g.midi(0xb0, 64, 0);
        g.midi(0x91, 62, 90); g.midi(0x91, 64, 90);
        g.midi(0xb1, 123, 0);                                     // all notes off on that channel
        CHECK(g.keys_held() == 0 && g.asleep(false, false, nullptr, nullptr, 128));
        g.heard(tone.data(), tone.data(), 128);                   // a sound (a free-running drone) starts it counting again
        CHECK(!g.asleep(false, false, nullptr, nullptr, 128));
        IdleGate fx;
        fx.reset(false, 44100);
        quiet_for(fx, 200);
        CHECK(fx.asleep(false, true, silent.data(), silent.data(), 128)); // the song playing does not keep an effect awake
        CHECK(!fx.asleep(false, true, tone.data(), tone.data(), 128));    // sound in: awake
        CHECK(!fx.asleep(false, true, silent.data(), silent.data(), 128));
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

// The transport plug-ins hear (engine/effects.h, EffectClock; the rack hands it on as each plug-in's play head):
// stopped until Play, then where the song is in quarter notes from its top at the tempo; held where Stop left it;
// stopped through a count-in and from the top once it is over. A plug-in with a sequencer of its own (LXRXXL
// BAZZLINE) starts and stops with Melodee instead of playing from the moment it loads.
struct ClockPlugin final : PluginRunner {
    std::vector<EffectClock> heard;
    void midi(uint8_t, uint8_t, uint8_t) override {}
    void silence() override {}
    void render(float*, float*, unsigned) override { heard.push_back(*effect_clock_now); }
};

#include "melodee/shell/pad_lights.h"
#include "melodee/shell/step_lane.h"

// The pads light as what a press does there (shell/pad_context.h, shell/pad_lights.cpp), on five rungs.
void pad_lights() {
    using namespace pad_light;
    // The ladder: Faint keeps every channel that carries a tenth of each track's colour lit; 0.07 left track 1 pure red.
    auto channels = [](Rgb c) { return unsigned(c.r > 0) + unsigned(c.g > 0) + unsigned(c.b > 0); };
    for (unsigned t = 0; t < 16; ++t) {
        Rgb top = track(t, full).rgb, low = track(t, faint).rgb;
        CHECK((top.r < 13 || low.r) && (top.g < 13 || low.g) && (top.b < 13 || low.b));
    }
    CHECK(channels(track(0, 0.07f).rgb) == 1 && channels(track(0, full).rgb) == 3);
    CHECK(rung(0) == 0 && rung(faint) == 1 && rung(dim) == 2 && rung(mid) == 3 && rung(full) == 4 && rung(0.3f) == 1);

    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    auto host_held = std::make_unique<EngineHost>(); // on the heap: a test's frame stays under the 8 MB stack
    auto& host = *host_held;
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
    { // a voice that stops (a one-shot's end) fades as a pad let go, not dark at once; a voice's rise is not held back
        PadGlow glow;
        auto glowing = [&](uint32_t now) { PadScene scene {ui, *project, *bank, host, taps, plays, now, nullptr, 0}; scene.glow = &glow; pad_looks(scene, looks); };
        taps.voice[0].pad = 3; taps.voice[0].level = 1; glowing(1000); CHECK(is(3, Hue::Track, 4));
        taps.count = 0;
        glowing(1100); float soon = looks[3].level; CHECK(soon > dim && soon < full);
        glowing(1300); CHECK(looks[3].level < soon && looks[3].level > dim);
        glowing(3000); CHECK(is(3, Hue::Track, 2));
        taps.count = 1; glowing(3010); CHECK(is(3, Hue::Track, 4));
        ui.track = 0; glowing(3020); ui.track = 1; taps.count = 0; glowing(3030); CHECK(is(3, Hue::Track, 2)); // another track's glow is dropped
    }
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
    { // the level hit last fades as a pad let go when its voice stops, not dark at once
        PadGlow glow;
        auto glowing = [&](uint32_t now) { PadScene scene {ui, *project, *bank, host, taps, {}, now, nullptr, 0}; scene.glow = &glow; pad_looks(scene, looks); };
        ui.levels_hit = 1;
        glowing(1000); Look rest = looks[1];
        taps.track = 1; taps.count = 1; taps.voice[0].pad = int16_t(kit.levels_pad); taps.voice[0].level = 1;
        glowing(1010); CHECK(is(1, Hue::White, 4));
        taps.count = 0;
        glowing(1110); CHECK(looks[1].hue == Hue::White && looks[1].rgb.r > rest.rgb.r && looks[1].rgb.r < 255);
        glowing(3000); CHECK(looks[1].rgb.r == rest.rgb.r && looks[1].hue == rest.hue);
        taps = TrackTaps {}; ui.levels_hit = -1;
    }
    { // the sequence plays: its note lights the pad written at its pitch, not the pad hit last, though the voice is that pad's
        auto& p = bank->at[1][0];
        p.length = 16 * sixteenth_ticks;
        Event e; e.tick = 0; e.length = uint16_t(p.length); e.key = uint8_t(sampler_first_note + kit.levels_pad);
        e.pitch = int8_t(levels_of(*project, kit, 6).semitones);
        p.events.push_back(e);
        ui.levels_hit = 0; light({});
        auto stopped = looks;
        auto playing_held = std::make_unique<EngineHost>(); // on the heap: a test's frame stays under the 8 MB stack
        auto& playing = *playing_held;
        playing.load(*project, *bank);
        playing.send({Command::Type::Play});
        std::vector<int32_t> block(256);
        playing.render(block.data(), 128, 1);
        taps.track = 1; taps.count = 1; taps.voice[0].pad = int16_t(kit.levels_pad); taps.voice[0].level = 1;
        PadScene scene {ui, *project, *bank, playing, taps, {}, 1000, nullptr, 0};
        pad_looks(scene, looks);
        CHECK(playing.playing() && looks[6].hue == Hue::White && rung(looks[6].level) == 4);
        CHECK(looks[0].hue == stopped[0].hue && looks[0].level == stopped[0].level);
        taps = TrackTaps {}; p = Pattern {};
    }
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

    // The mixer's tracks: picked Full, heard Dim, muted Faint red; a solo leaves the rest Faint.
    ui.page = PageId::Mixer; project->tracks[3].engine = EngineKind::Plugin; project->tracks[3].mix.mute = true; light({});
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
    // Mute mode: a kit's pads on a kit page, the tracks on the mixer; muted Dim red, a solo white.
    ui.mute = true; light(plays);
    CHECK(is(8, Hue::Red, 2) && is(0, Hue::Track, 3) && is(12, Hue::None, 0));
    ui.page = PageId::Mixer; light({});
    CHECK(is(3, Hue::Red, 2) && is(0, Hue::White, 3) && is(5, Hue::Track, 1));
    ui.mute = false; project->tracks[0].mix.solo = false;
    // Song: the lane picked, a clip under the cursor Mid, not in the song Faint.
    ui.page = PageId::Song; project->song.clips[0].track = 0; project->song.clips[0].bars = 2; project->song.count = 1; light({});
    CHECK(is(1, Hue::Track, 4) && is(0, Hue::Track, 3) && is(5, Hue::Track, 1));

    // Seq held: the sequences, white (shell/sequences.h): the one in hand Full, one keeping notes on a track Faint,
    // one keeping nothing dark. On Patterns the selected track's patterns: the one in play Full, with notes Dim;
    // + Erase red; + Shift the sequences; a copy.
    auto& p = bank->at[1][0];
    put(p, 0, uint8_t(sampler_first_note + 1), 127);
    put(p, 2, uint8_t(sampler_first_note), 100);
    put(bank->at[1][2], 0, uint8_t(sampler_first_note), 100);
    ui.page = PageId::Stage; ui.seq_held = true; light(plays); // Seq held: the track's patterns, as on Patterns
    CHECK(is(0, Hue::Track, 4) && is(2, Hue::Track, 2) && is(5, Hue::None, 0));
    ui.page = PageId::Patterns; light(plays);
    CHECK(is(0, Hue::Track, 4) && is(2, Hue::Track, 2) && is(5, Hue::None, 0));
    ui.page = PageId::Stage;
    ui.erase = true; light(plays);
    CHECK(is(0, Hue::Red, 3) && is(2, Hue::Red, 2) && is(5, Hue::None, 0));
    project->tracks[1].in_scene[0] = 1; project->tracks[1].in_scene[2] = 3; // SEQ 1 (in hand) keeps P1, SEQ 3 P3
    ui.erase = false; ui.shift = true; light(plays);
    CHECK(is(0, Hue::White, 4) && is(2, Hue::White, 1) && is(1, Hue::None, 0));
    ui.shift = false; ui.copying = UiState::Copying::Patterns; ui.copy_from = 0; ui.copy_track = 1; light(plays);
    CHECK(is(0, Hue::White, 4) && is(2, Hue::Red, 1) && is(3, Hue::Track, 1));
    ui.copying = UiState::Copying::Off; ui.copy_from = -1; ui.seq_held = false;
    // Step Edit's pads on the steps, on a kit: the last pad's lane only, by velocity; past the pattern's end faint as
    // an empty step while a step there would make it longer (shell/spare_bar.h), dark at 128 bars.
    ui.page = PageId::StepEdit; ui.step_pads = true; light(plays);
    CHECK(is(0, Hue::Track, 4) && is(2, Hue::Track, 1) && is(1, Hue::Track, 1));
    p.length = 12 * sixteenth_ticks; light(plays);
    CHECK(is(12, Hue::Track, 1) && is(11, Hue::Track, 1));
    p.length = project->max_length() - 4 * sixteenth_ticks; ui.steps_bar = uint8_t(p.steps() / bar_steps); light(plays);
    CHECK(is(12, Hue::Track, 1) && is(11, Hue::Track, 1)); // it can still grow, to 128 bars
    p.length = project->max_length(); ui.steps_bar = uint8_t(p.steps() / bar_steps); light(plays);
    CHECK(is(0, Hue::None, 0) && is(12, Hue::None, 0)); // past 128 bars: nothing
    ui.steps_bar = 0;
    p.length = 16 * sixteenth_ticks;
    CHECK(steps_lane(kit) == 37 && steps_lane(project->tracks[0]) == -1);
    CHECK(step_velocity(p, 0, 37) == 127 && step_velocity(p, 2, 37) == 0 && step_velocity(p, 2, -1) == 100);
    CHECK(add_lane_note(p, 2, 37, 90) && step_velocity(p, 2, 37) == 90 && step_velocity(p, 2, 36) == 100); // beside the other pad's
    CHECK(clear_lane(p, 2, 37) && step_velocity(p, 2, 37) == 0 && step_velocity(p, 2, 36) == 100);        // only its lane goes
    CHECK(!clear_lane(p, 5, 37));
    CHECK(move_lane(p, 0, 37, 38) && step_velocity(p, 0, 38) == 127 && step_velocity(p, 0, 37) == 0 && move_lane(p, 0, 38, 37));
    // Step Edit: the chosen event white, a pad with nothing on the step Faint.
    ui.page = PageId::StepEdit; ui.step_pads = false; ui.step_cursor = 0; light({});
    CHECK(is(1, Hue::White, 4) && is(0, Hue::Track, 1) && is(12, Hue::None, 0));
    // Roll: the note in focus white, notes elsewhere Dim, none Faint.
    ui.page = PageId::Roll; ui.roll_key = 37; light({});
    CHECK(is(1, Hue::White, 4) && is(0, Hue::Track, 2) && is(5, Hue::Track, 1));
    // Time Correct: the pads play the track, lit as its pads are anywhere (the groove is on the encoder).
    ui.page = PageId::TimeCorrect; light(plays); auto played = looks;
    ui.page = PageId::Stage; light(plays);
    for (unsigned k = 0; k < 16; ++k) CHECK(played[k].hue == looks[k].hue && rung(played[k].level) == rung(looks[k].level));
    {   // B2 GROOVE held: the patterns of the bank shown, the groove's Full, the one in hand White, with notes Dim.
        auto& patterns = bank->at[1];
        const unsigned here = project->tracks[1].pattern % pattern_count;
        Event e; e.tick = 0; e.length = 120; e.key = 36;
        patterns[18].events.push_back(e); patterns[20].events.push_back(e); // B3, B5
        patterns[here].feel.groove_from = groove_source(1, 18);
        ui.page = PageId::TimeCorrect; ui.tc_groove_held = true; ui.tc_groove_track = 1; ui.tc_groove_bank = 1; light(plays);
        CHECK(is(2, Hue::Track, 4) && is(4, Hue::Track, 2) && is(0, Hue::None, 0));
        ui.tc_groove_bank = uint8_t(here / pattern_pads); light(plays);
        CHECK(is(here % pattern_pads, Hue::White, 3));
        ui.tc_groove_held = false; light(plays); // let go: the track's pads again
        for (unsigned k = 0; k < 16; ++k) CHECK(played[k].hue == looks[k].hue && rung(played[k].level) == rung(looks[k].level));
        patterns[here].feel.groove_from = 0; patterns[18].events.clear(); patterns[20].events.clear();
    }

    // Chop: the slices, the one in hand Full; Manual armed, pad 1 flashes red.
    ui.page = PageId::Chop; ui.slices.count = 5; ui.chop_slice = 2; light({});
    CHECK(is(2, Hue::Track, 4) && is(0, Hue::Track, 2) && is(6, Hue::None, 0));
    // Past sixteen slices the pads hold the bank of the slice in hand (Pad Bank steps it): slice 18 on pad 2.
    ui.slices.count = 20; ui.chop_slice = 17; light({});
    CHECK(is(1, Hue::Track, 4) && is(0, Hue::Track, 2) && is(3, Hue::Track, 2) && is(4, Hue::None, 0));
    CHECK(slice_bank(17) == 1 && slice_banks(20) == 2 && slice_banks(16) == 1 && slice_banks(0) == 1);
    CHECK(pad_slice(17, 3) == 19 && pad_slice(2, 3) == 3 && pad_slice(63, 15) == 63);
    ui.slices.count = 5; ui.chop_slice = 2;
    ui.chop_mode = ChopMode::Manual; ui.slices.count = 1; light({}, 1000); // no cut yet: armed
    CHECK(chop_armed(ui) && is(0, Hue::Red, 4) && is(1, Hue::None, 0));
    ui.slices.count = 5; light({}, 1000); // cuts there (K3 onto Manual, a restore, Split): they show, no red
    CHECK(!chop_armed(ui) && is(2, Hue::Track, 4) && looks[0].hue == Hue::Track);
    ui.slices.count = 1; light({}, 1000);
    CHECK(is(0, Hue::Red, 4) && is(1, Hue::None, 0));
    // The red walks (model/slice_banks.h): while the take plays, the slices so far light (the newest Full) and the
    // next slice's pad blinks red; past sixteen the pads show bank B and its pad 1 is the red one; at 128, no red.
    CHECK(view_slice(2, 5, true, max_slices) == 5 && view_slice(2, 5, false, max_slices) == 2 && view_slice(127, 128, true, max_slices) == 127
          && view_slice(64, 64, true, max_slices) == 64 && slice_bank(127) == 7);
    CHECK(red_slice(5, 5, max_slices) && !red_slice(4, 5, max_slices) && red_slice(64, 64, max_slices) && !red_slice(128, 128, max_slices));
    CHECK(pad_slice(view_slice(15, 16, true, max_slices), 0) == 16 && red_slice(16, 16, max_slices) && slice_bank(16) == 1);
    CHECK(take_frame(1000, 500, 1500) == 45100 && take_live(500, 1000, 50000, 1500) && !take_live(500, 1000, 40000, 1500)
          && !take_live(0, 0, 50000, 1500) && !take_live(2000, 0, 50000, 1500));
    ui.chop_sample = 200; project->samples.edit(200).frames = 441000;
    ui.slices = {}; ui.slices.whole(0, 441000);
    for (uint32_t at : {10000u, 20000u, 30000u, 40000u}) ui.slices.add(at, 441000, max_slices);
    ui.chop_slice = 4; ui.chop_take_at = 500; light({}, 1000); // half a second in: 22050 frames, inside slice 3
    CHECK(looks[5].hue == Hue::Red && is(4, Hue::Track, 4) && is(0, Hue::Track, 2) && is(6, Hue::None, 0));
    for (uint32_t at = 50000; ui.slices.count < 16; at += 10000) ui.slices.add(at, 441000, max_slices);
    ui.chop_slice = 15; light({}, 1000); // sixteen slices: bank B, its pad 1 red, nothing past it
    CHECK(looks[0].hue == Hue::Red && is(1, Hue::None, 0));
    light({}, 20000); // played out (19.5 s past a 10 s region): the pads play slices again, bank A's sixteen
    CHECK(looks[0].hue == Hue::Track && is(15, Hue::Track, 4));
    ui.chop_take_at = 0;
    // After a take the red stays on the next slice's pad (Context::end_take): with the newest slice in hand the pads show
    // its bank (sixteen slices: bank B, pad 1 red); another slice in hand, its own bank, the red where it falls in it.
    ui.chop_more = true; light({}, 20000);
    CHECK(looks[0].hue == Hue::Red && is(1, Hue::None, 0) && chop_red_shown(*project, ui, 20000));
    ui.chop_slice = 3; light({}, 20000);
    CHECK(is(3, Hue::Track, 4) && is(15, Hue::Track, 2) && looks[0].hue == Hue::Track);
    ui.slices.count = 6; ui.chop_slice = 2; light({}, 20000); // six: the red on pad 7, in the bank in view
    CHECK(looks[6].hue == Hue::Red && is(2, Hue::Track, 4) && is(7, Hue::None, 0));
    ui.chop_mode = ChopMode::Threshold; light({}, 20000); // not Manual: no red
    CHECK(looks[6].hue == Hue::None && !chop_red_shown(*project, ui, 20000));
    ui.chop_mode = ChopMode::Manual; ui.slices.count = 16;
    ui.chop_take_from = 30000; ui.chop_take_at = 500; ui.chop_slice = 3; // a lit pad redid it from slice 4: heard from there
    CHECK(chop_take_playing(*project, ui, 1000) && !chop_take_playing(*project, ui, 10000));
    ui.chop_take_at = 0; ui.chop_take_from = 0; ui.chop_more = false;
    // A slice tapped plays through its kit pad (Context::slice_hit): that voice lights its slice's pad as the audition
    // voice did; the kit pad playing its whole region lights none.
    {
        ui.chop_mode = ChopMode::Threshold; ui.chop_slice = 3;
        kit.pads[1].sample = 200;
        taps.track = 1; taps.count = 1; taps.voice[0] = VoiceTap {};
        taps.voice[0].pad = 1; taps.voice[0].slot = 200; taps.voice[0].from = 50000; taps.voice[0].to = 60000;
        taps.voice[0].at = 55000; taps.voice[0].step = -1; taps.voice[0].level = 1; // slice 6, reversed
        light({}, 20000);
        CHECK(is(5, Hue::Track, 4) && is(3, Hue::Track, 4) && is(4, Hue::Track, 2));
        taps.voice[0].from = 0; taps.voice[0].to = 441000; light({}, 20000);
        CHECK(is(5, Hue::Track, 2));
        taps.voice[0].pad = 0; taps.voice[0].from = 50000; taps.voice[0].to = 60000; light({}, 20000); // another pad's
        CHECK(is(5, Hue::Track, 2));
        // Shift held: Loop lit while the slice in hand plays backwards (its own Reverse against the kit pad's).
        CHECK(!slice_heard_reversed(kit, 200, ui.slices, 3));
        ui.slices.reverse[3] = true;
        CHECK(slice_heard_reversed(kit, 200, ui.slices, 3) && !slice_heard_reversed(kit, 200, ui.slices, 4));
        kit.pads[1].reverse = true;
        CHECK(!slice_heard_reversed(kit, 200, ui.slices, 3) && slice_heard_reversed(kit, 200, ui.slices, 4));
        kit.pads[1].reverse = false; ui.slices.reverse[3] = false;
        taps.count = 0; kit.pads[1].sample = 1; ui.chop_mode = ChopMode::Manual;
    }
    // SHIFT + the wheel: a cut onto the next hit the way it turns, as strong as Threshold's amount, 10 ms clear of it.
    {
        std::array<Hit, max_hits> hits {};
        hits[0] = {1000, 200}; hits[1] = {5000, 40}; hits[2] = {9000, 180}; hits[3] = {9300, 255}; hits[4] = {20000, 150};
        const uint8_t floor = hit_floor(50);
        CHECK(floor == 127 && hit_floor(0) == 0 && hit_floor(100) == 255);
        CHECK(next_hit(1000, 1, hits, 5, floor) == 9000);      // past the faint one at 5000
        CHECK(next_hit(9000, 1, hits, 5, floor) == 20000);     // 9300 is within 10 ms of 9000
        CHECK(next_hit(8800, 1, hits, 5, floor) == 9300);      // a cut just before a hit (zero snap) moves past it
        CHECK(next_hit(20000, -1, hits, 5, floor) == 9300 && next_hit(1000, -1, hits, 5, floor) == 1000);
        CHECK(next_hit(1000, 1, hits, 5, 0) == 5000 && next_hit(30000, 1, hits, 5, floor) == 30000);
        // Inside its room only: the hit that is the next slice's own cut (or within 20 ms of it) is not a place to land.
        CHECK(next_hit_within(1000, 1, hits, 5, floor, 0, 9000) == 1000 && next_hit_within(1000, 1, hits, 5, floor, 0, 9100) == 9000);
        CHECK(next_hit_within(20000, -1, hits, 5, floor, 9300, 30000) == 20000 && mark_floor(50) == hit_floor(25) && mark_floor(10) == hit_floor(10));
    }
    ui.chop_mode = ChopMode::Threshold; ui.slices.count = 0;
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
    ui.track = 0; light({}); // a plug-in track: Resample opens AUTO SAMPLE; Trim Sample and Warp kits only
    CHECK(is(10, Hue::White, 2) && is(12, Hue::None, 0) && is(14, Hue::None, 0));
    ui.track = 1; ui.shift = false;
    // Erase held on a kit, stopped: the pads that would empty. A pad copy: from, then where.
    ui.erase = true; light(plays);
    CHECK(is(0, Hue::Red, 1) && is(12, Hue::None, 0));
    ui.erase = false; ui.copying = UiState::Copying::Pads; light(plays);
    CHECK(is(0, Hue::Track, 2) && is(12, Hue::None, 0));
    ui.copy_from = 4; ui.copy_track = 1; light(plays);
    CHECK(is(4, Hue::White, 4) && is(0, Hue::Red, 1) && is(12, Hue::Track, 1));
    ui.copy_track = 3; light(plays); // picked on another kit (issue 48): every pad here is a place it can go
    CHECK(is(4, Hue::Red, 1) && is(0, Hue::Red, 1) && is(12, Hue::Track, 1));
    ui.copying = UiState::Copying::Off; ui.copy_from = -1; ui.copy_track = 0;
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
#include "melodee/shell/erase_copy.h"
#include "melodee/shell/note_copy.h"
#include "melodee/shell/step_edit.h"

// Erase and Copy on whatever the page holds (shell/erase_copy.h): what Shift + Erase copies on each page, where a tap
// of Erase has a job, a key's notes and a step copied inside a pattern, and the pads and buttons lit for it.
void erase_copy_everywhere() {
    using namespace pad_light;
    // A key's notes onto another key: the same times and values, the target's own notes gone first.
    Pattern p;
    p.length = 4 * bar_steps * sixteenth_ticks;
    Event a; a.tick = 0; a.key = 60; a.velocity = 90; a.chance = 50;
    Event b = a; b.tick = 4 * sixteenth_ticks; b.velocity = 70;
    Event c = a; c.tick = 2 * sixteenth_ticks; c.key = 64;
    CHECK(p.add(a) && p.add(b) && p.add(c));
    CHECK(!copy_key_events(p, 62, 67) && !copy_key_events(p, 60, 60)); // nothing on 62; onto itself
    CHECK(copy_key_events(p, 60, 64));
    unsigned on64 = 0;
    for (const auto& e : p.events) if (e.key == 64) { ++on64; CHECK(e.chance == 50 && (e.tick == 0 || e.tick == 4 * sixteenth_ticks)); }
    CHECK(on64 == 2 && p.events.size() == 4); // 64's own note (tick 2) replaced
    // A step onto another: each note as far into it; on a lane only that key's notes move and the others stay.
    Pattern s;
    s.length = bar_steps * sixteenth_ticks;
    Event x; x.tick = 1 * s.q() + 10; x.key = 36; x.velocity = 100;
    Event y = x; y.key = 38;
    Event z; z.tick = 5 * s.q(); z.key = 36;
    CHECK(s.add(x) && s.add(y) && s.add(z));
    CHECK(step_has_notes(s, 1, -1) && !step_has_notes(s, 2, -1) && step_has_notes(s, 5, 36) && !step_has_notes(s, 5, 38));
    CHECK(!copy_step_notes(s, 2, 3, -1) && !copy_step_notes(s, 1, 1, -1));
    CHECK(copy_step_notes(s, 1, 5, 38)); // the kick on step 6 stays, the snare joins it
    CHECK(step_has_notes(s, 5, 36) && step_has_notes(s, 5, 38) && s.events.size() == 4);
    CHECK(copy_step_notes(s, 1, 5, -1)); // the whole step: step 6 as step 2, 10 ticks in
    unsigned in6 = 0;
    for (const auto& e : s.events) if (e.tick >= 5 * s.q() && e.tick < 6 * s.q()) { ++in6; CHECK(e.tick == 5 * s.q() + 10); }
    CHECK(in6 == 2 && s.events.size() == 4);
    CHECK(!copy_step_notes(s, 1, 99, -1)); // past the end

    // What Shift + Erase copies, page by page.
    auto project = std::make_unique<Project>();
    auto& synth = project->tracks[0];
    synth.engine = EngineKind::Plugin;
    auto& kit = project->tracks[1];
    kit.engine = EngineKind::Sampler;
    kit.pads[0].sample = 1;
    UiState ui;
    using C = CopyHere;
    ui.page = PageId::Stage; ui.track = 0;
    CHECK(copy_here(*project, ui, false, true) == C::Keys && copy_here(*project, ui, false, false) == C::None);
    ui.track = 1;
    CHECK(copy_here(*project, ui, true, false) == C::Pads);
    ui.pad_bank = true; CHECK(copy_here(*project, ui, true, false) == C::Tracks); ui.pad_bank = false;
    ui.seq_held = true; CHECK(copy_here(*project, ui, true, false) == C::Patterns); ui.seq_held = false;
    ui.page = PageId::Mixer; CHECK(copy_here(*project, ui, false, false) == C::None); // SENDS: nothing (Pad Bank held: the tracks)
    ui.page = PageId::Patterns; CHECK(copy_here(*project, ui, true, false) == C::Patterns); // the page's patterns before the kit's pads
    ui.page = PageId::Scenes; CHECK(copy_here(*project, ui, true, false) == C::Scenes); // Sequences: a sequence
    ui.scene_capture = true; CHECK(copy_here(*project, ui, true, false) == C::None); ui.scene_capture = false; // CAPTURE waits for its pad
    ui.page = PageId::StepEdit; ui.step_pads = true; CHECK(copy_here(*project, ui, false, false) == C::Steps);
    ui.page = PageId::Roll; CHECK(copy_here(*project, ui, false, false) == C::Keys);
    ui.page = PageId::StepEdit; ui.step_pads = false; CHECK(copy_here(*project, ui, false, false) == C::Keys);
    ui.track = 5; // an empty track: no notes to copy
    ui.page = PageId::Roll; CHECK(copy_here(*project, ui, false, false) == C::None);
    ui.page = PageId::StepEdit; CHECK(copy_here(*project, ui, false, false) == C::None);
    ui.track = 0;
    ui.page = PageId::Song; ui.song_cursor = 0;
    CHECK(copy_here(*project, ui, false, false) == C::None); // no clip under the cursor
    Clip clip; clip.track = 0; clip.pattern = 0; clip.bar = 0; clip.bars = 2;
    CHECK(project->song.place(clip));
    CHECK(copy_here(*project, ui, false, false) == C::Clip);
    ui.song_export = true; CHECK(copy_here(*project, ui, false, false) == C::None); ui.song_export = false;
    ui.page = PageId::Rack; // the track's chain: an effect in focus, else nothing
    unsigned chain = page_chain(*project, ui);
    ui.fx_cursor[UiState::cursor_of(chain)] = 0;
    CHECK(copy_here(*project, ui, false, false) == C::None);
    project->chain(chain).slots[0].kind = 1;
    CHECK(copy_here(*project, ui, false, true) == C::Effect); // the effect before the synth's keys
    ui.picker_open = true; CHECK(copy_here(*project, ui, false, false) == C::None); ui.picker_open = false;
    ui.page = PageId::Project; ui.project_list = false; CHECK(copy_here(*project, ui, false, false) == C::None);
    ui.project_list = true; CHECK(copy_here(*project, ui, false, false) == C::Project);
    ui.page = PageId::Chop; ui.track = 1; ui.slices.count = 3; CHECK(copy_here(*project, ui, true, false) == C::Slice);
    ui.page = PageId::SettingsPage; ui.track = 0; CHECK(copy_here(*project, ui, false, false) == C::None);

    // Where a tap of Erase has a job.
    for (PageId page : {PageId::Roll, PageId::StepEdit, PageId::Patterns,
                        PageId::Looper, PageId::Rack, PageId::Bus, PageId::Master, PageId::PadFx, PageId::Chop,
                        PageId::LayersPage, PageId::ModPage}) {
        ui.page = page; CHECK(erase_tap_page(ui));
    }
    ui.page = PageId::Project; ui.project_list = false; CHECK(!erase_tap_page(ui));
    ui.project_list = true; CHECK(erase_tap_page(ui));
    ui.page = PageId::Stage; CHECK(!erase_tap_page(ui));
    ui.plugin_panel.screen = true; CHECK(!erase_tap_page(ui)); // a plug-in's own page: only when it takes Erase
    ui.plugin_panel.screen_keys = screen_key_mute; CHECK(!erase_tap_page(ui));
    ui.plugin_panel.screen_keys = screen_key_mute | screen_key_erase; CHECK(erase_tap_page(ui));
    ui.plugin_panel = PluginPanel {};

    // Erase lit wherever it has a job, Shift + Erase wherever Copy does.
    using B = SurfaceButton;
    ui.page = PageId::Stage; ui.track = 0;
    CHECK(!button_has_job(*project, ui, B::Erase, false, false) && button_has_job(*project, ui, B::Erase, false, false, true));
    CHECK(!button_has_shift_job(*project, ui, B::Erase, false) && button_has_shift_job(*project, ui, B::Erase, false, true));
    ui.page = PageId::Rack; CHECK(button_has_job(*project, ui, B::Erase, false, false) && button_has_shift_job(*project, ui, B::Erase, false));
    ui.page = PageId::Song; CHECK(button_has_shift_job(*project, ui, B::Erase, false));

    // The pads: a key copy and a key erase where the pads play a synth's keys, a step copy on Steps.
    auto bank = std::make_unique<PatternBank>();
    auto host_held = std::make_unique<EngineHost>(); // on the heap: a test's frame stays under the 8 MB stack
    auto& host = *host_held;
    TrackTaps taps;
    std::array<Look, 16> looks;
    PadFacts keys; keys.key_pads = true;
    auto light = [&](PadFacts facts) { PadScene scene {ui, *project, *bank, host, taps, facts, 1000, nullptr, 0}; pad_looks(scene, looks); };
    auto is = [&](unsigned pad, Hue hue, unsigned r) { return looks[pad].hue == hue && rung(looks[pad].level) == r; };
    int k0 = pad_key(*project, synth, 0), k2 = pad_key(*project, synth, 2);
    Event note; note.key = uint8_t(k0); bank->at[0][synth.pattern].add(note);
    ui.page = PageId::Stage; ui.track = 0;
    ui.erase = true;
    CHECK(pad_context(ui, synth, keys, 0) == PadContext::KeyErase && pad_context(ui, synth, PadFacts {}, 0) == PadContext::Page);
    light(keys); CHECK(is(0, Hue::Red, 2) && is(2, Hue::None, 0)); // the key with notes red, the rest dark
    ui.erase = false;
    ui.copying = UiState::Copying::Keys;
    CHECK(pad_context(ui, synth, keys, 0) == PadContext::KeyCopy);
    light(keys); CHECK(is(0, Hue::Track, 2) && is(2, Hue::None, 0)); // the keys that can be copied
    ui.copy_from = int16_t(k0);
    light(keys); CHECK(is(0, Hue::White, 4) && is(2, Hue::Track, 1)); // the source white, where it can land Faint
    {   // Time Correct's B2 GROOVE (shell/time_correct.h): the bank it shows, what a pad does, Minus / Plus's track.
        CHECK(groove_view(0, 3, 37).track == 3 && groove_view(0, 3, 37).bank == 2); // no groove: the bank of the one in hand
        CHECK(groove_view(groove_source(5, 70), 3, 0).track == 5 && groove_view(groove_source(5, 70), 3, 0).bank == 4);
        CHECK(groove_pick(0, 3, 0, 3, 2, true) == GroovePick::Take && groove_pick(groove_source(3, 2), 3, 0, 3, 2, true) == GroovePick::Clear);
        CHECK(groove_pick(0, 3, 0, 3, 0, true) == GroovePick::Here && groove_pick(0, 3, 0, 3, 5, false) == GroovePick::Empty);
        CHECK(groove_pick(0, 3, 0, 4, 0, true) == GroovePick::Take); // another track's first pattern is not the one in hand
        std::array<bool, track_count> notes {};
        notes[1] = notes[6] = true;
        CHECK(groove_track_step(notes, 3, 3, 1) == 6 && groove_track_step(notes, 3, 6, -1) == 3 && groove_track_step(notes, 3, 3, -1) == 1);
        CHECK(groove_track_step(notes, 3, 6, 1) == 6 && groove_track_step(notes, 3, 1, -1) == 1); // it stops at the ends
    }
    CHECK(k2 != k0);
    ui.page = PageId::Roll; CHECK(pad_context(ui, synth, PadFacts {}, 0) == PadContext::KeyCopy);
    ui.page = PageId::StepEdit; ui.step_pads = true; CHECK(pad_context(ui, synth, PadFacts {}, 0) == PadContext::Page); // keys are not steps
    ui.copying = UiState::Copying::Steps; ui.copy_from = -1; ui.steps_bar = 0;
    CHECK(pad_context(ui, synth, PadFacts {}, 0) == PadContext::StepCopy);
    light({}); CHECK(is(0, Hue::Track, 2) && is(1, Hue::None, 0)); // step 1 holds the note
    ui.copy_from = 0;
    light({}); CHECK(is(0, Hue::White, 4) && is(1, Hue::Track, 1));
    ui.page = PageId::Roll; CHECK(pad_context(ui, synth, PadFacts {}, 0) == PadContext::Page); // steps only on Step Edit's steps
    // Patterns' pads are the track's patterns without Seq held; Shift + pad keeps the printed jobs.
    {
        UiState q; q.page = PageId::Patterns;
        Track kit; kit.engine = EngineKind::Sampler;
        CHECK(pad_context(q, kit, PadFacts {}, 0) == PadContext::SeqLayer);
        q.shift = true; CHECK(pad_context(q, kit, PadFacts {}, 0) == PadContext::Shift);
        q.shift = false; q.page = PageId::Roll; CHECK(pad_context(q, kit, PadFacts {}, 0) != PadContext::SeqLayer);
    }
    ui.stop_copying();
}

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
    // What names a track by its number inside the patterns and the modulators: a punch kept on track 3 in its own
    // pattern and in track 10's, one on the master; an LFO on track 3's lane, a follower hearing track 7 that
    // moves track 10's lane, a macro on both tracks.
    bank->at[2][0].punches = {{0, 48, 4, 2, 100, 2, 1}, {96, 48, 4, 255, 100, 2, 1}};
    bank->at[9][3].punches = {{0, 48, 4, 2, 100, 2, 1}, {96, 48, 4, 6, 100, 2, 1}, {192, 48, 4, 9, 100, 2, 1}};
    p.modulators[0].kind = ModKind::Lfo; p.modulators[0].source = 15;
    p.modulators[0].targets[0].track = 2; p.modulators[0].targets[0].param = 7;
    p.modulators[3].kind = ModKind::Follower; p.modulators[3].source = 6;
    p.modulators[3].targets[1].track = 9; p.modulators[3].targets[1].param = 4;
    p.modulators[5].kind = ModKind::Lfo; p.modulators[5].source = 15; p.modulators[5].targets[0].track = 9;
    p.macros[1].targets[0].track = 2; p.macros[1].targets[1].track = 6; p.macros[1].targets[2].track = 9;

    auto changed = swap_tracks(p, *bank, 2, 6);
    // (They stayed on the numbers: the punch, the LFO and the macro went on moving whatever came to slot 3.)
    CHECK(bank->at[6][0].punches[0].target == 6 && bank->at[6][0].punches[1].target == 255);
    CHECK(bank->at[9][3].punches[0].target == 6 && bank->at[9][3].punches[1].target == 2 && bank->at[9][3].punches[2].target == 9);
    CHECK(changed.kept.size() == 1 && changed.kept[0].first == 9 && changed.kept[0].second == 3); // the two tracks' own go whole
    CHECK(p.modulators[0].targets[0].track == 6 && p.modulators[0].targets[0].param == 7 && !p.modulators[0].targets[1].used());
    CHECK(p.modulators[3].source == 2 && p.modulators[3].targets[1].track == 9 && p.modulators[5].targets[0].track == 9);
    CHECK(changed.mods == (1u << 0 | 1u << 3));
    CHECK(p.macros[1].targets[0].track == 6 && p.macros[1].targets[1].track == 2 && p.macros[1].targets[2].track == 9 && !p.macros[1].targets[3].used());
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
    CHECK(bank->at[2][0].punches[0].target == 2 && bank->at[9][3].punches[1].target == 6 && p.modulators[0].targets[0].track == 2);
    CHECK(p.modulators[3].source == 6 && p.macros[1].targets[0].track == 2 && p.macros[1].targets[1].track == 6);
    CHECK(p.tracks[2].engine == EngineKind::Plugin && p.tracks[2].mix.level == 90 && p.tracks[6].pads[0].sample == 4);
    CHECK(!bank->at[2][0].events.empty() && p.song.clip_at(2, 0) >= 0 && p.song.clip_at(6, 4) >= 0 && p.song.punches[0].target == 6);
    CHECK(p.looper.loops[0].source == looper::source_track(2) && p.knob_fx.track_on(2) && p.knob_fx.pad_on(6, 0) && p.compressor.key == 6);
    // To an empty slot: a plain move, the slot it left empty; nothing that names neither changes.
    changed = swap_tracks(p, *bank, 2, 11);
    CHECK(p.tracks[11].engine == EngineKind::Plugin && !p.tracks[2].used() && p.song.clip_at(11, 0) >= 0 && p.song.clip_at(6, 4) >= 0);
    CHECK(changed.song && !changed.punches && changed.loops == 1 && changed.knob && !changed.key);
    changed = swap_tracks(p, *bank, 4, 4);
    CHECK(!changed.song && !changed.punches && !changed.loops && !changed.knob && !changed.key && !changed.mods && changed.kept.empty());

    // The screen's memory of each track goes with it, and the track in hand stays the one it was.
    UiState ui;
    ui.edit_page[2] = 3; ui.stage_group[6] = 1; ui.seq_editor[2] = 2; ui.track = 2;
    swap_track_views(ui, 2, 6);
    CHECK(ui.edit_page[6] == 3 && ui.edit_page[2] == 0 && ui.stage_group[2] == 1 && ui.seq_editor[6] == 2 && ui.track == 6);
    swap_track_views(ui, 0, 1);
    CHECK(ui.track == 6);

    // Only a tap of Pad Bank steps the bank: held past pad_bank_hold_ms, or used while down, it does not.
    {
        UiState tap; tap.pad_bank = true; tap.pad_bank_down = 1000;
        CHECK(pad_bank_tap(tap, 1000 + pad_bank_hold_ms - 1) && !pad_bank_tap(tap, 1000 + pad_bank_hold_ms));
        tap.pad_bank_used = true;
        CHECK(!pad_bank_tap(tap, 1001));
    }
    // B1-B3 with Pad Bank held: B2 MOVE, B3 COPY; B1 steps back, then out; the active key again ends it.
    UiState held; held.pad_bank = true;
    CHECK(track_key(held, 0) == TrackKey::Tracks && held.copying == UiState::Copying::Off && held.pad_bank_used); // B1 TRACKS
    CHECK(track_key(held, 1) == TrackKey::Started && held.copying == UiState::Copying::Tracks && held.moving && held.copy_from < 0);
    held.copy_from = 3;
    CHECK(track_key(held, 2) == TrackKey::Switched && !held.moving && held.copy_from == 3);
    CHECK(track_key(held, 0) == TrackKey::Back && held.copy_from < 0 && held.copying == UiState::Copying::Tracks);
    CHECK(track_key(held, 2) == TrackKey::Ended && held.copying == UiState::Copying::Off);
    held.pad_bank = false;
    CHECK(track_key(held, 1) == TrackKey::Ignored); // the page's keys answer
    start_track_pick(held, true); // Shift + B2 on Tracks
    CHECK(tracks_on_pads(held) && track_overlay_shown(held) && held.moving);
    CHECK(track_key(held, 0) == TrackKey::Ended && !held.moving && !tracks_on_pads(held));
    held.copying = UiState::Copying::Pads; held.pad_bank = true;
    CHECK(track_key(held, 2) == TrackKey::Started && held.copying == UiState::Copying::Tracks && !held.moving); // a pad copy gives way
    // The MIX hub's row (shell/mix_hub.h): MIXER, none (the page stays), and FX along the chains, a kit's pads' last.
    CHECK(mix_next(PageId::Stage, 0, false) == PageId::Mixer && mix_next(PageId::Rack, 1, false) == PageId::Rack);
    CHECK(mix_section(PageId::Mixer) == MixSection::Tracks && mix_next(PageId::Mixer, 2, false) == PageId::Rack);
    {   // The mixer's view: the strips as it opens; Plus held the card, Minus held the strips, a tap nudges as it is let go.
        UiState m; m.page = PageId::Mixer;
        CHECK(!m.mixer_card);
        MixerViewHold h;
        mixer_view_press(h, 1, 1000);
        mixer_view_tick(h, m, 1000 + mixer_view_hold_ms - 1);
        CHECK(!m.mixer_card && mixer_view_release(h) == 1); // a tap: + moves through the lit block, the view stays
        mixer_view_press(h, 1, 2000);
        mixer_view_tick(h, m, 2000 + mixer_view_hold_ms);
        CHECK(m.mixer_card && mixer_view_release(h) == 0); // held: the card, the release does nothing more
        const uint32_t from = 2000 + mixer_view_hold_ms; // on the way there over mixer_view_ms, eased out
        CHECK(mixer_card_shown(m, from) == 0 && mixer_card_shown(m, from + mixer_view_ms / 2) > 0.5f && mixer_card_shown(m, from + mixer_view_ms) == 1);
        mixer_view_press(h, -1, 3000);
        mixer_view_tick(h, m, 3000 + mixer_view_hold_ms);
        CHECK(!m.mixer_card && mixer_view_release(h) == 0);
        mixer_view_press(h, 1, 4000);
        m.page = PageId::Stage; // another page opened while it is down: let go, the view stays
        mixer_view_tick(h, m, 4000 + mixer_view_hold_ms);
        CHECK(!m.mixer_card && !h.down && mixer_view_release(h) == 0);
    }
    {   // The mixer's second row (shell/mixer_channels.h): submixes on pads 1-8, buses 9-12, the input 15, the master 16.
        UiState m; m.settings.full_features = true; m.page = PageId::Mixer;
        CHECK(pad_other(m, 0) == int(strip_sub(0)) && pad_other(m, 7) == int(strip_sub(7)) && pad_other(m, 8) == int(bus_chain(0)));
        CHECK(pad_other(m, 12) < 0 && pad_other(m, 13) < 0 && pad_other(m, 14) == int(strip_input) && pad_other(m, 15) == int(master_chain));
        for (unsigned pad = 0; pad < 16; ++pad) if (const int n = pad_other(m, pad); n >= 0) CHECK(other_pad(unsigned(n)) == int(pad));
        CHECK(mixer_strip(m) == m.track % track_count && mixer_row_count(m) == track_count);
        m.mixer_bank = 1;
        CHECK(mixer_strip(m) == strip_sub(0) && mixer_row_count(m) == 14 && mixer_row_at(m) == 0);
        pick_other(m, master_chain);
        CHECK(mixer_strip(m) == master_chain && mixer_row_at(m) == 13 && mixer_row_strip(m, 12) == strip_input);
        // What each channel's keys reach: B1 SENDS, again FX (a bus or the master FX alone, the input INPUT); B3 COMP
        // everywhere (the master's the printed Compressor).
        m.mixer_block = 0;
        CHECK(mixer_block_of(m, ChannelKind::Bus) == block_fx && mixer_block_of(m, ChannelKind::Submix) == block_sends);
        CHECK(mixer_block_of(m, ChannelKind::Track) == block_sends && mixer_block_of(m, ChannelKind::Input) == block_input);
        m.mixer_b1 = 1;
        CHECK(mixer_block_of(m, ChannelKind::Track) == block_fx && mixer_block_of(m, ChannelKind::Submix) == block_fx);
        m.mixer_b1 = 0;
        m.mixer_block = 2;
        CHECK(mixer_block_of(m, ChannelKind::Master) == block_comp && mixer_block_of(m, ChannelKind::Input) == block_comp);
        CHECK(channel_chain(strip_sub(3)) == int(sub_chain(3)) && channel_chain(bus_chain(2)) == int(bus_chain(2))
              && channel_chain(master_chain) == int(master_chain) && channel_chain(5) == 5 && channel_chain(strip_input) < 0);
        CHECK(mute_on_others(m) && !mute_on_kit(m, true)); // Mute + pad: the channels, never the kit's pads
        // Basic (decision 3): no submixes; the second row starts at the buses.
        UiState b = m; b.settings.full_features = false; b.mixer_other = 0;
        CHECK(pad_other(b, 0) < 0 && pad_other(b, 8) == int(bus_chain(0)) && mixer_strip(b) == bus_chain(0) && mixer_row_count(b) == 6);
        // Cards and fader: a submix's sends, pan and level; a bus's return; the input's gain; the master's trim.
        auto project = std::make_unique<Project>();
        project->subs[2].mix.sends[1] = 40; project->subs[2].mix.level = 90; project->buses[1].level = 55;
        m.mixer_block = 0; pick_other(m, strip_sub(2));
        auto cards = mixer_cards(*project, m, false);
        CHECK(cards[1].kind == Binding::Kind::SubMix && cards[1].id == int(2 << 8 | (Command::SubSend0 + 1)) && cards[1].value == 40);
        auto second = mixer_cards(*project, m, true);
        CHECK(second[0].id == int(2 << 8 | Command::SubPan) && !second[1].bound()); // a submix plays to the master: no OUTPUT
        auto fj = fader_job(*project, m);
        CHECK(fj.binding.kind == Binding::Kind::SubMix && fj.binding.value == 90);
        pick_other(m, bus_chain(1));
        CHECK(fader_job(*project, m).binding.kind == Binding::Kind::BusReturn && fader_job(*project, m).binding.value == 55);
        CHECK(!mixer_cards(*project, m, false)[0].bound()); // B1: its FX, an empty chain (the plus)
        m.mixer_block = 1;
        CHECK(mixer_cards(*project, m, false)[0].kind == Binding::Kind::Strip); // B2: its EQ
        m.mixer_block = 0;
        pick_other(m, strip_input);
        CHECK(fader_job(*project, m).binding.kind == Binding::Kind::RecGain);
        pick_other(m, master_chain);
        CHECK(fader_job(*project, m).binding.id == int(master_chain << 8 | unsigned(StripPart::Trim)));
        // FX on B1: the lit effect of the channel's chain on K1-K3 (its FX page's cursor), Shift its other row; − / +
        // left SENDS on its second row: K1 PAN, Shift the first row again.
        project->tracks[4].engine = EngineKind::Sampler;
        project->tracks[4].fx.slots[0].kind = uint8_t(EffectKind::Drive);
        project->tracks[4].fx.slots[1].kind = uint8_t(EffectKind::Chorus);
        UiState f = m; f.mixer_bank = 0; f.track = 4; f.mixer_block = 0; f.mixer_b1 = 1; f.page = PageId::Mixer;
        f.fx_cursor[UiState::cursor_of(4)] = 1;
        auto fxc = mixer_cards(*project, f, false);
        CHECK(fxc[0].kind == Binding::Kind::FxParam && fxc[0].id == int(4 * 64 + 1 * 8 + 0) && mixer_fx_lit(f));
        // On the FX block Erase takes the lit effect out and Shift + Erase copies it (its chain is the page's), and a
        // Mute tap switching it is one undo step of that chain.
        CHECK(page_chain(*project, f) == 4 && copy_here(*project, f, false, false) == CopyHere::Effect && erase_tap_page(f));
        const auto mute_scope = press_scope(*project, f, SurfaceButton::Mute, false);
        CHECK(mute_scope.kind == UndoScope::Kind::Chain && mute_scope.index == 4);
        UiState sends_lit = f; sends_lit.mixer_b1 = 0;
        CHECK(!erase_tap_page(sends_lit) && copy_here(*project, sends_lit, false, false) == CopyHere::None);
        f.fx_cursor[UiState::cursor_of(4)] = 2; // the plus: no cards
        CHECK(!mixer_cards(*project, f, false)[0].bound());
        f.mixer_b1 = 0; f.mixer_row = 1;
        CHECK(mixer_cards(*project, f, false)[0].kind == Binding::Kind::Pan && mixer_cards(*project, f, true)[0].kind == Binding::Kind::Send);
        // The input's B1 INPUT: trim, monitor, dry or wet takes; the master's B3 the printed Compressor, Shift In Boost.
        UiState in = m; pick_other(in, strip_input); in.mixer_block = 0;
        auto inc = mixer_cards(*project, in, false);
        CHECK(mixer_block_of(in, ChannelKind::Input) == block_input && inc[0].kind == Binding::Kind::Strip);
        CHECK(inc[1].kind == Binding::Kind::InputMonitor && inc[2].kind == Binding::Kind::InputWet && !mixer_cards(*project, in, true)[0].bound());
        UiState mc = m; pick_other(mc, master_chain); mc.mixer_block = 2; mc.mixer_row = 0;
        CHECK(mixer_master_comp(mc) && mixer_cards(*project, mc, false)[0].kind == Binding::Kind::CompAttack);
        CHECK(mixer_cards(*project, mc, true)[2].kind == Binding::Kind::CompBoost);
        CHECK(press_scope(*project, mc, SurfaceButton::WheelPush, false).kind == UndoScope::Kind::Master);
        // The EQ picture (shell/spectrum_view.h): a full-scale 1 kHz sine reads about 0 dB in its band, far below
        // elsewhere; with nothing new it falls at spectrum_fall_db_per_s.
        {
            auto ring = std::make_unique<ScopeRing>();
            std::vector<float> sine(ScopeRing::size);
            for (unsigned f = 0; f < sine.size(); ++f) sine[f] = std::sin(2 * 3.14159265f * 1000.0f * float(f) / 48000.0f);
            ring->put(sine.data(), sine.data(), unsigned(sine.size()));
            auto view = std::make_unique<SpectrumView>();
            view->update(*ring, 48000, 1000);
            unsigned at = 0;
            while (at + 1 < spectrum_bands && spectrum_band_hz(float(at + 1)) <= 1000) ++at;
            const float peak = view->bands()[at], low = view->bands()[2];
            CHECK(peak > -2.5f && peak < 0.5f && low < -50);
            view->update(*ring, 48000, 1500); // half a second, nothing new
            CHECK(std::fabs(view->bands()[at] - (peak - spectrum_fall_db_per_s * 0.5f)) < 0.01f);
        }
        // Undo keeps the submixes' level, pan and sends.
        auto before = mix_of(*project);
        project->subs[2].mix.pan = -20;
        CHECK(!(mix_of(*project) == before));
        before = mix_of(*project);
        project->input_wet = !project->input_wet;
        CHECK(!(mix_of(*project) == before)); // and the input's dry or wet
        // A submix's chain (25-32) on the effect page: Rack with fx_sub, a cursor of its own (the kit pads' share the
        // last), its Compressor's KEY row; keys on it and on the strips follow a track swap.
        UiState r; r.page = PageId::Rack; r.fx_sub = 2;
        CHECK(page_chain(*project, r) == sub_chain(2) && UiState::cursor_of(sub_chain(7)) == sub_chain(7));
        CHECK(UiState::cursor_of(pad_chain(0, 0)) == mix_chain_count);
        EffectSlot comp {}; comp.kind = uint8_t(EffectKind::Compressor);
        CHECK(effect_rows(comp, sub_chain(2)) == effect_rows(comp, 0) && effect_rows(comp, pad_chain(0, 0)) == effect_rows(comp, 0) - 1);
        auto swap_bank = std::make_unique<PatternBank>();
        project->buses[0].strip.comp_key = fx_key_track(3);
        project->subs[1].fx.slots[0] = comp; project->subs[1].fx.slots[0].key = fx_key_track(3);
        auto swapped = swap_tracks(*project, *swap_bank, 3, 5);
        CHECK(project->buses[0].strip.comp_key == fx_key_track(5) && project->subs[1].fx.slots[0].key == fx_key_track(5) && swapped.fx_keys);
    }
    CHECK(mix_next(PageId::Mixer, 2, false) == PageId::Rack && mix_next(PageId::Rack, 2, false) == PageId::Bus);
    CHECK(mix_next(PageId::Bus, 2, false) == PageId::Master && mix_next(PageId::Master, 2, false) == PageId::Rack);
    CHECK(mix_next(PageId::Master, 2, true) == PageId::PadFx && mix_next(PageId::PadFx, 2, true) == PageId::Rack);
    CHECK(mix_section(PageId::Bus) == MixSection::Fx && mix_section(PageId::Stage) == MixSection::None);
    {   // Pad FX + Knob FX together, either order: the MIX hub, neither's own job; alone, its job on release;
        // Shift + either at once (Flex Beat, FX Select).
        using SB = SurfaceButton;
        UiState f;
        CHECK(fx_chord(f, SB::PadFx, true) == FxChord::Wait && fx_chord(f, SB::PadFx, false) == FxChord::Own);
        CHECK(fx_chord(f, SB::KnobFx, true) == FxChord::Wait && fx_chord(f, SB::PadFx, true) == FxChord::Mix);
        CHECK(fx_chord(f, SB::KnobFx, false) == FxChord::Wait && fx_chord(f, SB::PadFx, false) == FxChord::Wait && !f.fx_chord_used);
        CHECK(fx_chord(f, SB::KnobFx, true) == FxChord::Wait && fx_chord(f, SB::KnobFx, false) == FxChord::Own); // its own again
        f.shift = true;
        CHECK(fx_chord(f, SB::KnobFx, true) == FxChord::Own && fx_chord(f, SB::KnobFx, false) == FxChord::Wait && !f.knob_fx_down);
    }
    held.moving = true; held.stop_copying();
    CHECK(!held.moving && held.copy_from < 0);

    // A synth track's B2 has no Shift job on Keys (track move and copy are Pad Bank + B2, B3).
    UiState tracks; tracks.page = PageId::Keys; tracks.track = 11;
    CHECK(!button_has_shift_job(p, tracks, SurfaceButton::TopMiddle, false) && !button_has_shift_job(p, tracks, SurfaceButton::TopRight, false));
    tracks.page = PageId::Punch; // Shift + B3 QUANTIZE on Pad FX
    CHECK(!button_has_shift_job(p, tracks, SurfaceButton::TopMiddle, false) && button_has_shift_job(p, tracks, SurfaceButton::TopRight, false));

    // The pads: the track picked white, a move's targets in their own colour (used Dim, empty Faint), a copy's used ones red.
    using namespace pad_light;
    auto host_held = std::make_unique<EngineHost>(); // on the heap: a test's frame stays under the 8 MB stack
    auto& host = *host_held;
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
    t.levels_kind = uint8_t(LevelsKind::Chords); use_drum_pads(t); CHECK(t.levels_kind == uint8_t(LevelsKind::Velocity));
    t.levels_kind = uint8_t(LevelsKind::Tune);
    auto level = levels_of(*project, t, 15);
    CHECK(level.key < 0 && level.semitones == 0 && level.velocity > 0);
    std::array<LevelsKind, 5> types {};
    CHECK(levels_types(t, types) == 1 && types[0] == LevelsKind::Velocity && levels_types(synth, types) == 4 && types[3] == LevelsKind::Chords);
    { // 16 Levels Chords: a pad plays the chord of the track's progression there, a kit its one sample at each note.
        Track chords = synth;
        chords.layout = PadLayout::Scale; chords.levels = true; chords.levels_kind = uint8_t(LevelsKind::Chords);
        chords.prog.chords[0].count = 3; chords.prog.chords[0].notes[0] = 60; chords.prog.chords[0].notes[1] = 64; chords.prog.chords[0].notes[2] = 67;
        std::array<int, notes_per_step> keys {};
        CHECK(levels_chord(chords, 0, keys) == 3 && keys[1] - keys[0] == 4 && keys[2] - keys[0] == 7);
        CHECK(levels_of(*project, chords, 0).key == keys[0] && !levels_of(*project, chords, 0).none);
        CHECK(levels_chord(chords, 1, keys) == 0 && levels_of(*project, chords, 1).none); // no chord on pad 2
        Track one = kit; one.layout = PadLayout::Scale; one.levels = true; one.levels_kind = uint8_t(LevelsKind::Chords); one.prog = chords.prog;
        one.octave = kit_level_octave; one.bank = 3; // a kit's bank is its pads': the first sixteen chords whatever it is
        CHECK(levels_chord(one, 0, keys) == 3 && levels_of(*project, one, 0).semitones == levels_chord_semitones(keys[0]));
        CHECK(levels_chord_semitones(60) == 0 && levels_chord_semitones(67) == 7 && levels_chord_semitones(200) == 63);
    }
    // K3 on Keys reaches Drums (and Prog after it).
    UiState ui; ui.page = PageId::Keys; ui.track = 0;
    Pattern pattern;
    CHECK(page_bindings(*project, ui, pattern)[2].kind == Binding::Kind::Layout && page_bindings(*project, ui, pattern)[2].max >= int(PadLayout::Drums));
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
    auto host_held = std::make_unique<EngineHost>(); // on the heap: a test's frame stays under the 8 MB stack
    auto& host = *host_held;
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
    project->tracks[0].repeat = true; lp.track = 0;
    CHECK(!repeat_keys(*project, lp));
    lp.page = PageId::Keys;
    CHECK(repeat_keys(*project, lp));
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
    auto host_held = std::make_unique<EngineHost>(); // on the heap: a test's frame stays under the 8 MB stack
    auto& host = *host_held;
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
        ui.page = PageId::StepEdit; ui.step_pads = true; ui.step_cursor = 1;
        CHECK(page_bindings(*project, ui, q, false)[0].kind == Binding::Kind::StepLength); // the main row: its length
        ui.step_cards = 2; // TRIG
        auto trig = page_bindings(*project, ui, q, false);
        CHECK(trig[0].kind == Binding::Kind::StepRatchet && trig[0].value == 3 && trig[0].min == 1 && trig[0].max == 8);
        CHECK(trig[1].kind == Binding::Kind::StepRamp && trig[1].value == -40);
        CHECK(shift_bindings(*project, ui, q)[1].kind == Binding::Kind::Division); // a synth has no pitch ramp: Q, as on NOTE
        ui.page = PageId::Roll; ui.step_cursor = 1; ui.roll_key = 60; ui.trig_cards = true; // Roll's TRIG row
        CHECK(page_bindings(*project, ui, q, false)[0].kind == Binding::Kind::EventRatchet);
        ui.trig_cards = false;
        CHECK(page_bindings(*project, ui, q, false)[0].kind == Binding::Kind::EventLength
              && shift_bindings(*project, ui, q)[0].kind == Binding::Kind::EventNudge); // Shift + K1 off the TRIG row: NUDGE (round 2)
        ui.track = 3; ui.page = PageId::StepEdit; ui.step_cards = 2; ui.trig_cards = false;
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
        ui.page = PageId::StepEdit; ui.step_cards = 0;
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
        ui.page = PageId::StepEdit; ui.step_cursor = 1; ui.step_cards = 2;
        auto cards = page_bindings(*project, ui, q, false);
        CHECK(cards[2].kind == Binding::Kind::StepCondition && cards[2].value == int(trig_of(3, 4)) && cards[2].max == int(trig_count) - 1);
        ui.page = PageId::Roll; ui.roll_key = 60; ui.trig_cards = true; // Roll's TRIG row
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
    const double tick = 44100 * 60.0 / 120 / ticks_per_quarter;
    // A MIDI track's CC lane shows Motion as it plays: a point on tick 50, a lock on tick 480 for a sixteenth over the
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
        MotionPoint a; a.tick = 50; a.param = uint8_t(Cc1); a.value = 20;
        MotionPoint lock = point(2, Cc1, 100, 0, sixteenth_ticks);
        MotionPoint other = point(3, Cc2, 90, 0, 60);
        CHECK(p.write_motion(a) && p.write_motion(lock) && p.write_motion(other));
        CHECK(p.lock_at(480, Cc1, 0) && !p.lock_at(50, Cc1, 0) && p.lane_value(600, Cc1, 0, -1) == 20 && p.lane_value(40, Cc1, 0, -1) == -1);
        auto host_held = std::make_unique<EngineHost>(); // on the heap: a test's frame stays under the 8 MB stack
        auto& host = *host_held;
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
        int64_t start = at(74, 20) - std::llround(50 * tick); // the point on tick 50 sets the run's frames
        CHECK(at(74, 20) > 0 && std::llabs(at(74, 100) - start - std::llround(480 * tick)) <= 2);
        CHECK(std::llabs(at(74, 20) - start - std::llround(50 * tick)) <= 2);
        unsigned back20 = 0;
        for (const auto& e : ccs) back20 += e.a == 74 && e.b == 20;
        CHECK(back20 == 2); // tick 50, then the lock's end at tick 720
        CHECK(at(71, 90) > 0 && at(71, 33) > at(71, 90) && std::llabs(at(71, 33) - at(71, 90) - std::llround(60 * tick)) <= 2);
    }
    // A pattern's edits carry its Motion: doubled with the notes, halved, at half and double speed.
    {
        Pattern p;
        p.length = 16 * sixteenth_ticks;
        p.write_motion(point(2, engine_lane, 10)); p.write_motion(point(12, engine_lane, 90, 0, 12));
        CHECK(double_pattern(p, 128 * 3840) && p.motion.size() == 4 && p.motion[2].tick == 18 * sixteenth_ticks && p.motion[3].hold == 12);
        half_pattern(p);
        CHECK(p.motion.size() == 2);
        scale_pattern(p, true); // half speed: twice as far apart; past the end goes
        CHECK(p.motion.size() == 1 && p.motion[0].tick == 4 * sixteenth_ticks);
        p.write_motion(point(10, engine_lane, 50, 0, 8));
        scale_pattern(p, false);
        CHECK(p.motion.size() == 2 && p.motion[1].tick == 5 * sixteenth_ticks && p.motion[1].hold == 4);
        // A shorter length hides the points past the end, as the notes there. A track at another speed does not play
        // them (they all sounded on its last tick), and Double Speed does not bring them in (it did, and a kept punch
        // there too, the notes left out).
        Pattern hidden;
        hidden.length = 16 * sixteenth_ticks;
        hidden.add({20 * sixteenth_ticks, 24, 60, 100, 100, 0});
        hidden.write_motion(point(4, engine_lane, 10)); hidden.write_motion(point(20, engine_lane, 99));
        hidden.punches = {{24 * sixteenth_ticks, 24, 4, 255, 100, 2, 1}};
        for (unsigned speed : {0u, 1u, 3u, 4u}) {
            auto played = speed_pattern(hidden, speed);
            CHECK(played.motion.size() == 1 && played.motion[0].value == 10 && played.events.empty() && played.punches.empty());
        }
        scale_pattern(hidden, false);
        CHECK(hidden.events.empty() && hidden.punches.empty() && hidden.motion.size() == 1 && hidden.motion[0].tick == 2 * sixteenth_ticks);
        // Clear pattern takes the Motion with the notes, as its question says (the notes went, the Motion played on),
        // a pattern holding only Motion too; the length, Q and lanes stay.
        Pattern cleared;
        cleared.length = 32 * sixteenth_ticks; cleared.division = Division::Eighth;
        put(cleared, 0, 60, 100);
        cleared.write_motion(point(4, engine_lane, 10));
        cleared.set_lane_length(64, 6 * sixteenth_ticks);
        CHECK(cleared.clear_notes_and_motion() && cleared.events.empty() && cleared.motion.empty());
        CHECK(cleared.length == 32 * sixteenth_ticks && cleared.division == Division::Eighth && cleared.lanes.size() == 1);
        CHECK(!cleared.clear_notes_and_motion());
        cleared.write_motion(point(4, engine_lane, 10));
        CHECK(cleared.clear_notes_and_motion() && cleared.motion.empty());
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
        ui.page = PageId::StepEdit; ui.step_pads = true; ui.held_step = 3; ui.step_cursor = 3;
        auto cards = page_bindings(*project, ui, q, false);
        CHECK(cards[0].kind == Binding::Kind::Lock && cards[0].id == motion_lane(unsigned(info.favourites[0])) && cards[0].value == 1);
        CHECK(cards[1].kind == Binding::Kind::Lock && cards[1].id == motion_lane(unsigned(info.favourites[1])));
        CHECK(cards[1].value == project->tracks[0].params[size_t(info.favourites[1])]);
        uint32_t tick = 0, hold = 0;
        CHECK(lock_place(*project, ui, q, tick, hold) && tick == 3 * sixteenth_ticks && hold == 2 * sixteenth_ticks);
        ui.held_step = -1;
        CHECK(page_bindings(*project, ui, q, false)[0].kind == Binding::Kind::StepLength && !lock_place(*project, ui, q, tick, hold));
        ui.page = PageId::Roll; ui.roll_key = 60; ui.held_note = 4;
        CHECK(page_bindings(*project, ui, q, false)[0].kind == Binding::Kind::Lock && lock_place(*project, ui, q, tick, hold) && hold == 2 * sixteenth_ticks);
        // Erase + pot on a lock card: that lane's lock out; the step's note, the other lane's lock and the lane's
        // recorded moves stay, and the card shows the track's own value again.
        const unsigned first = lane_param(cards[0].id), second = lane_param(cards[1].id);
        q.write_motion(point(3, second, 9, 0, 48));
        q.write_motion(point(5, first, 30)); // a recorded move
        CHECK(erase_resets(cards[0]) && erase_resets(cards[1]));
        CHECK(!remove_lock(q, 4 * sixteenth_ticks, first, 0) && !remove_lock(q, 5 * sixteenth_ticks, first, 0)); // none; a move is no lock
        CHECK(remove_lock(q, 3 * sixteenth_ticks, first, 0) && !q.lock_at(3 * sixteenth_ticks, first, 0));
        CHECK(q.lock_at(3 * sixteenth_ticks, second, 0) && q.lane_points(first, 0) == 1 && q.events.size() == 1);
        CHECK(!remove_lock(q, 3 * sixteenth_ticks, first, 0));
        ui.page = PageId::StepEdit; ui.held_step = 3;
        int shown = 0; // the lanes with moves come first now: the card found by its lane
        for (const auto& b : page_bindings(*project, ui, q, false))
            if (b.kind == Binding::Kind::Lock && lane_param(b.id) == first)
                shown += b.value == project->tracks[0].params[size_t(info.favourites[0])];
        CHECK(shown == 1);
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
    // A lane's notes past its own length are hidden, as notes past the pattern's end. A feel does not play one (it
    // came round into the lane), and Time Correct leaves it where it is; a note Time Correct snaps to the lane's end
    // goes to the lane's first step, as the feel plays it (it went past the lane's end, unheard).
    {
        Pattern q;
        q.length = 16 * sixteenth_ticks;
        Event e;
        e.key = b; e.length = 60;
        e.tick = 240; q.add(e);
        e.tick = 3400; q.add(e); // past the lane's twelve steps
        q.set_lane_length(b, 12 * sixteenth_ticks);
        q.feel.strength = 100;
        auto played = feel_pattern(q);
        CHECK(played.events.size() == 1 && played.events[0].tick == 240);
        q.events[0].tick = 2800; // nearest the lane's end
        CHECK(feel_pattern(q).events.size() == 1 && feel_pattern(q).events[0].tick == 0);
        q.feel = Feel {};
        time_correct_all(q, nullptr, 50, 0);
        CHECK(q.events.size() == 2 && q.events[0].tick == 0 && q.events[1].tick == 3400);
        time_correct_all(q, nullptr, 50, -20); // shifted back: round the lane's end, not the pattern's
        CHECK(q.events.size() == 2 && q.events[0].tick == 2860 && q.events[1].tick == 3400);
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
        ui.track = 3; ui.page = PageId::StepEdit; ui.step_pads = true;
        Pattern q;
        q.set_lane_length(b, 6 * sixteenth_ticks);
        auto shifted = shift_bindings(*project, ui, q);
        CHECK(shifted[0].kind == Binding::Kind::LaneLength && shifted[0].id == b && shifted[0].value == 6 && shifted[0].max == 16);
        project->tracks[3].last_pad = 0;
        CHECK(shift_bindings(*project, ui, q)[0].value == 16);
        synth(project->tracks[0], 0);
        ui.track = 0;
        CHECK(shift_bindings(*project, ui, q)[0].kind == Binding::Kind::PatternLength); // a synth's steps: the whole step, no lanes
    }
}

// Note Repeat (roadmap P1): 1/4 and 1/64, the pad's pressure as the repeats' velocity, and its notes kept by Seq Record.
void note_repeat_p1() {
    { // Each repeat on its own frame, held or swung, not on the start of the audio block it falls in
        using namespace midi_track;
        for (unsigned swung : {50u, 66u}) {
            auto project = std::make_unique<Project>();
            auto bank = std::make_unique<PatternBank>();
            load_sound(project->tracks[0], EngineKind::Midi, 0);
            project->tracks[0].params[Port] = UsbC; project->tracks[0].params[Channel] = 1;
            project->tempo = 1234; project->swing = uint8_t(swung);
            auto host_held = std::make_unique<EngineHost>(); // on the heap: a test's frame stays under the 8 MB stack
            auto& host = *host_held;
            host.load(*project, *bank);
            host.set_audio_format(44100, 128);
            host.send({Command::Type::Repeat, 0, 0, 2, 6}); // 1/32
            host.send({Command::Type::Repeat, 0, 0, 0, 1});
            std::vector<int32_t> out(256);
            for (unsigned b = 0; b < 37; ++b) host.render(out.data(), 128, 1);
            host.send({Command::Type::RepeatKey, 0, 0, 60, 100});
            std::vector<uint64_t> at;
            for (unsigned b = 0; b < 4000; ++b) {
                host.render(out.data(), 128, 1);
                MidiOutEvent e;
                while (host.take_midi(e)) if (e.kind == MidiOutEvent::Kind::NoteOn) at.push_back(e.frame);
            }
            const double step = 44100.0 * 60.0 / 123.4 / 8.0; // a 1/32 in frames
            const double late = swung > 50 ? step * (swung - 50) / 50.0 : 0;
            CHECK(at.size() > 100);
            double worst = 0;
            for (size_t n = 1; n < at.size(); ++n) {
                double want = double(at[0]) + double(n) * step + (n % 2 ? late : 0); // every second step swung late
                worst = std::max(worst, std::fabs(double(at[n]) - want));
            }
            CHECK(worst <= 1.5); // within a frame and a half (whole-frame rounding), across 4000 blocks: no drift, no block steps
        }
    }
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
    auto host_held = std::make_unique<EngineHost>(); // on the heap: a test's frame stays under the 8 MB stack
    auto& host = *host_held;
    host.load(*project, *bank);
    host.set_audio_format(44100, 384);
    host.send({Command::Type::Repeat, 0, 0, 2, 4}); // 1/16
    host.send({Command::Type::Repeat, 0, 0, 5, 1}); // Settings > PADS: Repeat velocity on Pressure
    host.send({Command::Type::Repeat, 0, 0, 0, 1});
    host.send({Command::Type::SongRecord, 0, 0, 0, 1});
    host.send({Command::Type::Play});
    host.send({Command::Type::RepeatKey, 0, 0, 60, 100});
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
    host.send({Command::Type::Repeat, 0, 0, 5, 0});           // Full Level: the pressure is no velocity
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
        CHECK(kept[n].track == 0 && kept[n].pattern == 0 && kept[n].key == 60 && kept[n].tick == n * sixteenth_ticks % (4 * ticks_per_quarter));
        CHECK(kept[n].velocity == ons[n].b && kept[n].length == sixteenth_ticks * 60 / 100);
    }
    // After a count-in, Seq Record writes the repeats from the take's first tick: the bar counted in is not part of
    // the pattern (a 2-bar pattern's first bar held its first-bar repeats, not its second).
    {
        bank->at[0][0].length = 2 * 4 * ticks_per_quarter;
        auto heap = std::make_unique<EngineHost>(); // large: off the stack
        auto& counted = *heap;
        counted.load(*project, *bank);
        counted.set_audio_format(44100, 384);
        counted.send({Command::Type::Repeat, 0, 0, 2, 4}); // 1/16
        counted.send({Command::Type::Repeat, 0, 0, 0, 1});
        counted.send({Command::Type::SongRecord, 0, 0, 0, 1});
        counted.send({Command::Type::Play, 1});             // a bar's count-in first
        counted.send({Command::Type::RepeatKey, 0, 0, 60, 100}); // held through the count-in
        do counted.render(out.data(), 128, 1); while (counted.count_in());
        for (unsigned b = 0; b < 300; ++b) counted.render(out.data(), 128, 1); // about two beats of the take
        std::vector<EngineHost::ArpWritten> written;
        while (counted.take_arp_written(w)) written.push_back(w);
        CHECK(written.size() >= 6);
        for (size_t n = 0; n < written.size(); ++n) CHECK(written[n].tick == n * sixteenth_ticks);
    }
    // On a kit lane of its own length the repeats are written where the lane plays them: round its three steps
    // (they were written along the pattern, most of them past the lane's end, unheard).
    {
        auto kit = std::make_unique<Project>();
        auto kit_bank = std::make_unique<PatternBank>();
        load_sound(kit->tracks[0], EngineKind::Sampler, 0);
        kit->tempo = 1200;
        const unsigned pad = sampler_first_note + 1;
        kit_bank->at[0][0].set_lane_length(pad, 3 * sixteenth_ticks);
        auto heap = std::make_unique<EngineHost>();
        auto& lane = *heap;
        lane.load(*kit, *kit_bank);
        lane.send({Command::Type::Repeat, 0, 0, 2, 4}); // 1/16
        lane.send({Command::Type::Repeat, 0, 0, 0, 1});
        lane.send({Command::Type::SongRecord, 0, 0, 0, 1});
        lane.send({Command::Type::Play});
        lane.send({Command::Type::RepeatKey, 0, 0, uint16_t(pad), 100});
        for (unsigned b = 0; b < 300; ++b) lane.render(out.data(), 128, 1);
        std::vector<EngineHost::ArpWritten> written;
        while (lane.take_arp_written(w)) written.push_back(w);
        CHECK(written.size() >= 6);
        for (size_t n = 0; n < written.size(); ++n) CHECK(written[n].key == pad && written[n].tick == n % 3 * sixteenth_ticks);
    }
    // In the song, played from a bar inside a clip: the repeats where the pattern is (they fell 256 ticks off, the
    // clip's start before tick 0 read as a tick far ahead).
    {
        auto song = std::make_unique<Project>(*project);
        auto song_bank = std::make_unique<PatternBank>();
        song->song.place({0, 0, 0, 8, false});
        auto heap = std::make_unique<EngineHost>();
        auto& in_song = *heap;
        in_song.load(*song, *song_bank);
        in_song.set_audio_format(44100, 384);
        in_song.send({Command::Type::Repeat, 0, 0, 2, 4}); // 1/16
        in_song.send({Command::Type::Repeat, 0, 0, 0, 1});
        in_song.send({Command::Type::SongRecord, 0, 0, 0, 1});
        in_song.send({Command::Type::Play, 0, 0, 2, 1}); // the song, from bar 3
        in_song.send({Command::Type::RepeatKey, 0, 0, 60, 100});
        for (unsigned b = 0; b < 300; ++b) in_song.render(out.data(), 128, 1);
        std::vector<EngineHost::ArpWritten> written;
        while (in_song.take_arp_written(w)) written.push_back(w);
        CHECK(written.size() >= 6);
        for (size_t n = 0; n < written.size(); ++n) CHECK(written[n].tick == n * sixteenth_ticks);
    }
}

// A pattern's feel (roadmap P1): strength toward Q, swing, humanize and a groove from another pattern, heard live.
void feel_p1() {
    auto note = [](Pattern& p, uint32_t tick, uint8_t key, uint8_t velocity) {
        Event e;
        e.tick = tick; e.key = key; e.velocity = velocity; e.length = 60;
        p.add(e);
    };
    Pattern p;
    note(p, 50, 60, 100); note(p, 300, 62, 100); note(p, 500, 64, 100);
    CHECK(feel_pattern(p) == p); // plain: as written
    p.feel.strength = 100;
    auto played = feel_pattern(p);
    CHECK(played.events[0].tick == 0 && played.events[1].tick == 240 && played.events[2].tick == 480);
    CHECK(p.events[0].tick == 50); // the notes as written stay
    p.feel.strength = 50;
    played = feel_pattern(p);
    CHECK(played.events[0].tick == 25 && played.events[1].tick == 270 && played.events[2].tick == 490);
    p.feel.strength = 100; p.feel.swing = 75; // every second sixteenth half a sixteenth late
    played = feel_pattern(p);
    CHECK(played.events[1].tick == 360 && played.events[2].tick == 480);
    // Time Correct's APPLY (issue 55): the notes where they play, the feel plain, so it sounds the same; a note
    // hidden past the end stays as written.
    {
        Pattern a = p;
        Event hidden; hidden.tick = a.length + 100; hidden.key = 70; hidden.velocity = 90; hidden.length = 60;
        a.events.push_back(hidden);
        apply_feel(a);
        CHECK(a.feel.plain() && a.feel == Feel {});
        CHECK(a.events.size() == 4 && a.events[0].tick == 0 && a.events[1].tick == 360 && a.events[2].tick == 480 && a.events[3].tick == hidden.tick);
        CHECK(feel_pattern(a) == a); // plain now: played as written, where the feel put them
        Pattern plain_one; note(plain_one, 50, 60, 100);
        apply_feel(plain_one); CHECK(plain_one.events[0].tick == 50); // no feel: nothing moves
    }
    // Humanize: each note a little early or late (at most a quarter of a step) and softer or harder (at most 20),
    // the same each time it is laid, until its amount changes.
    Pattern h;
    for (unsigned n = 0; n < 16; ++n) note(h, n * 240 + 120, uint8_t(40 + n), 100);
    h.feel.humanize = 100; h.feel.humanize_velocity = 100;
    auto a = feel_pattern(h), b = feel_pattern(h);
    CHECK(a == b);
    unsigned moved = 0, louder = 0, softer = 0;
    for (const auto& e : a.events) {
        int off = int(e.tick) - int((e.key - 40) * 240 + 120);
        CHECK(std::abs(off) <= 60 && std::abs(int(e.velocity) - 100) <= 20);
        moved += off != 0; louder += e.velocity > 100; softer += e.velocity < 100;
    }
    CHECK(moved >= 8 && louder >= 2 && softer >= 2);
    // A groove taken from a pattern: its second sixteenth 40 ticks late and soft, laid on another's notes.
    Pattern source;
    for (unsigned n = 0; n < 16; ++n) note(source, n * 240 + (n % 2 ? 40 : 0), 36, n % 2 ? 60 : 120);
    Pattern bass;
    note(bass, 0, 40, 100); note(bass, 240, 41, 100);
    groove_of(source, bass.feel);
    CHECK(bass.feel.groove_offsets.size() == 16 && bass.feel.groove_offsets[1] == 40 && bass.feel.groove_offsets[0] == 0);
    CHECK(bass.feel.groove_levels[0] == 133 && bass.feel.groove_levels[1] == 67);
    played = feel_pattern(bass);
    CHECK(played.events[0].tick == 0 && played.events[0].velocity == 127 && played.events[1].tick == 280 && played.events[1].velocity == 67);
    bass.feel.groove_amount = 50;
    played = feel_pattern(bass);
    CHECK(played.events[1].tick == 260 && played.events[1].velocity == 84);
    {   // Time Correct's picture (shell/time_correct.h): each note where feel_pattern plays it; one pulled early off
        // the first step drawn before the start, not round the loop at the end.
        FeltTicks felt(bass);
        CHECK(felt.at(bass.events[0]) == 0 && felt.at(bass.events[1]) == 260);
        Pattern early;
        note(early, 10, 60, 100); note(early, 250, 61, 100);
        early.feel.groove_offsets.assign(1, int16_t(-60)); early.feel.groove_levels.assign(1, uint8_t(100));
        CHECK(feel_pattern(early).events.back().tick > early.length / 2); // heard round the loop
        FeltTicks pulled(early);
        CHECK(pulled.at(early.events[0]) == -50 && pulled.at(early.events[1]) == 190);
        Event hidden; hidden.tick = early.length + 5; hidden.key = 62;
        CHECK(pulled.at(hidden) == hidden.tick); // past the end: not played, drawn as written
    }
    // Heard: the audio thread plays the feel (a note written on tick 50, at strength 100 on tick 0's frame).
    {
        Pattern q;
        note(q, 50, 60, 100); note(q, 2 * 240, 62, 100);
        q.feel.strength = 100;
        auto notes = played_notes(q, 120);
        const double tick = 44100 * 60.0 / 120 / ticks_per_quarter;
        CHECK(notes.size() == 2 && std::llabs(int64_t(notes[1].frame - notes[0].frame) - std::llround(480 * tick)) <= 2);
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
        ui.track = 3; ui.page = PageId::StepEdit; ui.step_cards = 3;
        auto cards = page_bindings(*project, ui, p, false);
        CHECK(cards[0].kind == Binding::Kind::EuclidHits && cards[0].value == 16 && cards[1].kind == Binding::Kind::EuclidSteps && cards[2].value == 3);
        ui.page = PageId::Patterns; ui.sheet_open = true; // Shift + B3 VARY on Patterns
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
    for (size_t n = 0; n < loose.events.size(); ++n) CHECK(std::abs(int(loose.events[n].tick) - int(line.events[n].tick)) <= 60);
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

// A track's playback speed (roadmap P1): x1/64 .. x4, its notes as written.
void track_speed() {
    Pattern p;
    put(p, 0, 60, 100); put(p, 4, 62, 100, 2);
    p.write_motion(point(8, engine_lane, 20, 0, 24));
    p.set_lane_length(64, 6 * sixteenth_ticks);
    auto fast = speed_pattern(p, 9); // x2: every tick half as far
    CHECK(fast.length == 8 * sixteenth_ticks && fast.events[1].tick == 2 * sixteenth_ticks && fast.events[1].length == 2 * sixteenth_ticks / 2);
    CHECK(fast.motion[0].tick == 4 * sixteenth_ticks && fast.motion[0].hold == 12 && fast.lane_length(64) == 3 * sixteenth_ticks);
    auto slow = speed_pattern(p, 5); // x1/2
    CHECK(slow.length == 32 * sixteenth_ticks && slow.events[1].tick == 8 * sixteenth_ticks);
    auto three = speed_pattern(p, 8); // x3/2: sixteenths become sixteenth triplets
    CHECK(three.length == 2560 && three.events[1].tick == 640);
    CHECK(speed_pattern(p, speed_normal) == p && played_pattern(p, speed_normal) == p);
    CHECK(std::string(speed_name(9)) == "\xc3\x97" "2" && speed_of(6).num == 3 && speed_of(6).den == 4);
    // Heard: a track at x2 plays its sixteenths as thirty-seconds; its place is still read in the pattern as written.
    {
        using namespace midi_track;
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        load_sound(project->tracks[0], EngineKind::Midi, 0);
        project->tracks[0].params[Port] = UsbC; project->tracks[0].params[Channel] = 1; project->tracks[0].speed = 9;
        project->tempo = 1200;
        put(bank->at[0][0], 0, 60, 100); put(bank->at[0][0], 4, 62, 100);
        auto host_held = std::make_unique<EngineHost>(); // on the heap: a test's frame stays under the 8 MB stack
        auto& host = *host_held;
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
        const double tick = 44100 * 60.0 / 120 / ticks_per_quarter;
        CHECK(ons.size() >= 2 && ons[0].a == 60 && ons[1].a == 62 && std::llabs(int64_t(ons[1].frame - ons[0].frame) - std::llround(480 * tick)) <= 2);
        int at = host.position_of(0); // 12800 frames: 557 ticks played, 1114 of the pattern as written
        CHECK(at >= 1100 && at <= 1120);
    }
    // An editor's loop (Loop on Roll or Step Edit) is sent in the pattern's ticks as written: on a track at another
    // speed it plays the same bar. Bar 2 of two looped: at x2 the loop was ignored (it starts past the pattern as it
    // plays), at x1/2 beats 3 and 4 of bar 1 played. A project loaded after it does not keep the loop.
    for (unsigned speed : {9u, 5u, speed_normal}) {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        synth(project->tracks[0], 0);
        project->tracks[0].speed = uint8_t(speed);
        project->tempo = 1200;
        bank->at[0][0].length = 768;
        auto host = std::make_unique<EngineHost>();
        host->load(*project, *bank);
        tones_for(*host, *project);
        Command loop {Command::Type::EditLoop, 0};
        loop.position = 384; loop.index = 384;
        host->send(loop);
        host->send({Command::Type::Play});
        std::vector<int32_t> out(256);
        int low = INT32_MAX, high = -1;
        for (unsigned b = 0; b < 2200; ++b) { // 1226 ticks: every tick of the loop at any speed
            host->render(out.data(), 128, 1);
            low = std::min(low, host->position_of(0)); high = std::max(high, host->position_of(0));
        }
        CHECK(low == 384 && high >= 760 && high < 768);
        Command reload {Command::Type::Reload};
        reload.pack = new ProjectPack(*project, *bank);
        host->send(reload);
        host->send({Command::Type::Play});
        low = INT32_MAX;
        for (unsigned b = 0; b < 400; ++b) { host->render(out.data(), 128, 1); low = std::min(low, host->position_of(0)); }
        CHECK(low < 24);
        const ProjectPack* done = nullptr;
        while (host->take_done_pack(done)) delete done;
    }
    // Shift + Play on a track at another speed carries on where it stopped: Stop keeps the track's place as written
    // (position_of), Play gets it as the transport's tick (played_ticks). At x2 it carried on at twice the place, round
    // the pattern (444 came back as 120). MIDI's song position comes in as the transport's tick (written_ticks).
    CHECK(played_ticks(444, 9) == 222 && written_ticks(222, 9) == 444 && played_ticks(111, 5) == 222 && played_ticks(333, 8) == 222);
    CHECK(played_ticks(321, speed_normal) == 321 && written_ticks(321, speed_normal) == 321);
    for (unsigned speed : {5u, 6u, 8u, 9u}) CHECK(played_ticks(written_ticks(7 * sixteenth_ticks, speed), speed) == 7 * sixteenth_ticks);
    for (unsigned speed : {9u, 5u, 6u, 8u, speed_normal}) {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        synth(project->tracks[0], 0);
        project->tracks[0].speed = uint8_t(speed);
        project->tempo = 1200;
        bank->at[0][0].length = 768;
        auto host = std::make_unique<EngineHost>();
        host->load(*project, *bank);
        tones_for(*host, *project);
        std::vector<int32_t> out(256);
        host->send({Command::Type::Play});
        for (unsigned b = 0; b < 400; ++b) host->render(out.data(), 128, 1);
        int stopped = host->position_of(0);
        host->send({Command::Type::Stop});
        host->render(out.data(), 128, 1);
        Command from {Command::Type::Play};
        from.position = uint32_t(played_ticks(uint32_t(stopped), speed));
        host->send(from);
        host->render(out.data(), 128, 1);
        CHECK(stopped > 0 && std::abs(host->position_of(0) - stopped) <= 12); // a block on: 5.6 ticks played
        host->send({Command::Type::Stop});
        host->render(out.data(), 128, 1);
    }
    // The file: TSPD; without it every track at x1.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        project->tracks[6].speed = 6; // x3/4
        project->tracks[4].speed = 0; // x1/64: TSP2 alone holds it (TSPD, as builds before read it: x1)
        auto data = save_project(*project, *bank);
        auto back = std::make_unique<Project>();
        auto back_bank = std::make_unique<PatternBank>();
        CHECK(load_project(data, *back, *back_bank).empty() && back->tracks[6].speed == 6 && back->tracks[4].speed == 0 && back->tracks[5].speed == speed_normal);
        std::string text(data.begin(), data.end());
        data[text.find("TSP2")] = 'X'; // a file from before: TSPD's five speeds
        CHECK(load_project(data, *back, *back_bank).empty() && back->tracks[6].speed == 6 && back->tracks[4].speed == speed_normal);
        data[text.find("TSPD")] = 'X';
        CHECK(load_project(data, *back, *back_bank).empty() && back->tracks[6].speed == speed_normal);
    }
    // Tracks' Shift + K1.
    {
        auto project = std::make_unique<Project>();
        synth(project->tracks[2], 2);
        project->tracks[2].speed = 8;
        UiState ui;
        ui.page = PageId::Patterns; ui.track = 2; // since the mixer: Patterns, Shift + K3 (Kerem's pick)
        Pattern none;
        auto cards = shift_bindings(*project, ui, none);
        CHECK(cards[2].kind == Binding::Kind::TrackSpeed && cards[2].value == 8 && cards[2].id == 2 && cards[2].max == int(speed_count) - 1);
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
        // Random hold: by hand as Random; held under Note Repeat, every hit the layer the first picked, until let go.
        PadSound held = pad;
        held.layer_play = LayerPlay::RandomHold;
        e.set_pad(2, held);
        std::array<unsigned, 3> loose {};
        for (unsigned n = 0; n < 60; ++n) { auto slots = hit(e, 64); if (slots.size() == 1) ++loose[slots[0] - 4u]; }
        CHECK(loose[0] > 5 && loose[1] > 5 && loose[2] > 5);
        std::array<unsigned, 3> kept_first {};
        for (unsigned hold = 0; hold < 30; ++hold) {
            e.hold_layer(2, true);
            auto first = hit(e, 64);
            CHECK(first.size() == 1);
            for (unsigned n = 0; n < 8; ++n) CHECK(hit(e, 64) == first); // the hold keeps it
            e.hold_layer(2, false);
            if (first.size() == 1) ++kept_first[first[0] - 4u];
        }
        CHECK(kept_first[0] > 2 && kept_first[1] > 2 && kept_first[2] > 2); // each hold picks again
        PadSound cycled = pad; // Cycle hold: each hold the next layer, kept while held
        cycled.layer_play = LayerPlay::CycleHold;
        cycled.layers[0] = PadLayer {};
        cycled.layers[1].low = cycled.layers[1].high = 127;
        e.set_pad(2, cycled);
        e.hold_layer(2, true);
        auto one = hit(e, 10);
        CHECK(hit(e, 10) == one && hit(e, 10) == one);
        e.hold_layer(2, false);
        e.hold_layer(2, true);
        auto two = hit(e, 10);
        CHECK(two != one && hit(e, 10) == two);
        e.hold_layer(2, false);
        CHECK(std::string(layer_play_name(LayerPlay::CycleHold)) == "CYCLE HOLD" && int(LayerPlay::RandomHold) == 4); // saved: appended
        // Issue 66: CC, the layer the controller's value falls in, whatever the velocity; none heard yet, 0 (as 1).
        PadSound hat = pad;
        CHECK(int(LayerPlay::Cc) == 5 && std::string(layer_play_name(LayerPlay::Cc)) == "CC" && hat.layer_cc == 4);
        set_layer_play_choice(hat, layer_play_choice(hat) + 0); // still Velocity
        CHECK(hat.layer_play == LayerPlay::Velocity);
        set_layer_play_choice(hat, int(LayerPlay::Cc) + 3); // CC 4
        CHECK(hat.layer_play == LayerPlay::Cc && hat.layer_cc == 4 && layer_play_choice(hat) == int(LayerPlay::Cc) + 3);
        CHECK(hat.low_velocity == 1 && hat.layers[0].low == 64); // ranges of their own: kept
        e.set_pad(2, hat);
        CHECK((hit(e, 127) == std::vector<uint16_t> {4}));     // CC 4 not heard: 0, the first layer
        e.control(4, 80);
        CHECK((hit(e, 10) == std::vector<uint16_t> {5}));      // half open, however soft the hit
        e.control(4, 120);
        CHECK((hit(e, 10) == std::vector<uint16_t> {5, 6}));
        e.control(7, 0); // another controller changes nothing
        CHECK((hit(e, 10) == std::vector<uint16_t> {5, 6}));
        set_layer_play_choice(hat, int(LayerPlay::Cc) + 10); // CC 11: not heard, 0
        e.set_pad(2, hat);
        CHECK(hat.layer_cc == 11 && (hit(e, 10) == std::vector<uint16_t> {4}));
        PadSound even = pad; // every layer on the whole range: CC first spreads it, in order
        even.low_velocity = 1; even.high_velocity = 127;
        even.layers[0].low = 1; even.layers[0].high = 127; even.layers[1].low = 1; even.layers[1].high = 127;
        set_layer_play_choice(even, int(LayerPlay::Cc) + 3);
        CHECK(even.low_velocity == 1 && even.high_velocity == 41 && even.layers[0].low == 42 && even.layers[0].high == 84 &&
              even.layers[1].low == 85 && even.layers[1].high == 127);
        set_layer_play_choice(even, 0);
        CHECK(even.layer_play == LayerPlay::Velocity && even.layer_cc == 4);
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
        std::snprintf(project->samples.edit(4).path, sizeof(SampleRef::path), "a.wav");
        std::snprintf(project->samples.edit(5).path, sizeof(SampleRef::path), "b.wav");
        std::snprintf(project->samples.edit(6).path, sizeof(SampleRef::path), "c.wav");
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
        // Issue 66: CC's ranges on K2 / K3; Shift + K1 past the five, CC 1-127; kept in LYCC, as is the pad's CC.
        set_layer_play_choice(project.tracks[1].pads[9], int(LayerPlay::Cc) + 7); // CC 8
        CHECK(base_bindings(project, ui, none)[1].kind == Binding::Kind::LayerLow);
        auto play = shift_bindings(project, ui, none)[0];
        CHECK(play.max == layer_play_choices - 1 && play.value == int(LayerPlay::Cc) + 7);
        auto bank = std::make_unique<PatternBank>();
        auto back = std::make_unique<Project>();
        auto back_bank = std::make_unique<PatternBank>();
        CHECK(load_project(save_project(project, *bank), *back, *back_bank).empty());
        CHECK(back->tracks[1].pads[9].layer_play == LayerPlay::Cc && back->tracks[1].pads[9].layer_cc == 8 && back->tracks[1].pads[3].layer_cc == 4);
        CHECK(lane_name(project.tracks[1], motion_lane(motion_cc, 4), &project) == "CC 4" && !lane_binding(project.tracks[1], 1, motion_lane(motion_cc, 4), &project).bound());
        CHECK(sampler_next(PageId::Trim, 0) == PageId::LayersPage && sampler_next(PageId::LayersPage, 0) == PageId::PadPage);
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
    // The master's chain late by 50 more: MIDI out (a MIDI track's notes) goes out as late as what is heard; played by
    // hand, at once.
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
    while (host->take_midi(e)) if (e.kind == MidiOutEvent::Kind::NoteOn && e.a == 64) { found = true; CHECK(e.live && e.frame == before - EngineHost::block); }
    CHECK(found);
    host->send({Command::Type::Mute, 2, 0, 0, 1}); // its Note Off for the mute: as late as what is heard
    before = rendered;
    play(1, nullptr, nullptr);
    found = false;
    while (host->take_midi(e)) if (e.kind == MidiOutEvent::Kind::NoteOff && e.a == 64) { found = true; CHECK(!e.live && e.frame == before + 250); }
    CHECK(found);
    host->send({Command::Type::Mute, 2, 0, 0, 0});
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
    Event e; e.tick = 0; e.length = 4 * ticks_per_quarter - 40; e.key = 60;
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
        if (at > 200 && at < 1800) early = std::max(early, stem);
        if (at > 2200 && at < 3700) { late = std::max(late, stem); mix_late = std::max(mix_late, mix); }
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
    // DELAY, played by hand: out at once from the frame it was played at (a block before the one it plays at), +10 ms
    // later; a negative DELAY only brings sequenced notes earlier, so played by hand it goes out at once.
    got.clear();
    host->send({Command::Type::Param, 0, 0, uint16_t(Delay), 10});
    uint64_t at = rendered;
    host->send({Command::Type::NoteOn, 0, 0, 60, 100});
    run(1);
    CHECK(find(E::NoteOn) && find(E::NoteOn)->live &&
          find(E::NoteOn)->frame == at - EngineHost::block + uint64_t(std::lround(10 * dsp::sample_rate() / 1000.0)));
    got.clear();
    host->send({Command::Type::NoteOff, 0, 0, 60, 0});
    host->send({Command::Type::Param, 0, 0, uint16_t(Delay), -50});
    run(1);
    got.clear();
    at = rendered;
    host->send({Command::Type::NoteOn, 0, 0, 62, 100});
    run(1);
    CHECK(find(E::NoteOn) && find(E::NoteOn)->live && find(E::NoteOn)->frame == at - EngineHost::block);
    // An input as the track's sound: the rear inputs into its stem, at its LEVEL.
    host->send({Command::Type::MidiAudio, 0, 0, 0, AudioRear});
    std::vector<int32_t> input(256, int32_t(0.25 * 2147483647.0));
    std::vector<float> stems(size_t(track_count) * EngineHost::block);
    run(4, input.data(), stems.data());
    float most = 0;
    for (unsigned f = 0; f < 128; ++f) most = std::max(most, std::fabs(stems[f]));
    CHECK(most > 0.15f);
    // A pad's note on and off inside the block split it into runs: the input plays on through them, frame by frame
    // (each run started the input from the block's first frame again: a click at each touch and release).
    {
        std::vector<int32_t> ramp(256);
        for (unsigned f = 0; f < 128; ++f) ramp[2 * f] = ramp[2 * f + 1] = int32_t((0.1 + 0.002 * f) * 2147483647.0);
        run(2, ramp.data(), stems.data());
        Command on {Command::Type::NoteOn, 0, 0, 60, 100}, off {Command::Type::NoteOff, 0, 0, 60, 0};
        on.timed = off.timed = true;
        on.at = uint32_t(rendered + 40); off.at = uint32_t(rendered + 90);
        host->send(on); host->send(off);
        run(1, ramp.data(), stems.data());
        bool rising = true;
        for (unsigned f = 1; f < 128; ++f) rising &= stems[f] > stems[f - 1];
        CHECK(rising);
    }
    host->send({Command::Type::MidiAudio, 0, 0, 0, AudioNone});
    run(2, input.data(), stems.data());
    most = 0;
    for (unsigned f = 0; f < 128; ++f) most = std::max(most, std::fabs(stems[f]));
    CHECK(most < 1e-4f);
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
        project->scenes[2].colour = 5; project->scenes[11].colour = 16; // issue 19: a colour alone is kept too
        CHECK(!project->scenes[11].plain());
        auto data = save_project(*project, *bank);
        auto back = std::make_unique<Project>();
        auto back_bank = std::make_unique<PatternBank>();
        CHECK(load_project(data, *back, *back_bank).empty() && back_bank->at[3][5].follow == f && !back_bank->at[3][4].follow.on());
        CHECK(std::string(back->scenes[2].name) == "CHORUS" && back->scenes[2].tempo == 1285 && back->scenes[9].tempo == 900 && back->scenes[0].plain());
        CHECK(back->scenes[2].colour == 5 && back->scenes[9].colour == 0 && back->scenes[11].colour == 16 && !back->scenes[11].tempo);
        std::string text(data.begin(), data.end());
        data[text.find("FOLW")] = 'X'; data[text.find("SCNE")] = 'X';
        CHECK(load_project(data, *back, *back_bank).empty() && !back_bank->at[3][5].follow.on() && back->scenes[2].plain());
    }
    // SCENES' cards: the selected track's pattern's follow action; Shift + K1 the second.
    {
        auto project = std::make_unique<Project>();
        synth(project->tracks[1], 2);
        UiState ui;
        ui.page = PageId::Patterns; ui.patterns_cards = 2; ui.track = 1; // FOLLOW
        Pattern p;
        p.follow = f;
        auto cards = base_bindings(*project, ui, p);
        CHECK(cards[0].kind == Binding::Kind::FollowAction && cards[0].value == int(FA::Next) && cards[1].kind == Binding::Kind::FollowAfter &&
              cards[1].value == 66 && cards[2].kind == Binding::Kind::FollowChance && cards[2].value == 75);
        auto shifted = shift_bindings(*project, ui, p);
        CHECK(shifted[0].kind == Binding::Kind::FollowOther && shifted[0].value == int(FA::Previous) && !shifted[1].bound());
        // SEQUENCE (Shift + B1): K1 the tempo of the sequence in hand, whatever pattern the track is on; the pattern's
        // cards again on PATTERNS.
        ui.patterns_cards = 1; project->tracks[1].pattern = 4; project->scene = 19; project->scenes[19].tempo = 1284;
        auto scene = base_bindings(*project, ui, p);
        CHECK(scene[0].kind == Binding::Kind::SceneTempo && scene[0].id == 19 && scene[0].value == 128);
        CHECK(scene[1].kind == Binding::Kind::SceneColour && scene[1].id == 19 && scene[1].value == 0 && scene[1].max == 16 && !scene[2].bound());
        CHECK(!shift_bindings(*project, ui, p)[0].bound());
        ui.patterns_cards = 0;
        CHECK(base_bindings(*project, ui, p)[0].kind == Binding::Kind::PatternLength);
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
        // Stop before the bar it waits for: the tempo comes with the scene's patterns, which Stop brings at once
        // (it waited for the first bar of the next Play, the screen already showing it).
        host->send({Command::Type::Pattern, 0, 0, 0, 3});
        host->send({Command::Type::SceneTempo, 0, 0, 0, 900});
        for (unsigned b = 0; b < 20; ++b) host->render(buffer.data(), 128, 1);
        CHECK(host->tempo_now() == 1500 && host->pattern_of(0) == 0);
        host->send({Command::Type::Stop});
        host->render(buffer.data(), 128, 1);
        CHECK(host->pattern_of(0) == 3 && host->tempo_now() == 900);
    }
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
            if (host->song_bar() == 1 && host->ticks() % 3840 >= 1000 && !tempo_in_bar2) { tempo_in_bar2 = host->tempo_now(); quarter_in_bar2 = effect_clock.quarter; }
            if (host->song_bar() == 2 && host->ticks() % 3840 >= 1920 && !tempo_mid_ramp) tempo_mid_ramp = host->tempo_now(); // the first block past half way
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
        CHECK(!tempos.empty() && tempos[0].first == 4 * smf_ppq && tempos[0].second == 900 && tempos.back().second == 1200 && tempos.size() > 10);
        auto bytes = write_smf({}, 1200, 4, 4, tempos);
        unsigned count = 0;
        for (size_t n = 0; n + 2 < bytes.size(); ++n) count += bytes[n] == 0xff && bytes[n + 1] == 0x51 && bytes[n + 2] == 3;
        CHECK(count == tempos.size() + 1);
    }
}

// Freeze plan F2d: a deep freeze in the file and in the plug-in rack's bookkeeping (plugin/plugin_sleep.h). FRZN
// keeps Deep; the plug-in states go and come back byte for byte. A project opened with a deep-frozen track never
// loads that track's plug-ins (its own and its chain's), keeps their state as the file had it, and loads them with
// it when the track thaws; one let go while loaded loads again with the state it had then. A plug-in a bus also uses
// stays.
void deep_freeze_f2d() {
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    auto name = [&](unsigned e, const char* path) {
        std::snprintf(project->plugins[e].path, sizeof(project->plugins[e].path), "%s", path);
        std::snprintf(project->plugins[e].name, sizeof(project->plugins[e].name), "%s", path);
    };
    name(0, "Plugins/Poly"); name(1, "Plugins/Amp"); name(2, "Plugins/Hall");
    load_sound(project->tracks[1], EngineKind::Plugin, 0);
    project->tracks[1].plugin = 0;
    project->tracks[1].fx.slots[0] = {plugin_effect_kind, false, {1, 0, 0, 0, 0, 0}};
    project->tracks[1].fx.slots[1] = {plugin_effect_kind, false, {2, 0, 0, 0, 0, 0}};
    project->buses[0].fx.slots[0] = {plugin_effect_kind, false, {2, 0, 0, 0, 0, 0}};
    put(bank->at[1][0], 0, 60, 100);
    put(bank->at[1][3], 4, 64, 90);
    const uint32_t deep = 1u << 1;
    CHECK(plugin_used_by(*project, 0) && plugin_used_by(*project, 1) && plugin_used_by(*project, 2));
    CHECK(!plugin_used_awake(*project, 0, deep) && !plugin_used_awake(*project, 1, deep) && plugin_used_awake(*project, 2, deep));
    CHECK(plugin_used_awake(*project, 0, 0) && plugin_used_awake(*project, 1, 0));

    // FRZN: Deep goes and comes back, every pattern with notes; the states untouched.
    PrintTable table;
    auto& f = table.freeze[1];
    f.state = FreezeState::Deep; f.tap = PrintTap::PostFx; f.tail = PrintTail::Ring;
    f.set(0, true); f.set(3, true);
    std::array<std::vector<uint8_t>, max_plugins> states;
    states[0] = {7, 0, 255, 3, 9, 9, 9};
    states[1].assign(70000, 0);
    for (size_t n = 0; n < states[1].size(); ++n) states[1][n] = uint8_t(n * 31 + 7);
    states[2] = {1};
    auto bytes = save_project(*project, *bank, &states, &table);
    auto back = std::make_unique<Project>();
    auto back_bank = std::make_unique<PatternBank>();
    std::array<std::vector<uint8_t>, max_plugins> read_states;
    PrintTable read;
    CHECK(load_project(bytes, *back, *back_bank, nullptr, &read_states, &read).empty());
    CHECK(read == table && read.freeze[1].state == FreezeState::Deep && read.freeze[1].plays(3) && !read.freeze[1].plays(1));
    CHECK(read_states == states);
    { // a freeze of an older build (state 1) reads as a freeze, not a deep one
        PrintTable older = table;
        older.freeze[1].state = FreezeState::Frozen;
        CHECK(load_project(save_project(*project, *bank, &states, &older), *back, *back_bank, nullptr, nullptr, &read).empty());
        CHECK(read.freeze[1].state == FreezeState::Frozen);
    }

    // The rack, as sync() runs it with the project just opened: entries 0 and 1 sleep, never loaded; 2 loads.
    std::array<PluginPlan, 3> plan;
    for (unsigned e = 0; e < 3; ++e) { plan[e].state = read_states[e]; plan[e].reload = true; }
    auto step = [&](unsigned e, uint32_t asleep, double rate, bool loaded) {
        std::string want = plugin_used_by(*back, e) ? back->plugins[e].path : "";
        bool only = !want.empty() && asleep && !plugin_used_awake(*back, e, asleep);
        return plan_plugin(plan[e], want, 0, only, rate, loaded);
    };
    CHECK(step(0, deep, 48000, false) == PluginStep::Changed && plan[0].asleep && plan[0].status == PluginStatus::None);
    CHECK(step(1, deep, 48000, false) == PluginStep::Changed && plan[1].asleep);
    CHECK(step(2, deep, 48000, false) == PluginStep::Load && !plan[2].asleep);
    for (unsigned tick = 0; tick < 3; ++tick) CHECK(step(0, deep, 48000, false) == PluginStep::Keep && step(1, deep, 48000, false) == PluginStep::Keep);
    CHECK(step(0, deep, 96000, false) == PluginStep::Keep); // a new rate: still not loaded
    CHECK(plan[0].state == states[0] && plan[1].state == states[1]); // saved again as they came
    // Thawed: both load, with the file's state, at the rate now.
    CHECK(step(0, 0, 96000, false) == PluginStep::Load && plan[0].state == states[0] && plan[0].rate == 96000 && plan[0].status == PluginStatus::Loading);
    CHECK(step(1, 0, 96000, false) == PluginStep::Load && plan[1].state == states[1]);
    plan[0].status = plan[1].status = PluginStatus::Ready;
    CHECK(step(0, 0, 96000, true) == PluginStep::Keep && step(1, 0, 96000, true) == PluginStep::Keep);
    // Deep frozen again while loaded: let go (Sleep: the rack takes the state it has now), a load under way dropped.
    uint32_t was = plan[0].generation;
    CHECK(step(0, deep, 96000, true) == PluginStep::Sleep && plan[0].generation == was + 1 && plan[0].asleep);
    plan[0].state = {42, 43}; // what the rack took from the plug-in as it let go
    CHECK(step(0, deep, 96000, false) == PluginStep::Keep);
    CHECK(step(0, 0, 96000, false) == PluginStep::Load && plan[0].state == std::vector<uint8_t>({42, 43}));
    // Deep frozen while still loading: let go too, the state it was loading with kept.
    plan[1].status = PluginStatus::Loading;
    auto loading_with = plan[1].state;
    CHECK(step(1, deep, 96000, false) == PluginStep::Sleep && plan[1].state == loading_with);
    // The track's plug-in taken off while asleep: let go entirely.
    back->tracks[1].fx.slots[0] = {};
    CHECK(step(1, deep, 96000, false) == PluginStep::Unload && !plan[1].asleep && plan[1].state.empty());

    // A plug-in replaced, then undone and done again (Context::swap_plugin, PluginRack::restore): another plug-in in
    // a loaded entry starts from its own defaults; a restore's reload keeps the state handed back, whichever loads.
    {
        PluginPlan p;
        p.path = "Old.vst3"; p.status = PluginStatus::Ready; p.rate = 48000; p.state = {1, 2};
        CHECK(plan_plugin(p, "New.vst3", 0, false, 48000, true) == PluginStep::Load && p.state.empty() && p.path == "New.vst3");
        p.status = PluginStatus::Ready;
        p.state = {1, 2}; p.reload = true; // undo: the old plug-in's state, kept in the edit
        CHECK(plan_plugin(p, "Old.vst3", 0, false, 48000, true) == PluginStep::Load && p.state == std::vector<uint8_t>({1, 2}));
        p.status = PluginStatus::Ready;
        p.state = {7}; p.reload = true; // redo: the new one's, as it was when undone
        CHECK(plan_plugin(p, "New.vst3", 0, false, 48000, true) == PluginStep::Load && p.state == std::vector<uint8_t>({7}));
        Edit replaced;
        replaced.plugin = std::make_unique<PluginSwap>();
        std::snprintf(replaced.plugin->before.name, sizeof(PluginRef::name), "Surge XT");
        CHECK(replaced.plugin->before.used() == false && std::string(replaced.plugin->before.name) == "Surge XT");
    }
    std::puts("deep freeze ok");
}

// Kit pads' own colours: an MPC kit brings them, Stage's COLOUR (B2 after PLAY) picks them from the track
// colours, PCOL and BCOL keep them, and the pads light in them.
void pad_colours() {
    using K = Binding::Kind;
    // A dark MPC colour at full brightness, its hue kept; none stays none.
    CHECK(full_colour(0x7F0000) == 0xFF0000 && full_colour(0x50007F) == 0xA100FF && full_colour(0x0000FF) == 0x0000FF && full_colour(0) == 0);
    // COLOUR's list: the track's, the sixteen, then Kit, the colour the pad's MPC kit brought.
    CHECK(colour_choice(0) == colour_track && colour_choice(ui::track_shades[2][0]) == 3 && colour_choice(0x123456) == colour_own);
    PadSound plain, picked, from_kit, before;
    picked.colour = ui::track_shades[15][0];
    from_kit.colour = from_kit.kit_colour = 0x123456;
    before.colour = 0x123456; // a pad from before PKCL: its colour off the sixteen is its kit's
    CHECK(colour_last(plain) == colour_own - 1 && colour_last(picked) == colour_own - 1 && colour_last(from_kit) == colour_own);
    CHECK(colour_last(before) == colour_own && kit_colour(before) == 0x123456 && !kit_colour(picked));
    CHECK(colour_at(colour_track, from_kit) == 0 && colour_at(3, plain) == ui::track_shades[2][0] && colour_at(colour_own, from_kit) == 0x123456);
    CHECK(std::string(ui::track_colour_names[2]) == "Teal");
    // Turned off Kit and back: the kit's colour comes back (it was lost once another was picked), a pad from before too.
    for (PadSound* pad : {&from_kit, &before}) {
        pick_colour(*pad, 5);
        CHECK(pad->colour == ui::track_shades[4][0] && pad->kit_colour == 0x123456 && colour_last(*pad) == colour_own);
        pick_colour(*pad, colour_track);
        CHECK(!pad->colour && colour_last(*pad) == colour_own);
        pick_colour(*pad, colour_own);
        CHECK(pad->colour == 0x123456 && colour_choice(pad->colour) == colour_own);
    }
    pick_colour(picked, colour_own); // no kit colour: the list ends on Grey
    CHECK(colour_last(picked) == colour_own - 1 && !picked.kit_colour);

    // The LEDs: the sixteen squared as a level is and raised to full, so they light as vivid as the screen draws
    // them (Coral FF7A5C drove 127, 61, 46: pale); an MPC kit's as it came.
    CHECK(pad_light::led(0xFF7A5C) == 0xFF3A21 && pad_light::led(0x46D9B0) == 0x1BFFA8 && pad_light::led(0xFFFFFF) == 0xFFFFFF);
    CHECK(pad_light::led(0xFF0000) == 0xFF0000 && pad_light::led(0) == 0);
    CHECK(pad_light::pad_led(ui::track_shades[0][0]) == 0xFF3A21 && pad_light::pad_led(0x7F3F00) == 0x7F3F00);
    CHECK(pad_light::track(0, pad_light::full).rgb.g == 29 && pad_light::track(2, pad_light::full).rgb.g == 127);

    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    auto& kit = project->tracks[1];
    kit.engine = EngineKind::Sampler;
    std::snprintf(project->samples.edit(0).path, sizeof(SampleRef::path), "Samples/Kick.wav");
    project->samples.edit(0).frames = 44100;
    kit.pads[5].sample = 0; kit.pads[7].sample = 0;
    kit.last_pad = 5;
    UiState ui;
    ui.track = 1; ui.page = PageId::Stage;
    Pattern none;
    stage_go(ui, EngineKind::Sampler, StageKind::Colour);
    CHECK(ui.stage_group[1] == 1 && ui.stage_sub[1] == 1); // B2, after PLAY
    auto cards = page_bindings(*project, ui, none, false);
    CHECK(cards[0].kind == K::PadColour && cards[0].value == colour_track && cards[0].min == colour_track && cards[0].max == colour_own - 1);
    CHECK(!cards[1].bound() && !cards[2].bound() && !shift_bindings(*project, ui, none)[0].bound());
    kit.pads[5].colour = 0x123456; // an MPC kit's: the list reaches it at its end
    cards = page_bindings(*project, ui, none, false);
    CHECK(cards[0].value == colour_own && cards[0].max == colour_own);
    kit.pads[5].colour = ui::track_shades[4][0];
    CHECK(page_bindings(*project, ui, none, false)[0].value == 5 && page_bindings(*project, ui, none, false)[0].max == colour_own - 1);
    kit.pads[5].kit_colour = 0x123456; // Sky over a kit's colour: the list still ends on Kit
    CHECK(page_bindings(*project, ui, none, false)[0].value == 5 && page_bindings(*project, ui, none, false)[0].max == colour_own);
    kit.pads[5].kit_colour = 0;
    kit.last_pad = 6; // an empty pad: nothing to colour
    CHECK(!page_bindings(*project, ui, none, false)[0].bound());
    kit.last_pad = 5;

    // The pads light in their own colours; one without stays in the track's.
    kit.pads[5].colour = 0xFF0000; kit.pads[7].colour = ui::track_shades[4][0];
    auto host_held = std::make_unique<EngineHost>(); // on the heap: a test's frame stays under the 8 MB stack
    auto& host = *host_held;
    TrackTaps taps;
    PadFacts plays; plays.kit_pads = true;
    std::array<pad_light::Look, 16> looks;
    pad_looks(PadScene {ui, *project, *bank, host, taps, plays, 1000, nullptr, 0}, looks);
    CHECK(looks[5].colour == 0xFF0000 && looks[5].hue == pad_light::Hue::Track && pad_light::rung(looks[5].level) == 4);
    CHECK(looks[7].colour == pad_light::led(ui::track_shades[4][0]) && pad_light::rung(looks[7].level) == 2);
    kit.pads[7].colour = 0;
    pad_looks(PadScene {ui, *project, *bank, host, taps, plays, 1000, nullptr, 0}, looks);
    CHECK(looks[7].colour == pad_light::led(ui::track_shades[1][0]));

    // PCOL keeps them; an empty pad's is not kept; a build without PCOL leaves the track's.
    kit.pads[7].colour = ui::track_shades[4][0]; kit.pads[9].colour = 0x00FF00;
    auto file = save_project(*project, *bank);
    auto loaded = std::make_unique<Project>();
    auto loaded_bank = std::make_unique<PatternBank>();
    kit.pads[7].kit_colour = 0x2AC2FF; // Kit kept behind Sky; PKCL keeps it
    file = save_project(*project, *bank);
    CHECK(load_project(file, *loaded, *loaded_bank).empty());
    CHECK(loaded->tracks[1].pads[5].colour == 0xFF0000 && loaded->tracks[1].pads[7].colour == ui::track_shades[4][0] && loaded->tracks[1].pads[9].colour == 0);
    CHECK(loaded->tracks[1].pads[7].kit_colour == 0x2AC2FF && !loaded->tracks[1].pads[5].kit_colour);
    std::string text(file.begin(), file.end());
    file[text.find("PCOL")] = 'X';
    CHECK(load_project(file, *loaded, *loaded_bank).empty() && loaded->tracks[1].pads[5].colour == 0);
    // A bank file keeps them in BCOL, and they come back with it.
    BankFile saved = bank_of(*project, 1, 0, "Colours"), back;
    CHECK(load_bank(save_bank(saved), back) && back.pads[5].colour == 0xFF0000 && back.pads[7].colour == ui::track_shades[4][0]);
    CHECK(back.pads[7].kit_colour == 0x2AC2FF); // and Kit in BKCL
    Track into;
    import_bank(back, 2, into, [](const SampleRef&) { return 0; });
    CHECK(into.pads[2 * 16 + 5].colour == 0xFF0000 && into.pads[2 * 16 + 7].kit_colour == 0x2AC2FF);
}

// Pad sends, phase 4: each send is a lane of its pad (Motion records it, plays it back through the pad, the lane's card
// is SEND n of that pad), and an MPC program's per-pad Send 1-4 come in as the pad's sends.
void pad_sends_lanes() {
    PadSound pad;
    set_pad_param(pad, PadParam::SendB, 140);
    CHECK(pad.sends[1] == 100 && pad_param_value(pad, PadParam::SendB) == 100 && pad_param_value(pad, PadParam::SendA) == 0);
    CHECK(motion_pad_param(PadParam::SendD) < motion_mix_base);
    Track kit;
    kit.engine = EngineKind::Sampler;
    kit.last_pad = 5;
    kit.pads[5].sample = 0; kit.pads[5].sends = {0, 0, 45, 0};
    Binding card {Binding::Kind::PadSend, 2, 0, 100, 45};
    int lane = binding_lane(kit, card);
    CHECK(lane == motion_lane(motion_pad_param(PadParam::SendC), 5));
    CHECK(binding_lane(kit, Binding {Binding::Kind::PadSend, 0, 0, 100, 0}) == motion_lane(motion_pad_param(PadParam::SendA), 5));
    Binding back = lane_binding(kit, 1, lane);
    CHECK(back.kind == Binding::Kind::PadSend && back.id == 2 && back.value == 45);
    // The sampler plays a moved lane: a send turned up by Motion is heard on its bus.
    SampleData data;
    data.frames = 44100;
    data.left.assign(data.frames, 0.3f); data.right = data.left;
    SamplerEngine::Table table {};
    table[1] = &data;
    auto engine = std::make_unique<SamplerEngine>();
    engine->set_samples(&table);
    PadSound dry; dry.sample = 1;
    engine->set_pad(0, dry);
    std::vector<float> l(128), r(128), bl(128), br(128);
    engine->set_send_out(0, bl.data(), br.data());
    engine->move_pad(0, PadParam::SendA, 50);
    engine->note_on(int(sampler_first_note), 100);
    for (int n = 0; n < 2; ++n) {
        std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f); std::fill(bl.begin(), bl.end(), 0.0f); std::fill(br.begin(), br.end(), 0.0f);
        engine->render_stereo(l.data(), r.data(), 128);
    }
    CHECK(std::fabs(bl[100] / l[100] - 0.5f) < 1e-4f);
    // An MPC drum program's instrument: Send1 0.3 and Send3 1.0 into A and C.
    KeygroupInstrument hit;
    hit.tags = {{"Send1", "0.300000"}, {"Send2", "0.000000"}, {"Send3", "1.000000"}, {"Send4", "0.000000"}};
    hit.layers.push_back({{"Active", "True"}, {"SampleName", "Snare"}, {"VelStart", "0"}, {"VelEnd", "127"}});
    PadSound read;
    keygroup_pad(hit, 1.0, read);
    CHECK((read.sends == std::array<uint8_t, 4> {30, 0, 100, 0}));
}

// Pad sends, phase 2: Stage's SENDS on a kit is the last pad's; an empty pad has none; since the mixer (step 6) no
// Shift row (the kit's own send is the mixer's) and no SENDS on a synth; undo keeps the pad; the pads light by their
// send into the bus in view.
void pad_sends_stage() {
    using K = Binding::Kind;
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    auto& kit = project->tracks[1];
    kit.engine = EngineKind::Sampler;
    std::snprintf(project->samples.edit(0).path, sizeof(SampleRef::path), "Samples/Snare.wav");
    project->samples.edit(0).frames = 44100;
    for (unsigned n : {0u, 1u, 2u}) kit.pads[n].sample = 0;
    kit.pads[1].sends = {30, 0, 0, 0}; kit.pads[2].sends = {15, 40, 0, 0}; kit.mix.sends = {20, 0, 0, 0};
    kit.last_pad = 1;
    UiState ui;
    ui.track = 1; ui.page = PageId::Stage;
    Pattern none;
    CHECK(stage_go(ui, EngineKind::Sampler, StageKind::Sends));
    auto cards = page_bindings(*project, ui, none, false);
    CHECK(cards[1].kind == K::SendBus && cards[0].kind == K::PadSend && cards[0].id == 0 && cards[0].value == 30 && cards[2].kind == K::BusReturn);
    auto shift = shift_bindings(*project, ui, none);
    CHECK(!shift[0].bound() && shift[1].kind == K::PadOut && shift[1].value == 0 && shift[1].max == int(sub_count) && !shift[2].bound()); // Shift + K2: OUTPUT
    ui.sends_bus = 1;
    CHECK(page_bindings(*project, ui, none, false)[0].value == 0 && page_bindings(*project, ui, none, false)[0].id == 1);
    kit.last_pad = 2;
    CHECK(page_bindings(*project, ui, none, false)[0].value == 40);
    CHECK(binding_scope(*project, ui, page_bindings(*project, ui, none, false)[0]).kind == UndoScope::Kind::Pad);
    kit.last_pad = 5; // empty: nothing to send
    cards = page_bindings(*project, ui, none, false);
    CHECK(!cards[0].bound() && cards[1].bound() && cards[2].bound());
    // A synth has no SENDS (or MIX) on Stage: Stage passes over them; the mixer has its sends.
    auto& synth = project->tracks[0];
    load_sound(synth, EngineKind::Plugin, 0);
    UiState su;
    su.track = 0; su.page = PageId::Stage;
    const auto skip = stage_skip(*project, su, synth);
    CHECK((skip.kinds & stage_bit(StageKind::Sends)) && (skip.kinds & stage_bit(StageKind::Mix)));
    CHECK(!(stage_skip(*project, ui, kit).kinds & stage_bit(StageKind::Sends)) && (stage_skip(*project, ui, kit).kinds & stage_bit(StageKind::Mix)));

    // Pad lights on bus A: Snare (30) brighter than Hat (15), Kick (dry) dark, the last pad white.
    kit.last_pad = 0; ui.sends_bus = 0;
    auto host_held = std::make_unique<EngineHost>(); // on the heap: a test's frame stays under the 8 MB stack
    auto& host = *host_held;
    TrackTaps taps;
    PadFacts plays; plays.kit_pads = true;
    std::array<pad_light::Look, 16> looks;
    pad_looks(PadScene {ui, *project, *bank, host, taps, plays, 1000, nullptr, 0}, looks);
    CHECK(looks[0].hue == pad_light::Hue::White && looks[1].level > looks[2].level && looks[2].lit() && !looks[3].lit());
    kit.last_pad = 1;
    pad_looks(PadScene {ui, *project, *bank, host, taps, plays, 1000, nullptr, 0}, looks);
    CHECK(!looks[0].lit()); // Kick, dry and not selected
    ui.sends_bus = 1; // bus B: only Hat sends there
    pad_looks(PadScene {ui, *project, *bank, host, taps, plays, 1000, nullptr, 0}, looks);
    CHECK(looks[2].lit() && looks[2].hue == pad_light::Hue::Track);
}

// Free length (shell/free_length.h): an empty pattern's first take sets its length a bar at a time; with notes on
// another track in its sequence it takes theirs; K1 shows Free below 1 bar; the file keeps it; the audio thread
// counts a growing take from its own bar (Rebase).
void free_length() {
    const uint32_t bar = 4 * ticks_per_quarter, most = max_bars * bar, grace = ticks_per_quarter / 2;
    auto note = [](uint32_t tick, unsigned key) { Event e; e.tick = tick; e.key = uint8_t(key); return e; };
    {   // A new pattern is Free while it is empty; Free counts only then.
        Pattern p;
        CHECK(p.free && p.is_free());
        Pattern q = p;
        q.free = false;
        CHECK(!q.is_free() && q != p);
        p.add(note(0, 60)); q.add(note(0, 60));
        CHECK(!p.is_free() && p == q); // with notes the flag is not compared
    }
    {   // Growing: the bar playing and the next, 128 bars at most.
        CHECK(free_grown(0, bar, most) == 2 * bar && free_grown(bar - 1, bar, most) == 2 * bar);
        CHECK(free_grown(bar, bar, most) == 3 * bar && free_grown(5 * bar + 7, bar, most) == 7 * bar);
        CHECK(free_grown(most - 1, bar, most) == most);
        // Ending: the end of the bar playing; in the first eighth of a bar after the first, that bar line, wrapped.
        auto end = free_end(3 * bar + 1000, bar, grace, most); // past the first eighth of bar 4
        CHECK(end.length == 4 * bar && !end.wrap);
        end = free_end(4 * bar + 20, bar, grace, most);
        CHECK(end.length == 4 * bar && end.wrap);
        end = free_end(4 * bar + grace, bar, grace, most);
        CHECK(end.length == 5 * bar && !end.wrap);
        end = free_end(20, bar, grace, most); // the first bar: never back to nothing
        CHECK(end.length == bar && !end.wrap);
        end = free_end(most - 10, bar, grace, most);
        CHECK(end.length == most && !end.wrap);
        // Closing with a wrap: what was played past the line goes to the top, in order.
        Pattern p;
        p.free = false; p.length = 5 * bar;
        p.add(note(0, 60)); p.add(note(2 * bar, 62)); p.add(note(4 * bar + 10, 64));
        p.write_motion({4 * bar + 12, 3, 0, 50, 0}); p.write_motion({bar, 3, 0, 20, 0});
        free_close(p, free_end(4 * bar + 20, bar, grace, most));
        CHECK(p.length == 4 * bar && !p.free && p.events.size() == 3);
        CHECK(p.events[0].tick == 0 && p.events[1].tick == 10 && p.events[1].key == 64 && p.events[2].tick == 2 * bar);
        CHECK(p.motion.size() == 2 && p.motion[0].tick == 12 && p.motion[1].tick == bar);
    }
    {   // What plays with it: the longest pattern with notes another used track is on (not off), and that track.
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        synth(project->tracks[0], 0); synth(project->tracks[1], 0); synth(project->tracks[2], 0);
        for (unsigned t = 0; t < track_count; ++t) project->tracks[t].pattern = 3;
        CHECK(column_fit(*project, *bank, 0).length == 0 && column_fit(*project, *bank, 0).track == -1);
        bank->at[0][3].add(note(0, 60)); // its own notes do not count
        CHECK(column_fit(*project, *bank, 0).length == 0);
        bank->at[1][3].length = 2 * bar; bank->at[1][3].add(note(0, 60));
        bank->at[2][3].length = 4 * bar; bank->at[2][3].add(note(0, 60));
        auto fit = column_fit(*project, *bank, 0);
        CHECK(fit.length == 4 * bar && fit.track == 2);
        bank->at[5][3].length = 8 * bar; bank->at[5][3].add(note(0, 60)); // an unused track's pattern does not count
        CHECK(column_fit(*project, *bank, 0).length == 4 * bar);
        project->tracks[2].off = true; CHECK(column_fit(*project, *bank, 0).length == 2 * bar); // an off track does not count
        project->tracks[2].off = false;
        project->tracks[2].pattern = 7; bank->at[2][7].length = 6 * bar; bank->at[2][7].add(note(0, 60)); // another number counts
        CHECK(column_fit(*project, *bank, 0).length == 6 * bar && column_fit(*project, *bank, 0).track == 2);
        project->tracks[1].pattern = 4; project->tracks[2].pattern = 4;
        CHECK(column_fit(*project, *bank, 0).length == 0);
        // K1: Free below 1 bar on an empty pattern of an empty sequence; a Free pattern shows the sequence's length.
        UiState ui;
        Pattern empty;
        auto card = length_card(*project, ui, empty);
        CHECK(card.kind == Binding::Kind::PatternLength && card.min == -1 && card.value == -1 && free_shown(empty, 0) == 0);
        ui.column_ticks = 2 * bar;
        card = length_card(*project, ui, empty);
        CHECK(card.min == 0 && pattern_lengths[card.value] == 2 && free_shown(empty, ui.column_ticks) == 2 * bar);
        Pattern fixed;
        fixed.free = false; fixed.length = 4 * bar;
        ui.column_ticks = 0;
        card = length_card(*project, ui, fixed);
        CHECK(card.min == -1 && pattern_lengths[card.value] == 4 && free_offered(fixed, 0));
        fixed.add(note(0, 60));
        card = length_card(*project, ui, fixed);
        CHECK(card.min == 0 && pattern_lengths[card.value] == 4 && !free_offered(fixed, 0));
    }
    {   // The file: an empty Free pattern of a bar is not written and loads Free; an empty fixed one, a Free one
        // with its own Q, and one with notes keep what they were.
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        synth(project->tracks[0], 0);
        bank->at[0][1].free = false;                                      // empty, a bar, fixed
        bank->at[0][2].division = Division::Eighth;                       // empty, Free, its own Q
        bank->at[0][3].free = false; bank->at[0][3].length = 4 * bar;     // empty, 4 bars, fixed
        bank->at[0][4].add(note(0, 60));                             // notes
        auto bytes = save_project(*project, *bank);
        auto back = std::make_unique<Project>();
        auto back_bank = std::make_unique<PatternBank>();
        CHECK(load_project(bytes, *back, *back_bank).empty());
        CHECK(back_bank->at[0][0].is_free() && !back_bank->at[0][1].free && back_bank->at[0][1].length == bar);
        CHECK(back_bank->at[0][2].is_free() && back_bank->at[0][2].division == Division::Eighth);
        CHECK(!back_bank->at[0][3].free && back_bank->at[0][3].length == 4 * bar);
        CHECK(!back_bank->at[0][4].free && back_bank->at[0][4] == bank->at[0][4]);
        for (unsigned p = 0; p < 5; ++p) CHECK(back_bank->at[0][p] == bank->at[0][p]);
    }
    {   // The audio thread: Rebase counts a pattern from the bar playing, so a longer copy goes on from there; with
        // another track playing the same number it counts with that track.
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        synth(project->tracks[0], 0); synth(project->tracks[1], 0);
        project->tempo = 1200;
        bank->at[1][0].length = 4 * bar; put(bank->at[1][0], 0, 57, 100);
        auto host_held = std::make_unique<EngineHost>(); // on the heap: a test's frame stays under the 8 MB stack
        auto& host = *host_held;
        host.load(*project, *bank);
        tones_for(host, *project);
        host.send({Command::Type::Play});
        std::vector<int32_t> block(256);
        for (unsigned n = 0; n < 1034; ++n) host.render(block.data(), 128, 1); // 1.5 bars: tick 5760
        int before = host.position_of(0);
        CHECK(before >= 1800 && before < 2000);
        host.send({Command::Type::Rebase, 0, 0, 0xffff});
        Command grown {Command::Type::PatternData, 0, 0};
        grown.data = new Pattern;
        grown.data->free = false; grown.data->length = 2 * bar;
        host.send(grown);
        host.render(block.data(), 128, 1);
        int after = host.position_of(0);
        CHECK(after >= before && after < before + 20); // not 5760: from the bar it was in
        for (unsigned n = 0; n < 689; ++n) host.render(block.data(), 128, 1); // a bar on: into its second bar
        CHECK(host.position_of(0) >= int(bar) + before && host.position_of(0) < int(bar) + before + 30);
        CHECK(host.position_of(1) >= 2 * int(bar) + before && host.position_of(1) < 2 * int(bar) + before + 30);
        // With track 2 (the same number): it counts from where track 2's pattern began.
        Command four {Command::Type::PatternData, 0, 0};
        four.data = new Pattern;
        four.data->free = false; four.data->length = 4 * bar;
        host.send(four);
        host.send({Command::Type::Rebase, 0, 0, 1});
        host.render(block.data(), 128, 1);
        CHECK(host.position_of(0) == host.position_of(1));
        // Another number than its own: nothing changes.
        host.send({Command::Type::Rebase, 0, 5, 0xffff});
        host.render(block.data(), 128, 1);
        CHECK(host.position_of(0) == host.position_of(1));
        CHECK(host.dropped() == 0);
    }
}

// Step Edit's spare steps (shell/spare_bar.h): past the last step the caret walks into the rest of the last page, or
// the next page; a step there makes the pattern grow to the end of its bar; nothing past 128 bars; the cards and
// the selection see no step there.
void spare_steps() {
    Project project; // 4/4: a bar is 3840 ticks
    Pattern p;
    p.free = false; // a bar of sixteenths: 16 steps, a full page
    CHECK(p.steps() == 16 && spare_page_end(p) == 32 && spare_end(project, p) == 32);
    CHECK(spare_length(project, p, 15) == 0 && spare_length(project, p, 16) == 7680 && spare_length(project, p, 31) == 7680);
    Pattern eighths = p;
    eighths.division = Division::Eighth; // 8 steps: the rest of the page is spare, a step there makes two bars
    CHECK(eighths.steps() == 8 && spare_end(project, eighths) == 16 && spare_length(project, eighths, 8) == 7680);
    Pattern full = p;
    full.length = project.max_length(); // 128 bars: nothing to grow into
    CHECK(spare_length(project, full, full.steps()) == 0 && spare_end(project, full) == full.steps());
    Pattern almost = p;
    almost.length = project.max_length() - 3840;
    CHECK(spare_length(project, almost, almost.steps()) == project.max_length());
    // The caret: a spare step while step_cursor marks the last step on Step Edit.
    UiState ui;
    ui.page = PageId::StepEdit; ui.step_cursor = 15; ui.spare_step = 16;
    CHECK(in_spare(ui, p) && edit_caret(ui, p) == 16 && spare_mark(project, ui, p));
    ui.step_cursor = 14; CHECK(!in_spare(ui, p) && edit_caret(ui, p) == 14); // moved by anything else: no longer spare
    ui.step_cursor = 15; ui.page = PageId::Roll; CHECK(!in_spare(ui, p));
    ui.page = PageId::StepEdit; ui.spare_step = 32; CHECK(!in_spare(ui, p)); // past the spare page
    ui.spare_step = -1; CHECK(spare_mark(project, ui, p)); // on the last page
    Pattern two = p;
    two.length = 7680;
    ui.step_cursor = 3; CHECK(!spare_mark(project, ui, two)); // the first of two pages
    ui.step_cursor = 17; CHECK(spare_mark(project, ui, two) && !spare_mark(project, ui, full));
    // A step in the spare steps holds nothing: no selection, no step cards (only what a pad writes).
    Event e; e.tick = 15 * 240; e.key = 60;
    p.add(e);
    ui.step_cursor = 15; ui.spare_step = -1;
    CHECK(selected_event(p, ui) == 0);
    ui.spare_step = 20;
    CHECK(selected_event(p, ui) == -1);
    ui.step_pads = true;
    auto cards = page_bindings(project, ui, p, false);
    CHECK(cards[0].kind == Binding::Kind::None && cards[1].kind == Binding::Kind::None);
    ui.step_pads = false;
    cards = page_bindings(project, ui, p, false);
    CHECK(cards[0].kind == Binding::Kind::EntryLength);
    ui.step_cards = 1; cards = page_bindings(project, ui, p, false); // LOCK
    CHECK(cards[0].kind == Binding::Kind::None);
}

// The usage log (shell/usage_log.h): off until started; page changes with the time on the page left, one nojob for
// a knob's run of steps, undo with the time since the last edit and only its first word, lesson steps, MENU picks,
// and the lines handed over every 10 s or at 4 KB. The setting is kept with the others.
void sample_lists() {
    std::vector<std::string> starred;
    CHECK(star_toggle(starred, "/media/card/Melodee/Samples/Kick.wav"));
    CHECK(star_toggle(starred, "/content/Samples/Drums/Snare.wav"));
    CHECK(!star_toggle(starred, "/media/card/Melodee/Samples/Kick.wav")); // again: no longer starred
    CHECK(starred.size() == 1 && starred[0] == "/content/Samples/Drums/Snare.wav");
    // A part of a file (a program's pad, a SoundFont's preset) by its '#'; a '#' in a file's or a folder's own name is
    // no part: the factory's "G#m90" phrases star and delete as files.
    std::string whole;
    CHECK(part_of_file("/media/card/Kits/Kit.xpm#3", &whole) && whole == "/media/card/Kits/Kit.xpm");
    CHECK(part_of_file("/content/Projects/Kits/Kalim.XPJ#k2") && part_of_file("/media/card/Kit.xty#1.4"));
    CHECK(part_of_file("/media/card/Melodee/Samples/Piano.sf2#12", &whole) && whole == "/media/card/Melodee/Samples/Piano.sf2");
    CHECK(!part_of_file("/content/Samples/Phrases/KeysPh-Kalim1 G#m90 MP.WAV"));
    CHECK(!part_of_file("/media/card/Melodee/Samples/C# Loops/Strings.wav") && !part_of_file("/media/card/Melodee/Samples/Kit.xpm#"));
    whole = "unchanged";
    CHECK(!part_of_file("/media/card/F#m.wav", &whole) && whole == "unchanged");
    std::vector<std::string> recent;
    for (int n = 0; n < 20; ++n) recent_add(recent, "/media/card/Melodee/Samples/" + std::to_string(n) + ".wav");
    CHECK(recent.size() == recent_max && recent.front() == "/media/card/Melodee/Samples/19.wav" && recent.back() == "/media/card/Melodee/Samples/4.wav");
    recent_add(recent, "/media/card/Melodee/Samples/10.wav"); // loaded again: first, once
    CHECK(recent.size() == recent_max && recent.front() == "/media/card/Melodee/Samples/10.wav"
          && std::count(recent.begin(), recent.end(), "/media/card/Melodee/Samples/10.wav") == 1);
    list_remove(recent, "/media/card/Melodee/Samples/10.wav");
    CHECK(recent.size() == recent_max - 1 && recent.front() == "/media/card/Melodee/Samples/19.wav");
    // Kept on the card: a card's file from the card's top, an internal one whole; read back as it was.
    const std::vector<std::string> kept {"/media/card/Melodee/Samples/Loops/Bass Loop.wav", "/content/Samples/Drums/Kick.wav"};
    const std::string text = list_text(kept, "/media/card");
    CHECK(text == "Melodee/Samples/Loops/Bass Loop.wav\n/content/Samples/Drums/Kick.wav\n");
    CHECK(list_read(text, "/media/card", 100) == kept);
    CHECK(list_read(text, "/media/other", 100)[0] == "/media/other/Melodee/Samples/Loops/Bass Loop.wav"); // another mount point
    CHECK(list_read("a.wav\r\n\n a\na.wav\n", "/media/card", 100) == std::vector<std::string>({"/media/card/a.wav", "/media/card/ a"}));
    CHECK(list_read("x.wav\n", "", 100).empty()); // no card: a card's path means nothing
    CHECK(list_read("a.wav\nb.wav\nc.wav\n", "/c", 2).size() == 2);
    CHECK(list_where("/media/card/Melodee/Samples/Loops/Bass Loop.wav") == "Loops");
    CHECK(list_where("/media/card/A folder name too long/x.wav").empty()); // longer than fits beside a name
    CHECK(list_folder(starred_list) && list_folder(recent_list) && !list_folder("/media/card/STARRED"));
}

void history_list() {
    // Shift + Minus: a tap undoes on its release; held history_hold_ms, the list opens once.
    HistoryHold h;
    history_press(h, 1000);
    CHECK(!history_tick(h, 1000 + history_hold_ms - 1));
    CHECK(history_release(h)); // a tap
    history_press(h, 2000);
    CHECK(history_tick(h, 2000 + history_hold_ms) && !history_tick(h, 2000 + 2 * history_hold_ms));
    CHECK(!history_release(h)); // held: no undo on the release
    CHECK(!history_release(h));
    // Rows: 2 undone, 3 done, START. NOW is row 2.
    CHECK(history_rows(2, 3) == 6 && history_now(2) == 2);
    CHECK(history_step(2, 2) == 0 && history_step(0, 2) == 2 && history_step(1, 2) == 1);
    CHECK(history_step(3, 2) == -1 && history_step(5, 2) == -3); // START: every step done undone
    CHECK(history_row_dim(1, 3) && !history_row_dim(3, 3) && !history_row_dim(4, 3));
    bool redo = false;
    unsigned index = 9;
    CHECK(history_edit_at(0, 2, 3, redo, index) && redo && index == 0); // the newest step undone
    CHECK(history_edit_at(2, 2, 3, redo, index) && !redo && index == 2); // NOW: the newest step done
    CHECK(history_edit_at(4, 2, 3, redo, index) && !redo && index == 0); // the oldest
    CHECK(!history_edit_at(5, 2, 3, redo, index)); // START
    CHECK(history_age(100000, 50000) == "now" && history_age(200000, 50000) == "2 min" && history_age(8000000, 0) == "2 h");
    // The History: each edit's time, the two lists in the list's order.
    uint32_t clock = 5000;
    History edits;
    edits.clock = &clock;
    for (const char* what : {"A", "B", "C"}) { Edit e; e.what = what; edits.push(std::move(e)); clock += 1000; }
    CHECK(edits.size() == 3 && edits.kept(2).what == "C" && edits.kept(2).at == 7000);
    edits.undo(); edits.undo(); // C, then B
    CHECK(edits.redos() == 2 && edits.undone(0).what == "C" && edits.undone(1).what == "B" && edits.size() == 1);
    // Rows top down: C, B (undone), A (NOW), START.
    CHECK(history_edit_at(0, 2, 1, redo, index) && edits.undone(index).what == "C");
    CHECK(history_edit_at(2, 2, 1, redo, index) && !redo && edits.kept(index).what == "A");
    Edit pad; pad.pad = 4;
    CHECK(history_kind(pad) == HistoryKind::Pad);
    Edit pattern; pattern.pattern = 2; pattern.pad = 4;
    CHECK(history_kind(pattern) == HistoryKind::Pattern);
}

void usage_log() {
    UsageLog log;
    log.line(5, "nothing");
    log.page(5, PageId::Stage);
    CHECK(!log.on() && !log.due(100000) && log.take(0).empty());
    log.start(1000, 1763400000, "1.2.3", true);
    CHECK(log.on() && log.file() == "usage-1763400000.txt");
    log.page(1000, PageId::Stage);          // the first page: no line yet, the clock starts
    log.page(3500, PageId::Chop);           // 2.5 s on Stage
    log.page(3600, PageId::Chop);           // the same page: nothing
    log.nojob(4000, "K2", PageId::Chop);
    log.nojob(4500, "K2", PageId::Chop);    // the same knob turning on: one line
    log.nojob(6400, "K2", PageId::Chop);    // still turning (each step within 2 s of the last)
    log.nojob(9500, "B2", PageId::Chop);
    log.edited(10000);
    log.undo(10850, false, "Clear pattern 2", PageId::Patterns);
    log.undo(11000, true, "Replace Surge XT", PageId::Stage); // its first word only
    log.undo(11100, false, "+3 dB", PageId::Stage);          // no word: Edit
    log.menu_go(12000, 3);
    log.menu_here(12100, "CAPTURE");
    log.lesson(13000, 1, 0);
    log.lesson(17200, 1, 1);
    log.lesson(17300, 0, 0); // left
    CHECK(log.due(11000 + UsageLog::flush_ms));
    std::string text = log.take(18000);
    const char* want =
        "0.000 start 1.2.3 Full\n"
        "2.500 page Chop Stage 2500\n"
        "3.000 nojob K2 Chop\n"
        "8.500 nojob B2 Chop\n"
        "9.850 undo 850 Clear Patterns\n"
        "10.000 redo 1000 Replace Stage\n"
        "10.100 undo 1100 Edit Stage\n"
        "11.000 menu GO 4\n"
        "11.100 menu HERE CAPTURE\n"
        "16.200 lesson 1 1 4200\n";
    if (text != want) std::printf("%s", text.c_str());
    CHECK(text == want);
    CHECK(!log.due(18001) && log.take(18001).empty());
    log.nojob(19000, "Wheel", PageId::Stage);
    CHECK(!log.due(20000) && log.due(18001 + UsageLog::flush_ms));
    log.stop();
    log.nojob(30000, "Wheel", PageId::Roll);
    CHECK(!log.on() && log.take(30000) == "18.000 nojob Wheel Stage\n");
    CHECK(std::string(usage_page_name(PageId::Stems)) == "Stems" && std::string(usage_page_name(PageId::Count)) == "None");

    Settings fresh;
    CHECK(!fresh.usage_log);
    Settings on; on.usage_log = true;
    Settings back; back.read(on.text());
    CHECK(back.usage_log);
    std::puts("usage log ok");
}

// The preview's controls audit (shell/controls_rules.h): BACK on B1 only, send cards in SEND / BUS / RETURN order
// (not Motion's lanes), the SEQ and MIX hubs' rows kept unless something is drawn over them.
void controls_audit() {
    auto row = [](PageId page, std::array<std::string, 3> keys, std::array<std::string, 3> knobs = {}) {
        UiState ui; ui.page = page;
        ControlsRow r = controls_row(ui);
        r.keys = std::move(keys); r.knobs = std::move(knobs);
        return r;
    };
    CHECK(controls_breaks(row(PageId::SettingsPage, {"BACK", "", "OPEN"})).empty());
    auto b3 = controls_breaks(row(PageId::SettingsPage, {"", "", "BACK"}));
    CHECK(b3.size() == 1 && b3[0] == "back: BACK on B3");
    auto icon = row(PageId::Samples, {"", "OK", ""}); icon.back[1] = true;
    CHECK(controls_breaks(icon).size() == 1);
    CHECK(controls_breaks(row(PageId::Looper, {"BACK", "", "CANCEL"})).empty()); // cancel a queued stop: a job, not a way out

    CHECK(controls_breaks(row(PageId::Stage, {"TRIM", "TUNE", "SENDS"}, {"SEND A", "BUS", "RETURN"})).empty());
    auto sends = controls_breaks(row(PageId::Stage, {"", "", ""}, {"BUS", "SEND A", "RETURN"}));
    CHECK(sends.size() == 1 && sends[0] == "sends: BUS / SEND A / RETURN");
    CHECK(controls_breaks(row(PageId::Motion, {"SEQUENCES", "ROLL", "MOTION"}, {"RETURN", "SEND A", ""})).empty()); // lanes
    CHECK(controls_breaks(row(PageId::Stage, {"", "", ""}, {"SENDING", "BUS", ""})).empty()); // SysEx going out, not a send

    CHECK(controls_breaks(row(PageId::Roll, {"SEQUENCES", "ROLL", ""})).empty()); // Basic: no MOTION
    CHECK(controls_breaks(row(PageId::Patterns, {"PATTERNS", "ROLL", "MOTION"})).empty());
    CHECK(controls_breaks(row(PageId::ModPage, {"SEQUENCES", "ROLL", "MACRO"})).empty());
    auto mod = controls_breaks(row(PageId::ModPage, {"MOD", "MACRO", "ADD"}));
    CHECK(mod.size() == 1 && mod[0] == "seq: MOD / MACRO / ADD");
    CHECK(controls_breaks(row(PageId::Rack, {"MIXER", "", "FX"})).empty());
    CHECK(controls_breaks(row(PageId::Bus, {"MIXER", "SENDS", "FX"})).size() == 1);
    // Over the row: Shift held, a sheet; a loop's chain; a frame with no key row.
    UiState ui; ui.page = PageId::ModPage; ui.shift = true;
    auto shifted = controls_row(ui); shifted.keys = {"", "", "ADD"};
    CHECK(shifted.overlay && controls_breaks(shifted).empty());
    ui = UiState {}; ui.page = PageId::Rack; ui.fx_loop = 1;
    auto loop = controls_row(ui); loop.keys = {"LOOPER", "L2", ""};
    CHECK(loop.loop_chain && controls_breaks(loop).empty());
    auto bare = row(PageId::Rack, {"", "", ""}); bare.keys_drawn = false;
    CHECK(controls_breaks(bare).empty());
    ui = UiState {}; ui.page = PageId::Patterns; ui.sheet_open = true;
    CHECK(controls_overlay(ui));
    std::puts("controls audit ok");
}

// Settings' USB volume: Full (the default) gives USB-C the mix at full main volume beside the jacks' block at the
// knob's level; Knob leaves USB-C on the jacks' block. Kept in the settings file.
void usb_volume() {
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    for (auto& t : project->tracks) t.engine = EngineKind::None;
    auto host = std::make_unique<EngineHost>();
    host->load(*project, *bank);
    std::vector<int32_t> input(256), out(256), fixed(256);
    for (unsigned f = 0; f < 128; ++f) { input[2 * f] = int32_t(0.5 * 2147483647.0); input[2 * f + 1] = int32_t(0.25 * 2147483647.0); }
    host->send({Command::Type::RecordSource, uint8_t(InputSource::Rear)});
    host->send({Command::Type::Monitor, 0, 0, 0, 1});
    for (unsigned n = 0; n < 8; ++n) host->render(out.data(), 128, 0.25f, input.data(), nullptr, nullptr, fixed.data());
    const float centre = std::cos(dsp::pi / 4);
    auto at = [](const std::vector<int32_t>& v, unsigned i) { return v[i] / 2147483647.0f; };
    CHECK(std::fabs(at(out, 254) - 0.5f * centre * 0.25f) < 1e-3f && std::fabs(at(out, 255) - 0.25f * centre * 0.25f) < 1e-3f);
    CHECK(std::fabs(at(fixed, 254) - 0.5f * centre) < 1e-3f && std::fabs(at(fixed, 255) - 0.25f * centre) < 1e-3f);
    for (unsigned n = 0; n < 8; ++n) host->render(out.data(), 128, 0, input.data(), nullptr, nullptr, fixed.data());
    CHECK(out[254] == 0 && std::fabs(at(fixed, 254) - 0.5f * centre) < 1e-3f); // the knob down: USB-C still hears it

    Settings fresh;
    CHECK(fresh.usb_full && fresh.value_text(Settings::UsbVolume) == "Full" && std::string(Settings::row_name(Settings::UsbVolume)) == "USB volume");
    CHECK(fresh.choices(Settings::UsbVolume) == 2 && fresh.choice(Settings::UsbVolume) == 1);
    Settings knob = fresh;
    knob.step(Settings::UsbVolume, 1);
    CHECK(!knob.usb_full && knob.value_text(Settings::UsbVolume) == "Knob" && knob.choice(Settings::UsbVolume) == 0);
    Settings back;
    back.read(knob.text());
    CHECK(!back.usb_full);
    back.set_choice(Settings::UsbVolume, 1);
    CHECK(back.usb_full);
    Settings old; old.read("rec_quantize=0\n"); // a file from before the row: Full
    CHECK(old.usb_full);
    knob.reset_factory();
    CHECK(knob.usb_full);
}

// Settings' USB-C in Direct (base's drivers/direct_monitor.h): what a computer plays goes straight to the jacks, just
// ahead of the codec's DMA, and this mix leaves it out wherever it would have played it.
void direct_monitoring() {
    Settings fresh;
    CHECK(fresh.usb_direct && fresh.value_text(Settings::UsbDirect) == "Direct" && std::string(Settings::row_name(Settings::UsbDirect)) == "USB-C in");
    CHECK(fresh.value_text(Settings::DirectLevel) == "0 dB" && std::fabs(fresh.direct_gain() - 1.0f) < 1e-6f &&
          std::string(Settings::row_name(Settings::DirectLevel)) == "Direct level");
    Settings changed = fresh;
    changed.step(Settings::UsbDirect, 1);
    changed.step(Settings::DirectLevel, -1);
    CHECK(!changed.usb_direct && changed.value_text(Settings::UsbDirect) == "Tracks" && changed.value_text(Settings::DirectLevel) == "-3 dB");
    changed.set_choice(Settings::DirectLevel, 8);
    CHECK(changed.value_text(Settings::DirectLevel) == "+6 dB" && std::fabs(changed.direct_gain() - 1.9953f) < 1e-3f);
    Settings back; back.read(changed.text());
    CHECK(!back.usb_direct && back.direct_level == 8);
    Settings old; old.read("usb_volume=1\n"); // a file from before the rows: Direct at 0 dB
    CHECK(old.usb_direct && old.direct_level == 6);

    // The monitor: each period goes `ahead` frames in front of the DMA, added to what the app wrote there, at the gain.
    {
        DirectMonitor m;
        std::vector<int32_t> buffer(2 * 384, int32_t(0.25 * 2147483648.0));
        std::vector<float> period(2 * 64, 0.5f);
        auto now = [] { return int64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); };
        m.attach(buffer.data(), 384, 48000);
        m.at(384, 0, now(), true);
        m.add(period.data(), 64); // off: nothing
        CHECK(m.added() == 0 && buffer[2 * DirectMonitor::ahead] == int32_t(0.25 * 2147483648.0));
        m.set(true, 0.5f);
        m.at(384, 0, now(), true);
        m.add(period.data(), 64);
        auto value = [&](unsigned frame) { return buffer[2 * frame] / 2147483648.0; };
        unsigned lead = m.lead();
        CHECK(m.added() == 64 && lead >= DirectMonitor::ahead && lead < DirectMonitor::ahead + 4);
        // Faded in from silence (5 ms), on top of what the app wrote; the frames before as it wrote them.
        unsigned first = 384;
        for (unsigned f = 0; f < 384; ++f) if (value(f) > 0.25 + 1e-6) { first = f; break; }
        CHECK(first >= DirectMonitor::ahead && first < DirectMonitor::ahead + 4);
        CHECK(value(first) < 0.252 && value(first + 63) > value(first + 32) && value(first + 32) > value(first) && value(first + 63) < 0.5);
        // The next period follows on; the DMA a period further on, the pace kept: no slip, no nudge.
        m.at(448, 64, now(), true);
        m.add(period.data(), 64);
        CHECK(m.slips() == 0 && m.nudges() == 0 && m.added() == 128);
        // Settled (40 ms on): at the gain, 0.25 + 0.5 x 0.5, each side its own.
        {
            DirectMonitor s;
            std::vector<int32_t> ring(2 * 384, int32_t(0.25 * 2147483648.0));
            s.attach(ring.data(), 384, 48000);
            s.set(true, 0.5f, 0.25f);
            for (unsigned k = 0; k < 30; ++k) {
                if (k == 29) std::fill(ring.begin(), ring.end(), int32_t(0.25 * 2147483648.0));
                s.at(384 + 64 * k, 64 * k, now(), true);
                s.add(period.data(), 64);
            }
            unsigned at = unsigned((s.added() - 64 + DirectMonitor::ahead) % 384);
            for (unsigned f = at; f < at + 64; ++f) {
                CHECK(std::fabs(ring[2 * (f % 384)] / 2147483648.0 - 0.5) < 2e-3);
                CHECK(std::fabs(ring[2 * (f % 384) + 1] / 2147483648.0 - 0.375) < 2e-3);
            }
            // Off: it fades out, not cut; the first period off starts at about the gain and ends lower.
            s.set(false, 0.5f, 0.25f);
            std::fill(ring.begin(), ring.end(), int32_t(0.25 * 2147483648.0));
            s.at(384 + 64 * 30, 64 * 30, now(), true);
            s.add(period.data(), 64);
            at = unsigned((s.added() - 64 + DirectMonitor::ahead) % 384);
            const double fade_from = ring[2 * at] / 2147483648.0, fade_to = ring[2 * ((at + 63) % 384)] / 2147483648.0;
            CHECK(fade_from < 0.5 && fade_from > 0.49 && fade_to < fade_from - 0.04 && fade_to > 0.4);
            // Then, silent (under 1e-5 of the gain: from 0.5 on a 5 ms ease, 41 periods, 55 ms), it leaves the buffer alone.
            for (unsigned k = 31; k < 90; ++k) { s.at(384 + 64 * k, 64 * k, now(), true); s.add(period.data(), 64); }
            std::fill(ring.begin(), ring.end(), int32_t(0.25 * 2147483648.0));
            s.at(384 + 64 * 90, 64 * 90, now(), true);
            s.add(period.data(), 64);
            for (auto v : ring) CHECK(v == int32_t(0.25 * 2147483648.0));
            s.detach();
        }
        // The computer a little behind (the DMA 20 frames further than it sent): a frame repeated, not a slip.
        m.at(512, 148, now(), true);
        m.add(period.data(), 64);
        CHECK(m.slips() == 0 && m.nudges() == 1 && m.lead() < DirectMonitor::ahead - DirectMonitor::nudge && m.lead() > DirectMonitor::min_ahead);
        // The DMA far ahead (the computer behind): placed afresh, a slip.
        m.at(1000, 700, now(), true);
        m.add(period.data(), 64);
        CHECK(m.slips() == 1 && m.lead() >= DirectMonitor::ahead && m.lead() < DirectMonitor::ahead + 4);
        // Never past what the app has written: those frames would be written over.
        m.restart();
        m.at(100, 0, now(), true);
        m.add(period.data(), 64);
        CHECK(m.short_writes() == 1);
        // Waiting to start, or detached: nothing.
        uint64_t added = m.added();
        m.at(384, 0, now(), false);
        m.add(period.data(), 64);
        m.at(384, 0, now(), true);
        m.detach();
        m.add(period.data(), 64);
        CHECK(m.added() == added && !m.attached());
        // Loud on loud: clipped, not wrapped.
        std::vector<int32_t> full(2 * 384, INT32_MAX - 10);
        m.attach(full.data(), 384, 48000);
        m.at(384, 0, now(), true);
        m.add(period.data(), 64);
        unsigned clipped = 0;
        for (auto v : full) { CHECK(v > 0); clipped += v == INT32_MAX; }
        CHECK(clipped == 2 * 64);
        m.detach();
    }

    // The engine: Input Config's Monitor on a USB-C source, and a MIDI track's AUDIO input USB, are left out of the mix
    // with Direct on, and said to be heard (usb_heard()); with it off they play here as before.
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    for (auto& t : project->tracks) t.engine = EngineKind::None;
    auto host = std::make_unique<EngineHost>();
    host->load(*project, *bank);
    std::vector<int32_t> out(256);
    std::vector<float> usb(256, 0.5f);
    auto loudest = [&] { int32_t m = 0; for (auto v : out) m = std::max(m, std::abs(v)); return m / 2147483647.0f; };
    host->send({Command::Type::RecordSource, uint8_t(InputSource::Usb)});
    host->send({Command::Type::Monitor, 0, 0, 0, 1});
    for (unsigned n = 0; n < 8; ++n) host->render(out.data(), 128, 1.0f, nullptr, nullptr, usb.data());
    const float monitored = loudest();
    CHECK(monitored > 0.3f && !host->usb_heard());
    host->set_usb_direct(true);
    for (unsigned n = 0; n < 8; ++n) host->render(out.data(), 128, 1.0f, nullptr, nullptr, usb.data());
    CHECK(loudest() < 1e-4f && host->usb_heard());
    // Heard direct at the level Monitor gave it (its input gain at a centred pad's pan law).
    CHECK(std::fabs(0.5f * host->usb_gain_left() - monitored) < 2e-3f && std::fabs(host->usb_gain_right() - host->usb_gain_left()) < 1e-6f);
    host->send({Command::Type::Monitor, 0, 0, 0, 0});
    for (unsigned n = 0; n < 8; ++n) host->render(out.data(), 128, 1.0f, nullptr, nullptr, usb.data());
    CHECK(!host->usb_heard()); // nothing would have played it: not heard direct either
    // A MIDI track hearing USB-C in through its AUDIO page.
    auto midi = std::make_unique<Project>();
    for (auto& t : midi->tracks) t.engine = EngineKind::None;
    load_sound(midi->tracks[0], EngineKind::Midi, 0);
    auto host2 = std::make_unique<EngineHost>();
    host2->load(*midi, *bank);
    host2->send({Command::Type::MidiAudio, 0, 0, 0, midi_track::AudioUsb});
    for (unsigned n = 0; n < 8; ++n) host2->render(out.data(), 128, 1.0f, nullptr, nullptr, usb.data());
    float through = loudest();
    host2->set_usb_direct(true);
    for (unsigned n = 0; n < 8; ++n) host2->render(out.data(), 128, 1.0f, nullptr, nullptr, usb.data());
    CHECK(through > 0.05f && loudest() < 1e-4f && host2->usb_heard());
    CHECK(std::fabs(0.5f * host2->usb_gain_left() - through) < 2e-3f); // at the level the track gave it
    host2->send({Command::Type::Pan, 0, 0, 0, -50}); // hard left: the right side silent, as through the track
    for (unsigned n = 0; n < 8; ++n) host2->render(out.data(), 128, 1.0f, nullptr, nullptr, usb.data());
    CHECK(host2->usb_gain_left() > 0.05f && host2->usb_gain_right() < 1e-3f);
    host2->send({Command::Type::Mute, 0, 0, 0, 1}); // muted: not heard either way
    for (unsigned n = 0; n < 8; ++n) host2->render(out.data(), 128, 1.0f, nullptr, nullptr, usb.data());
    CHECK(!host2->usb_heard());
}

void pad_value_erase() {
    // Erase + pot on a kit pad's own values: back to a new pad's value, as an engine's parameter goes back to its initial.
    using Kind = Binding::Kind;
    auto project = std::make_unique<Project>();
    auto& kit = project->tracks[3];
    load_sound(kit, EngineKind::Sampler, 0);
    auto& pad = kit.pads[8];
    pad.sample = 0; kit.last_pad = 8;
    pad.pitch = 7; pad.fine = -20; pad.level = 30; pad.pan = 40; pad.warp = 150; pad.attack = 60; pad.decay = 10;
    pad.velocity_sense = 5; pad.cutoff = 12; pad.resonance = 70; pad.filter_attack = 30; pad.filter_decay = 90; pad.filter_amount = 80;
    pad.cloud_position = 90; pad.cloud_size = 1; pad.cloud_density = 99; pad.sends[1] = 77; pad.loop_xfade = 30;
    const PadSound fresh {};
    UiState ui;
    ui.track = 3;
    Pattern none;
    // Every card with a knob range on Tune, Mix, Amp Env, Filter, Filt Env, Cloud and the Pads page; Filter's type and
    // Cloud on or off are choices, which Erase leaves alone.
    int resets = 0;
    for (PageId page : {PageId::PadPage, PageId::PadMix, PageId::EnvelopePage, PageId::PadFilterPage, PageId::FilterEnvPage,
                        PageId::CloudPage, PageId::Pads}) {
        ui.page = page;
        for (bool row2 : {false, true}) {
            ui.sampler_row = row2;
            for (const auto& b : page_bindings(*project, ui, none, false)) {
                if (!b.bound()) continue;
                int value = -999;
                const bool choice = b.kind == Kind::PadFilterType || (b.kind == Kind::PadCloud && b.id == 0);
                CHECK(pad_reset_value(b, value) == !choice && erase_resets(b) == !choice);
                if (choice) continue;
                CHECK(value >= b.min && value <= b.max);
                ++resets;
            }
        }
    }
    CHECK(resets >= 20);
    int value = -999;
    CHECK(pad_reset_value(Binding {Kind::PadPitch, 0, -24, 24, pad.pitch}, value) && value == fresh.pitch && value == 0);
    CHECK(pad_reset_value(Binding {Kind::PadLevel, 0, 0, 127, pad.level}, value) && value == fresh.level);
    CHECK(pad_reset_value(Binding {Kind::PadCutoff, 0, 0, 100, pad.cutoff}, value) && value == fresh.cutoff);
    CHECK(pad_reset_value(Binding {Kind::PadSend, 1, 0, 100, pad.sends[1]}, value) && value == fresh.sends[1]);
    CHECK(pad_reset_value(Binding {Kind::PadCloud, 2, 0, 100, pad.cloud_size}, value) && value == fresh.cloud_size);
    CHECK(pad_reset_value(Binding {Kind::PadWarp, 0, 0, warp_card_seq, warp_card(pad.warp)}, value) && warp_of_card(value) == fresh.warp);
    CHECK(pad_reset_value(Binding {Kind::PadLoopXfade, 0, 0, int(max_loop_xfade), pad.loop_xfade}, value) && value == fresh.loop_xfade);
    // Trim's handles, toggles and choices: no value to go back to.
    for (Kind kind : {Kind::PadStart, Kind::PadEnd, Kind::PadLoopPoint, Kind::PadLoop, Kind::PadReverse, Kind::PadMono, Kind::PadPlayMode,
                      Kind::PadGroup, Kind::PadLink, Kind::PadBeats, Kind::PadColour, Kind::PadDecayFrom, Kind::Level, Kind::Pan})
        CHECK(!pad_reset_value(Binding {kind, 0, 0, 16, 1}, value) && !erase_resets(Binding {kind, 0, 0, 16, 1}));
    CHECK(erase_resets(Binding {Kind::Param, 0, 0, 127, 1}) && erase_resets(Binding {Kind::KnobParam, 0, 0, 127, 1})
          && erase_resets(Binding {Kind::PluginParam, 0, 0, 127, 1}));
    // Seq Record still finds each pad value's lane on the last pad.
    CHECK(binding_lane(kit, Binding {Kind::PadSend, 1, 0, 100, 0}) == motion_lane(motion_pad_param(PadParam::SendB), 8));
    CHECK(binding_lane(kit, Binding {Kind::PadCloud, 2, 0, 100, 0}) == motion_lane(motion_pad_param(PadParam::CloudSize), 8));
}

void audio_monitor_choice() {
    // An audio track's Monitor (shell/audio_monitor.h): Auto heard while Seq Record arms it or it records, On all the
    // time and also while a track that is not an audio track is selected, Off never; one input at a time.
    auto project = std::make_unique<Project>();
    project->tracks[0].engine = EngineKind::Sampler;
    project->tracks[2].engine = EngineKind::Audio; project->tracks[2].audio.monitor = TrackMonitor::Auto;
    project->tracks[4].engine = EngineKind::Audio; project->tracks[4].audio.monitor = TrackMonitor::On;
    project->tracks[5].engine = EngineKind::Audio; project->tracks[5].audio.monitor = TrackMonitor::On;
    auto pick = [&](int take, unsigned selected, bool armed, bool record) { return audio_monitor_pick(*project, take, selected, armed, record); };
    CHECK(AudioTrackSettings {}.monitor == TrackMonitor::Auto && std::string(track_monitor_name(TrackMonitor::On)) == "On");
    auto p = pick(-1, 2, false, false);
    CHECK(p.track == 2 && !p.heard);                  // Auto, not armed: its source, not heard
    p = pick(-1, 2, true, false);
    CHECK(p.track == 2 && p.heard);                   // armed
    p = pick(2, 0, false, false);
    CHECK(p.track == 2 && p.heard);                   // its take waits or runs, whatever is selected
    p = pick(-1, 4, false, false);
    CHECK(p.track == 4 && p.heard);                   // On: stopped, not armed
    p = pick(-1, 0, false, false);
    CHECK(p.track == 4 && p.heard);                   // a kit selected: the first audio track set to On
    p = pick(-1, 0, false, true);
    CHECK(p.track == -1 && !p.heard);                 // the Record page: Input Config's
    p = pick(-1, 7, false, false);
    CHECK(p.track == 4);                              // an empty track selected too
    project->tracks[4].audio.monitor = TrackMonitor::Off;
    p = pick(-1, 4, true, false);
    CHECK(p.track == 4 && !p.heard);                  // Off: never, armed or not
    p = pick(-1, 0, false, false);
    CHECK(p.track == 5 && p.heard);
    project->tracks[5].audio.monitor = TrackMonitor::Auto;
    p = pick(-1, 0, true, false);
    CHECK(p.track == -1 && !p.heard);                 // no audio track set to On

    // Saved as a number: Auto is what older builds called On (1); On is new (2) and kept.
    auto bank = std::make_unique<PatternBank>();
    project->tracks[2].audio.monitor = TrackMonitor::On;
    auto back = std::make_unique<Project>();
    CHECK(load_project(save_project(*project, *bank), *back, *bank).empty());
    CHECK(back->tracks[2].audio.monitor == TrackMonitor::On && back->tracks[5].audio.monitor == TrackMonitor::Auto);
    CHECK(unsigned(TrackMonitor::Auto) == 1);
}

void stems_lookup() {
    // The stem separator: in the card folder the app was copied from (sd-folder beside its copy in RAM), a boot
    // menu's as a release's, else the fixed folders. Nowhere else on the card.
    namespace fs = std::filesystem;
    const fs::path card = fs::temp_directory_path() / ("melodee-stems-card-" + std::to_string(::getpid()));
    const fs::path ram = fs::temp_directory_path() / ("melodee-stems-ram-" + std::to_string(::getpid()));
    std::error_code error;
    fs::remove_all(card, error);
    fs::remove_all(ram, error);
    auto put = [&](const fs::path& folder) {
        fs::create_directories(folder / "stems");
        std::ofstream(folder / "stems" / stems_program_name) << "x";
        return (folder / "stems" / stems_program_name).string();
    };
    fs::create_directories(card / "Melodee/Samples");
    fs::create_directories(ram);
    CHECK(find_stems_program("").empty() && find_stems_program(card.string()).empty());
    CHECK(release_sd_folder(ram).empty()); // started by device.py start: no sd-folder
    std::ofstream(ram / "sd-folder") << "relative/melodee\n";
    CHECK(release_sd_folder(ram).empty()); // only a whole path
    const fs::path mod = card / "autorun/mods/Melodee 0.5.1/melodee"; // SampleBoot, a space in the name
    std::ofstream(ram / "sd-folder") << mod.string() << "\n";
    CHECK(release_sd_folder(ram) == mod.string());
    put(card / "autorun/mods/Melodee 0.3.0/melodee"); // another version beside it: never searched for
    CHECK(find_stems_program(card.string(), release_sd_folder(ram)).empty());
    const auto installed = put(card / "sample-base"); // an install from before
    CHECK(find_stems_program(card.string(), release_sd_folder(ram)) == installed);
    const auto dev = put(card / "autorun/sample-base");
    CHECK(find_stems_program(card.string()) == dev);
    const auto release = put(card / "autorun/melodee");
    CHECK(find_stems_program(card.string()) == release);
    const auto own = put(mod); // the folder it was started from before the fixed ones
    CHECK(find_stems_program(card.string(), release_sd_folder(ram)) == own);
    CHECK(find_stems_program("", release_sd_folder(ram)) == own);
    fs::remove_all(card, error);
    fs::remove_all(ram, error);
}

// An MPC Sample project's hits played on Chop's pads (stock 1.3.0.12, Kerem's "Pokemom"): pad 13 (note 48) holds
// "Sample 001008-Normalized.wav", 987776 frames at 44.1 kHz, its region 1790..987775 (sliceIndex 129), and the WAV's
// "atem" 16 slices. Its eight notes each have the Sample Slice modifier on ("Absolute"), its value the slice / 127.
// Before, each came in as a plain note of pad 13 and played the whole 22 s sample.
void mpc_chop_hits() {
    // Each note's modifierActiveState15 and modifierValue15, as the file has them (the doubles the MPC wrote).
    const double saved[8] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.007874015718698502, 0.0, 0.015748031437397003};
    // The WAV's "Slice 0".."Slice 15" Start and End.
    const uint32_t atem[16][2] = {{1790, 61759}, {61759, 113386}, {113386, 165540}, {165540, 248286}, {248286, 329958},
                                  {329958, 360078}, {360078, 431368}, {431368, 493708}, {493708, 523828}, {523828, 605858},
                                  {605858, 657458}, {657458, 739488}, {739488, 821160}, {821160, 851638}, {851638, 903238},
                                  {903238, 985269}};
    Slices chop;
    for (const auto& s : atem) { chop.start[chop.count] = s[0]; chop.stop[chop.count] = s[1]; ++chop.count; }
    PadSound pad;
    pad.sample = 0; pad.start = 1790; pad.end = 987775; pad.play = PadPlay::OneShot;
    const uint8_t key = uint8_t(sampler_first_note + 12);
    Pattern p;
    p.length = 8 * ticks_per_quarter;
    for (unsigned n = 0; n < 8; ++n) {
        Event e;
        e.tick = n * ticks_per_quarter; // one a beat, as recorded (960 MPC ticks apart)
        e.key = key;
        e.slice = mpc_slice_hit(true, saved[n], false);
        CHECK(p.add(e));
    }
    const uint8_t slices[8] = {1, 1, 1, 1, 1, 2, 1, 3}; // Slice 1, 1, 1, 1, 1, 2, 1, 3 on the MPC's screen
    CHECK(p.events.size() == 8);
    for (unsigned n = 0; n < 8; ++n) {
        const auto& e = p.events[n];
        CHECK(e.slice == slices[n] && unsigned(e.slice - 1) < chop.count);
        // What the sampler plays for it (SamplerEngine::hit): the pad over that slice, not 1790..987775.
        const auto played = slice_sound(pad, chop.start[size_t(e.slice - 1)], chop.stop[size_t(e.slice - 1)], 0);
        CHECK(played.start == atem[e.slice - 1][0] && played.end == atem[e.slice - 1][1]);
        CHECK(note_slice(pad_note(int(e.key), 0, -1, e.slice - 1)) == e.slice - 1);
    }
    // A note with the modifier off plays its pad, whatever value it keeps (the MPC writes one for every note).
    CHECK(mpc_slice_hit(false, 0.0, false) == 0 && mpc_slice_hit(false, 0.5, false) == 0);
    // Relative modifiers offset the pad's slice, and a pad playing its own region has none: its pad.
    CHECK(mpc_slice_hit(true, 0.015748031437397003, true) == 0);
    // The MPC's 128 slices, all of them a chop's: the last is slice 128.
    CHECK(mpc_slice_hit(true, 1.0, false) == max_slices && mpc_slice_hit(true, 63.0 / 127, false) == 64);
    CHECK(mpc_slice_hit(true, -0.1, false) == 0 && mpc_slice_hit(true, std::nan(""), false) == 0);
}

// Sequence links (model/scene_links.h): a pick kept in the sequence in hand fills what the others are on; a sequence
// keeping nothing plays on; off; the file keeps them, a file from before them plays its columns; prints do not see
// them; the engine's off waits for the bar and holds through Play.
void sequence_links() {
    {
        auto p = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        synth(p->tracks[0], 0); synth(p->tracks[1], 0); synth(p->tracks[2], 0); // track 4 unused
        CHECK(!scene_kept(*p, 0) && scene_target(p->tracks[0], 0) == scene_plays_on);
        // The first pick in SEQ 1: every used track keeps what it is on (P1), the picked one P3.
        CHECK(keep_pick(*p, 0, 1, 2, 0));
        CHECK(scene_kept(*p, 0) && p->tracks[0].in_scene[0] == 1 && p->tracks[1].in_scene[0] == 3 && p->tracks[2].in_scene[0] == 1);
        CHECK(p->tracks[3].in_scene[0] == 0 && scene_target(p->tracks[1], 0) == 2);
        // A second pick changes that track alone.
        p->tracks[0].pattern = 7;
        CHECK(!keep_pick(*p, 0, 2, 4, 0));
        CHECK(p->tracks[0].in_scene[0] == 1 && p->tracks[2].in_scene[0] == 5);
        // SEQ 2: a track off and one a follow action stopped are kept off.
        p->tracks[0].off = true;
        CHECK(keep_pick(*p, 1, 1, 6, 1u << 2));
        CHECK(p->tracks[0].in_scene[1] == scene_off && p->tracks[2].in_scene[1] == scene_off && p->tracks[1].in_scene[1] == 7);
        CHECK(scene_target(p->tracks[0], 1) == scene_stops && scene_now(*p, 0, 0) == scene_stops && scene_now(*p, 1, 0) == 0);
        // Off picked: kept off.
        p->tracks[0].off = false;
        keep_pick(*p, 0, 0, scene_stops, 0);
        CHECK(scene_target(p->tracks[0], 0) == scene_stops);
        // Exports: the kept pattern, nothing where off, pattern n where nothing is kept.
        CHECK(export_pattern(*p, 1, 0) == 2 && export_pattern(*p, 0, 0) == -1 && export_pattern(*p, 0, 5) == 5);
        // Which sequences keep a pattern.
        unsigned out[2] {};
        CHECK(scenes_keeping(p->tracks[1], 2, out, 2) == 1 && out[0] == 0);
        p->tracks[1].in_scene[9] = 3; p->tracks[1].in_scene[20] = 3;
        CHECK(scenes_keeping(p->tracks[1], 2, out, 2) == 3 && out[0] == 0 && out[1] == 9);
        // How many play notes there.
        put(bank->at[1][2], 0, 60, 100);
        CHECK(scene_with(*p, *bank, 0) == 1 && scene_with(*p, *bank, 3) == 0);
        // CAPTURE keeps what every used track is on; ERASE keeps nothing.
        p->tracks[2].pattern = 11;
        CHECK(keep_all(*p, 6, 0) == 3 && p->tracks[0].in_scene[6] == 8 && p->tracks[2].in_scene[6] == 12 && p->tracks[3].in_scene[6] == 0);
        forget_scene(*p, 6);
        CHECK(!scene_kept(*p, 6));
    }
    {   // Before links (and from the MPC): a scene with notes on a used track's pattern n, or a name, tempo or colour,
        // keeps pattern n on every used track; the rest keep nothing.
        auto p = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        synth(p->tracks[0], 0); synth(p->tracks[1], 0);
        put(bank->at[0][2], 0, 60, 100);
        p->scenes[4].set_name("BREAK");
        put(bank->at[5][7], 0, 60, 100); // an unused track's notes do not count
        keep_columns(*p, *bank);
        CHECK(p->tracks[0].in_scene[2] == 3 && p->tracks[1].in_scene[2] == 3 && p->tracks[5].in_scene[2] == 0);
        CHECK(p->tracks[0].in_scene[4] == 5 && p->tracks[1].in_scene[4] == 5);
        CHECK(!scene_kept(*p, 3) && !scene_kept(*p, 7));
    }
    {   // The file: kept patterns, off and the sequence in hand come back; a file without SEQL loads its columns.
        auto p = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        synth(p->tracks[0], 0); synth(p->tracks[1], 0);
        put(bank->at[0][0], 0, 60, 100); put(bank->at[1][3], 0, 60, 100);
        p->tracks[0].in_scene[2] = 1; p->tracks[1].in_scene[2] = 4; p->tracks[1].in_scene[5] = scene_off;
        p->tracks[1].off = true; p->scene = 5;
        auto bytes = save_project(*p, *bank);
        auto back = std::make_unique<Project>();
        auto back_bank = std::make_unique<PatternBank>();
        CHECK(load_project(bytes, *back, *back_bank).empty());
        CHECK(back->scene == 5 && back->tracks[1].off && !back->tracks[0].off);
        CHECK(back->tracks[0].in_scene == p->tracks[0].in_scene && back->tracks[1].in_scene == p->tracks[1].in_scene);
        CHECK(!scene_kept(*back, 0) && !scene_kept(*back, 3)); // new sequences stay new: no columns
        // Without SEQL (an older file): its columns, scene 1 (notes on track 1) and 4 (notes on track 2).
        std::vector<uint8_t> old(bytes.begin(), bytes.begin() + 8);
        for (size_t at = 8; at + 8 <= bytes.size();) {
            size_t size = size_t(bytes[at + 4]) | size_t(bytes[at + 5]) << 8 | size_t(bytes[at + 6]) << 16 | size_t(bytes[at + 7]) << 24;
            if (std::memcmp(bytes.data() + at, "SEQL", 4) != 0) old.insert(old.end(), bytes.begin() + long(at), bytes.begin() + long(at + 8 + size));
            at += 8 + size;
        }
        CHECK(old.size() < bytes.size());
        CHECK(load_project(old, *back, *back_bank).empty());
        CHECK(back->scene == 0 && !back->tracks[1].off);
        CHECK(back->tracks[0].in_scene[0] == 1 && back->tracks[1].in_scene[0] == 1 && back->tracks[0].in_scene[3] == 4);
        CHECK(!scene_kept(*back, 2) && !scene_kept(*back, 5));
        // A print's hash does not see them (prints made before links stay good).
        std::array<std::vector<uint8_t>, max_plugins> states;
        const uint64_t with = print_hash(*p, *bank, states, PrintTap::PostFx, PrintTail::Loop, 48000);
        for (auto& t : p->tracks) { t.in_scene = {}; t.off = false; }
        p->scene = 0;
        CHECK(print_hash(*p, *bank, states, PrintTap::PostFx, PrintTail::Loop, 48000) == with);
    }
    {   // Mutes kept per sequence: a pick and CAPTURE keep the mutes as they are; a mute by hand is kept in the
        // sequence in hand (filling a new one as a pick does); a launch sets the mutes of the tracks it keeps.
        auto p = std::make_unique<Project>();
        synth(p->tracks[0], 0); synth(p->tracks[1], 0); synth(p->tracks[2], 0); // track 4 unused
        p->tracks[1].mix.mute = true;
        CHECK(keep_pick(*p, 0, 0, 3, 0));
        CHECK(!scene_muted(p->tracks[0], 0) && scene_muted(p->tracks[1], 0) && !scene_muted(p->tracks[2], 0));
        // A mute in a new sequence: every used track keeps what it is on, the muted one its mute.
        p->tracks[2].mix.mute = true;
        CHECK(keep_mute(*p, 1, 2, true, 0));
        CHECK(scene_kept(*p, 1) && p->tracks[0].in_scene[1] == 1 && p->tracks[2].in_scene[1] == 1 && p->tracks[3].in_scene[1] == 0);
        CHECK(scene_muted(p->tracks[1], 1) && scene_muted(p->tracks[2], 1) && !scene_muted(p->tracks[0], 1));
        // In a kept one: the mute alone, the patterns kept stay.
        p->tracks[0].pattern = 9;
        CHECK(!keep_mute(*p, 0, 0, true, 0) && scene_muted(p->tracks[0], 0) && p->tracks[0].in_scene[0] == 4);
        CHECK(!keep_mute(*p, 0, 1, false, 0) && !scene_muted(p->tracks[1], 0));
        // What a launch sets: the tracks the sequence keeps, and those muted.
        auto m = scene_mutes_of(*p, 0);
        CHECK(m.set == 0b111 && m.muted == 0b001);
        CHECK(scene_mutes_of(*p, 5).set == 0); // a new sequence sets none: every mute stays
        p->tracks[2].in_scene[0] = scene_off; // a track kept off has its mute set too
        CHECK(scene_mutes_of(*p, 0).set == 0b111);
        // Scenes far apart keep their own bits.
        CHECK(keep_mute(*p, 100, 1, true, 0) && scene_muted(p->tracks[1], 100) && !scene_muted(p->tracks[1], 36));
        // CAPTURE keeps the mutes now; ERASE forgets them.
        p->tracks[0].mix.mute = false; p->tracks[1].mix.mute = true; p->tracks[2].mix.mute = false;
        keep_all(*p, 0, 0);
        CHECK(!scene_muted(p->tracks[0], 0) && scene_muted(p->tracks[1], 0) && !scene_muted(p->tracks[2], 0));
        forget_scene(*p, 0);
        CHECK(!scene_muted(p->tracks[1], 0) && scene_mutes_of(*p, 0).set == 0);
        // The file: the mutes come back; a file from before (no SEQL) keeps each track's mute as saved.
        auto bank = std::make_unique<PatternBank>();
        keep_all(*p, 3, 0);
        p->tracks[1].mix.mute = false;
        auto bytes = save_project(*p, *bank);
        auto back = std::make_unique<Project>();
        auto back_bank = std::make_unique<PatternBank>();
        CHECK(load_project(bytes, *back, *back_bank).empty());
        for (unsigned t = 0; t < track_count; ++t) CHECK(back->tracks[t].scene_mutes == p->tracks[t].scene_mutes);
        CHECK(scene_muted(back->tracks[1], 3) && scene_muted(back->tracks[2], 1) && !back->tracks[1].mix.mute);
        put(bank->at[0][1], 0, 60, 100); // a column with notes: kept in an old file
        p->tracks[2].mix.mute = true;
        auto old_bytes = save_project(*p, *bank);
        std::vector<uint8_t> older(old_bytes.begin(), old_bytes.begin() + 8);
        for (size_t at = 8; at + 8 <= old_bytes.size();) {
            size_t size = size_t(old_bytes[at + 4]) | size_t(old_bytes[at + 5]) << 8 | size_t(old_bytes[at + 6]) << 16 | size_t(old_bytes[at + 7]) << 24;
            if (std::memcmp(old_bytes.data() + at, "SEQL", 4) != 0) older.insert(older.end(), old_bytes.begin() + long(at), old_bytes.begin() + long(at + 8 + size));
            at += 8 + size;
        }
        CHECK(load_project(older, *back, *back_bank).empty());
        CHECK(back->tracks[2].in_scene[1] == 2 && scene_muted(back->tracks[2], 1) && !scene_muted(back->tracks[0], 1));
        CHECK(!scene_muted(back->tracks[2], 5)); // a scene that keeps nothing keeps no mute
        // A print's hash does not see them.
        std::array<std::vector<uint8_t>, max_plugins> states;
        const uint64_t with = print_hash(*p, *bank, states, PrintTap::PostFx, PrintTail::Loop, 48000);
        for (auto& t : p->tracks) t.scene_mutes = {};
        CHECK(print_hash(*p, *bank, states, PrintTap::PostFx, PrintTail::Loop, 48000) == with);
    }
    {   // A sequence copied (Shift + Erase on Sequences): its name, tempo, colour and mutes, and each kept pattern a copy
        // in the track's next free pattern; off and nothing kept as they are; no free pattern: the same one, shared.
        auto p = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        synth(p->tracks[0], 0); synth(p->tracks[1], 0); synth(p->tracks[2], 0); synth(p->tracks[3], 0);
        put(bank->at[0][0], 0, 60, 100); put(bank->at[1][4], 0, 62, 100);
        p->tracks[0].in_scene[0] = 1; p->tracks[1].in_scene[0] = 5; p->tracks[2].in_scene[0] = scene_off; // track 4 plays on
        set_scene_muted(p->tracks[1], 0, true);
        p->scenes[0].set_name("VERSE"); p->scenes[0].tempo = 1000; p->scenes[0].colour = 3;
        put(bank->at[0][1], 0, 60, 100);   // track 1's P2 has notes: not free
        p->tracks[0].in_scene[7] = 3;      // its P3 is kept in SEQ 8: not free
        p->tracks[0].pattern = 3;          // and it is on P4: not free
        bank->at[0][4].follow.action = 1;  // P5 has a follow action: not free
        Clip clip; clip.track = 0; clip.pattern = 5; clip.bar = 0; clip.bars = 1;
        CHECK(p->song.place(clip));         // P6 plays in the song: not free
        CHECK(free_pattern(*p, *bank, 0, 0, 1) == 6);
        CHECK(free_pattern(*p, *bank, 0, 0, 7) == 2); // the scene copied onto may give up its own
        CHECK(free_pattern(*p, *bank, 1, 4, 1) == 5);
        std::vector<PatternCopy> copies;
        CHECK(copy_scene_links(*p, *bank, 0, 1, copies) == 0);
        CHECK(copies.size() == 2);
        CHECK(copies[0].track == 0 && copies[0].from == 0 && copies[0].into == 6);
        CHECK(copies[1].track == 1 && copies[1].from == 4 && copies[1].into == 5);
        CHECK(p->tracks[0].in_scene[1] == 7 && p->tracks[1].in_scene[1] == 6);
        CHECK(p->tracks[2].in_scene[1] == scene_off && p->tracks[3].in_scene[1] == 0);
        CHECK(scene_muted(p->tracks[1], 1) && !scene_muted(p->tracks[0], 1));
        CHECK(std::strcmp(p->scenes[1].name, "VERSE") == 0 && p->scenes[1].tempo == 1000 && p->scenes[1].colour == 3);
        CHECK(p->tracks[0].in_scene[0] == 1 && p->tracks[1].in_scene[0] == 5); // the source as it was
        // Onto itself: nothing.
        copies.clear();
        CHECK(copy_scene_links(*p, *bank, 1, 1, copies) == 0 && copies.empty());
        // No free pattern on a track: it keeps the same pattern in both.
        for (unsigned n = 0; n < pattern_count; ++n) if (bank->at[1][n].empty()) put(bank->at[1][n], 0, 64, 100);
        copies.clear();
        CHECK(copy_scene_links(*p, *bank, 0, 2, copies) == 1);
        CHECK(p->tracks[1].in_scene[2] == 5 && copies.size() == 1 && copies[0].track == 0);
    }
    {   // The engine: a scene's mutes on the bar its patterns launch on; stopped, at once; a mute by hand meanwhile wins.
        using namespace midi_track;
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        load_sound(project->tracks[0], EngineKind::Midi, 0);
        project->tracks[0].params[Port] = UsbC; project->tracks[0].params[Channel] = 1;
        project->tempo = 1200;
        put(bank->at[0][0], 0, 60, 100); put(bank->at[0][0], 8, 60, 100); // two notes a bar
        auto host = std::make_unique<EngineHost>();
        host->load(*project, *bank);
        tones_for(*host, *project);
        host->set_audio_format(44100, 384);
        std::vector<int32_t> buffer(256);
        auto play = [&](unsigned blocks) {
            unsigned ons = 0;
            for (unsigned b = 0; b < blocks; ++b) {
                host->render(buffer.data(), 128, 1);
                MidiOutEvent e;
                while (host->take_midi(e)) ons += e.kind == MidiOutEvent::Kind::NoteOn;
            }
            return ons;
        };
        const unsigned bar = 690; // blocks of 128 frames in a bar at 120 BPM
        host->send({Command::Type::Play});
        CHECK(play(bar / 4) == 1);
        host->send({Command::Type::SceneMutes, 0, 0, 1, 1}); // muted from the next bar: the rest of this one plays
        CHECK(play(bar / 2) == 1);
        CHECK(play(bar) == 0);
        host->send({Command::Type::SceneMutes, 0, 0, 1, 0}); // back on at the next bar
        CHECK(play(bar) <= 2);
        CHECK(play(bar) == 2);
        host->send({Command::Type::SceneMutes, 0, 0, 1, 1});
        host->send({Command::Type::Mute, 0, 0, 0, 0}); // the hand's after it: it stays on
        CHECK(play(2 * bar) >= 3);
        host->send({Command::Type::Stop});
        play(10);
        host->send({Command::Type::SceneMutes, 0, 0, 1, 1}); // stopped: at once
        host->send({Command::Type::Play});
        CHECK(play(bar) == 0);
        host->send({Command::Type::SceneMutes, 0, 0, 1, 0});
        host->send({Command::Type::Stop}); // one waiting for its bar when the transport stops: set then
        play(10);
        host->send({Command::Type::Play});
        CHECK(play(bar) >= 2); // the bar's two (the next bar's first may fall in the last block)
        // Issue 40: a mute with a grid (Rec quantize's Q) waits for its next step while playing, the note on that
        // step left out; pressed back before it, nothing waits; Stop sets what waits.
        host->send({Command::Type::Stop});
        play(10);
        host->send({Command::Type::Play});
        CHECK(play(bar / 4) == 1); // the first note, at tick 0
        Command mute {Command::Type::Mute, 0, 0, 0, 1};
        mute.position = 2 * ticks_per_quarter; // half a bar
        host->send(mute);
        play(1);
        CHECK(host->mutes_waiting() == 1);
        CHECK(play(bar / 4 - 20) == 0 && host->mutes_waiting() == 1); // still before the half: waiting, nothing to play
        CHECK(play(bar / 2) == 0 && host->mutes_waiting() == 0);      // on the half: muted before its note
        Command back {Command::Type::Mute, 0, 0, 0, 0};
        back.position = 4 * ticks_per_quarter; // the bar
        host->send(back);
        play(1);
        CHECK(host->mutes_waiting() == 1);
        back.value = 1; host->send(back); // pressed back: as it is, nothing waits
        play(1);
        CHECK(host->mutes_waiting() == 0);
        back.value = 0; host->send(back);
        play(1);
        CHECK(host->mutes_waiting() == 1);
        host->send({Command::Type::Stop}); // set on Stop
        play(10);
        CHECK(host->mutes_waiting() == 0);
        host->send({Command::Type::Play});
        CHECK(play(bar) >= 2);
        // A kit pad's mute waits the same way, a bit a pad.
        Command pad {Command::Type::PadMute, 0, 0, 3, 1};
        pad.position = ticks_per_quarter;
        host->send(pad);
        play(1);
        CHECK(host->pad_mutes_waiting(0, 0) == 8u && host->mutes_waiting() == 0);
        play(bar / 4 + 10);
        CHECK(host->pad_mutes_waiting(0, 0) == 0);
    }
    {   // Issue 59: a MIDI track plays from the first bar after Play, Locate or MTC, however far on it stopped, with a
        // negative DELAY (read ahead) or none.
        using namespace midi_track;
        for (int delay : {0, -50}) {
            auto project = std::make_unique<Project>();
            auto bank = std::make_unique<PatternBank>();
            load_sound(project->tracks[0], EngineKind::Midi, 0);
            project->tracks[0].params[Port] = UsbC; project->tracks[0].params[Channel] = 1;
            project->tracks[0].params[Delay] = delay;
            project->tempo = 1200;
            put(bank->at[0][0], 0, 60, 100); put(bank->at[0][0], 8, 60, 100); // two notes a bar
            auto host = std::make_unique<EngineHost>();
            host->load(*project, *bank);
            tones_for(*host, *project);
            host->set_audio_format(44100, 384);
            std::vector<int32_t> buffer(256);
            auto play = [&](unsigned blocks) {
                unsigned ons = 0;
                for (unsigned b = 0; b < blocks; ++b) {
                    host->render(buffer.data(), 128, 1);
                    MidiOutEvent e;
                    while (host->take_midi(e)) ons += e.kind == MidiOutEvent::Kind::NoteOn;
                }
                return ons;
            };
            const unsigned bar = 690; // blocks of 128 frames in a bar at 120 BPM
            host->send({Command::Type::Play});
            const unsigned before = play(5 * bar + bar / 2); // eleven (twelve when the read-ahead reaches the next note)
            CHECK(before >= 11 && before <= 12);
            host->send({Command::Type::Stop});
            play(10);
            host->send({Command::Type::Play}); // from the top: its first bar plays
            CHECK(play(bar) >= 2);
            play(3 * bar);
            Command back {Command::Type::Locate}; // back to the first bar while playing
            back.position = 0;
            host->send(back);
            CHECK(play(bar) >= 2);
            play(3 * bar);
            host->send({Command::Type::Stop});
            play(10);
            Command from {Command::Type::Play}; // Shift + Play: from a place before where it stopped
            from.position = 4 * ticks_per_quarter;
            host->send(from);
            CHECK(play(bar) >= 2);
        }
    }
    {   // The engine: off stops the track on the next bar, holds through Stop and Play, and a launch ends it.
        using namespace midi_track;
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        load_sound(project->tracks[0], EngineKind::Midi, 0);
        project->tracks[0].params[Port] = UsbC; project->tracks[0].params[Channel] = 1;
        project->tempo = 1200;
        put(bank->at[0][0], 0, 60, 100); put(bank->at[0][0], 8, 60, 100); // two notes a bar
        auto host = std::make_unique<EngineHost>();
        host->load(*project, *bank);
        tones_for(*host, *project);
        host->set_audio_format(44100, 384);
        std::vector<int32_t> buffer(256);
        auto play = [&](unsigned blocks) { // the notes on in these blocks
            unsigned ons = 0;
            for (unsigned b = 0; b < blocks; ++b) {
                host->render(buffer.data(), 128, 1);
                MidiOutEvent e;
                while (host->take_midi(e)) ons += e.kind == MidiOutEvent::Kind::NoteOn;
            }
            return ons;
        };
        const unsigned bar = 690; // blocks of 128 frames in a bar at 120 BPM (88200 frames)
        host->send({Command::Type::Play});
        CHECK(play(bar / 4) == 1);
        host->send({Command::Type::Pattern, 0, 0, 0, -1}); // off: the rest of this bar still plays
        CHECK(play(bar / 2) == 1 && !(host->stopped_tracks() & 1));
        CHECK(play(bar) == 0 && (host->stopped_tracks() & 1) && host->position_of(0) < 0);
        host->send({Command::Type::Stop});
        play(10);
        host->send({Command::Type::Play});
        CHECK(play(bar) == 0 && (host->stopped_tracks() & 1)); // still off after Play (a follow action's STOP is not)
        host->send({Command::Type::Pattern, 0, 0, 0, 0}); // a launch: back on the next bar
        CHECK(play(2 * bar) >= 2 && !(host->stopped_tracks() & 1));
        // Off while stopped, and off as the project has it: silent from Play.
        host->send({Command::Type::Stop});
        play(10);
        host->send({Command::Type::Pattern, 0, 0, 0, -1});
        host->send({Command::Type::Play});
        CHECK(play(bar) == 0);
        host->send({Command::Type::Stop});
        play(10);
        project->tracks[0].off = true;
        host->load(*project, *bank);
        host->send({Command::Type::Play});
        CHECK(play(bar) == 0 && (host->stopped_tracks() & 1));
        // A launch waiting for its bar when the transport stops ends off too.
        host->send({Command::Type::Pattern, 0, 0, 0, 0});
        host->send({Command::Type::Stop});
        play(10);
        host->send({Command::Type::Play});
        CHECK(play(bar) >= 1 && !(host->stopped_tracks() & 1));
    }
}

// An MPC song as Song (model/song_steps.h): the steps end to end, a clip a step on each track with notes in its
// sequence's pattern, a marker a step, and the tempo lane where the sequences' tempos change.
void song_steps_mpc() {
    auto setup = [](Project& p, PatternBank& bank) {
        synth(p.tracks[0], 0); synth(p.tracks[1], 0); synth(p.tracks[2], 0); // track 3 has no notes anywhere
        put(bank.at[0][0], 0, 60, 100); put(bank.at[0][1], 0, 60, 100);
        put(bank.at[1][1], 0, 60, 100); put(bank.at[1][2], 0, 60, 100);
        p.scenes[1].set_name("Verse");
        p.tempo = 900;
        p.pattern_tempo[0] = 900; p.pattern_tempo[1] = 900; p.pattern_tempo[2] = 960;
        keep_columns(p, bank);
    };
    const std::vector<SongStep> steps {{0, 1, 4}, {1, 1, 8}, {2, 2, 8}, {1, 1, 8}};
    {
        auto p = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        setup(*p, *bank);
        p->song.markers[0].bar = 99; p->song.marker_count = 1; // whatever Song held goes
        CHECK(song_from_steps(*p, *bank, steps, true) == 0);
        const auto& song = p->song;
        CHECK(song.count == 6 && song.length() == 36 && !song.looping());
        CHECK(song.clip_at(0, 0) >= 0 && song.clip_at(1, 0) < 0 && song.clip_at(2, 0) < 0);   // SEQ 1: track 1 alone
        const auto& verse = song.clips[size_t(song.clip_at(1, 4))];
        CHECK(verse.pattern == 1 && verse.bar == 4 && verse.bars == 8 && song.clip_at(0, 4) >= 0);
        const auto& twice = song.clips[size_t(song.clip_at(1, 12))]; // two repeats: one clip of 16 bars
        CHECK(twice.pattern == 2 && twice.bar == 12 && twice.bars == 16 && song.clip_at(0, 12) < 0 && song.clip_at(1, 27) >= 0);
        CHECK(song.clip_at(0, 28) >= 0 && song.clip_at(1, 35) >= 0 && song.clip_at(1, 36) < 0);
        CHECK(song.marker_count == 4 && !std::strcmp(song.markers[0].name, "SEQ 1") && song.markers[1].bar == 4);
        CHECK(!std::strcmp(song.markers[1].name, "Verse") && !std::strcmp(song.markers[2].name, "SEQ 3") && song.markers[3].bar == 28);
        CHECK(song.tempo.count == 3 && song.tempo.points[0].bar == 0 && song.tempo.points[0].tempo == 900);
        CHECK(song.tempo.points[1].bar == 12 && song.tempo.points[1].tempo == 960 && !song.tempo.points[1].ramp);
        CHECK(song.tempo.points[2].bar == 28 && song.tempo.points[2].tempo == 900);
        CHECK(int(song.tempo.at(14.0, p->tempo)) == 960);
    }
    {   // GBL tempo or Ignore Tempo: no lane. Every sequence at the project's tempo: none either.
        auto p = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        setup(*p, *bank);
        song_from_steps(*p, *bank, steps, false);
        CHECK(p->song.tempo.empty() && p->song.count == 6);
        p->pattern_tempo[2] = 900;
        song_from_steps(*p, *bank, steps, true);
        CHECK(p->song.tempo.empty());
    }
    {   // A track the sequence keeps off gets no clip; one it keeps another pattern for plays that.
        auto p = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        setup(*p, *bank);
        p->tracks[0].in_scene[1] = scene_off;
        p->tracks[1].in_scene[0] = 3; // P3 (pattern 2)
        song_from_steps(*p, *bank, steps, false);
        CHECK(p->song.clip_at(0, 4) < 0 && p->song.clip_at(1, 0) >= 0 && p->song.clips[size_t(p->song.clip_at(1, 0))].pattern == 2);
    }
    {   // Song's room: past 999 bars, or 256 clips, the rest is left out; markers stop at 16.
        auto p = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        setup(*p, *bank);
        CHECK(song_from_steps(*p, *bank, {{1, 1, 128}, {1, 7, 128}, {0, 1, 4}}, false) == 2 && p->song.length() == 128);
        std::vector<SongStep> many(200, SongStep {1, 1, 1});
        CHECK(song_from_steps(*p, *bank, many, false) == 72);
        CHECK(p->song.count == song_clips && p->song.length() == 128 && p->song.marker_count == song_markers);
        CHECK(song_from_steps(*p, *bank, {}, true) == 0 && !p->song.count && !p->song.marker_count);
    }
}

// Jam capture (model/jam_log.h): the bars logged outside Song laid after the song's end, a clip a run of the same
// pattern (muted where not heard), a marker where a sequence took over, the tempo lane where the tempo changed.
void jam_capture() {
    auto setup = [](Project& p, PatternBank& bank) {
        synth(p.tracks[0], 0); synth(p.tracks[1], 0); synth(p.tracks[2], 0);
        put(bank.at[0][0], 0, 60, 100); put(bank.at[0][1], 0, 60, 100); put(bank.at[1][0], 0, 60, 100);
        p.scenes[1].set_name("Drop");
        p.tempo = 1200;
    };
    auto bar = [](std::initializer_list<int> patterns, uint16_t muted = 0, uint16_t tempo = 1200, int16_t scene = -1) {
        JamBar b;
        unsigned t = 0;
        for (int v : patterns) b.pattern[t++] = v < 0 ? jam_silent : uint8_t(v);
        b.muted = muted; b.tempo = tempo; b.scene = scene;
        return b;
    };
    {
        auto p = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        setup(*p, *bank);
        p->song.place({0, 0, 0, 4, false}); // the song so far: four bars
        JamLog log;
        // T1 P1 two bars, then P2 three (muted in the last), T2 P1 silent in bar 3, T3 on an empty pattern throughout
        log.bars = {bar({0, 0, 2}, 0, 1200, 0), bar({0, 0, 2}), bar({1, -1, 2}, 0, 1400, 1), bar({1, 0, 2}, 0, 1400),
                    bar({1, 0, 2}, 1, 1400)};
        const auto took = jam_to_song(*p, *bank, log);
        const auto& song = p->song;
        CHECK(took.at == 4 && took.bars == 5 && took.left_out == 0 && took.clips == 5 && song.count == 6 && song.length() == 9);
        const auto& first = song.clips[size_t(song.clip_at(0, 4))];
        CHECK(first.pattern == 0 && first.bars == 2 && !first.muted);
        const auto& second = song.clips[size_t(song.clip_at(0, 6))];
        CHECK(second.pattern == 1 && second.bar == 6 && second.bars == 2 && !second.muted);
        CHECK(song.clips[size_t(song.clip_at(0, 8))].muted && song.clips[size_t(song.clip_at(0, 8))].bars == 1);
        CHECK(song.clip_at(1, 4) >= 0 && song.clip_at(1, 6) < 0 && song.clips[size_t(song.clip_at(1, 7))].bars == 2);
        CHECK(song.clip_at(2, 4) < 0 && song.clip_at(2, 8) < 0); // an empty pattern: no clip
        CHECK(song.marker_count == 2 && song.markers[0].bar == 4 && !std::strcmp(song.markers[0].name, "SEQ 1"));
        CHECK(song.markers[1].bar == 6 && !std::strcmp(song.markers[1].name, "Drop"));
        CHECK(song.tempo.count == 1 && song.tempo.points[0].bar == 6 && song.tempo.points[0].tempo == 1400);
        // Taken again: after it, the tempo stepping back first.
        const auto again = jam_to_song(*p, *bank, log);
        CHECK(again.at == 9 && again.bars == 5 && p->song.length() == 14);
        CHECK(p->song.tempo.count == 3 && p->song.tempo.points[1].bar == 9 && p->song.tempo.points[1].tempo == 1200);
        CHECK(p->song.tempo.points[2].bar == 11 && p->song.tempo.points[2].tempo == 1400);
    }
    {   // Song's room: past 999 bars, or 256 clips, the rest is left out.
        auto p = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        setup(*p, *bank);
        p->song.place({2, 0, 0, 995, false});
        JamLog log;
        log.bars.assign(10, bar({0, 0, -1}));
        auto took = jam_to_song(*p, *bank, log);
        CHECK(took.at == 995 && took.bars == 4 && took.left_out == 6 && p->song.length() == 999);
        CHECK(jam_to_song(*p, *bank, log).bars == 0);
        p->song = Song {};
        log.bars.clear();
        for (unsigned n = 0; n < 200; ++n) log.bars.push_back(bar({int(n % 2), 0, -1}, uint16_t(n % 2 ? 2 : 0))); // a clip a bar on two lanes
        took = jam_to_song(*p, *bank, log);
        CHECK(took.clips == song_clips && took.bars == 128 && took.left_out == 72 && p->song.length() == 128);
        CHECK(jam_to_song(*p, *bank, log).bars == 0 && JamLog {}.bars.empty());
    }
}

// The jam list (model/jam_log.h): each run kept as a jam of its own (not under two bars, the newest eight), part of a
// jam taken, and the list in a project file's JAMS section.
void jam_list() {
    auto run = [](unsigned bars, uint8_t pattern) {
        JamLog log;
        for (unsigned n = 0; n < bars; ++n) { JamBar b; b.pattern[0] = pattern; b.tempo = 1200; log.bars.push_back(b); }
        return log;
    };
    JamList list;
    CHECK(!list.keep(run(1, 0)) && list.jams.empty()); // a PLAY and STOP by mistake
    CHECK(list.keep(run(4, 0)) && list.jams.size() == 1 && list.jams[0].number == 1 && list.next == 2);
    for (unsigned n = 0; n < 9; ++n) CHECK(list.keep(run(2 + n, 1)));
    CHECK(list.jams.size() == jams_kept && list.jams.front().number == 3 && list.jams.back().number == 10 && list.jams.back().bars.size() == 10);
    CHECK(list.erase(0) && list.jams.size() == jams_kept - 1 && list.jams.front().number == 4 && !list.erase(99));
    {   // Bars 3-6 of a jam: what played there, from the song's end.
        auto p = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        synth(p->tracks[0], 0);
        put(bank->at[0][0], 0, 60, 100); put(bank->at[0][1], 0, 60, 100);
        p->tempo = 1200;
        JamLog jam = run(8, 0);
        for (unsigned n = 4; n < 8; ++n) jam.bars[n].pattern[0] = 1;
        jam.bars[4].scene = 2;
        const auto took = jam_to_song(*p, *bank, jam, 2, 6);
        CHECK(took.at == 0 && took.bars == 4 && took.left_out == 0 && p->song.count == 2 && p->song.length() == 4);
        CHECK(p->song.clips[size_t(p->song.clip_at(0, 0))].pattern == 0 && p->song.clips[size_t(p->song.clip_at(0, 0))].bars == 2);
        CHECK(p->song.clips[size_t(p->song.clip_at(0, 2))].pattern == 1 && p->song.markers[0].bar == 2);
        CHECK(jam_to_song(*p, *bank, jam, 6, 99).bars == 2 && p->song.length() == 6); // TO past the end: to the end
        CHECK(jam_to_song(*p, *bank, jam, 9, 12).bars == 0);
    }
    {   // In a project file: after its own sections, which load_project skips.
        auto p = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        synth(p->tracks[0], 0);
        auto bytes = save_project(*p, *bank);
        const size_t plain = bytes.size();
        JamList none;
        write_jams(bytes, none);
        CHECK(bytes.size() == plain); // no jams: nothing written
        list.jams.back().bars[1].muted = 3; list.jams.back().bars[1].scene = 5; list.jams.back().bars[0].pattern[15] = 127;
        write_jams(bytes, list);
        CHECK(bytes.size() > plain);
        JamList back;
        CHECK(read_jams(bytes, back) && back == list);
        auto loaded = std::make_unique<Project>();
        CHECK(load_project(bytes, *loaded, *bank).empty() && loaded->tracks[0].used());
        auto plain_bytes = save_project(*p, *bank);
        CHECK(read_jams(plain_bytes, back) && back.jams.empty() && back.next == 1);
        auto broken = bytes;
        broken.resize(broken.size() - 5); // the section's length now runs past the file: left out
        CHECK(read_jams(broken, back) && back.jams.empty());
    }
}


// Elektron's sequencing in the shell: Step Edit's card sets (SOUND on a kit or keygroup), the SOUND row's cards, the
// sounds a step may take, and the step the cards and the sounds on the pads edit.
void step_sound_cards() {
    auto project = std::make_unique<Project>();
    synth(project->tracks[0], 0);
    load_sound(project->tracks[1], EngineKind::Sampler, 0);
    load_sound(project->tracks[2], EngineKind::Keygroup, 0);
    load_sound(project->tracks[3], EngineKind::Sampler, 0);
    auto& kit = project->tracks[1];
    for (auto& pad : kit.pads) pad = PadSound {};
    kit.pads[0].sample = 4; kit.pads[1].sample = 5; kit.pads[16].sample = 4; // A01 and B01 the same sample: listed once
    project->tracks[2].pads[0].sample = 6;
    for (auto& pad : project->tracks[3].pads) pad = PadSound {};
    project->tracks[3].pads[3].sample = 7;
    for (unsigned n : {4u, 5u, 6u, 7u}) { std::snprintf(project->samples.edit(n).name, sizeof(SampleRef::name), "S%u", n); project->samples.edit(n).frames = 1000; }
    // The sets on each track, in B1's order.
    std::array<uint8_t, cards_count> sets {};
    CHECK(step_card_sets(kit, sets) == 5 && sets[3] == cards_sound && sets[4] == cards_euclid);
    CHECK(step_card_sets(project->tracks[2], sets) == 4 && sets[3] == cards_sound);
    CHECK(step_card_sets(project->tracks[0], sets) == 3);
    CHECK(next_step_set(kit, cards_trig) == cards_sound && next_step_set(kit, cards_sound) == cards_euclid && next_step_set(kit, cards_euclid) == cards_note);
    CHECK(next_step_set(project->tracks[2], cards_sound) == cards_note && step_set(project->tracks[0], cards_sound) == cards_note);
    CHECK(step_set_place(kit, cards_sound) == 3 && std::string(step_cards_name(cards_sound)) == "SOUND");
    // The sounds: every sample on a kit or keygroup pad, by track and pad, each once.
    const auto choices = step_sound_choices(*project);
    CHECK(choices.size() == 4 && choices[0].slot == 4 && choices[0].track == 1 && choices[0].pad == 0 && choices[1].slot == 5);
    CHECK(choices[2].slot == 6 && choices[2].track == 2 && choices[3].slot == 7 && choices[3].pad == 3);
    CHECK(step_sound_index(choices, 0) == 0 && step_sound_index(choices, 7) == 3 && step_sound_index(choices, 200) == -1);
    project->chops.edit(7).slices.count = 12; // slot 7 chopped in twelve
    CHECK(step_sound_slices(*project, kit, sampler_first_note, 8) == 12 && step_sound_slices(*project, kit, sampler_first_note, 0) == 0);
    CHECK(step_sound_slices(*project, project->tracks[2], 60, 8) == 0); // a keygroup plays whole samples
    // The SOUND row on the caret's step: SOUND the step's sample (B02's place), SLICE PAD .. 12 .. RND, PLAY its way.
    Pattern q;
    put(q, 2, uint8_t(sampler_first_note), 100);
    q.events[0].sample = 8; q.events[0].slice = slice_random;
    q.events[0].flags = with_play(q.events[0].flags, StepPlay::Reverse);
    UiState ui;
    ui.page = PageId::StepEdit; ui.track = 1; ui.step_cursor = 2; ui.step_cards = cards_sound;
    kit.last_pad = 0; kit.last_note = uint8_t(sampler_first_note);
    auto row = page_bindings(*project, ui, q, false);
    CHECK(row[0].kind == Binding::Kind::StepSound && row[0].value == 4 && row[0].id == 8 && row[0].max == 4);
    CHECK(row[1].kind == Binding::Kind::StepSlice && row[1].id == 12 && row[1].value == 13 && row[1].max == 13);
    CHECK(row[2].kind == Binding::Kind::StepPlay && row[2].value == int(StepPlay::Reverse));
    { // The screen's copy, made without the chop table (Shell::snapshot): its SLICE card has the chop back on Step Edit.
        auto drawn = std::make_unique<Project>();
        SlotTable<SampleChop> chops = std::move(project->chops);
        *drawn = *project;
        project->chops = std::move(chops);
        CHECK(page_bindings(*drawn, ui, q, false)[1].max == 0); // without it: no SLICE card
        copy_step_chops(*drawn, *project, drawn->tracks[1], q);
        const auto drawn_row = page_bindings(*drawn, ui, q, false);
        CHECK(drawn_row[1].kind == Binding::Kind::StepSlice && drawn_row[1].id == 12 && drawn_row[1].max == 13);
        CHECK(drawn->chops[7].slices.count == 12 && !drawn->chops[4].used() && drawn->chops.blocks_held() == 1);
    }
    q.events[0].sample = 0; q.events[0].slice = 0;
    row = page_bindings(*project, ui, q, false);
    CHECK(row[0].value == 0 && row[0].id == 0 && row[1].max == 0); // its pad's own sample, no chop: SLICE stays PAD
    ui.step_cursor = 3; // a rest: no cards
    CHECK(page_bindings(*project, ui, q, false)[0].kind == Binding::Kind::None);
    // A step held on SOUND: its SOUND row, not locks; on NOTE, locks as ever.
    ui.step_pads = true; ui.held_step = 2;
    CHECK(page_bindings(*project, ui, q, false)[0].kind == Binding::Kind::StepSound);
    ui.step_cards = cards_note;
    CHECK(page_bindings(*project, ui, q, false)[0].kind != Binding::Kind::StepSound);
    // The step they edit: the sounds on the pads' step, else the held one, else the caret's.
    CHECK(sound_step(ui, q) == 2 && !sounds_open(ui));
    ui.held_step = -1; ui.step_cursor = 5;
    CHECK(sound_step(ui, q) == 5);
    ui.sounds_step = 9; ui.sounds_for = 1;
    CHECK(sounds_open(ui) && sound_step(ui, q) == 9);
    ui.track = 3;
    CHECK(!sounds_open(ui)); // another track: closed
    ui.track = 1; ui.page = PageId::Roll;
    CHECK(!sounds_open(ui));
    // Patterns' Shift + K3 over every speed.
    ui.page = PageId::Patterns; ui.track = 1;
    CHECK(shift_bindings(*project, ui, q)[2].max == int(speed_count) - 1);
    std::puts("step sound cards ok");
}

int main() {
    step_sound_cards();
    browse_trim_sheet();
    audio_monitor_choice();
    pad_value_erase();
    stems_lookup();
    sample_lists();
    history_list();
    direct_monitoring();
    usb_volume();
    controls_audit();
    usage_log();
    free_length();
    spare_steps();
    progressions();
    pad_sends_stage();
    pad_sends_lanes();
    pad_colours();
    tempo_lane_p5();
    scenes_p5();
    sequence_links();
    song_steps_mpc();
    jam_capture();
    jam_list();
    banks_f1();
    deep_freeze_f2d();
    midi_tracks_p4();
    mix_motion_p4();
    keyed_dynamics_p4();
    delay_compensation_p4();
    samples_used();
    loop_seams();
    pad_layers_p3();
    track_speed();
    generators();
    feel_p1();
    note_repeat_p1();
    ratchets();
    trig_conditions();
    pattern_motion();
    kit_lanes();
    preset_folders();
    nam_page();
    card_plugin_paths();
    card_paths_moved();
    qlink_pages();
    plugin_json();
    xpl_presets();
    plugin_page_layout();
    page_files();
    companion_packages();
    synths_as_plugins();
    voice_taps();
    voice_heads();
    chop_heads();
    cloud_grains();
    head_lines();
    place_bar();
    song_buttons();
    keygroup_tracks();
    instrument_zones_r3();
    track_source();
    zero_snap();
    roll();
    roll_marks();
    roll_marks_velocity_shift();
    roll_opens_on_notes();
    roll_levels();
    stage();
    midi_tracks();
    device_rates();
    key_pick();
    card_access();
    projects_and_polish();
    midi_and_sync();
    fader_and_takeover();
    performance_effects();
    audio_input_and_rendering();
    light_show_lights();
    meter_lights();
    input_taken();
    auto_chop_kinds();
    chop_and_browser();
    zoom_and_fine_edit();
    sample_mode();
    step_edit_and_pad_motion();
    timing();
    pad_expression();
    parity_1c();
    shift_pads();
    pad_lights();
    track_move();
    erase_copy_everywhere();
    drum_layout();
    button_remap();
    looper_chord_and_sheet();
    parity_basics();
    history();
    names();
    settings();
    sampler_kit();
    bindings();
    mpc_chop_hits();
    std::puts("melodee ok");
}
