// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// A companion screen (sdk/melodee-screen/melodee_companion.h) for one plug-in instance, as the rack's screen thread
// sees any plug-in screen (plugin/screen_source.h). It keeps the copies of the plug-in's parameters the screen reads,
// read on the screen thread (never the audio thread), and hands them to the helper process (plugin/companion_host.h)
// with each draw. What the screen asks for (a value, a program) waits in take() for the MIDI thread, which makes the
// change as a pot turn does (shell/plugin_jobs.cpp). A screen that crashes, hangs or misbehaves stops for good:
// hasScreen() turns false and the rack shows the plug-in's pages again.
#pragma once
#include "melodee/plugin/companion_package.h"
#include "melodee/plugin/companion_protocol.h"
#include "melodee/plugin/screen_source.h"
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace juce {
class AudioPluginInstance;
class AudioProcessorParameter;
} // namespace juce

namespace melodee {

class CompanionHost;

// A package found on the card, matched to a plug-in instance, with the copy of its library the helper loads.
struct CompanionPackage {
    std::string folder;  // its folder's name under Melodee/Plugin Screens
    std::string library; // the copy in RAM the helper loads
    CompanionManifest manifest;
};

// What a companion asked for: a parameter's value, or a program (the plug-in's own number for it).
struct CompanionRequest {
    bool program = false;
    int index = 0;   // a parameter's index, or the program
    float value = 0; // 0..1
};

class CompanionScreen final : public ScreenSource {
public:
    // Rack's thread, the plug-in opened. `session`: a number no other screen in this run has.
    CompanionScreen(juce::AudioPluginInstance& plugin, CompanionHost& host, CompanionPackage package, uint32_t session);
    ~CompanionScreen() override;

    bool hasScreen() const override { return !stopped.load(std::memory_order_acquire); }
    bool screenDraw(const melodee_screen_frame& frame, bool& changed) override;
    bool screenInput(const melodee_screen_input& input, bool& used) override;
    bool screenControls(std::array<int, 3>& indices) override;
    bool takeRedraw() override;
    bool wantsProcess() const override { return false; } // its edits reach the plug-in as a pot's do

    // The requests waiting (any thread): the newest value for each parameter, the last program asked for.
    void take(std::vector<CompanionRequest>& out);
    const CompanionPackage& package() const { return pack; }
    std::string stop_reason() const;
    // Its screen destroyed in the helper (the screen thread kept out, as destroy() does).
    void close();

private:
    bool fail(const std::string& why);
    bool ensure_open(bool& fresh);
    bool refresh(bool texts);
    void write_block();
    bool collect();
    template <class Fill> bool send(companion::Op op, companion::Op answer, Fill fill, companion::Message& reply);

    juce::AudioPluginInstance& plugin;
    CompanionHost& host;
    const CompanionPackage pack;
    const uint32_t session;
    std::vector<juce::AudioProcessorParameter*> params; // by handle
    std::vector<melodee_companion_param_t> copies;      // by handle (screen thread)
    melodee_companion_program_t program {};
    int program_count = 1;
    std::array<int, 3> controls {-1, -1, -1};
    std::chrono::steady_clock::time_point refreshed {};
    std::atomic<bool> redraw {true}, stopped {false};
    mutable std::mutex requests_lock; // the two below and `reason`
    std::map<int, float> edits;       // by parameter index
    int picked = -1;
    std::string reason;
};

} // namespace melodee
