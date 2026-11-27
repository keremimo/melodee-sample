# Looper

Previous: [Song](song.md) · [Contents](../GUIDE.md) · Next: [Mixer and effects](mixer-and-effects.md)

The Looper records four audio loops, L1 to L4, that follow the tempo. Each
loop is a track with its own mixer strip and effect chain.

The Looper is in Full. In Basic it does not open (see
[Basic and Full](settings.md#basic-and-full)).

To open the Looper, press **LOOP** and **16 LEVELS** together, in either
order, from any page. You can also open the Menu, press **B3** (**GO**) and
press pad 12. The first time, the Looper is off: press **B3** (**TURN ON**)
or push the encoder to turn it on. **SHIFT** + **LOOP** held for half a
second turns the Looper on or off (off asks first while a loop holds audio,
and clears the loops).

![The Looper](../images/screens/looper.png)

**B1** (**LOOPS**) shows four rings, one for each loop: its waveform, a hand
where it plays, its state and its length. Red: recording. Amber: overdub.
Grey: stopped. The header shows the quantize setting (a magnet with
**BAR**, **BEAT** or **OFF**), the selected loop and its layers.

- **K1** selects the loop, **K2** its length (Free, 1, 2, 4, 8, 16, 32, 64 or 128 bars),
  **K3** its level. The encoder and **−** / **+** also select the loop.
- **SHIFT** + **K1** feedback, **SHIFT** + **K2** quantize (Bar, Beat,
  Off), **SHIFT** + **K3** what happens after a take (Play, Overdub, Stop).

To record a loop:

1. Press **B3** (**MORE**) and turn **K1** (**SOURCE**) to choose what the
   loop records: the input, a track, another loop, the mix, or an input of
   its own (Rear, Rear L, Rear R, USB, USB L, USB R or Mic). Loops on
   different inputs can record at once. Mic and Rear share one input, so
   two loops cannot take one each.
2. Press **PLAY**, then push the encoder. The loop starts recording at the
   next bar, or where the longest loop playing starts again.
3. Push again: the loop closes on whole bars, then does what **SHIFT** +
   **K3** says (Play, Overdub or Stop).
4. Push while it plays to overdub. Push again to stop overdubbing.

A loop recorded at another tempo shows that tempo in amber. Push fits it to
the tempo now first (push again to cancel), then overdubs.

**ERASE** pressed alone clears the selected loop (asks first). In mute
mode, the pads mute the loops, one column per loop; **SHIFT** solos.

The pads control the loops too, one column per loop:

| Pads | Action |
| --- | --- |
| 13–16 | Record, overdub, play |
| 9–12 | Play or stop at the next bar or beat, as quantize says (**SHIFT**: at once) |
| 5–8 | Undo, then redo, the last overdub |
| 1–4 | Hold to clear the loop |

Hold **LOOP** on any other page: the pads are this grid until you let go.

**B3** (**MORE**) opens the loop's sheet. **B1** (**BACK**) closes it.
**B2** (**COMMIT**) puts the loop onto the next empty pad of the selected
kit. **B3** plays or stops the loop at the next bar or beat, as quantize
says; press it again to cancel. With **SHIFT** held: **CLEAR**, **+ NOTE**
(commit, add a note at bar 1 as long as the loop, and stop the loop) and
**STOP** / **PLAY** at once.

![The loop's sheet](../images/screens/loopersheet.png)

**B2** (**MIX**) shows L1–L4 as mixer strips: **K1** level, **K2** pan,
**K3** send, **SHIFT** + **K3** the bus. **SHIFT** + **KNOB FX** opens the
selected loop's effect chain; **B1** (**LOOPER**) goes back.

Loops are saved with the project, in a folder next to the project file.
For how the Looper was designed, see the
[Looper design notes](../LOOPER.md).

---

Previous: [Song](song.md) · [Contents](../GUIDE.md) · Next: [Mixer and effects](mixer-and-effects.md)
