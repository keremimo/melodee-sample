# SPDX-License-Identifier: AGPL-3.0-only
"""Melodee's model, engine host, sequencer and mixer, built for the host."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ["src/melodee/engine/engine_info.cpp", "src/melodee/engine/engine_host.cpp", "src/melodee/engine/looper.cpp",
           "src/melodee/shell/bindings.cpp", "src/melodee/shell/loop_sheet.cpp", "src/melodee/engine/effects.cpp", "src/melodee/engine/effects_extra.cpp", "src/melodee/engine/master_comp.cpp", "src/melodee/engine/effect_info.cpp",
           "src/melodee/engine/sampler.cpp", "src/melodee/engine/sample_data.cpp", "src/melodee/engine/onsets.cpp", "src/melodee/engine/sfz.cpp", "src/melodee/engine/keygroup.cpp", "src/melodee/engine/exs.cpp", "src/melodee/engine/sf2.cpp", "src/melodee/engine/dspreset.cpp", "src/melodee/engine/stream_pager.cpp", "src/melodee/engine/render_pool.cpp", "src/melodee/shell/sample_cache.cpp",
           "src/melodee/model/project_file.cpp", "src/melodee/model/pattern.cpp", "src/melodee/model/note_edits.cpp", "src/melodee/model/smf.cpp", "src/melodee/model/generators.cpp", "src/melodee/engine/punch.cpp", "src/melodee/engine/print_render.cpp", "src/melodee/engine/print_player.cpp", "src/melodee/model/print.cpp", "src/melodee/engine/punch_mpc.cpp", "src/melodee/engine/pad_fx_info.cpp", "src/melodee/model/sound_file.cpp",
           "src/melodee/shell/page_stack.cpp", "src/melodee/shell/button_jobs.cpp", "src/melodee/shell/roll.cpp",
           "src/melodee/shell/shift_pad_jobs.cpp", "src/melodee/shell/fader_job.cpp", "src/melodee/shell/midi_route.cpp", "src/melodee/shell/mtc_in.cpp",
           "src/melodee/shell/undo_scope.cpp", "src/melodee/shell/project_kits.cpp", "src/melodee/shell/card_access.cpp",
           "src/melodee/shell/pad_lights.cpp", "src/melodee/plugin/tone3000.cpp", "src/melodee/plugin/nam.cpp"]


class MelodeeTests(unittest.TestCase):
    def test_layouts_bindings_engine_and_sequencer(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "melodee"
            subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(ROOT / "src"), "-I" + str(ROOT / "base/src"), str(ROOT / "tests/melodee.cpp"),
                            *[str(ROOT / s) for s in SOURCES], "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("melodee ok", result.stdout)


if __name__ == "__main__":
    unittest.main()
