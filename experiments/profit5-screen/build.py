#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Kerem Kilic (Ellic Studio)
"""Isolated Profit-5 + Melodee screen build. Downloads and products stay in build/."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = ROOT / 'build/profit5-screen'
PROFIT = '8d4857585ce5f8b246d0c203891bc0b970c48cfa'
TOOLS = 'd2daf62ab8ca9690a747f5278567848a15ac31cb'


def run(*args, **kwargs):
    subprocess.run([str(a) for a in args], check=True, **kwargs)


def checkout(name, url, revision, local=None):
    path = OUT / name
    if not (path / '.git').exists():
        run('git', 'clone', str(local) if local and local.is_dir() else url, path)
    if subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip() != revision:
        run('git', '-C', path, 'checkout', '--detach', revision)
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inside', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    if not args.inside:
        checkout('upstream', 'https://github.com/sd88me/mpc-vst-profit5.git', PROFIT)
        checkout('toolchain', 'https://github.com/open-mpc/mpc-vst-plugins.git', TOOLS, ROOT.parent / 'mpc-vst-plugins')
        python = OUT / 'venv/bin/python'
        if not python.exists():
            run(sys.executable, '-m', 'venv', OUT / 'venv')
            run(python, '-m', 'pip', 'install', 'Pillow==12.0.0')
        run(python, HERE / 'fonts.py', OUT / 'upstream/skin/fonts', OUT / 'fonts.h')
        run('docker', 'run', '--rm', '--platform', 'linux/arm64', '-v', f'{ROOT}:/work', '-w', '/work',
            'melodee-builder', 'python3', 'experiments/profit5-screen/build.py', '--inside')
        run(python, HERE / 'preview.py', OUT)
        return
    upstream, toolchain = OUT / 'upstream', OUT / 'toolchain'
    # Generate the frozen upstream VST table with the upstream generator; no parameter reorder or identity change.
    run(sys.executable, toolchain / 'tools/gen_vst.py', upstream / 'vst/vst.json', '--params-h')
    common = ['-O2', '-fPIC', '-fvisibility=hidden', '-I' + str(HERE), '-I' + str(ROOT / 'sdk/melodee-screen'),
              '-I' + str(OUT), '-I' + str(upstream / 'vst/build'), '-I' + str(upstream / 'src'),
              '-I' + str(toolchain / 'wrapper')]
    objects = []
    for source in [upstream / 'src/plugin.c', upstream / 'src/engine.c', upstream / 'src/p5_import.c', HERE / 'bridge.c']:
        obj = OUT / (source.stem + '.o')
        run('gcc', '-std=c11', *common, '-D_GNU_SOURCE', '-DHAS_DISPLAY_REV=1', '-DDISPLAY_REV_NO_UPDATE=1',
            '-DMODULE_SUBDIR="banks"', '-c', source, '-o', obj)
        objects.append(obj)
    run('g++', '-std=c++17', *common, '-Wall', '-Wextra', '-Werror', '-c', HERE / 'screen.cpp', '-o', OUT / 'screen.o')
    plugin = OUT / 'profit5-melodee.so'
    run('g++', '-shared', *objects, OUT / 'screen.o', '-lm', '-ldl', '-pthread', '-o', plugin)
    run('g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-Isrc', '-Isdk/melodee-screen',
        HERE / 'test.cpp', '-ldl', '-o', OUT / 'test-screen')
    run(OUT / 'test-screen', plugin)
    preview = OUT / 'preview'
    # Host loading/input smoke test. See README: this host tool selects generic EDIT, not PANEL.
    steps = ['k1:25', 'k2:83', 'k3:110', 'shift', 'plus', 'shift', 'k1:79', 'k2:40', 'k3:75',
             'shift', 'plus', 'shift', 'k1:20', 'k2:65', 'k3:85', 'shift', 'plus', 'plus', 'plus', 'shift']
    run(ROOT / 'build/melodee', '--melodee-plugin-screen', plugin, preview, *steps)
    manifest = {'upstream': PROFIT, 'toolchain': TOOLS, 'sdk': 'melodee.screen/1',
                'plugin': str(plugin), 'scope': 'Local experiment. Do not merge or publish.'}
    (OUT / 'build.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('Built local experiment:', plugin)


if __name__ == '__main__':
    main()
