# Companion screens

<!-- SPDX-License-Identifier: MIT -->
<!-- This format reference is MIT licensed, apart from the rest of Melodee, so that screen makers and other hosts
     may copy it. -->

A companion screen draws a plug-in's page on the MPC Sample's screen in
Melodee without changing the plug-in. It is installed apart from the
plug-in, by anyone, for a plug-in as it is released.

A plug-in can also draw its own screen ([Plug-in screens](PLUGIN_SCREENS.md)).
That needs its maker to build it in. A companion does not.

- The interface is one header, plain C, MIT licensed:
  [sdk/melodee-screen/melodee_companion.h](../sdk/melodee-screen/melodee_companion.h).
  It uses the frame and inputs of
  [melodee_screen.h](../sdk/melodee-screen/melodee_screen.h).
- An example: the Profit-5 panel,
  [experiments/profit5-screen](../experiments/profit5-screen/README.md).
- This version takes VST2 plug-ins, 64-bit and 32-bit ARM (the 32-bit ones
  through Melodee's bridge; the screen is always 64-bit). CLAP, VST3 and LV2
  come later through the same interface.

## The folder

```
Melodee/Plugin Screens/<folder>/
    screen.json
    screen-aarch64.so
    assets/          (its own files, if any)
    LICENSE
```

The plug-in stays in Melodee/Plugins as it is. Removing the screen's folder
leaves the plug-in and every project that uses it as they were; Melodee
shows its own pages for it again the next time the plug-in opens.

## screen.json

```json
{
  "name": "Profit-5 Panel",
  "author": "Kerem Kilic (Ellic Studio)",
  "version": "0.1.0",
  "sdk": 1,
  "library": "screen-aarch64.so",
  "plugin": {"vst2": "Pft5", "name": "Profit-5", "vendor": "sd88me"},
  "params": [
    {"index": 0, "name": "PM Filt Env"},
    {"index": 2, "name": "A Frequency", "steps": 60}
  ]
}
```

| Field | Meaning |
|-------|---------|
| `name` | The screen's name, as notices say it |
| `author`, `version` | For the person installing it |
| `sdk` | The interface version it was built for. A later one than Melodee has is refused |
| `library` | The library in the folder; `screen-aarch64.so` when left out |
| `plugin` | Which plug-in: `vst2` (its unique ID, four letters or a number), `clap`, `lv2`, or `name` and `vendor`, as a page file names one |
| `params` | The parameters the screen uses: the plug-in's `index` for each and the plug-in's `name` there. Their order makes the screen's handles: the first is handle 0 |
| `steps` | Positions − 1, for a parameter whose plug-in does not say (VST2 never does). 1 is a switch |

At most 512 parameters.

## When Melodee uses it

When a plug-in opens, Melodee reads each folder's screen.json. A screen fits
when it names the plug-in and every parameter it lists is at its index with
that exact name. A VST2 plug-in's indices can move between versions; the
names catch that.

In this order, Melodee shows:

1. the plug-in's own screen, when it draws one;
2. the one companion screen that fits;
3. a page file's pages or Melodee's own pages.

Two screens that fit the same plug-in: neither is used. Settings > SCREEN >
**Plug-in screens** off: none is used. Stage's **EDIT** stays Melodee's
pages in every case.

A screen that does not fit, cannot be read, or stopped is said in
Melodee/Plugin Screens/problems.txt, one line each:

```
Profit-5 Panel: not for this Profit-5: parameter 13 is "Cutoff", not "Cutoff Frequency"
```

## How it runs

- Melodee runs the screen in a process of its own, at normal priority, with
  its memory limited. One screen is open at a time: the one in view. A
  screen shown again starts afresh (its row and focus are not kept).
- The screen reads copies of the parameters and programs, which Melodee
  reads from the plug-in on its own thread. It never calls the plug-in.
- What the screen asks for, Melodee does the way a pot turn on the plug-in's
  card does it (a frozen track asks first). K1-K3 on the screen's three are
  Melodee's pots, with pickup and Erase + pot as on any card. A program it
  picks is picked as PRESETS picks one.
- A value the screen asked for reads back at once, before Melodee has made
  it, so two encoder detents in a row add up.
- The screen is stopped, and Melodee's pages come back with a notice, when
  it crashes, exits, takes longer than half a second to draw, sends more
  than 192 messages for one draw, sends a message Melodee cannot read, or
  asks for a handle or program the plug-in does not have. A stopped screen
  is not tried again until Melodee restarts.
- The plug-in's sound and saved state are the plug-in's. A project never
  needs a companion screen to open.

## The calls

See the header for each call. In short:

| The screen exports | |
|--------------------|-|
| `melodee_companion_entry(host_abi)` | its table, or NULL when it cannot run on that host |
| `create(host)`, `destroy(screen)` | one screen per plug-in instance |
| `draw(screen, frame)` | as melodee_screen.h's draw |
| `input(screen, input)` | wheel, Push, Minus / Plus |
| `controls(screen, handles)` | the handles K1-K3 turn now |

| The host gives | |
|----------------|-|
| `param(host, handle, out)` | value, default, steps, name, text |
| `set(host, handle, value)` | ask for a value, 0..1 |
| `program(host, out)`, `select_program(host, n)` | the programs, and pick one |
| `redraw(host)` | draw again soon |
| `extension(host, id)` | later additions; none yet |

## Tools

Run on Linux aarch64 (Melodee's build image) with Melodee's binary:

| Command | Does |
|---------|------|
| `melodee --melodee-plugin-params PLUGIN.so` | lists the plug-in's parameters: index, name, value, text, default |
| `melodee --melodee-companion-check PLUGIN.so FOLDER` | checks a screen's folder against the plug-in, as Melodee does |
| `melodee --melodee-companion-screen PLUGIN.so FOLDER OUT [STEP...]` | draws the screen as Stage's PANEL shows it, to OUT/0.png and one after each step |

Steps: `wheel:N`, `push`, `minus`, `plus`, `shift`, `k1:V` to `k3:V` (a pot
to 0..127), `param:INDEX:V` and `program:N` (changes from outside the
screen), `play`, `stop`, `wait:MS`.

[tests/test_companions.py](../tests/test_companions.py) runs all of them
against a plug-in and its screen.

## Not yet

- Picking a screen by hand when several fit, and keeping that pick.
- The web page's Screens section: install, update, turn off, remove.
- CLAP, VST3 and LV2 plug-ins.
