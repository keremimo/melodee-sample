# Plug-in screens

<!-- SPDX-License-Identifier: MIT -->
<!-- This format reference is MIT licensed, apart from the rest of Melodee, so that plug-in makers and other hosts
     may copy it. -->

A plug-in can draw its own page on the MPC Sample's screen in Melodee, with
its own code: its own look, pictures that move with the sound, its own way of
using the encoder. It is an extension the plug-in offers, not a special build:
the same plug-in file works in every other host, which never asks for it.
Melodee needs no change for a new plug-in.

- CLAP plug-ins and VST2 plug-ins, built for the MPC Sample (aarch64 Linux)
  as any plug-in that runs there must be. 32-bit ARM VST2 plug-ins work too:
  Melodee runs them in its bridge, and they need nothing different.
- The format is one header, plain C:
  [sdk/melodee-screen/melodee_screen.h](../sdk/melodee-screen/melodee_screen.h),
  MIT licensed. Copy it into the plug-in.
- An example, one source for both formats:
  [sdk/melodee-screen/example](../sdk/melodee-screen/example/).

A plug-in that writes no code can still change how it looks in Melodee with
a page file and a skin: [Plug-in page files](PLUGIN_PAGES.md).

## Where it shows

| Where | What the plug-in draws |
|-------|------------------------|
| Stage, B1 **PANEL** (between PRESETS and EDIT) on a PLUGIN track | its page: everything under B1-B3, 320 × 218 |
| An effect in a track, bus or master chain | its strip: everything under the chain's slots, 320 × 144 |
| Push on that effect | its page |

- Melodee keeps **B1**–**B3**, its MENU, notices and questions, drawn over
  the page. It draws nothing over the plug-in's own controls.
- Stage's **EDIT** is always Melodee's pages of its parameters.
- Settings > SCREEN > **Plug-in screens** off: Melodee's own pages for every
  plug-in.

## The calls

The plug-in offers a table of three calls, `melodee_screen_t`:

- `draw(plugin, frame)`: draw into `frame->pixels`; return true if anything
  new was drawn, false if nothing changed.
- `input(plugin, input)`: an input on its page; return true if it used it.
- `controls(plugin, ids)`: the three parameters **K1**–**K3** turn, in the
  format's own IDs (a CLAP param id, a VST2 parameter index);
  `MELODEE_SCREEN_NO_CONTROL` for a pot that turns nothing. Asked after every
  input and draw, so the three may change with what the page shows.

Melodee offers one call back, `redraw(host, flags)`, from any thread: the
picture changed (a value, a meter, an animation's next frame), so Melodee
calls `draw()` soon. With `MELODEE_SCREEN_RUN` it also runs the plug-in's
next block though it is silent, for an input its audio side must hear
(Melodee does not run a silent plug-in).

Version 2 of Melodee's table (`melodee_screen_host_2_t`, which starts with
`redraw` as version 1 does) adds `keys(host, keys)`: the printed buttons the
plug-in's page takes from now on, `MELODEE_SCREEN_KEY_MUTE` and
`MELODEE_SCREEN_KEY_ERASE`, or 0 for none. Call it when that changes, from any
thread. See [Printed buttons](#printed-buttons).

### The frame

| Field | What |
|-------|------|
| `pixels`, `stride` | 32-bit pixels, 0x00RRGGBB, row after row; they hold the plug-in's last picture |
| `width`, `height` | 320 × 218 (the page) or 320 × 144 (the strip) |
| `area` | `MELODEE_SCREEN_PAGE` or `MELODEE_SCREEN_STRIP` |
| `redraw_all` | 1: the pixels are not its last picture; draw every one |
| `colour` | the track's colour |
| `time_ms` | Melodee's clock, for animation |
| `playing`, `shift` | Melodee plays; **SHIFT** is held |
| `pots[3]`, `pots_live[3]` | where **K1**–**K3** sit (0-127, -1 not known yet), and whether each drives its value yet |

### Inputs

| `kind` | When | `value` |
|--------|------|---------|
| `MELODEE_SCREEN_WHEEL` | the encoder turned | detents, clockwise positive |
| `MELODEE_SCREEN_PUSH` | the encoder pushed and let go | 1 |
| `MELODEE_SCREEN_STEP` | **−** or **+** | -1 or 1 |
| `MELODEE_SCREEN_MUTE` | **MUTE** tapped, while its page takes it | 1 |
| `MELODEE_SCREEN_ERASE` | **ERASE** tapped, while its page takes it | 1 |

`shift` says whether **SHIFT** is held. The encoder held opens Melodee's MENU
and **SHIFT** + push goes back; neither reaches the plug-in.

### Printed buttons

On a plug-in's page **MUTE** and **ERASE** do Melodee's jobs: **MUTE** tapped
latches mute mode, **ERASE** has nothing to erase. A page with a list of its
own, where something can be switched off or removed (an effect chain, a step
list), can take their taps instead, as Melodee's own chains do: tell Melodee
with `keys()` while such a thing is lit, and 0 when it is not (an empty slot:
**MUTE** latches mute mode again, as on a chain's **+**).

- **MUTE** tapped (pressed and let go with no pad between) comes as
  `MELODEE_SCREEN_MUTE`. Held, with pads, it still mutes tracks.
- **ERASE** tapped on its own comes as `MELODEE_SCREEN_ERASE`, and Melodee
  lights **ERASE** while the page takes it. **SHIFT** + **ERASE** stays
  Melodee's copy.
- Only on the plug-in's page (on a track's **PANEL**, or an effect's full
  page). On an effect's strip the chain's own **MUTE** and **ERASE** act on
  the slot.

## K1-K3 and pickup

Melodee binds **K1**–**K3** to the three parameters `controls()` names, as it
binds any plug-in parameter: takeover, **ERASE** + pot (the default), the
fader, **SHIFT** + encoder fine steps.

Each control is drawn once, by the plug-in. Melodee draws no ring or card
over it, so the plug-in shows the pickup itself: while `pots_live[k]` is 0
the pot has not reached the value, and `pots[k]` says where it sits (the
example draws a small mark there).

## Each format

**CLAP.** The plug-in's `get_extension(MELODEE_SCREEN_CLAP)` returns its
table, `plugin` being its `clap_plugin_t*`. Melodee's
`get_extension(MELODEE_SCREEN_CLAP_HOST)` returns the `redraw` call, `host`
being the `clap_host_t*`; `get_extension(MELODEE_SCREEN_CLAP_HOST_2)` returns
`redraw` and `keys` (null in a Melodee without it: no keys).

**VST2.** Melodee first asks `effCanDo("melodeeScreen")`; a plug-in with a
screen answers 1. Only then does it call `effVendorSpecific` (50) with index
`MELODEE_SCREEN_VST2`, and the plug-in returns its table as an `intptr_t`,
`plugin` being its `AEffect*`. The plug-in finds Melodee with
`audioMasterCanDo("melodeeScreen")` returning 1, and asks for a draw with
`audioMasterVendorSpecific` (35), index `MELODEE_SCREEN_VST2`, value the
flags. Once `audioMasterCanDo("melodeeScreenKeys")` returned 1, it tells its
keys with `audioMasterVendorSpecific`, index `MELODEE_SCREEN_VST2_KEYS`, value
the keys.

**32-bit ARM VST2.** The same, without keys: Melodee's bridge makes the calls
in the plug-in's own process, and does not pass `keys` on yet.

## Threads

`draw()`, `input()` and `controls()` come from a thread of Melodee's own:
never the audio thread, and never while another of the plug-in's non-audio
calls runs (CLAP's main-thread calls; VST2's dispatcher calls other than
processing). Each should take under 2 ms. `redraw()` may be called from any
thread, the audio thread too.

## Drawing well

- Return false from `draw()` when nothing it shows changed: the screen then
  does no work at all.
- Draw only what changed, unless `redraw_all` says otherwise; the pixels keep
  the last picture.
- The panel sends only the rectangles that changed. A needle or a light costs
  little; a picture that changes all over runs at about 18 frames a second.
- The panel shows 16-bit colour (RGB565): gradients look smoother dithered.
- Text 11 pixels tall or more stays readable at arm's length.
- Ask for draws as the sound moves (a meter) at most every 30 ms or so, and
  stop asking when nothing moves.

## Without an MPC

Melodee's own binary opens a plug-in as Edit shows it and writes the screen as
PNGs into a folder: `0.png`, then one after each step.

```sh
melodee --melodee-plugin-screen screen-example.clap out wheel:10 plus push k1:64 shift
```

Steps: `wheel:N`, `push`, `minus`, `plus`, `shift` (held until `shift`
again), `k1:V`, `k2:V`, `k3:V` (a pot to V, 0-127, driving its parameter from
then), `play`, `stop`, `wait:MS`. A CLAP library's first plug-in, or a 64-bit
VST2 `.so`.

## The example

[sdk/melodee-screen/example](../sdk/melodee-screen/example/): Screen
Example, a drive effect (Drive, Tone, Mix). It draws its name, the clip's
curve, the levels in and out and its three knobs, on its page and its strip;
**−** / **+** pick a knob, the wheel turns it, push sets it back to its
default. One C++ file, one library with both entry points: name it `.clap`
or `.so`.

```sh
cd sdk/melodee-screen/example
make CLAP_INCLUDE=/path/to/clap/include   # screen-example.clap and .so
make armv7                                # screen-example-armv7.so
```
