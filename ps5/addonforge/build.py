#!/usr/bin/env python3
"""Patch, vendor, test, and reproducibly build the pinned AddonForge core."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

TOOLCHAIN = '1.99.1'
TARGET = 'x86_64-pc-windows-gnullvm'
UPSTREAM = '0dc9ab28f207f86e33770e359f2345043ef19585'


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def run(command, **kwargs):
    print('+', ' '.join(map(str, command)), flush=True)
    subprocess.run([str(value) for value in command], check=True, **kwargs)


def verified_vendor(root):
    manifests = sorted(root.glob('*/.cargo-checksum.json'))
    if not manifests:
        return False
    try:
        for manifest_path in manifests:
            manifest = json.loads(manifest_path.read_text())
            crate = manifest_path.parent
            for relative, expected in manifest.get('files', {}).items():
                path = crate / relative
                if not path.is_file() or sha256(path) != expected:
                    return False
    except (OSError, ValueError, TypeError):
        return False
    return True


def vendor(source, cache, cargo_home):
    lock_digest = sha256(source / 'src-tauri/Cargo.lock')
    root = cache / 'addonforge-vendor-v1' / lock_digest
    complete = root / '.complete'
    if complete.is_file() and complete.read_text().strip() == lock_digest and verified_vendor(root / 'crates'):
        print(f'ADDONFORGE_VENDOR_HIT key={lock_digest[:12]}', flush=True)
        return root / 'crates', lock_digest
    shutil.rmtree(root, ignore_errors=True)
    temporary = root.with_name(f'.{root.name}.{os.getpid()}.tmp')
    shutil.rmtree(temporary, ignore_errors=True)
    temporary.mkdir(parents=True)
    environment = os.environ.copy()
    environment['CARGO_HOME'] = str(cargo_home)
    run(['/opt/cargo/bin/cargo', 'vendor', '--locked', '--versioned-dirs',
         temporary / 'crates', '--manifest-path', source / 'src-tauri/Cargo.toml'],
        env=environment, cwd=source)
    if not verified_vendor(temporary / 'crates'):
        raise SystemExit('cargo vendor output failed checksum verification')
    (temporary / '.complete').write_text(lock_digest + '\n')
    root.parent.mkdir(parents=True, exist_ok=True)
    os.replace(temporary, root)
    print(f'ADDONFORGE_VENDOR_STORE key={lock_digest[:12]}', flush=True)
    return root / 'crates', lock_digest


def cargo_config(source, vendor_root):
    config = source / '.cargo/config.toml'
    config.parent.mkdir()
    escaped = str(vendor_root).replace('\\', '\\\\').replace('"', '\\"')
    config.write_text(
        '[source.crates-io]\nreplace-with = "vendored-sources"\n\n'
        '[source.vendored-sources]\ndirectory = "' + escaped + '"\n\n'
        '[net]\noffline = true\n')


def cargo_environment(cargo_home, target_dir):
    environment = os.environ.copy()
    environment.update({
        'CARGO_HOME': str(cargo_home),
        'RUSTUP_HOME': '/opt/rustup',
        'CARGO_TARGET_DIR': str(target_dir),
        'CARGO_TARGET_X86_64_PC_WINDOWS_GNULLVM_LINKER':
            '/opt/llvm-mingw/bin/x86_64-w64-mingw32-clang',
        'CC_x86_64_pc_windows_gnullvm':
            '/opt/llvm-mingw/bin/x86_64-w64-mingw32-clang',
        'AR_x86_64_pc_windows_gnullvm': '/opt/llvm-mingw/bin/llvm-ar',
        'SOURCE_DATE_EPOCH': '0',
    })
    return environment


def cargo_build(source, cargo_home, target_dir):
    environment = cargo_environment(cargo_home, target_dir)
    logical = '/usr/src/addonforge'
    environment['RUSTFLAGS'] = (
        '-C target-feature=+crt-static -C debuginfo=0 '
        '-C link-arg=-Wl,--no-insert-timestamp '
        f'--remap-path-prefix={source}={logical}')
    run(['/opt/cargo/bin/cargo', 'build', '--offline', '--locked', '--release',
         '--target', TARGET, '--no-default-features', '--features', 'headless',
         '--bin', 'addonforge-core', '--manifest-path', source / 'src-tauri/Cargo.toml'],
        env=environment, cwd=source)
    return target_dir / TARGET / 'release/addonforge-core.exe'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--project', type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    source = args.source.resolve()
    output = args.output.resolve()
    project = args.project.resolve()
    work = output.with_name(output.name + '-source')
    fetch_home = args.cache.resolve() / 'cargo-home'
    offline_home = output.with_name(output.name + '-cargo-offline-home')
    shutil.rmtree(work, ignore_errors=True)
    shutil.rmtree(output, ignore_errors=True)
    shutil.copytree(source, work, ignore=shutil.ignore_patterns('.git', 'target'))
    patches = sorted((project / 'ps5/addonforge/patches').glob('*.patch'))
    if not patches:
        raise SystemExit('no AddonForge patches found')
    for patch in patches:
        run(['patch', '--batch', '--fuzz=0', '-p1', '-i', patch], cwd=work)

    fetch_home.mkdir(parents=True, exist_ok=True)
    vendor_root, lock_digest = vendor(work, args.cache.resolve(), fetch_home)
    cargo_config(work, vendor_root)
    shutil.rmtree(offline_home, ignore_errors=True)
    offline_home.mkdir()

    test_environment = cargo_environment(
        offline_home, output.with_name(output.name + '-host-tests'))
    run(['/opt/cargo/bin/cargo', 'test', '--offline', '--locked', '--no-default-features',
         '--features', 'headless', '--lib', '--manifest-path', work / 'src-tauri/Cargo.toml'],
        env=test_environment, cwd=work)

    first_dir = output.with_name(output.name + '-cold-a')
    second_dir = output.with_name(output.name + '-cold-b')
    shutil.rmtree(first_dir, ignore_errors=True)
    shutil.rmtree(second_dir, ignore_errors=True)
    first = cargo_build(work, offline_home, first_dir)
    second = cargo_build(work, offline_home, second_dir)
    first_hash, second_hash = sha256(first), sha256(second)
    if first_hash != second_hash:
        raise SystemExit(f'AddonForge cold builds differ: {first_hash} != {second_hash}')

    output.mkdir(parents=True)
    shutil.copy2(first, output / 'addonforge-core.exe')
    compiler = '/opt/llvm-mingw/bin/x86_64-w64-mingw32-clang'
    run([compiler, '-std=c11', '-Os', '-s', '-static', '-Wall', '-Wextra', '-Werror',
         '-Wl,--no-insert-timestamp', project / 'ps5/addonforge/forever-maintenance.c',
         '-o', output / 'forever-maintenance.exe', '-lws2_32'])
    shutil.copy2(work / 'LICENSE', output / 'ADDONFORGE-LICENSE.txt')
    notice = output / 'THIRD-PARTY-NOTICES.txt'
    run([project / 'ps5/addonforge/generate-notices.py',
         '--manifest', work / 'src-tauri/Cargo.toml', '--output', notice],
        env=cargo_environment(offline_home, output.with_name(output.name + '-metadata')),
        cwd=work)
    patch_hashes = {patch.name: sha256(patch) for patch in patches}
    receipt = {
        'format': 1,
        'upstream_commit': UPSTREAM,
        'rust_toolchain': TOOLCHAIN,
        'target': TARGET,
        'cargo_lock_sha256': lock_digest,
        'patches': patch_hashes,
        'cold_build_a_sha256': first_hash,
        'cold_build_b_sha256': second_hash,
        'reproducible': first_hash == second_hash,
        'offline_empty_cargo_home': True,
        'addonforge_core': {
            'sha256': first_hash,
            'size': first.stat().st_size,
        },
        'maintenance_launcher': {
            'sha256': sha256(output / 'forever-maintenance.exe'),
            'size': (output / 'forever-maintenance.exe').stat().st_size,
        },
        'third_party_notices_sha256': sha256(notice),
    }
    (output / 'build-receipt.json').write_text(json.dumps(receipt, indent=2, sort_keys=True) + '\n')
    print(f'ADDONFORGE_REPRODUCIBLE sha256={first_hash} size={first.stat().st_size} '
          f'toolchain={TOOLCHAIN} target={TARGET}', flush=True)
    shutil.rmtree(first_dir)
    shutil.rmtree(second_dir)
    shutil.rmtree(output.with_name(output.name + '-host-tests'), ignore_errors=True)
    shutil.rmtree(output.with_name(output.name + '-metadata'), ignore_errors=True)
    shutil.rmtree(offline_home, ignore_errors=True)
    shutil.rmtree(work)


if __name__ == '__main__':
    main()
