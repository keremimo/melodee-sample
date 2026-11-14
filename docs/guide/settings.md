# Settings

Previous: [Projects and files](projects-and-files.md) · [Contents](../GUIDE.md) · Next: [Exit and troubleshooting](exit-and-troubleshooting.md)

Press **SHIFT** + **Pad 8**. Turn the encoder to a page and push to open
it. **B1** goes back.

![Settings](../images/screens/settings.png)

| Page | Settings |
| --- | --- |
| **PADS** | Pad response (soft, medium, hard, full level), pad pressure, pad lights, what Double Seq copies, Recall length and grid |
| **KNOBS** | Knob, Knob FX and fader takeover: Pickup, Scaled or Instant |
| **SCREEN** | Palette, brightness, place bar at the top or bottom |
| **AUDIO** | Sample rate (44.1, 48 or 96 kHz) and audio buffer (256 to 1024 frames). A longer buffer gives fewer crackles on a busy project. A shorter one responds sooner. |
| **SAMPLING** | Audio Recall settings, and when a Free take ends on its own |
| **MIDI** | MIDI Config (see [MIDI and sync](midi-and-sync.md)) |
| **SCREEN MIRROR** | Screen mirror (on or off) and resolution (320 x 240 up to 2560 x 1920): the screen as a USB camera for a computer |
| **ABOUT** | How the MPC is doing: processor load, audio dropouts, card space, plug-in load |

![AUDIO](../images/screens/settingsaudio.png)

Settings are the same for every project. A sample rate change stops
playback for about a second while the audio restarts.

## Screen Mirror

With **Screen mirror** on, a computer connected by USB-C sees the MPC as a
camera named **MPC Sample**. Choose it in any camera app (QuickTime Player,
OBS, a video call) to show the screen. Macs, Windows and Linux computers
and iPads with USB-C take it; iPhones do not take USB cameras.

**Resolution** sets the picture size the computer gets. Each is the screen
scaled up by a whole number, so its pixels stay sharp squares:

| Resolution | Scale | Picture |
| --- | --- | --- |
| 320 x 240 | 1 | uncompressed |
| 640 x 480 | 2 | uncompressed |
| 1280 x 720 | 3, black bars at the sides | JPEG |
| 1280 x 960 | 4 | JPEG |
| 1920 x 1440 | 6 | JPEG |
| 2560 x 1920 | 8 | JPEG |

All run at 30 pictures a second. The uncompressed sizes are exact to the
pixel; the JPEG sizes are larger than USB could carry uncompressed. If the
picture looks soft in an app, pick a larger size: the app then stretches
it less.

Under the rows: **READY** (the computer has the camera), **LIVE** (an app
shows it), **CARD SHARED** (the camera pauses while the card is shared
over USB, and comes back after). USB audio, MIDI and the network drop for
about a second when you turn it on or off or change the resolution.

The MPC never unplugs the camera while an app shows it. Turning it off or
changing the resolution then waits, with **IN USE** under the rows, until
you close the camera in that app. SD card access says **CAMERA IN USE**
and does not start, and a new sample rate reaches the computer once the
camera is closed. If Melodee restarts while an app shows the camera, the
picture stops for a moment and then goes on.

![SCREEN MIRROR](../images/screens/settingsmirror.png)

---

Previous: [Projects and files](projects-and-files.md) · [Contents](../GUIDE.md) · Next: [Exit and troubleshooting](exit-and-troubleshooting.md)
