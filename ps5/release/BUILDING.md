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
JOBS=8 bash ps5/release/build-linux.sh /absolute/SOURCE_ROOT 01.000.011
```

Source validation and the full host suite run before the SDK and runtime builds.

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

## Unreleased crash-dialog exit policy

Both WoW GPU profiles list `BlizzardError.exe` in optional `crash_reporters`,
not `refused_programs`. When the game requests that reporter, title ntdll logs
the basename and terminates the requesting process with `STATUS_UNSUCCESSFUL`.
The GPU parent's existing game-exit path then closes the app instead of leaving
WoW's invisible error dialog in front. WoW writes its crash report before the
reporter request; this policy does not fix the underlying crash.

The list uses the same bounded ASCII basename validation and case-insensitive
path matching as `refused_programs`, but is accepted only in GPU profiles.
Ordinary refused programs still return access denied without ending their
requester. Maintenance/Battle.net has no crash-reporter policy, and each role
overwrites `WINEPS5_CRASH_REPORTERS` even when empty so LoadExec cannot carry
the game policy back into maintenance.

## 01.000.011 source changes

- PS5 in-process title servers synthesize Battle.net Agent's expected ACL grants
  for world-rwx files and directories when security xattrs cannot persist;
  payload and non-PS5 descriptors remain unchanged.
- Native cursor publication uses the opt-in `PS5_DESKTOP_CURSOR_CAPABLE` HELLO
  capability and type-12 ARGB cursor messages with a hotspot. Maintenance windows
  publish through wineps5.drv; the headless GPU parent uses win32u's title-GPU
  null-driver behaviour plus cursor callbacks, enabled only by `wineSetCursor`.
  CPU-only headless children retain the plain null driver.
- Battle.net.exe receives `--enable-spatial-navigation`, using the existing
  D-pad arrow, Triangle Enter and Options Escape mappings on most pages.
- Final-channel paths now use the actual `_forever_/Wow.exe` spelling for preset
  installation and direct start; the per-channel user-settings preservation
  rules remain unchanged.
- The write-broker pool grows from 4 to 16 lazily opened connections per Wine
  process. Routing drops the two fixed alignment bits from Windows thread IDs
  before selecting a slot; previously every Windows thread selected the same slot.
  Requests and replies remain synchronous and locked per connection, with no new
  client worker. Each live connection uses one broker worker and a 1 MiB buffer
  (at most 16 workers / 16 MiB for one fully used Wine process's pool).
- Battle.net defaults to Classic beta (`wow_classic_beta`,
  `_classic_beta_/WowB.exe`) again; final Forever (`wowf`, `_forever_/Wow.exe`)
  remains supported when selected. Direct-game startup prefers a complete beta
  install, then a complete final install.
- The recommended `Config.wtf` is installed independently for final and beta,
  only when that channel's executable exists and its own settings file is absent.
- Wine's WoW loader recognition tries the derived loader name, then a lowercase
  basename for final's `wow_loader.dll`, without changing the directory path or
  losing beta's `WowB_loader.dll` support.
- SMBIOS uses generic `"The Wine project"` / `"Wine"` identity, serial `"0"`
  and an all-zero UUID instead of Sony/PlayStation 5 branding.
- The human-readable GPU name is `"AMD Radeon Graphics (RADV)"`; the synthetic
  EDID identifies a `WIN` / `"Wine HDR TV"` display. Real CPUID results and PCI
  identity `1002:13fb` are intentionally unchanged, preserving hardware capabilities.
- The downstream `SfcIsFileProtected` compatibility stub stays leaf-only for
  title-child pre-context calls: it returns `FALSE` without diagnostics or
  last-error helper calls, avoiding the updated WoW startup fault.
- Release logs omit raw keyboard records, key usages and modifiers; aggregate
  input-health counters remain.
- Wine's DirectWrite core-family aliases route Chromium's last-resort font
  requests to the bundled Tahoma / Droid Sans Fallback families.
- The Battle.net installer downloader tries the direct versioned
  `win/1.0.66/Battle.net-Setup.exe` link once if the Live download fails.
- Startup creates missing `Program Files` and `Program Files (x86)` folders in
  the prefix; existing folders and user files are not replaced.
- Wine window surfaces publish completed drawing only at flush, rather than
  exposing drawing in progress to the compositor, fixing the installer's
  crawling flicker.
- Windows disk-space queries use the write broker's real storage counters for
  `/data/forever`. An older or unavailable broker retains the previous synthetic
  16 GiB free-space answer.
- The desktop background uses the drawn purple picture again.

In the PS5 synthetic write benchmark, 32 KiB writes from 4–8 threads improved from
approximately 16 to 30 MiB/s; single-thread writes are unchanged. Workspace
evidence: `release-proof/broker-bench-20261016T163238Z`. This measures parallel
file-write throughput, not real Battle.net download or update throughput; those
speedups have not yet been measured.

01.000.010 was not published: its build failed on a compiler warning in the new cursor code.

The `01.000.007` workflow failed before publication because of stale import-policy
entries. The `.008` import-policy correction removes only the obsolete
`wineps5.so` `strlen` and `strstr` rows from the B1 policy; all other policy entries
and qualification requirements remain unchanged.

Native acceptance of the exact final `.011` archive is **pending**. These source
changes do not record a native pass; the C:/W:-only scope and release gates below
remain in force.

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
This requests zero timestamps from the PE linker; Wine-generated export and
data-only headers instead retain deterministic filename/CPU hashes. Ordinary
Mac builds keep their existing flags unless that variable is explicitly set.
These settings alone do not prove byte-for-byte release reproducibility.

The Linux image replaces only llvm-mingw's i686/x86_64 `as` entrypoints with
assemble-only compiler invocations (`-x assembler -c`). The pinned upstream
entrypoints alias the generic Clang wrapper, which otherwise attempts an
executable link when Wine assembles individual import-library members. That
incorrectly pulls in MinGW startup code and reports missing `WinMain` and
`__wine_import_*_name` symbols. This adapter does not choose PE section permissions.

Wine's i386/x86_64 delay-import emitter explicitly declares its `.text$1`
thunks as COFF code with read/execute (`"xr"`) attributes. LLVM's assembler
otherwise treats this bare subsection as writable, non-executable data; LLD
then preserves it as a second non-executable `.text` in the consumer PE. This
caused the Linux `.002` explorer startup fault at the first delayed
`SHGetDesktopFolder` call. The fix is in `tools/winebuild/import.c`, not the
loader or a post-link permission patch; ELF, Mach-O and ARM emission is unchanged.
Rebuild the Wine host `tools/winebuild/winebuild`, regenerate both architectures'
`*.delay.a` libraries and relink their PE consumers before restaging the runtime.

The `.011` sources retain the two independent drive-reporting fixes:

- Wine's `server/directory.c` initializes the PS5 in-process server's `C:` and
  `W:` links in `\??` with fd-backed `\Device\HarddiskVolume1` and
  `\Device\HarddiskVolume2` devices. `C:` maps to the prefix's `drive_c`; `W:`
  maps to the prefix's parent (`/data/forever` by default). Ordinary file
  access alone did not populate the object namespace used for drive enumeration.
- The SDK's `platform/src/libc.c` sets `MNT_LOCAL` in its FreeBSD
  `statfs`/`fstatfs` responses. Without this flag Wine classified the console's
  local storage as a network volume, leading to Battle.net's network-folder
  rejection.

This is a bounded namespace/device and local-filesystem reporting fix, not a
registry drive-label override. It does **not** expose `Z:` or add UNC/network-share
support. Source pins and host qualification do not establish native acceptance.

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

## Linux development checks

After the pinned build image exists, run the host suite from a source tree
with dependencies fetched. It keeps compiler warnings as errors and does not
contact the console. These checks are not native PS5 execution.

```sh
SOURCE_ROOT=/absolute/SOURCE_ROOT
docker run --rm --pull=never --user "$(id -u):$(id -g)" \
  --env HOME=/tmp/forever-host-tests \
  --mount "type=bind,source=$SOURCE_ROOT,target=/sources" \
  --workdir /sources/ps5-proton/ps5-wow \
  forever-launcher-linux:ubuntu-24.04 \
  bash -c 'mkdir -p "$HOME"; make -s -C ps5/probes/psmkit-wine test MINGW_CC=/opt/llvm-mingw/bin/x86_64-w64-mingw32-clang MINGW32_CC=/opt/llvm-mingw/bin/i686-w64-mingw32-clang'
```

The `test-file-security` host gate compiles Wine's real `mode_to_sd`,
`sd_to_mode`, and SID definitions. Only PS5 in-process title servers synthesize
Administrators, Users, and Interactive full-access ACEs plus All Application
Packages read/execute for world-rwx files; payload and non-PS5 descriptors are
unchanged. Directories inherit these grants, and the DACL remains unprotected.
This reproduces Battle.net's installation repair when title security xattrs
cannot persist. The existing PS5 `sd_to_mode` policy keeps repair chmods at 0777.

The cursor host regressions are included in that suite (`test-cursor`,
`test-runtime-input`, `test-wine-title`, and `test-desktop-surface`). Wine sends
top-down straight-alpha ARGB images up to 64×64 with a desktop-space hotspot;
Battle.net's CPU compositor uses them at 1×, and the GPU parent uses two 64 KiB
VideoOut slots. Its nearest-neighbour integer scale follows RADV's fixed
3840×2160 scanout versus the 1920×1080 desktop (2× when the complete image fits
64×64, otherwise 1×); the hotspot is scaled identically. App-hidden cursors
stay hidden on motion; the GPU plane still hides after three seconds idle.
The B1 headless GPU parent uses win32u's title-GPU driver (null-driver behaviour
plus native cursor callbacks), enabled only by the `wineSetCursor` callback.
CPU-only headless children retain the plain null driver. This GPU role does not
load wineps5.drv; that driver publishes maintenance/desktop cursor messages.
Both paths use the single conversion/cache implementation in `wine/ps5_cursor.h`.
The 40-byte cursor prefix includes the top-level HWND (zero means desktop)
and a reserved zero word; the maximum frame is 16,424 bytes. Conversion is
cached per HCURSOR and evicted by DestroyCursorIcon, but every SetCursor republishes.
The compositor uses each window's clipped visible rectangle for ownership and
remembers other windows' states. Both hubs replay cursor snapshots incrementally
in at most 64 KiB batches, retaining 64 KiB of queue headroom for live updates.
Outstanding snapshot entries finish before newer live updates are promoted to the
next generation, preventing frequently changing windows from starving replay.
HELLO flag `PS5_DESKTOP_CURSOR_CAPABLE` requests an opt-in support reply;
no reply means no type-12 publication, preserving old hubs and driver connections.
Capability probes only peek at ordinary input and never probe a buffered partial
frame. The latest pending shape is sent when HELLO completes; the publishing GUI
queue handles late replies. Per-thread queue registration is independent of the
desktop INPUT_SINK activation, including activation from another GUI thread.
Registration uses a connection-mutex-protected thread-id set, cleaned by
ThreadDetach, not ELF TLS. On ELF hosts the cursor host gate compiles the real
desktop driver sources and the native cursor adapter with Wine API/table
declarations, rejecting TLS segments, sections, relocations and unreviewed imports.
Animation uses frame zero and monochrome invert pixels become black. Native
checks must still establish WoW's `gxCursor 0` software/hardware-cursor interaction,
mouselook hide/show, and the VideoOut buffer-switch/cache-flush behaviour.

After building Wine's host tools, run the focused assembler/linker regression
inside the same image (paths below are relative to `ps5-wow`):

```sh
make -s -C ps5/probes/psmkit-wine test-winebuild-delayload \
  LLVM_MINGW=/opt/llvm-mingw \
  WINEBUILD=../../build/wine-ps5-tools/tools/winebuild/winebuild
```

This separate gate uses the actual Wine-generated import archives and MinGW
delay-load helper, links i386 and x86_64 PE consumers, and follows each initialized
IAT pointer to its first-call thunk. Both the archive thunk and linked target
must be readable/executable code without write permission; the IAT must remain
writable data without execute permission. It fails against the old Wine host
tool even when ordinary `.text` and the delay helper are executable. No Wine
runtime, full title build or console execution is needed for this regression.

## Native release gate

Before publishing the corrected release or promoting its catalog entry, install
the exact final `PPSA99030.zip` on the PS5. Repeat the unchanged raw drive probe
in both 32-bit and 64-bit modes, then run Wine's opt-in, read-only regression
`kernel32_test.exe volume ps5-title-drives` in both architectures. Require only
`C:`/`W:` in drive enumeration, local fixed-disk device information for both
roots and their DOS-device targets, and rejection of unsupported `D:`, `Z:`
and UNC paths. Finally restore the default Battle.net startup and confirm its
install destination is accepted rather than rejected as a network folder.
These are required checks, **not recorded native-pass results**; the existing
delay-import regression gate remains required as well.

