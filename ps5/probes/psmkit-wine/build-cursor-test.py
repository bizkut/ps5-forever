#!/usr/bin/env python3
"""Compile real desktop and win32u title-GPU cursor callbacks with host GDI boundaries."""
import argparse
import importlib.util
from pathlib import Path
import shlex
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--wine', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--cc', default='cc')
args = parser.parse_args()
here = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('surface_test', here / 'build-desktop-surface-test.py')
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)
args.output.parent.mkdir(parents=True, exist_ok=True)
source = args.wine / 'dlls/win32u/driver.c'
(args.output.parent / 'cursor-title-driver.inc').write_text('\n\n'.join(
    helper.definition(source, name) for name in (
        'nulldrv_SetCursor', 'nulldrv_DestroyCursorIcon',
        'ps5_title_gpu_SetCursor', 'ps5_title_gpu_DestroyCursorIcon',
        'ps5_init_title_gpu_driver', 'ps5_load_headless_driver')) + '\n')
subprocess.run(shlex.split(args.cc) + [
    '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-pthread',
    '-I' + str(args.output.parent), '-I' + str(args.wine / 'include'),
    '-I' + str(args.wine / 'dlls/wineps5.drv'), str(here / 'test-cursor.c'),
    '-o', str(args.output)], check=True)
