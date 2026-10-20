#!/usr/bin/env python3
"""Exercise production ntdll exit and TLS-unregister functions with real host pthreads."""
import argparse
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--wine', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    spec = spec_from_file_location('surface_test', here / 'build-desktop-surface-test.py')
    module = module_from_spec(spec)
    spec.loader.exec_module(module)
    server = args.wine / 'dlls/ntdll/unix/server.c'
    args.output.parent.mkdir(parents=True, exist_ok=True)
    functions = '\n\n'.join(module.definition(server, name) for name in (
        'unregister_title_thread', 'ps5_title_request_exit'))
    (args.output.parent / 'wine-title-exit.inc').write_text(functions + '\n')
    command = shlex.split(args.cc) + [
        '-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
        '-I' + str(args.wine / 'include'), '-I' + str(args.output.parent),
        str(args.wine / 'loader/tests/ps5_title_exit.c'), '-o', str(args.output),
    ]
    subprocess.run(command, check=True)
    print('TITLE_EXIT_ISOLATION complete=ps5_title_request_exit,unregister_title_thread '
          'implementation=PRODUCTION_EXTRACTED')


if __name__ == '__main__':
    main()
