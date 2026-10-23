#!/usr/bin/env python3
"""Build production worker authentication/probe and socket transfer on the host."""
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
    args.output.parent.mkdir(parents=True, exist_ok=True)
    source = here / 'native-entry.c'
    (args.output.parent / 'addon-worker-transport.inc').write_text(
        module.definition(source, 'pw_writebroker_transfer') + '\n')
    functions = module.definition(source, 'pw_addon_worker_start_context', structure=True)
    functions += '\n\n' + '\n\n'.join(module.definition(source, name) for name in (
        'pw_addon_worker_get_u32', 'pw_addon_worker_put_u32', 'pw_addon_worker_auth_tag',
        'pw_addon_worker_secure_equal', 'pw_addon_worker_request_until', 'pw_addon_worker_probe'))
    (args.output.parent / 'addon-worker-request.inc').write_text(functions + '\n')
    subprocess.run(shlex.split(args.cc) + [
        '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
        '-I' + str(args.output.parent), str(here / 'test-addon-worker-request.c'),
        '-o', str(args.output),
    ], check=True)


if __name__ == '__main__':
    main()
