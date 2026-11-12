# Melodee quickstart

You have the release zip and an MPC Sample. This page takes you from the
card to a saved beat, one screen at a time. Everything else is in
[MELODEE.md](MELODEE.md).

Melodee is an alpha. A crash, or any failure to start, puts the stock MPC
app back; nothing on the MPC itself is changed, only files on the card.

## 1. Install

You need an MPC Sample on firmware **1.3.0.12** exactly. On any other
firmware Melodee does not start and the stock app keeps running.

The zip holds an `autorun` folder, a `README.txt` and the licences.

1. Back up the microSD card. If it already has an `autorun` folder at its
   top level, move that somewhere safe: Melodee's replaces it.
2. Copy the zip's `autorun` folder to the top of the card, so the card has
   `autorun/autorun.sh` and `autorun/melodee/`. Do not edit or rename
   anything inside it: the MPC refuses an autorun whose files changed.
3. Put the card in the MPC and switch it on. The stock app starts first; a
   few seconds later Melodee takes over the screen.

**Back to the stock app:** press Power briefly. The Exit sheet opens: **B3
LEAVE** saves the project and returns to the stock app until the next time
the MPC is switched on; **B2 POWER OFF** saves it and turns the MPC off; B1
stays in Melodee. Next time, Melodee opens the project you had open. To
stop Melodee starting at all, delete the `autorun` folder from the card.

## 2. The screen

![Stage on a synth track](images/quickstart/stage.png)

Every page is laid out the same way:

- **Top row: B1, B2, B3.** The three buttons above the screen are the
  page's soft keys; the name over each says what it does. The highlighted
  one is where you are.
- **Header.** Left: play state and tempo. Right: the selected track's
  number, its engine and its sound.
- **Picture.** What the page edits: a sound list, an envelope, a waveform,
  a grid.
- **Bottom row: K1, K2, K3.** The three knobs under the screen turn the
  cards above them. The card with a ring is the one touched last. A card
  with an arrow (⇧) has a second job on Shift + that knob.

The printed buttons keep their MPC meaning, and the jobs printed under the
pads are Shift + pad.

## 3. Stage: the track's quick pages

Press **Sample** from any page to get to Stage: the quick pages of the
selected track. What you see depends on what the track is.

- **B1–B3** each hold up to three pages. Press the highlighted key again
  to step to its next page; another key jumps to its page. The dots under
  each key show which of its pages you are on. Each track remembers its
  own page.
- **K1–K3** turn the cards. The **wheel** fine-edits the ringed card, and
  **Push** moves the ring to the next card.
- The pads play the track. −/+ change the octave (a kit's bank).

A new project has one empty kit on track 1, in C Chromatic; the other tracks
are empty until you give them an engine.

**A synth: PRESET (B1).** K1 or the wheel walks the sounds; the one you
stop on loads a moment later. K2 picks the list (all, starred, recent), K3
the engine. Push opens the full Sounds list.

![Stage PRESET on a PROPHET](images/quickstart/stagepreset.png)

**A synth: AMP ENV (B2, second page).** B2 holds the shape pages: on a
PROPHET FILTER ENV, AMP ENV, LFO. B1 is PRESET, TONE, MIX; B3 is ARP, CHORD,
the engine's own page, FX.

![Stage AMP ENV on a PROPHET](images/quickstart/stageampenv.png)

**A kit: TRIM (B1).** The last pad you hit: its waveform, with K1 start,
K2 end, K3 loop point. B1 goes on to MIX and AMP ENV, B2 is TUNE and PLAY,
B3 FILTER and FILT ENV.

![Stage TRIM on a kit](images/quickstart/trim.png)

**A 909: VOICE (B1, second page).** Edits the drum the last pad played;
the B1 key reads its name (here SNARE, for the clap on pad 3). B1 also
holds PRESET and LEVELS, B2 is MIX, B3 FX.

![Stage VOICE on a 909](images/quickstart/stage909voice.png)

**A MIDI track: CC (B2).** Three controllers on K1–K3, sent to the
track's port and channel as you turn; Shift + K picks a controller's
number. B1 is DEVICE and PROGRAM, B3 ARP.

![Stage CC on a MIDI track](images/quickstart/stagemidicc.png)

An empty track offers three doors as tiles: SYNTH, KIT and MIDI, the knob
under each choosing (K1 the engine, K2 where a kit's first sound comes from,
K3 the MIDI port); the key under a tile makes that track. One of SYNTH's engines is **PLUGIN**: a VST3
or VST2 instrument from a `Plugins` folder at the top of the card (Linux
ARM builds only; the picker dims the rest and says why).

## 4. Picking a track

Sixteen tracks, one per pad: pad 1 (bottom left) is track 1.

**Hold Pad Bank and tap a pad** to select that track, from any page. While
Pad Bank is held a second the screen shows all sixteen tracks.

![Pad Bank held](images/quickstart/padbank.png)

**Press Sample on Stage** for Tracks, the mixer: all sixteen tracks as the
pads sit, each with its pattern, engine glyph and level, and the track in
hand as a strip on the right (fader, meter, pan, send). Tap a pad to select
its track; K2 sets its level, K3 its pan, Push opens its Stage. B2 opens
the Looper, B3 the Sends. Sample again goes on to Keys (Pads on a kit),
and once more back to Stage.

![The Tracks screen](images/quickstart/tracks.png)

## 5. Making a beat

Press **Seq** for Patterns: tracks down, sixteen patterns across. The pads
play the selected track. **Hold Seq** and they are its patterns: tap one
to queue it (at the next bar, or at once when stopped); the screen shows
them after a moment. K1 sets the pattern's length in bars, K2 its grid
(Q), K3 swing; holding Seq, they do that from any page, and the wheel
sets the tempo. Tap Seq again for the editor (Steps, Roll, Motion on B1–B3).

![Patterns](images/quickstart/patterns.png)

To record:

1. Select a track (step 4) and a pattern (hold Seq, tap its pad).
2. Press **Seq Record**. Stopped, it arms (REC ARMED, the button flashes).
3. Press **Play**. Recording starts; play the pads. Seq Record while
   playing records at once.
4. Press **Seq Record** again to stop recording; playback goes on.
   **Stop** stops.

![Seq Record armed](images/quickstart/recarmed.png)

Play starts from the top. Each take is one undo step: **Shift + −** undoes,
Shift + + redoes. Shift + Pad 4 turns on a bar of count-in. Shift + Pad 10
opens Rec Quantize: B3 turns it on or off (notes land on the grid), the
wheel picks the grid (the pattern's Q, or 1/16, 1/32 and the rest).

## 6. Samples

Samples live on a kit track (track 1 on a new project). Put WAV files in a
`Samples` folder on the card; 16, 24 or 32-bit, any rate, up to 60 s.

**Load: Sample Select.** On a kit, Sample Select opens the browser. The
wheel moves, Push opens a folder or loads the file onto the last pad you
hit. B2 switches between the card and the MPC's internal factory samples;
B3 PREVIEW plays each file as you land on it. Shift + pad loads onto the
next empty pad.

![The sample browser](images/quickstart/samples.png)

**Multisampled instruments.** An SFZ (`.sfz`), an MPC keygroup program
(`.xpm`, or an MPC 3 track `.xty`) or a Logic EXS24 / Sampler instrument
(`.exs`) loads the same way, onto one pad; the browser tags each by its
kind. Put an `.exs` in the folder with its samples (or a `Samples` folder
beside it) and Melodee finds them as Logic does. An MPC drum program
(`.xpm` or `.xty`, tagged KIT) loads as the whole kit instead, its cycled
hits kept. Push on an MPC expansion (`.xpn`, tagged INSTALL) installs it
into `Samples/Expansions` and opens it; with PREVIEW on, its programs play
their own previews.

![Instruments in the browser](images/quickstart/samplesinstruments.png)

The first load prepares the instrument's files on the card, which can take
a while for a big library; after that it opens at once. TRIM then shows
which keys and velocities it covers instead of a waveform, and names
anything in the file Melodee does not play.

![TRIM on an instrument](images/quickstart/triminstrument.png)

An instrument's pad plays while you hold it and fades out over its release
when you let go. To play it in pitch, press **16 Levels**, then B3 (Tune,
then Scale) or Shift + 16 Levels for the type: Tune puts its own pitch on
pad 4, Scale walks the project's scale from pad 1. **− / +** move all
sixteen pads an octave down or up. While 16 Levels is on, a MIDI keyboard
plays the instrument at real pitch.

![16 Levels on an instrument, an octave up](images/quickstart/levelsoctave.png)

**Cut: Chop.** Chop slices the last pad's sample. K1 and K2 move the
slice's start and end, K3 picks how it slices (Threshold, Regions, Beats,
Manual). The pads play the slices. B3 TO PADS lays them out on pads; B1
DROP goes back.

![Chop](images/quickstart/chop.png)

**Record: Sample Record.** On a kit, Sample Record opens Record. K1 picks
the source (mic, rear inputs, USB), K2 the length, K3 the level the take
starts at. Tap a pad to record onto it; other pads tapped during the take
cut it there. The pad again, Stop or Push ends the take; Sample Record
ends it and opens TRIM. Shift + Sample opens the full Input Config.

![Recording onto a pad](images/quickstart/record.png)

## 7. Loops

Four audio loops that follow the tempo, on tracks of their own (L1–L4).

**Open: press Loop and 16 Levels together** (either first), from any page,
or Sample to Tracks, then B2 LOOPER. The first time, the Looper is off: B3
or Push turns it on (or hold Shift + Loop half a second). Held on any page,
Loop turns the pads into the loop grid (below) for as long as it is down.

Each ring is one loop: its waveform bent round, a hand where it plays, its
state (ARM, REC, DUB, PLAY, STOP) and its length. K1 picks the loop, K2 its length
(Free or 1–16 bars, set before recording), K3 its level. A loop playing at
a tempo other than the one it was recorded at shows that tempo in amber; it
keeps its pitch and stays on the beat.

![The Looper](images/quickstart/looper.png)

To record a loop:

1. Pick what it records: **B3 MORE**, then turn **K1 SOURCE** through
   Input, the tracks, the other loops, Mix and the inputs. B1 closes the
   sheet (or leave it open: Push records from it too).
2. Press **Play**, then **Push**. The loop arms and starts recording on the
   next bar.
3. Push again: it closes on the next bar and plays. (A set length closes
   by itself.)
4. Push while it plays to overdub, Push again to stop overdubbing.

The pads are the loops too, a column each: the top row records and
overdubs, the next plays or stops on the next bar, the third undoes the
last overdub, and the bottom row held clears the loop.

**B3 MORE** opens the loop's sheet: K1 where it records from, K2 its
length, K3 its level; **B2 COMMIT** puts the loop onto the next empty pad
of the selected kit track, **B3** plays or stops it on the next bar. Hold
Shift for the second row: **CLEAR** (asks), **+ NOTE** (commit and a note
at bar 1) and **STOP** or **PLAY** at once. **B2 MIX** is L1–L4's levels,
pans and sends; Shift + Knob FX opens a loop's effects.

![The loop's sheet](images/quickstart/loopersheet.png)

Loops are saved with the project, next to its file.

## 8. Save the project

Press **Shift + Pad 16** for Project: **B1 SAVE**, B2 OPEN (the projects
on the card), B3 NEW. Save is one press (Push too). In the list, Push on a
project opens its sheet on Save or Load; B2 MORE has the rest.

![Project: Save, Open, New](images/quickstart/project.png)

The first time, Melodee asks for a name. The pads type like phone keys,
each showing the letter a tap gives now (pad 14 space, pad 15 digits, pad
16 delete), the wheel moves the cursor. B3 SAVE or Push saves, B1 cancels.

![Naming a project](images/quickstart/nameproject.png)

Projects go to `Melodee/Projects` on the card. Leaving through the Exit
sheet saves too, and Melodee opens the same project the next time it
starts.

## 9. Where to go next

[MELODEE.md](MELODEE.md) covers every button, page and Shift job: Tracks
and Keys (press Sample again), the editors and Step Edit (Seq, Shift + Seq),
Song (Shift + Pad 12), the effects (Knob FX, Pad FX, Shift + Knob FX),
16 Levels, Note Repeat and the arp, the Looper, plug-ins, MIDI and SD card
access.
