<!-- SPDX-License-Identifier: MIT -->
# Profit-5 / Melodee screen experiment

The companion screen example ([docs/PLUGIN_COMPANIONS.md](../../docs/PLUGIN_COMPANIONS.md)).
The embedded build further down (`build.py`) links Profit-5's GPL source into
its library: it stays a local comparison, never published.

A native 320 × 218 Profit-5 panel using the **Melodee screen SDK**
(`melodee.screen/1`), for Melodee on the MPC Sample. Six sections, 22 rows,
all 58 synth controls and the 128-slot Program control. Warm ivory type,
amber arcs, waveform/envelope diagrams, and one control per hardware pot.
The white dot outside a knob is its physical pot's pickup position; the
underline marks encoder focus and the amber lamp marks encoder editing.

This is a separate experiment alongside Melodee's source. Downloads, fonts,
generated files, binaries and captures all live in the ignored
`build/profit5-screen/` directory.

## Companion package (exp/companion-screens)

The same panel as a **companion screen**
([docs/PLUGIN_COMPANIONS.md](../../docs/PLUGIN_COMPANIONS.md)): a package
installed beside the released Profit-5, which stays as it is. Nothing of
Profit-5 is compiled into it.

- `companion.cpp`: the panel's host calls answered from Melodee's copies
  (`melodee_companion.h`); `screen.cpp` unchanged but for its `steps` call,
  which now gets the instance.
- `screen.json`: the 59 parameters the panel reads, by the released
  plug-in's indices and names (Profit-5 3.0.2: indices 0-63 are locked to
  the 0.2.1 wrapper's order), with step counts from upstream's
  `vst/params.json`.
- `build_companion.py`: builds `build/profit5-screen/package/Profit-5 Panel/`
  (screen-aarch64.so, screen.json, LICENSE). Needs `fonts.h` from build.py.

```sh
python3 experiments/profit5-screen/build_companion.py
python3 tests/test_companions.py \
  "build/profit5-release/aarch64/Profit-5-3.0.2/portable/sd88me - VST - Profit-5/profit_5.so" \
  "build/profit5-screen/package/Profit-5 Panel"
```

The release zips (Profit-5-3.0.2-mpc-aarch64.zip and -armv7.zip, tag
profit-5-vst-v3.0.2) are unpacked into `build/profit5-release/`. To try it on
the MPC: the release's `sd88me - VST - Profit-5` folder in Melodee/Plugins,
the package folder in Melodee/Plugin Screens, a PLUGIN track with Profit-5,
Stage B1 **PANEL**.

The embedded build below (`bridge.c`, `build.py`) is kept for comparison.

## Build and review

From the Melodee repository root:

```sh
python3 experiments/profit5-screen/build.py
```

Requires Docker and the existing `melodee-builder` image (created by
Melodee's `./build.sh`), Git, Python with venv/pip, and an existing
`build/melodee`. The first run fetches pinned source and installs Pillow
12.0.0 into an isolated build-only environment. It builds a Linux aarch64
VST2, exercises the SDK against the real engine and writes:

- `build/profit5-screen/profit5-melodee.so`: the experimental plug-in.
- `build/profit5-screen/profit5-overview.png`: six native-size panels.
- `build/profit5-screen/profit5-filter-3x.png`: enlarged filter panel.
- `build/profit5-screen/panels/0.png` through `21.png`: **actual SDK
  framebuffer captures**, one per row, without host chrome.
- `build/profit5-screen/preview/`: the host's CLI loading/input smoke test.
- `build/profit5-screen/build.json`: pinned versions and local-only scope.

The current host preview tool selects generic Stage EDIT even when a screen
is loaded, so its PNGs show the host's generic parameter pages. The review
sheet uses the SDK framebuffer captures instead; this experiment leaves the
host preview code alone. On the MPC, the intended route is the plug-in
track's **Stage > PANEL**, with Plug-in screens enabled.

No installation or device restart is performed. To try the compiled
instrument later, place its `.so` in a separate test plug-in folder on the
card. It retains Profit-5's original VST identity, so avoid keeping the
original and this build in the same scanned plug-in library. This is **not**
an MPC OS TUI skin: Melodee's screen extension is what draws the new panel.
Other Open MPC hosts do not request that extension and keep their normal UI.

## Controls

| Control | Action |
| --- | --- |
| K1–K3 | Edit the visible row through Melodee's usual pot bindings and pickup |
| Minus / Plus | Previous / next row, wrapping across all 22 rows |
| Shift + Minus / Plus | Jump to the first row of the previous / next section |
| Encoder | Select one of the three controls |
| Encoder push | Enter / leave encoder editing (amber lamp) |
| Encoder while editing | Change the selected value; switches and integers step by one |
| Shift + encoder while editing | Fine adjustment of continuous values |
| Erase + pot | Melodee's normal parameter-default action |

The plots show parameter positions, **not** measured filter responses, live
audio, or envelope activity. The five header dots show configured polyphony,
not currently sounding voices. Values use the plug-in's own dial units;
pitch adds semitones, master tune adds cents and Program displays 001–128.

## Implementation and validation

`screen.cpp` is the UI, with a small C host interface in `screen.h`.
`bridge.c` compiles Open MPC's original C wrapper intact under a renamed
entry, then adds the optional SDK calls and per-instance screen state.
Parameter changes, program/chunk loads and the original wrapper's MIDI
change notifications request redraws. UI edits notify the host and request
a processing block for a sleeping plug-in. Screen work stays off the audio
thread. Drawing allocates nothing, returns false at rest and reuses the
static header, tabs and diagrams when they have not changed.

The audio renderer, MIDI handling, parameter order, VST identity and engine
state format are unchanged. This uses the upstream's supported **64-param
C-wrapper build** around its current engine, rather than its newer 177-param
native MPC front end. Native-front-end favourites, naming keyboard and the
extended banks browser are outside this screen experiment; the C-wrapper's
programs/imports remain available.

`test.cpp` loads the real library and checks every synth parameter is
reachable, row wrapping, independent instances, encoder selection/editing,
host notifications, pickup redraws, external parameter changes, chunk
restoration, strip drawing, idle drawing and framebuffer bounds with a
padded stride. A note-on/off render is compared sample for sample with the
untouched wrapper entry. Timing is measured for all 22 full redraws.
No physical controls, panel readability on hardware or listening test has
been performed.

Offline validation, 2026-11-24: the aarch64 library builds and every experiment
check passes. Full-page redraws averaged 0.20 ms (maximum 0.34 ms) in the
Linux ARM64 build container; these are not device timing measurements. The
rendered audio matched the untouched wrapper sample for sample. The host
CLI also loaded the screen and accepted all 20 scripted input steps.
The required root `./build.sh` was attempted; it stopped on the current
unrelated `mixer_scene` declaration errors in `src/melodee/tools.cpp`
(the mixer work is outside this experiment). Its output is in
`build/profit5-screen/host-build.log`.

## Origin and licences

Melodee by **Kerem Kiliç (Ellic Studio)** is the origin of the GUI SDK;
the experiment's UI is Copyright © 2026 Kerem Kiliç (Ellic Studio), MIT.
The test harness includes Melodee's AGPL-3.0-only host ABI header and carries
that licence. Existing copyright and SPDX notices are preserved.

- [Profit-5 by sd88me](https://github.com/sd88me/mpc-vst-profit5), commit
  `8d4857585ce5f8b246d0c203891bc0b970c48cfa`: engine and original wrapper
  parameters, GPL-3.0-or-later; its `LICENSE-MIT` and per-file notices remain.
- [Open MPC toolchain](https://github.com/open-mpc/mpc-vst-plugins), commit
  `d2daf62ab8ca9690a747f5278567848a15ac31cb`: C VST wrapper and parameter
  generator. Its own licence remains with the downloaded checkout.
- Liberation Sans Bold: upstream's pinned `skin/fonts/`, SIL OFL 1.1, with
  Google and Red Hat copyright. Rasterised into build-only arrays; the
  full notice is `build/profit5-screen/upstream/skin/fonts/OFL-LiberationSans.txt`.

The resulting plug-in includes GPL upstream code; the MIT licence on the
new screen does not relicense the combined binary. There is no release or
distribution workflow here.
