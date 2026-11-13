# Sources and attribution

## Project licence

Melodee: Copyright (C) 2026 Kerem Kilic (Ellic Studio), AGPL-3.0-only. The
full text is in `LICENSE`.

## Boot art

The boot animation's two images are not covered by the AGPL, and neither
is licensed with this code:

- The Melodee logo (`src/melodee/ui/boot_logo.cpp`): Copyright (C) 2026
  Kerem Kilic (Ellic Studio), all rights reserved.
- The night city backdrop (`src/melodee/ui/boot_backdrop.cpp`): Copyright
  (C) Yasmine Elsen (https://www.smallcutekitty.net), all rights reserved.
  Made for Kerem Kilic (Ellic Studio), the only one permitted to use it.

A fork may keep both files as they are. No public release of a fork, or of
any other project built from this code (a binary, an SD card zip, an image
or any other download), may contain either one: before releasing, replace
both files with art of the fork's own (`tools/boot_art.py` writes them from
any two 320 x 240 PNGs).

## sample-base

The runtime, drivers, build and install scripts come from sample-base
(https://github.com/keremimo/sample-base), the `base/` submodule, under
AGPL-3.0-only. Its own `THIRD_PARTY.md` lists its sources.

## JUCE

JUCE 9.0.5, Raw Material Software Limited, pinned commit
`d60b7a78030661574a615b586d9325ba7a7fa7dd`, used under AGPLv3 (JUCE also
offers a commercial licence). The base's `tools/dependencies.py` fetches it
into ignored `build/deps/JUCE`, whose `LICENSE.md` and `JUCE.spdx.json`
travel with it; release zips include `LICENSE.md`. Its MP3 decoder
(`JUCE_USE_MP3AUDIOFORMAT`, for an MPC expansion's previews) is on: JUCE's
own code, under the same licence; the MP3 patents ran out in 2017.

## Plug-in hosting

- The VST3 SDK, Steinberg Media Technologies GmbH, MIT licence, as bundled
  in JUCE (`juce_audio_processors_headless/format_types/VST3_SDK`); release
  zips include its `LICENSE.txt`.
- The CLAP headers, free-audio/clap 1.2.9, Alexandre Bique and the CLAP
  contributors, MIT licence (`build/deps/clap`, fetched by
  `src/melodee/dependencies.py`); release zips include its `LICENSE`.
- LV2: lilv, serd, sord and sratom (David Robillard) and the LV2 headers,
  ISC licence, as bundled in JUCE
  (`juce_audio_processors_headless/format_types/LV2_SDK`); release zips
  include their `COPYING` files.
- VST2: `src/melodee/plugin/vst2_abi.h` is Melodee's own description of the
  VST 2.4 binary interface, written from its public layout. No Steinberg
  VST2 SDK file is used.
- The GUI-library stubs (`tools/plugin_support/make_stubs.py`) are
  generated: functions returning 0 under the names Debian's libGL, libX11
  and others export. None of those libraries' code is copied.
- Libraries plug-ins use for sound and the MPC lacks
  (`tools/plugin_support/SURVEY.md`): Debian bookworm's aarch64 libsndfile
  (LGPL-2.1+) with libFLAC, libogg, libvorbis, libvorbisenc, libopus
  (BSD-style) and libmp3lame (LGPL-2.0+), unchanged, in the same pack;
  their copyright files travel in `licenses/armhf-runtime/` too.
- The 32-bit bridge's runtime: Debian bookworm's armhf loader, glibc
  (LGPL-2.1+), libstdc++ and libgcc_s (GPL-3 with the GCC Runtime Library
  Exception), unchanged, packed into the binary by
  `tools/plugin_support/build.py`. Release zips carry their copyright
  files, the licence texts and the package versions in
  `licenses/armhf-runtime/`; the source is those Debian packages' source.
- Plug-ins themselves are not part of Melodee and none ship with it: each
  is the user's own, under its own licence.

## Rubik

Melodee embeds Rubik, Copyright 2015 The Rubik Project Authors, under the
SIL Open Font License 1.1, from google/fonts commit
`5bb28733cec313b63fbd415b574b5ae35c8aa8b8` (`ofl/rubik/Rubik[wght].ttf`).
`src/melodee/dependencies.py` fetches it and its `OFL.txt` into ignored
`build/deps/rubik/` and checks both against pinned SHA-256 digests; the
font is not tracked here. Release zips include `OFL.txt`.

## Engines

Melodee's sound engines, sequencer and effects are written for this
project. They follow what Melodee's engines do, but no Melodee firmware or
DSP source is compiled into the app, and its factory sounds are new.
