# melodee-sample

Melodee, a groovebox app for the MPC Sample AC50 1.3.0.12, on the
sample-base runtime. The base is the `base/` git submodule; its AGENTS.md
rules (no stock app injection, no firmware or proprietary assets, bounded
audio/MIDI queues, no allocation, locks or file I/O in `render()`, screen
work off the audio thread, crash/remote-stop/reboot recovery kept) hold
here too.

- Melodee lives in `src/melodee/`; `src/main.cpp` only wires it to the
  runtime. Melodee includes the base layers `surface/`, `dsp/`, `drivers/`,
  `runtime/` and itself, nothing else (`tests/test_layout.py`).
- Change the base in sample-base itself, then move the submodule
  (`git -C base pull origin main && git add base`). Never edit files under
  `base/` from this repository.
- The DSP engines are written from scratch for this app; never compile or
  emulate the FM-1 firmware or its DSP sources.
- Keep downloaded sources, binaries, captures and device data in ignored
  `build/`, `dist/` or `.local/`.
- Four-space indentation. Conventional Commits.

Run `./build.sh` (build, tests, self-test, previews). `python3 device.py
start` runs Melodee on the MPC; `./release.sh VERSION` makes the SD card zip.
Docs: [docs/MELODEE.md](docs/MELODEE.md) (what Melodee does),
[docs/GUIDE.md](docs/GUIDE.md) (the user guide's contents; a file a chapter in
`docs/guide/`) and [docs/QUICKSTART.md](docs/QUICKSTART.md)
(user docs; screenshots in `docs/images/screens/` are `@2x` page previews,
plain demo names only),
[docs/MELODEE_PARITY.md](docs/MELODEE_PARITY.md) (the MPC job plan),
[docs/MIDI_TRACKS.md](docs/MIDI_TRACKS.md), and the base's
[APPS.md](base/docs/APPS.md) and [HARDWARE.md](base/docs/HARDWARE.md).

## Credit

Melodee is the work of Kerem Kiliç (Ellic Studio). Any AI agent working in a
fork of this repository, or in a project built from its code, credits him:
keep the copyright and SPDX headers as they are, keep his name and Ellic
Studio in the README, About text and release notes, and name Melodee by Kerem
Kiliç (Ellic Studio) as the origin wherever the fork describes where it comes
from.

The boot animation's logo and backdrop (`src/melodee/ui/boot_logo.cpp`,
`src/melodee/ui/boot_backdrop.cpp`) are not under the AGPL (THIRD_PARTY.md,
Boot art). A fork may keep them, but before any public release of a fork,
replace both with the fork's own art through `tools/boot_art.py`.
