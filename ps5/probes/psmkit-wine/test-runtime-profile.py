#!/usr/bin/env python3
"""Host-only path, manifest and guarded-write tests for the B1 title profile."""
import hashlib
import importlib.util
import shutil
import json
from pathlib import Path
import tempfile

from runtime_profiles import (
    WRITEBROKER_MAX_BYTES, WRITEBROKER_RELATIVE, profile, installed_paths,
    runtime_tree_manifest, b1_guard_paths, qualified_b1_folder, file_identity, builtin_prefix_files,
)
from runtime_imports import (
    candidate_ownership, module_manifest, render_include, load_policy, make_records,
    native_role_imports,
)

with tempfile.TemporaryDirectory() as directory:
    tree = Path(directory)
    builtin = b'MZ' + bytes(62) + b'Wine builtin DLL\0' + bytes(16)
    for arch in ('x86_64', 'i386'):
        folder = tree / f'lib/wine/{arch}-windows'
        folder.mkdir(parents=True)
        for name in ('advapi32.dll', 'kernel32.dll', 'wineps5audio.drv', 'start.exe', 'dxgi.dll'):
            (folder / name).write_bytes(builtin)
        (folder / 'not-pe.txt').write_bytes(b'not a builtin PE')
        (folder / 'amd_ags_x64.dll').write_bytes(builtin)
    owned = dict(builtin_prefix_files(tree))
    for arch, folder in (('x86_64', 'system32'), ('i386', 'syswow64')):
        for name in ('advapi32.dll', 'kernel32.dll', 'wineps5audio.drv', 'start.exe'):
            assert owned[f'lib/wine/{arch}-windows/{name}'] == f'drive_c/windows/{folder}/{name}'
    assert not any('not-pe.txt' in name or 'amd_ags_x64.dll' in name for name in owned)
    assert list(owned) == sorted(owned)

b1 = profile('b1')
p1w = profile('p1w')
product = json.loads(Path(__file__).with_name('b1-product.json').read_text())
assert product['debug'] == 'err+all,err-seh'
assert product['argv'] == [r'C:\Forever\Addons\forever-maintenance.exe']
game_config = json.loads(Path(__file__).with_name('b1-game-wow-forever.json').read_text())
beta_config = json.loads(Path(__file__).with_name('b1-game-wow-classic-beta.json').read_text())
maintenance = profile('b1', product)
assert maintenance['refused_programs'] == []
assert 'B1_GAME_PROFILE_CONFIG ?= $(CURDIR)/b1-game-wow-classic-beta.json' in (
    Path(__file__).with_name('Makefile').read_text())
for config, channel, executable, uid in (
        (game_config, '_forever_', 'Wow.exe', 'wowf'),
        (beta_config, '_classic_beta_', 'WowB.exe', 'wow_classic_beta')):
    game = profile('b1', config)
    windows_cwd = rf'C:\Program Files (x86)\World of Warcraft\{channel}'
    assert config['argv'] == [windows_cwd + '\\' + executable, '-launcherlogin', '-uid', uid]
    assert game['argv'] == [game['loader'], *config['argv']]
    assert game['cwd'] == f'/data/forever/prefix/drive_c/Program Files (x86)/World of Warcraft/{channel}'
    assert game['prefix'] == '/data/forever/prefix'
    assert game['home'] == '/data/forever/home'
    assert game['gpu'] is True and game['headless'] is True
    assert game['refused_programs'] == ['BlizzardError.exe']
    assert game['environment'] == {'PROTON_ENABLE_HDR': '1'}
    assert 'WINEPS5_SDR_WHITE_NITS' not in game['environment']
assert {key: value for key, value in game_config.items() if key not in ('argv', 'cwd')} == {
    key: value for key, value in beta_config.items() if key not in ('argv', 'cwd')}
assert set(b1['module_paths']) == {
    'ntdll.so', 'win32u.so', 'wineserver.so', 'ws2_32.so', 'crypt32.so', 'opengl32.so',
    'secur32.so', 'bcrypt.so', 'libgnutls.so', 'wineps5.so', 'dwrite.so', 'libfreetype.so',
}
assert b1['headless'] is False
assert b1['gpu'] is False
assert 'gpu' not in p1w
assert profile('b1', {'gpu': False})['gpu'] is False
assert profile('b1', {'gpu': True, 'headless': True})['gpu'] is True
gpu = profile('b1', {'headless': True, 'gpu': True})
assert gpu['gpu'] is True
assert gpu['module_paths'] == {**b1['module_paths'],
                               'wineps5audio.so': '/app0/wine-title/lib/wine/x86_64-unix/wineps5audio.so',
                               'winevulkan.so': '/app0/wine-title/lib/wine/x86_64-unix/winevulkan.so',
                               **{f'xinput1_{v}.so': f'/app0/wine-title/lib/wine/x86_64-unix/xinput1_{v}.so'
                                  for v in (1, 2, 3, 4)}}
assert 'winevulkan.so' not in profile('b1', {'headless': True})['module_paths']
assert profile('b1', {'gpu': True, 'headless': True,
                      'environment': {'PROTON_ENABLE_HDR': '1'}})['environment'] == {'PROTON_ENABLE_HDR': '1'}
assert profile('b1', {'gpu': True, 'headless': True,
                      'environment': {'WINEPS5_HDR_TRACE': '1'}})['environment'] == {'WINEPS5_HDR_TRACE': '1'}
assert profile('b1', {'gpu': True, 'headless': True,
                      'environment': {'WINEPS5_QUERY_TRACE': '1'}})['environment'] == {
                          'WINEPS5_QUERY_TRACE': '1'}
for direct in ('0', '1'):
    assert profile('b1', {'gpu': True, 'headless': True,
                          'environment': {'WINEPS5_DIRECT_REQUESTS': direct}})['environment'] == {
                              'WINEPS5_DIRECT_REQUESTS': direct}
for log_async in ('0', '1'):
    assert profile('b1', {'gpu': True, 'headless': True,
                          'environment': {'WINEPS5_LOG_ASYNC': log_async}})['environment'] == {
                              'WINEPS5_LOG_ASYNC': log_async}
for trace_name in ('WINEPS5_MXCSR_TRACE', 'WINEPS5_WRITE_TRACE', 'WINEPS5_WRITE_STICKY'):
    for trace_value in ('0', '1'):
        for gpu_enabled in (False, True):
            assert profile('b1', {'gpu': gpu_enabled, 'headless': gpu_enabled,
                                  'environment': {trace_name: trace_value}})['environment'] == {
                                      trace_name: trace_value}
assert profile('b1', {'gpu': True, 'headless': True,
                      'environment': {'PROTON_ENABLE_HDR': '1', 'WINEPS5_DIRECT_REQUESTS': '0',
                                      'WINEPS5_HDR_TRACE': '1', 'WINEPS5_QUERY_TRACE': '1'}})['environment'] == {
                          'PROTON_ENABLE_HDR': '1', 'WINEPS5_DIRECT_REQUESTS': '0',
                          'WINEPS5_HDR_TRACE': '1', 'WINEPS5_QUERY_TRACE': '1'}
for name, setting in (
        ('WINEPS5_PROFILE', '1'),
        ('PS5_VIDEOOUT_STATS', '1'),
        ('VKD3D_CONFIG', 'single_queue'),
        ('VKD3D_CONFIG', 'single_queue,no_staggered_submit'),
        ('VKD3D_QUEUE_PROFILE', r'W:\vkd3d-queue-profile.json'),
        ('VKD3D_SWAPCHAIN_PRESENT_MODE', 'FIFO'),
        ('VKD3D_SWAPCHAIN_PRESENT_MODE', 'IMMEDIATE'),
        ('VKD3D_SWAPCHAIN_PRESENT_MODE', 'MAILBOX'),
        ('WINEPS5_SDR_WHITE_NITS', '80'),
        ('WINEPS5_SDR_WHITE_NITS', '203'),
        ('WINEPS5_SDR_WHITE_NITS', '480'),
        ('WINEPS5_HDR_MODE', 'mode=off'),
        ('WINEPS5_HDR_MODE', 'mode=cap max=80'),
        ('WINEPS5_HDR_MODE', 'mode=cap max=637'),
        ('WINEPS5_HDR_MODE', 'mode=cap max=10000'),
        ('WINEPS5_HDR_MODE', 'mode=sdr'),
        ('WINEPS5_HDR_MODE', 'mode=sdr paper=203 peak=637 expand=1 sat=1.0 gamma=2.2 key=y'),
        ('WINEPS5_HDR_MODE', 'mode=sdr gamma=srgb key=max'),
        ('WINEPS5_HDR_MODE', 'mode=auto sdr_shaders=52596c4112cfc065 '
                             'hdr_shaders=c2d9013c5a1bd61e paper=203 peak=637 '
                             'expand=0.4 sat=1.0 gamma=2.6 key=y'),
        ('WINEPS5_HDR_MODE', 'mode=auto sdr_shaders=52596c4112cfc065,0123456789abcdef '
                             'hdr_shaders=c2d9013c5a1bd61e hdr_cap=637')):
    assert profile('b1', {'gpu': True, 'headless': True,
                          'environment': {name: setting}})['environment'] == {name: setting}
game_mode = profile('b1', {'gpu': True, 'headless': True,
                           'child_dll_overrides': 'vulkan-1=n,b;dxgi=b'})
assert game_mode['child_dll_overrides'] == 'vulkan-1=n,b;dxgi=b'
assert b1['child_dll_overrides'] == b1['dll_overrides']
for setting in ({'DXVK_HDR': '1'}, {'PATH': '/app0'}, {'PROTON_ENABLE_HDR': 1},
                {'PROTON_ENABLE_HDR': 'yes'}, {'WINEPS5_DIRECT_REQUESTS': 0},
                {'WINEPS5_DIRECT_REQUESTS': 'yes'}, {'WINEPS5_HDR_TRACE': 1},
                {'WINEPS5_HDR_TRACE': 'yes'}, {'WINEPS5_QUERY_TRACE': 1},
                {'WINEPS5_QUERY_TRACE': 'yes'}, {'PS5_VIDEOOUT_STATS': '0'},
                {'WINEPS5_PROFILE': 1}, {'WINEPS5_PROFILE': 'yes'},
                {'WINEPS5_MXCSR_TRACE': 1}, {'WINEPS5_MXCSR_TRACE': 'yes'},
                {'WINEPS5_WRITE_TRACE': 1}, {'WINEPS5_WRITE_TRACE': 'yes'},
                {'WINEPS5_WRITE_STICKY': 1}, {'WINEPS5_WRITE_STICKY': 'yes'},
                {'VKD3D_CONFIG': 'no_staggered_submit'},
                {'VKD3D_QUEUE_PROFILE': '/tmp/profile.json'},
                {'VKD3D_SWAPCHAIN_PRESENT_MODE': 'FIFO_RELAXED'},
                {'WINEPS5_HDR_MAX_NITS': '637'}, {'WINEPS5_HDR_MODE': 637},
                {'WINEPS5_SDR_WHITE_NITS': 203},
                {'WINEPS5_SDR_WHITE_NITS': '79'},
                {'WINEPS5_SDR_WHITE_NITS': '481'},
                {'WINEPS5_SDR_WHITE_NITS': '203.0'},
                {'WINEPS5_SDR_WHITE_NITS': '0203'},
                {'WINEPS5_HDR_MODE': 'sdr'}, {'WINEPS5_HDR_MODE': 'mode=cap'},
                {'WINEPS5_HDR_MODE': 'mode=cap max=79'},
                {'WINEPS5_HDR_MODE': 'mode=cap max=10001'},
                {'WINEPS5_HDR_MODE': 'mode=off peak=637'},
                {'WINEPS5_HDR_MODE': 'mode=sdr paper=638 peak=637'},
                {'WINEPS5_HDR_MODE': 'mode=sdr expand=1.1'},
                {'WINEPS5_HDR_MODE': 'mode=sdr sat=2.1'},
                {'WINEPS5_HDR_MODE': 'mode=sdr gamma=0.9'},
                {'WINEPS5_HDR_MODE': 'mode=sdr key=red'},
                {'WINEPS5_HDR_MODE': 'mode=sdr\npeak=637'},
                {'WINEPS5_HDR_MODE': 'mode=sdr sdr_shaders=52596c4112cfc065'},
                {'WINEPS5_HDR_MODE': 'mode=auto hdr_shaders=c2d9013c5a1bd61e'},
                {'WINEPS5_HDR_MODE': 'mode=auto sdr_shaders=52596c4112cfc065 hdr_shaders=short'},
                {'WINEPS5_HDR_MODE': 'mode=auto sdr_shaders=52596c4112cfc065,52596c4112cfc065 '
                                     'hdr_shaders=c2d9013c5a1bd61e'},
                {'WINEPS5_HDR_MODE': 'mode=auto sdr_shaders=52596c4112cfc065 '
                                     'hdr_shaders=52596c4112cfc065'},
                {'WINEPS5_HDR_MODE': 'mode=auto sdr_shaders=0000000000000001,0000000000000002,'
                                     '0000000000000003,0000000000000004,0000000000000005 '
                                     'hdr_shaders=c2d9013c5a1bd61e'},
                {'WINEPS5_HDR_MODE': 'mode=auto sdr_shaders=52596c4112cfc065 '
                                     'hdr_shaders=c2d9013c5a1bd61e hdr_cap=79'},
                [], None):
    try:
        profile('b1', {'gpu': True, 'headless': True, 'environment': setting})
    except ValueError:
        pass
    else:
        raise AssertionError(f'B1 accepted unsafe environment: {setting}')
try:
    profile('b1', {'environment': {'PROTON_ENABLE_HDR': '1'}})
except ValueError:
    pass
else:
    raise AssertionError('product profile accepted a GPU environment')
assert 'headless' not in p1w
assert 'payload_programs' not in p1w and 'payload_dll_overrides' not in p1w
assert b1['payload_programs'] == []
assert b1['refused_programs'] == []
assert 'refused_programs' not in p1w
assert profile('b1', {'refused_programs': ['BlizzardError.exe']})['refused_programs'] == [
    'BlizzardError.exe']
assert b1['installer_dos'] == r'C:\Battle.net-Setup.exe'
assert b1['installer_url'] == 'https://downloader.battle.net//download/getInstallerForGame?os=win&gameProgram=BATTLENET_APP&version=Live'
# A direct versioned download, tried only after the Live redirect fails.
assert b1['installer_fallback_url'] == 'https://downloader.battle.net/download/installer/win/1.0.66/Battle.net-Setup.exe'
for key in ('installer_url', 'installer_fallback_url', 'installer_dos', 'launcher_unix', 'installer_unix', 'installer_cwd'):
    assert key not in p1w
    try:
        profile('b1', {key: b1[key]})
    except ValueError:
        pass
    else:
        raise AssertionError(f'B1 profile accepted overridable installer field: {key}')
relocated = profile('b1', {'prefix': '/app0/wine/prefix'})
assert relocated['installer_unix'] == '/app0/wine/prefix/drive_c/Battle.net-Setup.exe'
assert relocated['launcher_unix'] == '/app0/wine/prefix/drive_c/Program Files (x86)/Battle.net/Battle.net Launcher.exe'
assert relocated['installer_cwd'] == '/app0/wine/prefix/drive_c'
assert profile('b1', {'payload_programs': [r'C:\ProgramData\Battle.net\Agent\Agent.exe']})['payload_programs'] == [
    r'C:\ProgramData\Battle.net\Agent\Agent.exe']
assert ('"WINEDLLOVERRIDES=' + b1['payload_dll_overrides'] + '"') in (
    Path(__file__).resolve().parents[2] / 'deploy-wine.py').read_text()
assert profile('b1', {'payload_programs': [r'D:\Tools\Switcher.exe']})['payload_programs'] == [r'D:\Tools\Switcher.exe']
for headless in (False, True):
    roles = native_role_imports('b1', headless=headless)
    assert all(roles['parent'][name] == 'libkernel.prx'
               for name in ('connect', 'setsockopt', 'shutdown'))
    assert roles['parent']['sceSystemServiceLaunchWebBrowser'] == 'libSceSystemService.prx'
    assert roles['parent']['sceUserServiceInitialize'] == 'libSceUserService.prx'
    assert roles['child'] == {}
assert all(name not in native_role_imports('p1w')['parent']
           for name in ('connect', 'setsockopt', 'shutdown'))
assert 'sceSystemServiceLaunchWebBrowser' not in native_role_imports('p1w')['parent']
gpu_roles = native_role_imports('b1', headless=True, gpu=True)
assert gpu_roles['child'] == native_role_imports('b1', headless=True)['child']
assert gpu_roles['parent']['sceAgcInit'] == 'libSceAgc.prx'
assert gpu_roles['parent']['sceAgcDriverSubmitDcb'] == 'libSceAgcDriver.prx'
assert gpu_roles['parent']['sceVideoOutOpen'] == 'libSceVideoOut.prx'
for symbol in ('sceAudioOutInit', 'sceAudioOutOpen', 'sceAudioOutOutput', 'sceAudioOutClose'):
    assert gpu_roles['parent'][symbol] == 'libSceAudioOut.prx'
    assert symbol not in gpu_roles['child']
    assert symbol not in native_role_imports('b1', headless=False)['parent']
    assert symbol not in native_role_imports('b1', headless=True)['parent']
for symbol in ('sceVideoOutCursorDisable', 'sceVideoOutCursorEnable',
               'sceVideoOutCursorSetHotSpot', 'sceVideoOutCursorSetPosition'):
    assert gpu_roles['parent'][symbol] == 'libSceVideoOut.prx'
    assert symbol not in gpu_roles['child']
    assert symbol not in native_role_imports('b1', headless=False)['parent']
    assert symbol not in native_role_imports('b1', headless=True)['parent']
for symbol, provider in (
        ('scePadRead', 'libScePad.prx'),
        ('sceKeyboardClose', 'libSceKeyboard.prx'),
        ('sceKeyboardRead', 'libSceKeyboard.prx'),
        ('sceKeyboardReadState', 'libSceKeyboard.prx'),
        ('sceKeyboardSetProcessPrivilege', 'libSceKeyboard.prx'),
        ('sceMouseRead', 'libSceMouse.prx'),
        ('sceUserServiceGetLoginUserIdList', 'libSceUserService.prx'),
        ('sceImeOpen', 'libSceIme.prx')):
    assert gpu_roles['parent'][symbol] == provider
    assert native_role_imports('b1', headless=False)['parent'][symbol] == provider
    assert symbol not in native_role_imports('b1', headless=True)['parent']
    assert symbol not in gpu_roles['child']
assert gpu_roles['parent']['scePadSetVibration'] == 'libScePad.prx'
assert 'scePadSetVibration' not in gpu_roles['child']
for roles in (native_role_imports('b1'), native_role_imports('b1', headless=True),
              native_role_imports('p1w')):
    assert 'scePadSetVibration' not in roles['parent']
assert not any(name.startswith('xinput') for name in b1['module_paths'])
assert not any(name.startswith('xinput') for name in p1w['module_paths'])
assert 'sceHttp2Init' not in gpu_roles['parent']
for roles in (native_role_imports('b1'), native_role_imports('b1', headless=True),
              native_role_imports('p1w')):
    assert 'sceAgcInit' not in roles['parent']
    assert 'sceAgcDriverSubmitDcb' not in roles['parent']
assert len(p1w['module_paths']) == 3
reviewed = load_policy(Path(__file__).with_name('runtime-import-policy-b1.json'), profile_name='b1')
gpu_surfaces = {name: {'strong': set(), 'weak': set()} for name in gpu['module_paths']}
for row in reviewed:
    gpu_surfaces[row['module']][row['binding']].add(row['symbol'])
assert any('winevulkan.so' in row['modules'] for row in make_records(gpu_surfaces, reviewed, profile_name='b1', gpu=True))
assert any('wineps5audio.so' in row['modules']
           for row in make_records(gpu_surfaces, reviewed, profile_name='b1', gpu=True))
assert any(row['name'] == 'ps5_title_profile_set_blocking' and row['owner'] == 'title_ntdll'
           and set(row['modules']) == {'wineps5audio.so', 'winevulkan.so'}
           for row in make_records(gpu_surfaces, reviewed, profile_name='b1', gpu=True))
assert gpu_roles['parent']['pthread_getcpuclockid'] == 'libkernel.prx'
try:
    make_records(gpu_surfaces, reviewed, profile_name='b1')
except ValueError:
    pass
else:
    raise AssertionError('a launcher build accepted the GPU module')
surfaces = {name: gpu_surfaces[name] for name in b1['module_paths']}
authorized = make_records(surfaces, reviewed, profile_name='b1')
assert not any('winevulkan.so' in row['modules'] for row in authorized)
assert not any('wineps5audio.so' in row['modules'] for row in authorized)
assert any(row['owner'] == 'title_win32u' for row in authorized)
assert any(row['name'] == 'getaddrinfo' and row['target'] == 'pw_b1_getaddrinfo' for row in authorized)
# B1 klog writes leave on the native writer thread (title-log.c); p1w is unchanged.
assert candidate_ownership('sceKernelDebugOutText', profile_name='b1') == ('platform', 'pw_title_klog_text')
assert candidate_ownership('sceKernelDebugOutText') == ('UNREVIEWED_NOT_AUTHORIZED', 'sceKernelDebugOutText')
assert {row['module'] for row in reviewed if row['symbol'] == 'sceKernelDebugOutText'} == {
    'ntdll.so', 'win32u.so', 'wineps5.so', 'wineserver.so'}
assert all(row['target'] == 'pw_title_klog_text' for row in authorized if row['name'] == 'sceKernelDebugOutText')
surfaces['ws2_32.so']['strong'].add('future_unreviewed_network_function')
try:
    make_records(surfaces, reviewed, profile_name='b1')
except ValueError:
    pass
else:
    raise AssertionError('B1 import policy accepted a new resolver import')
assert b1['root'] == '/app0/wine-title'
assert installed_paths(b1['loader']) == {
    'dll_dir': '/app0/wine-title/lib/wine',
    'bin_dir': '/app0/wine-title/bin',
    'data_dir': '/app0/wine-title/share/wine',
    'wineloader': b1['loader'],
}
assert b1['prefix'] == '/app0/wine/tests/b1-trace/prefix'
assert b1['home'] == '/app0/wine/tests/b1-trace/home'
assert b1['server'] == '/app0/wine-title/server-title'
assert p1w['loader'] == '/app0/wine/lib/wine/x86_64-unix/wine'
assert p1w['module_paths']['wineserver.so'] == '/app0/wine/psmkit-wine-disposable/bin/wineserver.so'
custom = profile('b1', {'argv': ['start.exe', '/exec', 'custom.exe', '--disable-gpu'],
                        'cwd': '/app0/custom', 'prefix': '/app0/wine/custom-prefix',
                        'home': '/app0/wine/custom-home', 'dll_overrides': 'wineps5.drv=d',
                        'headless': True, 'payload_programs': []})
assert custom['argv'] == [b1['loader'], 'start.exe', '/exec', 'custom.exe', '--disable-gpu']
assert custom['module_paths'] == b1['module_paths'] and custom['root'] == b1['root']
assert custom['headless'] is True
assert custom['payload_programs'] == []
assert custom['payload_dll_overrides'] == b1['payload_dll_overrides']
for invalid in ({'root': '/app0/wine'}, {'prefix': '/app0/../data'}, {'argv': []},
                {'headless': 'yes'}, {'gpu': 'yes'}, {'gpu': 1}, {'gpu': True},
                {'gpu': True, 'headless': False}, {'payload_programs': 'Agent.exe'},
                {'payload_programs': ['Agent.exe']}, {'payload_programs': [r'C:\Agent.exe;C:\Other.exe']},
                {'payload_programs': ['C:\\Agent.exe\n']}, {'payload_programs': [None]},
                {'refused_programs': 'BlizzardError.exe'},
                {'refused_programs': [r'C:\BlizzardError.exe']},
                {'refused_programs': ['Utils/BlizzardError.exe']},
                {'refused_programs': ['BlizzardError.exe;Other.exe']},
                {'refused_programs': ['BlizzardError.exe', 'blizzarderror.EXE']},
                {'refused_programs': ['BlizzárdError.exe']}, {'refused_programs': [None]},
                {'gpu': True, 'headless': True, 'child_dll_overrides': ''},
                {'gpu': True, 'headless': True, 'child_dll_overrides': 'dxgi=b\nsecret'},
                {'child_dll_overrides': 'dxgi=b'}):
    try:
        profile('b1', invalid)
    except ValueError:
        pass
    else:
        raise AssertionError(f'B1 profile accepted unsafe input: {invalid}')

with tempfile.TemporaryDirectory(prefix='pw-b1-profile-') as temporary:
    root = Path(temporary) / 'tree'
    for relative in (WRITEBROKER_RELATIVE, 'lib/wine/i386-windows/ntdll.dll',
                     'lib/wine/x86_64-windows/ntdll.dll', 'share/wine/nls/locale.nls',
                     'share/wine/wine.inf', 'lib/wine/x86_64-unix/wine',
                     'lib/wine/x86_64-unix/ntdll.so', 'lib/wine/x86_64-unix/win32u.so',
                     'bin/wineserver.so', 'lib/wine/x86_64-unix/ws2_32.so',
                     'lib/wine/x86_64-unix/crypt32.so', 'lib/wine/x86_64-unix/opengl32.so',
                     'lib/wine/x86_64-unix/secur32.so', 'lib/wine/x86_64-unix/bcrypt.so',
                     'lib/wine/x86_64-unix/wineps5.so', 'lib/libgnutls.so',
                     'lib/wine/x86_64-unix/dwrite.so', 'lib/libfreetype.so'):
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b'\x7fELFhost-test' if relative == WRITEBROKER_RELATIVE else relative.encode())
    manifest = runtime_tree_manifest(root, b1)
    assert manifest['remote_root'] == '/data/homebrew/PPSA99030/wine-title'
    assert manifest['pe_dll_counts'] == {'i386': 1, 'x86_64': 1}
    assert manifest['files']['share/wine/nls/locale.nls']['sha256'] == hashlib.sha256(
        b'share/wine/nls/locale.nls').hexdigest()
    assert manifest['files'][WRITEBROKER_RELATIVE] == file_identity(root / WRITEBROKER_RELATIVE)
    assert (root / WRITEBROKER_RELATIVE).stat().st_size <= WRITEBROKER_MAX_BYTES
    modules = module_manifest({name: root / relative.removeprefix('/app0/wine-title/')
                              for name, relative in b1['module_paths'].items()}, profile_name='b1')
    assert all(item['path'].startswith('/app0/wine-title/') for item in modules)
    assert '/app0/wine-title/bin/wineserver.so' in render_include([], modules, profile_name='b1')
    folder = Path(temporary) / 'published'
    for relative in ('eboot.bin', 'sce_sys/param.json', 'sce_module/libc.prx',
                     'assets/icon0.png', 'psmkit-wine-runtime-child.bin',
                     'runtime-qualification.json'):
        path = folder / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b'published')
    guards = b1_guard_paths(folder, '/data/homebrew/PPSA99030')
    assert '/data/homebrew/PPSA99030/wine/boot.txt' in guards
    assert '/data/homebrew/PPSA99030/assets/icon0.png' in guards
    assert '/data/homebrew/PPSA99030/.upload-eboot.bin' in guards
    assert '/data/homebrew/PPSA99030/sce_sys/.upload-param.json' in guards
    assert not any('/wine/lib/wine/' in path or '/wine-title/' in path for path in guards)
    (root / 'title-root-manifest.json').write_text(json.dumps(manifest, sort_keys=True))
    shutil.copytree(root, folder / 'wine-title')
    def inventory(directory):
        return [{'path': path.relative_to(directory).as_posix(),
                 'size': path.stat().st_size, 'sha256': file_identity(path)['sha256']}
                for path in sorted(directory.rglob('*')) if path.is_file()]
    broker_identity = {
        'path': WRITEBROKER_RELATIVE,
        'app_path': b1['root'] + '/' + WRITEBROKER_RELATIVE,
        'classification': 'data_only_not_loaded',
        'size': (root / WRITEBROKER_RELATIVE).stat().st_size,
        'sha256': file_identity(root / WRITEBROKER_RELATIVE)['sha256'],
    }
    material = {
        'profile': b1, 'modules': modules, 'writebroker': broker_identity,
        'title_root': inventory(folder / 'wine-title'),
        'parent_title': [item for item in inventory(folder)
                         if not item['path'].startswith('wine-title/') and item['path'] not in
                         ('runtime-qualification.json', 'psmkit-wine-runtime-child.bin')],
        'native_parent': {'fself_size': 9, 'fself_sha256': hashlib.sha256(b'published').hexdigest()},
        'native_child': {'fself_size': 9, 'fself_sha256': hashlib.sha256(b'published').hexdigest()},
    }
    receipt = {
        'status': 'QUALIFIED_STATIC_NOT_EXECUTED', 'material': material,
        'qualification_key': hashlib.sha256(json.dumps(
            material, sort_keys=True, separators=(',', ':')).encode()).hexdigest(),
    }
    (folder / 'runtime-qualification.json').write_text(json.dumps(receipt))
    assert qualified_b1_folder(folder) == (receipt, manifest)
    for selected_profile, accepted in ((profile('b1', {'gpu': True, 'headless': True}), False),
                                       ({key: value for key, value in b1.items() if key != 'gpu'}, True)):
        material['profile'] = selected_profile
        receipt['qualification_key'] = hashlib.sha256(json.dumps(
            material, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
        (folder / 'runtime-qualification.json').write_text(json.dumps(receipt))
        try:
            assert qualified_b1_folder(folder) == (receipt, manifest)
        except ValueError:
            assert not accepted, 'a receipt without gpu was refused'
        else:
            assert accepted, 'a gpu receipt passed without winevulkan.so in the tree'
    material['profile'] = b1
    receipt['qualification_key'] = hashlib.sha256(json.dumps(
        material, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    (folder / 'runtime-qualification.json').write_text(json.dumps(receipt))
    (folder / 'sce_module/libc.prx').write_bytes(b'corrupted')
    try:
        qualified_b1_folder(folder)
    except ValueError:
        pass
    else:
        raise AssertionError('qualification accepted corrupted title bytes')
    (folder / 'sce_module/libc.prx').write_bytes(b'published')
    receipt['qualification_key'] = '00' * 32
    (folder / 'runtime-qualification.json').write_text(json.dumps(receipt))
    try:
        qualified_b1_folder(folder)
    except ValueError:
        pass
    else:
        raise AssertionError('qualification accepted a forged material digest')
    try:
        (folder / 'wine').mkdir()
        (folder / 'wine/bad.dll').write_bytes(b'bad')
        b1_guard_paths(folder, '/data/homebrew/PPSA99030')
    except ValueError:
        pass
    else:
        raise AssertionError('B1 publication accepted a payload write')
    extra = root / 'lib/wine/x86_64-unix/kerberos.so'
    extra.write_bytes(b'not admitted')
    try:
        runtime_tree_manifest(root, b1)
    except ValueError:
        pass
    else:
        raise AssertionError('B1 tree accepted an additional Unix module')

print('RUNTIME_PROFILE_HOST_PASS root=SEPARATE_TITLE_ONLY guards=NO_PAYLOAD_WRITES modules=EXACT_PROFILE console=NOT_CONTACTED')
