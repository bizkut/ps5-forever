#!/usr/bin/env python3
"""Compile actual registry save/flush/status and Wineboot proof publication.

Only the server object/protocol context, serialized test bytes and Win32 path/
registration boundaries are adapted. File streams, syscalls, errno mapping,
dirty transitions, flush handler and proof publication are production code.
"""
import argparse
import importlib.util
from pathlib import Path
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--wine', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--save-source', type=Path,
                        help='Use an earlier production save_branch for a behavioral RED baseline')
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    spec = importlib.util.spec_from_file_location('surface_extraction', here / 'build-desktop-surface-test.py')
    extraction = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(extraction)
    definition = extraction.definition
    registry = args.wine / 'server/registry.c'
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # Replace only the framework declaration, not the handler body, to expose
    # the real request handler to the standalone server-context adapter.
    source = registry.read_text()
    declaration = 'DECL_HANDLER(flush_key)'
    if source.count(declaration) != 1:
        raise SystemExit('ambiguous flush_key handler declaration')
    handler_input = args.output.parent / 'registry-save-handler-source.c'
    handler_input.write_text(source.replace(declaration, 'static void test_flush_key(void)'))
    functions = definition(registry, 'make_clean') + '\n\n'
    functions += definition(args.save_source or registry, 'save_branch') + '\n\n'
    functions += definition(registry, 'flush_registry') + '\n\n'
    functions += definition(args.wine / 'server/file.c', 'file_set_error') + '\n\n'
    functions += definition(handler_input, 'test_flush_key') + '\n'
    (args.output.parent / 'registry-save-functions.inc').write_text(functions)
    proof = definition(args.wine / 'programs/wineboot/ps5-crypto.h', 'wineboot_finish_title_prefix')
    (args.output.parent / 'registry-save-proof.inc').write_text(proof + '\n')
    subprocess.run(shlex.split(args.cc) + [
        '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
        '-Wno-unused-parameter', '-Wno-sign-compare', '-D__PROSPERO__',
        '-I' + str(args.output.parent), '-I' + str(args.wine / 'include'),
        str(here / 'test-registry-save.c'), '-o', str(args.output),
    ], check=True)
    print('REGISTRY_SAVE_ISOLATION save_branch,flush_registry,flush_key,file_set_error,proof=PRODUCTION '
          'filesystem=REAL fault_adapters=DESCRIPTOR_CLOSURE_AND_PATH_ERRORS')


if __name__ == '__main__':
    main()
