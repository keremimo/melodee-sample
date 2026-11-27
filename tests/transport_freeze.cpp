// SPDX-License-Identifier: AGPL-3.0-only
// Freeze with a plug-in that plays by itself, through PRINT, the rack and the engine: tests/clap_fixture.cpp built
// with MELODEE_TEST_SEQUENCER, whose Sine plays a bump on every sixteenth while it hears the song play, with no note
// sent. Run by tests/test_plugin_hosts.py: transport_freeze CARD PLUGIN MODE RATE.
//   freeze         a pattern without notes, not lit by default or by ALL, picked on PRINT and frozen: its print not
//                  silent, heard alone (not under the plug-in as well), the engine asleep under it; unfrozen, the
//                  plug-in plays on where the song is, the same to the sample as a run never frozen.
//   transport-off  TRANSPORT off: the plug-in plays only what it is sent, so a pattern without notes cannot be picked.
//   plain          the fixture built without it (a plug-in that plays nothing by itself): the pattern prints silent,
//                  so the track is not frozen.
//   deep-stage     frozen, then Shift + B3 DEEP FREEZE on Stage: the pattern without notes stays frozen, deep.
//   deep-command   frozen, then `melodee-freeze 1 deep` naming no pattern: the same.
//   gate           frozen, TRANSPORT switched: "Unfreeze track 1?" first, TRANSPORT as it was.
//   launch         frozen while a two-bar A2 plays, then launched from the Patterns page: it keeps its one bar (its
//                  print's), so the print plays.
#include "melodee/engine/sample_data.h"
#include "melodee/plugin/plugin_rack.h"
#include "melodee/shell/print_engine.h"
#include "melodee/shell/seq_layer.h"
#include "melodee/shell/shell.h"
#include "runtime/controller.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>

using namespace melodee;
namespace fs = std::filesystem;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " + #x); } while (false)

namespace {

struct Audio { std::vector<float> left, right; std::vector<int32_t> main; };

// One run of the project: frozen (picked on PRINT, then unfrozen there) or played through live. The audio of the bar
// after the unfreeze and of the one after it comes back, for the two runs to be compared.
Audio play(const std::string& card, const std::string& plugin, bool freeze, bool transport, unsigned rate, const std::string& mode) {
    constexpr unsigned block = EngineHost::block;
    // A bar of whole blocks (87.5 BPM at 44.1 kHz, 125 BPM at 48 kHz), so each pass starts on a block.
    const unsigned tempo = rate == 44100 ? 875 : 1250, bar_blocks = rate == 44100 ? 945 : 720;
    dsp::device_rate.store(rate);
    auto host = std::make_unique<EngineHost>();
    host->set_render_threads(0);
    auto rack = std::make_unique<PluginRack>(*host, freeze ? "frozen-" : "live-");
    rack->set_card(card);
    auto printer = std::make_unique<PrintEngine>(nullptr);
    printer->set_pager(&host->streams());
    auto shell = std::make_unique<Shell>(*host);
    shell->set_printer(printer.get());
    shell->set_plugins(rack.get());
    auto& c = shell->context();
    c.project_path = (fs::path(card) / (freeze ? "frozen.mldp" : "live.mldp")).string();
    c.now = 1000;
    c.project.tempo = uint16_t(tempo);
    c.project.signature = common_time;
    auto& track = c.project.tracks[0];
    load_sound(track, EngineKind::Plugin, 0);
    track.plugin = 0;
    std::snprintf(c.project.plugins[0].path, sizeof(PluginRef::path), "%s", plugin.c_str());
    c.project.plugins[0].transport = transport;
    auto& pattern = c.patterns.at[0][0]; // A1: no notes, a bar long
    pattern.length = c.project.bar_ticks();
    pattern.free = false;
    CHECK(pattern.empty());
    if (mode == "launch") { // A2 plays first: two bars, no notes either
        auto& two = c.patterns.at[0][1];
        two.length = 2 * c.project.bar_ticks();
        two.free = false;
        track.pattern = 1;
    }
    host->load(c.project, c.patterns);
    host->set_audio_format(rate, block);
    host->set_print_tap(0, true);
    std::array<int32_t, block * 2> out {};
    auto render = [&] {
        while (host->streams().pump()) {}
        host->render(out.data(), block, 0.7f);
    };
    auto tick = [&] { c.plugins_tick(++c.now); c.print_tick(); };
    auto press = [&](SurfaceButton button, bool pressed) {
        SurfaceEvent event;
        event.input.kind = SurfaceInput::Kind::Button;
        event.input.index = uint8_t(button);
        event.input.pressed = pressed;
        shell->surface(event, c.now + 1);
    };
    auto tap = [&](SurfaceButton button) { press(button, true); press(button, false); };
    auto spin = [&](auto done) {
        for (unsigned n = 0; n < 3000 && !done(); ++n) {
            tick();
            render();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(done());
    };
    spin([&] { return rack->status(0) == PluginStatus::Ready; });
    for (unsigned n = 0; n < 32; ++n) { tick(); render(); }
    c.open(PageId::Stage);

    unsigned blocks = 0;
    auto advance = [&](unsigned until, Audio* capture = nullptr) {
        for (; blocks < until; ++blocks) {
            tick();
            render();
            if (!capture) continue;
            capture->left.insert(capture->left.end(), host->print_left(), host->print_left() + block);
            capture->right.insert(capture->right.end(), host->print_right(), host->print_right() + block);
            capture->main.insert(capture->main.end(), out.begin(), out.end());
        }
    };
    tap(SurfaceButton::Play);
    advance(bar_blocks / 2);
    const auto& sheet = shell->ui().print_sheet;
    if (freeze) {
        tap(SurfaceButton::SampleRecord); // PRINT: by default the patterns with notes, so none lit here
        CHECK(shell->ui().page == PageId::Print && !sheet.picked(0));
        c.print_sheet_all(); // ALL: every pattern with notes, so still none
        CHECK(!sheet.picked(0));
        unsigned pad = 0;
        while (pad < pattern_pads && shell->ui().pad_pattern(pad) != 0) ++pad;
        CHECK(pad < pattern_pads);
        c.print_sheet_pad(pad);
        if (!transport) { // it plays only what it is sent: A1 has nothing to print
            CHECK(!sheet.picked(0) && std::string(shell->ui().notice) == "P1 is empty");
            tap(SurfaceButton::TopRight);
            CHECK(!sheet.printing && !c.frozen(0) && std::string(shell->ui().notice) == "No pattern lit");
            return {};
        }
        CHECK(sheet.picked(0));
        tap(SurfaceButton::TopRight); // B3 FREEZE
        CHECK(sheet.printing && c.frozen(0));
        c.print_tick();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (printer->busy() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        CHECK(!printer->busy());
    }
    advance(2 * bar_blocks); // the print sent, and switched in on a bar line
    if (mode == "plain") { // it played nothing by itself: nothing frozen, and the sheet says why
        CHECK(!c.frozen(0) && !(host->printing_tracks() & 1) && !(host->frozen_tracks() & 1));
        const std::string said = shell->ui().notice;
        if (said != "Track 1 silent: not frozen") throw std::runtime_error("notice: " + said);
        return {};
    }
    auto wait_prints = [&] {
        c.print_tick();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (printer->busy() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        CHECK(!printer->busy());
    };
    if (mode == "deep-stage" || mode == "deep-command") {
        CHECK(c.frozen(0) && c.prints.freeze[0].plays(0) && !c.deep(0));
        if (mode == "deep-stage") c.deep_freeze(0);
        else { // `melodee-freeze 1 deep`: no pattern named
            Context::PrintAsk ask;
            ask.track = 0;
            ask.mode = Context::PrintAsk::Freeze;
            ask.deep = true;
            c.print(ask);
        }
        CHECK(c.deep(0) && c.prints.freeze[0].plays(0));
        wait_prints();
        advance(blocks + 2 * bar_blocks);
        CHECK(c.deep(0) && c.prints.freeze[0].plays(0) && (host->printing_tracks() & 1));
        return {};
    }
    if (mode == "gate") {
        CHECK(c.frozen(0));
        c.toggle_plugin_transport(0);
        CHECK(shell->ui().ask == UiState::Ask::Unfreeze && c.project.plugins[0].transport && c.frozen(0));
        return {};
    }
    if (mode == "launch") {
        CHECK(c.frozen(0) && host->pattern_of(0) == 1 && !(host->printing_tracks() & 1)); // A2 plays live
        c.open(PageId::Patterns);
        unsigned pad = 0;
        while (pad < pattern_pads && shell->ui().pad_pattern(pad) != 0) ++pad;
        CHECK(pad < pattern_pads);
        seq_pad(c, pad, true);
        seq_pad(c, pad, false);
        CHECK(pattern.length == c.project.bar_ticks() && !pattern.free); // the length its print was made at
        advance(blocks + 3 * bar_blocks);
        CHECK(host->pattern_of(0) == 0 && (host->printing_tracks() & 1));
        return {};
    }
    if (freeze) {
        CHECK(c.frozen(0) && !c.deep(0) && (host->printing_tracks() & 1));
        const auto* record = c.prints.find(0, 0);
        CHECK(record && record->tap == PrintTap::PostFx && record->tail == PrintTail::Loop);
        SampleData wav;
        CHECK(read_wav((fs::path(prints_folder(c.project_path)) / record->file).string(), wav).empty());
        CHECK(wav.frames == bar_blocks * block);
        float peak = 0;
        for (float v : wav.left) peak = std::max(peak, std::fabs(v));
        CHECK(peak > 0.2f); // the plug-in's own sixteenths, with no note sent
        Audio printed;
        advance(3 * bar_blocks, &printed);
        CHECK(printed.left == wav.left && printed.right == wav.right); // the print alone, to the sample
        CHECK(host->sleeping_tracks() & 1); // and the plug-in's engine asleep under it
        tap(SurfaceButton::SampleRecord); // PRINT again: Shift + B1 UNFREEZE
        CHECK(shell->ui().page == PageId::Print && sheet.picked(0));
        press(SurfaceButton::Shift, true);
        tap(SurfaceButton::TopLeft);
        press(SurfaceButton::Shift, false);
        CHECK(!c.frozen(0));
    }
    advance(3 * bar_blocks + 8); // past the print's few milliseconds of fade
    CHECK(!(host->printing_tracks() & 1) && !(host->sleeping_tracks() & 1));
    Audio live;
    advance(5 * bar_blocks, &live);
    CHECK(host->print_waits() == 0 && host->print_underruns() == 0);
    float peak = 0;
    for (float v : live.left) peak = std::max(peak, std::fabs(v));
    CHECK(peak > 0.2f);
    return live;
}

} // namespace

int main(int argc, char** argv) {
    try {
        CHECK(argc == 5);
        const std::string card = argv[1], plugin = argv[2], mode = argv[3];
        const unsigned rate = unsigned(std::stoul(argv[4]));
        CHECK(rate == 44100 || rate == 48000);
        if (mode == "transport-off") {
            play(card, plugin, true, false, rate, mode);
        } else if (mode != "freeze") {
            CHECK(mode == "plain" || mode == "deep-stage" || mode == "deep-command" || mode == "gate" || mode == "launch");
            play(card, plugin, true, true, rate, mode);
        } else {
            const Audio uninterrupted = play(card, plugin, false, true, rate, mode);
            const Audio resumed = play(card, plugin, true, true, rate, mode);
            CHECK(resumed.left == uninterrupted.left && resumed.right == uninterrupted.right);
            CHECK(resumed.main == uninterrupted.main);
        }
        std::printf("transport freeze %s ok\n", mode.c_str());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
