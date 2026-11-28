# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Kerem Kilic (Ellic Studio)
"""Melodee sits on the base's layers and the core (ellic-core), nothing else (base/docs/APPS.md)."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
SRC, BASE_SRC, CORE_SRC = ROOT / "src", ROOT / "base/src", ROOT / "core/src"
# The base layers an app may include, and Melodee itself (its own and the core's).
ALLOWED = {"surface", "dsp", "drivers", "runtime", "melodee"}
INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.MULTILINE)


def sources():
    return sorted(p for p in SRC.rglob("*") if p.suffix in (".h", ".cpp"))


class LayoutTests(unittest.TestCase):
    def test_src_holds_main_and_melodee(self):
        self.assertEqual([p.name for p in SRC.iterdir() if p.is_file()], ["main.cpp"])
        self.assertEqual({p.name for p in SRC.iterdir() if p.is_dir()}, {"melodee"})

    def test_includes_name_an_allowed_folder_and_exist(self):
        for path in sources():
            for name in INCLUDE.findall(path.read_text()):
                with self.subTest(file=str(path.relative_to(ROOT)), include=name):
                    self.assertIn("/", name, "include the folder too, e.g. \"runtime/app.h\"")
                    self.assertIn(name.split("/")[0], ALLOWED)
                    self.assertTrue(any((root / name).is_file() for root in (SRC, BASE_SRC, CORE_SRC)))

    def test_nothing_shadows_the_core(self):
        # One file a name: Melodee's own never stands in for the core's (AGENTS.md).
        for path in sources():
            with self.subTest(file=str(path.relative_to(ROOT))):
                self.assertFalse((CORE_SRC / path.relative_to(SRC)).exists(), "the core has this file")


if __name__ == "__main__":
    unittest.main()
