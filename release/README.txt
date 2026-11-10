Melodee {version} for the Akai MPC Sample
==========================================

Melodee is a sixteen-track groovebox that runs on the MPC Sample in place of
the stock app: one track per pad, four synths, a 909 kit and a sampler,
patterns and a song, mixer and effects.

THIS IS AN ALPHA. Expect rough edges and the odd crash. A crash, or any
failure to start, puts the stock MPC app back; nothing on the MPC itself is
changed, only files on the microSD card. Back up the card first anyway.


Requirements
------------
- MPC Sample on firmware 1.3.0.12 exactly. On any other firmware Melodee
  does not start and the stock app keeps running.
- A microSD card for the MPC (the one you already use is fine).


Install
-------
1. Back up the card. If it already has a folder named "autorun" at its top
   level (MPC Sample Plugins uses one, for example), move it somewhere
   safe: Melodee's replaces it.
2. Copy the "autorun" folder from this zip to the top level of the card,
   so the card has autorun/autorun.sh, autorun/autorun.json,
   autorun/autorun.check and autorun/melodee/.
   Do not edit or rename anything inside it: the MPC refuses an autorun
   whose files changed.
3. Put the card in the MPC and switch it on. The stock app starts first;
   a few seconds later Melodee takes over the screen.

Projects are saved on the card in Melodee/Projects.

New to Melodee? The quickstart, with pictures of each screen:
https://github.com/keremimo/melodee-sample/blob/main/docs/QUICKSTART.md


Back to the stock app
---------------------
- Press Power briefly and choose Exit: Melodee saves and the stock app
  comes back until the next time the MPC is switched on.
- To stop Melodee starting at all, delete the "autorun" folder from the
  card (and put back any autorun folder you moved away).


Reporting problems
------------------
Say what you pressed, what you saw and the version line below. If Melodee
did not start at all, check that the firmware is 1.3.0.12 and that the
autorun folder sits at the top level of the card.


Version and source
------------------
Melodee {version}, source commit {commit}
Source code: {source}
Built on sample-base: https://github.com/keremimo/sample-base

Melodee is free software under the GNU Affero General Public License,
version 3 (licenses/LICENSE-AGPL-3.0.txt). It uses JUCE under the AGPLv3
and embeds the Rubik typeface under the SIL Open Font License 1.1; see
licenses/. Not affiliated with or endorsed by inMusic or Akai Professional.
