#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Kerem Kilic (Ellic Studio)
"""The Profit-5 panel as a companion screen package (sdk/melodee-screen/melodee_companion.h).

Builds build/profit5-screen/package/Profit-5 Panel/ with screen-aarch64.so, screen.json and LICENSE: the folder to
copy to the card's Melodee/Plugin Screens/. Nothing of Profit-5 is compiled, linked or changed; the panel reads the
released plug-in's parameters through Melodee. Needs the melodee-builder image and build/profit5-screen/fonts.h
(build.py writes it from the upstream's OFL fonts).
"""
import argparse
from pathlib import Path
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = ROOT / 'build/profit5-screen'
PACKAGE = OUT / 'package/Profit-5 Panel'


def run(*args, **kwargs):
    subprocess.run([str(a) for a in args], check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inside', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if not (OUT / 'fonts.h').is_file():
        raise SystemExit('build/profit5-screen/fonts.h is missing: run build.py once')
    if not args.inside:
        run('docker', 'run', '--rm', '--platform', 'linux/arm64', '-v', f'{ROOT}:/work', '-w', '/work',
            'melodee-builder', 'python3', 'experiments/profit5-screen/build_companion.py', '--inside')
        print('Built companion package:', PACKAGE)
        return
    PACKAGE.mkdir(parents=True, exist_ok=True)
    library = PACKAGE / 'screen-aarch64.so'
    run('g++', '-std=c++17', '-O2', '-fPIC', '-fvisibility=hidden', '-shared', '-Wall', '-Wextra', '-Werror',
        '-Wl,--no-undefined', '-I' + str(HERE), '-I' + str(ROOT / 'sdk/melodee-screen'), '-I' + str(OUT),
        HERE / 'screen.cpp', HERE / 'companion.cpp', '-o', library)
    exported = subprocess.check_output(['nm', '-D', '--defined-only', str(library)], text=True)
    names = sorted(line.split()[-1] for line in exported.splitlines() if ' T ' in line)
    if names != ['melodee_companion_entry']:
        raise SystemExit(f'unexpected exports: {names}')
    shutil.copyfile(HERE / 'screen.json', PACKAGE / 'screen.json')
    licence = (HERE / 'LICENSE').read_text()
    font = (OUT / 'OFL-LiberationSans.txt').read_text() if (OUT / 'OFL-LiberationSans.txt').is_file() else ''
    (PACKAGE / 'LICENSE').write_text(
        'Profit-5 Panel, a Melodee companion screen.\n\n'
        'The screen code (screen.cpp, companion.cpp) is MIT licensed:\n\n' + licence +
        '\nIts text is Liberation Sans Bold, rasterised into the library, under the SIL Open Font License 1.1:\n\n' + font +
        '\nProfit-5 itself (sd88me, GPL-3.0-or-later) is not part of this package.\n')


if __name__ == '__main__':
    main()
