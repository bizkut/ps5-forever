#!/usr/bin/env python3
"""Host semantic tests for deny-by-default title import ownership."""
import argparse
import copy
from pathlib import Path
import shlex
import subprocess
import tempfile

from runtime_imports import (
    NATIVE_ROLE_IMPORTS, candidate_ownership, load_policy, make_records, render_include,
)

parser = argparse.ArgumentParser()
parser.add_argument('--cc', default='cc')
args = parser.parse_args()
here = Path(__file__).resolve().parent
policy_path = here / 'runtime-import-policy.json'
policy = load_policy(policy_path)

module_symbols = {
    'ntdll.so': {'strong': set(), 'weak': set()},
    'wineserver.so': {'strong': set(), 'weak': set()},
    'win32u.so': {'strong': set(), 'weak': set()},
}
for row in policy:
    module_symbols[row['module']][row['binding']].add(row['symbol'])
records = make_records(module_symbols, policy)
if len(records) != len({row['symbol'] for row in policy}):
    raise SystemExit('reviewed policy did not merge deterministically')

expected_mappings = {
    'dlopen': ('custom_loader', 'pw_public_dlopen'),
    'dlsym': ('custom_loader', 'pw_public_dlsym'),
    'dlclose': ('custom_loader', 'pw_public_dlclose'),
    'dlerror': ('custom_loader', 'pw_public_dlerror'),
    'dladdr': ('custom_loader', 'pw_public_dladdr'),
    '__real_pthread_key_create': ('native_pthread', 'pthread_key_create'),
    'syscall': ('fail_closed', 'pw_native_fail_syscall'),
    'fork': ('fail_closed', 'pw_native_fail_fork'),
    'setsid': ('fail_closed', 'pw_native_fail_setsid'),
    '__tls_get_addr': ('custom_tls', 'pw_native_tls_get_addr'),
    'extattr_get_fd': ('fail_closed', 'pw_native_fail_minus_one'),
    'dup': ('custom_fd', 'pw_native_dup_fd'),
    'posix_fadvise': ('fail_closed', 'pw_native_fail_posix_error'),
    'if_nametoindex': ('fail_closed', 'pw_native_fail_if_nametoindex'),
    'getmntinfo': ('fail_closed', 'pw_native_fail_getmntinfo'),
    'futimens': ('platform', 'ps5_futimens'),
    'posix_fallocate': ('platform', 'ps5_posix_fallocate'),
    'realpath': ('platform', 'ps5_realpath'),
    'umask': ('platform', 'ps5_umask'),
    'isatty': ('platform', 'ps5_isatty'),
    'link': ('platform', 'ps5_link'),
    'execv': ('fail_closed', 'pw_native_fail_minus_one'),
    'fchdir': ('fail_closed', 'pw_native_fail_minus_one'),
    'openat': ('platform', 'ps5_openat'),
    'fstatat': ('platform', 'ps5_fstatat'),
    'fstatfs': ('platform', 'ps5_fstatfs'),
    'readlink': ('platform', 'ps5_readlink'),
    'symlink': ('platform', 'ps5_symlink'),
}
for symbol, expected in expected_mappings.items():
    if candidate_ownership(symbol) != expected:
        raise SystemExit(f'explicit ownership mapping drift for {symbol}')
if candidate_ownership('future_secret_runtime_api') != (
        'UNREVIEWED_NOT_AUTHORIZED', 'future_secret_runtime_api'):
    raise SystemExit('unknown import received permissive candidate ownership')


def rejected(symbols, rows, description):
    try:
        make_records(symbols, rows)
    except ValueError:
        return
    raise SystemExit(f'import policy unexpectedly accepted {description}')


added = copy.deepcopy(module_symbols)
added['ntdll.so']['strong'].add('future_secret_runtime_api')
rejected(added, policy, 'an added symbol')

removed = copy.deepcopy(module_symbols)
removed_row = policy[0]
removed[removed_row['module']][removed_row['binding']].remove(removed_row['symbol'])
rejected(removed, policy, 'a removed symbol')

binding = copy.deepcopy(module_symbols)
binding_row = policy[1]
old_binding = binding_row['binding']
new_binding = 'weak' if old_binding == 'strong' else 'strong'
binding[binding_row['module']][old_binding].remove(binding_row['symbol'])
binding[binding_row['module']][new_binding].add(binding_row['symbol'])
rejected(binding, policy, 'binding drift')

with tempfile.TemporaryDirectory(prefix='pw-title-imports-') as directory:
    root = Path(directory)
    drift_policy = root / 'drift-policy.json'
    policy_document = {
        'format': 2,
        'modules': ['ntdll.so', 'win32u.so', 'wineserver.so'],
        'imports': copy.deepcopy(policy),
        'native_roles': copy.deepcopy(NATIVE_ROLE_IMPORTS),
    }
    policy_document['imports'][0]['target'] = 'future_secret_runtime_api'
    import json
    drift_policy.write_text(json.dumps(policy_document))
    try:
        load_policy(drift_policy)
    except ValueError:
        pass
    else:
        raise SystemExit('import policy unexpectedly accepted target drift')
    policy_document['imports'] = copy.deepcopy(policy)
    policy_document['native_roles']['child'] = copy.deepcopy(NATIVE_ROLE_IMPORTS['parent'])
    drift_policy.write_text(json.dumps(policy_document))
    try:
        load_policy(drift_policy)
    except ValueError:
        pass
    else:
        raise SystemExit('import policy unexpectedly admitted child supervisor APIs')

    # This small link proof deliberately supplies a real definition for the
    # unknown name. The policy gate still rejects it before native link, and
    # the reviewed table consequently cannot resolve it.
    small_symbols = {
        'ntdll.so': {'strong': {'dlopen'}, 'weak': set()},
        'win32u.so': {'strong': set(), 'weak': set()},
        'wineserver.so': {'strong': set(), 'weak': set()},
    }
    small_policy = [{
        'module': 'ntdll.so',
        'binding': 'strong',
        'symbol': 'dlopen',
        'target': 'pw_public_dlopen',
        'owner': 'custom_loader',
    }]
    small_records = make_records(small_symbols, small_policy)
    supplied_unknown = copy.deepcopy(small_symbols)
    supplied_unknown['ntdll.so']['strong'].add('future_secret_runtime_api')
    rejected(supplied_unknown, small_policy, 'an SDK/native-provided unknown symbol')

    modules = [
        {
            'name': 'ntdll.so',
            'path': '/app0/wine/lib/wine/x86_64-unix/ntdll.so',
            'sha256': '00' * 32,
            'size': 1,
        },
        {
            'name': 'win32u.so',
            'path': '/app0/wine/lib/wine/x86_64-unix/win32u.so',
            'sha256': '22' * 32,
            'size': 3,
        },
        {
            'name': 'wineserver.so',
            'path': '/app0/wine/psmkit-wine-disposable/bin/wineserver.so',
            'sha256': '11' * 32,
            'size': 2,
        },
    ]
    for missing in (modules[:1] + modules[2:],):
        try:
            render_include(small_records, missing)
        except ValueError:
            pass
        else:
            raise SystemExit('missing win32u identity accepted')
    bad_path = copy.deepcopy(modules)
    bad_path[1]['path'] = '/app0/unreviewed/win32u.so'
    try:
        render_include(small_records, bad_path)
    except ValueError:
        pass
    else:
        raise SystemExit('tampered win32u path accepted')
    include = root / 'imports.inc'
    include.write_text(render_include(small_records, modules))
    source = root / 'provided-unknown.c'
    source.write_text(f'''#include <stdint.h>
#include <string.h>
struct pw_elf_import {{ const char *name; uintptr_t address; }};
struct pw_title_module_manifest {{
    const char *path;
    uint8_t sha256[32];
    uint64_t size;
}};
#include "{include}"
#define DEFINE_EXACT(name, symbol) void name(void) __asm__(symbol); void name(void) {{}}
DEFINE_EXACT(exact_pw_public_dlopen, "pw_public_dlopen")
DEFINE_EXACT(exact_future_secret_runtime_api, "future_secret_runtime_api")
static uintptr_t lookup(const char *name) {{
    for (unsigned i = 0; i < sizeof(native_imports) / sizeof(native_imports[0]); ++i)
        if (!strcmp(name, native_imports[i].name)) return native_imports[i].address;
    return 0;
}}
int main(void) {{
    (void)&exact_future_secret_runtime_api;
    return lookup("dlopen") != (uintptr_t)&exact_pw_public_dlopen ||
           lookup("future_secret_runtime_api") != 0 ||
           sizeof(title_modules) / sizeof(title_modules[0]) != 3 ||
           strcmp(title_modules[0].path,
                  "/app0/wine/lib/wine/x86_64-unix/ntdll.so") ||
           title_modules[0].size != UINT64_C(1) ||
           title_modules[0].sha256[0] != 0x00 ||
           strcmp(title_modules[1].path,
                  "/app0/wine/lib/wine/x86_64-unix/win32u.so") ||
           title_modules[1].size != UINT64_C(3) ||
           title_modules[1].sha256[0] != 0x22 ||
           strcmp(title_modules[2].path,
                  "/app0/wine/psmkit-wine-disposable/bin/wineserver.so") ||
           title_modules[2].size != UINT64_C(2) ||
           title_modules[2].sha256[0] != 0x11;
}}
''')
    executable = root / 'provided-unknown'
    command = shlex.split(args.cc) + [
        '-std=c11', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(executable)]
    subprocess.run(command, check=True)
    subprocess.run([str(executable)], check=True)

print(f'TITLE_IMPORT_SELECTION_PASS imports={len(records)} '
      'unknown_import=POLICY_REJECTED_WITH_DEFINITION')
