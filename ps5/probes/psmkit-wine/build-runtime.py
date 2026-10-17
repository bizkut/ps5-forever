#!/usr/bin/env python3
"""Build isolated x86-64-only Wine Unix modules and their native title."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import struct
import shutil
import subprocess

from runtime_imports import (
    B1_NATIVE_FORWARDED, B1_NATIVE_RESOLVERS, load_policy, make_records, module_manifest,
    write_candidate, write_outputs,
)
from runtime_profiles import (
    WRITEBROKER_MAX_BYTES, WRITEBROKER_RELATIVE, profile, runtime_tree_manifest, builtin_prefix_files,
)

here = Path(__file__).resolve().parent
# ps5/build/gnutls-stage mirrors build-gnutls.sh's DESTDIR layout (the SDK's
# absolute prefix under the stage directory).
sdk_root = Path(os.environ.get('PS5_PAYLOAD_SDK', str(Path.home() / 'ps5-payload-sdk-platform')))
gnutls_stage_lib = Path(str(here / '../../build/gnutls-stage') + str(sdk_root) +
                        '/target/user/homebrew/lib')
parser = argparse.ArgumentParser()
parser.add_argument('--wine-build', type=Path, default=here / '../../build/wine-ps5-build')
parser.add_argument('--profile', choices=('p1w', 'b1'), default='p1w')
parser.add_argument('--profile-config', type=Path)
parser.add_argument('--quarantine', type=Path,
    help='isolated intermediate output tree (defaults to the selected profile tree)')
parser.add_argument('--gnutls-lib', type=Path, default=gnutls_stage_lib,
    help="ps5/build-gnutls.sh's staged static archives (B1 only)")
parser.add_argument('--freetype-build', type=Path, default=here / '../../build/freetype-ps5-build',
    help="ps5/build-freetype.sh's CMake build tree, whose objects are relinked (B1 only)")
parser.add_argument('--writebroker-elf', type=Path,
    help='bounded payload ELF shipped as B1 title data')
parser.add_argument('--vkd3d-dlls', type=Path, default=here / '../../build/vkd3d-proton-64/dlls',
    help="ps5/build-vkd3d-proton.sh's 64-bit d3d12.dll and d3d12core.dll (B1 only)")
parser.add_argument('--dxvk-dlls', type=Path, default=here / '../../build/dxvk-64/dlls',
    help="ps5/build-dxvk.sh's 64-bit dxgi.dll, the game's native dxgi (B1 only)")
args = parser.parse_args()
build = args.wine_build.resolve()
selected = profile(args.profile, json.loads(args.profile_config.read_text()) if args.profile_config else None)
gnutls_lib = args.gnutls_lib.resolve()
freetype_build = args.freetype_build.resolve()
writebroker_elf = args.writebroker_elf.resolve() if args.writebroker_elf else None
if args.profile == 'b1':
    if (writebroker_elf is None or writebroker_elf.is_symlink() or
            not writebroker_elf.is_file()):
        raise SystemExit(f'B1 write broker ELF missing or unsafe: {writebroker_elf}')
    writebroker_size = writebroker_elf.stat().st_size
    if not 0 < writebroker_size <= WRITEBROKER_MAX_BYTES:
        raise SystemExit(f'B1 write broker ELF exceeds bounded size: {writebroker_size}')
    with writebroker_elf.open('rb') as source:
        if source.read(4) != b'\x7fELF':
            raise SystemExit(f'B1 write broker payload is not an ELF: {writebroker_elf}')
elif writebroker_elf is not None:
    raise SystemExit('--writebroker-elf is B1-only')
profile_leaf = 'title-psmkit-wine-b1' if args.profile == 'b1' else 'title-psmkit-wine-runtime'
quarantine = (args.quarantine or (here / '../../build' / (profile_leaf + '-quarantine'))).resolve()
work = quarantine / 'child-work'
child_out = quarantine / 'child-title'
parent_work = quarantine / 'parent-work'
parent_out = quarantine / 'parent-title'
policy_path = here / ('runtime-import-policy-b1.json' if args.profile == 'b1' else 'runtime-import-policy.json')
llvm = Path(os.environ.get('LLVM_BIN', '/opt/homebrew/opt/llvm/bin' if
                          os.uname().sysname == 'Darwin' else '/usr/lib/llvm-19/bin'))
safe_libc_members = (
    'assert.o', 'passwd.o', 'termios.o', 'times.o',
)
if args.profile == 'b1':
    # netdb.o: resolvers. nl_langinfo.o: the charset query GnuTLS's gnulib
    # makes (no raw syscall). Its C-locale ctype entry points are in
    # native-entry.c: SDK no-locale.o needs wide-string calls the title lacks.
    safe_libc_members += ('netdb.o', 'nl_langinfo.o')
for owned in (work, child_out, parent_work, parent_out):
    if owned.is_symlink() or owned.is_file():
        owned.unlink()
    elif owned.exists():
        shutil.rmtree(owned)
work.mkdir(parents=True)


def run(command, *, cwd=None, env=None):
    subprocess.run([str(item) for item in command], cwd=cwd, env=env, check=True)


def command_for(entry):
    return list(entry['arguments']) if 'arguments' in entry else shlex.split(entry['command'])


def output_path(command, directory):
    try:
        value = command[command.index('-o') + 1]
    except (ValueError, IndexError):
        raise SystemExit(f'compile command has no separate -o: {command!r}')
    path = Path(value)
    return (directory / path).resolve() if not path.is_absolute() else path.resolve()


def compile_entry(entries, suffix, destination, extra, original_output=None):
    matches = [entry for entry in entries if Path(entry['file']).as_posix().endswith(suffix)]
    if original_output:
        matches = [entry for entry in matches if output_path(
            command_for(entry), Path(entry.get('directory', build))).as_posix().endswith(original_output)]
    if len(matches) != 1:
        raise SystemExit(f'expected one compile command for {suffix} ({original_output or "any output"}), found {len(matches)}')
    entry = matches[0]
    command = command_for(entry)
    index = command.index('-o')
    command[index + 1] = str(destination)
    if '-MF' in command:
        command[command.index('-MF') + 1] = str(destination.with_suffix('.d'))
    command.extend(extra)
    run(command, cwd=Path(entry.get('directory', build)))
    return entry, command


def exact_object_manifest(entries, directory, generated=()):
    """Extract the built object set for one generated module directory."""
    root = (build / directory).resolve()
    objects = []
    for entry in entries:
        command = command_for(entry)
        path = output_path(command, Path(entry.get('directory', build)))
        if (path.is_relative_to(root) and path.suffix == '.o' and
                any(Path(value).name.startswith('prospero-clang') for value in command)):
            objects.append(path)
    objects = sorted(set(objects))
    for relative in generated:
        path = (build / relative).resolve()
        if path not in objects:
            objects.append(path)
    if not objects:
        raise SystemExit(f'no compile objects found for {directory}')
    missing = [str(path) for path in objects if not path.is_file()]
    if missing:
        raise SystemExit(f'missing objects for {directory}: {missing}')
    return objects


def compiler_from(entry):
    for value in command_for(entry):
        if Path(value).name.startswith('prospero-clang'):
            compiler = Path(value)
            directory = Path(entry.get('directory', build))
            return compiler if compiler.is_absolute() else (directory / compiler).resolve()
    raise SystemExit('compile command does not identify prospero-clang')


def soname(path):
    text = subprocess.check_output([str(llvm / 'llvm-readelf'), '-d', str(path)], text=True)
    match = re.search(r'\(SONAME\).*\[([^]]+)\]', text)
    if not match:
        raise SystemExit(f'missing DT_SONAME in {path}')
    return match.group(1)


def harden_module_headers(path):
    """Make the SDK linker's fixed 16-KiB layout W^X and explicitly RELRO."""
    data = bytearray(path.read_bytes())
    if data[:7] != b'\x7fELF\x02\x01\x01' or len(data) < 64:
        raise SystemExit(f'{path}: not an ELF64 little-endian module')
    phoff = struct.unpack_from('<Q', data, 32)[0]
    phentsize, phnum = struct.unpack_from('<HH', data, 54)
    if phentsize != 56 or not phnum:
        raise SystemExit(f'{path}: unsupported program-header layout')
    headers = []
    for index in range(phnum):
        offset = phoff + index * phentsize
        if offset + phentsize > len(data):
            raise SystemExit(f'{path}: truncated program headers')
        headers.append([index, offset, *struct.unpack_from('<IIQQQQQQ', data, offset)])
    loads = [item for item in headers if item[2] == 1]
    dynamics = [item for item in headers if item[2] == 2]
    # Code, RELRO (relocated data, read-only afterwards) and writable data.
    # A module without writable data (FreeType) has no third LOAD.
    if len(loads) not in (2, 3) or len(dynamics) != 1:
        raise SystemExit(f'{path}: expected two or three LOADs and one DYNAMIC')
    if any(item[2] in (0x6474e551, 0x6474e552) for item in headers):
        raise SystemExit(f'{path}: linker layout unexpectedly already has STACK/RELRO')
    executable = [item for item in loads if item[3] & 1]
    if len(executable) != 1 or not executable[0][3] & 4:
        raise SystemExit(f'{path}: expected one readable executable LOAD')
    dynamic_address = dynamics[0][5]
    relro_candidates = [
        item for item in loads
        if not item[3] & 1 and item[3] & 2 and
        item[5] <= dynamic_address < item[5] + item[8]
    ]
    if len(relro_candidates) != 1:
        raise SystemExit(f'{path}: expected one writable LOAD containing DYNAMIC')
    relro = relro_candidates[0]
    following = sorted(item[5] for item in loads if item[5] > relro[5])
    if (following and following[0] & 0x3fff) or relro[5] & 0x3fff:
        raise SystemExit(f'{path}: RELRO LOAD is not 16-KiB separated')
    if len(following) != len(loads) - 2:
        raise SystemExit(f'{path}: unexpected LOAD after the code and RELRO LOADs')
    # Without a writable LOAD after it, the whole RELRO LOAD becomes read-only.
    relro_size = (following[0] if following else (relro[5] + relro[8] + 0x3fff) & ~0x3fff) - relro[5]
    if relro[8] > relro_size or relro[7] > relro[8]:
        raise SystemExit(f'{path}: RELRO LOAD overlaps writable data')
    first_file_data = min(item[4] for item in loads)
    if phoff + (phnum + 2) * phentsize > first_file_data:
        raise SystemExit(f'{path}: no room for hardened program headers')

    executable[0][3] &= ~2
    struct.pack_into('<IIQQQQQQ', data, executable[0][1], *executable[0][2:])
    relro[8] = relro_size
    struct.pack_into('<IIQQQQQQ', data, relro[1], *relro[2:])
    additions = (
        (0x6474e552, 4, relro[4], relro[5], relro[6],
         relro[7], relro_size, 1),
        (0x6474e551, 6, 0, 0, 0, 0, 0, 16),
    )
    for index, header in enumerate(additions, phnum):
        struct.pack_into('<IIQQQQQQ', data, phoff + index * phentsize, *header)
    struct.pack_into('<H', data, 56, phnum + len(additions))
    path.write_bytes(data)


def link_module(compiler, destination, module_soname, objects, wrap=()):
    command = [compiler, '-shared', '-nodefaultlibs', '-Wl,-Bsymbolic', '-Wl,-t',
               '-Wl,-z,separate-code,-z,relro,-z,now,-z,noexecstack',
               f'-Wl,-soname,{module_soname}', '-o', destination, *objects]
    command.extend(f'-Wl,--wrap={name}' for name in wrap)
    trace = subprocess.check_output([str(item) for item in command],
                                    stderr=subprocess.STDOUT, text=True)
    destination.with_suffix('.link-trace.txt').write_text(trace)
    if 'libc.a(' in trace:
        raise SystemExit(f'{destination}: SDK libc archive body in dedicated module link')
    harden_module_headers(destination)
    return [str(item) for item in command]


def undefined_symbols(path):
    text = subprocess.check_output(
        [str(llvm / 'llvm-readelf'), '--dyn-symbols', '--wide', str(path)], text=True)
    result = {'strong': set(), 'weak': set()}
    for line in text.splitlines():
        fields = line.split()
        if len(fields) < 8 or fields[6] != 'UND' or fields[4] not in ('GLOBAL', 'WEAK'):
            continue
        name = fields[7].split('@', 1)[0]
        if name:
            result['weak' if fields[4] == 'WEAK' else 'strong'].add(name)
    return result


entries = json.loads((build / 'compile_commands.json').read_text())


def require_current_title_abi(entries, directories):
    """Refuse linked Wine objects compiled before the last title-ABI change.

    A runtime-version bump rebuilt only ntdll once; the in-process server kept
    the old version and the title stopped at start (run b1-b2-3). Only the
    directories this build links are checked. A source counts when it reaches
    the header through quoted includes (ntdll's files get it via
    unix_private.h)."""
    roots = [(build / directory).resolve() for directory in directories]
    include = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)
    reaches = {}

    def reaches_header(path, top):
        if path not in reaches:
            reaches[path] = False
            for name in include.findall(path.read_text(errors='replace')):
                if name == 'wine/ps5_title.h':
                    reaches[path] = True
                    break
                found = next((base / name for base in (path.parent, top / 'include')
                              if (base / name).is_file()), None)
                if found and reaches_header(found.resolve(), top):
                    reaches[path] = True
                    break
        return reaches[path]

    header = None
    stale = []
    for entry in entries:
        output = output_path(command_for(entry), Path(entry.get('directory', build)))
        if not any(output.is_relative_to(root) for root in roots):
            continue
        source = Path(entry.get('directory', build), entry['file']).resolve()
        if source.suffix != '.c':
            continue
        top = next((parent for parent in source.parents
                    if (parent / 'include/wine/ps5_title.h').is_file()), None)
        if top is None or not reaches_header(source, top):
            continue
        candidate = top / 'include/wine/ps5_title.h'
        header = header or candidate
        if not output.is_file() or output.stat().st_mtime < candidate.stat().st_mtime:
            stale.append(str(output.relative_to(build)))
    if stale:
        raise SystemExit(f'Wine objects older than {header}: rebuild them first '
                         f'(ps5/build-wine.sh dlls/ntdll/all server/all loader/all ...): {sorted(stale)}')


# The profile's Unix modules besides ntdll/win32u/wineserver and native libraries.
extra_modules = [name.removesuffix('.so') for name in selected['module_paths']
                 if name not in ('ntdll.so', 'win32u.so', 'wineserver.so') and not name.startswith('lib')]

def module_directory(stem):
    return f'dlls/{stem}.drv' if stem in ('wineps5', 'wineps5audio') else f'dlls/{stem}'

require_current_title_abi(entries, (
    'loader', 'server', 'dlls/ntdll/unix', 'dlls/win32u',
    *(module_directory(stem) for stem in extra_modules)))
loader_object = work / 'wine-public-title.o'
loader_entry, loader_command = compile_entry(
    entries, '/loader/main.c', loader_object,
    ['-ffunction-sections', '-fdata-sections', '-Wno-unused-function',
     '-DWINEPS5_PUBLIC_TITLE_ONLY',
     '-Dmain=wine_public_title_main', '-Dpayload_exit=pw_native_exit',
     '-Ddlopen=pw_public_dlopen', '-Ddlsym=pw_public_dlsym',
     '-Ddlclose=pw_public_dlclose', '-Ddlerror=pw_public_dlerror',
     '-Ddladdr=pw_public_dladdr',
     *([f'-DWINE_PS5_TITLE_ROOT={json.dumps(selected["root"])}'] if args.profile == 'b1' else [])],
    '/loader/main.o')
compiler = compiler_from(loader_entry)

ntdll_objects = exact_object_manifest(
    entries, 'dlls/ntdll/unix', ('dlls/ntdll/unix/version.o',))
signal_matches = [path for path in ntdll_objects
                  if path.as_posix().endswith('/dlls/ntdll/unix/signal_x86_64.o')]
if len(signal_matches) != 1:
    raise SystemExit(f'expected title signal object once in ntdll manifest, found {signal_matches}')
signal_object = work / 'signal_x86_64-title.o'
signal_entry, signal_command = compile_entry(
    entries, '/dlls/ntdll/unix/signal_x86_64.c', signal_object,
    ['-Wno-unused-function', '-Wno-macro-redefined', '-DWINE_PS5_TITLE'],
    '/dlls/ntdll/unix/signal_x86_64.o')
if compiler_from(signal_entry) != compiler:
    raise SystemExit('loader and signal compile commands use different host toolchains')
if output_path(command_for(signal_entry), Path(signal_entry.get('directory', build))) != signal_matches[0]:
    raise SystemExit('signal compile command output does not match ntdll object manifest')
ntdll_title_objects = [signal_object if path == signal_matches[0] else path for path in ntdll_objects]
variant_compiles = {}
if args.profile == 'b1':
    for stem, extra in (
            ('server', [f'-DWINE_PS5_TITLE_ROOT={json.dumps(selected["root"])}']),
            ('env', ['-DWINE_PS5_TITLE_PARENT_CWD=({const char *cwd=getenv("WINEPS5_TITLE_PARENT_CWD"); if (!cwd) abort(); cwd;})'])):
        destination = work / f'{stem}-b1.o'
        _, command = compile_entry(
            entries, f'/dlls/ntdll/unix/{stem}.c', destination, extra,
            f'/dlls/ntdll/unix/{stem}.o')
        original = (build / f'dlls/ntdll/unix/{stem}.o').resolve()
        if original not in ntdll_title_objects:
            raise SystemExit(f'B1 replacement object not in ntdll manifest: {original}')
        ntdll_title_objects = [destination if path == original else path for path in ntdll_title_objects]
        variant_compiles[stem] = command
ntdll_title = work / 'ntdll-title.so'
ntdll_link = link_module(
    compiler, ntdll_title,
    selected['remote_root'] + '/lib/wine/x86_64-unix/ntdll.so' if args.profile == 'b1'
    else soname(build / 'dlls/ntdll/ntdll.so'), ntdll_title_objects)

server_objects = exact_object_manifest(entries, 'server')
main_matches = [path for path in server_objects if path.as_posix().endswith('/server/main.o')]
if len(main_matches) != 1:
    raise SystemExit(f'expected server main object once, found {main_matches}')
server_main = work / 'server-main-title.o'
objcopy = compiler.with_name('prospero-objcopy')
run([objcopy, '--redefine-sym', 'main=wineserver_main', main_matches[0], server_main])
server_title_objects = [server_main if path == main_matches[0] else path for path in server_objects]
server_title = work / 'wineserver-title.so'
server_link = link_module(
    compiler, server_title, 'wineserver.so', server_title_objects,
    ('malloc', 'calloc', 'realloc', 'free', 'pthread_key_create',
     'pthread_getspecific', 'pthread_setspecific'))

# No SDK libraries or payload ntdll DT_NEEDED: Wine's ntdll exports resolve
# against the already admitted title module in the custom loader.
win32u_objects = exact_object_manifest(entries, 'dlls/win32u')
if args.profile == 'b1':
    for stem in ('driver', 'winstation'):
        destination = work / f'win32u-{stem}-b1.o'
        _, command = compile_entry(
            entries, f'/dlls/win32u/{stem}.c', destination, ['-DWINE_PS5_TITLE_B1'],
            f'/dlls/win32u/{stem}.o')
        original = (build / f'dlls/win32u/{stem}.o').resolve()
        if original not in win32u_objects:
            raise SystemExit(f'B1 replacement object not in win32u manifest: {original}')
        win32u_objects = [destination if path == original else path for path in win32u_objects]
        variant_compiles['win32u_' + stem] = command
win32u_title = work / 'win32u-title.so'
win32u_link = link_module(
    compiler, win32u_title, selected['module_paths']['win32u.so'],
    win32u_objects)

sdk_root = compiler.parent.parent
archive = sdk_root / 'target/lib/libc.a'
archive_tool = compiler.with_name('prospero-ar')
safe_libc_dir = work / 'safe-libc'
safe_libc_dir.mkdir(parents=True, exist_ok=True)
safe_libc_objects = []
for member in safe_libc_members:
    destination = safe_libc_dir / member
    destination.write_bytes(subprocess.check_output(
        [str(archive_tool), 'p', str(archive), member]))
    rewritten = destination.with_suffix('.rewrite.o')
    rename = ['--redefine-sym', '__syscall=pw_native_fail_syscall']
    if member == 'netdb.o':
        for source, target in {**B1_NATIVE_RESOLVERS, **B1_NATIVE_FORWARDED}.items():
            rename += ['--redefine-sym', f'{source}={target}']
    run([objcopy, *rename, destination, rewritten])
    os.replace(rewritten, destination)
    safe_libc_objects.append(destination)
safe_libc_archive = work / 'libtitle-safe-libc.a'
safe_libc_archive.unlink(missing_ok=True)
run([archive_tool, 'rc', safe_libc_archive, *safe_libc_objects])


module_files = {
    'ntdll.so': ntdll_title,
    'win32u.so': win32u_title,
    'wineserver.so': server_title,
}
extra_build = {}
if args.profile == 'b1':
    for stem in extra_modules:
        objects = exact_object_manifest(entries, module_directory(stem))
        if stem == 'crypt32':
            # Root certificates come from the title root's share/wine/cacert.pem.
            variant = work / 'crypt32-unixlib-b1.o'
            _, command = compile_entry(
                entries, '/dlls/crypt32/unixlib.c', variant,
                [f'-DWINE_PS5_TITLE_ROOT={json.dumps(selected["root"])}'], '/dlls/crypt32/unixlib.o')
            original = (build / 'dlls/crypt32/unixlib.o').resolve()
            if original not in objects:
                raise SystemExit(f'B1 replacement object not in crypt32 manifest: {original}')
            objects = [variant if path == original else path for path in objects]
            variant_compiles['crypt32_unixlib'] = command
        destination = work / f'{stem}-title.so'
        link = link_module(compiler, destination, selected['module_paths'][stem + '.so'], objects)
        module_files[stem + '.so'] = destination
        extra_build[stem] = {
            'objects': [str(path.relative_to(build)) if path.is_relative_to(build) else path.name
                        for path in objects],
            'output': destination.name, 'link': link,
        }
    # GnuTLS from ps5/build-gnutls.sh's static archives (no raw syscalls),
    # linked without the SDK libc like every other title module.
    archives = [gnutls_lib / f'lib{name}.a' for name in ('gnutls', 'hogweed', 'nettle', 'tasn1')]
    missing = [str(path) for path in archives if not path.is_file()]
    if missing:
        raise SystemExit(f'GnuTLS static archives missing (run ps5/build-gnutls.sh): {missing}')
    # Verified title modules carry no constructors. GnuTLS's only pair is
    # global.o's implicit gnutls_global_init/deinit; secur32, bcrypt and
    # crypt32 all call gnutls_global_init themselves. Link a copy without it.
    gnutls_archive = work / 'libgnutls-noctor.a'
    shutil.copyfile(archives[0], gnutls_archive)
    global_object = work / 'global.o'
    global_object.write_bytes(subprocess.check_output(
        [str(archive_tool), 'p', str(archives[0]), 'global.o']))
    run([objcopy, '-R', '.ctors', '-R', '.rela.ctors', '-R', '.dtors', '-R', '.rela.dtors',
         global_object])
    run([archive_tool, 'r', gnutls_archive, global_object])
    destination = work / 'libgnutls-title.so'
    link = link_module(compiler, destination, selected['module_paths']['libgnutls.so'], [
        '-Wl,--whole-archive', gnutls_archive, '-Wl,--no-whole-archive',
        '-Wl,--start-group', *archives[1:], '-Wl,--end-group'])
    module_files['libgnutls.so'] = destination
    extra_build['libgnutls'] = {
        'archives': {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in archives},
        'output': destination.name, 'link': link,
    }
    # FreeType from ps5/build-freetype.sh's own objects (built position-
    # independent for its shared library; no raw syscall instruction), relinked
    # without the SDK libc like every other title module.
    freetype_objects = sorted((freetype_build / 'freetype/CMakeFiles/freetype.dir').rglob('*.o'))
    if not freetype_objects:
        raise SystemExit(f'FreeType objects missing (run ps5/build-freetype.sh): {freetype_build}')
    destination = work / 'libfreetype-title.so'
    link = link_module(compiler, destination, selected['module_paths']['libfreetype.so'],
                       freetype_objects)
    module_files['libfreetype.so'] = destination
    extra_build['libfreetype'] = {
        'objects': {str(path.relative_to(freetype_build)): hashlib.sha256(path.read_bytes()).hexdigest()
                    for path in freetype_objects},
        'output': destination.name, 'link': link,
    }
module_symbols = {name: undefined_symbols(path) for name, path in module_files.items()}
candidate_path = work / 'title-imports-candidate.json'
gpu = selected.get('gpu', False)
write_candidate(module_symbols, candidate_path, profile_name=args.profile, gpu=gpu)
try:
    policy_rows = load_policy(policy_path, profile_name=args.profile)
    records = make_records(module_symbols, policy_rows, profile_name=args.profile, gpu=gpu)
except ValueError as error:
    raise SystemExit(str(error)) from error
title_modules = module_manifest(module_files, profile_name=args.profile, gpu=gpu)
include_path = work / 'title-imports.inc'
imports_manifest = work / 'title-imports.json'
policy_sha256 = hashlib.sha256(policy_path.read_bytes()).hexdigest()
generated_imports = write_outputs(
    records, title_modules, include_path, imports_manifest, policy_sha256,
    profile_name=args.profile, gpu=gpu)
profile_defines = ''.join(
    f'#define PW_RUNTIME_{macro} {json.dumps(value)}\n'
    for macro, value in (
        ('ROOT', selected['root']), ('LOADER', selected['loader']),
        ('PREFIX', selected['prefix']), ('HOME', selected['home']),
        ('DEBUG', selected['debug']), ('NTDLL', selected['module_paths']['ntdll.so']),
        ('CWD', selected['cwd'] or ''), ('DLL_OVERRIDES', selected['dll_overrides'] or '')))
profile_defines += '#define PW_RUNTIME_CHILD_DLL_OVERRIDES ' + json.dumps(
    selected.get('child_dll_overrides', selected.get('dll_overrides') or '')) + '\n'
profile_defines += f'#define PW_RUNTIME_MODULE_COUNT {len(selected["module_paths"])}\n'
profile_defines += f'#define PW_RUNTIME_GPU {1 if selected.get("gpu", False) else 0}\n'
profile_defines += '#define PW_RUNTIME_PROTON_ENABLE_HDR ' + json.dumps(
    selected.get('environment', {}).get('PROTON_ENABLE_HDR', '')) + '\n'
profile_defines += '#define PW_RUNTIME_DIRECT_REQUESTS ' + json.dumps(
    selected.get('environment', {}).get('WINEPS5_DIRECT_REQUESTS', '')) + '\n'
profile_defines += '#define PW_RUNTIME_PROFILE ' + json.dumps(
    selected.get('environment', {}).get('WINEPS5_PROFILE', '')) + '\n'
profile_defines += '#define PW_RUNTIME_LOG_ASYNC ' + json.dumps(
    selected.get('environment', {}).get('WINEPS5_LOG_ASYNC', '')) + '\n'
profile_defines += '#define PW_RUNTIME_MXCSR_TRACE ' + json.dumps(
    selected.get('environment', {}).get('WINEPS5_MXCSR_TRACE', '')) + '\n'
profile_defines += '#define PW_RUNTIME_WRITE_TRACE ' + json.dumps(
    selected.get('environment', {}).get('WINEPS5_WRITE_TRACE', '')) + '\n'
profile_defines += '#define PW_RUNTIME_WRITE_STICKY ' + json.dumps(
    selected.get('environment', {}).get('WINEPS5_WRITE_STICKY', '')) + '\n'
profile_defines += '#define PW_RUNTIME_HDR_TRACE ' + json.dumps(
    selected.get('environment', {}).get('WINEPS5_HDR_TRACE', '')) + '\n'
profile_defines += '#define PW_RUNTIME_QUERY_TRACE ' + json.dumps(
    selected.get('environment', {}).get('WINEPS5_QUERY_TRACE', '')) + '\n'
profile_defines += '#define PW_RUNTIME_HDR_MODE ' + json.dumps(
    selected.get('environment', {}).get('WINEPS5_HDR_MODE', '')) + '\n'
profile_defines += '#define PW_RUNTIME_SDR_WHITE_NITS ' + json.dumps(
    selected.get('environment', {}).get('WINEPS5_SDR_WHITE_NITS', '')) + '\n'
profile_defines += '#define PW_RUNTIME_VIDEOOUT_STATS ' + json.dumps(
    selected.get('environment', {}).get('PS5_VIDEOOUT_STATS', '')) + '\n'
profile_defines += '#define PW_RUNTIME_VKD3D_CONFIG ' + json.dumps(
    selected.get('environment', {}).get('VKD3D_CONFIG', '')) + '\n'
profile_defines += '#define PW_RUNTIME_VKD3D_QUEUE_PROFILE ' + json.dumps(
    selected.get('environment', {}).get('VKD3D_QUEUE_PROFILE', '')) + '\n'
profile_defines += '#define PW_RUNTIME_VKD3D_PRESENT_MODE ' + json.dumps(
    selected.get('environment', {}).get('VKD3D_SWAPCHAIN_PRESENT_MODE', '')) + '\n'
profile_defines += '#define PW_RUNTIME_CHILD_PATH ' + json.dumps(
    '/app0/game-child.bin' if selected.get('gpu', False) else
    '/app0/psmkit-wine-runtime-child.bin') + '\n'
if args.profile == 'b1':
    profile_defines += '#define PW_RUNTIME_B1 1\n'
    profile_defines += f'#define PW_RUNTIME_HEADLESS {1 if selected["headless"] else 0}\n'
    profile_defines += ''.join(
        f'#define PW_RUNTIME_{key.upper()} {json.dumps(selected[key])}\n'
        for key in ('installer_url', 'installer_fallback_url', 'installer_dos', 'launcher_unix', 'installer_unix',
                    'installer_cwd'))
    profile_defines += '#define PW_RUNTIME_PAYLOAD_PROGRAMS ' + json.dumps(';'.join(selected['payload_programs'])) + '\n'
    profile_defines += '#define PW_RUNTIME_REFUSED_PROGRAMS ' + json.dumps(';'.join(selected['refused_programs'])) + '\n'
    profile_defines += '#define PW_RUNTIME_PREFETCH_HOSTS ' + json.dumps(';'.join(selected['prefetch_hosts'])) + '\n'
    profile_defines += '#define PW_RUNTIME_DNS_UPSTREAMS {' + (','.join(
        '0x%08xu' % int.from_bytes(bytes(int(part) for part in server.split('.')), 'big')
        for server in selected['dns_upstreams']) or '0') + '}\n'
    profile_defines += '#define PW_RUNTIME_PAYLOAD_DLL_OVERRIDES ' + json.dumps(selected['payload_dll_overrides']) + '\n'
    profile_defines += '#define PW_RUNTIME_BROWSER_SWITCHES ' + json.dumps(';'.join(
        program + ' ' + ' '.join(switches) for program, switches in selected['browser_switches'].items())) + '\n'
    profile_defines += '#define PW_RUNTIME_WRITEBROKER_ELF ' + json.dumps(
        selected['root'] + '/' + WRITEBROKER_RELATIVE) + '\n'
    profile_defines += '#define PW_RUNTIME_PARENT_ARGV {' + ','.join(
        '(char *)' + json.dumps(value) for value in selected['argv']) + ',NULL}\n'
    profile_defines += f'#define PW_RUNTIME_PARENT_ARGC {len(selected["argv"])}\n'
wrapper = work / 'native-entry-generated.c'
wrapper.write_text(
    '#include <stddef.h>\n#include <wine/ps5_title.h>\n' + profile_defines +
    f'#define PW_NATIVE_IMPORTS_INCLUDE {json.dumps(str(include_path))}\n'
    f'#include {json.dumps(str(here / "native-entry.c"))}\n')

# B1: the runtime's own io_log lines (title vulkan frames on the presenting
# thread, input, supervisor) take the same queue as the Unix modules'
# sceKernelDebugOutText import (runtime_imports.B1_PLATFORM_OVERRIDES).
io_sources = [here / '../localproc/psmkit-io.c']
if args.profile == 'b1':
    io_wrapper = work / 'psmkit-io-title.c'
    io_wrapper.write_text(
        '#define sceKernelDebugOutText pw_title_klog_text\n'
        f'#include {json.dumps(str(here / "../localproc/psmkit-io.c"))}\n')
    io_sources = [io_wrapper, here / 'title-log.c']


loader_source = Path(loader_entry['file'])
if not loader_source.is_absolute():
    loader_source = Path(loader_entry.get('directory', build)) / loader_source
env = dict(os.environ, PS5_TITLE_WORK=str(work), PS5_TITLE_RADV='',
           WINE_SRC=str(loader_source.resolve().parents[1]))
run(['bash', here / '../../title/build.sh', child_out, wrapper,
     here / 'elf-runtime.c', here / 'protocol.c', *io_sources,
     loader_object, safe_libc_archive], cwd=here, env=env)
converted_elf = work / 'eboot.elf'
patched_elf = work / 'localproc.elf'
child_fself = child_out / 'eboot.bin'
# Title SDK fields: firmware 11.60 refuses a child carrying 5.50's local-helper
# SDK pair (PRX_SCE_MODULE_LOAD_ERROR); the title's pair runs on 5.50 and 11.60.
run(['python3', here / '../localproc/patch-local-helper.py',
     '--sdk', 'title', converted_elf, patched_elf])
tool = (here / '../../../../../PS5_Vulkan/build/host/ps5-native-tool').resolve()
run([tool, 'self', '--sign', '--in', patched_elf,
     '--out', child_fself, '--magic', '0x1D3D154F'])
run(['python3', here / '../localproc/patch-local-helper.py', '--container',
     child_fself, child_fself])
run([tool, 'self', '--inspect', '--file', child_fself])

# Both roles bind the identical immutable module/import manifest and public
# loader. Only the parent has the native supervisor and BigApp process type.
parent_work.mkdir(parents=True)
parent_loader_object = loader_object
parent_loader_command = loader_command
if args.profile == 'b1':
    parent_loader_object = parent_work / 'wine-public-title-parent.o'
    parent_loader_command = list(loader_command)
    parent_loader_command[parent_loader_command.index('-o') + 1] = str(parent_loader_object)
    if '-MF' in parent_loader_command:
        parent_loader_command[parent_loader_command.index('-MF') + 1] = str(parent_loader_object.with_suffix('.d'))
    parent_loader_command.extend([
        '-DWINE_PS5_TITLE_B1_PARENT',
        '-Dwine_ps5_title_wait_children=pw_supervisor_wait_children',
    ])
    run(parent_loader_command, cwd=Path(loader_entry.get('directory', build)))
parent_wrapper = parent_work / 'native-entry-parent.c'
parent_wrapper.write_text(
    '#include <stddef.h>\n#include <wine/ps5_title.h>\n' +
    profile_defines + '#define PW_PARENT_ROLE 1\n'
    f'#define PW_NATIVE_IMPORTS_INCLUDE {json.dumps(str(include_path))}\n'
    f'#include {json.dumps(str(here / "native-entry.c"))}\n')
parent_protocol = parent_work / 'protocol-parent.c'
parent_protocol.write_text(
    '#define PW_P1C_FAULT_INJECTION 1\n'
    f'#include {json.dumps(str(here / "protocol.c"))}\n')
parent_supervisor = parent_work / 'runtime-parent.c'
parent_supervisor.write_text(
    '#include <stddef.h>\n#include <wine/ps5_title.h>\n' +
    profile_defines + '#define PW_P1C_FAULT_INJECTION 1\n'
    f'#include {json.dumps(str(here / "runtime-parent.c"))}\n')
desktop_sources = []
if args.profile == 'b1' and not selected['headless']:
    title = (here / '../../title').resolve()
    desktop_wrapper = parent_work / 'title-desktop.c'
    desktop_wrapper.write_text(
        '#define PS5_TITLE_EMBEDDED 1\n'
        f'#include {json.dumps(str(title / "main.c"))}\n')
    hub_wrapper = parent_work / 'title-hub.c'
    hub_wrapper.write_text(
        '#define PS5_TITLE_EMBEDDED 1\n'
        f'#include {json.dumps(str(title / "hub.c"))}\n')
    desktop_sources = [
        desktop_wrapper, hub_wrapper, title / 'compositor.c', title / 'display.c',
        title / 'input.c', title / 'pointer.c', title / 'ime.c',
    ]
gpu_sources = []
if selected.get('gpu', False):
    title = (here / '../../title').resolve()
    gpu_sources = [here / 'runtime-vulkan.c', here / 'runtime-input.c',
                   title / 'input.c', title / 'pointer.c', title / 'ime.c']
radv_archive = (Path(os.environ.get('PS5_RADV_INSTALL', str(here / '../../build/radv-release-patched'))) /
                'lib/libvulkan_radeon.ps5.a').resolve()
if gpu_sources and not radv_archive.is_file():
    raise SystemExit(f'RADV archive missing (build it separately, never as part of title qualification): {radv_archive}')
parent_env = dict(env, PS5_TITLE_WORK=str(parent_work),
                  PS5_TITLE_RADV=str(radv_archive) if gpu_sources else '')
run(['bash', here / '../../title/build.sh', parent_out, parent_wrapper,
     here / 'elf-runtime.c', parent_protocol, parent_supervisor, *desktop_sources, *gpu_sources,
     *io_sources, parent_loader_object, safe_libc_archive],
    cwd=here, env=parent_env)
if gpu_sources:
    parameter = parent_out / 'sce_sys/param.json'
    metadata = json.loads(parameter.read_text())
    metadata['attribute'] = metadata.get('attribute', 0) | 0x20000000
    parameter.write_text(json.dumps(metadata, indent=2) + '\n')

writebroker_identity = None
if args.profile == 'b1':
    # Use Wine's own install rules for the complete PE/data layout, but copy
    # only the Windows trees and data. No generic Unix library is admitted.
    install = quarantine / 'wine-install'
    if install.exists():
        shutil.rmtree(install)
    # Staging must not relink payload Unix outputs when B1-only source
    # switches changed their dependency timestamps. They are not used here.
    run(['make', '-s', '-C', build, '-j' + (os.environ.get('JOBS') or '8'), 'install-lib',
         '-o', 'dlls/ntdll/ntdll.so', '-o', 'dlls/win32u/win32u.so',
         '-o', 'server/wineserver', '-o', 'server/wineserver.so',
         '-o', 'loader/wine',
         '-o', 'dlls/wineps5.drv/wineps5.so',
         *(['-o', 'dlls/wineps5audio.drv/wineps5audio.so'] if gpu else []),
         f'DESTDIR={install}', f'prefix={selected["root"]}'])
    installed = install / selected['root'].lstrip('/')
    tree = quarantine / 'wine-title'
    if tree.exists():
        shutil.rmtree(tree)
    (tree / 'lib/wine').mkdir(parents=True)
    writebroker_destination = tree / WRITEBROKER_RELATIVE
    shutil.copy2(writebroker_elf, writebroker_destination)
    writebroker_identity = {
        'path': WRITEBROKER_RELATIVE,
        'app_path': selected['root'] + '/' + WRITEBROKER_RELATIVE,
        'classification': 'data_only_not_loaded',
        'size': writebroker_destination.stat().st_size,
        'sha256': hashlib.sha256(writebroker_destination.read_bytes()).hexdigest(),
    }
    for arch in ('i386', 'x86_64'):
        shutil.copytree(installed / f'lib/wine/{arch}-windows', tree / f'lib/wine/{arch}-windows')
    shutil.copytree(installed / 'share/wine', tree / 'share/wine')
    loader_path = tree / 'lib/wine/x86_64-unix/wine'
    loader_path.parent.mkdir(parents=True, exist_ok=True)
    # argv[0] is a path identity for the compiled-in public loader, not an
    # executable that a title can spawn. Keep the fresh loader file there so
    # Wine's installed-path resolver can stat it.
    shutil.copy2(build / 'loader/wine', loader_path)
    for name, source in module_files.items():
        destination = tree / selected['module_paths'][name].removeprefix(selected['root'] + '/')
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
    # Mozilla's root certificates (curl's extract), pinned as in
    # ps5/stage-wine.sh: crypt32 and so Chromium verify HTTPS against them.
    cacert = here / '../../build/cacert-2026-09-25.pem'
    if not cacert.is_file():
        run(['curl', '--fail', '--silent', '--location', '--output', cacert.with_suffix('.part'),
             'https://curl.se/ca/cacert-2026-09-25.pem'])
        cacert.with_suffix('.part').rename(cacert)
    if hashlib.sha256(cacert.read_bytes()).hexdigest() != (
            'a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505'):
        raise SystemExit(f'pinned certificate bundle digest mismatch: {cacert}')
    shutil.copy2(cacert, tree / 'share/wine/cacert.pem')
    # The console has no system fonts: the same pinned fallback faces (CJK,
    # Thai, right-to-left) and notices ps5/stage-wine.sh gives payload Wine.
    droid = 'https://raw.githubusercontent.com/aosp-mirror/platform_frameworks_base/android-4.4.4_r2/data/fonts'
    noto = 'https://raw.githubusercontent.com/notofonts/noto-fonts/ffebf8c1ee449e544955a7e813c54f9b73848eac'
    for name, digest, url in (
            ('DroidSansFallbackLegacy.ttf', '82a74d0ef369ec7979172023cfff87f04f0a617a2871c39ef6aeb0f399c5cdaf',
             f'{droid}/DroidSansFallbackLegacy.ttf'),
            ('NotoSansThai-Regular.ttf', '404ddfb5ed0aaa6b6ec8a85700d682978992062d67da93903967b56cbd9a4acc',
             f'{noto}/hinted/ttf/NotoSansThai/NotoSansThai-Regular.ttf'),
            ('NotoSansArabic-Regular.ttf', 'ceea25b464a656dc3b26849bab9356740401af62aedf1bfa8b7f0d9b75925b1b',
             f'{noto}/hinted/ttf/NotoSansArabic/NotoSansArabic-Regular.ttf'),
            ('NotoSansHebrew-Regular.ttf', 'a7fa16fffb27bedb060a0866267c29e9859aeb9c21cc33f5b3aaf6eb062eca85',
             f'{noto}/hinted/ttf/NotoSansHebrew/NotoSansHebrew-Regular.ttf'),
            ('DroidSansFallback-NOTICE', '38751245389e1e23f73e6f5384b5cbe7fa972cc4410c5adc9c04b082a0b9561a',
             f'{droid}/NOTICE'),
            ('NotoSans-LICENSE', '0dab92d0544f7b233403f14b84a663bdbfa746982eda629e7f4f9ffe1b036feb',
             f'{noto}/LICENSE')):
        cache = here / '../../build' / name
        if not cache.is_file():
            run(['curl', '--fail', '--silent', '--location', '--output', cache.with_suffix('.part'), url])
            cache.with_suffix('.part').rename(cache)
        if hashlib.sha256(cache.read_bytes()).hexdigest() != digest:
            raise SystemExit(f'pinned font asset digest mismatch: {cache}')
        shutil.copy2(cache, tree / 'share/wine/fonts' / name)
    # Forever Launcher keeps the user's prefix in /data/forever. forever-setup.h
    # installs these first-start defaults and copies the prefix files the app
    # owns whenever this list (source, prefix path, digest) changes.
    forever = tree / 'share/forever'
    (forever / 'system32').mkdir(parents=True)
    shutil.copy2(here / '../../release/defaults/Config.wtf', forever / 'Config.wtf')
    # A protected application can restore system DLL code from its disk copy.
    # Every builtin PE must therefore match the image Wine actually loads.
    owned = {destination: source for source, destination in builtin_prefix_files(tree)}
    for source_dir, names in ((args.vkd3d_dlls, ('d3d12.dll', 'd3d12core.dll')),
                              (args.dxvk_dlls, ('dxgi.dll',))):
        for name in names:
            source = source_dir.resolve() / name
            if not source.is_file():
                raise SystemExit(f'missing app-owned prefix file {source}')
            shutil.copy2(source, forever / 'system32' / name)
            owned[f'drive_c/windows/system32/{name}'] = f'share/forever/system32/{name}'
    lines = ['# Forever Launcher: prefix files the app owns (source below wine-title, prefix path, sha256)']
    for destination, source in sorted(owned.items()):
        path = tree / source
        if not path.is_file():
            raise SystemExit(f'missing app-owned prefix file {path}')
        lines.append(f'{source}\t{destination}\t{hashlib.sha256(path.read_bytes()).hexdigest()}')
    (forever / 'prefix-files.txt').write_text('\n'.join(lines) + '\n')
    tree_manifest = runtime_tree_manifest(tree, selected)
    (tree / 'title-root-manifest.json').write_text(json.dumps(tree_manifest, indent=2, sort_keys=True) + '\n')
    shutil.rmtree(install)
identity_by_name = {item['name']: item for item in title_modules}
build_manifest = {
    'format': 2,
    'feature_macro': 'WINE_PS5_TITLE',
    'profile': selected,
    **({'writebroker': writebroker_identity} if writebroker_identity else {}),
    'variant_compiles': variant_compiles,
    'quarantine': str(quarantine.relative_to(here.parent.parent)),
    'generic_outputs_untouched': [
        'dlls/ntdll/ntdll.so', 'dlls/win32u/win32u.so',
        *(['dlls/wineps5.drv/wineps5.so'] if args.profile == 'b1' else []),
        *(['dlls/wineps5audio.drv/wineps5audio.so'] if gpu else []),
        'server/wineserver', 'server/wineserver.so', 'loader/wine'],
    'loader_compile': loader_command,
    'parent_loader_compile': parent_loader_command,
    **({'desktop': {
        'enabled': not selected['headless'],
        'sources': [str(path) for path in desktop_sources],
    }} if args.profile == 'b1' else {}),
    'gpu': {
        'enabled': bool(gpu_sources),
        'parent_sources': [str(path) for path in gpu_sources],
        'parent_archive': str(radv_archive) if gpu_sources else None,
    },
    'signal_compile': signal_command,
    'policy': {
        'path': policy_path.name,
        'sha256': policy_sha256,
    },
    'candidate_manifest': {
        'path': candidate_path.name,
        'sha256': hashlib.sha256(candidate_path.read_bytes()).hexdigest(),
        'authorization': 'CANDIDATE_ONLY_NOT_AUTHORIZED',
    },
    'imports_manifest': {
        'path': imports_manifest.name,
        'sha256': hashlib.sha256(imports_manifest.read_bytes()).hexdigest(),
        'include_sha256': generated_imports['include_sha256'],
    },
    'ntdll': {
        'objects': [str(path.relative_to(build)) if path.is_relative_to(build) else path.name
                    for path in ntdll_title_objects],
        'output': ntdll_title.name,
        'link': ntdll_link,
        'identity': identity_by_name['ntdll.so'],
    },
    'wineserver': {
        'objects': [str(path.relative_to(build)) if path.is_relative_to(build) else path.name
                    for path in server_title_objects],
        'output': server_title.name,
        'link': server_link,
        'identity': identity_by_name['wineserver.so'],
    },
    'win32u': {
        'objects': [str(path.relative_to(build)) if path.is_relative_to(build) else path.name
                    for path in win32u_objects],
        'output': win32u_title.name,
        'link': win32u_link,
        'identity': identity_by_name['win32u.so'],
    },
    **{stem: dict(record, identity=identity_by_name[stem + '.so'])
       for stem, record in extra_build.items()},
    'safe_libc': {
        'archive': str(archive),
        'members': [
            {'name': path.name, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
            for path in safe_libc_objects
        ],
        'output': safe_libc_archive.name,
        'sha256': hashlib.sha256(safe_libc_archive.read_bytes()).hexdigest(),
    },
    'native_child': {
        'llvm_pie': {
            'path': 'llvm-pie.elf',
            'sha256': hashlib.sha256((work / 'llvm-pie.elf').read_bytes()).hexdigest(),
        },
        'converted_elf': {
            'path': converted_elf.name,
            'sha256': hashlib.sha256(converted_elf.read_bytes()).hexdigest(),
        },
        'patched_elf': {
            'path': patched_elf.name,
            'sha256': hashlib.sha256(patched_elf.read_bytes()).hexdigest(),
        },
        'fself': {
            'path': str(child_fself.relative_to(quarantine)),
            'sha256': hashlib.sha256(child_fself.read_bytes()).hexdigest(),
            'size': child_fself.stat().st_size,
        },
    },
    'native_parent': {
        key: {
            'path': str(path.relative_to(quarantine)),
            'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
            'size': path.stat().st_size,
        } for key, path in (
            ('llvm_pie', parent_work / 'llvm-pie.elf'),
            ('converted_elf', parent_work / 'eboot.elf'),
            ('fself', parent_out / 'eboot.bin'))
    },
}
(work / 'title-runtime-build.json').write_text(
    json.dumps(build_manifest, indent=2, sort_keys=True) + '\n')
