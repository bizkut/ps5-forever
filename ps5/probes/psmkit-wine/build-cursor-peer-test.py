#!/usr/bin/env python3
"""Compile complete production cursor negotiation and both hub state machines."""
import argparse
import importlib.util
from pathlib import Path
import re
import shlex
import subprocess
import struct
import tempfile
from runtime_imports import load_policy

parser = argparse.ArgumentParser()
parser.add_argument('--wine', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--cc', default='cc')
args = parser.parse_args()
here = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('surface_test', here / 'build-desktop-surface-test.py')
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)
definition = helper.definition
args.output.parent.mkdir(parents=True, exist_ok=True)
for source, prefix, types, functions in (
    (here / 'runtime-parent.c', 'native', ('pw_hub_peer', 'pw_hub_window', 'pw_desktop_hub'),
     ('hub_find_window', 'hub_unlink_window', 'hub_remove_window', 'hub_restack',
      'hub_replay_cursors', 'hub_replay', 'hub_previous_sink', 'hub_set_sink', 'hub_driver_message')),
    (args.wine / 'loader/main.c', 'payload', ('ps5_hub_peer', 'ps5_hub_window', 'ps5_desktop_hub'),
     ('ps5_hub_find_window', 'ps5_hub_unlink_window', 'ps5_hub_remove_window', 'ps5_hub_restack',
      'ps5_hub_replay_cursors', 'ps5_hub_replay', 'ps5_hub_previous_sink', 'ps5_hub_set_sink', 'ps5_hub_driver_message')),
):
    (args.output.parent / f'cursor-{prefix}-types.inc').write_text('\n\n'.join(
        definition(source, name, structure=True) for name in types) + '\n')
    (args.output.parent / f'cursor-{prefix}-functions.inc').write_text('\n\n'.join(
        definition(source, name) for name in functions) + '\n')
source = args.wine / 'dlls/wineps5.drv/init.c'
(args.output.parent / 'cursor-queue-types.inc').write_text(definition(source, 'queue_thread', structure=True) + '\n')
(args.output.parent / 'cursor-transport.inc').write_text('\n\n'.join(
    definition(source, name) for name in ('queue_is_registered', 'register_queue', 'PS5_ThreadDetach',
        'read_event', 'cursor_flush_pending',
        'cursor_confirm_capabilities', 'cursor_peer_capabilities', 'ps5_send_cursor_message',
        'PS5_ProcessEvents', 'PS5_SetDesktopWindow')) + '\n')
subprocess.run(shlex.split(args.cc) + [
    '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
    '-I' + str(args.output.parent), '-I' + str(args.wine / 'include'),
    str(here / 'test-cursor-peer.c'), '-o', str(args.output)], check=True)

# Compile the real desktop sources plus the new native B1 adapter extracted
# verbatim with real Wine API/table declarations. Full win32u private headers
# require generated COM headers, which a source-only host checkout does not have.
def inspect_no_tls(obj, shared, label):
    data = obj.read_bytes()
    if data[:4] != b'\x7fELF':
        print(f'CURSOR_DRIVER_ELF_INSPECTION_SKIPPED module={label} host_object=NON_ELF')
        return
    if data[4:6] != b'\x02\x01':
        raise SystemExit('cursor driver host inspection requires ELF64 little-endian')
    subprocess.run(shlex.split(args.cc) + ['-shared', str(obj), '-o', str(shared)], check=True)
    tls_relocations = set(range(16, 24)) | {34, 35, 36}
    for artifact in (obj, shared):
        data = artifact.read_bytes()
        phoff = struct.unpack_from('<Q', data, 32)[0]
        phentsize, phnum = struct.unpack_from('<HH', data, 54)
        for index in range(phnum):
            if struct.unpack_from('<I', data, phoff + index * phentsize)[0] == 7:
                raise SystemExit(f'{label}: PT_TLS segment forbidden')
        shoff = struct.unpack_from('<Q', data, 40)[0]
        shentsize, shnum = struct.unpack_from('<HH', data, 58)
        for index in range(shnum):
            section = struct.unpack_from('<IIQQQQIIQQ', data, shoff + index * shentsize)
            if section[2] & 0x400:
                raise SystemExit(f'{label}: ELF TLS section forbidden')
            if section[1] in (4, 9):
                for offset in range(section[4], section[4] + section[5], section[9]):
                    info = struct.unpack_from('<Q', data, offset + 8)[0]
                    if info & 0xffffffff in tls_relocations:
                        raise SystemExit(f'{label}: AMD64 ELF TLS relocation forbidden')
    print(f'CURSOR_DRIVER_NO_ELF_TLS_PASS module={label} segments=NO_TLS sections=NO_TLS relocations=NO_TLS')


def check_title_imports(obj):
    data = obj.read_bytes()
    if data[:4] != b'\x7fELF':
        return
    shoff = struct.unpack_from('<Q', data, 40)[0]
    shentsize, shnum = struct.unpack_from('<HH', data, 58)
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shentsize) for i in range(shnum)]
    undefined = set()
    for section in sections:
        if section[1] != 2:
            continue
        strings = sections[section[6]]
        names = data[strings[4]:strings[4] + strings[5]]
        for offset in range(section[4], section[4] + section[5], section[9]):
            name, info, _, index, _, _ = struct.unpack_from('<IBBHQQ', data, offset)
            if index == 0 and info >> 4 in (1, 2) and name:
                undefined.add(names[name:names.index(b'\0', name)].decode())
    internal = {
        'NtUserGetIconInfo', 'NtGdiGetDIBitsInternal', 'NtGdiExtGetObjectW',
        'NtGdiCreateCompatibleDC', 'NtGdiDeleteObjectApp',
        '__wine_set_user_driver', 'update_display_cache',
    }
    reviewed = {row['symbol'] for row in load_policy(here / 'runtime-import-policy-b1.json', profile_name='b1')
                if row['module'] == 'win32u.so'}
    extra = undefined - internal - reviewed
    if extra:
        raise SystemExit(f'title-GPU cursor introduced unreviewed imports: {sorted(extra)}')
    print('CURSOR_WIN32U_IMPORTS_PASS unreviewed=NONE')


with tempfile.TemporaryDirectory(prefix='cursor-driver-elf-') as directory:
    root = Path(directory)
    (root / 'config.h').write_text('/* Host-only driver object inspection. */\n')
    (root / 'ps5platform').mkdir()
    (root / 'ps5platform/klog.h').write_text('int sceKernelDebugOutText(int, const char *);\n')
    driver_source = args.wine / 'dlls/win32u/driver.c'
    text = driver_source.read_text()
    declarations = []
    for name in ('null_user_driver', 'ps5_title_cursor_cache', 'ps5_title_gpu_driver',
                 'ps5_title_cursor_send', 'ps5_title_gpu_once'):
        rows = re.findall(r'^static [^\n]*\b' + re.escape(name) + r'\b[^\n]*;\s*$', text, re.M)
        if len(rows) != 1:
            raise SystemExit(f'expected one complete native driver state declaration for {name}')
        declarations.append(rows[0])
    adapter = root / 'win32u-title-gpu.c'
    adapter.write_text('''#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "ntgdi.h"
#include "ntuser.h"
#include "wine/gdi_driver.h"
#include "wine/ps5_title.h"
#include "wine/debug.h"
WINE_DEFAULT_DEBUG_CHANNEL(driver);
#include "wine/ps5_cursor.h"
extern BOOL update_display_cache(BOOL);
extern int sceKernelDebugOutText(int, const char *);
''' + '\n'.join(declarations) + '\n' + '\n\n'.join(
        definition(driver_source, name) for name in (
            'ps5_title_gpu_SetCursor', 'ps5_title_gpu_DestroyCursorIcon',
            'ps5_init_title_gpu_driver', 'ps5_load_headless_driver')) +
        '\nBOOL cursor_title_driver_host_entry(void) { return ps5_load_headless_driver(); }\n')
    for unit, defines, label in (
        (args.wine / 'dlls/wineps5.drv/init.c', [], 'wineps5-init'),
        (args.wine / 'dlls/wineps5.drv/cursor.c', [], 'wineps5-cursor'),
        (adapter, ['-D__PROSPERO__', '-DWINE_PS5_TITLE_B1'], 'win32u-title-gpu'),
    ):
        obj = root / (label + '.o')
        subprocess.run(shlex.split(args.cc) + [
            # Distribution compilers default to fortify/stack-protector, which
            # the title toolchain does not; their helpers are not title imports.
            '-std=gnu11', '-O2', '-fPIC', '-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=0',
            '-fno-stack-protector', '-D__WINESRC__', '-DWINE_UNIX_LIB', *defines,
            '-I' + str(root), '-I' + str(args.wine / 'include'),
            '-c', str(unit), '-o', str(obj)], check=True)
        inspect_no_tls(obj, root / (label + '.so'), label)
        if label == 'win32u-title-gpu':
            check_title_imports(obj)
