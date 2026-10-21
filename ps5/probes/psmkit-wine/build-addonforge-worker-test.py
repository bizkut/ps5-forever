#!/usr/bin/env python3
"""Compile complete production AddonForge worker/refresh functions on the host."""
import argparse
from pathlib import Path
import re
import shlex
import subprocess

parser = argparse.ArgumentParser()
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


here = Path(__file__).resolve().parent
names = ('forever_addons_publish_error', 'forever_addons_progress_worker',
         'forever_addons_mutation_command', 'forever_addons_select_channel',
         'forever_addons_refresh', 'forever_addons_safe_key', 'forever_addons_mutate',
         'forever_addons_channel_visit', 'forever_addons_worker')
args.output.parent.mkdir(parents=True, exist_ok=True)
(args.output.parent / 'addonforge-worker.inc').write_text(
    '\n\n'.join(definition(here / 'native-entry.c', name) for name in names) + '\n')
subprocess.run(shlex.split(args.cc) + [
    '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
    '-I' + str(args.output.parent), str(here / 'test-addonforge-worker.c'),
    str(here / '../../title/addonforge-client.c'), str(here / '../../title/forever-menu.c'),
    '-o', str(args.output),
], check=True)
print('ADDONFORGE_WORKER_ISOLATION complete=' + ','.join(names)
      + ' implementation=PRODUCTION_EXTRACTED')
