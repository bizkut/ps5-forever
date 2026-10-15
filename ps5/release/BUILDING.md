# Building Forever Launcher on Linux

Use an x86-64 Linux host with Docker and a fresh, writable source directory.
No console, shared SDK, remote RADV build, prefix, installed game, or Mac build
output is used. The only network inputs during the build are pinned tool and
source downloads; source repositories are supplied by the caller.

## Source layout and command

`deps.json` records each external checkout's GitHub URL, commit, and path
relative to `SOURCE_ROOT`, including the required nested graphics submodules.
After checking out this repository at `SOURCE_ROOT/ps5-proton/ps5-wow`, run
`python3 ps5/release/fetch-sources.py SOURCE_ROOT` from that checkout. It fetches
the exact public commits, including nested dependencies, without recursively
fetching Proton's unused desktop submodules:

```text
SOURCE_ROOT/
  PS5_Vulkan/
  PS5_Mesa/
  PS5_PayloadSDK/
  pacbrew-repo/
  ps5-proton/
    ps5-wine/
    ps5-wow/                 # this release's source
      vkd3d-proton/          # nested checkouts listed in deps.json
      dxvk/
```

`sdk_revision` and `radv_revision` select the source objects exported for the
actual Mac SDK and patched title RADV. `PS5_MESA_REVISION` can override the latter.
These objects must exist locally; no build stage fetches private ancestry.
Public single-commit snapshots can set these selectors to their new commit IDs.
The checkout `commit` records provenance; `content_sha256` independently pins
tracked content and permits history-free replacement commits. Its input is the
NUL-joined `git ls-files --stage -z` records, without the final NUL or mode-160000
gitlink records (each nested source has its own pin). Modified tracked sources
are refused, and the Wine lock must agree with its manifest commit.

From `ps5-wow`:

```sh
JOBS=8 bash ps5/release/build-linux.sh /absolute/SOURCE_ROOT 01.000.001
```

The tag must equal the source `sce_sys/param.json` content version. This builds
the Docker image, then builds the SDK, native title tooling/clean-room libc,
patched RADV, FreeType, GnuTLS, Wine, vkd3d-proton, DXVK, both B1 roles, and the
release archive. Generated Wine server headers are restored on exit. Use an
empty source/output directory for a cold build; rerunning can reuse outputs.

Outputs under `ps5-wow/ps5/build/`:

- `title-psmkit-wine-b1/`: independently qualified maintenance and game roles.
- `release/PPSA99030.zip`: catalog archive, with size and SHA-256 in the output.
- `release/logs/`: complete per-stage logs and `timings.tsv` (seconds/status).

Require two `RUNTIME_TITLE_QUALIFIED` records with `raw_syscalls=0`, followed by
`RUNTIME_TITLE_DUAL_QUALIFIED`. The qualifier's deliberate unknown-import and
win32u-identity rejection records are expected negative tests. Qualification
here is a host build/inspection gate, **not console execution**.

## Pinned inputs and limits

`Dockerfile` pins the Ubuntu 24.04 image by digest and Ubuntu apt packages
(including LLVM/Clang 18 and 19, SPIR-V translator/libclc 19, glslang, Python,
bison, CMake and autotools) by the immutable 2026-10-01 archive snapshot.
LLVM 19 builds native/title code and RADV's LLVM-dependent host tools.
llvm-mingw 20260922/UCRT (LLVM 23.1.2) is SHA-256 checked; Python build packages
have explicit versions. Ubuntu's fixed `ca-certificates=20240203` package
is unpacked only to bootstrap HTTPS trust; no packages or dependencies are
installed from the live Ubuntu archive before switching to the snapshot.
The release container sets `SOURCE_DATE_EPOCH=0` and passes
`PS5_PE_LDFLAGS=-Wl,--no-insert-timestamp` to Wine and both graphics builders.
This removes wall-clock PE/export/debug-directory timestamps; ordinary Mac
builds keep their existing flags unless that variable is explicitly set.

`deps.json` inventories SHA-256-pinned downloads. The established recipes also
verify those digests: FreeType; pacbrew Nettle/mini-gmp, libtasn1 and GnuTLS;
Vulkan native zlib; Mesa's zlib and Meson patch; Mozilla roots; Droid/Noto fonts
and notices. Repository-contained assets, headers and stb_image come from the
release source. Optional workstation desktop artwork is explicitly disabled.

Wine and the graphics DLLs normally stamp `git describe`, which depends on
unpublished tags/history and can silently fall back in shallow snapshots.
After validating source fingerprints, the build generates a narrow Git wrapper
that replays only the four known version-stamp commands from explicit
`describe`/`original_commit` provenance. All other Git commands use `/usr/bin/git`.
This is recorded version provenance, not a claim that private ancestry exists
in a public snapshot. Generated headers in the build trees show these values.

The SDK's established `platform/tools/setup-sdk.sh` recipe rebuilds the fork's
headers, platform, libc, CRT and additional import stubs over the hash-pinned
upstream **v0.42 binary SDK**. In particular its libc++, libc++abi, libunwind,
and existing stubs are not rebuilt from source by this release recipe. The
Docker image likewise consumes published compiler/package binaries. This is
a pinned-source/pinned-binary bootstrap, not a claim of a fully bootstrapped
source-only toolchain. No proprietary or workstation-only binary is supplied.
The published dependency snapshots have fresh histories. Their content
fingerprints and original build-version provenance are recorded separately;
no private GitHub repository or development history is needed.
Keep at least **30 GiB free** for sources, build output and Docker. The measured
cold workspace used 13 GiB before Docker overhead. CI targets a self-hosted
Linux x86-64 runner and defaults to `JOBS=8` (override with the `FOREVER_JOBS`
repository variable). RADV Ninja and Wine's `install-lib` honor the same cap.
The container runs with the invoking user's UID/GID, writes only its temporary
home and the supplied source/build tree, and never removes host toolchains.

For the macOS compatibility checks (not a full title rebuild):

```sh
PATH=/tmp/py314:$PATH make -s -C ps5/probes/psmkit-wine test
bash -n ps5/release/build-linux.sh # repeat for edited build/staging scripts
```

The portable scripts retain the existing Mac SDK, Homebrew and llvm-mingw
defaults; Linux selects its tool paths through the container environment.
