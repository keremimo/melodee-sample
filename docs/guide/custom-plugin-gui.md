# Custom plug-in panels (version 1 draft)

Melodee by Kerem Kiliç (Ellic Studio) can draw an optional panel described
by the plug-in package. Version 1 is for Open MPC VST2 folders containing
`plugin-meta.xml` and one plug-in. Place an ordinary file named
`melodee-gui.txt` alongside that metadata. The plug-in and its presets keep
their normal formats.

Second Spin uses this descriptor.

```text
MELODEE-GUI 1
accent E9BF89
scope stereo
parameter 9 Input
parameter 10 Output
parameter 4 Mix
```

The first line identifies the format and version. Give a six-digit RGB
accent, then `scope stereo`, followed by zero to three parameter lines in
display order. Each parameter line contains the zero-based VST2 index
(before Q-Link ordering) and its exact, trimmed plug-in name. Use printable
ASCII, one space between fields, LF or CRLF line endings, and at most 1024
bytes. Parameter names may contain spaces and use up to 47 characters.
Each index must be unique and exposed in Melodee's ordinary controls.

On Edit for instruments and in the effect chain for effects, the panel
shows the plug-in's live stereo output and the current values of the
selected parameters. The scope shows the signed range of the processed
samples in each recent block, at the plug-in's own rate. Mono is shown in
both lanes. The instrument scope excludes the track mix, and the effect
scope goes quiet in bypass. Melodee draws the panel in its picture area and
keeps its existing control cards, knobs, parameter banks, and preset
behavior. The indicators follow edits made through those controls.

The rack reads and checks the file during plug-in load. A missing, invalid,
or unsupported file, or an unavailable parameter binding, leaves the
ordinary controls and picture in place. Reopen the project or reload
the plug-in after changing the file. Hosts that do not understand the file
can keep using the same package.

The file contains layout data. Melodee's renderer owns the drawing and the
standard parameter path owns edits. Each loaded handle publishes bounded
output peaks through lock-free atomics. The UI uses a copy of the current
trace and parameter values in its normal panel snapshot. There are no
plug-in drawing callbacks or externally owned UI buffers.

## Try it with Second Spin

1. Work on a copy of the Second Spin Open MPC folder. Add the example file
   beside `plugin-meta.xml` and load it into a track FX slot on a build with
   this draft.
2. Play a signal with distinct left and right content. The corresponding
   lanes should differ. Bypass the effect and check that the traces clear.
3. Change Input, Output and Mix from their usual parameter banks. Each
   indicator should follow its associated parameter. Navigate the other
   banks and recall a preset to check the normal controls.
4. Remove the file or change the header to `MELODEE-GUI 2`, then reload
   the plug-in. The generic FX view and all parameter banks should return.
