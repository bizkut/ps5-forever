#!/usr/bin/env python3
"""Package a qualified B1 build as the homebrew catalog's artifact.

    python3 ps5/release/package.py --tag 01.000.000 [--title DIR] [--out DIR]

Writes <out>/PPSA99030.zip with the PPSA99030/ app folder at its top, the
same bytes for the same build: sorted entries, fixed times and modes. The
tag must be the content version (NN.NNN.NNN) in both the source and the
built sce_sys/param.json. Qualification receipts stay out (they are build
records, not read by the app). The qualified dual runtime must match the
checked-in publication inventory; release notices are added only to the ZIP.
"""
import argparse
import hashlib
import json
import re
import sys
import zipfile
from pathlib import Path, PurePosixPath

HERE = Path(__file__).resolve().parent
PS5 = HERE.parent
sys.path.insert(0, str(PS5 / "probes/psmkit-wine"))
from runtime_profiles import qualified_b1_folder

TITLE_ID = "PPSA99030"
LIMIT = 2 << 30            # the catalog's artifact limit
EPOCH = (1980, 1, 1, 0, 0, 0)

# Private build evidence is validated, but never distributed.
RECEIPTS = frozenset(("runtime-qualification.json", "maintenance-qualification.json",
                      "game-qualification.json"))
REFUSED = re.compile(r"(^|/)(lossless\.dll|drive_c|dosdevices|prefix|home|[^/]*\.reg)(/|$)", re.IGNORECASE)


def content_version(param: Path) -> str:
    data = json.loads(param.read_text())
    if data.get("titleId") != TITLE_ID:
        raise SystemExit(f"{param}: titleId is {data.get('titleId')!r}, not {TITLE_ID}")
    return data.get("contentVersion", "")


def directory_files(root: Path):
    if root.is_symlink() or not root.is_dir():
        raise SystemExit(f"unsafe app directory: {root}")
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise SystemExit(f"symbolic link in the app: {path}")
        if path.is_file():
            yield path.relative_to(root).as_posix(), path
        elif not path.is_dir():
            raise SystemExit(f"not a regular app file: {path}")


def safe_relative(name: str) -> bool:
    path = PurePosixPath(name)
    return (bool(name) and not path.is_absolute() and
            all(part not in ("", ".", "..") for part in name.split("/")) and
            "\\" not in name)


def members(title: Path):
    inventory = HERE / "runtime-files.txt"
    names = [line for line in inventory.read_text().splitlines()
             if line and not line.startswith("#")]
    if (not names or len(names) != len(set(names)) or
            any(not safe_relative(name) or name in RECEIPTS for name in names)):
        raise SystemExit(f"invalid runtime publication inventory: {inventory}")
    expected = set(names)
    actual = dict(directory_files(title))
    published = actual.keys() - RECEIPTS
    if published != expected:
        raise SystemExit("runtime publication inventory differs: "
                         f"unexpected={sorted(published - expected)} "
                         f"missing={sorted(expected - published)}")
    for name in sorted(expected):
        if REFUSED.search(name):
            raise SystemExit(f"refusing to ship {name}")
        yield name, actual[name]


def release_members():
    root = HERE / "licenses"
    actual = dict(directory_files(root))
    provenance = json.loads((root / "provenance.json").read_text())
    names = provenance["files"].keys()
    if (any(not safe_relative(name) for name in names) or
            set(actual) != set(names) | {"provenance.json"} or
            "GPL-3.0.txt" not in names):
        raise SystemExit("release licence assets differ from provenance.json")
    notice = HERE / "NOTICE.md"
    if notice.is_symlink() or not notice.is_file():
        raise SystemExit(f"missing public notice: {notice}")
    yield "LICENSE", actual["GPL-3.0.txt"]
    yield "NOTICE.md", notice
    for name, path in sorted(actual.items()):
        yield f"licenses/{name}", path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tag", required=True, help="release tag = contentVersion, e.g. 01.000.000")
    parser.add_argument("--title", type=Path, default=PS5 / "build/title-psmkit-wine-b1")
    parser.add_argument("--out", type=Path, default=PS5 / "build/release")
    args = parser.parse_args()

    if not re.fullmatch(r"\d\d\.\d\d\d\.\d\d\d", args.tag):
        raise SystemExit(f"tag {args.tag!r} is not a content version (NN.NNN.NNN)")
    if args.title.is_symlink() or not args.title.is_dir():
        raise SystemExit(f"unsafe app directory: {args.title}")
    title = args.title.resolve()
    # Walk before reading parameters or receipts, including symlink directory roots.
    entries = list(members(title))
    for param in (PS5 / "title/sce_sys/param.json", title / "sce_sys/param.json"):
        version = content_version(param)
        if version != args.tag:
            raise SystemExit(f"{param}: contentVersion {version!r} is not the tag {args.tag}")
    try:
        receipt, _ = qualified_b1_folder(title)
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise SystemExit(f"unqualified B1 build: {error}") from error
    if (receipt.get("topology") != "MAINTENANCE_GAME_LOADEXEC_V1" or
            not receipt["material"].get("game_profile")):
        raise SystemExit("the build is not a qualified maintenance/game dual runtime")
    runtime_names = {name for name, _ in entries}
    entries = sorted(entries + list(release_members()))

    args.out.mkdir(parents=True, exist_ok=True)
    zip_path = args.out / f"{TITLE_ID}.zip"
    temporary = zip_path.with_suffix(".zip.part")
    with zipfile.ZipFile(temporary, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, path in entries:
            info = zipfile.ZipInfo(f"{TITLE_ID}/{name}", EPOCH)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            executable = name in runtime_names and path.stat().st_mode & 0o111
            info.external_attr = (0o100755 if executable else 0o100644) << 16
            with path.open("rb") as source, archive.open(info, "w") as target:
                while chunk := source.read(1 << 20):
                    target.write(chunk)
    size = temporary.stat().st_size
    if size >= LIMIT:
        temporary.unlink()
        raise SystemExit(f"{zip_path.name} is {size} bytes, over the catalog's 2 GiB")
    temporary.replace(zip_path)
    digest = hashlib.sha256()
    with zip_path.open("rb") as source:
        while chunk := source.read(1 << 20):
            digest.update(chunk)
    print(f"{zip_path} files={len(entries)} bytes={size} sha256={digest.hexdigest()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
