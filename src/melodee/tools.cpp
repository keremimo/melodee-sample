// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/tools.h"
#include "melodee/engine/engine_host.h"
#include "melodee/engine/effect_info.h"
#include "melodee/engine/prophet_params.h"
#include "melodee/model/mod_matrix.h"
#include "melodee/pages/page.h"
#include "melodee/shell/shell.h"
#include "runtime/screen.h"
#include "runtime/util.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace melodee {
namespace {

// The boards' sample content, close enough to compare page by page. Names
// and numbers are placeholders, as on the boards.
void board_project(Shell& shell) {
    auto& project = shell.project();
    std::snprintf(project.name, sizeof(project.name), "Night drive");
    project.tempo = 1240;
    project.root = 2; project.scale = 2; // D Dorian
    const unsigned sounds[] {1, 2, 6, 7, 12, 5, 3, 4, 11, 9};
    for (unsigned t = 0; t < 10; ++t) load_sound(project.tracks[t], EngineKind::Prophet, sounds[t]);
    const uint8_t levels[] {98, 88, 74, 56, 104, 64, 81, 51, 70, 46};
    for (unsigned t = 0; t < 10; ++t) project.tracks[t].mix.level = levels[t];
    const int8_t pans[] {0, -12, 20, 0, 0, -20, 12, 25, -6, 0};
    for (unsigned t = 0; t < 10; ++t) project.tracks[t].mix.pan = pans[t];
    project.tracks[3].mix.mute = true;
    project.tracks[0].pattern = 1;
    project.tracks[0].set_name("FUNK BASS II");
    for (unsigned t = 1; t < 7; ++t) {
        auto& p = shell.patterns().at[t][0];
        p.steps[0].notes[0] = {48, 100};
    }
    // Track 1's pattern 2: the bass line of the Steps board, in each of its four bars.
    auto& line = shell.patterns().at[0][1];
    line = shell.patterns().at[0][0];
    line.length = 64;
    for (unsigned s = 16; s < 64; ++s) {
        line.steps[s] = line.steps[s % 16];
        for (auto& n : line.steps[s].notes) if (n.velocity) n.key = uint8_t(n.key + 12); // as on the Roll board
    }
    line.steps[22].notes[0] = {48, 96}; line.steps[22].length = 2; line.steps[22].flags = step_accent;
    // The Patterns board: which patterns hold notes, a small tune in each.
    const char* state[] {"dpdeeeeedeeeeeee", "pdeeeeeeeeeeeeee", "pqddeeeedeeeeeee", "ddpeeeeeeeeeeeee",
                         "dpddeeeeeeeeeeee", "pdeeeeeeeeeeeeee", "pdeeeeeeeeeeeeee", "deeeeeeeeeeeeeee"};
    const int tune[] {0, 3, 7, 5, 10, 7};
    const unsigned at[] {0, 3, 6, 8, 11, 14};
    for (unsigned t = 0; t < 8; ++t)
        for (unsigned n = 0; n < 16; ++n) {
            if (state[t][n] == 'e' || (t == 0 && n == 1)) continue;
            auto& pattern = shell.patterns().at[t][n];
            for (unsigned k = 0; k < 6; ++k) pattern.steps[at[k] + (n % 2)].notes[0] = {uint8_t(48 + tune[(k + n + t) % 6]), 100};
        }
    // The Song board: markers, clips and the loop.
    auto& song = project.song;
    song.markers[0] = {16, "VERSE"}; song.markers[1] = {24, "DROP"}; song.marker_count = 2;
    const Clip clips[] {{0, 0, 16, 8, false}, {0, 1, 24, 8, false}, {1, 0, 16, 4, false}, {1, 1, 24, 8, false},
                        {2, 0, 16, 8, false}, {2, 2, 24, 2, false}, {2, 2, 26, 6, false}, {3, 2, 16, 16, false},
                        {4, 0, 20, 4, false}, {4, 1, 24, 8, false}, {5, 0, 24, 8, false}, {7, 0, 28, 4, false}};
    for (const auto& clip : clips) song.place(clip);
    song.loop_start = 24; song.loop_end = 32;
    // The effects boards: track 3's chain, four buses, the master.
    auto put = [&](Chain& chain, unsigned slot, EffectKind kind) {
        chain.slots[slot].kind = uint8_t(kind);
        const auto& info = effect_info(kind);
        for (unsigned q = 0; q < effect_params; ++q) chain.slots[slot].params[q] = info.params[q].label ? info.params[q].initial : 0;
    };
    put(project.tracks[2].fx, 0, EffectKind::Chorus); put(project.tracks[2].fx, 1, EffectKind::Tape); put(project.tracks[2].fx, 2, EffectKind::Eq);
    put(project.buses[0].fx, 0, EffectKind::Room); put(project.buses[0].fx, 1, EffectKind::Echo); put(project.buses[0].fx, 2, EffectKind::Width);
    put(project.buses[1].fx, 0, EffectKind::Echo); put(project.buses[2].fx, 0, EffectKind::Crush);
    put(project.master, 0, EffectKind::Compressor); put(project.master, 1, EffectKind::Limiter);
    project.buses[0].level = 70; project.buses[1].level = 55; project.buses[2].level = 40;
    const uint8_t sends[] {8, 0, 42, 60, 4, 72, 55, 30, 24, 80};
    for (unsigned t = 0; t < 10; ++t) project.tracks[t].mix.send = sends[t];
    // The Mod board's six routes on track 1.
    const ModRoute routes[] {
        {uint8_t(ModSource::Lfo), uint8_t(ModDest::Cutoff), 34}, {uint8_t(ModSource::Velocity), uint8_t(ModDest::AmpLevel), 60},
        {uint8_t(ModSource::Aftertouch), uint8_t(ModDest::OscBPitch), -18}, {uint8_t(ModSource::Wheel), uint8_t(ModDest::LfoRate), 45},
        {uint8_t(ModSource::KeyTrack), uint8_t(ModDest::Cutoff), 50}, {uint8_t(ModSource::Random), uint8_t(ModDest::PulseWidth), 30},
    };
    for (unsigned r = 0; r < 6; ++r) project.tracks[0].mods[r] = routes[r];
    // Motion on CUTOFF and RESO, as drawn on the Motion board.
    const int cutoff[] {40, 44, 52, 60, 72, 80, 84, 80, 70, 62, 54, 48, 44, 50, 58, 62};
    for (unsigned n = 0; n < 16; ++n) project.motion.write({0, 1, uint16_t(n * 4), uint8_t(prophet::Cutoff), int16_t(cutoff[n])});
    const int reso[] {40, 52, 60, 52, 40, 35};
    for (unsigned n = 0; n < 6; ++n) project.motion.write({0, 1, uint16_t(24 + n * 4), uint8_t(prophet::Resonance), int16_t(reso[n])});
}

struct Preview {
    const char* name;
    void (*set)(Shell&, Snapshot&);
};

const Preview previews[] {
    {"stage", [](Shell&, Snapshot& s) { s.ui.page = PageId::Stage; s.playing = true; }},
    {"keys", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Keys; s.ui.held_pads = 1u << 4 | 1u << 9; s.playing = true;
         s.ui.turning = 1; s.ui.turned_at = s.now;
     }},
    {"sounds", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Sounds; s.ui.track = 2; s.ui.sounds_cursor = 2; s.playing = true;
         s.ui.turning = 1; s.ui.turned_at = s.now;
     }},
    {"edit", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Edit; s.ui.edit_page[0] = 3; s.playing = true;
         s.ui.turning = 0; s.ui.turned_at = s.now;
         s.ui.pots[1] = {81, false, Binding {Binding::Kind::Param, prophet::Resonance, 0, 100, 0}.key(0)};
     }},
    {"soundmap", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Edit; s.ui.edit_page[0] = 3; s.ui.map_open = true; s.ui.map_cursor = 3; s.playing = true;
     }},
    {"mod", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Edit; s.ui.edit_page[0] = 10; s.playing = true;
     }},
    {"mix", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Mix; s.ui.track = 2; s.playing = true;
         s.ui.turning = 0; s.ui.turned_at = s.now;
         s.ui.pots[2] = {90, false, Binding {Binding::Kind::Send, 0, 0, 100, 0}.key(2)};
         const float meters[] {0.5f, 0.42f, 0.2f, 0, 0.6f, 0.25f, 0.3f, 0.05f, 0.18f, 0.08f};
         for (unsigned t = 0; t < 10; ++t) s.meters[t] = meters[t];
     }},
    {"steps", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Steps; s.ui.step_cursor = 6; s.ui.held_step = 6; s.playing = true; s.position = 9;
         s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"roll", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Roll; s.ui.step_cursor = 22; s.playing = true; s.position = 26; s.ui.recording = true;
         s.ui.turning = 1; s.ui.turned_at = s.now;
     }},
    {"notesheet", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Roll; s.ui.step_cursor = 22; s.playing = true; s.position = 26; s.ui.sheet_open = true;
     }},
    {"motion", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Motion; s.playing = true; s.position = 40; s.ui.recording = true;
         s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"patterns", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Patterns; s.ui.track = 2; s.playing = true;
         const uint8_t live[] {1, 0, 0, 2, 1, 0, 0, 0};
         for (unsigned t = 0; t < 8; ++t) { s.playing_patterns[t] = live[t]; s.project.tracks[t].pattern = live[t]; s.steps[t] = int(4 + t); }
         s.project.tracks[2].pattern = 1;
     }},
    {"song", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Song; s.ui.track = 2; s.ui.song_cursor = 24; s.ui.song_view = 16; s.ui.song_zoom = 2;
         s.ui.song_pattern = 2; s.ui.song_length = 4;
         s.playing = s.song_playing = true; s.song_bar = 20; s.ticks = 48;
         s.ui.turning = 1; s.ui.turned_at = s.now;
     }},
    {"songzoom", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Song; s.ui.track = 2; s.ui.song_cursor = 26; s.ui.song_view = 24; s.ui.song_zoom = 0;
         s.playing = s.song_playing = true; s.song_bar = 25; s.ticks = 40;
         s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"clipsheet", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Song; s.ui.track = 2; s.ui.song_cursor = 26; s.ui.song_view = 16; s.ui.sheet_open = true;
         s.playing = true;
     }},
    {"length", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Length; s.playing = true; s.steps[0] = 34; s.project.swing = 54;
         s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"rack", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Rack; s.ui.track = 2; s.ui.fx_cursor[2] = 1; s.playing = true;
         s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"sends", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Sends; s.ui.track = 2; s.playing = true;
         s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"bus", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Bus; s.ui.track = 2; s.playing = true;
         s.ui.turning = 1; s.ui.turned_at = s.now;
     }},
    {"fxpick", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Rack; s.ui.track = 2; s.ui.fx_cursor[2] = 3; s.playing = true;
         s.ui.picker_open = true; s.ui.picker_items = true; s.ui.picker_family = 2; s.ui.picker_item = 2;
     }},
    {"shift", [](Shell&, Snapshot& s) { s.ui.page = PageId::Stage; s.ui.shift = true; s.playing = true; }},
    {"exit", [](Shell&, Snapshot& s) { s.ui.page = PageId::Stage; s.ui.exit_open = true; }},
};

void draw(juce::Graphics& g, const Snapshot& s) {
    ui::Paint p(g);
    g.fillAll(ui::colour::bg);
    page_spec(s.ui.page).draw(p, s);
    if (s.ui.exit_open) draw_exit_sheet(p, s);
    else if (s.ui.shift && !s.ui.shift_used) draw_shift_layer(p, s);
}

int preview(const std::string& folder, const std::vector<std::string>& names) {
    EngineHost host;
    Shell shell(host);
    board_project(shell);
    unsigned written = 0;
    for (const auto& preview : previews) {
        if (!names.empty() && std::find(names.begin(), names.end(), preview.name) == names.end()) continue;
        auto snapshot = std::make_unique<Snapshot>();
        shell.snapshot(*snapshot, 100000);
        snapshot->ui = UiState {};
        snapshot->now = 100000;
        preview.set(shell, *snapshot);
        // The pattern in view follows the track the preview picked.
        const auto& track = snapshot->project.tracks[snapshot->ui.track];
        snapshot->pattern = shell.patterns().at[snapshot->ui.track][track.pattern];
        for (int scale : {1, 2}) {
            juce::Image image(juce::Image::ARGB, screen_width * scale, screen_height * scale, true);
            {
                juce::Graphics g(image);
                g.addTransform(juce::AffineTransform::scale(float(scale)));
                draw(g, *snapshot);
            }
            save_png(image, folder + "/" + preview.name + (scale == 2 ? "@2x" : "") + ".png");
        }
        ++written;
    }
    require(written > 0, "Unknown Melodee page; use stage keys sounds edit soundmap mod mix steps roll notesheet motion patterns song songzoom clipsheet length rack sends bus fxpick shift exit");
    std::cout << "MELODEE_PREVIEW pages=" << written << '\n';
    return 0;
}

void little(std::ofstream& file, uint32_t value, unsigned bytes) {
    for (unsigned n = 0; n < bytes; ++n) file.put(char((value >> (n * 8)) & 255));
}

// Two bars of the starting project at 120 BPM (4 s), then two seconds of
// tail: the bass line must sound, stay below full scale and end in silence.
int sound(const std::string& path) {
    EngineHost host;
    Shell shell(host);
    host.load(shell.project(), shell.patterns());
    host.send({Command::Type::Play});
    constexpr unsigned rate = 44100, frames = rate * 6, block = EngineHost::block;
    std::ofstream file(path, std::ios::binary);
    require(bool(file), "Create Melodee test audio");
    file.write("RIFF", 4); little(file, 36 + frames * 4, 4); file.write("WAVEfmt ", 8);
    little(file, 16, 4); little(file, 1, 2); little(file, 2, 2); little(file, rate, 4);
    little(file, rate * 4, 4); little(file, 4, 2); little(file, 16, 2);
    file.write("data", 4); little(file, frames * 4, 4);
    std::array<int32_t, block * 2> samples {};
    double energy = 0, tail = 0;
    float peak = 0;
    for (unsigned offset = 0; offset < frames; offset += block) {
        if (offset >= rate * 4 && offset < rate * 4 + block) host.send({Command::Type::Stop});
        host.render(samples.data(), block, 0.7f);
        for (unsigned i = 0; i < block * 2; ++i) {
            float value = samples[i] / 2147483648.0f;
            peak = std::max(peak, std::fabs(value));
            energy += double(value) * value;
            if (offset > rate * 5) tail += double(value) * value;
            little(file, uint16_t(samples[i] >> 16), 2);
        }
    }
    double rms = std::sqrt(energy / (frames * 2)), tail_rms = std::sqrt(tail / (rate * 2));
    std::cout << "MELODEE_SOUND peak=" << peak << " rms=" << rms << " tail_rms=" << tail_rms << '\n';
    require(file.good() && rms > 0.005 && peak < 0.999f && tail_rms < 0.001, "Melodee sound check failed");
    return 0;
}

} // namespace

std::optional<int> run_tool(int argc, char** argv) {
    if (argc >= 3 && !std::strcmp(argv[1], "--melodee-preview")) {
        std::vector<std::string> names(argv + 3, argv + argc);
        return preview(argv[2], names);
    }
    if (argc == 3 && !std::strcmp(argv[1], "--melodee-sound")) return sound(argv[2]);
    return std::nullopt;
}

} // namespace melodee
