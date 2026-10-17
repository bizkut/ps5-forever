#!/usr/bin/env python3
"""Compile complete surface lifecycle/flush functions verbatim with the compositor.

Only Wine/GDI and title callback boundaries are substituted by the host harness;
shared memory creation, mappings, row copies, damage and composition are real.
"""
import argparse
from pathlib import Path
import re
import shlex
import subprocess



def definition(path, name, structure=False, expected=1):
    source = path.read_text()
    tokens = list(re.finditer(r'/\*[\s\S]*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|'
                              r"'(?:\\.|[^'\\])*'|[A-Za-z_][A-Za-z_0-9]*|[^\s]", source))
    tokens = [t for t in tokens if not t.group().startswith(('/*', '//'))]
    matches = []
    for index, token in enumerate(tokens):
        if token.group() != name:
            continue
        close = index + 1
        if structure:
            if tokens[index - 1].group() != 'struct' or tokens[close].group() != '{':
                continue
        else:
            if tokens[close].group() != '(':
                continue
            depth = 0
            while close < len(tokens):
                value = tokens[close].group()
                depth += (value == '(') - (value == ')')
                if not depth:
                    break
                close += 1
            close += 1
            if tokens[close].group() != '{':
                continue
        start = source.rfind('\n', 0, token.start()) + 1
        depth = 0
        for end in range(close, len(tokens)):
            value = tokens[end].group()
            depth += (value == '{') - (value == '}')
            if not depth:
                last = tokens[end + 1] if structure else tokens[end]
                matches.append(source[start:last.end()])
                break
    if len(matches) != expected:
        raise SystemExit(f'expected {expected} complete {name} definitions in {path}, got {len(matches)}')
    # runtime-parent's second definition is the GPU profile's ENOSYS stub.
    return matches[0]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--wine', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    window = args.wine / 'dlls/wineps5.drv/window.c'
    args.output.parent.mkdir(parents=True, exist_ok=True)
    (args.output.parent / 'desktop-surface-types.inc').write_text('\n\n'.join(
        definition(window, name, structure=True)
        for name in ('ps5_window_surface', 'ps5_foreign_surface')) + '\n')
    functions = '\n\n'.join(definition(window, name) for name in (
        'intersect_rect', 'unlink_surface', 'ps5_surface_destroy', 'create_surface',
        'ps5_surface_flush', 'foreign_surface_destroy', 'open_foreign_surface',
        'foreign_surface_flush'))
    (args.output.parent / 'desktop-surface-functions.inc').write_text(functions + '\n')
    callbacks = definition(here / 'runtime-parent.c', 'surface_name_valid') + '\n\n'
    callbacks += definition(here / 'runtime-parent.c', 'pw_desktop_surface', expected=2) + '\n\n'
    callbacks += definition(args.wine / 'dlls/ntdll/unix/server.c', '__wine_ps5_desktop_surface') + '\n'
    (args.output.parent / 'desktop-surface-callbacks.inc').write_text(callbacks)
    command = shlex.split(args.cc) + ['-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
                                    '-Wno-unused-parameter', '-Wno-sign-compare', '-pthread', '-D__PROSPERO__',
                                    '-I' + str(args.output.parent), '-I' + str(args.wine / 'include'),
                                    str(here / 'test-desktop-surface.c'), '-o', str(args.output)]
    subprocess.run(command, check=True)
    print('DESKTOP_SURFACE_ISOLATION lifecycle=owner,remote flush=owner,remote callbacks=ntdll,parent implementation=UNCHANGED')


if __name__ == '__main__':
    main()
