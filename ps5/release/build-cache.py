#!/usr/bin/env python3
"""Content-keyed, runner-local cache operations for the Linux release build."""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import subprocess
import sys
import time

SCHEMA = 1
CACHE_DIRECTORY = "forever-build-v1"


def fail(message):
    raise SystemExit(message)


def cache_root(path):
    return Path(path).resolve() / CACHE_DIRECTORY


def safe_relative(value):
    path = PurePosixPath(value)
    if path.is_absolute() or not path.parts or ".." in path.parts:
        fail(f"unsafe cache output path: {value}")
    return path


def remove_path(path):
    if path.is_symlink() or path.is_file():
        path.unlink()
    elif path.exists():
        shutil.rmtree(path)


def atomic_json(path, value):
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    os.replace(temporary, path)


def hash_records(records):
    digest = hashlib.sha256()
    for kind, label, payload in records:
        fields = (kind.encode(), label.encode(), payload)
        for field in fields:
            digest.update(len(field).to_bytes(8, "big"))
            digest.update(field)
    return digest.hexdigest()


def path_records(path, source_root):
    try:
        label = path.resolve().relative_to(source_root).as_posix()
    except ValueError:
        label = str(path.resolve())
    if path.is_symlink():
        yield "symlink", label, os.readlink(path).encode()
    elif path.is_file():
        yield "file", label, path.read_bytes()
    elif path.is_dir():
        yield "directory", label, b""
        for child in sorted(path.rglob("*"), key=lambda item: item.as_posix()):
            relative_parts = child.relative_to(path).parts
            if ".git" in relative_parts or "__pycache__" in relative_parts:
                continue
            child_label = f"{label}/{child.relative_to(path).as_posix()}"
            if child.is_symlink():
                yield "symlink", child_label, os.readlink(child).encode()
            elif child.is_file():
                yield "file", child_label, child.read_bytes()
            elif child.is_dir():
                yield "directory", child_label, b""
    else:
        yield "missing", label, b""


def command_key(args):
    source_root = Path(args.source_root).resolve()
    manifest = json.loads(Path(args.manifest).read_text())
    sources = manifest.get("sources", [])
    selected = []
    exact = set(args.source)
    prefixes = tuple(args.source_prefix)
    for source in sources:
        name = source["name"]
        if name in exact or any(name == prefix or name.startswith(prefix + "/") for prefix in prefixes):
            selected.append(source)
    missing = exact - {source["name"] for source in selected}
    if missing:
        fail("unknown dependency source(s): " + ", ".join(sorted(missing)))
    downloads = []
    by_download = {item["name"]: item for item in manifest.get("downloads", [])}
    for name in args.download:
        if name not in by_download:
            fail(f"unknown dependency download: {name}")
        downloads.append(by_download[name])
    records = [
        ("schema", "cache-key-schema", str(SCHEMA).encode()),
        ("stage", "stage", args.stage.encode()),
        ("sources", "dependency-sources",
         json.dumps(selected, sort_keys=True, separators=(",", ":")).encode()),
        ("downloads", "dependency-downloads",
         json.dumps(downloads, sort_keys=True, separators=(",", ":")).encode()),
    ]
    for value in args.value:
        if "=" not in value:
            fail(f"cache key value must be NAME=VALUE: {value}")
        name, text = value.split("=", 1)
        records.append(("value", name, text.encode()))
    for value in args.path:
        records.extend(path_records(Path(value), source_root))
    print(hash_records(records))


def file_manifest(root):
    entries = []
    if not root.is_dir():
        fail(f"manifest root is not a directory: {root}")
    for path in sorted(root.rglob("*"), key=lambda item: item.as_posix()):
        relative = path.relative_to(root).as_posix()
        info = path.lstat()
        mode = stat.S_IMODE(info.st_mode)
        if stat.S_ISLNK(info.st_mode):
            entries.append({"path": relative, "kind": "symlink", "target": os.readlink(path),
                            "mode": mode})
        elif stat.S_ISREG(info.st_mode):
            digest = hashlib.sha256()
            with path.open("rb") as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(block)
            entries.append({"path": relative, "kind": "file", "sha256": digest.hexdigest(),
                            "size": info.st_size, "mode": mode})
        elif stat.S_ISDIR(info.st_mode):
            entries.append({"path": relative, "kind": "directory", "mode": mode})
        else:
            fail(f"unsupported cached file type: {path}")
    return entries


def verify_file_manifest(root, expected):
    try:
        actual = file_manifest(root)
    except (OSError, SystemExit) as error:
        return False, str(error)
    if actual == expected:
        return True, ""
    actual_by_path = {item["path"]: item for item in actual}
    expected_by_path = {item["path"]: item for item in expected}
    for path in sorted(set(actual_by_path) | set(expected_by_path)):
        if actual_by_path.get(path) != expected_by_path.get(path):
            return False, f"manifest mismatch at {path}"
    return False, "manifest mismatch"


def stage_entry(args):
    return cache_root(args.cache) / "stages" / args.stage / args.key


def touch_entry(entry):
    marker = entry / ".last-used"
    marker.touch()


def activate_outputs(entry, source_root, outputs):
    cached_root = entry / "outputs"
    for value in outputs:
        relative = safe_relative(value)
        destination = source_root / Path(*relative.parts)
        cached = cached_root / Path(*relative.parts)
        if not cached.exists() and not cached.is_symlink():
            fail(f"cache entry is missing output: {relative}")
        destination.parent.mkdir(parents=True, exist_ok=True)
        remove_path(destination)
        destination.symlink_to(os.path.relpath(cached, destination.parent))


def command_restore(args):
    entry = stage_entry(args)
    manifest_path = entry / "manifest.json"
    if not manifest_path.is_file():
        return 1
    try:
        manifest = json.loads(manifest_path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        print(f"CACHE_INVALID {args.stage} key={args.key[:12]} reason={error}")
        remove_path(entry)
        return 1
    expected_header = {"schema": SCHEMA, "stage": args.stage, "key": args.key,
                       "outputs": args.output}
    if any(manifest.get(name) != value for name, value in expected_header.items()):
        print(f"CACHE_INVALID {args.stage} key={args.key[:12]} reason=identity")
        remove_path(entry)
        return 1
    valid, reason = verify_file_manifest(entry / "outputs", manifest.get("files", []))
    if not valid:
        print(f"CACHE_INVALID {args.stage} key={args.key[:12]} reason={reason}")
        remove_path(entry)
        return 1
    activate_outputs(entry, Path(args.source_root).resolve(), args.output)
    touch_entry(entry)
    return 0


def command_clear(args):
    source_root = Path(args.source_root).resolve()
    for value in args.output:
        relative = safe_relative(value)
        remove_path(source_root / Path(*relative.parts))


def command_store(args):
    source_root = Path(args.source_root).resolve()
    entry = stage_entry(args)
    stage_root = entry.parent
    stage_root.mkdir(parents=True, exist_ok=True)
    if entry.exists():
        remove_path(entry)
    temporary = stage_root / f".tmp-{args.key}-{os.getpid()}"
    remove_path(temporary)
    output_root = temporary / "outputs"
    output_root.mkdir(parents=True)
    moved = []
    try:
        for value in args.output:
            relative = safe_relative(value)
            source = source_root / Path(*relative.parts)
            if source.is_symlink() or not source.exists():
                fail(f"cache output was not built as a real path: {relative}")
            destination = output_root / Path(*relative.parts)
            destination.parent.mkdir(parents=True, exist_ok=True)
            os.rename(source, destination)
            moved.append((source, destination))
        manifest = {"schema": SCHEMA, "stage": args.stage, "key": args.key,
                    "outputs": args.output, "files": file_manifest(output_root)}
        atomic_json(temporary / "manifest.json", manifest)
        (temporary / ".last-used").touch()
        os.replace(temporary, entry)
    except BaseException:
        for source, destination in reversed(moved):
            if destination.exists() or destination.is_symlink():
                source.parent.mkdir(parents=True, exist_ok=True)
                os.rename(destination, source)
        remove_path(temporary)
        raise
    activate_outputs(entry, source_root, args.output)


def wine_entry(args):
    return cache_root(args.cache) / "wine" / args.key


def read_json(path):
    try:
        return json.loads(path.read_text())
    except (OSError, json.JSONDecodeError):
        return None


def wine_paths(entry, source_root, directories, files):
    output_root = entry / "outputs"
    for value in directories:
        relative = safe_relative(value)
        cached = output_root / Path(*relative.parts)
        cached.mkdir(parents=True, exist_ok=True)
    for value in files:
        relative = safe_relative(value)
        cached = output_root / Path(*relative.parts)
        cached.parent.mkdir(parents=True, exist_ok=True)
    for value in [*directories, *files]:
        relative = safe_relative(value)
        destination = source_root / Path(*relative.parts)
        cached = output_root / Path(*relative.parts)
        destination.parent.mkdir(parents=True, exist_ok=True)
        remove_path(destination)
        destination.symlink_to(os.path.relpath(cached, destination.parent))


def command_wine_prepare(args):
    entry = wine_entry(args)
    state_path = entry / "state.json"
    state = read_json(state_path)
    hit = bool(state and state.get("schema") == SCHEMA and state.get("key") == args.key and
               state.get("configured") is True and (entry / "build-manifest.json").is_file())
    if hit:
        manifest = read_json(entry / "build-manifest.json")
        valid = bool(manifest and manifest.get("schema") == SCHEMA)
        reason = "invalid build manifest"
        if valid:
            valid, reason = verify_file_manifest(entry / "outputs", manifest.get("files", []))
        if not valid:
            print(f"CACHE_INVALID wine key={args.key[:12]} reason={reason}")
            hit = False
    if not hit:
        remove_path(entry)
        entry.parent.mkdir(parents=True, exist_ok=True)
        temporary = entry.parent / f".tmp-{args.key}-{os.getpid()}"
        remove_path(temporary)
        (temporary / "outputs").mkdir(parents=True)
        atomic_json(temporary / "state.json", {"schema": SCHEMA, "key": args.key,
                                                "configured": False})
        (temporary / ".last-used").touch()
        os.replace(temporary, entry)
    wine_paths(entry, Path(args.source_root).resolve(), args.directory, args.file)
    touch_entry(entry)
    return 0 if hit else 1


def command_wine_mark_configured(args):
    entry = wine_entry(args)
    state = read_json(entry / "state.json")
    if not state or state.get("key") != args.key:
        fail("Wine cache entry identity changed before configure completed")
    state["configured"] = True
    atomic_json(entry / "state.json", state)


def git_source_index(source):
    data = subprocess.check_output(["git", "-C", str(source), "ls-files", "--stage", "-z"])
    result = {}
    for record in data.split(b"\0"):
        if not record:
            continue
        metadata, path = record.split(b"\t", 1)
        mode, object_id, stage = metadata.decode().split()
        if stage == "0":
            result[path.decode(errors="surrogateescape")] = f"{mode} {object_id}"
    return result


def command_wine_sync_source(args):
    entry = wine_entry(args)
    state = read_json(entry / "state.json")
    if not state or state.get("key") != args.key:
        fail("Wine cache entry identity changed before source synchronization")
    previous = state.get("source_index", {})
    current = git_source_index(Path(args.source))
    changed = sorted(path for path, identity in current.items() if previous.get(path) != identity)
    now = time.time()
    for relative in changed:
        path = Path(args.source) / relative
        if path.exists() and not path.is_symlink():
            os.utime(path, (now, now))
    print(f"WINE_INCREMENTAL_SOURCE changed={len(changed)} removed={len(set(previous) - set(current))}")


def generated_header_paths(source):
    return [source / "include/wine/server_protocol.h",
            source / "server/request_handlers.h",
            source / "server/request_trace.h"]


def command_wine_restore_headers(args):
    entry = wine_entry(args)
    header_root = entry / "generated-headers"
    manifest = read_json(entry / "generated-headers.json")
    if not manifest:
        return 1
    valid, reason = verify_file_manifest(header_root, manifest.get("files", []))
    if not valid:
        print(f"CACHE_INVALID wine-headers key={args.key[:12]} reason={reason}")
        remove_path(header_root)
        remove_path(entry / "generated-headers.json")
        return 1
    source = Path(args.source)
    for path in generated_header_paths(source):
        shutil.copy2(header_root / path.relative_to(source), path)
    return 0


def command_wine_save_headers(args):
    entry = wine_entry(args)
    source = Path(args.source)
    temporary = entry / f".generated-headers-{os.getpid()}"
    remove_path(temporary)
    for path in generated_header_paths(source):
        destination = temporary / path.relative_to(source)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, destination)
    manifest = {"schema": SCHEMA, "files": file_manifest(temporary)}
    remove_path(entry / "generated-headers")
    os.replace(temporary, entry / "generated-headers")
    atomic_json(entry / "generated-headers.json", manifest)


def command_wine_commit(args):
    entry = wine_entry(args)
    state = read_json(entry / "state.json")
    if not state or state.get("key") != args.key or not state.get("configured"):
        fail("Wine cache entry was not configured before commit")
    state["source_index"] = git_source_index(Path(args.source))
    manifest = {"schema": SCHEMA, "files": file_manifest(entry / "outputs")}
    atomic_json(entry / "build-manifest.json", manifest)
    atomic_json(entry / "state.json", state)
    touch_entry(entry)


def entry_last_used(path):
    marker = path / ".last-used"
    try:
        return marker.stat().st_mtime_ns
    except OSError:
        return path.stat().st_mtime_ns


def prune_group(group, keep):
    if not group.is_dir():
        return
    entries = [path for path in group.iterdir() if path.is_dir() and not path.name.startswith(".tmp-")]
    for path in sorted(entries, key=entry_last_used, reverse=True)[keep:]:
        remove_path(path)
    for path in group.glob(".tmp-*"):
        remove_path(path)


def command_prune(args):
    root = cache_root(args.cache)
    stages = root / "stages"
    if stages.is_dir():
        for group in stages.iterdir():
            if group.is_dir():
                prune_group(group, args.keep)
    prune_group(root / "wine", args.keep)


def command_size(args):
    root = cache_root(args.cache)
    total = 0
    if root.exists():
        for path in root.rglob("*"):
            try:
                if not path.is_symlink() and path.is_file():
                    total += path.stat().st_size
            except FileNotFoundError:
                pass
    print(f"CACHE_SIZE bytes={total} root={Path(args.cache).resolve()}")


def add_cache_argument(parser):
    parser.add_argument("--cache", required=True)


def add_stage_arguments(parser, outputs=False):
    add_cache_argument(parser)
    parser.add_argument("--stage", required=True)
    parser.add_argument("--key", required=True)
    parser.add_argument("--source-root", required=True)
    if outputs:
        parser.add_argument("--output", action="append", default=[], required=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    key = subparsers.add_parser("key")
    key.add_argument("--stage", required=True)
    key.add_argument("--source-root", required=True)
    key.add_argument("--manifest", required=True)
    key.add_argument("--source", action="append", default=[])
    key.add_argument("--source-prefix", action="append", default=[])
    key.add_argument("--download", action="append", default=[])
    key.add_argument("--path", action="append", default=[])
    key.add_argument("--value", action="append", default=[])
    key.set_defaults(function=command_key)

    restore = subparsers.add_parser("restore")
    add_stage_arguments(restore, outputs=True)
    restore.set_defaults(function=command_restore)

    clear = subparsers.add_parser("clear")
    clear.add_argument("--source-root", required=True)
    clear.add_argument("--output", action="append", default=[], required=True)
    clear.set_defaults(function=command_clear)

    store = subparsers.add_parser("store")
    add_stage_arguments(store, outputs=True)
    store.set_defaults(function=command_store)

    prepare = subparsers.add_parser("wine-prepare")
    add_cache_argument(prepare)
    prepare.add_argument("--key", required=True)
    prepare.add_argument("--source-root", required=True)
    prepare.add_argument("--directory", action="append", default=[])
    prepare.add_argument("--file", action="append", default=[])
    prepare.set_defaults(function=command_wine_prepare)

    for name, function in (("wine-mark-configured", command_wine_mark_configured),
                           ("wine-sync-source", command_wine_sync_source),
                           ("wine-restore-headers", command_wine_restore_headers),
                           ("wine-save-headers", command_wine_save_headers),
                           ("wine-commit", command_wine_commit)):
        command = subparsers.add_parser(name)
        add_cache_argument(command)
        command.add_argument("--key", required=True)
        if name != "wine-mark-configured":
            command.add_argument("--source", required=True)
        command.set_defaults(function=function)

    prune = subparsers.add_parser("prune")
    add_cache_argument(prune)
    prune.add_argument("--keep", type=int, default=2)
    prune.set_defaults(function=command_prune)

    size = subparsers.add_parser("size")
    add_cache_argument(size)
    size.set_defaults(function=command_size)

    args = parser.parse_args()
    result = args.function(args)
    raise SystemExit(result or 0)


if __name__ == "__main__":
    main()
