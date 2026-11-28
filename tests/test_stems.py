# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Kerem Kilic (Ellic Studio)
"""Stems (src/melodee/shell/stems.cpp) with stand-ins for the separating program, built for the host."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from sources import source  # noqa: E402


class StemsTests(unittest.TestCase):
    def test_a_job_writes_three_files_and_leaves_nothing_else(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "stems"
            subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
                            "-I" + str(ROOT / "src"), "-I" + str(ROOT / "core/src"), "-I" + str(ROOT / "base/src"), str(ROOT / "tests/stems.cpp"),
                            source("src/melodee/shell/stems.cpp"), source("src/melodee/engine/sample_data.cpp"),
                            "-o", str(binary)], check=True)
            folder = Path(temp) / "card"
            folder.mkdir()
            result = subprocess.run([str(binary), str(folder)], capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("stems ok", result.stdout)


if __name__ == "__main__":
    unittest.main()
