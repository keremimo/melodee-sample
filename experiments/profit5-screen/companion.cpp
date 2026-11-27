// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The Profit-5 panel (screen.cpp) as a companion screen (melodee_companion.h): installed beside the released Profit-5,
// never built into it. screen.json lists the 59 parameters the panel reads, in the plug-in's own order, so each
// handle is that parameter's index. Every value, text, step count and program name comes from Melodee's copies;
// every change is asked of Melodee.
#include "screen.h"
#include <melodee_companion.h>
#include <cstdio>
#include <new>

namespace {
constexpr uint32_t panel_params = 59; // 0..57 the synth, 58 Program

struct Companion {
    const melodee_companion_host_t* host = nullptr;
    void* ui = nullptr;
};

melodee_companion_param_t param(void* p, int id) {
    auto* c = static_cast<Companion*>(p);
    melodee_companion_param_t v {};
    if (id < 0 || !c->host->param(c->host->host, uint32_t(id), &v)) v = {};
    return v;
}
float get(void* p, int id) { return param(p, id).value; }
void set(void* p, int id, float value) {
    auto* c = static_cast<Companion*>(p);
    if (id >= 0) c->host->set(c->host->host, uint32_t(id), value);
}
void text(void* p, int id, char* out, int size) {
    const auto v = param(p, id);
    std::snprintf(out, size_t(size), "%s", v.text);
}
void program(void* p, char* out, int size) {
    auto* c = static_cast<Companion*>(p);
    melodee_companion_program_t now {};
    c->host->program(c->host->host, &now);
    std::snprintf(out, size_t(size), "%s", now.name);
}
int steps(void* p, int id) { return int(param(p, id).steps); }

void* create(const melodee_companion_host_t* host) {
    if (!host || host->abi < 1 || host->param_count < panel_params || !host->param || !host->set || !host->program) return nullptr;
    auto* c = new (std::nothrow) Companion;
    if (!c) return nullptr;
    c->host = host;
    c->ui = profit5_ui_create(profit5_ui_host {c, get, set, text, program, steps});
    if (!c->ui) { delete c; return nullptr; }
    return c;
}
void destroy(void* screen) {
    auto* c = static_cast<Companion*>(screen);
    profit5_ui_destroy(c->ui);
    delete c;
}
bool draw(void* screen, const melodee_screen_frame_t* frame) { return profit5_ui_draw(static_cast<Companion*>(screen)->ui, frame); }
bool input(void* screen, const melodee_screen_input_t* in) { return profit5_ui_input(static_cast<Companion*>(screen)->ui, in); }
void controls(void* screen, uint32_t handles[3]) { profit5_ui_controls(static_cast<Companion*>(screen)->ui, handles); }

const melodee_companion_t table {MELODEE_COMPANION_ABI, sizeof(melodee_companion_t), create, destroy, draw, input, controls, nullptr};
} // namespace

extern "C" __attribute__((visibility("default"))) const melodee_companion_t* melodee_companion_entry(uint32_t host_abi) {
    return host_abi >= 1 ? &table : nullptr;
}
