# Settings

Previous: [Projects and files](projects-and-files.md) · [Contents](../GUIDE.md) · Next: [Exit and troubleshooting](exit-and-troubleshooting.md)

Press **SHIFT** + **Pad 8**. Turn the encoder to a page and push it (or
press **B3**, **OPEN**) to open it. **B1** goes back.

On a page, turn the encoder to a row. **K1** sets it: its card shows the
row's name and value. **−** / **+** step it one value at a time. **Sample
rate**, **Audio buffer** and the three Screen Mirror rows change once **K1**
stops turning; until then the new value shows in amber. A page with more
rows than the screen holds scrolls: a
bar at the right edge shows where you are, with an arrow at the top or
bottom while more rows are that way. On the first page and on **AUDIO**,
the header shows the sample rate and the buffer (in ms) the audio runs at,
and in amber the audio dropouts since Melodee started.

![Settings](../images/screens/settings.png)

| Page | Settings |
| --- | --- |
| **PADS** | Pad response (soft, medium, hard, full level), pad pressure, Repeat velocity (Strike: every repeat as hard as the pad was struck; Pressure: as hard as the pad is held), pad lights (track colours, white, off), **Pad fade** (how fast a pad's light fades after its sound stops: 60, 100, 150 or 250 ms), **Tap Tempo light** (flashes on the beat while playing, on or off) |
| **SEQ** | **Rec quantize** (on or off, as **SHIFT** + **Pad 10**: recorded notes land on the pattern's **Q**), **Follow** (Step Edit and Roll while the pattern plays: Off, Page or Step; see [Sequencing](sequencing.md)), **Note preview** (on or off: a note you write or move on Step Edit or Roll without playing it is heard once; see [Sequencing](sequencing.md)), **Mute** (**Audio**: a muted track or pad is silent at once; **Notes**: it plays no new notes and what it plays rings out; see [Basics](basics.md)), what Double Seq copies, Capture length and grid, **Autosave** (On, or Off: the project is saved only when you save it; see [Projects and files](projects-and-files.md)) |
| **KNOBS** | Knob, Knob FX and fader takeover: Pickup, Scaled or Instant |
| **SCREEN** | **Palette** and **Font** (see below), **Brightness** (20 to 100 %), place bar at the top or bottom, **Plug-in screens** (a plug-in's own screen and page files' art, on or off), Screen mirror (on or off), its resolution and frame rate: the screen as a USB camera for a computer |
| **AUDIO** | Sample rate (44.1, 48 or 96 kHz; 44.1 kHz only on MPC firmware 1.4) and audio buffer (256 to 1024 frames). A longer buffer gives fewer crackles on a busy project. A shorter one responds sooner. **USB volume**: what a computer records from the MPC over USB-C (inputs 1 and 2). **Full** (the default) sends the mix at full level whatever **MAIN VOLUME** is set to; **Knob** follows **MAIN VOLUME**, as the jacks do. **USB out**: what a computer records on inputs 3 to 18 after the mix. **Tracks** (the default): tracks 1 to 16, one mono input each. **Submixes**: submixes 1 to 8 in stereo (3 and 4 submix 1, up to 17 and 18 submix 8), each as it goes into the master. Changing it keeps the computer connected. **USB-C in**: **Direct** (the default) plays what a computer sends straight to the jacks, about 3 ms after it arrives, wherever Melodee would play it (a MIDI track's **AUDIO** input on USB with **MONITOR** on, Input Config's monitor, a loop), at the level and pan it would have there but without effects; **Tracks** plays it through those, with their effects, about 8 ms later. **Direct level**: −24 to +6 dB on top (0 dB: as loud as **Tracks**), then **MAIN VOLUME**. Turning it on or off, muting and the volume knob fade it in a few milliseconds. **File sharing**: the card as a network drive for a computer on USB-C (on or off; see [Projects and files](projects-and-files.md#file-sharing)). Under the rows: **DSP**, the audio's processor load, with the dropouts in amber, and **PDC**, how long plug-in delay compensation holds the sound back while a plug-in needs it. |
| **SAMPLING** | Audio Recall settings, when a Free take ends on its own, and the level that counts as silence (**Silence below**) |
| **MIDI** | MIDI Config (see [MIDI and sync](midi-and-sync.md)) |
| **ABOUT** | How the MPC is doing: processor load, audio dropouts, card space, plug-in load. **B3** shows the version and what Melodee is built on. |

![SEQ](../images/screens/settingsseq.png)

![AUDIO](../images/screens/settingsaudio.png)

Settings are the same for every project. A sample rate change stops
playback for about a second while the audio restarts.

Melodee keeps the settings on the card, in `Melodee/settings`. These
choices made outside Settings are kept there too:

- **Full Level** (**SHIFT** + **Pad 1**), **Count-In** (**SHIFT** + **Pad 4**)
  and **Rec Quantize** (**SHIFT** + **Pad 10**)
- The metronome and its level
- Input Config
- Your starred sounds
- **PREVIEW** in the sample browser, on or off
- **BEST** (**B2**) on a sample's stems
- How Chop's **TO PADS** plays the slices: **POLY**, **MONO**, **THRU** or **KIT**
- The Chop type picked last (**K3** on Chop)

## Basic and Full

On the first Settings page, **K1** (**FEATURES**) switches between **Basic**
and **Full**. Melodee starts on Full and stays there until Basic is
picked.

**Full** shows every page. **Basic** shows the stock app's pages plus
tracks, plug-ins, the mixer and Song. Nothing is deleted or changed by the
switch: what a project holds keeps playing, and Full shows it again.

| Area | Basic leaves out |
| --- | --- |
| The MENU's **GO** | **MOTION** and **LOOPER**. The other places keep their pads. |
| The MENU's **HERE** | **LOOPER**, **FREEZE** and **FOLLOW TO ALL** |
| **SEQ** | **B3** (**MOTION**, **MOD**, **MACRO**), unless the pattern holds Motion. Follow actions and **VARY**. |
| The mixer | **EQ**, **COMP**, **FX** and the submixes. **SENDS** keeps send A, **SHIFT** + **K1** pan, and the fader the level. |
| A kit pad's **SAMPLE** pages | **LAYERS**, **COLOUR**, **FOLLOW**, **CLOUD**, **SENDS** and **KIT FILT**. The stock app's seven pages stay: Trim, Tune, Amp Env, Play, Mix, Filter, Filt Env. |
| **LOOP** | The Looper: **LOOP** held and **LOOP** + **16 LEVELS**. A tap of **LOOP** still loops the last pad. |

These still work in Basic by their own keys: every track type, Step Edit's
**TRIG** and **EUCLID** cards, freeze (**SAMPLE RECORD** on a plug-in
track), the auto sampler, a sample's stems (**SHIFT** + **B3** on
**TRIM**), stems and MPC export on Song, the buses' effect chains.

## Usage log

On the first Settings page, **K2** (**USAGE LOG**) turns the usage log on
or off. It is off until you turn it on.

With it on, Melodee writes what your hands do to a file in
`Melodee/Usage` on the card: the pages you open, presses and turns that
did nothing on a page, undo, MENU picks and lesson steps. It writes no
project, sample, plug-in or file names. A new file starts each time
Melodee starts. Send the files to the developer to show where Melodee is
hard to use. `tools/usage_report.py` in the source lists what they hold.

## Palette and Font

In **SCREEN**, press **Push** on **Palette** or **Font** to show the choices
as cards. Turn the **Data** wheel (or press **−**/**+**) to move between
them. The whole screen changes as you go, so you see each one on the page.
Press **Push** or **B3 KEEP** to keep it, or **B1 BACK** to go back to the
one you had.

Palettes: NIGHT (the default), DAY, CONTRAST, SUNSET, OCEAN, NEON, CANDY,
LO-FI and LCD. LCD looks like an old sampler's backlit LCD: dark letters on a
grey-green screen, no colour, always in the Melodee Pixel font. Each track keeps its colour family in every palette, so track 1 is
always coral; on DAY and CANDY the colours are darker so they read on the
light screen. The pads keep their own colours.

Fonts: Rubik (the default), Inter Tight, Atkinson, Space Grotesk, IBM Plex
and Manrope, and two pixel fonts: Melodee Pixel and VT323. Melodee Pixel is
drawn only at whole sizes of its grid, so its edges stay sharp.

![Palette cards](../images/screens/settingspalette.png)

## Screen Mirror

Screen Mirror needs MPC firmware 1.3.0.12. On firmware 1.4 the rows stay,
but the computer sees no camera.

In **SCREEN**, with **Screen mirror** on, a computer connected by USB-C sees the MPC as a
camera named **MPC Sample**. Choose it in any camera app (QuickTime Player,
OBS, a video call) to show the screen. Macs, Windows and Linux computers
and iPads with USB-C take it; iPhones do not take USB cameras.

**Resolution** sets the picture size the computer gets, and **Frame rate**
how many pictures a second (30 or 60). Each size is the screen scaled up by
a whole number, so its pixels stay sharp squares:

| Resolution | Scale | 30 fps | 60 fps |
| --- | --- | --- | --- |
| 320 x 240 | 1 | uncompressed | uncompressed |
| 640 x 480 | 2 | uncompressed | JPEG |
| 1280 x 720 | 3, black bars at the sides | JPEG | JPEG |
| 1280 x 960 | 4 | JPEG | JPEG |

Uncompressed pictures are exact to the pixel; JPEG is used where USB could
not carry them uncompressed. The pictures go out evenly spaced, about two
frames behind the MPC's screen. If the picture looks soft in an app, pick a
larger size: the app then stretches it less.

Under the rows while it is on: **READY** (the computer has the camera), **LIVE** (an app
shows it), **CARD MOUNTED** (the camera pauses while the card is mounted on a computer,
and comes back after). USB audio, MIDI and the network drop for
about a second when you turn it on or off or change the resolution.

The MPC never unplugs the camera while an app shows it. Turning it off or
changing the resolution or frame rate then waits, with **IN USE** under the rows, until
you close the camera in that app. SD card access says **CAMERA IN USE**
and does not start, and a new sample rate reaches the computer once the
camera is closed. If Melodee restarts while an app shows the camera, the
picture stops for a moment and then goes on.

![Screen Mirror in SCREEN](../images/screens/settingsmirror.png)

---

Previous: [Projects and files](projects-and-files.md) · [Contents](../GUIDE.md) · Next: [Exit and troubleshooting](exit-and-troubleshooting.md)
