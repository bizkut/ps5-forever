#!/usr/bin/env python3
"""Build production menu/voice admission and exit worker functions on the host."""
import argparse
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    spec = spec_from_file_location('surface_test', here / 'build-desktop-surface-test.py')
    module = module_from_spec(spec)
    spec.loader.exec_module(module)
    source_path = here / 'native-entry.c'
    source = source_path.read_text()
    start = source.index('enum forever_menu_lifetime\n')
    types = source[start:source.index('#endif', start)]
    start = source.index('static struct\n{\n    pthread_mutex_t lock, lifecycle;')
    end = source.index('.request = -1, .progress = -1};', start)
    types += source[start:end + len('.request = -1, .progress = -1};')] + '\n'
    args.output.parent.mkdir(parents=True, exist_ok=True)
    (args.output.parent / 'forever-exit-types.inc').write_text(types)
    functions = '\n\n'.join(module.definition(source_path, name) for name in (
        'forever_title_menu_begin_lifetime', 'forever_title_menu_finish_lifetime',
        'forever_title_voice_enable', 'pw_forever_menu_exit', 'forever_title_menu_action'))
    (args.output.parent / 'forever-exit-functions.inc').write_text(functions + '\n')
    command = shlex.split(args.cc) + [
        '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
        '-I' + str(args.output.parent), str(here / 'test-forever-exit.c'),
        '-o', str(args.output)]
    subprocess.run(command, check=True)


if __name__ == '__main__':
    main()
