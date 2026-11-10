#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""Run Melodee on an MPC Sample over SSH: the base's device.py
(base/device.py) with this repository's build. Same commands: start,
install-sd, uninstall-sd, status, ctl, screenshot (and stock)."""
from pathlib import Path
import importlib.util
import sys

ROOT = Path(__file__).resolve().parent


def base_device():
    path = ROOT / "base/device.py"
    if not path.is_file():
        raise SystemExit("base/ is empty: run git submodule update --init")
    spec = importlib.util.spec_from_file_location("sample_base_device", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    module.BINARY = ROOT / "build/melodee"
    return module


if __name__ == "__main__":
    base_device().cli(default_app="melodee")
