#!/usr/bin/env python3
"""Small host contracts for configure reuse and byte-identical ZIP assembly."""
import ast
import importlib.util
import os
from pathlib import Path
import shlex
import tempfile
import zipfile


PS5 = Path(__file__).resolve().parents[2]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def functions(path, names, namespace):
    tree = ast.parse(path.read_text())
    selected = ast.Module(body=[node for node in tree.body
                                if isinstance(node, ast.FunctionDef) and node.name in names],
                          type_ignores=[])
    exec(compile(selected, str(path), 'exec'), namespace)
    return namespace


stamp = load('configure_stamp', PS5 / 'configure-stamp.py')
package = load('release_package', PS5 / 'release/package.py')

with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    build = root / 'build'
    script = root / 'configure'
    script.write_text('''#!/bin/sh
set -eu
printf 'configure\\n' >> events
mkdir -p include
touch config.status include/config.h
printf 'CFLAGS = %s\\nCXXFLAGS = -g -O2\\ni386_CFLAGS = %s\\nx86_64_CFLAGS = %s\\nclean:\\n\\t@echo clean >> events\\n\\t@rm -f object.o\\n' "$CFLAGS" "${CROSSCFLAGS:-}" "${CROSSCFLAGS:-}" > Makefile
''')
    script.chmod(0o755)
    script.with_name('configure.ac').write_text('first input\n')
    environment = dict(os.environ, CFLAGS='-g -O2')
    command = [str(script)]
    stamp.configure(build, command, environment)
    assert (build / 'events').read_text().splitlines() == ['configure']
    (build / 'object.o').touch()
    stamp.configure(build, command, environment)
    assert (build / 'events').read_text().splitlines() == ['configure']
    assert (build / 'object.o').exists()
    environment['CFLAGS'] = '-g -O2 -DCHANGED'
    stamp.configure(build, command, environment)
    assert (build / 'events').read_text().splitlines() == ['configure', 'clean', 'configure']
    assert not (build / 'object.o').exists()
    (build / '.configure-stamp.json').unlink()
    stamp.configure(build, command, environment)
    assert (build / 'events').read_text().splitlines()[-1] == 'configure'
    assert (build / 'events').read_text().splitlines().count('clean') == 1
    script.with_name('configure.ac').write_text('changed input\n')
    stamp.configure(build, command, environment)
    assert (build / 'events').read_text().splitlines().count('configure') == 4
    assert (build / 'events').read_text().splitlines().count('clean') == 1
    # A legacy build tree without a stamp must not reuse differently flagged objects.
    (build / '.configure-stamp.json').unlink()
    environment['CFLAGS'] = '-g -O2'
    stamp.configure(build, command, environment)
    assert (build / 'events').read_text().splitlines()[-2:] == ['clean', 'configure']
    cross = stamp.compiler_flags([str(script), 'CROSSCFLAGS=-O3 -DPE'], environment)
    assert cross['i386_CFLAGS'] == cross['x86_64_CFLAGS'] == '-O3 -DPE'
    makefile = build / 'Makefile'
    makefile.write_text(makefile.read_text().replace('CFLAGS = -g -O2\n', 'CFLAGS = -O0\n', 1))
    stamp.configure(build, command, environment)
    assert (build / 'events').read_text().splitlines()[-2:] == ['clean', 'configure']
    cross_build = root / 'cross-build'
    environment['CROSSCFLAGS'] = '-O2'
    stamp.configure(cross_build, command, environment)
    stamp.configure(cross_build, command, environment)
    assert (cross_build / 'events').read_text().splitlines() == ['configure']
    environment['CROSSCFLAGS'] = '-O3'
    stamp.configure(cross_build, command, environment)
    assert (cross_build / 'events').read_text().splitlines() == ['configure', 'clean', 'configure']

    compile_helpers = functions(Path(__file__).with_name('build-runtime.py'), {
        'command_for', 'output_path', 'prepare_compile_entries', 'exact_object_manifest',
    }, {'Path': Path, 'os': os, 'shlex': shlex, 'build': root})
    outputs = ('dlls/one/unix/a.o', 'dlls/one/unix/b.o', 'dlls/one-extra/unix/c.o')
    for name in outputs:
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.touch()
    compiles = [
        {'file': 'a.c', 'directory': str(root),
         'command': "'/tool path/prospero-clang' -c a.c -o dlls/one/unix/a.o"},
        {'file': 'b.c', 'directory': str(root),
         'arguments': ['/tool/prospero-clang++', '-c', 'b.c', '-o', 'dlls/one/unix/b.o']},
        {'file': 'c.c', 'directory': str(root),
         'command': 'prospero-clang -c c.c -o dlls/one-extra/unix/c.o'},
        {'file': 'pe.c', 'directory': str(root),
         'command': 'x86_64-w64-mingw32-clang -c pe.c -o dlls/one/unix/pe.o'},
    ]
    original_commands = [compile_helpers['command_for'](entry) for entry in compiles]
    compile_helpers['prepare_compile_entries'](compiles, root)
    for entry, command in zip(compiles, original_commands):
        assert compile_helpers['command_for'](entry) == command
        changed = compile_helpers['command_for'](entry)
        changed[0] = 'changed'
        assert compile_helpers['command_for'](entry) == command
    assert compile_helpers['exact_object_manifest'](compiles, 'dlls/one') == [
        root / outputs[0], root / outputs[1]]

    entries = []
    for name, data, mode in (
            ('empty', b'', 0o644), ('bin/runner', b'ELF' * 700001, 0o755),
            ('unicode-\u03bb.txt', bytes(range(256)) * 8193, 0o644)):
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        path.chmod(mode)
        entries.append((name, path))
    names = {name for name, _ in entries}
    serial = root / 'serial.zip'
    with zipfile.ZipFile(serial, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, path in entries:
            info = zipfile.ZipInfo(f'{package.TITLE_ID}/{name}', package.EPOCH)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            info.external_attr = (0o100755 if path.stat().st_mode & 0o111 else 0o100644) << 16
            with path.open('rb') as source, archive.open(info, 'w') as target:
                while chunk := source.read(1 << 20):
                    target.write(chunk)
    for jobs in (1, 3):
        output = root / f'parallel-{jobs}.zip'
        package.write_archive(output, entries, names, jobs)
        assert output.read_bytes() == serial.read_bytes(), jobs
print('build-speed: configure stamp, compile index and parallel/serial ZIP bytes PASS')
