#!/usr/bin/env python3
"""Fail-closed qualification and publication for the dedicated title runtime."""
import argparse
import base64
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess

from runtime_imports import (
    discovered_rows, load_policy, module_names, native_role_imports,
    make_candidate_manifest, make_records,
)
from runtime_profiles import (
    ADDON_WORKER_MAX_BYTES, ADDON_WORKER_RELATIVE, WRITEBROKER_MAX_BYTES,
    WRITEBROKER_RELATIVE, profile, runtime_tree_manifest,
)

parser = argparse.ArgumentParser()
parser.add_argument('--wine-build', type=Path, default=Path('../../build/wine-ps5-build'))
parser.add_argument('--profile', choices=('p1w', 'b1'), default='p1w')
parser.add_argument('--profile-config', type=Path)
parser.add_argument('--host-test', type=Path, required=True)
parser.add_argument('--publish', type=Path)
parser.add_argument('--parent-title', type=Path)
parser.add_argument('--pe-dir', type=Path, default=Path('../../build/psmkit-wine-pe'))
parser.add_argument('--quarantine', type=Path)
args = parser.parse_args()
here = Path(__file__).resolve().parent
ps5_root = here.parent.parent
selected = profile(args.profile, json.loads(args.profile_config.read_text()) if args.profile_config else None)
native_roles = native_role_imports(
    args.profile, headless=selected.get('headless', False), gpu=selected.get('gpu', False))
module_paths = selected['module_paths']
profile_leaf = 'title-psmkit-wine-b1' if args.profile == 'b1' else 'title-psmkit-wine-runtime'
quarantine = (args.quarantine or here / '../../build' / (profile_leaf + '-quarantine')).resolve()
work = quarantine / 'child-work'
child_out = quarantine / 'child-title'
parent_title = (args.parent_title or quarantine / 'parent-title').resolve()
publish_out = (args.publish or here / '../../build' / profile_leaf).resolve()
host_out = (here / '../../build/psmkit-wine-host').resolve()
policy_path = here / ('runtime-import-policy-b1.json' if args.profile == 'b1' else 'runtime-import-policy.json')
llvm = Path(os.environ.get('LLVM_BIN', '/opt/homebrew/opt/llvm/bin'))
native_tool = (here / '../../../../../PS5_Vulkan/build/host/ps5-native-tool').resolve()

PT_LOAD = 1
PT_DYNAMIC = 2
PT_INTERP = 3
PT_NOTE = 4
PT_PHDR = 6
PT_TLS = 7
PT_GNU_EH_FRAME = 0x6474E550
PT_GNU_STACK = 0x6474E551
PT_GNU_RELRO = 0x6474E552
PF_X = 1
PF_W = 2
PF_R = 4
HEADER_NAMES = {
    PT_LOAD: 'LOAD',
    PT_DYNAMIC: 'DYNAMIC',
    PT_INTERP: 'INTERP',
    PT_NOTE: 'NOTE',
    PT_PHDR: 'PHDR',
    PT_TLS: 'TLS',
    PT_GNU_EH_FRAME: 'GNU_EH_FRAME',
    PT_GNU_STACK: 'GNU_STACK',
    PT_GNU_RELRO: 'GNU_RELRO',
}
ALLOWED_MODULE_HEADERS = {
    PT_PHDR, PT_LOAD, PT_DYNAMIC, PT_NOTE, PT_GNU_EH_FRAME, PT_GNU_STACK, PT_GNU_RELRO,
}
ALLOWED_RELOCATIONS = {
    'R_X86_64_64', 'R_X86_64_GLOB_DAT', 'R_X86_64_JUMP_SLOT', 'R_X86_64_RELATIVE',
}
EXPECTED_PRE_NEEDED = [
    'libkernel.sprx',
    'libSceLibcInternal.sprx',
]
EXPECTED_POST_NEEDED = [
    'libkernel.prx',
    'libSceLibcInternal.prx',
]
DEFINED_BRIDGE_OWNERS = {'custom_fd', 'custom_loader', 'custom_tls', 'fail_closed', 'platform'}
UNDEFINED_NM_CODES = {'U', 'u', 'w', 'v'}
PRIVATE = re.compile(
    r'^(?:kernel_|mdbg_|__kernel_init$|__patch_init$|__rtld_init$|__rtld_so_)')
FORBIDDEN_DEFINITIONS = {'mmap', 'mprotect', 'munmap', 'syscall', '__syscall'}
NID_SUFFIX = bytes.fromhex('518d64a635ded8c1e6b039b1c3e55230')
EXPECTED_TITLE_LIBC_SHA256 = 'e6ff45d16adf687855cc3b33b0c8a4132b6504360b221e0a34c7e99fb3ba0036'
EXPECTED_PARENT_NEEDED = [
    'libkernel.prx',
    'libSceLibcInternal.prx',
    'libSceSystemService.prx',
]
EXPECTED_PARENT_PRE_NEEDED = [*EXPECTED_PRE_NEEDED, 'libSceSystemService.sprx']
if args.profile == 'b1':
    # Children forward networking, desktop and payload-spawn operations.
    EXPECTED_PARENT_PRE_NEEDED = [*EXPECTED_PRE_NEEDED, 'libSceNet.sprx',
                                  'libSceNetCtl.sprx', 'libSceSystemService.sprx',
                                  'libSceUserService.sprx']
    EXPECTED_PARENT_NEEDED = [*EXPECTED_POST_NEEDED, 'libSceNet.prx',
                              'libSceNetCtl.prx', 'libSceSystemService.prx',
                              'libSceUserService.prx']
    if not selected['headless']:
        EXPECTED_PARENT_PRE_NEEDED = [
            'libkernel.sprx', 'libSceIme.prx', 'libSceKeyboard.sprx',
            'libSceLibcInternal.sprx', 'libSceMouse.sprx', 'libSceNet.sprx',
            'libSceNetCtl.sprx', 'libScePad.sprx', 'libSceSysmodule.sprx',
            'libSceSystemService.sprx', 'libSceUserService.sprx',
            'libSceVideoOut.sprx',
        ]
        EXPECTED_PARENT_NEEDED = [
            'libkernel.prx', 'libSceIme.prx', 'libSceKeyboard.prx',
            'libSceLibcInternal.prx', 'libSceMouse.prx', 'libSceNet.prx',
            'libSceNetCtl.prx', 'libScePad.prx', 'libSceSysmodule.prx',
            'libSceSystemService.prx', 'libSceUserService.prx',
            'libSceVideoOut.prx',
        ]
    EXPECTED_PARENT_PRE_NEEDED = sorted(set(EXPECTED_PARENT_PRE_NEEDED) | {
        'libSceHttp2.sprx', 'libSceSsl.sprx'}, key=str.lower)
    EXPECTED_PARENT_NEEDED = sorted(set(EXPECTED_PARENT_NEEDED) | {
        'libSceHttp2.prx', 'libSceSsl.prx'}, key=str.lower)
    # libSceIme's stub (ps5/title/stubs, soname libSceIme.prx, like AGC's) is
    # linked after the SDK's stubs (ps5/title/build.sh): the last needed library.
    for needed in (EXPECTED_PARENT_PRE_NEEDED, EXPECTED_PARENT_NEEDED):
        if 'libSceIme.prx' in needed:
            needed.remove('libSceIme.prx')
            needed.append('libSceIme.prx')
    if selected['gpu']:
        # SDK stubs precede custom AGC, inline-IME, then AGC-driver stubs.
        EXPECTED_PARENT_PRE_NEEDED = [
            'libkernel.sprx', 'libSceAudioOut.sprx', 'libSceKeyboard.sprx',
            'libSceLibcInternal.sprx', 'libSceMouse.sprx', 'libSceNet.sprx',
            'libSceNetCtl.sprx', 'libScePad.sprx', 'libSceSysmodule.sprx',
            'libSceSystemService.sprx', 'libSceUserService.sprx', 'libSceVideoOut.sprx',
            'libSceAgc.prx', 'libSceIme.prx', 'libSceAudioIn.prx', 'libSceAgcDriver.prx',
        ]
        EXPECTED_PARENT_NEEDED = [
            name.replace('.sprx', '.prx') for name in EXPECTED_PARENT_PRE_NEEDED]
NATIVE_ROLE_PROVIDERS = {
    provider for imports in native_roles.values() for provider in imports.values()
    if provider not in {'libkernel.prx', 'libSceLibcInternal.prx'}
}
PROVIDER_ID_ALPHABET = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-'


def output(tool, *arguments):
    return subprocess.check_output([str(llvm / tool), *map(str, arguments)], text=True)


def require(condition, message):
    if not condition:
        raise SystemExit(message)


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def regular(path, description):
    try:
        mode = path.lstat().st_mode
    except FileNotFoundError:
        raise SystemExit(f'missing {description}: {path}') from None
    require(stat.S_ISREG(mode), f'{description} is not a regular file: {path}')


def display_path(path):
    try:
        return str(path.resolve().relative_to(ps5_root))
    except ValueError:
        return str(path.resolve())


def read_json(path, description):
    regular(path, description)
    try:
        return json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise SystemExit(f'invalid {description} {path}: {error}') from error


def atomic_json(path, document):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + '.tmp')
    temporary.write_text(json.dumps(document, indent=2, sort_keys=True) + '\n')
    os.replace(temporary, path)


def elf_layout(path):
    regular(path, 'ELF input')
    data = path.read_bytes()
    require(len(data) >= 64, f'{path}: truncated ELF header')
    header = struct.unpack_from('<16sHHIQQQIHHHHHH', data)
    ident = header[0]
    require(ident[:4] == b'\x7fELF' and ident[4] == 2 and ident[5] == 1,
            f'{path}: not little-endian ELF64')
    require(header[2] == 62, f'{path}: not AMD64')
    program_offset = header[5]
    program_entry_size = header[9]
    program_count = header[10]
    require(program_entry_size == 56 and 0 < program_count <= 256,
            f'{path}: invalid program-header table')
    require(program_offset <= len(data) and
            program_count <= (len(data) - program_offset) // program_entry_size,
            f'{path}: program-header table outside file')
    headers = []
    for index in range(program_count):
        values = struct.unpack_from(
            '<IIQQQQQQ', data, program_offset + index * program_entry_size)
        kind, flags, offset, virtual, physical, file_size, memory_size, alignment = values
        if kind == PT_LOAD:
            require(file_size <= memory_size,
                    f'{path}: PT_LOAD segment {index} filesz exceeds memsz')
        require(offset <= len(data) and file_size <= len(data) - offset,
                f'{path}: segment {index} file range outside file')
        headers.append({
            'index': index,
            'type': kind,
            'type_name': HEADER_NAMES.get(kind, f'0x{kind:x}'),
            'flags': flags,
            'offset': offset,
            'virtual_address': virtual,
            'physical_address': physical,
            'file_size': file_size,
            'memory_size': memory_size,
            'alignment': alignment,
        })
    return data, headers


def flags_text(flags):
    return ''.join(letter if flags & bit else '-' for bit, letter in (
        (PF_R, 'R'), (PF_W, 'W'), (PF_X, 'X')))


def load_summary(path):
    data, headers = elf_layout(path)
    loads = []
    for header in headers:
        if header['type'] != PT_LOAD:
            continue
        segment = data[header['offset']:header['offset'] + header['file_size']]
        loads.append({
            **header,
            'flags_text': flags_text(header['flags']),
            'file_bytes_sha256': hashlib.sha256(segment).hexdigest(),
        })
    require(loads, f'{path}: no PT_LOAD segments')
    return data, headers, loads


def dynamic_text(path):
    return output('llvm-readelf', '--dynamic-table', '--wide', path)


def needed_libraries(path):
    return re.findall(
        r'\(NEEDED\).*Shared library: \[([^]]+)\]', dynamic_text(path))


def undefined_symbol_records(path):
    records = []
    text = output('llvm-readelf', '--dyn-symbols', '--wide', path)
    for line in text.splitlines():
        fields = line.split()
        if (len(fields) < 8 or fields[6] != 'UND' or
                fields[4] not in ('GLOBAL', 'WEAK')):
            continue
        name = fields[7].split('@', 1)[0]
        if not name:
            continue
        records.append({
            'name': name,
            'binding': fields[4],
            'type': fields[3],
            'visibility': fields[5],
            'size': int(fields[2], 0),
        })
    names = [item['name'] for item in records]
    require(len(names) == len(set(names)), f'{path}: duplicate dynamic undefined symbol')
    return records


def undefined_symbols(path):
    result = {'strong': set(), 'weak': set()}
    for record in undefined_symbol_records(path):
        result['weak' if record['binding'] == 'WEAK' else 'strong'].add(record['name'])
    return result


def nm_symbols(path, *options):
    symbols = {}
    for line in output('llvm-nm', '--format=posix', *options, path).splitlines():
        fields = line.split()
        if len(fields) >= 2:
            symbols.setdefault(fields[0].split('@', 1)[0], set()).add(fields[1])
    return symbols


def raw_syscall_sites(path):
    data, _, loads = load_summary(path)
    sites = []
    for segment in loads:
        if not segment['flags'] & PF_X:
            continue
        start = segment['offset']
        end = start + segment['file_size']
        cursor = start
        while True:
            cursor = data.find(b'\x0f\x05', cursor, end)
            if cursor < 0:
                break
            sites.append({
                'segment': segment['index'],
                'file_offset': cursor,
                'virtual_address': segment['virtual_address'] + cursor - start,
                'bytes': '0f05',
            })
            cursor += 1
    return sites


def code_audit(path, allowed_private=(), disassemble=True, scan_loads=True,
               scan_symbols=True, sce_policy=False):
    defined = nm_symbols(path, '--defined-only') if scan_symbols else {}
    forbidden = sorted(FORBIDDEN_DEFINITIONS & set(defined))
    private_symbols = sorted(
        name for name in defined if PRIVATE.search(name) and name not in allowed_private)
    data = b''
    headers = []
    if scan_loads:
        raw_sites = raw_syscall_sites(path)
        data, headers, loads = load_summary(path)
    else:
        raw_sites = []
        loads = []
    writable_executable = [
        item for item in loads if item['flags'] & PF_W and item['flags'] & PF_X
    ]
    page_flags = {}
    for item in loads:
        start = item['virtual_address'] & ~0x3fff
        end = (item['virtual_address'] + item['memory_size'] + 0x3fff) & ~0x3fff
        for page in range(start, end, 0x4000):
            page_flags[page] = page_flags.get(page, 0) | item['flags']
    writable_executable_pages = sorted(
        page for page, flags in page_flags.items()
        if flags & PF_W and flags & PF_X)
    if sce_policy:
        require(struct.unpack_from('<Q', data, 24)[0] == 0x120,
                f'{path}: unexpected native entry point')
        relro = [item for item in headers if item['type'] == PT_GNU_RELRO]
        require(len(relro) == 1 and relro[0]['flags'] == PF_R and
                relro[0]['memory_size'] > 0,
                f'{path}: missing or invalid native GNU_RELRO')
        stacks = [item for item in headers if item['type'] == PT_GNU_STACK]
        require(not stacks or (len(stacks) == 1 and not stacks[0]['flags'] & PF_X),
                f'{path}: executable or duplicate native GNU_STACK')
        require(sum(item['type'] == PT_DYNAMIC for item in headers) == 1,
                f'{path}: native DYNAMIC header drift')
    syscalls = []
    local_mmap_edges = []
    if disassemble:
        disassembly = output(
            'llvm-objdump', '--disassemble', '--no-show-raw-insn', path)
        current = None
        for line in disassembly.splitlines():
            label = re.match(r'^[0-9a-fA-F]+ <([^>]+)>:$', line.strip())
            if label:
                current = label.group(1)
                continue
            instruction = re.match(
                r'^\s*([0-9a-fA-F]+):(?:\s+[0-9a-fA-F]{2})*\s+'
                r'([A-Za-z][A-Za-z0-9.]*)\s*(.*)$', line)
            if not instruction:
                continue
            address, mnemonic, operands = instruction.groups()
            if mnemonic.lower() == 'syscall':
                syscalls.append({
                    'address': address.lower(),
                    'symbol': current or '',
                    'instruction': 'syscall',
                })
            if mnemonic.lower().startswith('call'):
                target = re.search(r'<([^>]+)>', operands)
                if target and re.fullmatch(
                        r'mmap(?:\+0x[0-9a-fA-F]+)?', target.group(1)):
                    local_mmap_edges.append({
                        'address': address.lower(),
                        'symbol': current or '',
                        'target': target.group(1),
                    })
    require(not forbidden, f'{path}: forbidden defined APIs: {forbidden}')
    require(not private_symbols, f'{path}: private defined symbols: {private_symbols}')
    require(not writable_executable, f'{path}: writable executable PT_LOAD')
    require(not writable_executable_pages,
            f'{path}: 16-KiB page permission union is writable/executable: '
            f'{[hex(page) for page in writable_executable_pages]}')
    require(not syscalls, f'{path}: disassembled syscall instructions: {syscalls}')
    require(not local_mmap_edges, f'{path}: local mmap call edges: {local_mmap_edges}')
    return {
        'defined_forbidden': forbidden,
        'private_defined': private_symbols,
        'disassembly_performed': disassemble,
        'syscall_instruction_count': len(syscalls),
        'syscall_sites': syscalls,
        'raw_syscall_site_count': len(raw_sites),
        'raw_syscall_sites': raw_sites,
        'raw_byte_sequences_require_instruction_boundary_classification': True,
        'writable_executable_16k_pages': [
            hex(page) for page in writable_executable_pages
        ],
        'local_mmap_call_edge_count': len(local_mmap_edges),
        'local_mmap_call_edges': local_mmap_edges,
        'executable_segments': [
            {
                'index': item['index'],
                'flags': item['flags_text'],
                'virtual_address': item['virtual_address'],
                'file_size': item['file_size'],
                'memory_size': item['memory_size'],
                'file_bytes_sha256': item['file_bytes_sha256'],
            }
            for item in loads if item['flags'] & PF_X
        ],
    }


def inspect_module(path, expected, allowed_private=()):
    file_header = output('llvm-readelf', '--file-header', '--wide', path)
    require(re.search(r'^\s*Type:\s+DYN\b', file_header, re.M),
            f'{path}: dedicated module is not ET_DYN')
    _, headers, loads = load_summary(path)
    unexpected = sorted({
        item['type_name'] for item in headers if item['type'] not in ALLOWED_MODULE_HEADERS
    })
    require(not unexpected, f'{path}: unsupported program headers: {unexpected}')
    require(sum(item['type'] == PT_DYNAMIC for item in headers) == 1,
            f'{path}: expected one PT_DYNAMIC')
    require(not any(item['type'] in (PT_TLS, PT_INTERP) for item in headers),
            f'{path}: TLS/interpreter program header forbidden')
    require(any(item['flags'] & PF_X for item in loads),
            f'{path}: no executable PT_LOAD')
    require(any(item['flags'] & PF_W for item in loads),
            f'{path}: no writable PT_LOAD')
    require(not any(item['flags'] & PF_W and item['flags'] & PF_X for item in loads),
            f'{path}: code and writable data are not separated')

    stacks = [item for item in headers if item['type'] == PT_GNU_STACK]
    require(len(stacks) == 1 and not stacks[0]['flags'] & PF_X,
            f'{path}: missing or executable PT_GNU_STACK')
    relro = [item for item in headers if item['type'] == PT_GNU_RELRO]
    require(len(relro) == 1 and relro[0]['file_size'] > 0 and
            relro[0]['memory_size'] >= relro[0]['file_size'],
            f'{path}: missing or empty PT_GNU_RELRO')
    relro_start = relro[0]['virtual_address']
    relro_file_end = relro_start + relro[0]['file_size']
    require(any(
        load['flags'] & PF_W and
        load['virtual_address'] <= relro_start and
        relro_file_end <= load['virtual_address'] + load['memory_size']
        for load in loads),
        f'{path}: file-backed PT_GNU_RELRO is not within a writable PT_LOAD')

    dynamic = dynamic_text(path)
    needed = re.findall(r'\(NEEDED\).*Shared library: \[([^]]+)\]', dynamic)
    sonames = re.findall(r'\(SONAME\).*Library soname: \[([^]]+)\]', dynamic)
    require(not needed, f'{path}: dedicated module has unexpected DT_NEEDED: {needed}')
    require(len(sonames) == 1, f'{path}: dedicated module must have one DT_SONAME')
    if path.name == 'win32u-title.so':
        require(sonames[0] == module_paths['win32u.so'],
                f'{path}: win32u title SONAME is not canonical')
    require(re.search(r'\((?:FLAGS|FLAGS_1)\).*NOW|\(BIND_NOW\)', dynamic),
            f'{path}: dedicated module is not bound with NOW')
    require('(TEXTREL)' not in dynamic and not re.search(r'\(FLAGS\).*TEXTREL', dynamic),
            f'{path}: text relocations are forbidden')

    relocation_text = output('llvm-readelf', '--relocations', '--wide', path)
    relocations = Counter(re.findall(r'\bR_X86_64_[A-Z0-9_]+\b', relocation_text))
    unexpected_relocations = sorted(set(relocations) - ALLOWED_RELOCATIONS)
    tls_relocations = sorted(
        name for name in relocations if 'TLS' in name or 'TPOFF' in name or 'DTP' in name)
    require(not unexpected_relocations,
            f'{path}: unsupported relocations: {unexpected_relocations}')
    require(not tls_relocations, f'{path}: TLS relocations forbidden: {tls_relocations}')

    actual = undefined_symbols(path)
    require(actual == expected, f'{path}: reviewed import/binding surface mismatch')
    all_imports = actual['strong'] | actual['weak']
    private_imports = sorted(name for name in all_imports if PRIVATE.search(name))
    require(not private_imports, f'{path}: private imports: {private_imports}')
    audit = code_audit(path, allowed_private)
    return {
        'path': display_path(path),
        'sha256': sha(path),
        'size': path.stat().st_size,
        'soname': sonames[0],
        'program_headers': [
            {
                'index': item['index'],
                'type': item['type_name'],
                'flags': flags_text(item['flags']),
                'file_size': item['file_size'],
                'memory_size': item['memory_size'],
            }
            for item in headers
        ],
        'needed': needed,
        'relocations': dict(sorted(relocations.items())),
        'strong_undefined': sorted(actual['strong']),
        'weak_undefined': sorted(actual['weak']),
        'code_audit': audit,
    }


def encode_provider_id(identifier):
    if identifier == 0:
        return 'A'
    result = ''
    while identifier:
        result = PROVIDER_ID_ALPHABET[identifier % 64] + result
        identifier //= 64
    return result


def symbol_nid(name):
    digest = hashlib.sha1(name.encode() + NID_SUFFIX).digest()[:8][::-1]
    return base64.b64encode(digest).decode()[:11].replace('/', '-')


def converted_provider_surface(pre_path, post_path, *, parent=False):
    pre_needed = needed_libraries(pre_path)
    post_needed = needed_libraries(post_path)
    require(pre_needed == (EXPECTED_PARENT_PRE_NEEDED if parent else EXPECTED_PRE_NEEDED),
            f'{pre_path}: native NEEDED drift: {pre_needed}')
    require(post_needed == (EXPECTED_PARENT_NEEDED if parent else EXPECTED_POST_NEEDED),
            f'{post_path}: converted NEEDED drift: {post_needed}')
    pre_records = undefined_symbol_records(pre_path)
    post_records = undefined_symbol_records(post_path)
    by_nid = {}
    for record in pre_records:
        nid = symbol_nid(record['name'])
        require(nid not in by_nid, f'{pre_path}: NID collision for {record["name"]}')
        by_nid[nid] = record
    expected_suffixes = {
        f'{encode_provider_id(index)}#{encode_provider_id(index + 1)}': provider
        for index, provider in enumerate(post_needed)
    }
    converted = []
    seen_plain = set()
    for post in post_records:
        match = re.fullmatch(
            r'([-+A-Za-z0-9]{11})#([-+A-Za-z0-9]+)#([-+A-Za-z0-9]+)',
            post['name'])
        require(match, f'{post_path}: malformed converted import {post["name"]}')
        nid, library_id, module_id = match.groups()
        require(nid in by_nid, f'{post_path}: converted import has no pre-conversion owner: {nid}')
        plain = by_nid[nid]
        require(plain['name'] not in seen_plain,
                f'{post_path}: duplicate converted owner for {plain["name"]}')
        seen_plain.add(plain['name'])
        suffix = f'{library_id}#{module_id}'
        require(suffix in expected_suffixes,
                f'{post_path}: unknown provider suffix for {plain["name"]}: {suffix}')
        require(
            (post['binding'], post['type'], post['visibility'], post['size']) ==
            (plain['binding'], plain['type'], plain['visibility'], plain['size']),
            f'{post_path}: converted symbol metadata drift for {plain["name"]}')
        converted.append({
            'symbol': plain['name'],
            'binding': plain['binding'],
            'type': plain['type'],
            'size': plain['size'],
            'nid': nid,
            'encoded_name': post['name'],
            'provider': expected_suffixes[suffix],
            'provider_suffix': f'#{suffix}',
        })
    require(seen_plain == {item['name'] for item in pre_records},
            f'{post_path}: pre/post native import surface is not one-to-one')
    return {
        'pre_needed': pre_needed,
        'post_needed': post_needed,
        'imports': sorted(converted, key=lambda item: item['symbol']),
    }


def compare_pt_loads(reference_path, extracted_path):
    reference_data, reference_headers, reference_loads = load_summary(reference_path)
    extracted_data, extracted_headers, extracted_loads = load_summary(extracted_path)
    reference_entry = struct.unpack_from('<Q', reference_data, 24)[0]
    extracted_entry = struct.unpack_from('<Q', extracted_data, 24)[0]
    require(reference_entry == extracted_entry,
            'final FSELF entry point differs from patched ELF')
    require(reference_headers == extracted_headers,
            'final FSELF security-relevant program headers differ from patched ELF')
    require(len(reference_loads) == len(extracted_loads),
            'final FSELF PT_LOAD count differs from patched ELF')
    proof = []
    for reference, extracted in zip(reference_loads, extracted_loads):
        fields = (
            'flags', 'virtual_address', 'physical_address', 'file_size',
            'memory_size', 'alignment',
        )
        require(all(reference[field] == extracted[field] for field in fields),
                f'final FSELF PT_LOAD metadata differs at segment {reference["index"]}')
        reference_bytes = reference_data[
            reference['offset']:reference['offset'] + reference['file_size']]
        extracted_bytes = extracted_data[
            extracted['offset']:extracted['offset'] + extracted['file_size']]
        require(reference_bytes == extracted_bytes,
                f'final FSELF PT_LOAD bytes differ at segment {reference["index"]}')
        proof.append({
            'index': reference['index'],
            'flags': reference['flags_text'],
            'virtual_address': reference['virtual_address'],
            'file_size': reference['file_size'],
            'memory_size': reference['memory_size'],
            'bytes_sha256': hashlib.sha256(reference_bytes).hexdigest(),
        })
    return {
        'reference': display_path(reference_path),
        'extracted': display_path(extracted_path),
        'whole_file_equal': reference_data == extracted_data,
        'entry_point': reference_entry,
        'program_headers_sha256': hashlib.sha256(json.dumps(
            reference_headers, sort_keys=True, separators=(',', ':')).encode()).hexdigest(),
        'segments': proof,
    }


def tree_inventory(root):
    require(root.is_dir() and not root.is_symlink(),
            f'publication source is not a real directory: {root}')
    files = []
    for path in sorted(root.rglob('*')):
        mode = path.lstat().st_mode
        require(not stat.S_ISLNK(mode), f'publication source contains symlink: {path}')
        if stat.S_ISDIR(mode):
            continue
        require(stat.S_ISREG(mode), f'publication source contains non-file: {path}')
        files.append({
            'path': path.relative_to(root).as_posix(),
            'size': path.stat().st_size,
            'sha256': sha(path),
        })
    return files


def remove_owned_tree(path):
    if path.is_symlink() or path.is_file():
        path.unlink()
    elif path.exists():
        shutil.rmtree(path)


def publish_runtime(receipt):
    regular(parent_title / 'eboot.bin', 'quarantined parent FSELF')
    regular(parent_title / 'sce_sys/param.json', 'quarantined parent parameter file')
    regular(parent_title / 'sce_module/libc.prx', 'quarantined parent libc')
    reserved = {
        Path('psmkit-wine-runtime-child.bin'),
        Path('runtime-qualification.json'),
        *(Path(path.removeprefix('/app0/')) for path in module_paths.values()),
    }
    for relative in reserved:
        require(not (parent_title / relative).exists(),
                f'parent quarantine contains reserved publication path: {relative}')
    stage = quarantine / 'publication-ready'
    previous = quarantine / 'publication-previous'
    remove_owned_tree(stage)
    remove_owned_tree(previous)
    shutil.copytree(parent_title, stage)
    require(tree_inventory(stage) == receipt['material']['parent_title'],
            'parent title changed after qualification inventory')
    child_destination = stage / 'psmkit-wine-runtime-child.bin'
    child_destination.parent.mkdir(parents=True, exist_ok=True)
    expected_child_sha = receipt['material']['native_child']['fself_sha256']
    require(sha(child_fself) == expected_child_sha,
            'child FSELF changed after final qualification')
    shutil.copy2(child_fself, child_destination)
    require(sha(child_destination) == expected_child_sha,
            'publication child copy differs from qualified FSELF')
    published_modules = {}
    if args.profile == 'b1':
        source_tree = quarantine / 'wine-title'
        require(tree_inventory(source_tree) == receipt['material']['title_root'],
                'B1 runtime tree changed after qualification')
        shutil.copytree(source_tree, stage / 'wine-title')
    for identity in generated_manifest['modules']:
        source = dedicated[identity['name']]
        relative = Path(identity['path'].removeprefix('/app0/'))
        require(not relative.is_absolute() and '..' not in relative.parts,
                f'unsafe canonical module publication path: {identity["path"]}')
        destination = stage / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        if args.profile != 'b1':
            shutil.copy2(source, destination)
        require(sha(destination) == identity['sha256'] and
                destination.stat().st_size == identity['size'],
                f'publication copy drift for {identity["name"]}')
        published_modules[identity['name']] = relative.as_posix()
    atomic_json(stage / 'runtime-qualification.json', receipt)
    for identity in receipt['material']['test_pes']:
        source = args.pe_dir.resolve() / identity['name']
        require(sha(source) == identity['sha256'], 'test PE changed after qualification')
        destination = stage / identity['path']
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        require(sha(destination) == identity['sha256'], 'published test PE identity drift')

    publish_out.parent.mkdir(parents=True, exist_ok=True)
    require(not publish_out.is_symlink() and
            (not publish_out.exists() or publish_out.is_dir()),
            f'publication destination has unsafe type: {publish_out}')
    had_previous = publish_out.exists()
    if had_previous:
        os.replace(publish_out, previous)
    try:
        os.replace(stage, publish_out)
    except BaseException:
        if had_previous and previous.exists() and not publish_out.exists():
            os.replace(previous, publish_out)
        raise
    remove_owned_tree(previous)
    return {
        'root': display_path(publish_out),
        'child': 'psmkit-wine-runtime-child.bin',
        'modules': published_modules,
        'receipt': 'runtime-qualification.json',
    }


policy_sha256 = sha(policy_path)
try:
    policy_rows = load_policy(policy_path, profile_name=args.profile)
except ValueError as error:
    raise SystemExit(str(error)) from error

build_manifest_path = work / 'title-runtime-build.json'
build_manifest = read_json(build_manifest_path, 'runtime build manifest')
require(build_manifest.get('format') == 2, 'unsupported runtime build manifest')
require(build_manifest.get('policy', {}).get('sha256') == policy_sha256,
        'runtime build did not use the current reviewed import policy')
require(build_manifest.get('feature_macro') == 'WINE_PS5_TITLE',
        'runtime build did not select the title WoW64 implementation')
require(build_manifest.get('profile') == selected,
        'runtime build profile differs from qualifier profile')
payload_identities = {}
if args.profile == 'b1':
    for key, label, relative, maximum in (
            ('writebroker', 'write broker', WRITEBROKER_RELATIVE, WRITEBROKER_MAX_BYTES),
            ('addon_worker', 'add-on worker', ADDON_WORKER_RELATIVE, ADDON_WORKER_MAX_BYTES)):
        path = quarantine / 'wine-title' / relative
        regular(path, f'B1 {label} data ELF')
        data = path.read_bytes()
        require(data.startswith(b'\x7fELF') and 0 < len(data) <= maximum,
                f'B1 {label} data ELF is invalid or oversized')
        identity = {
            'path': relative,
            'app_path': selected['root'] + '/' + relative,
            'classification': 'data_only_not_loaded',
            'size': len(data),
            'sha256': hashlib.sha256(data).hexdigest(),
        }
        payload_identities[key] = identity
        require(build_manifest.get(key) == identity, f'B1 {label} build identity drift')
else:
    require('writebroker' not in build_manifest and 'addon_worker' not in build_manifest,
            'non-B1 runtime unexpectedly carries payload helper data')
if args.profile == 'b1':
    variants = build_manifest.get('variant_compiles', {})
    require(f'-DWINE_PS5_TITLE_ROOT={json.dumps(selected["root"])}' in variants.get('server', []),
            'B1 server did not select the separate runtime root')
    require('-DWINE_PS5_TITLE_PARENT_CWD=({const char *cwd=getenv("WINEPS5_TITLE_PARENT_CWD"); if (!cwd) abort(); cwd;})'
            in variants.get('env', []),
            'B1 parent did not select the trusted native working directory')
    for stem in ('driver', 'winstation'):
        require('-DWINE_PS5_TITLE_B1' in variants.get('win32u_' + stem, []),
                f'B1 win32u {stem} did not select title child routing')
    require('-DWINE_PS5_TITLE_B1_PARENT' in build_manifest.get('parent_loader_compile', []),
            'B1 parent loader does not wait for the complete PsmKit session')
    desktop = build_manifest.get('desktop', {})
    require(desktop.get('enabled') == (not selected['headless']),
            'B1 native desktop selection differs from the profile')
    expected_desktop_sources = {
        'title-desktop.c', 'title-hub.c', 'compositor.c', 'forever-menu.c',
        'addonforge-client.c', 'display.c', 'input.c', 'pointer.c', 'ime.c',
    } if not selected['headless'] else set()
    require({Path(path).name for path in desktop.get('sources', [])} == expected_desktop_sources,
            'B1 native desktop source inventory drift')
gpu = build_manifest.get('gpu', {})
require(gpu.get('enabled') == selected.get('gpu', False),
        'native GPU selection differs from the profile')
require({Path(path).name for path in gpu.get('parent_sources', [])} ==
        ({'runtime-vulkan.c', 'runtime-input.c', 'input.c', 'pointer.c', 'ime.c', 'voice-state.c',
          'voice-overlay-raster.c', 'voice-overlay-vulkan.c'}
         if selected.get('gpu', False) else set()),
        'native GPU parent source inventory drift')
expected_radv = (Path(os.environ.get('PS5_RADV_INSTALL', str(here / '../../build/radv-release-patched'))) /
                 'lib/libvulkan_radeon.ps5.a').resolve()
require(gpu.get('parent_archive') == (str(expected_radv) if selected.get('gpu', False) else None),
        'native GPU parent archive drift')
if selected.get('gpu'):
    # These imports belong only to the HDR patch, not stock/mailbox RADV.
    # Keep the exact parent surface, deriving their presence from the archive linked.
    radv_undefined = output('llvm-nm', '--undefined-only', expected_radv).split()
    for name in ('rename', 'unlink', 'sceVideoOutModeSetAny_',
                 'sceVideoOutConfigureOptionsInitialize_', 'sceVideoOutConfigureOutputModeEx_',
                 'sceVideoOutGetCurrentOutputMode_', 'sceVideoOutGetOutputStatus',
                 'sceVideoOutSubmitChangeBufferAttribute2'):
        if name not in radv_undefined:
            native_roles['parent'].pop(name)
base_parameter = read_json(here / '../../title/sce_sys/param.json', 'source title parameters')
expected_parameter = dict(base_parameter)
if selected.get('gpu', False):
    expected_parameter['attribute'] = expected_parameter.get('attribute', 0) | 0x20000000
    # Keep system VRR enabled while retaining the high-frame-rate declaration.
    expected_parameter['attribute3'] = (
        expected_parameter.get('attribute3', 0) & ~0x80000) | 0x40040
require(read_json(parent_title / 'sce_sys/param.json', 'parent parameters') == expected_parameter,
        'native parent VRR/HDR attribute/profile mismatch')
signal_compile = build_manifest.get('signal_compile')
require(isinstance(signal_compile, list) and '-DWINE_PS5_TITLE' in signal_compile and
        '-DWINE_PS5_TITLE_X64_ONLY' not in signal_compile,
        'runtime signal object was not compiled for title WoW64')

candidate_path = work / 'title-imports-candidate.json'
candidate_manifest = read_json(candidate_path, 'candidate import manifest')
dedicated = {
    'ntdll.so': work / 'ntdll-title.so',
    'wineserver.so': work / 'wineserver-title.so',
    'win32u.so': work / 'win32u-title.so',
}
extra_modules = sorted(set(module_paths) - {'ntdll.so', 'wineserver.so', 'win32u.so'})
dedicated.update({name: work / (name.removesuffix('.so') + '-title.so') for name in extra_modules})
gpu_modules = selected.get('gpu', False)
require(candidate_manifest == make_candidate_manifest(
    {name: undefined_symbols(path) for name, path in dedicated.items()},
    profile_name=args.profile, gpu=gpu_modules), 'candidate import manifest differs from current dedicated modules')
require(build_manifest.get('candidate_manifest', {}).get('sha256') == sha(candidate_path),
        'runtime build candidate-manifest hash mismatch')

manifest_path = work / 'title-imports.json'
generated_manifest = read_json(manifest_path, 'generated import manifest')
include_path = work / 'title-imports.inc'
regular(include_path, 'generated import include')
require(generated_manifest.get('format') == 2, 'unsupported generated import manifest')
require(generated_manifest.get('policy_sha256') == policy_sha256,
        'generated imports are not bound to reviewed policy')
require(generated_manifest.get('include_sha256') == sha(include_path),
        'generated import include hash mismatch')
require(build_manifest.get('imports_manifest', {}).get('sha256') == sha(manifest_path),
        'runtime build import-manifest hash mismatch')
require(build_manifest.get('imports_manifest', {}).get('include_sha256') == sha(include_path),
        'runtime build import-include hash mismatch')

module_symbols = {name: undefined_symbols(path) for name, path in dedicated.items()}
try:
    records = make_records(module_symbols, policy_rows, profile_name=args.profile, gpu=gpu_modules)
except ValueError as error:
    raise SystemExit(str(error)) from error
require(generated_manifest.get('imports') == records,
        'generated imports differ from reviewed exact module union')
require(discovered_rows(module_symbols, profile_name=args.profile, gpu=gpu_modules) == candidate_manifest['imports'],
        'candidate manifest surface is not exact')
require(records == sorted(records, key=lambda item: item['name']),
        'generated imports are not deterministic')
require(len(records) == len({item['name'] for item in records}),
        'duplicate generated import')

module_identities = generated_manifest.get('modules')
require(isinstance(module_identities, list) and
        len(module_identities) == len(module_names(args.profile, gpu_modules)),
        'generated title module manifest count differs from the exact profile allow-list')
require([item.get('name') for item in module_identities] == list(module_names(args.profile, gpu_modules)),
        'generated title module identities are incomplete or out of order')
for identity in module_identities:
    require(identity.get('name') in dedicated, 'unknown generated title module identity')
    require(identity.get('path') == module_paths[identity['name']],
            f'generated module path is not canonical for {identity["name"]}')
    path = dedicated[identity['name']]
    regular(path, f'dedicated {identity["name"]}')
    require(identity.get('sha256') == sha(path) and
            identity.get('size') == path.stat().st_size,
            f'generated module identity drift for {identity["name"]}')
    require(build_manifest.get(
        identity['name'].removesuffix('.so'), {}
    ).get('identity') == identity,
            f'build manifest identity drift for {identity["name"]}')

expected_by_module = {}
for module in dedicated:
    expected_by_module[module] = {
        'strong': {
            item['name'] for item in records
            if item['module_bindings'].get(module) == 'strong'
        },
        'weak': {
            item['name'] for item in records
            if item['module_bindings'].get(module) == 'weak'
        },
    }
modules = [
    inspect_module(
        path, expected_by_module[name],
        {'kernel_stack_size'} if name == 'ntdll.so' else set())
    for name, path in dedicated.items()
]
signal_exports = []
for line in output('llvm-readelf', '--dyn-symbols', '--wide', dedicated['ntdll.so']).splitlines():
    fields = line.split()
    if len(fields) >= 8 and fields[7] == '__wine_ps5_signal_thread':
        signal_exports.append(fields)
require(len(signal_exports) == 1 and signal_exports[0][3:6] == ['FUNC', 'GLOBAL', 'DEFAULT'] and
        signal_exports[0][6] != 'UND', 'ntdll is missing the public child signal-agent export')

native = work / 'llvm-pie.elf'
converted = work / 'eboot.elf'
patched = work / 'localproc.elf'
signal_object = work / 'signal_x86_64-title.o'
child_fself = child_out / 'eboot.bin'
for path, description in (
        (native, 'pre-conversion native ELF'),
        (converted, 'converted native ELF'),
        (patched, 'patched native ELF'),
        (signal_object, 'title signal object'),
        (child_fself, 'final quarantined child FSELF')):
    regular(path, description)
native_manifest = build_manifest.get('native_child', {})
for key, path in (
        ('llvm_pie', native), ('converted_elf', converted),
        ('patched_elf', patched), ('fself', child_fself)):
    require(native_manifest.get(key, {}).get('sha256') == sha(path),
            f'build manifest hash drift for {key}')

fself_inspection = subprocess.check_output(
    [str(native_tool), 'self', '--inspect', '--file', str(child_fself)], text=True)
require('container: signed, plaintext' in fself_inspection and
        'integrity: valid' in fself_inspection,
        'final child FSELF is not a valid signed plaintext container')
extracted = work / 'final-fself-extracted.elf'
extracted.unlink(missing_ok=True)
subprocess.run(
    [str(native_tool), 'self', '--extract', '--file', str(child_fself),
     '--out', str(extracted)],
    check=True)
regular(extracted, 'extracted final child FSELF payload')
load_proof = compare_pt_loads(patched, extracted)
parent_fself = parent_title / 'eboot.bin'
parent_native = quarantine / 'parent-work/llvm-pie.elf'
parent_reference = quarantine / 'parent-work/eboot.elf'
parent_extracted = work / 'parent-fself-extracted.elf'
regular(parent_fself, 'quarantined parent FSELF')
regular(parent_reference, 'quarantined parent reference ELF')
regular(parent_native, 'quarantined parent pre-conversion ELF')
for key, path in (('llvm_pie', parent_native), ('converted_elf', parent_reference),
                  ('fself', parent_fself)):
    require(build_manifest.get('native_parent', {}).get(key, {}).get('sha256') == sha(path),
            f'parent build manifest hash drift for {key}')
parent_inspection = subprocess.check_output(
    [str(native_tool), 'self', '--inspect', '--file', str(parent_fself)], text=True)
require('container: signed, plaintext' in parent_inspection and
        'integrity: valid' in parent_inspection,
        'parent FSELF is not a valid signed plaintext container')
parent_extracted.unlink(missing_ok=True)
subprocess.run(
    [str(native_tool), 'self', '--extract', '--file', str(parent_fself),
     '--out', str(parent_extracted)],
    check=True)
regular(parent_extracted, 'extracted parent FSELF payload')
parent_load_proof = compare_pt_loads(parent_reference, parent_extracted)
require(needed_libraries(parent_reference) == EXPECTED_PARENT_NEEDED and
        needed_libraries(parent_extracted) == EXPECTED_PARENT_NEEDED,
        'parent FSELF provider surface drift')
parent_audit = code_audit(
    parent_extracted, scan_symbols=False, sce_policy=True)

parent_native_audit = code_audit(parent_native)
parent_surface = converted_provider_surface(parent_native, parent_reference, parent=True)
require(converted_provider_surface(parent_native, parent_extracted, parent=True) == parent_surface,
        'parent extracted provider surface differs from reference')
actual = {item['symbol']: item['provider'] for item in parent_surface['imports']
          if item['provider'] in NATIVE_ROLE_PROVIDERS or item['symbol'] in native_roles['parent']}
require(actual == native_roles['parent'], 'parent: native role import ownership drift')

pre_post_surface = converted_provider_surface(native, converted)
patched_surface = converted_provider_surface(native, patched)
final_surface = converted_provider_surface(native, extracted)
actual = {item['symbol']: item['provider'] for item in pre_post_surface['imports']
          if item['provider'] in NATIVE_ROLE_PROVIDERS}
require(actual == native_roles['child'], 'child: native role import ownership drift')
if args.profile == 'b1':
    require(all(native_roles['parent'].get(name) == 'libkernel.prx'
                for name in ('connect', 'setsockopt', 'shutdown')),
            'B1 payload transports are not explicitly reviewed for the parent')
require(patched_surface == pre_post_surface,
        'patched ELF NEEDED/provider surface differs from converted ELF')
require(final_surface == patched_surface,
        'final FSELF NEEDED/provider surface differs after extraction')

native_symbols = nm_symbols(native)
native_private = sorted(name for name in native_symbols if PRIVATE.search(name))
require(not native_private, f'native title contains private symbols: {native_private}')
native_undefined_records = undefined_symbol_records(native)
native_undefined = {item['name'] for item in native_undefined_records}
provider_by_symbol = {
    item['symbol']: item for item in pre_post_surface['imports']
}
bridge_proof = []
module_exports = {
    name: {line.split()[-1] for line in output('llvm-nm', '-D', '--defined-only', path).splitlines()
           if line.split()} for name, path in dedicated.items()
}
for item in records:
    target = item['target']
    if item['owner'] in {'title_ntdll', 'title_win32u'}:
        provider_module = 'ntdll.so' if item['owner'] == 'title_ntdll' else 'win32u.so'
        require(target in module_exports[provider_module],
                f'{provider_module} does not export reviewed import {target}')
        bridge_proof.append({
            'import': item['name'], 'owner': item['owner'], 'target': target,
            'native_symbol_codes': [], 'resolution': 'admitted_module_export',
            'provider': provider_module,
        })
        continue
    codes = sorted(native_symbols.get(target, set()))
    is_defined = any(code not in UNDEFINED_NM_CODES for code in codes)
    is_undefined = target in native_undefined
    require(is_defined or is_undefined,
            f'native title has no exact bridge target {target} for {item["name"]}')
    require(not (is_defined and is_undefined),
            f'native title has ambiguous defined/undefined ownership for {target}')
    if item['owner'] in DEFINED_BRIDGE_OWNERS:
        require(is_defined,
                f'native bridge owner {item["owner"]} does not define {target}')
    provider = None
    resolution = 'native_definition'
    if is_undefined:
        require(target in provider_by_symbol,
                f'native undefined bridge target has no converted provider: {target}')
        provider = provider_by_symbol[target]['provider']
        resolution = 'converted_public_provider'
    bridge_proof.append({
        'import': item['name'],
        'owner': item['owner'],
        'target': target,
        'native_symbol_codes': codes,
        'resolution': resolution,
        'provider': provider,
    })
parent_symbols = nm_symbols(parent_native)
require(not any(PRIVATE.search(name) for name in parent_symbols),
        'parent runtime contains private symbols')
parent_undefined = {item['symbol']: item['provider'] for item in parent_surface['imports']}
parent_bridge_proof = []
# RADV's existing link recipe binds these libc names to title-safe platform
# definitions. Children and non-GPU parents keep their reviewed public imports.
gpu_local_bridges = {'fstatvfs': 'ps5_fstatvfs', 'localeconv': 'ps5_localeconv'} if selected.get('gpu') else {}
gpu_symbol_addresses = {
    fields[0]: fields[2]
    for line in (output('llvm-nm', '--format=posix', '--defined-only', parent_native).splitlines()
                 if gpu_local_bridges else [])
    if len(fields := line.split()) >= 3
}
for proof in bridge_proof:
    target = proof['target']
    if proof['resolution'] == 'admitted_module_export':
        parent_bridge_proof.append(dict(proof))
        continue
    codes = sorted(parent_symbols.get(target, set()))
    defined = any(code not in UNDEFINED_NM_CODES for code in codes)
    if proof['resolution'] == 'native_definition':
        require(defined and target not in parent_undefined,
                f'parent bridge missing native definition: {target}')
    elif target in gpu_local_bridges:
        platform_target = gpu_local_bridges[target]
        require(defined and target not in parent_undefined and
                target in gpu_symbol_addresses and
                gpu_symbol_addresses[target] == gpu_symbol_addresses.get(platform_target),
                f'GPU parent platform bridge alias drift: {target}')
        proof = dict(proof, resolution='native_definition', provider=None,
                     platform_target=platform_target)
    else:
        require(not defined and parent_undefined.get(target) == proof['provider'],
                f'parent bridge provider drift: {target}')
    parent_bridge_proof.append(dict(proof, native_symbol_codes=codes))

native_audit = code_audit(native)
converted_audit = code_audit(
    converted, disassemble=False, scan_symbols=False, sce_policy=True)
patched_audit = code_audit(
    patched, disassemble=False, scan_symbols=False, sce_policy=True)
final_audit = code_audit(
    extracted, disassemble=False, scan_symbols=False, sce_policy=True)
signal_audit = code_audit(signal_object, scan_loads=False)

host_test = args.host_test.resolve()
regular(host_test, 'staged-runtime host test')
host_out.mkdir(parents=True, exist_ok=True)
import_file = host_out / 'explicit-import-names.txt'
import_file.write_text('\n'.join(item['name'] for item in records
                                 if item['owner'] not in {'title_ntdll', 'title_win32u'}) + '\n')
result = subprocess.run(
    [str(host_test), str(import_file), str(dedicated['ntdll.so']),
     str(dedicated['wineserver.so']), str(dedicated['win32u.so']),
     *(str(dedicated[name]) for name in extra_modules)],
    text=True, capture_output=True)
print(result.stdout, end='')
print(result.stderr, end='')
result.check_returncode()

audit_report = {
    'format': 2,
    'artifacts': {
        item['path']: item['code_audit'] for item in modules
    } | {
        display_path(signal_object): signal_audit,
        display_path(native): native_audit,
        display_path(converted): converted_audit,
        display_path(patched): patched_audit,
        display_path(extracted): final_audit,
        display_path(parent_extracted): parent_audit,
        display_path(parent_native): parent_native_audit,
    },
}
audit_file = host_out / 'defined-code-audit.json'
atomic_json(audit_file, audit_report)

require(sha(parent_title / 'sce_module/libc.prx') == EXPECTED_TITLE_LIBC_SHA256,
        'quarantined parent libc differs from reviewed signed title libc')
parent_inventory = tree_inventory(parent_title)
test_pes = []
pe_cases = () if args.profile == 'b1' else (
    ('console.exe', 'psmkit-wine-console.exe', 0x8664, 0x20b),
    ('p1w-parent.exe', 'psmkit-wine-parent.exe', 0x014c, 0x10b),
    ('p1w-child.exe', 'psmkit-wine-child.exe', 0x014c, 0x10b))
for name, leaf, machine, magic in pe_cases:
    pe = args.pe_dir.resolve() / name
    regular(pe, f'P1w test PE {name}')
    data = pe.read_bytes()
    require(len(data) >= 64 and data[:2] == b'MZ', f'{pe}: missing DOS header')
    offset = struct.unpack_from('<I', data, 60)[0]
    require(offset + 26 <= len(data) and data[offset:offset + 4] == b'PE\0\0' and
            struct.unpack_from('<H', data, offset + 4)[0] == machine and
            struct.unpack_from('<H', data, offset + 24)[0] == magic,
            f'{pe}: wrong PE machine/optional-header magic')
    test_pes.append({'name': name, 'path': 'wine/psmkit-wine-disposable/prefix/drive_c/' + leaf,
                     'sha256': sha(pe), 'size': pe.stat().st_size})
title_root_inventory = None
if args.profile == 'b1':
    tree = quarantine / 'wine-title'
    root_manifest = read_json(tree / 'title-root-manifest.json', 'B1 runtime tree manifest')
    require(root_manifest == runtime_tree_manifest(tree, selected),
            'B1 runtime tree differs from its manifest')
    title_root_inventory = tree_inventory(tree)
native_resolver_audit = None
if args.profile == 'b1':
    native_resolver_audit = code_audit(work / 'safe-libc/netdb.o', scan_loads=False)
qualification_material = {
    'format': 1,
    'topology': 'PARENT_SHARED_INPROCESS_WINESERVER',
    'runtime_abi': 3,
    'profile': selected,
    **payload_identities,
    'title_root': title_root_inventory,
    'native_resolver_audit': native_resolver_audit,
    'test_pes': test_pes,
    'qualifier_sources': {
        'inspect-runtime.py': sha(Path(__file__).resolve()),
        'runtime_imports.py': sha(here / 'runtime_imports.py'),
        policy_path.name: policy_sha256,
        'runtime_profiles.py': sha(here / 'runtime_profiles.py'),
        'host_test': sha(host_test),
    },
    'build_manifest_sha256': sha(build_manifest_path),
    'candidate_manifest_sha256': sha(candidate_path),
    'generated_manifest_sha256': sha(manifest_path),
    'generated_include_sha256': sha(include_path),
    'modules': module_identities,
    'native_child': {
        'llvm_pie_sha256': sha(native),
        'converted_elf_sha256': sha(converted),
        'patched_elf_sha256': sha(patched),
        'extracted_fself_payload_sha256': sha(extracted),
        'fself_sha256': sha(child_fself),
        'fself_size': child_fself.stat().st_size,
    },
    'native_parent': {
        'reference_elf_sha256': sha(parent_reference),
        'llvm_pie_sha256': sha(parent_native),
        'provider_surface': parent_surface,
        'extracted_fself_payload_sha256': sha(parent_extracted),
        'fself_sha256': sha(parent_fself),
        'fself_size': parent_fself.stat().st_size,
        'needed': EXPECTED_PARENT_NEEDED,
        'pt_load_proof_sha256': hashlib.sha256(json.dumps(
            parent_load_proof, sort_keys=True, separators=(',', ':')).encode()).hexdigest(),
    },
    'parent_title': parent_inventory,
    'provider_surface_sha256': hashlib.sha256(json.dumps(
        final_surface, sort_keys=True, separators=(',', ':')).encode()).hexdigest(),
    'pt_load_proof_sha256': hashlib.sha256(json.dumps(
        load_proof, sort_keys=True, separators=(',', ':')).encode()).hexdigest(),
    'defined_code_audit_sha256': sha(audit_file),
    'relocation_proof_sha256': hashlib.sha256(result.stdout.encode()).hexdigest(),
}
qualification_key = hashlib.sha256(json.dumps(
    qualification_material, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
ownership_counts = Counter(item['owner'] for item in records)
receipt = {
    'format': 3,
    'qualification_key': qualification_key,
    'status': 'QUALIFIED_STATIC_NOT_EXECUTED',
    'execution': 'NOT_ENTERED_HOST',
    'route': 'PUBLIC_USERSPACE_ELF_LOADER',
    'feature_macro': 'WINE_PS5_TITLE',
    'material': qualification_material,
    'modules': modules,
    'native_child': {
        'fself_inspection': fself_inspection.rstrip().splitlines(),
        'pt_load_identity': load_proof,
        'pre_post_provider_surface': final_surface,
        'private_symbols': native_private,
    },
    'native_parent': {
        'fself_inspection': parent_inspection.rstrip().splitlines(),
        'pt_load_identity': parent_load_proof,
        'needed': EXPECTED_PARENT_NEEDED,
    },
    'parent_import_ownership': parent_bridge_proof,
    'imports': {
        'count': len(records),
        'policy_sha256': policy_sha256,
        'ownership_counts': dict(sorted(ownership_counts.items())),
        'ownership': bridge_proof,
    },
    'relocation_proof': result.stdout.rstrip(),
}
receipt_dir = host_out / 'runtime-qualifications'
receipt_file = receipt_dir / f'{qualification_key}.json'
atomic_json(receipt_file, receipt)
inventory_file = host_out / 'runtime-inventory.json'
atomic_json(inventory_file, receipt)
publication = publish_runtime(receipt)
print(f'RUNTIME_TITLE_QUALIFIED key={qualification_key} imports={len(records)} '
      f'raw_syscalls=0 profile={args.profile} published={publication["root"]} receipt={receipt_file}')
