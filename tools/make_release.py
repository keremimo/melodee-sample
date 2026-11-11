#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""Make a Melodee release zip: an autorun folder to copy to the root of the
MPC Sample's microSD card, plus a README, the licences and the source note.

At boot the stock app runs autorun/autorun.sh (signed in autorun.check, the
firmware's salted digest). It hands autorun/melodee/ to the base's SD boot
scripts (base/install/sd-boot.sh, sd-worker.sh): they check the stock
firmware and the files, copy Melodee into RAM, run its self-test and start
it in place of the stock app, which comes back on exit or any failure.

Run ./release.sh VERSION, which builds, strips and then calls this.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys
from time import gmtime
import zipfile

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "base"
PACKAGE = "autorun/melodee"
SETUP_LOG = "/run/melodee-sd.log"
SOURCE_URL = "https://github.com/keremimo/melodee-sample"
VERSION = re.compile(r"v\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?")
# Nothing personal or private may ship: home or temp paths, keys, tokens.
FORBIDDEN = (re.compile(rb"/Use" rb"rs/|/ho" rb"me/[a-z]|/private/tmp/|/var/folders/"),
             re.compile(rb"PRIVATE KEY|PuTTY-User-Key"),
             re.compile(rb"(ssh-(ed25519|rsa|dss)|ecdsa-sha2-nistp\d+) AAAA"),
             re.compile(rb"authorized_keys|known_hosts|id_ed25519|id_rsa|id_ecdsa|mpc_sample_client"),
             re.compile(rb"ghp_[A-Za-z0-9]{20}|github_pat_|gho_[A-Za-z0-9]{20}|AKIA[0-9A-Z]{16}|sk-ant-"))


def base_device():
    spec = importlib.util.spec_from_file_location("sample_base_device", BASE / "device.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def autorun_script(version):
    return ("#!/bin/sh\n"
            f"# Melodee {version} for the MPC Sample: starts Melodee from autorun/melodee\n"
            "# once the stock app is up. Delete this autorun folder to uninstall.\n"
            'D=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\n'
            f'/bin/sh "$D/melodee/sd-boot.sh" "$D/melodee" > {SETUP_LOG} 2>&1 || true\n')


def autorun_manifest():
    script = {"path": "autorun.sh", "askQuestion": False, "message": "Start Melodee?",
              "yesButtonText": "Start", "noButtonText": "Cancel"}
    return json.dumps({"value0": {"scripts": [script], "files": []}}, indent=2) + "\n"


def sums(files, prefix):
    return "".join(hashlib.sha256(data).hexdigest() + "  " + name[len(prefix):] + "\n"
                   for name, data in sorted(files.items()) if name.startswith(prefix) and name.count("/") == prefix.count("/"))


def readme(version, commit):
    text = (ROOT / "release/README.txt").read_text()
    return text.replace("{version}", version).replace("{commit}", commit).replace("{source}", SOURCE_URL).encode()


def collect(binary, version, commit):
    """Every file of the zip, as {path in zip: bytes}."""
    device = base_device()
    if not binary.is_file():
        raise RuntimeError(f"Missing {binary}; run ./release.sh")
    files = {f"{PACKAGE}/sample-base": binary.read_bytes()}
    for name in ("restore-stock.sh", "sd-boot.sh", "sd-worker.sh"):
        files[f"{PACKAGE}/{name}"] = (BASE / "install" / name).read_bytes()
    files[f"{PACKAGE}/SHA256SUMS"] = sums(files, PACKAGE + "/").encode()
    files[f"{PACKAGE}/app"] = b"melodee\n"
    files[f"{PACKAGE}/VERSION"] = f"{version} {commit}\n".encode()
    script, manifest = autorun_script(version), autorun_manifest()
    files["autorun/autorun.sh"] = script.encode()
    files["autorun/autorun.json"] = manifest.encode()
    files["autorun/autorun.check"] = device.autorun_check(manifest, script).encode()
    files["README.txt"] = readme(version, commit)
    files["licenses/LICENSE-AGPL-3.0.txt"] = (ROOT / "LICENSE").read_bytes()
    files["licenses/THIRD_PARTY.md"] = (ROOT / "THIRD_PARTY.md").read_bytes()
    files["licenses/Rubik-OFL.txt"] = (ROOT / "build/deps/rubik/OFL.txt").read_bytes()
    files["licenses/JUCE-LICENSE.md"] = (ROOT / "build/deps/JUCE/LICENSE.md").read_bytes()
    # Plug-ins (docs/MELODEE.md): the VST3 SDK inside JUCE, and the armhf runtime inside the binary's support pack.
    files["licenses/VST3-SDK-LICENSE.txt"] = (ROOT / "build/deps/JUCE/modules/juce_audio_processors_headless/format_types/VST3_SDK/LICENSE.txt").read_bytes()
    for path in sorted((ROOT / "build/plugin-support/licenses").glob("*")):
        files[f"licenses/armhf-runtime/{path.name}"] = path.read_bytes()
    return files


def scan(name, data):
    for pattern in FORBIDDEN:
        if pattern.search(data):
            raise RuntimeError(f"{name} contains {pattern.pattern.decode()!r}; refusing to package it")


def write_zip(files, output, timestamp):
    """A reproducible zip: sorted entries, one timestamp, fixed permissions."""
    date = gmtime(max(timestamp, 315532800 + 86400))[:6]  # zip dates start in 1980
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name in sorted(files):
            info = zipfile.ZipInfo(name, date)
            info.compress_type = zipfile.ZIP_DEFLATED
            mode = 0o755 if name.endswith(".sh") or name.endswith("/sample-base") else 0o644
            info.external_attr = (0o100000 | mode) << 16
            info.create_system = 3
            archive.writestr(info, files[name])


def git(*args):
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, check=True).stdout.strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("version", help="release version, such as v0.1.0-alpha.1")
    parser.add_argument("--binary", default=str(ROOT / "build/melodee.release"), help="stripped build to ship")
    parser.add_argument("--allow-dirty", action="store_true", help="package uncommitted changes (not for sending)")
    arguments = parser.parse_args()
    if not VERSION.fullmatch(arguments.version):
        raise SystemExit("Version must look like v0.1.0 or v0.1.0-alpha.1")
    if git("status", "--porcelain") and not arguments.allow_dirty:
        raise SystemExit("Commit first: a release names the commit its source is at")
    commit = git("rev-parse", "--short=12", "HEAD")
    files = collect(Path(arguments.binary), arguments.version, commit)
    for name, data in files.items():
        scan(name, data)
    output = ROOT / "dist" / f"Melodee-MPC-Sample-{arguments.version}.zip"
    write_zip(files, output, int(git("log", "-1", "--format=%ct")))
    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    output.with_name(output.name + ".sha256").write_text(f"{digest}  {output.name}\n")
    print(f"Wrote {output} ({output.stat().st_size // 1024} KiB) at {commit}")
    print(f"SHA256: {digest}")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, ValueError, OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error))
