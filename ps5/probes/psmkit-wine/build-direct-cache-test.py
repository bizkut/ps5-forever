#!/usr/bin/env python3
"""Compile the production private direct-region cache and delete-view cutover."""
import argparse
from pathlib import Path
import shlex
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--wine', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--cc', default='cc')
parser.add_argument('--cpu-test', action='store_true')
parser.add_argument('--fail-fast-test', action='store_true')
args = parser.parse_args()
source = (args.wine / 'dlls/ntdll/unix/virtual.c').read_text()
start = '/* WINE_PS5_DIRECT_CACHE_TEST_BEGIN */'
end = '/* WINE_PS5_DIRECT_CACHE_TEST_END */'
if source.count(start) != 1 or source.count(end) != 1:
    raise SystemExit('direct-cache production extraction boundaries missing or ambiguous')
args.output.parent.mkdir(parents=True, exist_ok=True)
(args.output.parent / 'wine-direct-cache.inc').write_text(
    source[source.index(start) + len(start):source.index(end)])
delete_start = source.index('static NTSTATUS delete_view( struct file_view *view )')
delete_end = source.index('\n}\n', delete_start) + 3
(args.output.parent / 'wine-delete-view.inc').write_text(source[delete_start:delete_end])
cpu_start = '/* WINE_PS5_VM_CPU_TEST_BEGIN */'
cpu_end = '/* WINE_PS5_VM_CPU_TEST_END */'
if source.count(cpu_start) != 1 or source.count(cpu_end) != 1:
    raise SystemExit('VM CPU production extraction boundaries missing or ambiguous')
(args.output.parent / 'wine-vm-cpu.inc').write_text(
    source[source.index(cpu_start) + len(cpu_start):source.index(cpu_end)])
if not args.cpu_test:
    direct_source = (args.wine / 'dlls/ntdll/unix/ps5_direct.c').read_text()
    fatal_begin = '/* WINE_PS5_DIRECT_FAIL_FAST_TEST_BEGIN */'
    fatal_end = '/* WINE_PS5_DIRECT_FAIL_FAST_TEST_END */'
    if direct_source.count(fatal_begin) != 1 or direct_source.count(fatal_end) != 1:
        raise SystemExit('direct fail-fast production extraction markers missing or ambiguous')
    (args.output.parent / 'wine-direct-fail-fast.inc').write_text(
        direct_source[direct_source.index(fatal_begin) + len(fatal_begin):direct_source.index(fatal_end)])
subprocess.run(shlex.split(args.cc) + [
    '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
    '-D__PROSPERO__=1', '-pthread', '-I' + str(args.output.parent),
    str(Path(__file__).with_name('test-direct-fail-fast.c' if args.fail_fast_test else
                                'test-vm-cpu.c' if args.cpu_test else 'test-direct-cache.c')),
    '-o', str(args.output),
], check=True)
print('DIRECT_CACHE_ISOLATION implementation=PRODUCTION_EXTRACTED')
