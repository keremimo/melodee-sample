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

- P0: a note played by hand or by MIDI in plays exactly one block (128
  frames) after the frame it came in at, at that frame; commands sent
  after it wait behind it, so nothing overtakes it. Inside a block only
  notes and values (`EngineHost::mid_block`) are applied; anything else
  (Play, a project, an engine, a plug-in) waits for the next block's start,
  where it always was, so block-wide state stays whole. Why: keeps the
  order sent and the render workers' rules, at the cost of half a block
  more latency on average for no jitter.
- P0: a timed note more than three blocks ahead (the audio clock off just
  after an overrun) plays at once, as before. Why: a hit can never be
  that far ahead; better a block late than tens of ms.
- P0: Seq Record places an unquantised note on the tick nearest to where
  it was heard (it took the tick the playhead had last played). Why: half
  a tick nearer the played time.
- P0: pad presses through `--ctl melodee-press` and `melodee-midi` notes
  are stamped as a hand's, so the timed path can be checked remotely
  (status `melodee_timed`).
- P0: the VST2 bridge's lock inherits priority (rather than a lock-free
  copy of parameter values, which would go stale when a plug-in changes
  its own); the worker takes `SCHED_FIFO` 30 only once the plug-in is open
  (an open at that priority could hold a core from the audio threads),
  with `SCHED_RESET_ON_FORK` so threads the plug-in starts stay normal; a
  state is read or written one parameter a lock, blocks passing between.
- P1: ratchets, their ramp and (next) trig conditions sit on a second row of
  cards, the TRIG row, reached by pressing the lit editor key again (STEPS
  on Steps, ROLL on Roll), Stage's cycle grammar with its dots: K1 hits, K2
  velocity ramp, Shift + K2 a kit pad's pitch ramp. Why: knobs set values;
  round 2 (agreed with its session) turns Step Edit into step entry and
  puts NUDGE on Roll's Shift + K1, so neither Step Edit nor a Shift row
  carries them.
- P1: a ratchet's hits spread evenly over the note's length (no swing
  inside), each held as a one-hit note of that length is, each on its own
  frame (the render loop splits the block there, as for ticks); the chance
  is rolled once for all, an accent lifts each; the ramp goes evenly from
  the note's velocity to the last hit's (iOS's rule). Slide and Tie wait for
  a note of one hit. Why: the hand-off of a held note into the next note
  only works from a single note; ratchets are drums' job.
- P1: the pitch ramp is a kit pad's only (the sampler; not the 909, whose
  drums have no tune per hit), as the roadmap had it.
- P1: on a kit, Steps' cards (length, velocity, chance and the TRIG row) are
  the last pad's note in the step, not the step's first note. Why: Steps on
  a kit shows and toggles that pad's lane, so its cards follow it.
- P1: Fill is Play held while playing (Play there did nothing), for as long
  as it is down. Why: a held control on a printed key that had no job
  while playing; Seq held and Shift already carry layers.
- P1: PRE follows the last weighed note of its lane (a kit's pad, else the
  track); NEI the lane before (a kit's pad below, the first pad the track
  before; a synth the track before); a lane with nothing weighed since Play
  counts as played (iOS's rule). A note is weighed with a condition of its
  own or a chance below 100; the condition goes first, then the chance.
  Why: a kit's pads are its voices, as Elektron's tracks are.
- P1: trig conditions sit on the TRIG row's K3, beside the main row's K3
  chance.
- P1: Motion lives in each pattern (`Pattern::motion`, MOTK), sorted by
  tick, 8192 points a pattern, found by binary search each tick; edits reach
  the audio thread as the pattern's copy (`PatternData`), recorded moves at
  most every 40 ms (`Context::flush_motion` on the MIDI loop). Why: no
  per-event commands to keep two lists equal, and copies, doubles and track
  copies carry their motion for nothing. New files keep Motion only in
  MOTK, so builds from before P1 lose it (older files still load).
- P1: a lock is a Motion point with a hold: the lane holds its value from
  the step for as long as the step's longest note on its lane (a step at
  least; on Roll the note's length), then goes back to its recorded value
  there, else the track's own. Why: the sound changes for that step's notes
  only, as an Elektron lock does, without rewriting what Motion records.
- P1: holding a step on Steps now turns the cards to locks (the Motion
  page's three lanes); the step's own length, velocity and chance are the
  cursor step's cards when no pad is held (a tap that creates a step, or
  the wheel). On Roll a pad held on the note in focus does the same. Step
  Edit gets none: round 2 makes it step entry.
- P1: a kit lane's own length is set on Steps (the lane view) with Shift +
  K1, in steps of Q, the pattern's length meaning none; it loops from
  where the pattern began (not from the bar), so a 3-step hat drifts
  against a 16-step kick and meets it again after 48. Its notes past its
  length stay but are not heard. The 909 and synths have no lanes (Steps
  there edits whole steps). Why: Shift + K is the card's second parameter,
  and the lane's length is its length card's.
- P1: Note Repeat's 1/4 and 1/64 are the lit B1 and B3 pressed again
  (Stage's "lit key again steps on", dots under them), Shift their
  triplets. The arp's clock went from 24 to 96 ticks a quarter to hold
  1/64T. Pressure sets velocity once the pad reports some (before that the
  strike's), off under Full Level. Under Seq Record the arp's notes are
  written by the audio thread's count (`arp_origin` + its ticks, so on the
  grid even with swing) and the press is not written. Why: what is heard
  is what is kept; a press with Note Repeat on makes no sound of its own.
- P1 (with round 2's session, its Time Correct C pick "strength, heard
  live"): humanize and the groove are a pattern's `Feel`, with strength and
  swing, applied to the copy the audio thread plays (`feel_pattern` in
  `Context::sync_pattern`, the packs and `EngineHost::load`), never to the
  notes; humanize is a fixed offset a note (seeded by its tick and key), so
  a pass sounds like the last until the amount changes; a groove is a copy
  taken from the pattern picked, by sixteenths, 64 at most. P1 built the
  model, file (FEEL), cards (`feel_cards`) and their kinds; round 2 lays
  them out on Time Correct's page. Why: one timing path for both, and
  turning an amount back takes it back.

## Waiting for a hand check

- P0: with Rec Quantize off, record pads against the metronome: notes sit
  where played (before, about 8 ms late at 48 kHz, three periods).
- P0: fast rolls and flams on the pads keep their spacing (no 3 ms jitter),
  by ear; and a MIDI keyboard's notes the same.
- P0: an armv7 VST2 plug-in (Acid-303 in the project now on the MPC)
  playing while its parameters are turned on its page: no dropouts.
- P1: ratchets by ear: a hat at 2-8 hits with a ramp down and up, a kit
  pad's pitch ramp (a tom rising), a synth note ratcheted; say if the
  hits' gate (a tenth short, as a note's) wants to be shorter on synths.
- P1: locks by ear: a kit's hat with its tune locked up on one step and its
  level down on another, a synth's cutoff locked on one note of a line;
  say if a lock should last until the lane's next note instead of its own
  note's length.
- P1: Note Repeat by hand: a pad held lightly and hard (the repeats follow
  the pressure; say if the curve wants to be steeper or start higher),
  1/4 and 1/64 on B1 / B3 again, and a take with Seq Record kept as notes.
- P1: the feel by ear, once round 2 has it on Time Correct's page:
  strength from 0 to 100 on a loose take, humanize on a straight hat (say
  if a quarter of a step at 100 is too much), a drum pattern's groove laid
  on a bass line.
- P1: Fill by hand: Play held while playing for a bar, FILL steps heard and
  NOT FILL ones dropped, back on release; say if Play's hold should wait a
  moment (a tap does nothing while playing).

## P0: timing

- [x] Recorded notes land where they were heard: `Context::record`
  (`shell/context.cpp`) takes `host.position_of()`, the tick the engine is
  on when the MIDI thread sees the hit, so a take with Rec Quantize off is
  late by the output latency (about 8.7 ms at 3 × 128 frames). Place it
  from the hit's timestamp minus the output latency, as Manual chop's taps
  already are (`shell/shell.cpp`, `tap_frames`). Host test with a known
  latency.
- [x] Live notes at their frame: pad and MIDI-in Note On / Off (and
  `PadNote`, `ArpKey`) are applied at the start of the next block
  (`EngineHost::render`), so a hit moves by up to a block. Carry the
  rawmidi timestamp to a frame (`clock.frame_at`) and play it at a fixed
  one-block delay at that frame (split the block there, as ticks do). Host
  test: two hits a known time apart come out that far apart.
- [x] The MIDI thread (`Controller::run_midi` in the base) at a low
  real-time priority (SCHED_FIFO below the audio thread's 30 and the
  pager's 20, e.g. 15); it sleeps in `poll`, so it cannot starve anything.
- [x] VST2 bridge: the `vst2_bridge32` worker that answers the audio
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

- [x] Ratchet: `Event` gains a ratchet (1–8 hits inside the note's length)
  and a ramp (−100..100, the last hit's velocity in percent of the
  note's); on a kit also a pitch ramp if it fits. The repeats play through
  the late-note path the arp uses (`run_late`). File section, Steps / Step
  Edit / Roll editing, drawn on Steps and Roll, previews, tests.
- [x] Trig conditions: always, n:m (2..8), FILL / NOT FILL, FIRST / NOT
  FIRST, PRE / NOT PRE, NEI / NOT NEI, beside chance; a pass counter a
  track (a kit lane of its own length counts its own); a held control for
  Fill.
- [x] Per-step locks and Motion depth: Motion is 256 events for the whole
  project at sixteenth resolution, found by a linear scan
  (`model/project.h`). Store it per pattern with room for thousands, at
  tick resolution; holding a step on Steps (or the note in focus on Roll /
  Step Edit) and turning a pot writes a lock on that step. Old `MOTN`
  sections still load.
- [x] Kit lanes of their own length (polymeter inside a kit).
- [x] Note Repeat: pad pressure sets the repeats' velocity (as the MPC),
  1/4 and 1/64 rates, the repeats written as notes under Seq Record.
- [x] Humanize in Time Correct (a random timing and velocity amount) and a
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
- 2026-11-12, P0 timing: a note from a pad or MIDI in carries its receive
  stamp (base 039c64e: `SurfaceEvent::timestamp_ns`) and plays a block
  after it, at its frame (`EngineHost::send_at`, `take_commands`); Seq
  Record places it from that stamp less the output latency, on the nearest
  tick (`heard_place`, a ring of each tick's frame); the MIDI thread at
  `SCHED_FIFO` 15 (base 7985b50); the VST2 bridge at the audio priority
  behind a priority-inheriting lock. Host tests `hits_on_time`. On the MPC
  (48 kHz, three periods): remote pad presses and MIDI in notes all on
  their frame (`melodee_timed=18/0/0`), the MIDI thread at FIFO 15 and the
  Acid-303 bridge at FIFO 30 (`/proc`), a play of the project with its
  plug-ins without a dropout. Pushes owed in sample-base: 039c64e and
  7985b50 (on f8d92f0 and later 12f0679, a peer's).
