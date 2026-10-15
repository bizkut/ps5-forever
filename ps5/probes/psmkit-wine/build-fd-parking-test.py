#!/usr/bin/env python3
"""Compile the PS5 fd-group parking functions verbatim in a host harness."""
import argparse
from pathlib import Path
import re
import shlex
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--wine', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--cc', default='cc')
args = parser.parse_args()


def definition(path, name):
    source = path.read_text()
    tokens = list(re.finditer(r'/\*[\s\S]*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|'
                              r"'(?:\\.|[^'\\])*'|[A-Za-z_][A-Za-z_0-9]*|[^\s]", source))
    tokens = [token for token in tokens if not token.group().startswith(('/*', '//'))]
    matches = []
    for index, token in enumerate(tokens):
        if token.group() != name or tokens[index + 1].group() != '(':
            continue
        close = index + 1
        depth = 0
        while close < len(tokens):
            value = tokens[close].group()
            depth += (value == '(') - (value == ')')
            if not depth:
                break
            close += 1
        if tokens[close + 1].group() != '{':
            continue
        start = source.rfind('\n', 0, token.start()) + 1
        end = close + 1
        depth = 0
        while end < len(tokens):
            value = tokens[end].group()
            depth += (value == '{') - (value == '}')
            if not depth:
                matches.append(source[start:tokens[end].end()])
                break
            end += 1
    if len(matches) != 1:
        raise SystemExit(f'cannot isolate exactly one complete {name} definition in {path}')
    return matches[0]


source = args.wine / 'server/fd.c'
names = (
    'new_desc_id', 'park_now', 'same_park_group', 'park_group_leader',
    'fd_park_member_reason', 'fd_park_reason', 'fd_can_park', 'park_fd',
    'park_idle', 'reopen_parked', 'unpark_fd', 'touch_fd', 'park_forget_fd',
)
functions = '\n\n'.join(definition(source, name) for name in names) + '\n'
args.output.parent.mkdir(parents=True, exist_ok=True)
unit = args.output.parent / 'fd-parking-functions.inc'
unit.write_text(functions)
command = shlex.split(args.cc) + [
    '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
    '-I' + str(args.wine / 'include'), '-I' + str(args.output.parent),
    str(Path(__file__).with_name('test-fd-parking.c')), '-o', str(args.output),
]
subprocess.run(command, check=True)
print('FD_PARKING_ISOLATION complete=' + ','.join(names) + ' implementation=UNCHANGED')
