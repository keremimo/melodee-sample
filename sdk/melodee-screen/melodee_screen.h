/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio)
 *
 * Melodee screens: a plug-in draws its own page on the MPC Sample's screen in Melodee.
 *
 * MIT licensed, apart from the rest of Melodee, so that any plug-in may copy this file. Plain C, fixed-width types,
 * no CLAP or VST2 headers needed. A plug-in that offers a screen loads in any other host as before: the calls below
 * are never made there.
 *
 * The page is 320 x 218 pixels, everything under Melodee's soft keys (B1-B3). In a track's FX chain an effect gets
 * a strip, 320 x 144, under the chain's slots. Melodee keeps B1-B3, its MENU, notices and questions, drawn over the
 * page. Melodee draws nothing over the plug-in's controls: each control is drawn once, by the plug-in, with its own
 * pickup mark where a pot has not reached its value yet (`pots`, `pots_live`).
 *
 * The panel shows 16-bit colour (RGB565): gradients look smoother dithered. Changed rectangles are sent to the
 * panel, so a needle or a light costs little; a picture that changes all over tops out near 18 frames a second.
 *
 * Threads: draw(), input() and controls() come from a thread of Melodee's own, never the audio thread and never at
 * the same time as the plug-in's other non-audio calls (CLAP's main-thread calls). Each should take under 2 ms.
 * redraw() may be called from any thread.
 *
 * CLAP: the plug-in's get_extension(MELODEE_SCREEN_CLAP) returns its melodee_screen_t, `plugin` being its
 * clap_plugin_t*. The host's get_extension(MELODEE_SCREEN_CLAP_HOST) returns a melodee_screen_host_t, `host` being
 * the clap_host_t* the plug-in was given.
 *
 * VST2: the host first calls dispatcher(effect, effCanDo = 51, 0, 0, MELODEE_SCREEN_VST2_CANDO, 0); a plug-in with a
 * screen answers 1. Only then does it call dispatcher(effect, effVendorSpecific = 50, MELODEE_SCREEN_VST2, 0, NULL, 0),
 * and the plug-in returns its melodee_screen_t as an intptr_t (0: none), `plugin` being its AEffect*. The plug-in
 * finds Melodee by audioMaster(effect, audioMasterCanDo = 37, 0, 0, MELODEE_SCREEN_VST2_CANDO, 0) returning 1, and
 * asks for a draw with audioMaster(effect, audioMasterVendorSpecific = 35, MELODEE_SCREEN_VST2, flags, NULL, 0).
 *
 * Printed buttons: by default MUTE and ERASE do Melodee's jobs on a plug-in's page. A plug-in can take their taps
 * while its page needs them (a list of its own with something to switch off or remove): it says which with keys()
 * (CLAP: melodee_screen_host_2_t, from get_extension(MELODEE_SCREEN_CLAP_HOST_2); VST2: audioMaster(effect, 35,
 * MELODEE_SCREEN_VST2_KEYS, keys, NULL, 0), once audioMasterCanDo(MELODEE_SCREEN_VST2_KEYS_CANDO) returned 1), and
 * gets them as MELODEE_SCREEN_MUTE and MELODEE_SCREEN_ERASE inputs. MUTE held with pads still mutes, as everywhere.
 * Plug-ins run through Melodee's 32-bit bridge (armv7 VST2) cannot take them yet.
 */
#ifndef MELODEE_SCREEN_H
#define MELODEE_SCREEN_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MELODEE_SCREEN_CLAP "melodee.screen/1"
#define MELODEE_SCREEN_CLAP_HOST "melodee.screen-host/1"
#define MELODEE_SCREEN_CLAP_HOST_2 "melodee.screen-host/2" /* melodee_screen_host_2_t: redraw() and keys() */
#define MELODEE_SCREEN_VST2 0x4D4C5343 /* 'MLSC' */
#define MELODEE_SCREEN_VST2_CANDO "melodeeScreen"
#define MELODEE_SCREEN_VST2_KEYS 0x4D4C534B /* 'MLSK' */
#define MELODEE_SCREEN_VST2_KEYS_CANDO "melodeeScreenKeys"

enum {
    MELODEE_SCREEN_WIDTH = 320,
    MELODEE_SCREEN_PAGE_HEIGHT = 218,
    MELODEE_SCREEN_STRIP_HEIGHT = 144,
};

/* melodee_screen_frame_t.area */
enum {
    MELODEE_SCREEN_PAGE = 0,  /* the page under B1-B3 */
    MELODEE_SCREEN_STRIP = 1, /* an effect's strip under its chain's slots */
};

/* melodee_screen_input_t.kind */
enum {
    MELODEE_SCREEN_WHEEL = 0, /* the encoder turned: `value` detents, clockwise positive */
    MELODEE_SCREEN_PUSH = 1,  /* the encoder pushed and let go (held, it opens Melodee's MENU instead) */
    MELODEE_SCREEN_STEP = 2,  /* Minus (`value` -1) or Plus (+1) */
    MELODEE_SCREEN_MUTE = 3,  /* MUTE tapped (pressed and let go, no pad between), while keys() has MELODEE_SCREEN_KEY_MUTE */
    MELODEE_SCREEN_ERASE = 4, /* ERASE tapped on its own, while keys() has MELODEE_SCREEN_KEY_ERASE */
};

/* keys() bits: the printed buttons a plug-in's page takes */
enum {
    MELODEE_SCREEN_KEY_MUTE = 1,
    MELODEE_SCREEN_KEY_ERASE = 2,
};

/* redraw() flags */
enum {
    MELODEE_SCREEN_RUN = 1, /* also run the plug-in's next block although it is silent (an input its audio side takes) */
};

#define MELODEE_SCREEN_NO_CONTROL 0xFFFFFFFFu

/* What the plug-in draws into. The pixels hold the picture it drew last time, so it may draw only what changed,
 * unless `redraw_all` says to draw all of it. */
typedef struct melodee_screen_frame {
    uint32_t* pixels;     /* 0x00RRGGBB, `height` rows of `stride` pixels; the top byte is not used */
    uint32_t width;       /* MELODEE_SCREEN_WIDTH */
    uint32_t height;      /* MELODEE_SCREEN_PAGE_HEIGHT or MELODEE_SCREEN_STRIP_HEIGHT */
    uint32_t stride;
    uint32_t area;        /* MELODEE_SCREEN_PAGE or MELODEE_SCREEN_STRIP */
    uint32_t redraw_all;  /* 1: the pixels are not this plug-in's last picture: draw every one */
    uint32_t colour;      /* the track's colour, 0x00RRGGBB */
    uint64_t time_ms;     /* Melodee's clock, for animation */
    uint8_t playing;      /* 1 while Melodee plays */
    uint8_t shift;        /* 1 while SHIFT is held */
    uint8_t pots_live[3]; /* K1-K3: 1 the pot drives its value now, 0 it has yet to reach it */
    uint8_t reserved;
    int16_t pots[3];      /* K1-K3: where each pot sits, 0..127; -1 not known yet */
} melodee_screen_frame_t;

typedef struct melodee_screen_input {
    uint32_t kind;  /* MELODEE_SCREEN_WHEEL, _PUSH, _STEP, _MUTE, _ERASE */
    int32_t value;
    uint32_t shift; /* 1 while SHIFT is held */
} melodee_screen_input_t;

typedef struct melodee_screen {
    /* Draws its picture into `frame`. Returns true if it drew anything new, false if nothing changed. */
    bool (*draw)(const void* plugin, const melodee_screen_frame_t* frame);
    /* An input on its page. Returns true if it used it. */
    bool (*input)(const void* plugin, const melodee_screen_input_t* input);
    /* The parameters K1-K3 turn now, in the format's own IDs (a CLAP param id, a VST2 parameter index);
     * MELODEE_SCREEN_NO_CONTROL for a pot that turns nothing. Asked after each input and each draw. */
    void (*controls)(const void* plugin, uint32_t ids[3]);
} melodee_screen_t;

typedef struct melodee_screen_host {
    /* Its picture changed (a value, a meter, an animation's next frame): Melodee calls draw() soon. `flags`:
     * MELODEE_SCREEN_RUN or 0. */
    void (*redraw)(const void* host, uint32_t flags);
} melodee_screen_host_t;

typedef struct melodee_screen_host_2 {
    void (*redraw)(const void* host, uint32_t flags); /* as in melodee_screen_host_t */
    /* The printed buttons its page takes from now on: MELODEE_SCREEN_KEY_ bits, 0 none (they do Melodee's jobs).
     * Call it when that changes, from any thread. Melodee lights ERASE while the page in view takes it. */
    void (*keys)(const void* host, uint32_t keys);
} melodee_screen_host_2_t;

#ifdef __cplusplus
}
#endif

#endif /* MELODEE_SCREEN_H */
