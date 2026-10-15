#!/usr/bin/env python3
"""Build title RADV from an isolated pinned Mesa export, never its working tree."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile

HERE = Path(__file__).resolve().parent
WOW = HERE.parents[1]
VULKAN = WOW.parents[1] / 'PS5_Vulkan'
DEFAULT_REVISION = '15a5e9933e14f6493a7853e8cf1df973dd093eeb'
DEFAULT_PATCHES = sorted((HERE / 'patches').glob('*.patch'))
REVISION = os.environ.get('PS5_RADV_REVISION', DEFAULT_REVISION)
# PS5_RADV_PATCH: one patch, or several separated by os.pathsep, instead of
# every ps5/radv/patches/*.patch in name order.
PATCHES = ([Path(p).resolve() for p in os.environ['PS5_RADV_PATCH'].split(os.pathsep) if p]
           if 'PS5_RADV_PATCH' in os.environ else DEFAULT_PATCHES)
PATCH_SHA256 = hashlib.sha256(b''.join(hashlib.sha256(p.read_bytes()).digest() for p in PATCHES)).hexdigest()
INSTALL = Path(os.environ.get('PS5_RADV_INSTALL', str(WOW / 'ps5/build/radv-release-patched'))).resolve()
WORK = WOW / 'ps5/build/radv-patched'
if REVISION != DEFAULT_REVISION or PATCHES != DEFAULT_PATCHES or 'PS5_RADV_INSTALL' in os.environ:
    # Separate Meson configuration/build-id and objects for each experiment.
    work_identity = hashlib.sha256((REVISION + ':' + PATCH_SHA256 + ':' + str(INSTALL)).encode()).hexdigest()[:16]
    WORK = WORK.with_name(WORK.name + '-' + work_identity)
ARCHIVE = INSTALL / 'lib/libvulkan_radeon.ps5.a'


def run(args, **kwargs):
    subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def main():
    mesa = Path(os.environ.get('PS5_MESA_FORK', str(VULKAN.parent / 'PS5_Mesa')))
    sdk = VULKAN / '.deps/native/ps5-payload-sdk'
    identity = {'revision': REVISION, 'patch_sha256': PATCH_SHA256,
                'patches': [p.name for p in PATCHES],
                'sdk': (sdk / '.ps5-sdk-revision').read_text().strip()}
    receipt = INSTALL / 'PROVENANCE.json'
    if ARCHIVE.is_file() and receipt.is_file():
        saved = json.loads(receipt.read_text())
        if saved == {**identity, 'archive_sha256': hashlib.sha256(ARCHIVE.read_bytes()).hexdigest()}:
            print(f'RADV_PATCHED_ARCHIVE={ARCHIVE}')
            return
    WORK.mkdir(parents=True, exist_ok=True)
    source = WORK / 'src'
    # Patch a fresh git archive, then content-sync it to preserve Ninja's incremental build.
    with tempfile.TemporaryDirectory(dir=WORK) as temporary:
        staging = Path(temporary)
        with tempfile.TemporaryFile() as stream:
            run(['git', '-C', mesa, 'archive', REVISION], stdout=stream)
            stream.seek(0)
            with tarfile.open(fileobj=stream) as archive:
                archive.extractall(staging, filter='data')
        # The staging folder sits inside ps5-wow's work tree; without a
        # ceiling, git apply resolves `diff --git` paths against ps5-wow and
        # silently skips them.
        apply_env = dict(os.environ, GIT_CEILING_DIRECTORIES=str(staging.parent))
        for patch in PATCHES:
            result = subprocess.run(['git', 'apply', '-v', str(patch)], cwd=staging, env=apply_env,
                                    capture_output=True, text=True)
            if result.returncode or 'Skipped patch' in result.stderr:
                raise SystemExit(f'{patch.name} did not apply:\n{result.stderr}')
        source.mkdir(exist_ok=True)
        run(['rsync', '-rlp', '--checksum', '--delete', '--exclude=/subprojects/packagecache/',
             '--exclude=/subprojects/zlib-*/', str(staging) + '/', str(source) + '/'])
    constants = WORK / 'cross-constants.ini'
    constants.write_text(f"[constants]\nsdk = '{sdk}'\n")
    env = dict(os.environ)
    # The native LLVM, translator and libclc must share a major version.
    # LLVM_CONFIG lets hosts with multiple LLVM installations select it.
    if 'LLVM_CONFIG' not in env and shutil.which('llvm-config'):
        env['LLVM_CONFIG'] = shutil.which('llvm-config')
    clc_build = WORK / 'clc-build'
    clc_bin = WORK / 'clc-bin'
    meson = os.environ.get('MESON', 'meson')
    ninja = os.environ.get('NINJA', 'ninja')
    # Leave Ninja's default parallelism unchanged unless JOBS is supplied.
    ninja_jobs = ['-j', env['JOBS']] if env.get('JOBS') else []
    native = WORK / 'native.ini'
    llvm_config = env.get('LLVM_CONFIG', shutil.which('llvm-config'))
    if llvm_config:
        native.write_text(f"[binaries]\nllvm-config = '{llvm_config}'\n")
    native_options = ['--native-file', native] if llvm_config else []
    if not (clc_build / 'build.ninja').is_file():
        run([meson, 'setup', clc_build, source, *native_options, '-Dbuildtype=release', '-Dmesa-clc=enabled',
             '-Dinstall-mesa-clc=true', '-Dgallium-drivers=', '-Dvulkan-drivers=', '-Dplatforms=',
             '-Dglx=disabled', '-Degl=disabled', '-Dgbm=disabled', '-Dopengl=false',
             '-Dgles1=disabled', '-Dgles2=disabled', '-Dllvm=enabled', '-Dshared-llvm=enabled',
             '-Dbuild-tests=false', '-Dvalgrind=disabled', '-Dlibunwind=disabled', '-Dzstd=disabled',
             '-Dxmlconfig=disabled', '-Dtools='], env=env)
    run([ninja, *ninja_jobs, '-C', clc_build, 'src/compiler/clc/mesa_clc', 'src/compiler/spirv/vtn_bindgen2'], env=env)
    clc_bin.mkdir(exist_ok=True)
    for name, path in (('mesa_clc', 'clc/mesa_clc'), ('vtn_bindgen2', 'spirv/vtn_bindgen2')):
        link = clc_bin / name
        if not link.exists():
            link.symlink_to(clc_build / 'src/compiler' / path)
    env['PATH'] = str(clc_bin) + ':' + env['PATH']
    build = WORK / 'build'
    # A new default base must refresh the configured build ID too.
    if (build / 'build.ninja').is_file():
        run([meson, 'configure', build, '-Dradv-build-id=' + REVISION], env=env)
    options = ['-Dvulkan-drivers=amd', '-Dgallium-drivers=', '-Dplatforms=', '-Dradv-winsys=ps5',
               '-Dllvm=disabled', '-Damd-use-llvm=false', '-Dvideo-codecs=',
               '-Dbuildtype=debugoptimized', '-Db_ndebug=true', '-Dglx=disabled', '-Degl=disabled',
               '-Dgbm=disabled', '-Dopengl=false', '-Dgles1=disabled', '-Dgles2=disabled',
               '-Dvalgrind=disabled', '-Dlibunwind=disabled', '-Dzstd=disabled', '-Dzlib=enabled',
               '--force-fallback-for=zlib', '-Dexpat=disabled', '-Dxmlconfig=disabled',
               '-Dshader-cache=enabled', '-Dbuild-tests=false', '-Dvulkan-layers=', '-Dtools=',
               '-Dmesa-clc=system', '-Dradv-build-id=' + REVISION]
    if not (build / 'build.ninja').is_file():
        run([os.environ.get('MESON', 'meson'), 'setup', build, source, '--cross-file', constants,
             '--cross-file', VULKAN / 'tooling/radv/ps5-cross.ini', *options], env=env)
    run([ninja, *ninja_jobs, '-C', build, 'src/amd/vulkan/libvulkan_radeon.a'], env=env)
    ARCHIVE.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(build / 'src/amd/vulkan/libvulkan_radeon.a', ARCHIVE)
    (INSTALL / 'include').mkdir(exist_ok=True)
    for folder in ('vulkan', 'vk_video'):
        shutil.copytree(source / 'include' / folder, INSTALL / 'include' / folder, dirs_exist_ok=True)
    receipt.write_text(json.dumps({**identity, 'archive_sha256': hashlib.sha256(ARCHIVE.read_bytes()).hexdigest()},
                                 indent=2) + '\n')
    print(f'RADV_PATCHED_ARCHIVE={ARCHIVE}')


if __name__ == '__main__':
    main()
