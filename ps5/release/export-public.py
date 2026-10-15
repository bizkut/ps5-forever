#!/usr/bin/env python3
"""Export reviewed, history-free release sources; never publish or modify a checkout.

Usage: export-public.py SOURCE_ROOT OUTPUT_ROOT
SOURCE_ROOT has the layout in deps.json. OUTPUT_ROOT must not exist. Run from
committed sources after the container build scripts have been integrated.
External repositories retain their upstream licence files and submodule pins.
"""
import argparse
import io
import json
import subprocess
import tarfile
from pathlib import Path, PurePosixPath

PRODUCT_DIRS = (
    'ps5/probes/psmkit-wine/', 'ps5/probes/localproc/', 'ps5/title/',
    'ps5/writebroker/', 'ps5/radv/', 'ps5/include/', 'ps5/release/',
)
PRODUCT_FILES = {
    'ps5/build-common.sh', 'ps5/paths.sh', 'ps5/build-wine.sh',
    'ps5/build-freetype.sh', 'ps5/build-gnutls.sh', 'ps5/build-dxvk.sh',
    'ps5/build-vkd3d-proton.sh', 'ps5/wine-ps5.lock', 'ps5/deploy-wine.py',
    'ps5/tools/ps5-capture.py', 'ps5/tools/test-ps5-capture.py',
    '.github/workflows/release.yml', 'scripts/check-secrets.sh',
}


def private_path(name):
    parts = PurePosixPath(name).parts
    return (any(part in {'.tokensave', '.omp', '.claude', 'evidence', '__pycache__'} for part in parts) or
            name.startswith(('ps5/build/', 'ps5/probes/download0/', 'docs/PS5_', 'docs/ps5-')) or
            name.endswith(('.pyc', '.klog')) or name == 'PS5_FORK.md' or
            PurePosixPath(name).name.startswith('.env'))


def product_path(name):
    if name in PRODUCT_FILES:
        return True
    if not name.startswith(PRODUCT_DIRS):
        return False
    # Historical operational READMEs contain private run details. The public
    # README/BUILDING/NOTICE are generated from the reviewed release templates.
    if name.startswith('ps5/probes/') and PurePosixPath(name).name == 'README.md':
        return name.startswith('ps5/probes/psmkit-wine/third_party/')
    return True


def git(source, *args):
    return subprocess.check_output(['git', '-C', str(source), *args])


def export(source, revision, destination, product=False):
    revision = git(source, 'rev-parse', revision + '^{commit}').decode().strip()
    archive = git(source, 'archive', '--format=tar', revision)
    operational = {
        'PS5_Vulkan': ('docs/', 'jobs/', 'golden/', 'dist/', 'Klog_Logs/', '.deps/', 'README.md'),
        'PS5_PayloadSDK': ('platform/docs/', 'README.md', 'platform/README.md'),
        'PS5_Mesa': ('README.md',),
        'pacbrew-repo': ('LOVE_PS5_PLAN.md',),
    }.get(source.name, ())
    count = 0
    with tarfile.open(fileobj=io.BytesIO(archive)) as contents:
        for member in contents:
            name = member.name.rstrip('/')
            if private_path(name) or (product and not product_path(name)):
                continue
            if any(name == prefix.rstrip('/') or name.startswith(prefix) for prefix in operational):
                continue  # private operational docs and captures, not build source
            # No source tree may escape its new directory. tar's data filter
            # also verifies symlink targets; no special device files allowed.
            contents.extract(member, destination, filter='data')
            count += member.isfile()
    gitlinks = []
    for record in git(source, 'ls-tree', '-rz', revision).split(b'\0'):
        if not record:
            continue
        metadata, path = record.split(b'\t', 1)
        mode, kind, digest = metadata.decode().split()
        path = path.decode()
        if mode == '160000' and not private_path(path) and (not product or product_path(path)):
            gitlinks.append({'path': path, 'commit': digest})
    return revision, count, gitlinks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source_root', type=Path)
    parser.add_argument('output_root', type=Path)
    args = parser.parse_args()
    source_root = args.source_root.resolve()
    output = args.output_root.resolve()
    if output.exists():
        parser.error('output root already exists; choose a fresh destination')
    product = source_root / 'ps5-proton/ps5-wow'
    deps = json.loads((product / 'ps5/release/deps.json').read_text())
    sources = {source['name']: source for source in deps['sources']}
    plan = [
        ('ps5-forever', product, 'HEAD', True),
        ('ps5-forever-wine', source_root / sources['ps5-wine']['path'], sources['ps5-wine']['commit'], False),
        ('ps5-forever-vkd3d-proton', source_root / sources['vkd3d-proton']['path'], sources['vkd3d-proton']['commit'], False),
        ('ps5-forever-dxvk', source_root / sources['dxvk']['path'], sources['dxvk']['commit'], False),
        ('ps5-forever-vulkan', source_root / sources['PS5_Vulkan']['path'], sources['PS5_Vulkan']['commit'], False),
        ('ps5-forever-mesa', source_root / sources['PS5_Mesa']['path'], deps['radv_revision'], False),
        ('ps5-forever-sdk', source_root / sources['PS5_PayloadSDK']['path'], deps['sdk_revision'], False),
        ('ps5-forever-pacbrew', source_root / sources['pacbrew-repo']['path'], sources['pacbrew-repo']['commit'], False),
    ]
    output.mkdir(parents=True)
    records = []
    for name, source, revision, is_product in plan:
        destination = output / name
        destination.mkdir()
        original, count, gitlinks = export(source, revision, destination, is_product)
        records.append({'repository': name, 'source_commit': original, 'files': count, 'gitlinks': gitlinks})
        print(f'EXPORT {name} source={original} files={count}', flush=True)
        if source.name in {'PS5_Vulkan', 'PS5_Mesa', 'PS5_PayloadSDK'}:
            (destination / 'README.md').write_text(
                f'# {name}\n\n'
                f'Source snapshot used by [Forever Launcher](https://github.com/bizkut/ps5-forever).\n'
                f'Original source revision: `{original}`.\n\n'
                'Build instructions and dependency pins are in the launcher repository. '
                'Private development history and console evidence are not included.\n\n'
                "Based on mihawk-99's PS5 port and its upstream projects; "
                'all source copyright and licence notices are retained. '
                'See the source licence files for component-specific terms.\n')
    public = output / 'ps5-forever'
    for filename in ('README.md', 'NOTICE.md', 'BUILDING.md'):
        template = public / 'ps5/release' / filename
        (public / filename).write_bytes(template.read_bytes())
    (public / '.gitignore').write_bytes((public / 'ps5/release/public.gitignore').read_bytes())
    # Keep the Proton licensing notices for adapted build tooling, while the
    # combined launcher is licensed GPL-3.0-or-later (stated in README/NOTICE).
    for filename in ('LICENSE', 'LICENSE.proton', 'dist.LICENSE'):
        name = 'LICENSE.proton-top' if filename == 'LICENSE' else filename
        (public / name).write_bytes(git(product, 'show', f'HEAD:{filename}'))
    gpl = source_root / sources['vkd3d-proton']['path'] / 'libs/vkd3d/wineps5_fsr3fg/COPYING'
    (public / 'LICENSE').write_bytes(gpl.read_bytes())
    (output / 'export-provenance.json').write_text(json.dumps(records, indent=2) + '\n')


if __name__ == '__main__':
    main()
