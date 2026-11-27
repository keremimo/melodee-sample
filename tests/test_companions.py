#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Kerem Kilic (Ellic Studio)
"""Companion screens (sdk/melodee-screen/melodee_companion.h) against a released VST2 and a package for it.

    python3 tests/test_companions.py PLUGIN.so PACKAGE_FOLDER

Runs build/melodee's companion tools in the melodee-builder image (linux/arm64): the package fits the plug-in;
wrong parameter names, a missing index, a later SDK and another plug-in are refused; K1-K3 follow every row of the
Profit-5 panel; pots, encoder edits and outside changes travel both ways; two instances keep apart; the plug-in's
sound and saved state are the same with the screen as without; a screen that crashes, exits, hangs, floods, sends
garbage, asks for a parameter it does not list or refuses the host is stopped while the plug-in plays on; no helper
process is left; the plug-in's file is unchanged.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'build/companion-test'
# The Profit-5 panel's rows (experiments/profit5-screen/screen.cpp): the parameters K1-K3 turn on each.
ROWS = [(2, 3, 10), (27, 28, 29), (6, 7, 8), (38, 39, 40), (41, 42, 11), (10, 11, 12), (13, 14, 15), (43, 57, 48),
        (16, 17, 18), (19, 44, 15), (20, 21, 22), (23, 44, 49), (0, 1, 26), (24, 25, 26), (4, 5, 56), (30, 31, 32),
        (33, 34, 37), (35, 36, 37), (46, 47, 55), (9, 45, 54), (50, 51, 52), (58, 53, 54)]
FIXTURE_MODES = ['ok', 'crash', 'exit', 'hang', 'flood', 'garbage', 'badedit', 'abi']


def tool(*args, env=None, timeout=120):
    return subprocess.run([str(ROOT / 'build/melodee'), *map(str, args)], capture_output=True, text=True, env=env, timeout=timeout)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def lines(output, key):
    return [line.split('=', 1)[1] for line in output.splitlines() if line.startswith(key + '=')]


def values(output):
    return {int(k): float(v) for k, v in (item.split(':') for item in lines(output, 'value'))}


def helpers_running():
    found = []
    for pid in os.listdir('/proc'):
        if pid.isdigit():
            try:
                if b'--melodee-companion-helper' in Path(f'/proc/{pid}/cmdline').read_bytes():
                    found.append(pid)
            except OSError:
                pass
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('plugin')
    parser.add_argument('package')
    parser.add_argument('--inside', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if not args.inside:
        rel = lambda p: str(Path(p).resolve().relative_to(ROOT))
        sys.exit(subprocess.run(['docker', 'run', '--rm', '--platform', 'linux/arm64', '-v', f'{ROOT}:/work', '-w', '/work',
                                 'melodee-builder', 'python3', 'tests/test_companions.py', '--inside', rel(args.plugin),
                                 rel(args.package)]).returncode)
    plugin, package = Path(args.plugin), Path(args.package)
    shutil.rmtree(WORK, ignore_errors=True)
    WORK.mkdir(parents=True)
    failures = []

    def check(name, ok, detail=''):
        print(('ok    ' if ok else 'FAIL  ') + name + ('' if ok or not detail else '\n      ' + detail.strip().replace('\n', '\n      ')))
        if not ok:
            failures.append(name)

    before = sha(plugin)
    r = tool('--melodee-companion-check', plugin, package)
    check('the package fits the released plug-in', r.returncode == 0 and 'fits=' in r.stdout, r.stdout)

    manifest = json.loads((package / 'screen.json').read_text())

    def refused(name, change, expect):
        folder = WORK / ('refused-' + name)
        shutil.copytree(package, folder)
        m = json.loads(json.dumps(manifest))
        change(m)
        (folder / 'screen.json').write_text(json.dumps(m))
        r = tool('--melodee-companion-check', plugin, folder)
        check('refused: ' + name, r.returncode == 1 and expect in r.stdout, r.stdout)

    refused('a wrong parameter name', lambda m: m['params'][13].update(name='Cutoff Frequency'), 'parameter 13 is "Cutoff"')
    refused('an index the plug-in lacks', lambda m: m['params'].append({'index': 400, 'name': 'Nothing'}), 'no parameter 400')
    refused('a later SDK', lambda m: m.update(sdk=2), 'needs companion SDK 2')
    refused('another plug-in', lambda m: m.update(plugin={'vst2': 'Xyzw'}), 'names another plug-in')
    refused('no library', lambda m: m.update(library='missing.so'), 'no missing.so')

    r = tool('--melodee-companion-screen', plugin, package, WORK / 'rows', *['plus'] * 22)
    seen = [tuple(map(int, c.split(','))) for c in lines(r.stdout, 'controls')]
    check('K1-K3 follow all 22 rows and wrap', r.returncode == 0 and seen == ROWS + [ROWS[0]], f'{r.returncode} {seen}\n{r.stderr[-400:]}')

    r = tool('--melodee-companion-screen', plugin, package, WORK / 'pots', 'k1:0', 'k2:64', 'k3:127')
    v = values(r.stdout)
    check('pots reach the parameters K1-K3 name', r.returncode == 0 and abs(v[2]) < 0.01 and abs(v[3] - 64 / 127) < 0.01 and abs(v[10] - 1) < 0.01, str(v))

    r = tool('--melodee-companion-screen', plugin, package, WORK / 'wheel', 'push', 'wheel:1', 'wheel:1', 'wheel:1', 'push')
    first = tool('--melodee-plugin-params', plugin).stdout
    start = float([l.split('\t') for l in first.splitlines() if l.startswith('2\t')][0][2])
    v = values(r.stdout)
    check('encoder edits come back as edits and add up', r.returncode == 0 and len(lines(r.stdout, 'edit')) == 3
          and abs(v[2] - (start + 3 / 60)) < 0.005, f'{start} -> {v.get(2)} {lines(r.stdout, "edit")}')

    r = tool('--melodee-companion-screen', plugin, package, WORK / 'outside', 'shift', 'plus', 'shift', 'param:13:900', 'program:5')
    shots = [(WORK / 'outside' / f'{n}.png').read_bytes() for n in range(6)] if r.returncode == 0 else []
    check('outside changes (a value, a program) reach the screen', len(shots) == 6 and shots[3] != shots[4] and shots[4] != shots[5],
          r.stdout[-300:])

    r = tool('--melodee-companion-pair', plugin, package)
    check('two instances keep their own edits and pages', r.returncode == 0 and 'pair=ok' in r.stdout, r.stdout)

    plain, shown = tool('--melodee-companion-render', plugin, WORK / 'plain'), tool('--melodee-companion-render', plugin, WORK / 'shown', package)
    same_audio = plain.returncode == 0 and shown.returncode == 0 and (WORK / 'plain.raw').read_bytes() == (WORK / 'shown.raw').read_bytes()
    same_state = same_audio and (WORK / 'plain.state').read_bytes() == (WORK / 'shown.state').read_bytes()
    check('the sound is the same with the screen as without', same_audio, plain.stdout + shown.stdout + shown.stderr[-300:])
    check('the saved state is the same with the screen as without', same_state)

    fixture = WORK / 'fixture'
    fixture.mkdir()
    subprocess.run(['g++', '-std=c++17', '-O2', '-fPIC', '-shared', '-Wall', '-Wextra', '-Werror', '-Isrc', '-Isdk/melodee-screen',
                    'tests/companion_fixture.cpp', '-o', str(fixture / 'fixture.so')], check=True, cwd=ROOT)
    m = json.loads(json.dumps(manifest))
    m.update(name='Fixture', library='fixture.so')
    (fixture / 'screen.json').write_text(json.dumps(m))
    for mode in FIXTURE_MODES:
        t0 = time.monotonic()
        r = tool('--melodee-companion-screen', plugin, fixture, WORK / ('fixture-' + mode), 'plus', 'plus', 'plus',
                 env=dict(os.environ, MELODEE_FIXTURE=mode), timeout=60)
        took = time.monotonic() - t0
        why = (lines(r.stdout, 'stopped') or [''])[0]
        if mode == 'ok':
            check('fixture ok: draws and stays', r.returncode == 0 and not why, r.stdout[-300:])
        else:
            check(f'fixture {mode}: stopped, plug-in plays on ({why or "-"}, {took:.1f} s)',
                  r.returncode == 2 and why != '' and 'audio=ok' in r.stdout and took < 10, r.stdout[-300:] + r.stderr[-300:])
    check('no helper process left', not helpers_running(), str(helpers_running()))
    check('the plug-in file is unchanged', sha(plugin) == before)
    print(f'{len(failures)} failed' if failures else 'companions ok')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
