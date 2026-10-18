#!/usr/bin/env python3
"""Compile the complete production PS5 multi-run write buffer on the host."""
import argparse
from pathlib import Path
import shlex
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--wine', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--cc', default='cc')
args = parser.parse_args()

source = (args.wine / 'dlls/ntdll/unix/file.c').read_text()
begin_marker = '/* WINE_PS5_MULTI_BUFFER_TEST_BEGIN */'
end_marker = '/* WINE_PS5_MULTI_BUFFER_TEST_END */'
if source.count(begin_marker) != 1 or source.count(end_marker) != 1:
    raise SystemExit('multi-buffer production extraction markers missing or ambiguous')
dispatch = 'title_buffer_fence_nonfile( handle, type == FD_TYPE_FILE )'
if source.count(dispatch) != 1:
    raise SystemExit('NtWriteFile nonregular production fence missing or ambiguous')
prototype = (
    'static NTSTATUS title_buffer_flush_reason( HANDLE caller, int consume_error,\n'
    '                                           int native_worker, unsigned int reason );'
)
if source.count(prototype) != 1 or source.index(prototype) > source.index('NTSTATUS open_unix_file('):
    raise SystemExit('early title_buffer_flush_reason prototype missing or ambiguous')
server_source = (args.wine / 'dlls/ntdll/unix/server.c').read_text()
if server_source.count('if (source_process == NtCurrentProcess())') != 1:
    raise SystemExit('NtDuplicateObject pseudo-current classification missing or ambiguous')
if server_source.count('ps5_title_flush_all_coalesced_writes_reason(') != 1:
    raise SystemExit('NtDuplicateObject real-process global handoff fallback missing or ambiguous')
begin = source.index(begin_marker) + len(begin_marker)
end = source.index(end_marker, begin)
args.output.parent.mkdir(parents=True, exist_ok=True)
include = args.output.parent / 'wine-write-buffer.inc'
include.write_text(source[begin:end])
command = shlex.split(args.cc) + [
    '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Wno-unused-function', '-pthread',
    '-DWINE_PS5_WRITE_BUFFER_TEST=1', '-DWINE_PS5_WRITE_BUFFER_DIAG=1',
    '-I' + str(args.wine / 'include'), '-I' + str(args.output.parent),
    str(Path(__file__).with_name('test-write-buffer.c')), '-o', str(args.output),
]
subprocess.run(command, check=True)
print('WRITE_BUFFER_ISOLATION implementation=PRODUCTION_EXTRACTED')
