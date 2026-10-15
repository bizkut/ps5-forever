#!/usr/bin/env python3
"""Isolate complete Wine routing functions verbatim; compile their host contract.

A C token scan only finds definition boundaries (ignoring comments/literals).
No production statements are rewritten, no source-text policy assertions are used.
The harness substitutes unrelated environment/OS dependencies and observes the
real functions' loader selection, image rejection, unmapping and proxy consumer.
"""
import argparse
from pathlib import Path
import re
import shlex
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--wine', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--cc', default='cc')
parser.add_argument('--non-ps5', action='store_true')
args = parser.parse_args()


def definition(path, name):
    source = path.read_text()
    # Keep token offsets in the original source. Whitespace and comments cannot
    # affect brace nesting; quoted braces are a single token.
    tokens = list(re.finditer(r'/\*[\s\S]*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|'
                              r"'(?:\\.|[^'\\])*'|[A-Za-z_][A-Za-z_0-9]*|[^\s]", source))
    tokens = [t for t in tokens if not t.group().startswith(('/*', '//'))]
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


unix = args.wine / 'dlls/ntdll/unix'
functions = definition(unix / 'loader.c', 'get_alternate_wineloader') + '\n\n'
functions += definition(unix / 'env.c', 'build_initial_params') + '\n'
if not args.non_ps5:
    functions += definition(unix / 'process.c', 'ps5_winebrowser_url') + '\n'
args.output.parent.mkdir(parents=True, exist_ok=True)
unit = args.output.parent / 'startup-routing-functions.inc'
unit.write_text(functions)
command = shlex.split(args.cc) + ['-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                                '-Wno-unused-function', '-I' + str(args.output.parent)]
if not args.non_ps5:
    command += ['-D__PROSPERO__']
command += [str(Path(__file__).with_name('test-startup-routing.c')), '-o', str(args.output)]
subprocess.run(command, check=True)
print('STARTUP_ROUTING_ISOLATION complete=get_alternate_wineloader,build_initial_params' +
      (',ps5_winebrowser_url' if not args.non_ps5 else '') +
      ' implementation=UNCHANGED platform=' + ('host' if args.non_ps5 else 'PS5'))
