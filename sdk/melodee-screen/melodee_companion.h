/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio)
 *
 * Melodee companion screens: a screen for a plug-in, installed apart from the plug-in.
 *
 * MIT licensed, apart from the rest of Melodee, so that any screen may copy this file. Plain C, fixed-width types.
 *
 * A plug-in can draw its own screen (melodee_screen.h): its maker builds that into the plug-in. A companion screen is
 * for a plug-in as it is released. It is a library of its own in a folder on the card:
 *
 *     Melodee/Plugin Screens/<folder>/
 *         screen.json            what the screen is for (docs/PLUGIN_COMPANIONS.md)
 *         screen-aarch64.so      this interface
 *         assets/, LICENSE       its own files
 *
 * screen.json names the plug-in (its CLAP id, VST2 unique ID, LV2 URI, or VST3 name and vendor) and lists the
 * parameters the screen uses, each by the plug-in's index and the plug-in's name for it. Melodee checks the plug-in
 * against that list when it opens it: a parameter missing, or with another name at that index (a VST2 plug-in's
 * indices can move between versions), and the screen is not used; Melodee shows its own pages and says why in
 * Melodee/Plugin Screens/problems.txt.
 *
 * A companion never touches the plug-in. It sees each parameter by its handle, its place in screen.json's list
 * (0, 1, 2...), as a copy Melodee keeps, and asks Melodee for every change; Melodee makes the change as a pot turn
 * would (pickup, Erase + pot to the default, undo, a frozen track's question), on the plug-in's own thread.
 *
 * It runs in a process of its own. One that crashes, hangs, floods Melodee with requests or sends what Melodee cannot
 * read is stopped, and Melodee's own pages come back. The plug-in's sound and saved state are the plug-in's: a project
 * never needs a companion to open.
 *
 * The page, the strip, the frame and the inputs are melodee_screen.h's: 320 x 218 pixels under B1-B3 (a strip of
 * 320 x 144 for an effect in a chain), 0x00RRGGBB, `redraw_all`, pots and pickup marks drawn by the screen itself.
 *
 * Threads: every call comes from one thread, never two at once. The host's functions return at once and may only be
 * called from inside create(), draw(), input() and controls(). Each draw should take under 2 ms.
 */
#ifndef MELODEE_COMPANION_H
#define MELODEE_COMPANION_H

#include "melodee_screen.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The version of this interface; screen.json's "sdk" says which one a screen was built for. */
#define MELODEE_COMPANION_ABI 1
/* The function the library exports. */
#define MELODEE_COMPANION_ENTRY "melodee_companion_entry"

/* One parameter, as Melodee last read it from the plug-in. */
typedef struct melodee_companion_param {
    float value;   /* 0..1 */
    float initial; /* 0..1, the plug-in's default */
    uint32_t steps; /* 0 continuous; n: n + 1 positions (a switch: 1). The plug-in's own, else screen.json's "steps" */
    uint32_t flags; /* reserved: 0 */
    char name[32];  /* the plug-in's name for it */
    char text[32];  /* the plug-in's text for its value */
} melodee_companion_param_t;

/* The plug-in's programs (presets), as Melodee lists them on PRESETS. */
typedef struct melodee_companion_program {
    int32_t current; /* the one picked, -1 none */
    uint32_t count;  /* how many */
    char name[64];   /* the current one's name, "" none */
} melodee_companion_program_t;

typedef struct melodee_companion_host {
    uint32_t abi;  /* MELODEE_COMPANION_ABI as the host has it */
    uint32_t size; /* sizeof(melodee_companion_host_t) as the host has it: a field past it is not there */
    const void* host;
    uint32_t param_count; /* screen.json's parameters: handles 0 .. param_count - 1 */
    uint32_t reserved;
    /* A parameter's copy. False: no such handle. */
    bool (*param)(const void* host, uint32_t handle, melodee_companion_param_t* out);
    /* Asks for the parameter at `value`, 0..1. The copy shows it once Melodee has made the change. */
    void (*set)(const void* host, uint32_t handle, float value);
    /* The programs, and asks for program `program` (0 .. count - 1), as PRESETS would pick it. */
    void (*program)(const void* host, melodee_companion_program_t* out);
    void (*select_program)(const void* host, int32_t program);
    /* Its picture changed without an input or a parameter change (an animation's next frame): draw() soon. */
    void (*redraw)(const void* host);
    /* A later version's addition by its id, NULL when this host has none (none yet). */
    const void* (*extension)(const void* host, const char* id);
} melodee_companion_host_t;

typedef struct melodee_companion {
    uint32_t abi;  /* MELODEE_COMPANION_ABI the screen was built with */
    uint32_t size; /* sizeof(melodee_companion_t) as the screen has it */
    /* A screen for one plug-in instance: each instance has its own, never shared. NULL: it could not be made. The
     * host table stays valid until destroy(). */
    void* (*create)(const melodee_companion_host_t* host);
    void (*destroy)(void* screen);
    /* As melodee_screen_t's: true if it drew anything new. */
    bool (*draw)(void* screen, const melodee_screen_frame_t* frame);
    /* An input on its page. True if it used it. */
    bool (*input)(void* screen, const melodee_screen_input_t* input);
    /* The handles K1-K3 turn now, MELODEE_SCREEN_NO_CONTROL for a pot that turns nothing. Asked after each input and
     * each draw. */
    void (*controls)(void* screen, uint32_t handles[3]);
    /* A later version's addition by its id; NULL none. May be NULL itself. */
    const void* (*extension)(void* screen, const char* id);
} melodee_companion_t;

/* The library's export: its table for a host of version `host_abi`, NULL when it cannot run there. */
typedef const melodee_companion_t* (*melodee_companion_entry_t)(uint32_t host_abi);

#ifdef __cplusplus
}
#endif

#endif /* MELODEE_COMPANION_H */
