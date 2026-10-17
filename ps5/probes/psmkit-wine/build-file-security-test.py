#!/usr/bin/env python3
"""Compile the real mode/SD conversion and SID tables for host and PS5 paths."""
import argparse
import importlib.util
from pathlib import Path
import re
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--wine', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    spec = importlib.util.spec_from_file_location('surface_extraction', here / 'build-desktop-surface-test.py')
    extraction = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(extraction)
    definition = extraction.definition
    server = args.wine / 'server'
    protocol = args.wine / 'include/wine/server_protocol.h'
    types = '\n\n'.join(definition(protocol, name, structure=True) for name in
                        ('sid', 'acl', 'security_descriptor', 'generic_map'))
    types += '\n\n' + definition(server / 'security.h', 'ace', structure=True)
    helpers = '\n\n'.join(definition(server / 'security.h', name) for name in
                          ('sid_len', 'equal_sid', 'ace_next', 'set_ace', 'sd_get_dacl'))
    sids = re.findall(r'^(?:static |const |[ \t])*struct sid \w+ = [^\n]+;',
                      (server / 'token.c').read_text(), re.MULTILINE)
    if not sids:
        raise SystemExit('cannot isolate server SID table')
    functions = '\n\n'.join(definition(server / 'file.c', name) for name in
                            ('mode_to_sd', 'file_access_to_mode', 'sd_to_mode'))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    (args.output.parent / 'file-security-types.inc').write_text(types + '\n' + helpers + '\n' + '\n'.join(sids) + '\n')
    (args.output.parent / 'file-security-functions.inc').write_text(functions + '\n')
    subprocess.run(shlex.split(args.cc) + [
        '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-const-variable',
        '-I' + str(args.wine / 'include'), '-I' + str(args.output.parent),
        str(here / 'test-file-security.c'), '-o', str(args.output),
    ], check=True)
    print('FILE_SECURITY_ISOLATION mode_to_sd,sd_to_mode,SIDs implementation=UNCHANGED')


if __name__ == '__main__':
    main()
