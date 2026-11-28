# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Kerem Kilic (Ellic Studio)
"""A PCM WAV read in place by the stream pager (src/melodee/engine/stream_pager.cpp), built for the host."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from sources import source  # noqa: E402
from test_melodee import SOURCES  # noqa: E402  the engine's sources, as its own test builds them


class DirectStreamTests(unittest.TestCase):
    def test_wav_read_in_place(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "direct_stream"
            subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
                            "-I" + str(ROOT / "src"), "-I" + str(ROOT / "core/src"), "-I" + str(ROOT / "base/src"), str(ROOT / "tests/direct_stream.cpp"),
                            *[source(s) for s in SOURCES], "-o", str(binary)], check=True)
            result = subprocess.run([str(binary), temp], capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("direct stream ok", result.stdout)


if __name__ == "__main__":
    unittest.main()
