# Melodee roadmap (2026-11-12)

What Melodee still lacks, in six phases, built one phase a session in order
P0 → P5 (Kerem, 2026-11-12: "start all of them one by one in separate
sessions, fully automate the flow and do not stop until every phase is
done"). Each session reads this file, [AGENTS.md](../AGENTS.md),
[MELODEE.md](MELODEE.md) and the "Rules for every phase" in
[MELODEE_PARITY.md](MELODEE_PARITY.md) first, builds its phase, ticks its
boxes, adds a line to the log at the bottom and updates MELODEE.md where
behaviour changed.

## Rules for every phase

- Work in this checkout on `main`, no
  worktree. Build with `./build.sh` here, reusing `build/`; never in a
  private scratch copy. Other sessions edit this checkout at the same time:
  commit only your own paths (`git commit <paths> -m ...`), never `git
  checkout` / `restore` / `stash` a file you did not change, compile your
  own objects (`make build/obj/melodee/<path>.o`) before leaving a file, and
  fix your file first when it breaks a peer's build. GCC 12 `-Werror` trips
  on aggregate inits with missing fields and on `std::vector` assign from an
  init list.
- Device: `python3 device.py start --app melodee` deploys the built
  `build/melodee` (never return to stock, never the ctl prompt overlay);
  `--ctl melodee-press` and the `status` fields check it remotely. What
  needs ears or hands goes under "Waiting for a hand check" below instead
  of stopping.
- Audio thread: no allocation, locks or waits; new audio state arrives as
  `Command`s. Project file: new fields are new tagged sections, so older
  `.mldp` files still load. No literal 44100: rates and latency come from
  `EngineHost::set_audio_format` / `drivers/audio.h`.
- Base changes go into the sibling checkout `../sample-base` (committed
  there, then `base/` moved to that commit). Never push anything, here or
  there, and never touch GitHub: list the pushes owed in the log.
- Screens: pads pick, knobs set, keys act, Push has one job, no push and
  turn; 11 px text floor; glyphs over sentences, no text walls; tokens from
  `ui/`. Every new page gets a `--melodee-preview` name; the Shift overlay
  follows any Shift job that changes.
- No questions to Kerem: where a choice is open, take the one that fits
  these rules and Melodee's grammar best, write it under "Decisions made by
  sessions" with a line of why, and go on. Kerem may revisit them.
- Commits: Conventional Commits, subject line only, no attribution. Commit
  each finished item once `./build.sh` passes; no need to ask.
- Leave alone what other work owns: round 2 phase 3 (four sends a track,
  pattern chains, step entry) belongs to the UI redesign; track freezing to
  its own planning session; pad lights to their proposal.

## Decisions made by sessions

(none yet)

## Waiting for a hand check

(none yet)

## P0: timing

- [ ] Recorded notes land where they were heard: `Context::record`
  (`shell/context.cpp`) takes `host.position_of()`, the tick the engine is
  on when the MIDI thread sees the hit, so a take with Rec Quantize off is
  late by the output latency (about 8.7 ms at 3 × 128 frames). Place it
  from the hit's timestamp minus the output latency, as Manual chop's taps
  already are (`shell/shell.cpp`, `tap_frames`). Host test with a known
  latency.
- [ ] Live notes at their frame: pad and MIDI-in Note On / Off (and
  `PadNote`, `ArpKey`) are applied at the start of the next block
  (`EngineHost::render`), so a hit moves by up to a block. Carry the
  rawmidi timestamp to a frame (`clock.frame_at`) and play it at a fixed
  one-block delay at that frame (split the block there, as ticks do). Host
  test: two hits a known time apart come out that far apart.
- [ ] The MIDI thread (`Controller::run_midi` in the base) at a low
  real-time priority (SCHED_FIFO below the audio thread's 30 and the
  pager's 20, e.g. 15); it sleeps in `poll`, so it cannot starve anything.
- [ ] VST2 bridge: the `vst2_bridge32` worker that answers the audio
  thread runs at SCHED_FIFO with the audio thread's priority (set in the
  bridge, or by the host on the child); the connection lock the audio
  thread shares with parameter reads from the MIDI thread
  (`plugin/vst2_bridge_instance.h`) becomes priority-inheriting, or the
  reads come from a lock-free copy, so the audio thread never waits on a
  normal-priority thread.

## P1: drums and sequencer

Reference: melodee-ios (the sibling checkout `../melodee-ios`,
`MelodeeKit/Sources/MelodeeAudio/Pattern.swift` and `NoteEffects.swift`)
has ratchets, conditions, lane lengths and Euclid already; port the
behaviour, write the code fresh for this app.

- [ ] Ratchet: `Event` gains a ratchet (1–8 hits inside the note's length)
  and a ramp (−100..100, the last hit's velocity in percent of the
  note's); on a kit also a pitch ramp if it fits. The repeats play through
  the late-note path the arp uses (`run_late`). File section, Steps / Step
  Edit / Roll editing, drawn on Steps and Roll, previews, tests.
- [ ] Trig conditions: always, n:m (2..8), FILL / NOT FILL, FIRST / NOT
  FIRST, PRE / NOT PRE, NEI / NOT NEI, beside chance; a pass counter a
  track (a kit lane of its own length counts its own); a held control for
  Fill.
- [ ] Per-step locks and Motion depth: Motion is 256 events for the whole
  project at sixteenth resolution, found by a linear scan
  (`model/project.h`). Store it per pattern with room for thousands, at
  tick resolution; holding a step on Steps (or the note in focus on Roll /
  Step Edit) and turning a pot writes a lock on that step. Old `MOTN`
  sections still load.
- [ ] Kit lanes of their own length (polymeter inside a kit).
- [ ] Note Repeat: pad pressure sets the repeats' velocity (as the MPC),
  1/4 and 1/64 rates, the repeats written as notes under Seq Record.
- [ ] Humanize in Time Correct (a random timing and velocity amount) and a
  groove taken from one pattern and laid on another.
- [ ] Generators: Euclid on a kit lane (hits, steps, rotate); a pattern's
  variations (thinner, busier, humanised, reversed, an octave up / down).
- [ ] A track's playback speed (×½, ×¾, ×1, ×3/2, ×2) without rewriting
  its notes.

## P2: CPU scheduling

- [ ] One dispatch a block: `EngineHost::render` splits the block at every
  tick, events or not, and calls `workers.run` (a barrier) for each part,
  then once more for the chains. Engines take the block's events with
  frame offsets and render it whole; each track's engine and chain become
  one job; a block has one barrier. A test plays a busy project both ways
  and compares output sample for sample (or within float noise, said why).
- [ ] With one dispatch, drop or shorten the workers' 200 µs spin
  (`engine/render_pool.cpp`); measure CPU before and after on the device.
- [ ] Buses A–D, L1–L4 and their chains as parallel jobs after the tracks.
- [ ] Core pinning, measured: today audio plus three workers float over
  four cores at SCHED_FIFO. Try two workers with the audio thread on cores
  1–3 and the rest on core 0 against today's, on a busy project; keep the
  better as the default and the other on `$MELODEE_RENDER_THREADS` / a
  setting.
- [ ] DSP load shown: each track's share of the block (`track_ns`) on
  Tracks, the whole on Settings' AUDIO beside the dropouts; a dropout notes
  the busiest track in `status`.
- [ ] Overload guard: as block time nears its budget, steal the oldest
  voices and shorten tails before a dropout.
- [ ] Samples resampled once at load to the device rate (projects keep
  44.1 kHz frame counts, `engine/model_rate.h`), not per voice.

## P3: sampler

- [ ] Pad layers: up to four samples a pad, by velocity range, cycle or
  random (MPC programs' layers keep working as they do).
- [ ] Chop's slices kept with the project.
- [ ] Sample slots nothing uses any more freed on save.
- [ ] Pad Link through more than one pad.
- [ ] Seq Record keeps a 16 Levels pad's cutoff.
- [ ] A short crossfade at a loop's seam.
- [ ] Resample of the song onto a pad.
- [ ] SFZ: release samples (`trigger=release`), crossfades (`xfin_*` /
  `xfout_*`), keyswitches; SF2 and `.dspreset` instruments.

## P4: mix, effects, MIDI

- [ ] Plug-in delay compensation across tracks, buses and the master.
- [ ] A compressor or gate on any chain keyed by another track.
- [ ] SYNC on Delay and Diff Delay (note values from the song's clock).
- [ ] Motion lanes for a track's level and send and the master's values.
- [ ] Modulators: LFOs (free or on the clock) and envelope followers on
  any parameter; macros.
- [ ] MIDI tracks M2–M5 ([MIDI_TRACKS.md](MIDI_TRACKS.md)), its open
  questions decided as above.
- [ ] Stems to the card, Standard MIDI Files in and out, dither on the
  mixdown.

## P5: performance and arrange

- [ ] Scenes (a column of patterns with a name and tempo) and clip follow
  actions (next, previous, any, again, stop; after bars or plays; chances).
- [ ] Always-on capture: the last minutes of every track's played notes,
  kept to a pattern after the fact.
- [ ] Tempo changes over the song (a tempo lane on Song).

## Log

- 2026-11-12: roadmap made from a read of the engine, the render pool, the
  base's threads and melodee-ios; nothing built yet.
