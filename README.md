# Melodee for the MPC Sample

Melodee is a sixteen-track groovebox for the **Akai MPC Sample (firmware
1.3.0.12)**: one track per pad, four synths, a 909 kit and a sampler,
patterns and a song, mixer and effects. It runs in place of the stock app
on [sample-base](https://github.com/keremimo/sample-base), a standalone
JUCE runtime for the MPC Sample, and returns to the stock app on exit, a
crash or any failure. What it does, page by page:
[docs/MELODEE.md](docs/MELODEE.md).

## Layout

| Path | What |
| --- | --- |
| `src/melodee/` | The app: model, engines, pages, shell, design system |
| `src/main.cpp` | Wires Melodee to the sample-base runtime |
| `base/` | sample-base as a git submodule: drivers, runtime, build (`base.mk`), install scripts |
| `tests/` | Engine, sequencer and shell tests built for the host; layout and release checks |
| `tools/make_release.py`, `release/` | The SD card release zip |

Clone with the submodule:

```sh
git clone --recurse-submodules https://github.com/keremimo/melodee-sample.git
```

## Build

Docker builds for linux/arm64 in the base's build image, then runs the
tests, the sound self-test and the page previews (`build/melodee-preview/`):

```sh
./build.sh
```

## Run on an MPC over SSH

`device.py` is the base's device tool with this build: `start` copies
Melodee into RAM and swaps it in, `status`, `ctl`, `screenshot` talk to the
running app, `install-sd` starts it from the card at every boot.

```sh
python3 device.py start
```

## Release

```sh
./release.sh v0.1.0-alpha.1
```

builds, strips and writes `dist/Melodee-MPC-Sample-<version>.zip`: an
`autorun` folder to copy to the top of the MPC's microSD card, a README for
whoever installs it, and the licences. The tree must be committed; the zip
names its source commit.

## Updating the base

```sh
git -C base pull origin main
git add base
```

## Licence

AGPL-3.0-only (`LICENSE`); JUCE under AGPLv3, Rubik under the SIL OFL 1.1
([THIRD_PARTY.md](THIRD_PARTY.md)).
