// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Test Tone, a small LV2 instrument for tests/lv2_host.cpp: a sine at the last
// MIDI note held, at its Level port; two presets (Quiet, Loud) in presets.ttl.
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>
#include <lv2/urid/urid.h>

#include <cmath>
#include <cstring>

namespace {
struct Tone {
    const LV2_Atom_Sequence* events = nullptr;
    float* out[2] {};
    const float* level = nullptr;
    LV2_URID midiEvent = 0;
    double rate = 48000, phase = 0, step = 0;
    int key = -1;
};

LV2_Handle make(const LV2_Descriptor*, double rate, const char*, const LV2_Feature* const* features) {
    LV2_URID_Map* map = nullptr;
    for (int i = 0; features && features[i]; ++i)
        if (!std::strcmp(features[i]->URI, LV2_URID__map)) map = static_cast<LV2_URID_Map*>(features[i]->data);
    if (!map) return nullptr;
    auto* t = new Tone;
    t->rate = rate;
    t->midiEvent = map->map(map->handle, LV2_MIDI__MidiEvent);
    return t;
}

void connect(LV2_Handle h, uint32_t port, void* data) {
    auto* t = static_cast<Tone*>(h);
    switch (port) {
    case 0: t->events = static_cast<const LV2_Atom_Sequence*>(data); break;
    case 1: t->out[0] = static_cast<float*>(data); break;
    case 2: t->out[1] = static_cast<float*>(data); break;
    case 3: t->level = static_cast<const float*>(data); break;
    }
}

void run(LV2_Handle h, uint32_t frames) {
    auto* t = static_cast<Tone*>(h);
    LV2_ATOM_SEQUENCE_FOREACH(t->events, ev) {
        if (ev->body.type != t->midiEvent) continue;
        const auto* m = reinterpret_cast<const uint8_t*>(ev + 1);
        if ((m[0] & 0xf0) == 0x90 && m[2] > 0) { t->key = m[1]; t->step = 2 * M_PI * 440 * std::pow(2.0, (m[1] - 69) / 12.0) / t->rate; }
        else if (((m[0] & 0xf0) == 0x80 || (m[0] & 0xf0) == 0x90) && m[1] == t->key) t->key = -1;
    }
    for (uint32_t f = 0; f < frames; ++f) {
        float x = t->key >= 0 ? float(*t->level * std::sin(t->phase)) : 0.0f;
        if (t->key >= 0) t->phase += t->step;
        t->out[0][f] = t->out[1][f] = x;
    }
}

void cleanup(LV2_Handle h) { delete static_cast<Tone*>(h); }

const LV2_Descriptor descriptor {"urn:melodee:test:tone", make, connect, nullptr, run, nullptr, cleanup, nullptr};
} // namespace

extern "C" LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) { return index == 0 ? &descriptor : nullptr; }
