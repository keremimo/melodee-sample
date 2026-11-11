# vst_probe

A command-line VST3 and VST2 host for the MPC Sample, built on JUCE 9's
`juce_audio_processors_headless` (no GUI modules). VST3 goes through JUCE's
VST3 format. VST2 `.so` files (such as Open MPC's plug-ins) go through
`Vst2Instance` (`vst2_instance.h`), a `juce::AudioPluginInstance` over our
own `vst2_abi.h`, so both formats share one interface. 32-bit armv7 VST2
plug-ins run in a separate `vst2_bridge32` process through
`Vst2BridgeInstance`, which implements the same interface. It answers what Melodee's
VST instrument tracks and VST effect slots will face before any of that is
built. It opens no audio device, so it runs beside Melodee.

## Build and run

Inside the base's build image, after Melodee's build has compiled JUCE:

```sh
docker run --rm --platform linux/arm64 -v "$PWD:/work" sample-base-builder \
    sh -c 'make -f tools/vst_probe/Makefile -j4 && python3 tools/vst_probe/make_stubs.py build/vst_probe/stubs && sh tools/vst_probe/build_bridge32.sh'
```

`build_bridge32.sh` builds the bridge for armhf and copies Debian's armhf
runtime (loader, glibc 2.36, libstdc++, libgcc_s; 3 MB) into
`build/vst_probe/bridge32/`. Copy that folder next to the probe.

On the MPC, with `build/vst_probe/vst_probe` and `build/vst_probe/stubs/` copied
to `/tmp/vst_probe/` and the bundles to `/tmp/vst3/`:

The probe loads the stubs from `stubs/` beside its binary by itself.

```sh
./vst_probe scan /tmp/vst3
./vst_probe run /tmp/vst3/Nekobi.vst3 --rate 44100 --block 128 --seconds 8 --wav out.wav
./vst_probe run /tmp/vst3/DragonflyHallReverb.vst3 --instances 8
./vst_probe run "/tmp/openmpc/sd88me - VST - Acid-303/acid303.so" --rate 44100
```

## Findings (2026-11-11, AC50 1.3.0.12, Cortex-A55 x4, 2 GB)

Tested with the official arm64 releases of Dragonfly Reverb 3.2.10 and
DISTRHO DPF-Plugins v1.7: 19 bundles, 2 instruments (Nekobi, Kars) and 17
effects. All 19 load, run, and save and restore their state.

- **The SD card is `noexec`.** It is exFAT, mounted
  `noexec,nosymfollow`, so `dlopen` fails with "failed to map segment".
  Bundles must be copied to tmpfs (`/tmp` or `/run`) before loading. Copy
  only what a project uses: the 19 bundles take 57 MB of RAM.
- **GUI libraries are missing.** 14 of 19 list `libGL.so.1`, `libX11.so.6`,
  `libXext`, `libXrandr` or `libXcursor` in NEEDED for their editors. The
  MPC has EGL and GLES only. `make_stubs.py` builds no-op libraries with the
  real sonames and export lists; with them loaded, every plug-in opens.
  The probe `dlopen`s the stubs by path (`RTLD_GLOBAL`) before loading any
  plug-in. Their sonames are then already satisfied, with no
  `LD_LIBRARY_PATH` (verified: 19 of 19 load). Melodee can do the same.
- **CPU (one core, 128 frames at 48 kHz, 2667 us budget, Melodee
  stopped):**

  | Plug-in | Mean | Max | Notes |
  | --- | --- | --- | --- |
  | Nekobi (303 synth) | 0.8% | 3.8% | 16 instances: 12.9% mean |
  | Kars (pluck synth) | 0.7% | 14% | |
  | Dragonfly Hall | 11.4% | 21% | 8 instances overrun (107% mean) |
  | MVerb | 2.8% | 7.3% | 16 instances: 76% mean, 8 overruns |

  A Melodee build needs per-slot CPU metering and an instance cap. Heavy
  reverbs should go on send buses, not on every pad.
- **Memory:** 3 MB (Dragonfly Hall) to 12 MB (MVerb) resident per instance.
- **Creation:** 10 to 35 ms per instance, on the message thread. Load
  plug-ins off the audio thread and swap them in.
- **Parameters:** DPF instruments list about 2090 parameters. Two are
  host-info parameters marked not automatable, and 2048 are "MIDI Ch. N CC
  M" proxies for VST3 MIDI mapping. The synth's real controls come after
  those. The parameter pages must hide these proxies.
- **Buses:** Instruments are mono out (1 ch), and some effects are 1-in or
  1-in/2-out. 3 Band Splitter is 2 in, 6 out. Melodee must map buses to
  stereo itself.
- **Levels:** Kars peaks at 2.5 for a 4-note chord. A VST track needs gain
  and a safety limiter like Melodee's own engines.
- **Tail:** Dragonfly reports a 0 s tail. Do not stop processing an effect
  when its tail ends; use silence detection instead.
- **Teardown:** DPF prints "asked to delete component while audio processor
  still active" on destruction. It is harmless, but Melodee should call
  `setActive(false)`/`releaseResources()` before freeing.
- **Formats:** VST3 through JUCE. VST2 through `Vst2Instance`, because
  JUCE's VST2 host needs Steinberg's withdrawn VST2 SDK. JUCE 9.0.5 has no
  CLAP hosting. Most commercial plug-ins ship no `aarch64-linux`
  build, and Surge XT has none either (it needs building from source).

## Open MPC VST2 plug-ins (open-mpc.github.io)

These are VST2 `.so` files for MPC OS. Most are armv7 only, and a 64-bit
process cannot load those. The ones with a `-mpc-aarch64.zip` load as they
are. Tested: 8W8 1.2.0 (TR-808) and Acid-303 1.0.2. The sha256 of each
download matched the catalog.

- **Dependencies:** only libc, libm, libstdc++ and libgcc_s. They need
  glibc 2.34 at most (the MPC has 2.39). No GUI libraries, so no stubs are
  needed. There is one weak import, `mpc_engine_changed`, an MPC OS hook.
  Because it is weak, the library loads without it, and both plug-ins ran
  without it.
- **Host interface:** both plug-ins answered every host callback they sent;
  no opcode went unanswered.
- **Parameters:** these are the real controls, with display text: 8W8 has
  151 ("BD Tune", "SD Snappy", "Diode"...), Acid-303 has 24 ("Cut Off",
  "Devil Fish"...). There are no proxy parameters to hide.
- **Knob layout comes for free.** Each plug-in ships
  `Plugin Skins/Q-Links.json`, which maps 16 Q-Links to parameter indices
  per tab and sub-tab, and `TUI.json` with the page layout. Melodee's
  encoder pages can read these directly. `plugin-meta.xml` has the name,
  uid and instrument flag without loading the `.so`.
- **CPU (one core, 128 frames at 44.1 kHz, 2902 us budget):**

  | Plug-in | Mean | Max | Notes |
  | --- | --- | --- | --- |
  | 8W8 | 2.8% | 5.8% | 8 instances: 25% mean, no overruns |
  | Acid-303 | 22% | 48% | 3 instances overrun (67% mean, 57 blocks over) |

  Acid-303 is a circuit model and was run at "Quality: AUTO". The catalog
  also marks it WARN on Gen1. Allow one or two per core.
- **State:** both save and restore their chunk. Acid-303's chunk differs
  after a round trip; the cause is not yet checked.
- **Creation:** 8W8 takes 2 ms; Acid-303 takes 210 ms (it builds tables).
  Load off the audio thread.
- **Licensing:** these are GPL-3 (8W8) and AGPL-3 (Acid-303). Some catalog
  entries are "build it yourself" because they embed firmware. Melodee must
  never bundle plug-ins; it should only load the files a user brings.

## armv7 plug-ins through a 32-bit bridge

The MPC kernel has `CONFIG_COMPAT=y`, and the A55 runs AArch32 code, so
32-bit ARM processes work (a static armv7 test binary ran). A 64-bit
process cannot `dlopen` a 32-bit `.so`, so armv7 plug-ins run in a
separate process:

- **`vst2_bridge32`** (armhf, 16 KB, no JUCE) loads the plug-in with
  `vst2_abi.h`. It is started as `bridge32/armhf/ld-linux-armhf.so.3
  --library-path bridge32/armhf bridge32/vst2_bridge32 FD`, so nothing is
  installed on the rootfs. `PR_SET_PDEATHSIG` ends it if the host dies.
- **`Vst2BridgeInstance`** (64-bit, `juce::AudioPluginInstance`) creates a
  `memfd` holding `bridge::Shared` (`vst2_bridge_protocol.h`, fixed-width
  fields only) and hands each command over. Commands cover audio, MIDI,
  parameters, programs and state. Two futex sequence words carry the
  handoff; both sides spin briefly before sleeping. `processBlock` waits
  for its block in the same period, so the bridge adds no latency.
- The probe picks the bridge for any 32-bit ARM ELF `.so`.

Of Open MPC's 228 catalog entries, 130 are armv7 only, 86 have both
builds, and 10 are aarch64 only.

Tested with Acid-303 1.0.2's armv7 build (glibc 2.29 and
GLIBCXX_3.4.29 at most, hard-float, NEON) against its aarch64 build, at
128 frames and 44.1 kHz:

| | aarch64 in-process | armv7 through the bridge |
| --- | --- | --- |
| Mean | 646 us (22.2%) | 624 us (21.5%) |
| p99 | 1052 us (36.3%) | 1148 us (39.5%) |
| Max | 1394 us (48.0%) | 1484 us (51.1%) |
| 3 instances | 67% mean, 57 blocks over | 68.5% mean, 90 blocks over |
| Creation | 210 ms | 257 ms |

- **Handoff cost:** an empty round trip takes 1.2 us mean and 1.8 us p99,
  with rare peaks of 23 to 85 us. That is under 0.1% of a 128-frame period.
  The armv7 build costs about the same CPU as the aarch64 one.
- **Memory:** about 2 MB RSS per bridge process, plus 300 KB in the host.
- **Crash isolation, verified:** killing the bridge with SIGKILL
  mid-render left the host running. It marked the plug-in dead, output
  silence from the next block (one block overran while the death was
  detected), and exited normally. Melodee would show the slot as crashed
  and offer a reload.
- **Sound:** each build renders bit-identically run after run. The two
  builds differ from each other from sample 48 on, with the difference
  12 dB below the signal. That is the plug-in's arch-specific math (a
  nonlinear circuit model), not the bridge. Expect armv7 and aarch64
  builds of one plug-in to sound close, but not identical.
- **For Melodee:** the probe's bridge serves one thread. A real build
  would cache parameter values in shared memory and fold parameter
  changes into the process command, so the UI never makes its own round
  trip. It would give up on a block within the period instead of the
  probe's 200 ms, run the bridge at the audio thread's real-time priority,
  and ship the armhf runtime's licences with a source offer.
