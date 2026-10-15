#!/usr/bin/env bash
# Build in the pinned Ubuntu container; never uses the remote RADV build tree.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

if [[ ${1:-} != --container ]]; then
    [[ $# == 2 ]] || { echo "usage: $0 SOURCE_ROOT NN.NNN.NNN" >&2; exit 2; }
    sources=$(cd -- "$1" && pwd)
    tag=$2
    image=forever-launcher-linux:ubuntu-24.04
    start=$SECONDS
    docker build --platform linux/amd64 --tag "$image" "$here"
    printf 'BUILD_STAGE image seconds=%s\n' "$((SECONDS - start))"
    exec docker run --rm --platform linux/amd64 \
        --mount "type=bind,source=$sources,target=/sources" \
        --env "JOBS=${JOBS:-8}" \
        --env "PS5_MESA_REVISION=${PS5_MESA_REVISION:-}" "$image" \
        bash /sources/ps5-proton/ps5-wow/ps5/release/build-linux.sh --container "$tag"
fi
[[ $# == 2 && $(uname -s) == Linux && $(uname -m) == x86_64 ]] || {
    echo 'the container entry requires Linux x86_64 and a content-version tag' >&2; exit 2;
}
tag=$2
root=/sources
wow=$root/ps5-proton/ps5-wow
cd "$wow"
export JOBS=${JOBS:-8}
export BUILD_JOBS=$JOBS
export CMAKE_BUILD_PARALLEL_LEVEL=$JOBS
export PS5_PE_LDFLAGS=-Wl,--no-insert-timestamp
export PS5_VULKAN=$root/PS5_Vulkan
export PS5_MESA_FORK=$root/PS5_Mesa
export PACBREW_REPO=$root/pacbrew-repo
export WINE_SRC=$root/ps5-proton/ps5-wine
export PS5_PAYLOAD_SDK=$PS5_VULKAN/.deps/native/ps5-payload-sdk
export PATH=$LLVM_MINGW/bin:$PATH
export PS5_RADV_REVISION=${PS5_MESA_REVISION:-$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["radv_revision"])' "$here/deps.json")}
# The writebroker's existing Makefile uses this historic path.
ln -s "$PS5_PAYLOAD_SDK" "$HOME/ps5-payload-sdk-platform"
# Never import optional, untracked desktop artwork from a workstation.
export DESKTOP_DDS=/nonexistent/forever-desktop.dds
mkdir -p ps5/build/release/logs
logs=$wow/ps5/build/release/logs
export FOREVER_SOURCE_ROOT=$root
export FOREVER_SOURCE_MANIFEST=$here/deps.json
export PATH=$logs/tools:$PATH
printf 'stage\tseconds\tstatus\n' > "$logs/timings.tsv"
started=$SECONDS
stage() {
    local name=$1 begin=$SECONDS status=0
    local -a statuses
    shift
    printf '\n==> BUILD_STAGE_BEGIN %s %s\n' "$name" "$(date -u +%FT%TZ)"
    # Do not put a build function in an `||` condition: Bash would disable
    # errexit inside it and a failed prerequisite could be silently skipped.
    set +e
    (set -e; "$@") 2>&1 | tee "$logs/$name.log"
    statuses=("${PIPESTATUS[@]}")
    set -e
    status=${statuses[0]}
    (( status != 0 )) || status=${statuses[1]}
    # Preserve failure status even if printing the receipt itself succeeds.
    printf '%s\t%s\t%s\n' "$name" "$((SECONDS - begin))" "$status" >> "$logs/timings.tsv"
    printf 'BUILD_STAGE %s seconds=%s status=%s\n' "$name" "$((SECONDS - begin))" "$status"
    return "$status"
}
validate_sources() {
    python3 - "$root" "$here/deps.json" "$tag" <<'PY'
import hashlib, json, os, pathlib, re, subprocess, sys
root, manifest = pathlib.Path(sys.argv[1]), json.load(open(sys.argv[2]))
if not re.fullmatch(r'\d\d\.\d\d\d\.\d\d\d', sys.argv[3]):
    raise SystemExit('tag must be NN.NNN.NNN')
# Bind-mounted checkouts retain the host UID; trust only the manifest paths
# in this disposable container, not arbitrary repositories.
for source in [manifest['project'], *manifest['sources']]:
    subprocess.run(['git', 'config', '--global', '--add', 'safe.directory',
                    str(root / source['path'])], check=True)
def git(path, *args):
    return subprocess.check_output(['git', '-C', str(path), *args], text=True).strip()
for source in manifest['sources']:
    path = root / source['path']
    actual = git(path, 'rev-parse', 'HEAD')
    records = subprocess.check_output(['git', '-C', str(path), 'ls-files', '--stage', '-z'])
    fingerprint = hashlib.sha256(b'\0'.join(
        row for row in records.split(b'\0') if row and not row.startswith(b'160000 '))).hexdigest()
    if fingerprint != source['content_sha256']:
        raise SystemExit(f"{source['name']}: source content fingerprint mismatch: {fingerprint}")
    if git(path, 'status', '--porcelain', '--untracked-files=no', '--ignore-submodules'):
        raise SystemExit(f"{source['name']}: modified tracked sources")
    print(f"BUILD_SOURCE {source['name']} {actual}")
by_name = {source['name']: source for source in manifest['sources']}
for name, revision in (('PS5_PayloadSDK', manifest['sdk_revision']),
                       ('PS5_Mesa', os.environ['PS5_RADV_REVISION'])):
    git(root / by_name[name]['path'], 'cat-file', '-e', revision + '^{commit}')
lock = json.load(open(root / 'ps5-proton/ps5-wow/ps5/wine-ps5.lock'))
if lock['commit'] != by_name['ps5-wine']['commit']:
    raise SystemExit('Wine lock does not match deps.json')
param = json.load(open(root / 'ps5-proton/ps5-wow/ps5/title/sce_sys/param.json'))
if param['contentVersion'] != sys.argv[3]:
    raise SystemExit('tag does not match source contentVersion')
PY
    # Replaying explicit version provenance avoids dependence on unpublished
    # Git ancestry. Only these four upstream version-stamp commands change;
    # all Git source/export/validation operations use the real executable.
    mkdir -p "$logs/tools"
    cat > "$logs/tools/git" <<'PY'
#!/opt/venv/bin/python3
import json
import os
from pathlib import Path
import subprocess
import sys

real_git = '/usr/bin/git'
args = sys.argv[1:]
known = {
    ('describe', 'HEAD'): 'ps5-wine',
    ('describe', '--always', '--tags', '--dirty=+'): 'vkd3d-proton',
    ('describe', '--always', '--exclude=*', '--abbrev=15', '--dirty=0'): 'vkd3d-proton',
    ('describe', '--dirty=+'): 'dxvk',
}
name = known.get(tuple(args))
if name:
    manifest = json.loads(Path(os.environ['FOREVER_SOURCE_MANIFEST']).read_text())
    source = next(item for item in manifest['sources'] if item['name'] == name)
    expected = (Path(os.environ['FOREVER_SOURCE_ROOT']) / source['path']).resolve()
    if os.environ.get('GIT_DIR'):
        git_dir = Path(os.environ['GIT_DIR']).resolve()
        actual = git_dir.parent if git_dir.name == '.git' else None
    else:
        result = subprocess.run([real_git, 'rev-parse', '--show-toplevel'],
                                capture_output=True, text=True)
        actual = Path(result.stdout.strip()).resolve() if result.returncode == 0 else None
    if actual == expected:
        value = (source['original_commit'][:15] if '--abbrev=15' in args else source['describe'])
        print(value)
        raise SystemExit(0)
os.execv(real_git, [real_git, *args])
PY
    chmod +x "$logs/tools/git"
}
build_sdk() {
    local revision tree=$root/.sdk-source
    revision=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["sdk_revision"])' "$here/deps.json")
    mkdir -p "$tree"
    git -C "$root/PS5_PayloadSDK" archive "$revision" | tar -x -C "$tree"
    bash "$tree/platform/tools/setup-sdk.sh" \
        "$PS5_PAYLOAD_SDK" "$revision" "$root/.downloads"
}
build_host_tools() {
    bash "$PS5_VULKAN/tools/setup-native-dependencies.sh" --skip-sdk
    local native=$PS5_VULKAN/tooling/native
    local zlib=$PS5_VULKAN/.deps/native/zlib/root
    mkdir -p "$PS5_VULKAN/build/host/clang-resource/lib/linux"
    clang++-19 -std=c++20 -O2 -Wall -Wextra -Werror -I "$zlib/usr/include" \
        "$native/native_app_builder.cpp" "$native/self_container.cpp" \
        "$native/elf_object.cpp" "$native/sce_module_writer.cpp" \
        "$zlib/usr/lib/libz.a" -o "$PS5_VULKAN/build/host/ps5-native-tool"
    cp "$(clang-19 -print-file-name=libclang_rt.builtins-x86_64.a)" \
        "$PS5_VULKAN/build/host/clang-resource/lib/linux/"
    bash "$PS5_VULKAN/tools/rebuild-libc.sh"
}
restore_wine_headers() {
    git -C "$WINE_SRC" restore include/wine/server_protocol.h \
        server/request_handlers.h server/request_trace.h
}
build_wine() {
    (cd "$WINE_SRC" && perl tools/make_requests)
    bash ps5/build-wine.sh configure
    bash ps5/build-wine.sh all
}
cleanup() {
    local status=$?
    restore_wine_headers || status=$?
    rm -rf "$logs/tools"
    exit "$status"
}
trap cleanup EXIT
stage inputs validate_sources
stage sdk build_sdk
stage host-tools build_host_tools
stage radv python3 ps5/radv/build-radv.py
stage freetype bash ps5/build-freetype.sh
stage gnutls bash ps5/build-gnutls.sh
stage wine build_wine
stage vkd3d-proton bash ps5/build-vkd3d-proton.sh
stage dxvk bash ps5/build-dxvk.sh
stage runtime-b1-dual make -s -C ps5/probes/psmkit-wine runtime-b1-dual
stage package python3 ps5/release/package.py --tag "$tag"
printf 'BUILD_COMPLETE seconds=%s zip=%s\n' "$((SECONDS - started))" "$wow/ps5/build/release/PPSA99030.zip"
