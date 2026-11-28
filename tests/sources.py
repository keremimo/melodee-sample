# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Kerem Kilic (Ellic Studio)
"""Where a test finds a source: Melodee's own under src/, the engine, model
and files under core/src/ (ellic-core, the core/ submodule)."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def source(name):
    """`name` as src/melodee/..., wherever it lives now."""
    path = ROOT / name
    return str(path if path.exists() else ROOT / "core" / name)
