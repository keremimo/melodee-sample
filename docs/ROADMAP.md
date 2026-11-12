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
- P1: Euclid lives on Steps' card cycle (STEPS again: TRIG, again on a kit:
  EUCLID), laid as notes at once on each turn, the lane's own length set
  to its steps so E(5,12) loops as 12 against the pattern (polymeter, as
  item 4 made possible), its notes editable after. Variations are Steps'
  Push sheet (Push was free there), each into the next empty pattern which
  then plays, as round 2's Capture C does, the original kept. Why: keys
  act and knobs set; a variation is a new take to compare, not an edit.
- P1: a track's speed is Tracks' Shift + K1 (the strip in hand; K1 picks
  the track), applied like the feel to the copy the audio thread plays
  (`played_pattern`: ticks times den / num, to the nearest tick, so ×3/2
  and ×3/4 round triplets and odd ticks by half a tick at most), its
  positions published back in written ticks for the pages and Seq Record
  (`place_at`, the arp's notes, Rec Quantize's skips scaled). Speed is not
  part of undo, as the tempo is not. Why: one place for "how it plays";
  the pattern and its file keep what was written.
- P2: one dispatch a block: the audio thread sequences the whole block
  first; a call it makes to a track's engine past the block's first frame
  is kept as an `EngineCall` with its frame (notes, slides, values, the
  wheel, pressure, mod routes, kit pads, Motion's pad and effect values),
  384 a track a block, and the track's job makes each at its frame between
  render calls (`play_calls`). Calls at the first frame go straight in. A
  full list goes in whole at the block's start, in order (counted). The
  old way stays behind `set_one_dispatch(false)` for tests and comparing.
  Why: one barrier, no per-tick wake-ups, and a plug-in track renders a
  block in one bridge round trip.
- P2: Prophet, SID, FM6 and CZ step their modulation on their own count
  of frames (`engine.h` ControlStep: 16, 16, 128, 128), restarted by any
  call that changes them, their LFOs moved on by the frames played since
  the last step; a voice is let go at a step's end. Why: an engine then
  sounds the same however the host cuts its block, so the test compares
  both ways sample for sample (exact, not within a tolerance), and a
  synth's modulation no longer jitters with other tracks' ticks. FM6 and
  CZ at 128 cost what their once-a-block step cost.
- P2: the workers never spin between blocks: woken as a block starts
  (`RenderPool::wake`), they wait awake at most 200 µs, only while the
  block is sequenced and between its two runs, then sleep. Why: measured
  on the MPC, sleeping between blocks saves ~20 % of a core; waking them
  at the block's start hides their wake-up (~70 µs) under the sequencing.
  100 µs against 200 µs made no difference.
- P2: the Looper is one job of the tracks' run (its loops, strips and
  chains), not four jobs after the tracks: its loops play what came in the
  block before, never this block's tracks, and `Looper::run` makes the four
  together. Buses A–D and Knob FX are the second run, each a job; the
  master's chain stays on the audio thread. Plug-ins are checked for two
  places at once among the tracks and loops, and among the buses.
- P2: floating stays the default (audio and three workers over four cores);
  `MELODEE_RENDER_THREADS=pinned` puts the audio thread on core 1, two
  workers on cores 2 and 3 and every other thread of Melodee's on core 0
  (the service thread sweeps them every 2 s). Measured: the dsp bench's
  mean block 1403–1592 µs floating against 1773–1836 µs pinned (pinned
  overran its budget 33 times in 15 s once); the app with the MPC's
  project, render load peak 24–26 % floating against 28.5 % pinned, the
  same total CPU. The VST2 bridge is its own process and is not pinned.
- P2: a track's load is its job's time (engine and chain) against a
  block's period, held at its peak and falling as `render_load` does:
  along each Tracks cell's bottom (the whole width a core's whole block,
  green, amber from 60 %, red from 85 %) and as the header's pill for the
  track in hand; Settings' AUDIO shows the blocks' share under its rows
  with the dropouts beside it. A dropout (the runtime's xrun count rising)
  makes the service thread note the busiest track then
  (`melodee_dropout_track` / `_load`). Why: what the screen has, no new
  timing on the audio thread.
- P2: the overload guard engages at 85 % of a block's time and lets go
  under 70 %: from 85 % each block the busiest track's oldest held voice is
  let go, and every engine's released voices end their tails in about
  30 ms (`Envelope::hurry`; a kit's pads are choked); between 70 and 85 %,
  once engaged, the tails only. Effects' tails (reverbs, echoes) are left
  alone. The 909 and plug-ins have no voices to give. It is off in
  `EngineHost` unless asked (a test's output must not hang on its
  computer's speed); Melodee and the bench turn it on.
- P2: samples keep their 44.1 kHz frames (everything that reads a sample
  but the voices still does) and gain a copy at the device's rate, made
  once on the service thread as they are read: from the file itself, its
  own frames when its rate is the device's, else one conversion with the
  base's windowed sinc (`dsp/frame_ring.h`, 90 dB, already used for
  USB-C: neither JUCE's interpolators nor inMusic's packages were needed).
  The 44.1 kHz conversion at load is that sinc too (it was linear). Copies
  share a 256 MB budget; past it, and for instruments streamed from the
  card (their caches are 44.1 kHz), voices convert as before. A rate
  change remakes each copy from its file, a slot a pass. Kerem asked on
  the way (2026-11-12): nothing may force 44.1 kHz on what plays; it
  doesn't, playback follows Settings' rate; takes and renders are still
  saved at 44.1 kHz (follow-up below).
- P2 follow-up (2026-11-12): nothing Melodee records is converted to
  44.1 kHz any more. Takes, Audio Recall, Resample and the song's mixdown
  are captured at the device's rate (`TakeRecorder`, `RecallRing`,
  `Bounce`; `engine/model_rate.h` is gone), written at it (`write_wav`,
  `WavStream` take a rate) and kept as read_wav keeps a file
  (`keep_capture`: its own frames play, its 44.1 kHz frames made once with
  the sinc). Positions, not audio, are converted: a take's `frames()` and
  `length()`, a render's `frames()`, Recall's status and every region and
  cut stay in 44.1 kHz frames. Save Sample, Trim and Extract cut a
  sample's own frames when they are what plays (`plays_own`), so a 48 kHz
  take trimmed stays 48 kHz; else 44.1 kHz as before. Captures are
  quantised to the 24-bit step they are written at (`as_written`; write
  and read now both scale by 2^23), so a rate change reads the same frames
  back from the file. Buffers are sized for 96 kHz (take 46 MB, Recall
  20 MB, render ring 8 MB: 40 MB more than before) rather than remade on
  the audio thread at a rate change; a take or render running when the
  rate changes ends there. Plug-ins (reloaded at the new rate) and Monitor
  (the source straight into the mix) already ran at the device's rate; the
  metronome's blip and the Record meter fell a fixed step a frame or a
  block (twice as fast at 96 kHz) and now take the same time at any rate,
  unchanged at 44.1 kHz. Effects with such steps (Knob FX Vinyl and Tape,
  Pad FX Vinyl) are proposed as a session of their own.
  Loops followed, Kerem's choice ("size by rate"): each loop keeps its
  frames at their own rate (a take's the device's, written and read a
  frame a frame; an older project's 44.1 kHz file read at 44100 / rate),
  its memory made 64 s at the device's rate (180 MB in all at 44.1 kHz,
  197 at 48, 393 at 96), an empty loop's made again when the rate changes
  while one with audio keeps its own; files saved at the loop's rate, the
  project's frame counts still 44.1 kHz frames.
- Effects at the device's rate: every constant an effect took a 44.1 kHz
  frame at a time (a fall, a one-pole's step, a count of frames, a chance a
  frame, a length in frames) is made the device's where the rate is known
  (an update, a punch's start, a block; `dsp.h` `fall_per_frame`,
  `step_per_frame`, `every_model_frames`, `kept`, `noise_level`,
  `click_level`, `fall_per_block`), never a frame at a time. Knob FX: Vinyl
  (crackle chance, fall and level, hiss band and level, rumble cut), Tape
  emulator (a new aim every 4096 44.1 kHz frames, drift, hiss), Pumper,
  Tremolo, Gate, Transient, Tube drive's DC blocker, Crush's and Spit's
  holds, Spit's chance and length, Spring's dispersion (its allpasses'
  corner kept, bilinear), the reverbs' damping, Shimmer's window. Pad FX:
  Alias, LoFi, Color (Cassette's and Flutter's wobble, hiss, Tube Amp, Vinyl),
  Trance Gate, Reverb's damping and spread, Upper's window, Flange, Rev
  Stepper's edges, Vinyl's clicks. Also the master's Color, a sampler
  choke's fade and a zone's duck, the looper's play, stretch and punch
  ramps and seam fades, and the mix, loop and DSP load meters' fall.
  White noise is scaled by the square root of the rates' ratio and a
  one-sample click by the ratio, so each is as loud in the audible band.
  At 44.1 kHz each constant is itself: every Knob FX kind (three settings
  each), every punch and the master's Color rendered bit for bit as before,
  with and without FMA contraction (the noise scaled where it comes in, not
  after a product). Left: control steps (filters retuned every 8 or 16
  frames) only come more often; Prophet, CZ, SID, FM6 and the 909 leave
  Melodee. Why: Kerem, "I don't wanna force 44.1khz for anything".

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
- P1: generators by ear: Euclid rhythms on a kit's hat and rim against a
  4/4 kick (E(3,8), E(5,12), rotated), and each variation of a beat and a
  bass line; say if Busier or Humanised at 30 % is too much or too little.
- P1: Fill by hand: Play held while playing for a bar, FILL steps heard and
  NOT FILL ones dropped, back on release; say if Play's hold should wait a
  moment (a tap does nothing while playing).
- P2: the synths by ear after their own control steps: now the plug-ins'
  (Melodee Poly with LFO and vibrato, Melodee PD with the wheel and an LFO
  on pitch, each stepping its modulation from its last note): no zipper,
  nothing changed. Their checks live with them (mpc-vst-melodee-*).
- P2: the overload guard by ear: a project pushed past 85 % (busy kits;
  plug-ins have no voices to give): voices thin and tails shorten instead of crackles;
  say if 85 / 70 % or the 30 ms tail suit, and whether reverb tails should
  shorten too.
- P2: samples at 48 and 96 kHz against 44.1: a kit, a long loop and
  pitched pads sound the same or brighter, never duller; Trim and Chop's
  playheads sit on the waveform as at 44.1 kHz.
- P2: the Tracks cells' load bars and Settings' AUDIO DSP row at a glance
  while a busy project plays.

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
- [x] Generators: Euclid on a kit lane (hits, steps, rotate); a pattern's
  variations (thinner, busier, humanised, reversed, an octave up / down).
- [x] A track's playback speed (×½, ×¾, ×1, ×3/2, ×2) without rewriting
  its notes.

## P2: CPU scheduling

- [x] One dispatch a block: `EngineHost::render` splits the block at every
  tick, events or not, and calls `workers.run` (a barrier) for each part,
  then once more for the chains. Engines take the block's events with
  frame offsets and render it whole; each track's engine and chain become
  one job; a block has one barrier. A test plays a busy project both ways
  and compares output sample for sample (or within float noise, said why).
- [x] With one dispatch, drop or shorten the workers' 200 µs spin
  (`engine/render_pool.cpp`); measure CPU before and after on the device.
- [x] Buses A–D, L1–L4 and their chains as parallel jobs after the tracks.
- [x] Core pinning, measured: today audio plus three workers float over
  four cores at SCHED_FIFO. Try two workers with the audio thread on cores
  1–3 and the rest on core 0 against today's, on a busy project; keep the
  better as the default and the other on `$MELODEE_RENDER_THREADS` / a
  setting.
- [x] DSP load shown: each track's share of the block (`track_ns`) on
  Tracks, the whole on Settings' AUDIO beside the dropouts; a dropout notes
  the busiest track in `status`.
- [x] Overload guard: as block time nears its budget, steal the oldest
  voices and shorten tails before a dropout.
- [x] Samples resampled once at load to the device rate (projects keep
  44.1 kHz frame counts), not per voice; takes, Recall, Resample and
  mixdowns recorded and saved at the device rate too (decisions).

## P3: sampler

- [ ] Pad layers: up to four samples a pad, by velocity range, cycle or
  random (MPC programs' layers keep working as they do).
- [ ] Chop's slices kept with the project.
- [ ] Sample slots nothing uses any more freed on save.
- [x] Pad Link through more than one pad.
- [ ] Seq Record keeps a 16 Levels pad's cutoff.
- [x] A short crossfade at a loop's seam.
- [x] Resample of the song onto a pad.
- [x] SFZ: release samples (`trigger=release`), crossfades (`xfin_*` /
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
- 2026-11-12, P1 drums and sequencer: ratchets with velocity and pitch
  ramps, each hit on its frame (46e323c); trig conditions beside chance and
  Fill on Play held (ffb5e15); Motion kept by each pattern at tick
  resolution, 8192 points, with locks from a held step or note (b477285);
  kit lanes of their own length (6edb0c3, which also fixed a preview and a
  doc line a shared staging had misplaced into 8e7e690); Note Repeat at 1/4
  and 1/64, pressure as velocity, its repeats written by Seq Record
  (70d4e85); a pattern's feel heard live, strength, swing, humanize and a
  groove from another pattern (7dc0858; round 2 laid it on Time Correct C,
  a387316); generators, Euclid on a kit lane and variations (d826576); a
  track's playback speed (7cabb1e). Host tests for each. HEAD 7cabb1e
  passes what build.sh runs (make, the 8 unit tests, self-test, monkey 3000
  moves, 246 previews with no text issues). On the MPC (48 kHz, remote
  presses, every edit undone after): ratchet hits counted
  (`melodee_hits`), Fill 1/0 (`melodee_fill`), a 1:2 condition holding
  notes back (`melodee_trigs`), a kit's EUCLID row laying its lane and
  Shift + K1 shortening it, locks from a held step (Motion points 0 to 2),
  VARY's Busier into the next empty pattern, Note Repeat at 1/64 and 16
  repeats written under Seq Record, a track at x2 and back. Pressure by
  hand and everything by ear wait above. No base changes; no pushes owed.
- 2026-11-12, P2 CPU scheduling: one dispatch a block, the synths
  stepping their modulation on their own frames so it sounds the same to
  the sample as the old way (77f001f); no spin between blocks, the workers
  woken at a block's start, and `--melodee-dsp-bench` (1a19f91); the
  Looper beside the tracks, buses A–D and Knob FX as a second run
  (3a3db0f); `MELODEE_RENDER_THREADS=pinned`, floating kept as default
  (0db8461); DSP load on Tracks, Settings' AUDIO and `status` (65e05dc);
  the overload guard (1fbca79, which also carried UI/UX's in-progress
  `hand_punch` engine hunks, finished in their baf457b); samples played
  from a copy at the device's rate, band-limited (55abe30). Host tests
  `one_dispatch` (busy project both ways, equal to the sample, with and
  without workers), `loops_and_buses_parallel`, `overload_guard`,
  `sample_rates_p2`. HEAD 55abe30, built in a snapshot of HEAD (the shared
  checkout held a peer's unfinished scales/levels work whose test failed
  in ./build.sh): make, the 8 unit tests, self-test, monkey 12 x 3000
  moves, 248 previews with no text issues. On the MPC (48 kHz, three
  periods), the project there with its five plug-ins playing, each core
  busy over 12-15 s: before P2 27.9 / 28.7 / 25.9 / 28.1 %; items 1-4
  22.4 / 20.1 / 20.2 / 20.1 %; HEAD 24.3 / 24.1 / 24.0 / 24.0 %; render
  load peak 24-26 % throughout (pinned: 28.5 %). `--melodee-dsp-bench`
  (16 busy tracks, a block a period at FIFO 30, beside the running app):
  the old way (every track cut at every tick, workers kept awake 200 µs
  after every run) 208-215 % CPU, mean block 1377-1545 µs, p99 1551-1977;
  HEAD 187-192 %, mean 1376-1487 µs, p99 1498-1796; none over budget. A
  peer deployed the shared build over HEAD at 03:27 UTC; the MPC runs that.
  No base changes; no pushes owed.
- 2026-11-12, the synths leave Melodee (Kerem's call): PROPHET, CZ and SID
  became Open MPC plug-ins in their own repositories, Melodee Poly, PD and
  Chip (mpc-vst-melodee-poly, -pd, -chip: their DSP rendered side by side
  with Melodee's engines, identical to the bit), and FM6 and the 909 kit
  are gone. A project from before opens those synths' tracks as PLUGIN
  tracks on the plug-ins with their values as state (`Plugins/...` from the
  card's top), FM6 and 909 tracks empty with their notes. PLUGIN tracks send
  the accent as velocity and Slide / Tie as CC 84 (CC 68 over a tie); Open
  MPC plug-ins run at 44.1 kHz and are converted to the device's rate
  (`plugin/rate_bridge.h`). Host tests `synths_as_plugins`; the tests that
  used PROPHET as a sound play PLUGIN tracks through a test tone runner, or
  kits.
