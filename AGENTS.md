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
[docs/MELODEE_PARITY.md](docs/MELODEE_PARITY.md) (the MPC job plan),
[docs/MIDI_TRACKS.md](docs/MIDI_TRACKS.md), and the base's
[APPS.md](base/docs/APPS.md) and [HARDWARE.md](base/docs/HARDWARE.md).
