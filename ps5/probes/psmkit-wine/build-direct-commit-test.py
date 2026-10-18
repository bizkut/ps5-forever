#!/usr/bin/env python3
"""Compile the production direct-backing transaction with deterministic kernel boundaries."""
import argparse
from pathlib import Path
import shlex
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--wine', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--cc', default='cc')
args = parser.parse_args()
source = (args.wine / 'dlls/ntdll/unix/ps5_direct.c').read_text()
begin = 'BOOL ps5_direct_finish_commit('
end = '\nint ps5_direct_protect('
if source.count(begin) != 1 or source.count(end) != 1:
    raise SystemExit('direct commit production extraction boundaries missing or ambiguous')
body = source[source.index(begin):source.index(end)]
args.output.parent.mkdir(parents=True, exist_ok=True)
(args.output.parent / 'wine-direct-commit.inc').write_text(body)
whole_begin = 'BOOL ps5_direct_is_whole_backed('
whole_end = '\nBOOL ps5_direct_cache_pressure('
if source.count(whole_begin) != 1 or source.count(whole_end) != 1:
    raise SystemExit('direct whole-view production extraction boundaries missing or ambiguous')
(args.output.parent / 'wine-direct-whole.inc').write_text(
    source[source.index(whole_begin):source.index(whole_end)])
tune_begin = '/* WINE_PS5_DIRECT_TUNE_TEST_BEGIN */'
tune_end = '/* WINE_PS5_DIRECT_TUNE_TEST_END */'
if source.count(tune_begin) != 1 or source.count(tune_end) != 1:
    raise SystemExit('direct tune production extraction markers missing or ambiguous')
(args.output.parent / 'wine-direct-tune.inc').write_text(
    source[source.index(tune_begin) + len(tune_begin):source.index(tune_end)])
fatal_begin = '/* WINE_PS5_DIRECT_FAIL_FAST_TEST_BEGIN */'
fatal_end = '/* WINE_PS5_DIRECT_FAIL_FAST_TEST_END */'
if source.count(fatal_begin) != 1 or source.count(fatal_end) != 1:
    raise SystemExit('direct fail-fast production extraction markers missing or ambiguous')
(args.output.parent / 'wine-direct-fail-fast.inc').write_text(
    source[source.index(fatal_begin) + len(fatal_begin):source.index(fatal_end)])
subprocess.run(shlex.split(args.cc) + [
    '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-pthread',
    '-I' + str(args.output.parent), str(Path(__file__).with_name('test-direct-commit.c')),
    '-o', str(args.output),
], check=True)
print('DIRECT_COMMIT_ISOLATION implementation=PRODUCTION_EXTRACTED')
