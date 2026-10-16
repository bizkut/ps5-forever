#!/usr/bin/env python3
"""Canonical title profiles and host-only B1 publication/guard inventories."""
import hashlib
import json
import math
from pathlib import Path, PurePosixPath

# Payload ENV's WINEDLLOVERRIDES in ps5/deploy-wine.py (independent of title overrides).
PAYLOAD_DLL_OVERRIDES = ('winemenubuilder.exe=d;mscoree=d;mshtml=d;winedbg.exe=d;'
                         'msv1_0=d;kerberos=d;tabtip.exe=d')

WRITEBROKER_RELATIVE = 'writebroker.elf'
WRITEBROKER_MAX_BYTES = 1024 * 1024
# User data outside the app folder, which a store update replaces. ShadowMountPlus
# 1.7beta3+ mounts /data into the title's sandbox.
USER_DATA_ROOT = '/data/forever'


def _valid_hdr_mode(setting):
    """Validate the bounded key=value grammar consumed by vkd3d's HDR presenter."""
    if (not isinstance(setting, str) or not setting or len(setting) > 511 or
            not setting.isascii() or any(ord(char) < 0x20 or ord(char) > 0x7e for char in setting)):
        return False
    values = {}
    for token in setting.split():
        if token.count('=') != 1:
            return False
        key, value = token.split('=', 1)
        if not key or not value or key in values:
            return False
        values[key] = value

    def number(name, default=None):
        text = values.get(name, default)
        if text is None or any(char not in '+-.0123456789eE' for char in text):
            raise ValueError
        return float(text)

    def shaders(name):
        items = values.get(name, '').split(',')
        if (not 1 <= len(items) <= 4 or
                any(len(item) != 16 or any(char not in '0123456789abcdefABCDEF'
                                            for char in item) for item in items)):
            raise ValueError
        parsed = tuple(int(item, 16) for item in items)
        if 0 in parsed or len(set(parsed)) != len(parsed):
            raise ValueError
        return parsed

    mode = values.get('mode')
    if mode == 'off':
        return set(values) == {'mode'}
    if mode == 'cap':
        if set(values) != {'mode', 'max'}:
            return False
        try:
            maximum = number('max')
        except ValueError:
            return False
        return math.isfinite(maximum) and 80.0 <= maximum <= 10000.0

    sdr_keys = {'mode', 'paper', 'peak', 'expand', 'sat', 'gamma', 'key'}
    auto_keys = sdr_keys | {'sdr_shaders', 'hdr_shaders', 'hdr_cap'}
    if mode == 'sdr':
        if set(values) - sdr_keys:
            return False
        sdr_hashes = hdr_hashes = ()
    elif mode == 'auto':
        if (set(values) - auto_keys or
                not {'sdr_shaders', 'hdr_shaders'} <= set(values)):
            return False
        try:
            sdr_hashes = shaders('sdr_shaders')
            hdr_hashes = shaders('hdr_shaders')
        except ValueError:
            return False
        if set(sdr_hashes) & set(hdr_hashes):
            return False
    else:
        return False

    try:
        paper = number('paper', '203')
        peak = number('peak', '637')
        expansion = number('expand', '1')
        saturation = number('sat', '1')
        gamma = 0.0 if values.get('gamma') == 'srgb' else number('gamma', '2.2')
        hdr_cap = number('hdr_cap', '0') if mode == 'auto' else 0.0
    except ValueError:
        return False
    return (all(map(math.isfinite, (paper, peak, expansion, saturation, gamma, hdr_cap))) and
            40.0 <= paper <= 1000.0 and 80.0 <= peak <= 10000.0 and paper <= peak and
            0.0 <= expansion <= 1.0 and 0.0 <= saturation <= 2.0 and
            (gamma == 0.0 or 1.0 <= gamma <= 3.0) and
            (hdr_cap == 0.0 or 80.0 <= hdr_cap <= 10000.0) and
            values.get('key', 'y') in ('y', 'max'))

def _valid_sdr_white_nits(setting):
    """Validate Windows' integer 80..480-nit SDR content brightness range."""
    return (isinstance(setting, str) and 1 <= len(setting) <= 3 and
            setting.isascii() and setting.isdecimal() and
            80 <= int(setting) <= 480)


def installed_paths(loader):
    path = PurePosixPath(loader)
    if not path.is_absolute() or '..' in path.parts or path.parts[-4:] != ('lib', 'wine', 'x86_64-unix', 'wine'):
        raise ValueError(f'not an installed x64 Wine loader: {loader}')
    root = path.parents[3]
    return {'dll_dir': str(root / 'lib/wine'), 'bin_dir': str(root / 'bin'),
            'data_dir': str(root / 'share/wine'), 'wineloader': str(path)}


def profile(name, overrides=None):
    if name not in ('p1w', 'b1'):
        raise ValueError(f'unknown title runtime profile: {name}')
    b1 = name == 'b1'
    root = '/app0/wine-title' if b1 else '/app0/wine'
    fixture = '/app0/wine/tests/b1-trace' if b1 else root + '/psmkit-wine-disposable'
    loader = root + '/lib/wine/x86_64-unix/wine'
    selected = {
        'name': name, 'root': root,
        'remote_root': '/data/homebrew/PPSA99030/' + root.removeprefix('/app0/'),
        'loader': loader, 'prefix': fixture + '/prefix', 'home': fixture + '/home',
        'server': root + '/server-title' if b1 else fixture + '/server-title',
        'debug': 'err+all,fixme+all,+seh,+loaddll' if b1 else '+seh',
        'cwd': fixture + '/prefix/drive_c/Program Files (x86)/Battle.net' if b1 else None,
        'argv': [loader, r'C:\windows\system32\start.exe', '/exec',
                 r'C:\Program Files (x86)\Battle.net\Battle.net Launcher.exe']
                if b1 else [loader, r'C:\psmkit-wine-parent.exe'],
        'dll_overrides': ('winemenubuilder.exe=d;mscoree=d;mshtml=d;winedbg.exe=d;'
                          'msv1_0=d;kerberos=d;tabtip.exe=d') if b1 else None,
        **({'headless': False, 'gpu': False,
            'installer_url': 'https://downloader.battle.net//download/getInstallerForGame?os=win&gameProgram=BATTLENET_APP&version=Live',
            # Direct versioned file, tried only when the Live redirect download fails.
            'installer_fallback_url': 'https://downloader.battle.net/download/installer/win/1.0.66/Battle.net-Setup.exe',
            'installer_dos': r'C:\Battle.net-Setup.exe',
            'payload_programs': [],
            'refused_programs': [],
            'prefetch_hosts': [],
            'dns_upstreams': [],
            'browser_switches': {},
            'payload_dll_overrides': PAYLOAD_DLL_OVERRIDES} if b1 else {}),
        'module_paths': {
            'ntdll.so': root + '/lib/wine/x86_64-unix/ntdll.so',
            'win32u.so': root + '/lib/wine/x86_64-unix/win32u.so',
            'wineserver.so': root + '/bin/wineserver.so' if b1 else fixture + '/bin/wineserver.so',
            **({module + '.so': root + '/lib/wine/x86_64-unix/' + module + '.so'
                for module in ('ws2_32', 'crypt32', 'opengl32', 'secur32', 'bcrypt', 'wineps5',
                               'dwrite')} if b1 else {}),
            # schannel (secur32), bcrypt and crypt32 dlopen Wine's SONAME_LIBGNUTLS,
            # win32u and dwrite its SONAME_LIBFREETYPE; the B1 loader maps the
            # payload prefix of those names to this root.
            **({'libgnutls.so': root + '/lib/libgnutls.so',
                'libfreetype.so': root + '/lib/libfreetype.so'} if b1 else {}),
        },
    }
    if overrides is not None:
        if name != 'b1' or not isinstance(overrides, dict):
            raise ValueError('profile overrides are a B1 JSON object only')
        allowed = {'argv', 'cwd', 'prefix', 'home', 'dll_overrides', 'child_dll_overrides',
                   'debug', 'headless', 'payload_programs', 'refused_programs',
                   'browser_switches', 'gpu', 'environment', 'prefetch_hosts', 'dns_upstreams'}
        if overrides.keys() - allowed:
            raise ValueError(f'unsupported B1 profile inputs: {sorted(overrides.keys()-allowed)}')
        for key, value in overrides.items():
            if key == 'argv':
                if not isinstance(value, list) or not value or any(not isinstance(arg, str) or '\0' in arg for arg in value):
                    raise ValueError('B1 argv must contain a Windows program and string arguments')
                selected['argv'] = [loader, *value]
            elif key == 'payload_programs':
                if (not isinstance(value, list) or any(
                        not isinstance(path, str) or len(path) < 3 or
                        not path[0].isascii() or not path[0].isalpha() or path[1:3] != ':\\' or
                        any(char in path for char in '\0;\r\n') for path in value)):
                    raise ValueError('B1 payload_programs must be a list of absolute DOS paths')
                selected[key] = value
            elif key == 'refused_programs':
                if (not isinstance(value, list) or len(value) > 64 or any(
                        not isinstance(program, str) or not program or len(program) > 255 or
                        not program.isascii() or program in ('.', '..') or program[-1] in ' .' or
                        any(ord(char) < 32 or char in '<>:"/\\|?*;' for char in program)
                        for program in value) or
                        len({program.lower() for program in value}) != len(value) or
                        len(';'.join(value)) > 4096):
                    raise ValueError('B1 refused_programs must be unique ASCII Windows basenames')
                selected[key] = value
            elif key == 'prefetch_hosts':
                # host names the parent resolves at start, so the console's
                # per-process lookup cache answers the program's own lookups
                if (not isinstance(value, list) or len(value) > 32 or len(set(value)) != len(value) or any(
                        not isinstance(host, str) or not 0 < len(host) <= 253 or
                        any(not (char.isascii() and (char.isalnum() or char in '.-')) for char in host)
                        for host in value)):
                    raise ValueError('B1 prefetch_hosts must be up to 32 unique DNS host names')
                selected[key] = value
            elif key == 'dns_upstreams':
                # IPv4 DNS servers the title asks directly (console resolver fallback)
                if (not isinstance(value, list) or len(value) > 4 or len(set(value)) != len(value) or any(
                        not isinstance(server, str) or len(server.split('.')) != 4 or any(
                            not part.isdigit() or len(part) > 3 or int(part) > 255 or (len(part) > 1 and part[0] == '0')
                            for part in server.split('.')) or server == '0.0.0.0'
                        for server in value)):
                    raise ValueError('B1 dns_upstreams must be up to 4 unique dotted IPv4 addresses')
                selected[key] = value
            elif key == 'child_dll_overrides':
                if (not isinstance(value, str) or not value or len(value) > 4096 or
                        any(char in value for char in '\0\r\n')):
                    raise ValueError('B1 child_dll_overrides must be a bounded nonempty string')
                selected[key] = value
            elif key == 'environment':
                boolean_environment = {
                    'PROTON_ENABLE_HDR', 'WINEPS5_DIRECT_REQUESTS', 'WINEPS5_HDR_TRACE',
                    'WINEPS5_LOG_ASYNC', 'WINEPS5_QUERY_TRACE', 'WINEPS5_PROFILE',
                    'WINEPS5_MXCSR_TRACE', 'WINEPS5_WRITE_TRACE', 'WINEPS5_WRITE_STICKY',
                }
                enum_environment = {
                    'PS5_VIDEOOUT_STATS': {'1'},
                    'VKD3D_CONFIG': {'single_queue', 'single_queue,no_staggered_submit'},
                    'VKD3D_QUEUE_PROFILE': {r'W:\vkd3d-queue-profile.json'},
                    'VKD3D_SWAPCHAIN_PRESENT_MODE': {'FIFO', 'IMMEDIATE', 'MAILBOX'},
                }
                structured_environment = {
                    'WINEPS5_HDR_MODE': _valid_hdr_mode,
                    'WINEPS5_SDR_WHITE_NITS': _valid_sdr_white_nits,
                }
                allowed_environment = (
                    boolean_environment | enum_environment.keys() | structured_environment.keys())
                if (not isinstance(value, dict) or value.keys() - allowed_environment or
                        any(not isinstance(setting, str) or
                            (name in boolean_environment and setting not in ('0', '1')) or
                            (name in enum_environment and setting not in enum_environment[name]) or
                            (name in structured_environment and not structured_environment[name](setting))
                            for name, setting in value.items())):
                    raise ValueError('B1 environment has an unknown key or a value outside its fixed allowlist')
                selected[key] = value
            elif key == 'browser_switches':
                # program file name -> Chromium switches Wine adds to its launches
                if not isinstance(value, dict) or any(
                        not isinstance(program, str) or not program or any(char in program for char in ' ;\\/\0') or
                        not isinstance(switches, list) or not switches or any(
                            not isinstance(switch, str) or not switch.startswith('--') or
                            any(char in switch for char in ' ;"\0') for switch in switches)
                        for program, switches in value.items()):
                    raise ValueError('B1 browser_switches maps program file names to lists of --switches')
                selected[key] = value
            elif key in ('headless', 'gpu'):
                if not isinstance(value, bool):
                    raise ValueError(f'B1 {key} must be a boolean')
                selected[key] = value
            else:
                if not isinstance(value, str) or not value or '\0' in value:
                    raise ValueError(f'B1 {key} must be a nonempty string')
                if key not in ('dll_overrides', 'debug') and (
                        not value.startswith(('/app0/', USER_DATA_ROOT + '/')) or
                        '..' in PurePosixPath(value).parts):
                    raise ValueError(f'B1 {key} must stay inside the title /app0 tree or {USER_DATA_ROOT}')
                selected[key] = value
    if b1:
        if selected['gpu'] and not selected['headless']:
            raise ValueError('B1 gpu requires headless to give RADV exclusive VideoOut ownership')
        if (selected.get('environment', {}).keys() -
                {'WINEPS5_MXCSR_TRACE', 'WINEPS5_WRITE_TRACE', 'WINEPS5_WRITE_STICKY'}) and not selected['gpu']:
            raise ValueError('B1 graphics environment requires a gpu profile')
        if overrides is not None and 'child_dll_overrides' in overrides and not selected['gpu']:
            raise ValueError('B1 child_dll_overrides requires a gpu profile')
        selected.setdefault('child_dll_overrides', selected['dll_overrides'])
        if selected['gpu']:
            # The game parent owns the console port; PsmKit children load the
            # same driver but use its clocked silent fallback.
            selected['module_paths']['wineps5audio.so'] = (
                root + '/lib/wine/x86_64-unix/wineps5audio.so')
            # D3D's Vulkan path: winevulkan's Unix side reaches RADV through win32u.
            selected['module_paths']['winevulkan.so'] = root + '/lib/wine/x86_64-unix/winevulkan.so'
            # Native XInput bridges, admitted only for GPU titles.
            for version in (1, 2, 3, 4):
                module = f'xinput1_{version}.so'
                selected['module_paths'][module] = root + '/lib/wine/x86_64-unix/' + module
        selected['launcher_unix'] = selected['prefix'] + '/drive_c/Program Files (x86)/Battle.net/Battle.net Launcher.exe'
        selected['installer_unix'] = selected['prefix'] + '/drive_c/Battle.net-Setup.exe'
        selected['installer_cwd'] = selected['prefix'] + '/drive_c'
    return selected


def file_identity(path):
    data = path.read_bytes()
    return {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}


def runtime_tree_manifest(root, selected):
    root = Path(root)
    files = {}
    for path in sorted(root.rglob('*')):
        if path.is_symlink():
            raise ValueError(f'title runtime contains a symlink: {path}')
        if path.is_file() and path.relative_to(root).as_posix() != 'title-root-manifest.json':
            relative = path.relative_to(root).as_posix()
            files[relative] = file_identity(path)
    admitted = {value.removeprefix(selected['root'] + '/') for value in selected['module_paths'].values()}
    actual = {name for name in files if name.endswith('.so')}
    if actual != admitted:
        raise ValueError(f'title runtime Unix modules differ: added={sorted(actual-admitted)} missing={sorted(admitted-actual)}')
    required = {'lib/wine/x86_64-unix/wine', 'share/wine/wine.inf', 'share/wine/nls/locale.nls',
                'lib/wine/i386-windows/ntdll.dll', 'lib/wine/x86_64-windows/ntdll.dll'}
    if selected['name'] == 'b1':
        required.add(WRITEBROKER_RELATIVE)
    if not required <= files.keys():
        raise ValueError(f'incomplete title runtime: {sorted(required-files.keys())}')
    return {'format': 1, 'profile': selected['name'], 'title_root': selected['root'],
            'remote_root': selected['remote_root'], 'module_paths': selected['module_paths'],
            'pe_dll_counts': {arch: sum(name.startswith(f'lib/wine/{arch}-windows/') and name.endswith('.dll')
                                      for name in files) for arch in ('i386', 'x86_64')},
            'files': files}


def b1_title_files(folder):
    folder = Path(folder)
    files = []
    for path in sorted(folder.rglob('*')):
        relative = path.relative_to(folder).as_posix()
        if path.is_symlink():
            raise ValueError(f'B1 publication contains a symlink: {relative}')
        if not path.is_file() or relative.startswith('wine-title/'):
            continue
        if relative.startswith('wine/') or relative == 'wine':
            raise ValueError(f'B1 publication would write payload install: {relative}')
        files.append(relative)
    if not {'eboot.bin', 'sce_sys/param.json', 'runtime-qualification.json', 'psmkit-wine-runtime-child.bin'} <= set(files):
        raise ValueError('incomplete B1 title publication')
    return files


def b1_guard_paths(folder, title_root):
    paths = {title_root + '/wine/boot.txt': {}}
    for relative in b1_title_files(folder):
        remote = PurePosixPath(title_root) / relative
        paths[str(remote)] = {}
        paths[str(remote.parent / ('.upload-' + remote.name))] = {}
        parent = remote.parent
        while str(parent) != title_root:
            paths.setdefault(str(parent), {'kind': 'directory'})
            parent = parent.parent
    return dict(sorted(paths.items()))


def qualified_b1_folder(folder):
    """Bind every published title/root byte to the static qualifier receipt."""
    folder = Path(folder)
    receipt = json.loads((folder / 'runtime-qualification.json').read_text())
    material = receipt['material']
    supplied = material.get('profile', {})
    selected = profile('b1', {
        **{key: supplied.get(key) for key in ('cwd', 'prefix', 'home', 'dll_overrides', 'debug')},
        'headless': supplied.get('headless', False),
        'payload_programs': supplied.get('payload_programs', []),
        'refused_programs': supplied.get('refused_programs', []),
        'prefetch_hosts': supplied.get('prefetch_hosts', []),
        'dns_upstreams': supplied.get('dns_upstreams', []),
        **({'browser_switches': supplied['browser_switches']} if supplied.get('browser_switches') else {}),
        **({'gpu': supplied['gpu']} if supplied.get('gpu') else {}),
        **({'child_dll_overrides': supplied['child_dll_overrides']}
           if supplied.get('gpu') and 'child_dll_overrides' in supplied else {}),
        **({'environment': supplied['environment']} if 'environment' in supplied else {}),
        'argv': supplied.get('argv', [])[1:],
    })
    game_supplied = material.get('game_profile')
    game_selected = None
    if game_supplied:
        game_selected = profile('b1', {
            **{key: game_supplied.get(key) for key in ('cwd', 'prefix', 'home', 'dll_overrides', 'debug')},
            'headless': game_supplied.get('headless', False),
            'payload_programs': game_supplied.get('payload_programs', []),
            'refused_programs': game_supplied.get('refused_programs', []),
            'prefetch_hosts': game_supplied.get('prefetch_hosts', []),
            'dns_upstreams': game_supplied.get('dns_upstreams', []),
            **({'browser_switches': game_supplied['browser_switches']} if game_supplied.get('browser_switches') else {}),
            'gpu': True,
            **({'child_dll_overrides': game_supplied['child_dll_overrides']}
               if 'child_dll_overrides' in game_supplied else {}),
            **({'environment': game_supplied['environment']} if 'environment' in game_supplied else {}),
            'argv': game_supplied.get('argv', [])[1:],
        })
    digest = hashlib.sha256(json.dumps(material, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    if (receipt.get('status') != 'QUALIFIED_STATIC_NOT_EXECUTED' or
            receipt.get('qualification_key') != digest or
            selected != {**supplied, 'gpu': supplied.get('gpu', False)} or
            (game_supplied and game_selected != {**game_supplied, 'gpu': True})):
        raise ValueError('B1 qualification profile/status/digest mismatch')
    root = folder / 'wine-title'
    manifest = json.loads((root / 'title-root-manifest.json').read_text())
    if manifest != runtime_tree_manifest(root, game_selected or selected):
        raise ValueError('B1 root does not match its manifest')
    writebroker_path = root / WRITEBROKER_RELATIVE
    writebroker_data = writebroker_path.read_bytes()
    writebroker = {
        'path': WRITEBROKER_RELATIVE,
        'app_path': (game_selected or selected)['root'] + '/' + WRITEBROKER_RELATIVE,
        'classification': 'data_only_not_loaded',
        'size': len(writebroker_data),
        'sha256': hashlib.sha256(writebroker_data).hexdigest(),
    }
    if (not writebroker_data.startswith(b'\x7fELF') or
            not 0 < len(writebroker_data) <= WRITEBROKER_MAX_BYTES or
            material.get('writebroker') != writebroker):
        raise ValueError('B1 write broker data identity differs from qualification')
    inventories = ((folder, material['parent_title']), (root, material['title_root']))
    for directory, inventory in inventories:
        for item in inventory:
            relative = PurePosixPath(item['path'])
            if relative.is_absolute() or '..' in relative.parts:
                raise ValueError(f'unsafe qualified path: {relative}')
            identity = file_identity(directory / relative)
            if identity != {'bytes': item['size'], 'sha256': item['sha256']}:
                raise ValueError(f'qualified file differs: {directory / relative}')
    expected_root = {item['path'] for item in material['title_root']}
    if expected_root != {p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file()}:
        raise ValueError('B1 root file set differs from qualification')
    expected_title = {item['path'] for item in material['parent_title']} | {
        'runtime-qualification.json', 'psmkit-wine-runtime-child.bin'}
    if expected_title != set(b1_title_files(folder)):
        raise ValueError('B1 title file set differs from qualification')
    for role, filename in (('native_parent', 'eboot.bin'), ('native_child', 'psmkit-wine-runtime-child.bin')):
        identity = file_identity(folder / filename)
        if identity != {'bytes': material[role]['fself_size'], 'sha256': material[role]['fself_sha256']}:
            raise ValueError(f'B1 {role} differs from qualification')
    for item in material['modules']:
        identity = file_identity(folder / item['path'].removeprefix('/app0/'))
        if (item['path'] != (game_selected or selected)['module_paths'].get(item['name']) or
                identity != {'bytes': item['size'], 'sha256': item['sha256']}):
            raise ValueError(f'B1 admitted module differs from qualification: {item["name"]}')
    return receipt, manifest
