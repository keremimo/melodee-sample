# Audio tracks

Previous: [Instrument and MIDI tracks](instrument-and-midi-tracks.md) · [Contents](../GUIDE.md) · Next: [Sequencing](sequencing.md)

An audio track records what comes into the MPC Sample, as an audio track
does on the MPC 3. Each take goes into the pattern you record it in, and
plays wherever that pattern plays: from the pads' pages, on **Patterns** and
in the **Song**. The pads play nothing on an audio track.

## Make an audio track

1. Select an empty track with **PAD BANK** + pad. Stage shows the
   [track types](tracks.md#make-a-new-track). A new project opens on track 1
   with the track types.
2. Turn the encoder to **Audio**, the last card, and push it. Stage opens on
   **INPUT**.

![The Audio card](../images/screens/stageemptyaudio.png)

The project must have been saved once before you record: its takes are kept
beside it, in the folder `<project> Audio`.

## Input

Press **SAMPLE RECORD** on an audio track, or **B2** on Stage, to open
**INPUT**. The frame shows the pattern in hand, numbered by its bars, and the
bars under it show the input's level, left over right.

![INPUT](../images/screens/stageaudioinput.png)

| Control | Action |
| --- | --- |
| **K1** (**SOURCE**) | What the track records: Rear L+R, Rear L, Rear R, Mic, USB L+R, USB L or USB R. |
| **K2** (**MONITOR**) | **Auto**: you hear the input while the track is armed or recording. **On**: you always hear it, also while you play or stop, and while a track that is not an audio track is selected (the first audio track set to On). **Off**: you never hear it. |
| **K3** (**GAIN**) | The input's gain, −12 to +24 dB (the same gain as Input Config's). |

With the built-in mic as the source, the speaker goes quiet while you hear
the mic, so it does not feed back. The header then shows **SPEAKER OFF**.
Plug in headphones to hear yourself.

While a MIDI track or a Looper loop uses the other input (the rear inputs
for Mic, the mic for Rear), the source shows **IN USE**, is silent and does
not record.

## Record

1. Select the pattern to record into. Its length is the length of the take:
   set it on **Patterns** (**K1**).
2. Press **SEQ RECORD**. The pattern's chip turns red: the track is armed.
3. Press **PLAY**. After the count-in (when Count-In is on), one pass of the
   pattern is recorded from its top. At the end of the pass recording stops,
   **SEQ RECORD** turns off and the pattern plays on with its new take.

![Armed](../images/screens/stageaudioarmed.png)

![Recording](../images/screens/stageaudiorec.png)

- Press **SEQ RECORD** while the pattern plays: the take starts the next
  time the pattern comes round.
- On Song, **SEQ RECORD** records into a new clip at the cursor. See
  [Record into the song](song.md#audio-tracks).
- Press **STOP** to end a take early. What was recorded is kept.
- Recording into a pattern that already has a take replaces it. Press
  **SHIFT** + **−** (Undo) to bring the old take back.

## Edit the take

**B1** (**CLIP**) edits the take of the pattern in hand. The take always
plays from the top of its pattern; **START** and **END** set which part of
the recording it plays. Turn the encoder for fine steps.

![CLIP](../images/screens/stageaudioclip.png)

| Page | K1 | K2 | K3 |
| --- | --- | --- | --- |
| **CLIP** | **LEVEL** (−60 to +12 dB) | **START** | **END** |
| **FADES** (**B1** again) | **FADE IN** | **FADE OUT** | **REVERSE** |
| **WARP** (**B1** a third time) | **WARP**: Off, Stretch or Repitch | **BPM** | **SEMI** (**SHIFT**: **FINE**) |

**B3** opens **FX**, as on other tracks. The track's level, pan and sends are on the
[mixer](mixer-and-effects.md#the-mixer).

## Patterns and the Song

A take belongs to its pattern. Copy the pattern, or the whole track, and the
take comes with it. On the Song, place the pattern on the track's lane like
any other pattern: each clip shows the take's wave, and plays it each time
the pattern starts.

![Takes on the Song](../images/screens/songaudio.png)

## Warp

A take plays at the speed it was recorded at. To have it follow the tempo,
turn **WARP** on (**B1** three times, **K1**), as with Warp on the MPC:

- **Stretch** plays it faster or slower and keeps its pitch. **SEMI** and
  **FINE** move its pitch without changing its length.
- **Repitch** plays it faster or slower, and its pitch moves with it.

**BPM** is the tempo the take was recorded at. Melodee sets it when you
record; change it if the take was played to another tempo.

![WARP](../images/screens/stageaudiowarp.png)

## Files

Each take is a 24-bit WAV file at the sample rate Melodee runs at, named
`Take 001.wav`, `Take 002.wav` and so on, in `<project> Audio` beside the
project. A `.peaks` file beside each take holds its overview for the screen.
**Save As** copies the folder with the project.

When a project opens, take files that no pattern plays any more (takes you
undid or recorded over) are deleted.

When you [export to MPC 3](song.md#export), each audio track becomes an MPC 3
Audio track, and each pattern's take an audio region in its sequence.

When you open an MPC 3 project, each of its Audio tracks becomes an audio
track: the region at the start of each sequence becomes that pattern's take,
with its level, fades, warp and tune. A pattern plays one take from its top,
so other regions are left out (the note after opening says so). Until you
save the project, its takes wait in a hidden folder beside the projects;
the first save copies them into the project's own Audio folder.

---

Previous: [Instrument and MIDI tracks](instrument-and-midi-tracks.md) · [Contents](../GUIDE.md) · Next: [Sequencing](sequencing.md)
