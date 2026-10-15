#!/usr/bin/env python3
"""Compile the actual SDK realpath function, isolated by Clang AST on macOS.

The rest of SDK libc.c contains unrelated FreeBSD/Linux-only declarations;
no independently maintained algorithm or host compatibility implementation is introduced.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--platform', type=Path, required=True)
parser.add_argument('--wine', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--cc', default='cc')
args = parser.parse_args()
source = args.platform / 'src/libc.c'
compiler = os.environ.get('AST_CC', 'clang')
query = subprocess.run([compiler, '-std=gnu11', '-fsyntax-only', '-I' + str(args.platform / 'include'),
                        '-I' + str(args.platform / 'src'), '-Xclang', '-ast-dump=json',
                        '-Xclang', '-ast-dump-filter=ps5_realpath', str(source)],
                       text=True, capture_output=True)
# Clang recovery AST isolates this well-typed function despite unrelated
# platform declarations; the extracted definition must then compile strictly.
decoder = json.JSONDecoder()
text = query.stdout
nodes = []
while text.strip():
    text = text.lstrip()
    node, end = decoder.raw_decode(text)
    text = text[end:]
    if node.get('kind') == 'FunctionDecl' and node.get('name') == 'ps5_realpath' and any(
            child.get('kind') == 'CompoundStmt' for child in node.get('inner', [])):
        nodes.append(node)
if len(nodes) != 1:
    raise SystemExit('cannot structurally isolate SDK ps5_realpath: ' + query.stderr)
node = nodes[0]
start = node['range']['begin']['offset']
end = node['range']['end']['offset'] + node['range']['end']['tokLen']
body = source.read_bytes()[start:end].decode()
args.output.parent.mkdir(parents=True, exist_ok=True)
unit = args.output.with_suffix('.sdk.c')
unit.write_text('#define _GNU_SOURCE 1\n#include <errno.h>\n#include <limits.h>\n'
                '#include <stdbool.h>\n#include <stdlib.h>\n#include <string.h>\n'
                '#include <sys/stat.h>\n#include <unistd.h>\n'
                '#define ps5_getcwd getcwd /* Host cwd adapter; regression uses absolute paths. */\n'
                + body + '\n')
command = shlex.split(args.cc) + ['-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                                '-I' + str(args.platform / 'include'), '-I' + str(args.wine / 'loader'),
                                'test-loader-path.c', str(unit), '-o', str(args.output)]
subprocess.run(command, check=True)
print('SDK_REALPATH_AST_ISOLATION function=ps5_realpath implementation=UNCHANGED public_stat=REAL_HOST')
