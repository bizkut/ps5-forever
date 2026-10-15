#!/usr/bin/env python3
"""Publish independently qualified maintenance and game parents as one title."""
import argparse
import hashlib
import json
import shutil
from pathlib import Path

from runtime_profiles import WRITEBROKER_RELATIVE


def identity(path, root):
    data = path.read_bytes()
    return {'path': path.relative_to(root).as_posix(), 'size': len(data),
            'sha256': hashlib.sha256(data).hexdigest()}


def inventory(root):
    return [identity(path, root) for path in sorted(root.rglob('*')) if path.is_file()]

def title_inventory(root):
    return [identity(path, root) for path in sorted(root.rglob('*'))
            if path.is_file() and not path.relative_to(root).parts[0] == 'wine-title']


parser = argparse.ArgumentParser()
parser.add_argument('--maintenance', type=Path, required=True)
parser.add_argument('--game', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
a = parser.parse_args()
maintenance = a.maintenance.resolve()
game = a.game.resolve()
output = a.output.resolve()
for folder in (maintenance, game):
    if folder.is_symlink() or not folder.is_dir():
        raise SystemExit(f'unsafe qualified input: {folder}')
    receipt = json.loads((folder / 'runtime-qualification.json').read_text())
    material = receipt['material']
    key = hashlib.sha256(json.dumps(material, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    if receipt.get('status') != 'QUALIFIED_STATIC_NOT_EXECUTED' or receipt.get('qualification_key') != key:
        raise SystemExit(f'unqualified input: {folder}')
maintenance_receipt = json.loads((maintenance / 'runtime-qualification.json').read_text())
game_receipt = json.loads((game / 'runtime-qualification.json').read_text())
if maintenance_receipt['material']['profile'].get('gpu'):
    raise SystemExit('maintenance input is a GPU profile')
if not game_receipt['material']['profile'].get('gpu'):
    raise SystemExit('game input is not a GPU profile')
maintenance_writebroker = maintenance_receipt['material'].get('writebroker')
game_writebroker = game_receipt['material'].get('writebroker')
if (not isinstance(maintenance_writebroker, dict) or
        maintenance_writebroker != game_writebroker):
    raise SystemExit('maintenance/game write broker data identity differs')
for folder, writebroker in (
        (maintenance, maintenance_writebroker), (game, game_writebroker)):
    actual = identity(folder / 'wine-title' / WRITEBROKER_RELATIVE,
                      folder / 'wine-title')
    expected = {
        'path': writebroker.get('path'),
        'size': writebroker.get('size'),
        'sha256': writebroker.get('sha256'),
    }
    if actual != expected:
        raise SystemExit(f'qualified write broker data changed: {folder}')
stage = output.with_name(output.name + '-dual-stage')
if stage.exists(): shutil.rmtree(stage)
if output.exists(): shutil.rmtree(output)
shutil.copytree(game, stage)
(stage / 'eboot.bin').write_bytes((maintenance / 'eboot.bin').read_bytes())
(stage / 'psmkit-wine-runtime-child.bin').write_bytes(
    (maintenance / 'psmkit-wine-runtime-child.bin').read_bytes())
(stage / 'game.bin').write_bytes((game / 'eboot.bin').read_bytes())
(stage / 'game-child.bin').write_bytes((game / 'psmkit-wine-runtime-child.bin').read_bytes())
(stage / 'sce_sys/param.json').write_bytes((game / 'sce_sys/param.json').read_bytes())
(stage / 'maintenance-qualification.json').write_text(json.dumps(maintenance_receipt, indent=2, sort_keys=True) + '\n')
(stage / 'game-qualification.json').write_text(json.dumps(game_receipt, indent=2, sort_keys=True) + '\n')
(stage / 'runtime-qualification.json').unlink()
material = {
    'profile': maintenance_receipt['material']['profile'],
    'game_profile': game_receipt['material']['profile'],
    'maintenance_qualification_key': maintenance_receipt['qualification_key'],
    'game_qualification_key': game_receipt['qualification_key'],
    'writebroker': maintenance_writebroker,
    'parent_title': title_inventory(stage),
    'title_root': inventory(stage / 'wine-title'),
    'native_child': maintenance_receipt['material']['native_child'],
    'native_parent': maintenance_receipt['material']['native_parent'],
    'modules': game_receipt['material']['modules'],
}
receipt = {
    'format': 3,
    'status': 'QUALIFIED_STATIC_NOT_EXECUTED',
    'topology': 'MAINTENANCE_GAME_LOADEXEC_V1',
    'material': material,
    'qualification_key': hashlib.sha256(json.dumps(material, sort_keys=True, separators=(',', ':')).encode()).hexdigest(),
}
(stage / 'runtime-qualification.json').write_text(json.dumps(receipt, indent=2, sort_keys=True) + '\n')
stage.replace(output)
print(f'RUNTIME_TITLE_DUAL_QUALIFIED maintenance={maintenance_receipt["qualification_key"]} game={game_receipt["qualification_key"]} combined={receipt["qualification_key"]}')
