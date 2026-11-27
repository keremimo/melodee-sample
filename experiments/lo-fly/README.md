<!-- SPDX-License-Identifier: AGPL-3.0-only -->
# lo-fly for Melodee

lo-fly (Ellic Studio) as a CLAP instrument for Melodee on the MPC Sample,
with its own panel on the MPC's screen through Melodee's screen SDK
([sdk/melodee-screen](../../sdk/melodee-screen/),
[docs/PLUGIN_SCREENS.md](../../docs/PLUGIN_SCREENS.md)). No JUCE: a plain
CLAP wrapper around lo-fly's own engine (`lofly::Synth`) and presets, built
from the lo-fly repository's `dsp/`. In any other host it is the same
instrument without an editor.

| File | What |
|------|------|
| `plugin.cpp` | CLAP entry: parameters, notes, state, presets, remote-control pages, the screen extension |
| `panel.cpp`, `panel.h` | the panel: 10 pages drawn into Melodee's 320 × 218 frame |
| `store.h` | parameter values shared by the host calls, the audio thread and the screen |
| `fonts.py` | National Park (OFL) into `build/lo-fly/gen/fonts.h`: seven sizes and weights, anti-aliased |
| `preview.cpp` | hosts the plug-in on a Mac: writes each page and the strip as PNGs, measures render and draw times |

## Build

Needs the lo-fly checkout beside this one (`LOFLY_DIR`, default
`../lo-fly` from the repository root), zig (the MPC build), clang (the Mac
build) and Python with Pillow (the fonts).

```sh
cd experiments/lo-fly
make fonts              # National Park (Google Fonts' repository, a fixed commit), once
make                    # build/lo-fly/mpc/lo-fly/ (lo-fly.clap, Presets, FONTS.txt) and build/lo-fly/mac/lo-fly.clap
make screens            # build/lo-fly/screens/*.png
make bench              # render and draw times on this machine
make presets            # every preset found, loaded and played once
make validate           # clap-validator on the Mac bundle
```

`LOFLY_REV` picks lo-fly's source: `HEAD` (the default) or a commit exports
that commit's `dsp/` to `build/lo-fly/lofly-<commit>/`; `WORKTREE` copies
the working tree's `dsp/` to `build/lo-fly/lofly-worktree/` on every make.
Presets need lo-fly bd81974 or later.
The MPC build is aarch64 Linux for glibc 2.28, with libc++ linked in; it
exports `clap_entry` only.

## The look

Print: two inks on paper, as a risograph prints them. Blue carries the lines
and the type, fluorescent pink the fills; both multiply onto the paper, so
they turn violet where they cross, and the pink plate sits a fraction of a
pixel off register. The area under each curve is a pink halftone screen.
Knobs are discs in K1 pink, K2 blue and K3 both inks, with a dotted track,
the value's arc (one mark a setting for a choice) and a pointer. The wheel's
choice sits on a stroke of pink marker; a chevron fades at the end of the
list. The LOFI page draws the tape's wobble (the character's wow and
flutter, as deep as Wobble × Amount) as a line that moves while lo-fly
sounds, and the level on two bars. ENGINE pages put one tick a converter
sample under the cycle.

All text is National Park (lettering from the US National Park Service's
routed wooden signs): titles 27 px, the wheel's choice 20 px, knob values
17 px (13 or 11 px when wider than 96 px), labels 11 px. National Park is
SIL OFL 1.1; its licence goes beside the plug-in as `FONTS.txt`.

## On the MPC

Copy the folder `build/lo-fly/mpc/lo-fly` (`lo-fly.clap`, `Presets`, `FONTS.txt`) to
`Melodee/Plugins` on the card. Pick lo-fly as a PLUGIN track's sound, then
Stage **B1** to **PANEL**. The page's header shows the loaded preset's name.

| Control | Action |
|---------|--------|
| **−** / **+** | previous / next page |
| Wheel | the page's top control (**SHIFT**: finer steps on Sustain) |
| Push | the page's section on or off (pink dot left of the title: on; pink ring: off) |
| **K1**–**K3** | the three knobs at the bottom of the page |

FX is lo-fly's rack: FX 1 to FX 4, left to right as the sound runs, each
with its on/off lamp (pink: on; a ring: off).

| Control | Action |
|---------|--------|
| Wheel | the lit slot |
| **K1**–**K3** | the lit effect: Mix (Amount on Multiband and Auto Pan, Level for a spatial effect on Send, Scale on the EQ) and two of its values |
| Push | the effect menu: None, then the effects by category (Spatial, Dynamics, Filter / EQ, Distortion, Modulation); the wheel walks it, Push picks, **−** goes back |
| **SHIFT** + Push | a spatial effect (Reverb, Delay, Tape Echo, Pitch Delay) between Insert and Send |
| **MUTE** | the lit effect off or on (held with pads, it mutes tracks as always) |
| **ERASE** | the lit slot back to None; **ERASE** lights while there is an effect |

**MUTE** and **ERASE** need a Melodee that hands them to a plug-in's page
(`melodee.screen-host/2`, `docs/PLUGIN_SCREENS.md`).

| Page | Wheel | K1 | K2 | K3 | Push |
|------|-------|----|----|----|------|
| LOFI | Character | Amount | Drive | Wear | LoFi on/off |
| TAPE | Character | Tone | Wobble | Speaker | LoFi on/off |
| ENGINE 1, 2 | Wave | Timbre | Hardware | Level | engine on/off |
| NOISE | 6 textures, then 6 transients | Level | Tune | Stereo | noise on/off |
| FILTER | Type | Cutoff | Resonance | Env Amount | filter on/off |
| FILTER ENV, AMP ENV | Sustain | Attack | Decay | Release | |
| FX | the lit slot | Mix | by type | by type | the effect menu |
| VOICE | Voice Mode | Glide | Vintage | Volume | |

Every lo-fly parameter (956) is a CLAP parameter: Melodee's **EDIT** pages
show eleven pages of them first (`remote_pages` in `plugin.cpp`), then the
rest by section.

## Presets

Stage **B1** **PRESETS** lists:

- **Factory**: lo-fly's factory presets (24), inside the plug-in.
- **User**: `.loflypreset` files in `Melodee/Plugins/lo-fly/Presets`, in
  any subfolders (each subfolder is a folder in the list). Copy them from a
  computer: lo-fly saves them on a Mac in
  `~/Library/Audio/Presets/Ellic Studio/lo-fly`, a folder this plug-in also
  reads on a Mac.

A preset sets every parameter, the modulation and the Step Gate patterns.
The sound changes at once (no glide). The project saves the preset's name.

## Not done

- Instrument (SFZ) and Sampler content: no file pick on the MPC. A state
  that names files loads them.
- Modulation routings, drawn shapes and Step Gate patterns: no editor; the
  defaults play and the state keeps them.
- No preset save on the MPC.
- Not yet loaded in Melodee itself, on the MPC or in the builder image.
