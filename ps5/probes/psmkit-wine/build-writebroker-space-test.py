#!/usr/bin/env python3
"""Compile complete write-broker, ntdll pool/write and disk-space functions on the host."""
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


broker = Path(__file__).resolve().parents[2] / 'writebroker/writebroker.c'
client = args.wine / 'dlls/ntdll/unix/file.c'
server_names = ('transfer', 'discard', 'menu_remove_index', 'menu_remove_path_allowed',
                'menu_open_child_directory', 'menu_open_prefix_root', 'menu_open_remove_parent',
                'menu_remove_at', 'menu_remove_tree', 'serve_menu_remove', 'serve_write',
                'repair_open_data_root', 'repair_open_relative_directory',
                'repair_apply_mode', 'repair_scan_directory', 'repair_entry_at',
                'repair_root', 'repair_all',
                'broker_prepare_startup',
                'read_whole', 'install_icon', 'serve_statfs', 'serve')
client_names = ('bounce_acquire', 'bounce_release', 'broker_transfer', 'broker_connect',
                'broker_get_connection', 'broker_query_fs', 'broker_request', 'broker_pwrite',
                'get_full_size_info', 'get_full_size_info_ex')
args.output.parent.mkdir(parents=True, exist_ok=True)
for filename, path, names in (('writebroker-server.inc', broker, server_names),
                              ('writebroker-client.inc', client, client_names)):
    functions = '\n\n'.join(definition(path, name) for name in names) + '\n'
    if path == broker:
        source = path.read_text()
        repair_start = source.index('struct wb_permission_repair\n{')
        repair_end = source.index('static int write_token', repair_start)
        functions = source[repair_start:repair_end] + functions
    if path == client:
        source = path.read_text()
        bounce_start = source.index('#define BOUNCE_SIZE')
        bounce_end = source.index('/* a free bounce buffer', bounce_start)
        start = source.index('#define BROKER_CONNECTIONS')
        end = source.index('#define TITLE_WRITE_ROUTE_LOG_LIMIT', start)
        functions = source[bounce_start:bounce_end] + source[start:end] + functions
    (args.output.parent / filename).write_text(functions)
command = shlex.split(args.cc) + [
    '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
    '-I' + str(args.wine / 'include'), '-I' + str(args.output.parent),
    str(Path(__file__).with_name('test-writebroker-space.c')), '-o', str(args.output),
]
subprocess.run(command, check=True)
print('WRITEBROKER_SPACE_ISOLATION complete=' + ','.join(server_names + client_names)
      + ' implementation=PRODUCTION_EXTRACTED')
