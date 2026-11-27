// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Compile the pinned Open MPC wrapper intact, then add the optional Melodee extension.
// Its AEffect.user field is unused by that wrapper; the screen lives there per instance.
#define VSTPluginMain profit5_original_main
#include "vst2_wrap.c"
#undef VSTPluginMain
#include "screen.h"

typedef struct {
    void* ui;
    audioMasterCallback host;
    int screen_host;
} screen_instance;

static void request_draw(AEffect* e, int flags) {
    screen_instance* s = e->user;
    if (s && s->host && s->screen_host) s->host(e, 35, MELODEE_SCREEN_VST2, flags, NULL, 0);
}
static intptr_t screen_host(AEffect* e, int32_t op, int32_t idx, intptr_t v, void* p, float o) {
    screen_instance* s = e ? e->user : NULL;
    if (!s || !s->host) return 0;
    // Upstream announces MIDI/program-driven changes here. The callback remains bounded on the audio thread.
    if (op == audioMasterAutomate || op == audioMasterUpdateDisplay) request_draw(e, 0);
    return s->host(e, op, idx, v, p, o);
}
static void screen_set(AEffect* e, int32_t id, float value) {
    setParameter(e, id, value);
    request_draw(e, MELODEE_SCREEN_RUN);
}

static float ui_get(void* p, int id) { return getParameter(p, id); }
static void ui_set(void* p, int id, float value) {
    AEffect* e = p;
    wrap_t* w = e->object;
    if (w->master) w->master(e, 43, id, 0, NULL, 0); // audioMasterBeginEdit
    setParameter(e, id, value);
    if (w->master) {
        w->master(e, audioMasterAutomate, id, 0, NULL, getParameter(e, id));
        w->master(e, 44, id, 0, NULL, 0); // audioMasterEndEdit
        request_draw(e, MELODEE_SCREEN_RUN);
    }
}
static void ui_text(void* p, int id, char* out, int size) {
    char value[PARAM_TEXT_MAX] = {0};
    dispatcher(p, effGetParamDisplay, id, 0, value, 0);
    snprintf(out, (size_t)size, "%s", value);
}
static void ui_program(void* p, char* out, int size) {
    char value[64] = {0};
    dispatcher(p, effGetProgramName, 0, 0, value, 0);
    snprintf(out, (size_t)size, "%s", value);
}
static int ui_steps(void* p, int id) {
    (void)p;
    if (PARAMS[id].nopts > 1) return PARAMS[id].nopts - 1;
    if (PARAMS[id].int_display) return (int)(PARAMS[id].max - PARAMS[id].min);
    return 0;
}
static bool screen_draw(const void* p, const melodee_screen_frame_t* f) {
    return profit5_ui_draw(((screen_instance*)((const AEffect*)p)->user)->ui, f);
}
static bool screen_input(const void* p, const melodee_screen_input_t* in) {
    return profit5_ui_input(((screen_instance*)((const AEffect*)p)->user)->ui, in);
}
static void screen_controls(const void* p, uint32_t ids[3]) {
    profit5_ui_controls(((screen_instance*)((const AEffect*)p)->user)->ui, ids);
}
static const melodee_screen_t screen_api = {screen_draw, screen_input, screen_controls};
static intptr_t screen_dispatcher(AEffect* e, int32_t op, int32_t idx, intptr_t v, void* p, float o) {
    screen_instance* s = e->user;
    if (s->ui && op == effCanDo && p && !strcmp(p, MELODEE_SCREEN_VST2_CANDO)) return 1;
    if (s->ui && op == 50 && idx == MELODEE_SCREEN_VST2) return (intptr_t)&screen_api;
    if (op == effClose) {
        profit5_ui_destroy(s->ui);
        intptr_t result = dispatcher(e, op, idx, v, p, o);
        free(s);
        return result;
    }
    intptr_t result = dispatcher(e, op, idx, v, p, o);
    if (op == effSetProgram || op == effSetChunk || op == effProcessEvents) request_draw(e, 0);
    return result;
}
__attribute__((visibility("default"))) AEffect* VSTPluginMain(audioMasterCallback host) {
    AEffect* e = profit5_original_main(screen_host);
    if (!e) return NULL;
    screen_instance* s = calloc(1, sizeof(*s));
    if (!s) { dispatcher(e, effClose, 0, 0, NULL, 0); return NULL; }
    s->host = host;
    s->screen_host = host && host(e, 37, 0, 0, MELODEE_SCREEN_VST2_CANDO, 0) == 1;
    profit5_ui_host h = {e, ui_get, ui_set, ui_text, ui_program, ui_steps};
    s->ui = profit5_ui_create(h);
    e->user = s;
    e->dispatcher = screen_dispatcher;
    e->setParameter = screen_set;
    return e;
}
