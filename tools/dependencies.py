#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""Fetch the pinned dependencies into ignored build/deps: JUCE (through the
base's tools/dependencies.py) and Melodee's own (src/melodee/dependencies.py)."""
from pathlib import Path
import importlib.util

ROOT = Path(__file__).resolve().parents[1]


def base_dependencies():
    path = ROOT / "base/tools/dependencies.py"
    if not path.is_file():
        raise SystemExit("base/ is empty: run git submodule update --init")
    spec = importlib.util.spec_from_file_location("sample_base_dependencies", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


if __name__ == "__main__":
    base_dependencies().prepare(ROOT)
