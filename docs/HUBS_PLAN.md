# Hubs: plan

Status: all picks made 2026-11-13 (S-B, P-a, M-b, R-a, T-a, PL-a, H-b).
Nothing built. Mockups: https://claude.ai/artifact/KysLcHsF6LN7pnAVFtS6ut

## Problem

B1–B3 mean something different on almost every page:

- The SAMPLE cycle uses three meanings. Stage: sub-pages. Tracks: the page's
  own name, then links (LOOPER, SENDS). Pads/Keys: a tab bar that repeats the
  SAMPLE button.
- Stage puts MIX, SENDS, ARP, CHORD and FX on a different key for each track
  kind. A kit's SENDS takes 5 presses.
- Plug-in Edit opens only with Push on Stage.
- Sends uses ‹ TRACK / BUS A › arrows, Song uses B keys for actions, Scenes
  has ALL, Plug-in Edit has PLUG-INS / PRESETS.
- Some tools have B1 BACK, others don't.

## Model

Two kinds of page.

**Hubs.** A hub button opens its hub on the page this track last used. B1–B3
are the hub's three sections. The lit key steps through its section. Holding
the hub button puts the hub's list on the pads. SHIFT + B1–B3 are the page's
actions, where it has any.

| Hub | Opened by | B1 | B2 | B3 | Held |
| --- | --- | --- | --- | --- | --- |
| SOUND | SAMPLE | source | play | out | kit pads / Keys layout |
| SEQUENCE | SEQ | STEPS → TRIG → EUCLID | ROLL | MOTION → MOD | patterns (as today) |
| MIX | PAD BANK held + B1–B3 | TRACKS | SENDS | FX: TRACK → SENDS → BUS → MASTER | the tracks (as today); SHIFT + B2 / B3 MOVE / COPY |
| ARRANGE | SHIFT + PAD 12 | PATTERNS | SCENES | SONG | — |
| Looper | LOOP + 16 LEVELS | LOOPS | MIX | MORE | LOOP held (as today) |

SHIFT + KNOB FX stays FX Select and opens the FX section, as printed.

SOUND hub, option B (picked):

| Track | B1 | B2 | B3 |
| --- | --- | --- | --- |
| Kit | TRIM, LAYERS, TUNE, AMP ENV | PLAY, FOLLOW, CLOUD | MIX, SENDS, FILTER, FILT ENV, KIT FILT |
| Plug-in | PRESETS (if it has any) or its face, then EDIT | ARP, CHORD | MIX, SENDS, FX |
| MIDI | DEVICE, PROGRAM, CC | ARP, CHORD | — |

Empty track: the carousel, as today (B1 CANCEL).

**Tools.** Everything else: Chop, Sample Record, Sample Select, Pad FX, Knob
FX, 16 Levels, Tempo, Compressor, Fader, Rec Quantize, Time Correct,
Settings, Project, Input Config, Flex Beat, Step Edit. B1–B3 are the tool's
actions. Sheets keep B1 BACK / CANCEL and B3 the verb.

## Choices

| Id | Question | Pick |
| --- | --- | --- |
| S | SOUND hub keys | B: same role on every track |
| P | Patterns grid | P-a: ARRANGE hub (SHIFT + PAD 12) |
| M | MIX hub opener | M-b: PAD BANK held, then B1–B3 (not SHIFT + KNOB FX: FX Select stays) |
| R | Hub button inside its hub | R-a: home (B1's first page) |
| T | Leaving a tool | T-a: its opener again |
| PL | Plug-in's first page | PL-a: presets page when it has presets (the old Stage PRESET picture), else the plug-in's face (name, format, maker, load) |
| H | Screen while SAMPLE is held | H-b: Pads/Keys sheet after 0.3 s on every page but Steps / Roll; there the editor stays and the header names the held sound or notes |

SAMPLE held keeps every job: a kit pad pick (silent), Shift = Input Config,
and the jobs planned in the sequencing rethink (pads play the Keys layout,
the sound Steps place, K3 layout, 16 LEVELS on/off). B1–B3 while held open
the SOUND sections, the same as SEQ held.

Later: how to make a scene is unclear. Scenes get their own pass.

## What moves or goes (check before building)

- SAMPLE no longer reaches Tracks, Pads or Keys. Tracks goes to the MIX hub,
  and Pads/Keys become the SAMPLE-held screen.
- SEQ tap no longer toggles Patterns ↔ editor. This replaces part of the
  approved Seq flow (ad52807).
- PAD BANK held: −+ BANK hint goes (BANK chip stays in the header); MOVE /
  COPY move from B2 / B3 to SHIFT + B2 / B3.
- Tracks loses B2 LOOPER (LOOP + 16 LEVELS still opens the Looper).
- Push on Stage no longer opens Edit; EDIT gets the plug-in B1 key. Push needs
  a new job or none.
- Sends loses ‹ TRACK / BUS A ›: − / + pick the bus.
- Song: EXPORT / MUTE / DUPLICATE → SHIFT + B1–B3. Scenes: ALL → SHIFT + B3.
- T-a: B1 BACK goes from Fader, Rec Quantize, Time Correct, Settings, Input
  Config, Step Edit. T-b: Compressor's SIDECHAIN, Punch's LATCH, Knob FX's A,
  Project's SAVE, Record's MONITOR move to SHIFT + B1.
- 16 Levels types (VELOCITY / FILTER / TUNE) stay on B1–B3 as actions.

## Build order

1. Stage tables (`engine/engine_info.cpp` `stage_groups`): option B, the
   plug-in's first page per PL (reuse `preset_picture`). Remove
   the dead `plain` table and the stale comment in `engine_info.h`.
2. SAMPLE: hub only (`shell/sample_mode.cpp` `open_sample_page`). Pads/Keys
   become the SAMPLE-held screen per H; SAMPLE inside the hub goes home.
3. MIX hub: Tracks / Sends / Chain pages get one key row
   (`tracks_page`, `sends_page`, `chain_page`). PAD BANK held B1–B3 open them
   (`shell/pad_bank.h` `track_key`); MOVE / COPY on SHIFT.
4. SEQ and ARRANGE: SEQ opens the editor. Patterns in ARRANGE. Song
   and Scenes actions move to SHIFT + B.
5. Tools: B1 BACK goes; the opener closes.
6. GUIDE.md tables (Navigation, Stage, Tracks, Patterns, Buttons), screen
   PNGs, tests (`tests/melodee.cpp` Stage and page-stack cases), previews.
