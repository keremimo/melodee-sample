// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// A hosted plug-in that may draw its own screen (sdk/melodee-screen/melodee_screen.h), as the rack's screen thread
// calls it (plugin/plugin_rack.h): CLAP plug-ins (plugin/clap_instance.h) and VST2 ones (plugin/vst2_instance.h) each
// hand their screen over in their own format's way. screenDraw(), screenInput() and screenControls() run only while
// no other non-audio call into the plug-in does: false, and nothing done, while one does (try again a tick later).
#pragma once
#include <array>
#include <cstdint>

struct melodee_screen_frame;
struct melodee_screen_input;

namespace melodee {

class ScreenSource {
public:
    virtual ~ScreenSource() = default;
    virtual bool hasScreen() const = 0;
    virtual bool screenDraw(const melodee_screen_frame& frame, bool& changed) = 0;
    virtual bool screenInput(const melodee_screen_input& input, bool& used) = 0;
    // The parameters K1-K3 turn, as getParameters() numbers them, -1 none.
    virtual bool screenControls(std::array<int, 3>& indices) = 0;
    // It asked for a draw since the last call (any thread).
    virtual bool takeRedraw() = 0;
    // The printed buttons its page takes now, as it last told (MELODEE_SCREEN_KEY_ bits; any thread). 0: none, as
    // for a plug-in that never tells (melodee.screen-host/1, the 32-bit bridge).
    virtual uint32_t screenKeys() const { return 0; }
    // Its next block should run although the rack let it sleep (plugin/idle_gate.h): it asked to be run (a screen
    // input its audio side takes; CLAP's request_process), or a value waits for it. Any thread.
    virtual bool wantsProcess() const = 0;
};

} // namespace melodee
