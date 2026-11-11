# Melodee: the Looper (design)

Four tempo-synced audio loops, the looper of `mpc-sample-plugins` brought
into Melodee with all its jobs: record, close, overdub and play from one
press, quantised to the bar or beat, a set length or free, feedback and a
one-step undo of the last overdub, a source per loop (the input, a track,
another loop or the mix), the transport followed (Play lines every loop up,
Stop holds them), loops kept on the beat and in pitch when the tempo moves,
fitted to a new tempo before an overdub, a 4 x 4 pad grid, Commit to pad
(+ a note at bar 1), and the remote's `looper` commands. Nothing of the
plugin's code is used: it is written again for Melodee's engine host, and the
plugin's élastique (a stock MPC library) is replaced by a stretch of our own.

Status: Kerem's answers of 2026-11-10. **Built** (L1–L5), checked on the MPC on 2026-11-11.

## Decisions (Kerem, 2026-11-10)

| Question | Answer |
| --- | --- |
| Where a loop's audio goes | Four tracks of their own, **L1–L4**, there while the Looper is on: each a mix strip (level, pan, mute, solo, send and bus) and an effect chain of six, beside the sixteen tracks. The sixteen tracks keep every pad. |
| How it opens | **Loop double-tapped** (within 450 ms) or **held** half a second opens the Looper page; a single tap keeps the MPC's job (the last pad's loop), and the first tap's toggle is taken back. **16 Levels** of type **Loop** (Shift + 16 Levels) opens it too, as in the plugin. |
| Saved | With the project: each loop's audio as `Melodee/Projects/<project> Loops/Loop N.wav` (24-bit stereo, 44.1 kHz), its values in the project file (`LOOP`: settings and a self-sized record a loop; `LPMX`: L1–L4's strips and chains). The folder is not listed on Project and goes with its project (rename, move, duplicate, delete). A loop saved stopped comes back stopped. |

## What it does

Each loop is in one state: **Empty**, **Armed** (waits for its boundary),
**Recording** (its first take), **Playing**, **Overdub** or **Stopped**.

- **Push** (or a pad of the grid's top row): Empty → Armed, then Recording
  where the longest loop playing starts again (none playing: the next
  boundary), never halfway through it; Recording → closes on whole bars from
  the take's start: free, the nearest bar (at least one; one just passed
  closes at once and drops what ran past it), a set length on its last bar
  (pressing does not cut it short). Stop now or the transport stopping keeps
  the whole bars it has. After the first take the loop does what
  **After take** says (Play, Overdub or Stop). Then Playing ⇄ Overdub at once
  (punch in and out, short fades). A Stopped loop plays again on the next
  boundary.
- **Boundaries**: Quantize **Bar**, **Beat** or **Off** (at once), for takes,
  plays and stops. Overdub always punches at once.
- **Length**: Free, 1, 2, 4, 8, 16 bars; set before recording.
- **Level** is the loop track's level; **Feedback** (0–100 %) is how much of
  the old layers each overdub pass keeps.
- **Undo** takes the last overdub pass back, again redoes it (the layer count
  follows).
- **Stop at the next boundary**, again cancels; **Stop now**; **Clear** (asks).
- **Source**, changed between takes: **Input** (Input Config's source after
  its gain, as Sample Record hears it), **Track 1–16** (after its chain, before
  mute), **L1–L4** (another loop's track) or **Mix** (the master before main
  volume, as Resample hears it), or a hardware input of its own: **Rear**,
  **Rear L**, **Rear R**, **USB**, **USB L**, **USB R** or **Mic** (after
  Input Config's gain; one side is copied to both, centred). Each loop picks
  its own, so L1 on Rear L, L2 on Rear R and L3 on USB-C record at once. The
  mic and the rear inputs share one converter pair, so one loop on each is
  refused ("Mic and Rear share one input") and the picker skips the other;
  a loop's pick routes it whatever Input Config says, and USB-C stays open
  while a loop uses it. A loop on its own input hears it through its track
  as Input Config's **Monitor** says: Off never, On always, Auto while it
  is armed, records or overdubs (a block after Monitor). A loop never
  records itself; overdubbing from Mix replaces instead of adding (the mix
  holds the loop already): a bounce.
- **Lag**: Input and the hardware inputs arrive a converter round trip late;
  the loop writes them that far behind its play head (`looper latency
  FRAMES`, the audio delay by default). Tracks, loops and the mix arrive in
  the same block: no lag.
- **Transport** (Follow, the default): Stop closes a take in progress, ends
  an overdub and silences every loop, which keeps its place; while stopped a
  press arms and plays queue, and nothing starts until Play. **Play** lines
  every loop up with the song position from bar 1 and starts the armed takes
  on its downbeat. A loop armed while stopped counts in as Seq Record does
  (Count-In on: a bar of clicks first), and the metronome's Rec setting
  clicks while a loop records or overdubs. **Free** (`looper transport
  free`): the loops keep time on their own at the tempo while stopped.
- **Tempo**: each loop remembers the tempo it was recorded at. At another
  tempo it stays on the beat and keeps its pitch (a time stretch on the
  audio thread: two overlapping taps whose start is pulled onto the best
  match, WSOLA). Push on a stretched loop first **fits** it to the tempo now
  (rendered on the service thread while it plays on; Push again cancels),
  then the overdub starts. Fitting forgets the undo; a loop too long for its
  memory at the new tempo cannot be fitted.
- **Commit to pad** (kit tracks): the loop as `Samples/Looper/L<n> Loop m.wav`
  (the number counts up) onto the selected kit's next empty pad, one undo;
  **+ note** also writes one note at bar 1 of the kit's pattern as long as
  the loop, and stops the loop. A loop recording or overdubbing cannot be
  committed; one longer than a sample may be (60 s) is refused.

Memory: each loop holds 64 s of stereo audio and as much again for the undo,
at 44.1 kHz whatever the device runs at (`engine/model_rate.h`): 45 MB a
loop, 180 MB in all, made by the service thread when the Looper is turned
on and freed when it is turned off (the MPC has 2 GB).

## The page

**Looper** (LOOPER on Tracks; 16 Levels of type Loop). Calm, as every
page: the four loops as rows, each its tag (L1–L4 in its colour), its state
as a word (ARM, REC, DUB, PLAY, STOP; a change waiting blinks), its
waveform with the playhead, its length in bars and its source. The header
shows bar.beat, the tempo and the selected loop's layers.

- **B1 LOOPS**: K1 the loop (also the wheel), K2 length, K3 level; Shift +
  K1 feedback, Shift + K2 Quantize, Shift + K3 After take.
- **B2 MIX**: the four loop tracks as strips (level, pan, send and bus,
  meter, mute and solo); K1 level, K2 pan, K3 send, Shift + K3 the bus.
- **B3 MORE**: the loop's sheet, opening on its likely job as a tile (Play or Stop on the boundary with audio in the
  loop, Source before a take), B2 MORE for all of them: Source (Push steps it), Play / Stop at the
  next boundary, Stop now, Undo / Redo last overdub (layers), Commit to pad,
  Commit + note at bar 1, Clear loop (asks), Looper off (asks). B1 closes.
- **Push**: record → close → overdub ⇄ play, on the selected loop.
- **Shift + Knob FX** on the Looper opens the selected loop's effect chain.
- With the Looper off the page asks first: **B3 TURN ON**.
- **Shift + Loop held** half a second turns the Looper on or off from any
  page (off asks first while a loop holds audio); a tap stays Reverse.

**Pads** on the Looper are the loop grid, and on any other page while Loop
is held (lit from the press; half a second held, or a grid pad, takes the
press's own loop toggle back, and letting Loop go gives the pads back); each
column a loop, pad 1 bottom left:

| Pads | Press | Light |
| --- | --- | --- |
| 13–16 | Record, close, then overdub ⇄ play (as Push) | Red recording, blinking armed, orange overdubbing, the loop's colour dim otherwise |
| 9–12 | Play or stop on the next boundary; again cancels. **Shift**: at once | The loop's colour while playing, brighter on each beat, white on the bar; dim stopped; blinking queued |
| 5–8 | Undo, then redo, the last overdub | White when undo is there, dim for redo |
| 1–4 | Hold 0.6 s to clear | Dim red while the loop has audio, filling to red while held |

Mute mode on the Looper: a column's pads mute its loop track, Shift solos.

## Engine (audio thread)

`engine/looper.h` / `.cpp`: four loops, owned by the audio thread. The
engine host runs it once a block:

1. **Play**, before the tracks are summed: each loop's output for the block
   into its track (stereo, then the strip: gain and pan as a stereo engine,
   its chain, mute and solo, send).
2. **Record**, at the block's end, when every source is there: the input
   after `select_input`, each track's buffer after its chain, the loop
   tracks, and the mix as Resample hears it.

Musical time: the block's first tick (a double: `tick_now` less what is left
of the tick) and ticks a frame; bar and beat in ticks. A loop's place is
`(tick - start) mod ticks`, so it stays on the beat through tempo changes;
the source index is that fraction of its frames. At the tempo it was
recorded at it reads its frames (linear interpolation from 44.1 kHz to the
device), otherwise it stretches. Writes go to 44.1 kHz frames through a
19 kHz low pass at 96 kHz and linear interpolation at every whole frame the
clock passes.

Overdub: a frame's old layers are copied to the undo buffer the first time a
pass writes it (the pass's region grows from where it punched in), then
`audio = audio * feedback + input` (replace: `input`). Undo swaps the
region with the undo buffer, 8192 frames a block. Fades: 32 frames at the
take's seam and at each punch; 64 at a play, stop or level change.

Threads: the shell sends `Command::Looper` (index = what, track = loop,
value, position); the service thread hands memory in and takes it back
through queues of its own (`give`, `retired`); a fit is asked for and
finished through atomics (the audio thread never waits). The screen reads
each loop's published view and 256 peaks.

## Phases

- **L1 engine**: `engine/looper.*`, the loop tracks in the host's mix,
  memory on and off, Command::Looper, `melodee-looper` remote commands,
  host tests (takes, quantize, length, overdub, feedback, undo, transport,
  sources, lag).
- **L2 page**: the Looper page (LOOPS, MIX, MORE), Loop double tap and hold,
  the loop grid and its lights, 16 Levels Loop, mute mode, FX on L1–L4,
  previews.
- **L3 tempo**: the stretch, Fit to tempo, count-in and the metronome.
- **L4 keep**: saving and loading with the project, Commit to pad (+ note).
- **L5 docs**: MELODEE.md, QUICKSTART.md (section 7), device check.

Checked on the MPC (2026-11-11): Loop held and tapped twice open the page;
takes on the bar from a track and the mix, overdub, undo and redo, a stop on
the next bar and Now; all four loops playing stretched at 132 BPM with the
project playing (the app at 9 % CPU, no dropouts); Fit (4 bars, 352800 to
320727 frames) then overdub; Commit + note onto a kit's pad; save, a new
project, and the loops back from the file (a stopped one stopped).

Open: the input's round trip (`looper latency`) defaults to the output
queue; it wants measuring with a cable from line out to line in.
