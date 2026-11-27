// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// A small VST2 plug-in for tests/vst2_host.cpp, on Melodee's own VST2 interface (plugin/vst2_abi.h): Test Tone, an
// instrument whose output is its Level on both channels, with Level and Wave parameters, and its own screen
// (sdk/melodee-screen/melodee_screen.h): a bar as wide as Level in the track's colour over a dark page. The wheel turns
// Level a hundredth a detent (it asks the host for a draw and a block), Push steps Wave, Minus / Plus are not its; K1
// Level, K2 Wave, K3 nothing.
//
// With FIXTURE_NO_SCREEN set it says it has no screen (effCanDo), yet answers the vendor call with junk, as plug-ins
// that answer every vendor call do: the host must not take that as a screen.
#include "melodee/plugin/vst2_abi.h"
#include <melodee_screen.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
struct Tone {
    vst2::Effect effect {};
    vst2::HostCallback host = nullptr;
    float level = 0.5f, wave = 0.0f;
    int drawnBar = -1;
};
Tone& of(vst2::Effect* e) { return *static_cast<Tone*>(e->object); }
Tone& of(const void* e) { return of(static_cast<vst2::Effect*>(const_cast<void*>(e))); }
int hostScreen = -1; // what the host answered to canDo("melodeeScreen") at open

bool screenDraw(const void* plugin, const melodee_screen_frame_t* frame) {
    auto& t = of(plugin);
    int bar = int(std::lround(t.level * frame->width));
    if (!frame->redraw_all && bar == t.drawnBar) return false;
    for (uint32_t y = 0; y < frame->height; ++y)
        for (uint32_t x = 0; x < frame->width; ++x)
            frame->pixels[y * frame->stride + x] = y < 10 && int(x) < bar ? frame->colour : 0x101010u;
    t.drawnBar = bar;
    return true;
}
bool screenInput(const void* plugin, const melodee_screen_input_t* in) {
    auto& t = of(plugin);
    if (in->kind == MELODEE_SCREEN_WHEEL) t.level = std::clamp(t.level + float(in->value) * 0.01f, 0.0f, 1.0f);
    else if (in->kind == MELODEE_SCREEN_PUSH) t.wave = t.wave > 0.5f ? 0.0f : 1.0f;
    else if (in->kind == MELODEE_SCREEN_MUTE) t.level = 0.0f;
    else return false;
    // Push: its page takes Mute from now on; Mute (Level to 0): no longer.
    t.host(&t.effect, vst2::host::vendorSpecific, MELODEE_SCREEN_VST2_KEYS, in->kind == MELODEE_SCREEN_PUSH ? MELODEE_SCREEN_KEY_MUTE : 0, nullptr, 0.0f);
    t.host(&t.effect, vst2::host::vendorSpecific, MELODEE_SCREEN_VST2, MELODEE_SCREEN_RUN, nullptr, 0.0f);
    return true;
}
void screenControls(const void*, uint32_t ids[3]) {
    ids[0] = 0;
    ids[1] = 1;
    ids[2] = MELODEE_SCREEN_NO_CONTROL;
}
const melodee_screen_t screen {screenDraw, screenInput, screenControls};

void copy(void* ptr, const char* text) { std::snprintf(static_cast<char*>(ptr), 64, "%s", text); }

intptr_t dispatcher(vst2::Effect* e, int32_t opcode, int32_t index, intptr_t, void* ptr, float) {
    auto& t = of(e);
    const bool shown = std::getenv("FIXTURE_NO_SCREEN") == nullptr;
    switch (opcode) {
    case vst2::op::open:
        hostScreen = int(t.host(e, vst2::host::canDo, 0, 0, (void*) MELODEE_SCREEN_VST2_CANDO, 0.0f));
        return 0;
    case vst2::op::close: delete &t; return 0;
    case vst2::op::getEffectName: copy(ptr, "Test Tone"); return 1;
    case vst2::op::getVendorString: copy(ptr, "Melodee"); return 1;
    case vst2::op::getParamName: copy(ptr, index == 0 ? "Level" : "Wave"); return 0;
    case vst2::op::getParamDisplay: {
        char text[32];
        if (index == 0) std::snprintf(text, sizeof(text), "%d %%", int(std::lround(t.level * 100)));
        else std::snprintf(text, sizeof(text), "%s", t.wave > 0.5f ? "Square" : "Sine");
        copy(ptr, text);
        return 0;
    }
    case vst2::op::canDo:
        if (ptr && !std::strcmp(static_cast<const char*>(ptr), MELODEE_SCREEN_VST2_CANDO)) return shown ? 1 : 0;
        return 0;
    case vst2::op::vendorSpecific:
        if (index != MELODEE_SCREEN_VST2) return 0;
        return shown ? reinterpret_cast<intptr_t>(&screen) : 1; // junk where it has none
    default: return 0;
    }
}
void setParameter(vst2::Effect* e, int32_t index, float value) { (index == 0 ? of(e).level : of(e).wave) = value; }
float getParameter(vst2::Effect* e, int32_t index) { return index == 0 ? of(e).level : of(e).wave; }
void processReplacing(vst2::Effect* e, float**, float** outputs, int32_t frames) {
    for (int32_t f = 0; f < frames; ++f) outputs[0][f] = outputs[1][f] = of(e).level;
}
} // namespace

extern "C" {
__attribute__((visibility("default"))) vst2::Effect* VSTPluginMain(vst2::HostCallback host) {
    auto* t = new Tone;
    t->host = host;
    auto& e = t->effect;
    e.magic = vst2::magic;
    e.dispatcher = dispatcher;
    e.setParameter = setParameter;
    e.getParameter = getParameter;
    e.numPrograms = 0;
    e.numParams = 2;
    e.numInputs = 0;
    e.numOutputs = 2;
    e.flags = vst2::flag::canReplace | vst2::flag::isSynth;
    e.object = t;
    e.uniqueId = ('T' << 24) | ('s' << 16) | ('T' << 8) | 'n';
    e.version = 1;
    e.processReplacing = processReplacing;
    return &e;
}
// What the host answered to canDo("melodeeScreen") when the last one opened: 1 yes.
__attribute__((visibility("default"))) int fixture_host_screen() { return hostScreen; }
}
