# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Kerem Kilic (Ellic Studio)
"""The card's folders read ahead of the kernel (src/melodee/shell/card_warm.cpp), built for the host."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from sources import source  # noqa: E402


class CardWarmTests(unittest.TestCase):
    def test_folders_of_an_exfat_volume(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "card_warm"
            subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
                            "-I" + str(ROOT / "src"), "-I" + str(ROOT / "core/src"), str(ROOT / "tests/card_warm.cpp"),
                            source("src/melodee/shell/card_warm.cpp"), "-o", str(binary)], check=True)
            result = subprocess.run([str(binary), temp], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("card warm ok", result.stdout)


if __name__ == "__main__":
    unittest.main()
