// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <melodee_screen.h>

#ifdef __cplusplus
extern "C" {
#endif

// All callbacks are UI-thread calls. The upstream engine and its audio path are unchanged.
typedef struct {
    void* instance;
    float (*get)(void*, int);
    void (*set)(void*, int, float);
    void (*text)(void*, int, char*, int);
    void (*program)(void*, char*, int);
    int (*steps)(void*, int);
} profit5_ui_host;

void* profit5_ui_create(profit5_ui_host host);
void profit5_ui_destroy(void* ui);
bool profit5_ui_draw(void* ui, const melodee_screen_frame_t* frame);
bool profit5_ui_input(void* ui, const melodee_screen_input_t* input);
void profit5_ui_controls(void* ui, uint32_t ids[3]);

#ifdef __cplusplus
}
#endif
