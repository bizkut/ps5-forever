#!/usr/bin/env python3
"""Upload the staged Wine tree to the PS5 and start a Windows program with it.

  ps5/deploy-wine.py upload            ps5/build/stage$WINE_ROOT -> $WINE_ROOT (ps5/paths.sh)
  ps5/deploy-wine.py upload --verify   hash on the console; replace differing or missing files
      first build the read-only payload with: make -C ps5/probes/sha256tree
  ps5/deploy-wine.py run PROGRAM [ARG...]
      write $WINE_ROOT/boot.txt and send the Wine loader to the ELF
      loader (port 9021); prints what the loader streams back (Wine's stdout
      and stderr) until the connection closes or --timeout passes. The
      console's klog (port 3232) is saved for the whole run, with Wine's
      output, under ps5/build/runs/<time>.{klog,out}.
      --prefix PATH selects a disposable prefix; --env HOME=PATH and --cwd
      select its home and working directory. Registry checks use that prefix,
      and never restore the real prefix's registry into an alternate one.
      --loader PATH selects the local payload ELF; default: the staged Wine
      loader. This does not change Wine's installed console path.
  ps5/deploy-wine.py upload-title DIR  ps5/title/build.sh's DIR -> /data/homebrew/$TITLE_ID
      (eboot.bin, sce_sys, sce_module beside wine/; nothing is deleted)
  ps5/deploy-wine.py title launch|kill|restart
      start or close the title through ../../PS5_Vulkan's resident agent ps5vkctl
      (port 9111), which acts only on the title id it is given; it is sent to
      the ELF loader first if it does not answer
  ps5/deploy-wine.py title-backup RUN --paths-from BEFORE.json
      require title count=0, disable FTP SELF conversion, and make a fresh
      byte/hash/absence backup at ps5/build/runs/RUN-backup/before.json
  ps5/deploy-wine.py title-restore RUN
      stop Wine, restore that backup byte-exactly, delete originally absent
      disposable trees through a saved root-constrained inventory, and require
      title count=0
  ps5/deploy-wine.py runtime-title-run RUN --paths-from BEFORE.json --until REGEX
      one bounded PsmKit-Wine transaction: backup, WoW64 prefix readiness,
      guarded upload verification, one run-title launch, and guaranteed restore

PROGRAM is a console path under $WINE_ROOT, Wine's Z: drive
($WINE_ROOT/tests/m3/m3test.exe), or a Windows path. Everything Wine uses on
the console lives in $WINE_ROOT (/data/homebrew/PPSA99030/wine, inside the
launcher title's folder): the install (bin, lib, share), the prefix (prefix),
HOME (home) and test programs (tests).
Files whose size and previously uploaded hash match are skipped on plain upload.
--verify hashes the console tree through the ELF loader, without FTP downloads.
Host: PS5_HOST (default 10.0.1.41), FTP_PORT (2121), LOADER_PORT (9021), KLOG_PORT (3232).
"""
import argparse
import hashlib
import json
import posixpath
import signal
import os
import subprocess
import socket
import sys
import threading
import time
from ftplib import FTP, error_perm, error_reply
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "probes/psmkit-wine"))
from runtime_profiles import (
    profile, b1_title_files, b1_guard_paths, qualified_b1_folder, file_identity,
)
# TITLE_ID from ps5/paths.sh: Wine lives in the launcher title's folder.
TITLE_ID = next(line.split("=", 1)[1].strip() for line in (HERE / "paths.sh").read_text().splitlines()
                if line.startswith("TITLE_ID="))
REMOTE = f"/data/homebrew/{TITLE_ID}/wine"
STAGE = HERE / "build/stage" / REMOTE.lstrip("/")
LOADER = REMOTE + "/lib/wine/x86_64-unix/wine"
HOST = os.environ.get("PS5_HOST", "10.0.1.41")
FTP_PORT = int(os.environ.get("FTP_PORT", "2121"))
LOADER_PORT = int(os.environ.get("LOADER_PORT", "9021"))
KLOG_PORT = int(os.environ.get("KLOG_PORT", "3232"))
RUNS = HERE / "build/runs"

ENV = [
    f"WINEPREFIX={REMOTE}/prefix",
    f"HOME={REMOTE}/home",
    "USER=ps5",
    "PATH=/usr/bin:/bin",
    # winedbg: a crash must not start a debugger nobody can use, which on
    # the console multiplied processes until kernel page tables ran out.
    # msv1_0 (NTLM) and kerberos: Windows domain logins nothing here uses;
    # neither is built (no ntlm_auth, --without-krb5), and schannel's start
    # loaded each, failed and unloaded it, then hit a SceLibc heap error.
    # tabtip.exe, which explorer starts on every desktop for a touch keyboard,
    # queries the focus through UI Automation, which turns on Chromium's
    # accessibility; those queries came right before each breakpoint crash of
    # Battle.net's renderer that reloaded the page (2026-10-04). The title
    # has its own keyboard.
    "WINEDLLOVERRIDES=winemenubuilder.exe=d;mscoree=d;mshtml=d;winedbg.exe=d;msv1_0=d;kerberos=d;tabtip.exe=d",
]
WINDOWS_RUNTIME_MODULES = (
    ("x86_64-windows", "system32",
     ("ntdll", "wow64", "wow64cpu", "wow64win", "kernelbase", "kernel32", "win32u")),
    # user32 and its static import closure, for the PE32 win32k step.
    ("i386-windows", "syswow64", ("ntdll", "kernelbase", "kernel32", "win32u", "user32",
                                  "gdi32", "advapi32", "sechost", "msvcrt", "ucrtbase")),
)

def matching_windows_runtime(wine_build, prefix):
    files = []
    for bits, destination, names in WINDOWS_RUNTIME_MODULES:
        for name in names:
            local = wine_build / "dlls" / name / bits / f"{name}.dll"
            if not local.is_file():
                raise RuntimeError(f"matching Windows runtime is missing: {local}")
            files.append((local,
                          f"{prefix}/drive_c/windows/{destination}/{name}.dll",
                          f"{REMOTE}/lib/wine/{bits}/{name}.dll"))
        start = wine_build / "programs/start" / bits / "start.exe"
        if not start.is_file():
            raise RuntimeError(f"matching Windows runtime is missing: {start}")
        files.append((start,
                      f"{prefix}/drive_c/windows/{destination}/start.exe",
                      f"{REMOTE}/lib/wine/{bits}/start.exe"))
    return files




def connect():
    ftp = FTP()
    ftp.connect(HOST, FTP_PORT, timeout=30)
    ftp.login()
    return ftp


def remote_sizes(ftp, path):
    sizes = {}
    try:
        lines = []
        ftp.retrlines(f"LIST {path}", lines.append)
        for line in lines:
            parts = line.split(None, 8)
            if len(parts) == 9 and not parts[0].startswith("d"):
                sizes[parts[8]] = int(parts[4])
    except error_perm:
        pass
    return sizes


def mkdirs(ftp, path, made):
    parts = path.strip("/").split("/")
    for i in range(1, len(parts) + 1):
        p = "/" + "/".join(parts[:i])
        if p in made:
            continue
        try:
            ftp.mkd(p)
        except error_perm:
            pass
        made.add(p)


MANIFEST = HERE / "build/uploaded-sha256.json"
SHA256TREE = HERE / "probes/sha256tree/sha256tree.elf"


def remote_hashes():
    import re
    from io import BytesIO
    if not SHA256TREE.exists():
        raise SystemExit(f"missing {SHA256TREE}: build ps5/probes/sha256tree")
    output = BytesIO()
    send_payload(SHA256TREE.read_bytes(), 600, output, r"(?m)^sha256tree: end files=\d+\r?$", echo=False)
    hashes, count = {}, None
    for line in output.getvalue().decode(errors="replace").splitlines():
        if line.startswith("sha256tree: error:"):
            raise SystemExit("sha256tree verification failed; see payload output")
        match = re.fullmatch(r"sha256tree: ([0-9a-f]{64}) (\d+) (.+)", line)
        if match:
            digest, size, path = match.groups()
            hashes[path] = (digest, int(size))
        else:
            match = re.fullmatch(r"sha256tree: end files=(\d+)", line)
            if match:
                count = int(match[1])
    if count is None or count != len(hashes):
        raise SystemExit("incomplete sha256tree output; upload not started")
    return hashes


def upload(args):
    # A rebuilt file can keep its size, so a file is skipped only when the
    # console has its size and this host uploaded these exact bytes before.
    # A console that crashes can lose recent writes while keeping the file
    # size; --verify hashes the console tree locally, without FTP downloads.
    import hashlib
    import json
    manifest = json.loads(MANIFEST.read_text()) if MANIFEST.exists() else {}
    hashes = remote_hashes() if args.verify else {}
    ftp = connect()
    made, sent, skipped, damaged, total = set(), 0, 0, 0, 0
    for root, dirs, files in os.walk(STAGE):
        rel = os.path.relpath(root, STAGE)
        rdir = REMOTE if rel == "." else f"{REMOTE}/{rel}"
        mkdirs(ftp, rdir, made)
        sizes = {} if args.verify else remote_sizes(ftp, rdir)
        for name in sorted(files):
            local = Path(root) / name
            if local.is_symlink():
                continue  # the bin/ launch links point at a script the PS5 cannot run
            data = local.read_bytes()
            digest = hashlib.sha256(data).hexdigest()
            remote = f"{rdir}/{name}"
            same = (hashes.get(remote) == (digest, len(data)) if args.verify else
                    sizes.get(name) == len(data) and manifest.get(remote) == digest)
            if same:
                if args.verify:
                    manifest[remote] = digest
                skipped += 1
                continue
            if args.verify and remote in hashes:
                damaged += 1
                print(f"differs on the console: {remote}")
            with local.open("rb") as f:
                ftp.storbinary(f"STOR {remote}", f)
            manifest[remote] = digest
            sent += 1
            total += len(data)
    ftp.quit()
    MANIFEST.write_text(json.dumps(manifest, indent=0, sort_keys=True))
    print(f"uploaded {sent} files ({total / 1e6:.1f} MB), {skipped} already there"
          + (f", {damaged} damaged copies replaced" if args.verify else ""))


TITLE_ROOT = f"/data/homebrew/{TITLE_ID}"
TITLE_LAST = ("eboot.bin", "sce_sys/param.json")
CONTROL_PORT = int(os.environ.get("PS5VKCTL_PORT", "9111"))
CONTROL_ELF = HERE.parent.parent.parent / "PS5_Vulkan/build/ps5vkctl/ps5vkctl.elf"


def upload_title(args):
    """Upload a built title folder (ps5/title/build.sh's OUT_DIR) into the
    title's folder, beside wine/: each file under a temporary name, renamed
    once complete, eboot.bin and param.json last. Nothing is deleted."""
    local = Path(args.folder)
    files = getattr(args, "files", None)
    if files is None:
        files = sorted(p.relative_to(local).as_posix() for p in local.rglob("*") if p.is_file())
    if not set(TITLE_LAST) <= set(files):
        raise SystemExit(f"{local} has no {' or '.join(TITLE_LAST)}")
    ftp = connect()
    made = set()
    for rel in [f for f in files if f not in TITLE_LAST] + list(TITLE_LAST):
        remote = f"{TITLE_ROOT}/{rel}"
        temporary = remote.rsplit("/", 1)[0] + "/.upload-" + rel.rsplit("/", 1)[-1]
        mkdirs(ftp, remote.rsplit("/", 1)[0], made)
        with (local / rel).open("rb") as f:
            ftp.storbinary(f"STOR {temporary}", f)
        try:
            ftp.delete(remote)
        except (error_perm, error_reply):
            # missing: error_perm; the console's server answers a delete with
            # 226, which ftplib raises as an unexpected reply
            pass
        ftp.rename(temporary, remote)
    ftp.quit()
    print(f"uploaded {len(files)} title files to {TITLE_ROOT}")


def control(command):
    """One command to ../../PS5_Vulkan's resident agent, ps5vkctl; it is sent to
    the ELF loader first when it does not answer (once per console boot)."""
    def ask():
        with socket.create_connection((HOST, CONTROL_PORT), timeout=30) as s:
            s.sendall((command + "\n").encode())
            s.shutdown(socket.SHUT_WR)
            return b"".join(iter(lambda: s.recv(4096), b"")).decode(errors="replace").strip()
    try:
        return ask()
    except OSError:
        if not CONTROL_ELF.exists():
            raise SystemExit(f"ps5vkctl does not answer and {CONTROL_ELF} is missing")
        s = socket.create_connection((HOST, LOADER_PORT), timeout=10)
        s.sendall(CONTROL_ELF.read_bytes())
        s.close()
        time.sleep(3)
        return ask()


def title(args):
    print(control(f"{args.action} {TITLE_ID}"))

def raw_connect():
    """FTP connection returning raw signed SELF bytes, never decoded files."""
    ftp = connect()
    # SELF toggles the server's decoding of signed SELF files. Its default
    # differs by console (on with 5.50's server, off with 11.60's): toggle
    # until it reports disabled.
    reply = ftp.sendcmd("SELF")
    if "enabled" in reply:
        reply = ftp.sendcmd("SELF")
    if not reply.startswith("226") and "disabled" not in reply:
        ftp.close()
        raise RuntimeError(f"SELF conversion was not disabled: {reply}")
    return ftp, reply


def ftp_bytes(ftp, path):
    from io import BytesIO
    data = BytesIO()
    ftp.retrbinary(f"RETR {path}", data.write)
    return data.getvalue()


def title_backup(args):
    """Fresh raw backup of an existing run's guarded title path set."""
    guards = getattr(args, "guard_paths", None)
    source = {"paths": guards} if guards is not None else json.loads(Path(args.paths_from).read_text())
    source_paths = source.get("paths")
    if not isinstance(source_paths, dict) or not source_paths:
        raise SystemExit(f"{args.paths_from}: missing non-empty paths object")
    source_paths = dict(source_paths)
    extra_paths = set(getattr(args, "extra_paths", ()))
    added_paths = set(source_paths) if guards is not None else extra_paths.difference(source_paths)
    for remote in extra_paths:
        source_paths.setdefault(remote, {})
    procs = control("procs")
    if "count=0" not in procs:
        raise SystemExit(f"title is not idle: {procs}")
    backup = RUNS / f"{args.run}-backup"
    if backup.exists():
        raise SystemExit(f"backup already exists: {backup}")
    backup.mkdir(parents=True)
    ftp, reply = raw_connect()
    paths = {}
    drift = []
    try:
        for remote, previous in source_paths.items():
            if not remote.startswith(TITLE_ROOT + "/") or ".." in Path(remote).parts:
                raise RuntimeError(f"guard path escapes title root: {remote}")
            if previous.get("kind") == "directory":
                lines = []
                try:
                    ftp.retrlines(f"LIST {remote}", lines.append)
                    present = True
                except error_perm:
                    present = False
                paths[remote] = {"present": present, "kind": "directory"}
            else:
                try:
                    data = ftp_bytes(ftp, remote)
                except error_perm as error:
                    paths[remote] = {"present": False, "error": str(error)}
                else:
                    relative = remote.removeprefix(TITLE_ROOT + "/")
                    local = backup / relative
                    local.parent.mkdir(parents=True, exist_ok=True)
                    local.write_bytes(data)
                    paths[remote] = {
                        "present": True, "bytes": len(data),
                        "sha256": hashlib.sha256(data).hexdigest(),
                        "backup": str(local),
                    }
            actual = paths[remote]
            if remote not in added_paths and (
                    actual.get("present") != previous.get("present") or (
                        actual.get("present") and previous.get("sha256") and
                        actual.get("sha256") != previous.get("sha256"))):
                drift.append(remote)
    finally:
        ftp.quit()
    before = {
        "format": 1, "host": HOST, "run": args.run, "raw_ftp_reply": reply,
        "procs": procs, "paths": paths, "diff_vs_source": drift,
        "restore_mode": "TITLE_ONLY" if getattr(args, "title_only", False) else "PAYLOAD_AND_TITLE",
    }
    (backup / "before.json").write_text(json.dumps(before, indent=2, sort_keys=True) + "\n")
    if drift:
        raise SystemExit(f"fresh backup differs from source at: {drift}")
    print(f"title-backup: {backup} present={sum(v['present'] for v in paths.values())} "
          f"absent={sum(not v['present'] for v in paths.values())}")


def _delete_reply_ok(ftp, path):
    try:
        ftp.delete(path)
    except error_reply as error:
        if not str(error).startswith("226"):
            raise


def _inventory_tree(ftp, root):
    files, directories, seen, stack = [], [], set(), [root]
    while stack:
        path = posixpath.normpath(stack.pop())
        if path != root and not path.startswith(root + "/"):
            raise RuntimeError(f"inventory escaped root {root}: {path}")
        if path in seen:
            continue
        seen.add(path)
        directories.append(path)
        lines = []
        try:
            ftp.retrlines(f"LIST {path}", lines.append)
        except error_perm:
            continue
        for line in lines:
            parts = line.split(None, 8)
            if len(parts) != 9:
                raise RuntimeError(f"unparsed LIST row: {line}")
            name = parts[8]
            if name in (".", ".."):
                continue
            child = posixpath.normpath(path + "/" + name)
            if not child.startswith(root + "/"):
                raise RuntimeError(f"inventory child escaped root {root}: {child}")
            (stack if parts[0].startswith("d") else files).append(child)
    return sorted(files), sorted(directories, key=lambda p: (p.count("/"), p), reverse=True)


def title_restore(args):
    """Restore a title-backup and its original absences exactly."""
    backup = RUNS / f"{args.run}-backup"
    before = json.loads((backup / "before.json").read_text())
    paths = before["paths"]
    title_only = getattr(args, "title_only", False) or before.get("restore_mode") == "TITLE_ONLY"
    if title_only:
        if "count=0" not in control("procs"):
            control(f"kill {TITLE_ID}")
        deadline = time.monotonic() + 30
        while "count=0" not in control("procs"):
            if time.monotonic() >= deadline:
                raise RuntimeError("title did not stop before B1 restoration")
            time.sleep(0.25)
    else:
        kill(argparse.Namespace())
    ftp, reply = raw_connect()
    restored, absent, plans = {}, [], {}
    try:
        for remote, metadata in paths.items():
            if metadata.get("kind") == "directory":
                continue
            if metadata["present"]:
                data = Path(metadata["backup"]).read_bytes()
                if title_only and remote == REMOTE + "/boot.txt":
                    try:
                        unchanged = ftp_bytes(ftp, remote) == data
                    except error_perm:
                        unchanged = False
                    if unchanged:
                        continue
                ftp.storbinary(f"STOR {remote}", __import__("io").BytesIO(data), blocksize=1 << 16)
            elif metadata.get("kind") != "directory":
                try:
                    _delete_reply_ok(ftp, remote)
                except error_perm:
                    pass
                absent.append(remote)
        for root, metadata in paths.items():
            if metadata.get("kind") != "directory" or metadata["present"]:
                continue
            files, directories = _inventory_tree(ftp, root)
            plans[root] = {"files": files, "directories_deepest_first": directories}
            for path in files:
                try:
                    _delete_reply_ok(ftp, path)
                except error_perm:
                    pass
            for path in directories:
                try:
                    ftp.rmd(path)
                except (error_perm, error_reply):
                    pass
            absent.append(root)
        for remote, metadata in paths.items():
            if metadata.get("kind") == "directory" and metadata["present"]:
                ftp.retrlines(f"LIST {remote}", lambda line: None)
            elif metadata["present"]:
                data = ftp_bytes(ftp, remote)
                digest = hashlib.sha256(data).hexdigest()
                if digest != metadata["sha256"] or len(data) != metadata["bytes"]:
                    raise RuntimeError(f"restored bytes differ: {remote}")
                restored[remote] = {"sha256": digest, "bytes": len(data)}
            elif metadata.get("kind") == "directory":
                try:
                    lines = []
                    ftp.retrlines(f"LIST {remote}", lines.append)
                    raise RuntimeError(f"directory remains after restore: {remote}")
                except error_perm:
                    pass
            else:
                try:
                    ftp_bytes(ftp, remote)
                    raise RuntimeError(f"file remains after restore: {remote}")
                except error_perm:
                    pass
        # Home-screen copies an install replaced (b1-title-install): back as they were.
        art = backup / "home-art.json"
        for remote, metadata in (json.loads(art.read_text()) if art.exists() else {}).items():
            data = Path(metadata["backup"]).read_bytes()
            ftp.storbinary(f"STOR {remote}", __import__("io").BytesIO(data), blocksize=1 << 16)
            if hashlib.sha256(ftp_bytes(ftp, remote)).hexdigest() != metadata["sha256"]:
                raise RuntimeError(f"restored home-screen art differs: {remote}")
            restored[remote] = {"sha256": metadata["sha256"], "bytes": len(data)}
    finally:
        ftp.quit()
    (backup / "disposable-delete-plan.json").write_text(
        json.dumps({"roots": plans}, indent=2, sort_keys=True) + "\n")
    procs = control("procs")
    if "count=0" not in procs:
        raise RuntimeError(f"title is not idle after restore: {procs}")
    receipt = {
        "format": 1, "run": args.run, "raw_ftp_reply": reply,
        "restored_exact": restored, "original_absences_restored": absent,
        "procs": procs,
    }
    (backup / "final-restoration.json").write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
    print(f"title-restore: {backup} restored={len(restored)} absent={len(absent)} {procs}")


def runtime_title_run(args):
    """One bounded PsmKit-Wine title fixture/deploy/launch/restore transaction."""
    from io import BytesIO
    if not WINEKILL.is_file():
        raise SystemExit(f"missing {WINEKILL}: build ps5/probes/winekill before a title transaction")
    backup = RUNS / f"{args.run}-backup"
    prefix_root = f"{REMOTE}/psmkit-wine-disposable"
    prefix = prefix_root + "/prefix"
    home = prefix_root + "/home"
    wine_build = Path(args.wine_build).resolve()
    loader = wine_build / "loader/wine"
    if not loader.is_file():
        raise SystemExit(f"missing regular payload loader {loader}: rebuild loader/all with "
                         f"ps5/build-wine.sh in --wine-build {wine_build} before a title transaction")
    loader_data = loader.read_bytes()
    loader_metadata = {
        "local": str(loader), "bytes": len(loader_data),
        "sha256": hashlib.sha256(loader_data).hexdigest(),
        "wine_build": str(wine_build),
    }
    windows_runtime = matching_windows_runtime(wine_build, prefix)
    launched = False
    launch_result = None

    folder = Path(args.folder).resolve()
    published = [f"{TITLE_ROOT}/{path.relative_to(folder).as_posix()}"
                 for path in sorted(folder.rglob("*")) if path.is_file()]
    # Every file the transaction writes must be in the guarded backup set.
    title_backup(argparse.Namespace(
        run=args.run, paths_from=args.paths_from,
        extra_paths=[installed for _, _, installed in windows_runtime] + published))
    before = json.loads((backup / "before.json").read_text())
    try:
        prep = subprocess.Popen([
            sys.executable, str(HERE / "watchdog-run.py"), "--max-execs", "40", "--",
            "--prefix", prefix, "--env", f"HOME={home}", "--env", "WINEARCH=win64",
            "--timeout", "240", "--loader", str(loader),
            "C:\\windows\\system32\\wineboot.exe", "--", "-i",
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        required = [
            *(f"{prefix}/{name}" for name in REGISTRY),
            f"{prefix}/.update-timestamp",
            f"{prefix}/drive_c/windows/system32/ntdll.dll",
            f"{prefix}/drive_c/windows/system32/wow64.dll",
            f"{prefix}/drive_c/windows/system32/wow64cpu.dll",
            f"{prefix}/drive_c/windows/syswow64/ntdll.dll",
            f"{prefix}/drive_c/windows/syswow64/kernel32.dll",
        ]
        deadline = time.monotonic() + 180
        fixture = None
        try:
            while time.monotonic() < deadline:
                time.sleep(2)
                try:
                    ftp, reply = raw_connect()
                    values = {path: ftp_bytes(ftp, path) for path in required}
                    ftp.quit()
                    if (all(values[f"{prefix}/{name}"].startswith(b"WINE REGISTRY Version 2\n")
                            for name in REGISTRY) and
                            values[f"{prefix}/.update-timestamp"].strip().isdigit()):
                        fixture = {
                            "format": 2, "readiness": "REGISTRIES_TIMESTAMP_WOW64_DLLS_PRESENT",
                            "raw_ftp_reply": reply,
                            "payload_loader": loader_metadata,
                            "files": {path: {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
                                      for path, data in values.items()},
                        }
                        break
                except (OSError, error_perm, error_reply):
                    pass
            if fixture is None:
                raise RuntimeError("disposable WoW64 prefix did not reach readiness")
        finally:
            prep.terminate()
            try:
                prep.wait(timeout=30)
            except subprocess.TimeoutExpired:
                prep.kill()
                prep.wait()

        kill(argparse.Namespace())
        ftp, _ = raw_connect()
        boot = f"{REMOTE}/boot.txt"
        boot_metadata = before["paths"][boot]
        original_boot = Path(boot_metadata["backup"]).read_bytes()
        fixture["boot_txt_restored_from_backup"] = ftp_bytes(ftp, boot) != original_boot
        if fixture["boot_txt_restored_from_backup"]:
            ftp.storbinary(f"STOR {boot}", BytesIO(original_boot))
        for remote, metadata in before["paths"].items():
            if metadata["present"] and hashlib.sha256(ftp_bytes(ftp, remote)).hexdigest() != metadata["sha256"]:
                raise RuntimeError(f"guard changed before deployment: {remote}")
        matched = {}
        for local, prefix_remote, installed_remote in windows_runtime:
            data = local.read_bytes()
            for remote in (prefix_remote, installed_remote):
                ftp.storbinary(f"STOR {remote}", BytesIO(data), blocksize=1 << 16)
                if ftp_bytes(ftp, remote) != data:
                    raise RuntimeError(f"matching Windows runtime differs after upload: {remote}")
                matched[remote] = {
                    "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                    "local": str(local),
                }
        fixture["matching_windows_runtime"] = matched
        ftp.quit()
        (backup / "fixture-initialization.json").write_text(
            json.dumps(fixture, indent=2, sort_keys=True) + "\n")

        # folder resolved before the backup; its files are guarded paths
        qualification = json.loads((folder / "runtime-qualification.json").read_text())
        upload_title(argparse.Namespace(folder=str(folder)))
        ftp, reply = raw_connect()
        deployed = {}
        for local in sorted(path for path in folder.rglob("*") if path.is_file()):
            relative = local.relative_to(folder).as_posix()
            remote = f"{TITLE_ROOT}/{relative}"
            data = ftp_bytes(ftp, remote)
            if data != local.read_bytes():
                raise RuntimeError(f"published file differs: {remote}")
            deployed[remote] = {
                "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                "local": str(local),
            }
        if hashlib.sha256(ftp_bytes(ftp, boot)).hexdigest() != boot_metadata["sha256"]:
            raise RuntimeError("boot.txt changed before launch")
        ftp.quit()
        (backup / "deployed-all.json").write_text(json.dumps({
            "format": 1, "raw_ftp_reply": reply, "files": deployed,
            "qualification_digest": qualification["qualification_key"],
            "guards": "MATCHED_BEFORE_LAUNCH",
        }, indent=2, sort_keys=True) + "\n")

        command = [
            sys.executable,
            str(HERE.parent.parent.parent / "PS5_Vulkan/tools/run-title.py"),
            TITLE_ID, "--until", args.until, "--timeout", str(args.timeout),
            "--exit-grace", str(args.exit_grace),
            "--output", str(RUNS / f"{args.run}.klog"),
        ]
        if args.echo:
            command += ["--echo", args.echo]
        if args.elf:
            command += ["--elf", args.elf]
        launched = True
        result = subprocess.run(command)
        launch_result = {
            "command": command, "returncode": result.returncode,
            "title_launches": 1, "qualification_digest": qualification["qualification_key"],
        }
        (backup / "launch.json").write_text(json.dumps(launch_result, indent=2) + "\n")
    finally:
        title_restore(argparse.Namespace(run=args.run))
        if launch_result is None:
            (backup / "launch.json").write_text(json.dumps({
                "title_launches": int(launched), "returncode": None,
                "failure": "transaction ended before run-title returned",
            }, indent=2) + "\n")
    if launch_result["returncode"]:
        raise SystemExit(launch_result["returncode"])


def ftp_identity(ftp, remote):
    digest = hashlib.sha256()
    size = 0
    def consume(data):
        nonlocal size
        size += len(data)
        digest.update(data)
    ftp.retrbinary(f"RETR {remote}", consume, blocksize=1 << 16)
    return {"bytes": size, "sha256": digest.hexdigest()}


def title_root_upload(args):
    """Upload only the qualified, separately rooted B1 Wine tree."""
    folder = Path(args.folder).resolve()
    qualification, manifest = qualified_b1_folder(folder)
    expected_files = {item["path"]: {"bytes": item["size"], "sha256": item["sha256"]}
                      for item in qualification["material"]["title_root"]}
    root = profile("b1")["remote_root"]
    if root != TITLE_ROOT + "/wine-title":
        raise SystemExit("B1 profile is only authorized for PPSA99030")
    procs = control("procs")
    if "count=0" not in procs:
        raise SystemExit(f"title is not idle: {procs}")
    ftp, reply = raw_connect()
    verified = {}
    try:
        rows = []
        ftp.retrlines(f"LIST {TITLE_ROOT}", rows.append)
        entries = [row.split(None, 8) for row in rows]
        if any(len(entry) != 9 for entry in entries):
            raise RuntimeError("unparsed title-root LIST reply")
        existing = [entry for entry in entries if entry[8].split(" -> ", 1)[0] == "wine-title"]
        if existing:
            if not args.replace:
                raise SystemExit(f"title root already exists: {root}; use --replace deliberately")
            if len(existing) != 1 or not existing[0][0].startswith("d"):
                raise RuntimeError(f"title root is not a real directory: {root}")
            files, directories = _inventory_tree(ftp, root)
            for remote in files:
                _delete_reply_ok(ftp, remote)
            for directory in directories:
                ftp.rmd(directory)
        made = set()
        files = list(manifest["files"]) + ["title-root-manifest.json"]
        for relative in files:
            local = folder / "wine-title" / relative
            remote = root + "/" + relative
            mkdirs(ftp, posixpath.dirname(remote), made)
            with local.open("rb") as source:
                ftp.storbinary(f"STOR {remote}", source, blocksize=1 << 16)
            identity = ftp_identity(ftp, remote)
            expected = expected_files[relative]
            if identity != expected:
                raise RuntimeError(f"title root upload bytes differ: {remote}")
            verified[remote] = identity
    finally:
        ftp.quit()
    receipt = {"format": 1, "remote_root": root, "raw_ftp_reply": reply,
               "qualification_digest": qualification["qualification_key"],
               "pe_dll_counts": manifest["pe_dll_counts"], "verified": verified,
               "payload_install": "UNTOUCHED"}
    path = HERE / "build/title-root-upload.json"
    path.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
    print(f"title-root-upload: {root} verified={len(verified)} pe_dlls={manifest['pe_dll_counts']} receipt={path}")


def b1_preflight(folder, require_battlenet=True):
    """Read-only checks before B1 title files are published: a qualified folder
    for PPSA99030, an idle title, the uploaded root matching the qualifier,
    and an initialized prefix (update timestamp, registries) holding Battle.net
    unless require_battlenet is false (the title then installs it itself)."""
    qualification, manifest = qualified_b1_folder(folder)
    selected = qualification["material"]["profile"]
    if selected["remote_root"] != TITLE_ROOT + "/wine-title":
        raise SystemExit("B1 profile is only authorized for PPSA99030")
    guards = b1_guard_paths(folder, TITLE_ROOT)
    files = b1_title_files(folder)
    material = qualification["material"]
    expected_title = {item["path"]: {"bytes": item["size"], "sha256": item["sha256"]}
                      for item in material["parent_title"]}
    expected_title["psmkit-wine-runtime-child.bin"] = {
        "bytes": material["native_child"]["fself_size"],
        "sha256": material["native_child"]["fself_sha256"],
    }
    expected_title["runtime-qualification.json"] = file_identity(folder / "runtime-qualification.json")
    # The root must already be uploaded and match the qualifier; the
    # installed application prefix is never booted or updated.
    procs = control("procs")
    if "count=0" not in procs:
        raise SystemExit(f"title is not idle: {procs}")
    ftp, _ = raw_connect()
    try:
        remote_manifest = ftp_bytes(ftp, selected["remote_root"] + "/title-root-manifest.json")
        if remote_manifest != (folder / "wine-title/title-root-manifest.json").read_bytes():
            raise RuntimeError("B1 root manifest differs; run title-root-upload --replace")
        for relative, identity in manifest["files"].items():
            remote = selected["remote_root"] + "/" + relative
            if ftp_identity(ftp, remote) != identity:
                raise RuntimeError(f"B1 root bytes differ: {remote}")
        prefix = selected["prefix"].replace("/app0/", TITLE_ROOT + "/", 1)
        try:
            timestamp = ftp_bytes(ftp, prefix + "/.update-timestamp").strip()
        except error_perm:
            if require_battlenet:
                raise RuntimeError(f"no initialized B1 prefix at {prefix}")
            timestamp = None  # the app's first start creates the prefix itself
        if timestamp is not None:
            if not timestamp or not (timestamp.isdigit() or timestamp.startswith(b"disable")):
                raise RuntimeError("installed B1 prefix has no valid update timestamp")
            for name in REGISTRY:
                if not ftp_bytes(ftp, prefix + "/" + name).startswith(b"WINE REGISTRY Version 2\n"):
                    raise RuntimeError(f"installed B1 prefix has invalid {name}")
        for leaf in ("Battle.net Launcher.exe", "Battle.net.exe") if require_battlenet else ():
            executable = prefix + "/drive_c/Program Files (x86)/Battle.net/" + leaf
            if not ftp_bytes(ftp, executable).startswith(b"MZ"):
                raise RuntimeError(f"installed Battle.net executable is missing or invalid: {executable}")
    finally:
        ftp.quit()
    return qualification, selected, guards, files, expected_title


def b1_verify_published(files, expected_title):
    """Digest every published title file against the qualifier."""
    ftp, reply = raw_connect()
    deployed = {}
    try:
        for relative in files:
            remote = TITLE_ROOT + "/" + relative
            identity = ftp_identity(ftp, remote)
            if identity != expected_title[relative]:
                raise RuntimeError(f"B1 published file differs: {remote}")
            deployed[remote] = identity
    finally:
        ftp.quit()
    return deployed, reply

# The home screen reads the copies ShadowMountPlus made of sce_sys when it
# registered the title (once), not the title's own folder. The app installs
# its icon there itself, through the write broker (WB_ICON_MAGIC,
# psmkit-wine/title-icon.h); this replaces the backgrounds after a title
# install. Only existing copies are replaced.
HOME_ART = {
    "pic0.dds": ("/user/app/{id}/sce_sys/pic0.dds", "/user/appmeta/{id}/pic0.dds"),
    "pic1.dds": ("/user/app/{id}/sce_sys/pic1.dds", "/user/appmeta/{id}/pic1.dds"),
}


def install_home_art(run, art):
    """Replace the home screen's existing copies of each name in art (bytes),
    keeping every replaced copy in the run's backup (home-art.json), which
    title-restore puts back. Returns {remote: sha256 written}."""
    from io import BytesIO
    backup = RUNS / f"{run}-backup"
    store = backup / "home-art"
    store.mkdir(parents=True, exist_ok=True)
    record, written = {}, {}
    ftp, _ = raw_connect()
    try:
        for name, data in art.items():
            for pattern in HOME_ART[name]:
                remote = pattern.format(id=TITLE_ID)
                try:
                    old = ftp_bytes(ftp, remote)
                except error_perm:
                    continue
                saved = store / remote.strip("/").replace("/", "__")
                saved.write_bytes(old)
                record[remote] = {"backup": str(saved), "sha256": hashlib.sha256(old).hexdigest()}
                (backup / "home-art.json").write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
                ftp.storbinary(f"STOR {remote}", BytesIO(data), blocksize=1 << 16)
                digest = hashlib.sha256(ftp_bytes(ftp, remote)).hexdigest()
                if digest != hashlib.sha256(data).hexdigest():
                    raise RuntimeError(f"home-screen art differs after upload: {remote}")
                written[remote] = digest
    finally:
        ftp.quit()
    return written


def b1_title_install(args):
    """Make the qualified B1 title PPSA99030's program: the home-screen icon then
    runs Battle.net inside the title, on the title's own direct memory, instead
    of the launcher title's payload Wine, whose memory comes from the console's
    and froze it twice (2026-10-04, 2026-10-08). Nothing is launched and
    nothing is restored afterwards; `title-restore RUN` puts the previous title
    files and home-screen art back. A prefix without Battle.net is accepted:
    the title then downloads and runs Blizzard's installer itself."""
    folder = Path(args.folder).resolve()
    qualification, selected, guards, files, expected_title = b1_preflight(folder, require_battlenet=False)
    prefix = selected["prefix"].replace("/app0/", TITLE_ROOT + "/", 1)
    home = selected["home"].replace("/app0/", TITLE_ROOT + "/", 1)
    # The app owns its prefix files, Battle.net's SwiftShader manifest, the
    # first-start prefix in /data/forever (forever-setup.h) and its home-screen
    # icon (title-icon.h): install the title and the backgrounds only.
    backup = RUNS / f"{args.run}-backup"
    title_backup(argparse.Namespace(run=args.run, guard_paths=guards, title_only=True))
    try:
        upload_title(argparse.Namespace(folder=str(folder), files=files))
        deployed, reply = b1_verify_published(files, expected_title)
        art = {name: (folder / "sce_sys" / name).read_bytes() for name in ("pic0.dds", "pic1.dds")
               if (folder / "sce_sys" / name).is_file()}
        home_art = install_home_art(args.run, art)
    except BaseException:
        title_restore(argparse.Namespace(run=args.run, title_only=True))
        raise
    (backup / "installed.json").write_text(json.dumps({
        "format": 1, "raw_ftp_reply": reply, "files": deployed,
        "qualification_digest": qualification["qualification_key"],
        "prefix": prefix, "home": home,
        "home_art": home_art,
        "revert": f"deploy-wine.py title-restore {args.run}",
    }, indent=2, sort_keys=True) + "\n")
    print(f"b1-title-install: {len(deployed)} title files, user data {prefix}, "
          f"{len(home_art)} home-screen copies replaced; the home screen shows new art after a restart; "
          f"revert with: deploy-wine.py title-restore {args.run}")


def b1_title_run(args):
    """Back up all title writes, launch once, and restore without payload Wine."""
    if args.timeout <= 0 or args.exit_grace < 0:
        raise SystemExit("B1 needs a positive bounded timeout and nonnegative exit grace")
    folder = Path(args.folder).resolve()
    qualification, selected, guards, files, expected_title = b1_preflight(folder)
    backup = RUNS / f"{args.run}-backup"
    launched = False
    launch_result = None
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f"B1 transaction interrupted by signal {signum}")
    previous_handlers = {number: signal.getsignal(number) for number in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)}
    for number in previous_handlers:
        signal.signal(number, interrupted)
    try:
        title_backup(argparse.Namespace(run=args.run, guard_paths=guards, title_only=True))
        before = json.loads((backup / "before.json").read_text())
        try:
            upload_title(argparse.Namespace(folder=str(folder), files=files))
            deployed, reply = b1_verify_published(files, expected_title)
            ftp, _ = raw_connect()
            try:
                boot = REMOTE + "/boot.txt"
                boot_metadata = before["paths"][boot]
                if boot_metadata["present"]:
                    if ftp_identity(ftp, boot) != {key: boot_metadata[key] for key in ("bytes", "sha256")}:
                        raise RuntimeError("boot.txt changed before B1 launch")
                else:
                    try:
                        ftp_bytes(ftp, boot)
                    except error_perm:
                        pass
                    else:
                        raise RuntimeError("boot.txt appeared before B1 launch")
            finally:
                ftp.quit()
            (backup / "deployed-all.json").write_text(json.dumps({
                "format": 1, "raw_ftp_reply": reply, "files": deployed,
                "qualification_digest": qualification["qualification_key"],
                "guards": "MATCHED_BEFORE_LAUNCH", "prefix": "EXISTING_NO_WINEBOOT",
                "payload_install": "UNTOUCHED", "title_root": selected["remote_root"],
            }, indent=2, sort_keys=True) + "\n")
            command = [
                sys.executable, str(HERE.parent.parent.parent / "PS5_Vulkan/tools/run-title.py"),
                TITLE_ID, "--until", "b1 parent result=WINE_EXIT wine_exit=" +
                (f"|{args.until}" if args.until else ""),
                "--timeout", str(args.timeout), "--exit-grace", str(args.exit_grace),
                "--output", str(RUNS / f"{args.run}.klog"),
            ]
            if args.echo:
                command += ["--echo", args.echo]
            launched = True
            result = subprocess.run(command)
            launch_result = {"command": command, "returncode": result.returncode, "title_launches": 1,
                             "qualification_digest": qualification["qualification_key"]}
        finally:
            # Ordinary termination signals must not interrupt byte restoration.
            for number in previous_handlers:
                signal.signal(number, signal.SIG_IGN)
            try:
                title_restore(argparse.Namespace(run=args.run, title_only=True))
            finally:
                if launch_result is None:
                    launch_result = {"title_launches": int(launched), "returncode": None,
                                     "failure": "transaction ended before run-title returned"}
                (backup / "launch.json").write_text(json.dumps(launch_result, indent=2) + "\n")
    finally:
        for number, handler in previous_handlers.items():
            signal.signal(number, handler)
    if launch_result["returncode"]:
        raise SystemExit(launch_result["returncode"])


WINEKILL = HERE / "probes/winekill/winekill.elf"


def send_payload(elf, timeout, log=None, until=None, echo=True):
    """Send an ELF to the console's loader; print (unless echo is False) and log
    what it streams back until the connection closes, the timeout passes, or the
    regex until matches."""
    import re
    seen = ""
    s = socket.create_connection((HOST, LOADER_PORT), timeout=10)
    s.sendall(elf)
    s.shutdown(socket.SHUT_WR)
    s.settimeout(1.0)
    end = time.time() + timeout
    while time.time() < end:
        try:
            data = s.recv(65536)
        except socket.timeout:
            continue
        if not data:
            break
        text = data.decode(errors="replace")
        if echo:
            sys.stdout.write(text)
            sys.stdout.flush()
        if log:
            log.write(data)
            log.flush()
        if until:
            seen = (seen + text)[-65536:]
            if re.search(until, seen):
                break
    s.close()


def capture_klog(path, stop):
    """Append the console's klog to path until stop is set; a lost klog only ends the capture."""
    try:
        s = socket.create_connection((HOST, KLOG_PORT), timeout=10)
    except OSError as e:
        print(f"klog not captured: {e}", file=sys.stderr)
        return
    s.settimeout(1.0)
    try:
        with path.open("wb") as out:
            while not stop.is_set():
                try:
                    data = s.recv(65536)
                except socket.timeout:
                    continue
                if not data:
                    break
                out.write(data)
                out.flush()
    except OSError as e:
        print(f"klog capture ended: {e}", file=sys.stderr)
    finally:
        s.close()


def kill(args):
    """End the Wine session on the console (ps5/probes/winekill)."""
    if not WINEKILL.exists():
        raise SystemExit(f"missing {WINEKILL}: build ps5/probes/winekill")
    send_payload(WINEKILL.read_bytes(), 20)


REGISTRY = ("system.reg", "user.reg", "userdef.reg")


def check_registry(ftp, dest, prefix):
    """Back up the selected prefix's registries before a session.

    Only the default prefix may be repaired from its historical backups.
    Alternate prefixes, including fresh disposable ones, must never inherit
    registry files from the real prefix."""
    from io import BytesIO
    header = b"WINE REGISTRY Version 2"
    for name in REGISTRY:
        data = BytesIO()
        try:
            ftp.retrbinary(f"RETR {prefix}/{name}", data.write)
        except Exception:
            pass
        if data.getvalue().startswith(header):
            dest.mkdir(parents=True, exist_ok=True)
            (dest / name).write_bytes(data.getvalue())
            continue
        if prefix != f"{REMOTE}/prefix":
            print(f"registry: {prefix}/{name} missing or bad; leaving it for wineboot", flush=True)
            continue
        good = [p for p in sorted(RUNS.glob(f"*.reg/{name}")) if p.read_bytes().startswith(header)]
        if not good:
            print(f"registry: {name} is bad and no good copy exists", flush=True)
            continue
        path = f"{prefix}/{name}"
        try:
            ftp.rename(path, f"{path}.bad-{time.strftime('%Y%m%d-%H%M%S')}")
        except Exception:
            pass  # missing
        ftp.storbinary(f"STOR {path}.restore", BytesIO(good[-1].read_bytes()))
        ftp.rename(f"{path}.restore", path)
        print(f"registry: {name} was bad; restored {good[-1]}", flush=True)


def run(args):
    loader = Path(args.loader)
    if not loader.is_file():
        raise SystemExit(f"missing regular payload loader {loader}: rebuild Wine with "
                         "ps5/build-wine.sh and pass --loader WINE_BUILD/loader/wine, "
                         "or stage it with ps5/stage-wine.sh for the default")
    loader_data = loader.read_bytes()
    if not args.keep_session:
        kill(args)  # a server left from an older build would serve this one
    environment = dict(e.split("=", 1) for e in ENV + args.env)
    if args.prefix is not None:
        environment["WINEPREFIX"] = args.prefix
    lines = [f"arg {LOADER}"] + [f"arg {a}" for a in [args.program] + args.args]
    lines += [f"env {key}={value}" for key, value in environment.items()] + [f"cwd {args.cwd}"]
    RUNS.mkdir(parents=True, exist_ok=True)
    base = RUNS / time.strftime("%Y%m%d-%H%M%S")
    ftp = connect()
    registry_backup = base.with_suffix(".reg" if environment["WINEPREFIX"] == f"{REMOTE}/prefix"
                                       else ".prefix-registry")
    check_registry(ftp, registry_backup, environment["WINEPREFIX"])
    mkdirs(ftp, environment["HOME"], set())
    from io import BytesIO
    ftp.storbinary(f"STOR {REMOTE}/boot.txt", BytesIO(("\n".join(lines) + "\n").encode()))
    ftp.quit()

    stop = threading.Event()
    klog = threading.Thread(target=capture_klog, args=(base.with_suffix(".klog"), stop), daemon=True)
    klog.start()
    try:
        with base.with_suffix(".out").open("wb") as log:
            send_payload(loader_data, args.timeout, log, args.until)
        time.sleep(args.klog_tail)  # crash reports reach klog after the loader's socket closes
    finally:
        stop.set()
        klog.join(5)
    print(f"run log: {base}.out, {base}.klog")


# DXVK (ps5/build-dxvk.sh) as Proton installs it: its DLLs in the prefix's
# system32 (x86_64) and syswow64 (i386), and native overrides in the prefix's
# registry, so dev runs (boot.txt written here) and the title's launches
# (PS5_WineLauncher writes its own boot.txt) both use them. Wine's prefix
# updates replace only its own placeholder DLLs, not these.
DXVK_DLLS = ["d3d8", "d3d9", "d3d10core", "d3d11", "dxgi"]
DXVK_BUILDS = {"64": "system32", "32": "syswow64"}


def dxvk(args):
    from io import BytesIO
    files = []
    for bits, folder in DXVK_BUILDS.items():
        for name in DXVK_DLLS:
            local = Path(f"{args.builds}-{bits}") / "dlls" / f"{name}.dll"
            if not local.exists():
                raise SystemExit(f"missing {local}: run ps5/build-dxvk.sh")
            files.append((local, f"{REMOTE}/prefix/drive_c/windows/{folder}/{name}.dll"))
    ftp = connect()
    for local, remote in files:
        ftp.storbinary(f"STOR {remote}.new", BytesIO(local.read_bytes()))
        try:
            ftp.delete(remote)
        except error_reply as reply:
            if not str(reply).startswith("226"):  # the console's FTP server answers DELE with 226
                raise
        except error_perm:
            pass  # not there yet
        ftp.rename(f"{remote}.new", remote)
        print(f"dxvk: {local} -> {remote}", flush=True)
    reg = "REGEDIT4\n\n[HKEY_CURRENT_USER\\Software\\Wine\\DllOverrides]\n"
    reg += "".join(f'"{name}"="native"\n' for name in DXVK_DLLS)
    reg_path = f"{REMOTE}/share/wine/dxvk-overrides.reg"
    ftp.storbinary(f"STOR {reg_path}", BytesIO(reg.encode()))
    ftp.quit()
    run(argparse.Namespace(program="C:\\windows\\regedit.exe", args=["/S", "Z:" + reg_path.replace("/", "\\")],
                           env=[], prefix=None, timeout=args.timeout, until=None, klog_tail=5,
                           keep_session=False, cwd=REMOTE, loader=STAGE / "lib/wine/x86_64-unix/wine"))


def prefix_permissions(args):
    """Open a prefix tree to every console process, as Wine now creates it.

    Payload Wine (uid 0) and a title (uid 1) share prefixes, and each gets only
    the "other" bits of the other's files. Wine used to map installer DACLs to
    0770 (payload) or 0000 (title) modes. This sets directories to 0777 and
    files to 0666, or 0777 if any execute bit is set, through the FTP server
    (which may chmod anything), and records every change. A directory is
    changed before it is listed, so a 0000 directory is walked too."""
    root = args.folder.rstrip("/")
    if not root.startswith(f"/data/homebrew/{TITLE_ID}/") or "/../" in root + "/":
        raise SystemExit(f"prefix-permissions stays inside /data/homebrew/{TITLE_ID}: {root}")
    ftp = connect()
    changed, unlisted, seen, stack = [], [], 0, [root]
    while stack:
        directory = stack.pop()
        lines = []
        try:
            ftp.retrlines(f"LIST {directory}", lines.append)
        except error_perm as error:
            unlisted.append({"path": directory, "reply": str(error)})
            continue
        for line in lines:
            parts = line.split(None, 8)
            if len(parts) < 9 or parts[8] in (".", "..") or parts[0][0] not in "d-":
                continue
            path, flags = f"{directory}/{parts[8]}", parts[0]
            mode = sum(1 << (8 - i) for i, c in enumerate(flags[1:10]) if c != "-")
            seen += 1
            if flags[0] == "d":
                stack.append(path)
                wanted = 0o777
            else:
                wanted = 0o777 if mode & 0o111 else 0o666
            if mode != wanted:
                reply = ftp.sendcmd(f"SITE CHMOD {wanted:o} {path}")
                changed.append({"path": path, "old": f"{mode:o}", "new": f"{wanted:o}", "reply": reply[:3]})
    ftp.quit()
    record = {"host": HOST, "root": root, "seen": seen, "changed": changed, "unlisted": unlisted}
    Path(args.log).write_text(json.dumps(record, indent=1) + "\n")
    failed = [item for item in changed if item["reply"] != "200"]
    print(f"prefix-permissions: {root} seen={seen} changed={len(changed)} failed={len(failed)} "
          f"unlisted={len(unlisted)} log={args.log}")
    if failed or unlisted:
        raise SystemExit(1)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    u = sub.add_parser("upload")
    u.add_argument("--verify", action="store_true",
                   help="hash on the console and replace differing or missing files (no FTP downloads)")
    sub.add_parser("kill")
    r = sub.add_parser("run")
    r.add_argument("program")
    r.add_argument("args", nargs="*")
    r.add_argument("--env", action="append", default=[], help="extra NAME=VALUE")
    r.add_argument("--prefix", help="console WINEPREFIX (overrides --env WINEPREFIX; default: current prefix)")
    r.add_argument("--loader", default=str(STAGE / "lib/wine/x86_64-unix/wine"),
                   help="local payload ELF (default: staged Wine loader; console install unchanged)")
    r.add_argument("--timeout", type=float, default=120)
    r.add_argument("--until", help="end the run when Wine's output matches this regex")
    r.add_argument("--klog-tail", type=float, default=10, help="seconds of klog kept after the run ends")
    r.add_argument("--keep-session", action="store_true", help="do not end the running session first")
    r.add_argument("--cwd", default=REMOTE, help="console directory the program starts in")
    t = sub.add_parser("upload-title", help="upload ps5/title/build.sh's OUT_DIR beside wine/")
    t.add_argument("folder")
    x = sub.add_parser("dxvk", help="install ps5/build-dxvk.sh's DLLs in the prefix with native overrides")
    x.add_argument("--timeout", type=float, default=120)
    x.add_argument("--builds", default=str(HERE / "build/dxvk"),
                   help="the build trees' common prefix (ps5/build-dxvk.sh's DXVK_BUILD): PREFIX-32 and PREFIX-64")
    c = sub.add_parser("title", help=f"start, close or query {TITLE_ID} through ps5vkctl")
    c.add_argument("action", choices=["launch", "kill", "restart"])
    b = sub.add_parser("title-backup", help="make a fresh raw guarded title backup")
    b.add_argument("run", help="run name; creates ps5/build/runs/RUN-backup")
    b.add_argument("--paths-from", required=True, help="prior before.json defining guarded paths")
    z = sub.add_parser("title-restore", help="restore a title-backup byte-exactly")
    z.add_argument("run", help="run name used by title-backup")
    br = sub.add_parser("runtime-title-run",
                        help="bounded PsmKit-Wine backup, prefix, deploy, launch and restore")
    br.add_argument("run", help="unique run name")
    br.add_argument("--paths-from", required=True, help="prior before.json defining guarded paths")
    br.add_argument("--folder", default=str(HERE / "build/title-psmkit-wine-runtime"))
    br.add_argument("--until", required=True, help="run-title completion regex")
    br.add_argument("--echo", default="")
    br.add_argument("--timeout", type=float, default=240)
    br.add_argument("--exit-grace", type=float, default=5)
    br.add_argument("--elf")
    br.add_argument("--wine-build", default=str(HERE / "build/wine-ps5-build"),
                    help="build tree supplying the prefix payload loader and matching system32/syswow64 WoW64 DLLs")
    tr = sub.add_parser("title-root-upload", help="byte-verified upload of the qualified separate B1 Wine tree")
    tr.add_argument("--folder", default=str(HERE / "build/title-psmkit-wine-b1"))
    tr.add_argument("--replace", action="store_true", help="deliberately replace an existing wine-title root only")
    b1 = sub.add_parser("b1-title-run", help="one bounded existing-prefix B1 title launch with guarded restoration")
    b1.add_argument("run", help="unique run name")
    b1.add_argument("--folder", default=str(HERE / "build/title-psmkit-wine-b1"))
    b1.add_argument("--timeout", type=float, default=240)
    b1.add_argument("--exit-grace", type=float, default=5)
    b1.add_argument("--echo", default="b1 |p1c |err:|fixme:|wine: title")
    b1.add_argument("--until", help="also end the run at this klog regex (the parent's result always ends it)")
    bi = sub.add_parser("b1-title-install",
                        help="make the qualified B1 title the home-screen program (Battle.net in the title)")
    bi.add_argument("run", help="unique install name; title-restore RUN reverts it")
    bi.add_argument("--folder", default=str(HERE / "build/title-psmkit-wine-b1"))
    pp = sub.add_parser("prefix-permissions",
                        help="open a prefix tree to payload and title alike (dirs 0777, files 0666/0777)")
    pp.add_argument("--folder", default=REMOTE + "/prefix")
    pp.add_argument("--log", required=True, help="local JSON record of every change")
    a = p.parse_args()
    {"upload": upload, "run": run, "kill": kill, "upload-title": upload_title, "title": title,
     "title-backup": title_backup, "title-restore": title_restore,
     "runtime-title-run": runtime_title_run, "title-root-upload": title_root_upload,
     "b1-title-run": b1_title_run, "b1-title-install": b1_title_install, "dxvk": dxvk,
     "prefix-permissions": prefix_permissions}[a.cmd](a)


if __name__ == "__main__":
    main()
