#!/usr/bin/env python3
"""Check real Wine import-library members and their linked PE first-call targets."""
import argparse
import os
from pathlib import Path
import struct
import subprocess
import tempfile

import pefile


MEMORY_MASK = 0xe0000000
READ_EXECUTE = 0x60000000
READ_WRITE = 0xc0000000
CODE = 0x20


def run(command, directory, environment):
    return subprocess.run(command, cwd=directory, env=environment, check=True,
                          text=True, stdout=subprocess.PIPE).stdout


def check_arch(winebuild, tools, arch, directory, environment):
    directory.mkdir()
    spec = directory / 'probe.spec'
    spec.write_text('@ cdecl DelayProbe()\n')
    consumer = directory / 'consumer.c'
    consumer.write_text('__declspec(dllimport) int __cdecl DelayProbe(void);\n'
                        'int main(void) { return DelayProbe(); }\n')
    library = directory / 'libprobe.delay.a'
    executable = directory / 'consumer.exe'
    run([str(winebuild), '-w', '--implib', '-o', str(library),
         '--without-dlltool', '-b', arch + '-w64-mingw32',
         '--export', str(spec)], directory, environment)
    run([str(tools / (arch + '-w64-mingw32-clang')), str(consumer), str(library),
         '-ldelayimp', '-Wl,--no-insert-timestamp', '-o', str(executable)],
        directory, environment)
    # Inspect members extracted from the actual library, not a copied emitter.
    objects = directory / 'objects'
    objects.mkdir()
    run([str(tools / 'llvm-ar'), 'x', str(library)], objects, environment)
    failures = []
    thunk_sections = 0
    machine = 0x14c if arch == 'i686' else 0x8664
    for path in sorted(objects.glob('*.o')):
        data = path.read_bytes()
        actual_machine, sections, _, _, _, optional_size, _ = struct.unpack_from('<HHIIIHH', data)
        if actual_machine != machine:
            failures.append(f'{path.name}: wrong COFF machine {actual_machine:#x}')
        for index in range(sections):
            offset = 20 + optional_size + index * 40
            name = data[offset:offset + 8].rstrip(b'\0')
            if name != b'.text$1':
                continue
            thunk_sections += 1
            flags, = struct.unpack_from('<I', data, offset + 36)
            print(f'{arch} {path.name} .text$1 flags={flags:#010x}')
            if flags & (MEMORY_MASK | CODE) != READ_EXECUTE | CODE:
                failures.append(f'{arch}: import-library delay thunk is not read/execute code without write')
    if thunk_sections != 1:
        failures.append(f'{arch}: expected one generated delay thunk, found {thunk_sections}')

    symbols = {}
    for line in run([str(tools / 'llvm-nm'), '--format=posix', '--defined-only',
                     str(executable)], directory, environment).splitlines():
        name, _, value, *_ = line.split()
        symbols[name] = int(value, 16)
    prefix = '_' if arch == 'i686' else ''
    image = pefile.PE(str(executable))
    base = image.OPTIONAL_HEADER.ImageBase
    slot = symbols['__imp_' + prefix + 'DelayProbe']
    pointer_size = 4 if arch == 'i686' else 8
    target = int.from_bytes(image.get_data(slot - base, pointer_size), 'little')
    # This is the initialized IAT pointer the first import call actually jumps to.
    for label, address in [('first-call IAT target', target),
                           ('import jump', symbols[prefix + 'DelayProbe']),
                           ('delay helper', symbols[prefix + '__wine_delay_load_probe'])]:
        section = image.get_section_by_rva(address - base)
        flags = section.Characteristics if section is not None else 0
        print(f'{arch} {label} va={address:#x} flags={flags:#010x}')
        if flags & (MEMORY_MASK | CODE) != READ_EXECUTE | CODE:
            failures.append(f'{arch}: linked {label} is not read/execute code without write')
    slot_section = image.get_section_by_rva(slot - base)
    if slot_section is None or slot_section.Characteristics & MEMORY_MASK != READ_WRITE:
        failures.append(f'{arch}: delay IAT must remain writable data without execute')
    image.close()
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--winebuild', type=Path, required=True)
    parser.add_argument('--llvm-mingw', type=Path, required=True)
    args = parser.parse_args()
    tools = args.llvm_mingw.resolve() / 'bin'
    environment = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ['PATH'])
    failures = []
    with tempfile.TemporaryDirectory(prefix='winebuild-delayload-') as temporary:
        for arch in ('i686', 'x86_64'):
            failures.extend(check_arch(args.winebuild.resolve(), tools, arch,
                                       Path(temporary) / arch, environment))
    if failures:
        raise SystemExit('\n'.join(failures))
    print('WINEBUILD_DELAYLOAD_QUALIFIED arches=i686,x86_64 object=RX linked_first_call=RX iat=RW')


if __name__ == '__main__':
    main()
