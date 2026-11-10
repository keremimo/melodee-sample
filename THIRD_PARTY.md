# Sources and attribution

## Project licence

Melodee: Copyright (C) 2026 Kerem Kilic (Ellic Studio), AGPL-3.0-only. The
full text is in `LICENSE`.

## sample-base

The runtime, drivers, build and install scripts come from sample-base
(https://github.com/keremimo/sample-base), the `base/` submodule, under
AGPL-3.0-only. Its own `THIRD_PARTY.md` lists its sources.

## JUCE

JUCE 9.0.5, Raw Material Software Limited, pinned commit
`d60b7a78030661574a615b586d9325ba7a7fa7dd`, used under AGPLv3 (JUCE also
offers a commercial licence). The base's `tools/dependencies.py` fetches it
into ignored `build/deps/JUCE`, whose `LICENSE.md` and `JUCE.spdx.json`
travel with it; release zips include `LICENSE.md`.

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
