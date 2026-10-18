#!/usr/bin/env python3
"""Build the real supervisor admission gate and Add-ons yield callback on the host."""
import argparse
from pathlib import Path
import shlex
import subprocess

from importlib.util import module_from_spec, spec_from_file_location


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    spec = spec_from_file_location('surface_test', here / 'build-desktop-surface-test.py')
    module = module_from_spec(spec)
    spec.loader.exec_module(module)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    functions = '\n\n'.join(module.definition(here / '../../title/main.c', name) for name in (
        'title_desktop_addons_snapshot', 'title_desktop_addons_error',
        'title_desktop_addons_pause')) + '\n\n'
    functions += module.definition(here / 'native-entry.c', 'forever_title_addons_yield') + '\n\n'
    functions += '\n\n'.join(module.definition(here / 'runtime-parent.c', name) for name in (
        'occupied_slots', 'is_chromium_subprocess', 'spawn_command', 'is_addonforge_core',
        'is_battlenet_renderer', 'reserve_spawn_slot'))
    (args.output.parent / 'supervisor-admission.inc').write_text(functions + '\n')
    command = shlex.split(args.cc) + [
        '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
        '-I' + str(args.output.parent), str(here / 'test-supervisor-admission.c'),
        str(here / 'protocol.c'), str(here / '../../title/forever-menu.c'),
        str(here / '../localproc/psmkit-io.c'),
        '-o', str(args.output)]
    subprocess.run(command, check=True)


if __name__ == '__main__':
    main()
