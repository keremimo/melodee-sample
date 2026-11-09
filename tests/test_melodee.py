# SPDX-License-Identifier: AGPL-3.0-only
"""Melodee's model, PROPHET engine and engine host, built for the host."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ["src/melodee/engine/prophet.cpp", "src/melodee/engine/prophet_info.cpp",
           "src/melodee/engine/engine_info.cpp", "src/melodee/engine/engine_host.cpp",
           "src/melodee/shell/bindings.cpp"]


class MelodeeTests(unittest.TestCase):
    def test_layouts_bindings_engine_and_sequencer(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "melodee"
            subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(ROOT / "src"), str(ROOT / "tests/melodee.cpp"),
                            *[str(ROOT / s) for s in SOURCES], "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("melodee ok", result.stdout)


if __name__ == "__main__":
    unittest.main()
