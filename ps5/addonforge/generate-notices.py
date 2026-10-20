#!/usr/bin/env python3
"""Generate deterministic Rust dependency notices from offline Cargo metadata."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    command = [
        '/opt/cargo/bin/cargo', 'metadata', '--offline', '--locked', '--format-version', '1',
        '--no-default-features', '--features', 'headless', '--manifest-path', str(args.manifest),
    ]
    metadata = json.loads(subprocess.check_output(
        command, text=True, cwd=args.manifest.parent))
    root = args.manifest.parent.resolve()
    packages = []
    texts = {}
    for package in metadata['packages']:
        package_root = Path(package['manifest_path']).parent.resolve()
        if package_root == root:
            continue
        license_files = []
        for path in sorted(package_root.iterdir(), key=lambda item: item.name.lower()):
            upper = path.name.upper()
            if (upper.startswith(('LICENSE', 'LICENCE', 'COPYING', 'NOTICE')) and
                    path.is_file() and path.stat().st_size <= 256 * 1024):
                data = path.read_bytes()
                digest = hashlib.sha256(data).hexdigest()
                texts.setdefault(digest, data.decode('utf-8', errors='replace').rstrip())
                license_files.append((path.name, digest))
        packages.append({
            'name': package['name'],
            'version': package['version'],
            'license': package.get('license') or 'not declared',
            'files': license_files,
        })
    packages.sort(key=lambda item: (item['name'].lower(), item['version']))
    lines = [
        'AddonForge headless core — Rust dependency notices',
        'Generated from Cargo metadata and the checksum-verified vendored crate sources.',
        '',
        'Packages:',
    ]
    for package in packages:
        files = ', '.join(f'{name} [{digest[:12]}]' for name, digest in package['files']) or 'no licence file in crate package'
        lines.append(f"- {package['name']} {package['version']} — {package['license']} — {files}")
    lines.extend(['', 'Licence texts:'])
    used = sorted({digest for package in packages for _, digest in package['files']})
    for digest in used:
        owners = [
            f"{package['name']} {package['version']}"
            for package in packages if any(file_digest == digest for _, file_digest in package['files'])
        ]
        lines.extend([
            '',
            '=' * 78,
            f"SHA-256: {digest}",
            'Used by: ' + ', '.join(owners),
            '-' * 78,
            texts[digest],
        ])
    catalogue_license = Path(__file__).with_name('github-catalogue-LICENSE.txt').read_text()
    lines.extend([
        '', '=' * 78,
        'Bundled data: bizkut/github-wow-addon-catalogue',
        'Copyright layday, bizkut and github-wow-addon-catalogue contributors.',
        'Source: https://github.com/bizkut/github-wow-addon-catalogue',
        'Based on: https://github.com/layday/github-wow-addon-catalogue',
        'Catalogue: https://raw.githubusercontent.com/bizkut/github-wow-addon-catalogue/main/addons.csv',
        'Licence: GNU GPL version 3 (GPL-3.0). The fork expands and refreshes the data.',
        'The exact catalogue snapshot SHA-256 is recorded in build-receipt.json.',
        '-' * 78,
        catalogue_license.rstrip(),
    ])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix(args.output.suffix + '.tmp')
    temporary.write_text('\n'.join(lines).rstrip() + '\n')
    os.replace(temporary, args.output)


if __name__ == '__main__':
    main()
