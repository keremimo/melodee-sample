#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""Make a Melodee release zip: one autorun folder and nothing else, to unzip
or copy to the top of the MPC Sample's microSD card. The README, licences
and source note ride inside it, in autorun/melodee/.

At boot the stock app runs autorun/autorun.sh (signed in autorun.check, the
firmware's salted digest). It hands autorun/melodee/ to the base's SD boot
scripts (base/install/sd-boot.sh, sd-worker.sh): they check the stock
firmware and the files, copy Melodee into RAM, run its self-test and start
it in place of the stock app, which comes back on exit or any failure.

The zip is also an update file (base/docs/APPS.md, "Updates from the
card"): autorun/melodee/RELEASE.sha256 lists the SHA-256 of every other
file, and RELEASE.sha256.sig is its Ed25519 signature, made with the key at
$MELODEE_SIGNING_KEY (default ~/.config/melodee/release-signing.pem). The
signature is checked against the public key in src/melodee/shell/update.h,
which Melodee uses to verify updates.

Run ./release.sh VERSION, which builds, strips and then calls this.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from time import gmtime
import zipfile

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "base"
PACKAGE = "autorun/melodee"
SETUP_LOG = "/run/melodee-sd.log"
SOURCE_URL = "https://github.com/keremimo/melodee-sample"
SIGNING_KEY = Path(os.environ.get("MELODEE_SIGNING_KEY", Path.home() / ".config/melodee/release-signing.pem"))
LIST = f"{PACKAGE}/RELEASE.sha256"
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
    for name in ("restore-stock.sh", "sd-boot.sh", "sd-worker.sh", "sd-update.sh"):
        files[f"{PACKAGE}/{name}"] = (BASE / "install" / name).read_bytes()
    files[f"{PACKAGE}/SHA256SUMS"] = sums(files, PACKAGE + "/").encode()
    files[f"{PACKAGE}/app"] = b"melodee\n"
    files[f"{PACKAGE}/VERSION"] = f"{version} {commit}\n".encode()
    script, manifest = autorun_script(version), autorun_manifest()
    files["autorun/autorun.sh"] = script.encode()
    files["autorun/autorun.json"] = manifest.encode()
    files["autorun/autorun.check"] = device.autorun_check(manifest, script).encode()
    # Beside the files sd-worker.sh checks, so the zip unpacks to the one autorun folder.
    files[f"{PACKAGE}/README.txt"] = readme(version, commit)
    licenses = f"{PACKAGE}/licenses"
    files[f"{licenses}/LICENSE-AGPL-3.0.txt"] = (ROOT / "LICENSE").read_bytes()
    files[f"{licenses}/THIRD_PARTY.md"] = (ROOT / "THIRD_PARTY.md").read_bytes()
    files[f"{licenses}/Rubik-OFL.txt"] = (ROOT / "build/deps/rubik/OFL.txt").read_bytes()
    files[f"{licenses}/JUCE-LICENSE.md"] = (ROOT / "build/deps/JUCE/LICENSE.md").read_bytes()
    # Plug-ins (docs/MELODEE.md): the VST3 SDK inside JUCE, the CLAP headers, and the armhf runtime inside the binary's support pack.
    files[f"{licenses}/VST3-SDK-LICENSE.txt"] = (ROOT / "build/deps/JUCE/modules/juce_audio_processors_headless/format_types/VST3_SDK/LICENSE.txt").read_bytes()
    files[f"{licenses}/CLAP-LICENSE.txt"] = (ROOT / "build/deps/clap/LICENSE").read_bytes()
    for path in sorted((ROOT / "build/plugin-support/licenses").glob("*")):
        files[f"{licenses}/armhf-runtime/{path.name}"] = path.read_bytes()
    assert all(name.startswith("autorun/") for name in files), "the zip is the autorun folder alone"
    return files


def release_list(files):
    """RELEASE.sha256: the SHA-256 of every file in the zip except the list
    and its signature, sorted by path."""
    return "".join(hashlib.sha256(files[name]).hexdigest() + "  " + name + "\n"
                   for name in sorted(files) if name not in (LIST, LIST + ".sig")).encode()


def public_key():
    """The public key from src/melodee/shell/update.h, as PEM text."""
    text = (ROOT / "src/melodee/shell/update.h").read_text()
    lines = re.findall(r'^\s*"([^"\n]*)\\n"', text, re.MULTILINE)
    begin, end = lines.index("-----BEGIN PUBLIC KEY-----"), lines.index("-----END PUBLIC KEY-----")
    return "\n".join(lines[begin:end + 1]) + "\n"


def openssl():
    """Path of an OpenSSL 3 binary (needed for Ed25519 with -rawin; macOS LibreSSL lacks it)."""
    for tool in (os.environ.get("OPENSSL"), "/opt/homebrew/opt/openssl@3/bin/openssl", "/usr/local/opt/openssl@3/bin/openssl", "openssl"):
        if not tool:
            continue
        try:
            version = subprocess.run([tool, "version"], capture_output=True, text=True).stdout
        except OSError:
            continue
        if version.startswith("OpenSSL 3"):
            return tool
    raise RuntimeError("Signing needs OpenSSL 3 (brew install openssl@3, or set OPENSSL)")


def sign(files, key=SIGNING_KEY):
    """Adds RELEASE.sha256 and its signature to `files`. Fails if the
    signature does not verify with the public key in update.h, because
    Melodee would reject that release as an update."""
    if not Path(key).is_file():
        raise RuntimeError(f"No signing key at {key}. Create one with\n"
                           f"  openssl genpkey -algorithm ed25519 -out {key}\n"
                           "and put its public key (openssl pkey -in KEY -pubout) in src/melodee/shell/update.h.\n"
                           "Back up the key: installed versions of Melodee only accept updates signed with it.")
    tool = openssl()
    listing = release_list(files)
    with tempfile.TemporaryDirectory() as temp:
        temp = Path(temp)
        (temp / "list").write_bytes(listing)
        (temp / "public.pem").write_text(public_key())
        subprocess.run([tool, "pkeyutl", "-sign", "-inkey", str(key), "-rawin",
                        "-in", str(temp / "list"), "-out", str(temp / "list.sig")], check=True)
        checked = subprocess.run([tool, "pkeyutl", "-verify", "-pubin", "-inkey", str(temp / "public.pem"), "-rawin",
                                  "-in", str(temp / "list"), "-sigfile", str(temp / "list.sig")], capture_output=True)
        if checked.returncode:
            raise RuntimeError(f"{key} does not match the public key in src/melodee/shell/update.h")
        files[LIST] = listing
        files[LIST + ".sig"] = (temp / "list.sig").read_bytes()
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
    sign(files)
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
