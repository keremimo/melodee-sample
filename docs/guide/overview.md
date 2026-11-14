# Overview

[Contents](../GUIDE.md) · Next: [Coming from the stock MPC Sample](coming-from-the-stock-mpc-sample.md)

Melodee is a groovebox app for the MPC Sample. It runs from the microSD card
in place of the stock app. It changes nothing on the MPC itself.

A Melodee **project** has:

- **16 tracks**, one for each pad. A track is one of these types:
  - **Kit** (SAMPLER): 128 pads in 8 banks (A–H), each pad playing a sample
    or a sampled instrument.
  - **Plug-in** (PLUGIN): a VST3, LV2, CLAP or VST2 instrument from the card.
  - **MIDI**: notes, CCs and program changes sent to an external instrument.
- **128 patterns per track**, in 8 banks (A–H) of 16. A pattern holds notes
  and knob movements (Motion), up to 128 bars long. Each track plays one
  pattern at a time and can change pattern on its own.
- **128 scenes**. Scene 3 is pattern 3 of every track. Launch a scene to
  change all tracks at once.
- **A song**: an arrangement of pattern clips on a timeline, one lane per
  track.
- **A mixer**: a level, pan and four sends per track, four buses (A–D) and
  the master. On a kit, each pad also has its own four sends.
- **Effects**: a six-slot chain on every track, bus and the master. A slot
  holds a built-in effect or a plug-in effect.
- **The Looper**: four audio loops (L1–L4) that follow the tempo.
- **Prints**: a track can be frozen. Its patterns are rendered to audio on
  the card and play from there, which saves CPU (see
  [Freeze a track](tracks.md#freeze-a-track)).

Around the project:

- **Plug-ins** go in `Melodee/Plugins` on the card. The plug-in picker
  can also load one from any folder on the card (see
  [Plug-ins](plug-ins.md)).
- **The web page**: connect the MPC to a computer with USB-C and open
  **http://melodee.local** while Melodee runs. Its tabs are **Device**
  (updates and plug-in zips), **Files** (the card's files), **Quickstart**
  and **Manual** (this guide).
- **Updates**: a release zip copied to the card, or sent from the web page,
  updates Melodee (see [Update Melodee](projects-and-files.md#update-melodee)).

---

[Contents](../GUIDE.md) · Next: [Coming from the stock MPC Sample](coming-from-the-stock-mpc-sample.md)
