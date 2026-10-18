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
JOBS=8 bash ps5/release/build-linux.sh /absolute/SOURCE_ROOT 01.000.012
```

Source validation and the full host suite run before the SDK and runtime builds.

The tag must equal the source `sce_sys/param.json` content version. This builds
the Docker image, then builds the SDK, native title tooling/clean-room libc,
patched RADV, FreeType, GnuTLS, Wine, vkd3d-proton, DXVK, the patched
AddonForge headless core, both B1 roles, and the release archive. Generated
Wine server headers are restored on exit. Use an empty source/output directory
for a cold build; rerunning can reuse outputs.

Outputs under `ps5-wow/ps5/build/`:

- `title-psmkit-wine-b1/`: independently qualified maintenance and game roles.
- `release/PPSA99030.zip`: catalog archive, with size and SHA-256 in the output.
- `release/logs/`: complete per-stage logs and `timings.tsv` (seconds/status).

### Persistent release-build cache

Linux release builds keep compiled outputs in `SOURCE_ROOT/.cache` so the
self-hosted runner can reuse them even though checkout cleans
`ps5-wow/ps5/build`. RADV, FreeType, GnuTLS, vkd3d-proton, DXVK and
AddonForge entries are keyed by their pinned sources and downloads, build
recipes, container image ID, and relevant toolchain environment. Every restored
entry is checked against a
SHA-256 manifest; a mismatch is a cache miss and rebuilds that stage. Wine keeps
its out-of-tree build and host-tool trees there for incremental `make`, but
reconfigures from an empty tree when its configure key changes. Its compiler
flags map the physical cache root back to `/sources`, retaining the debug paths
and release bytes of an ordinary `ps5/build` tree.

Set the Release workflow's `clean` dispatch input to discard this cache before
building. The workflow retains the two newest entries per stage and logs
`CACHE_HIT`, `CACHE_MISS`, and `CACHE_SIZE` receipts. Host tests, both B1 runtime
roles, packaging, and qualification always run from fresh product outputs; the
package allowlist remains the only source of ZIP members.

`ps5/build-wine.sh configure` also retains a per-tree `.configure-stamp.json`
for direct/private incremental builds. Its key covers the configure command,
configure/configure.ac bytes, compiler/tool paths and configure environment
(including target/repro flags, PE linker flags and library sonames). Matching
inputs skip configure, not `make`. Changed C/C++ or PE compiler flags clean
the affected build tree before configuring: Wine does not track flag changes
in object dependencies. A tree without a stamp is configured; its existing
Makefile flags are compared first so switching flags cannot reuse old objects.
Generated Makefile/config.status/config.h hashes also invalidate the stamp if
another private build reconfigures the shared tree without this helper.

Runtime construction decodes each compile-database command and resolves its
output once per role, reusing that metadata for module manifests and the
existing title-ABI freshness checks. Qualification itself is unchanged:
the qualifier binds its own source hash into each qualification key, so even
a behavior-preserving qualifier edit would change the required receipts.
No qualification receipt or file-identity check is reused or skipped.

The package writer compresses independent members with up to 24 threads
(`package.py --jobs 1` selects one). It preserves the serial writer's local
headers, compression settings, member order and central-directory metadata;
workers spool to temporary files and retain at most one member per worker.
`make -C ps5/probes/psmkit-wine test-build-speed` covers configure reuse/clean
decisions and parallel-versus-original-serial ZIP byte equality.

Require two `RUNTIME_TITLE_QUALIFIED` records with `raw_syscalls=0`, followed by
`RUNTIME_TITLE_DUAL_QUALIFIED`. The qualifier's deliberate unknown-import and
win32u-identity rejection records are expected negative tests. Qualification
here is a host build/inspection gate, **not console execution**.

## 01.000.013 source changes

- Before publishing its token or listening, the version-6 write broker repairs
  modes only within fixed launcher-data roots. Traversal is descriptor-relative,
  no-follow, bounded, hard-link-safe, and stays on `/data`'s filesystem device.
- The broker token intentionally retains the existing protocol's `0644` mode in
  the normally `0777` `/data/forever` directory. It authenticates cooperative
  title clients but is readable by other local identities that can traverse the
  directory; this release does not redesign the token scheme.

## 01.000.012 source changes

- B1 staging inventories every Wine builtin/placeholder PE from both installed
  architecture trees and writes its source, prefix destination and SHA-256 into
  `prefix-files.txt`; the existing AGS exception remains excluded. DXVK/vkd3d
  app-owned graphics overrides remain in the same manifest.
- Startup treats every `prefix-files.txt` row as app-owned, including listed DLLs
  that were replaced with native files. The versioned `.forever-managed` stamp
  records each destination's SHA-256, size, nanosecond mtime, device and inode,
  with a checksum covering the complete state. Startup uses `stat` plus
  `open(O_RDONLY | O_NOFOLLOW | O_NONBLOCK)` and `fstat` before trusting any regular
  destination, including a cached one. Only a match of the applicable destination
  key avoids file-content reads, never mtime alone. Hashing reuses that same
  descriptor. `ELOOP`/`EMLINK` from the no-follow open identify symlink replace
  targets, including dangling links; a link to the cached inode is never clean.
  PS5 timestamp granularity can vary by filesystem despite the nanosecond field.
  On PS5, `st_dev` is excluded only from the persistent cache key: the saved
  title value `689700787` and FTP value `687931141` differed for the same files
  whose size, mtime seconds and inode matched exactly. Device IDs remain checked
  within an active hash/copy operation. Size, mtime seconds/nanoseconds and inode
  are still mandatory cache-key fields; other hosts also check device. Existing
  v2 stamps remain usable without another full audit or a format migration.
  Missing records, metadata/identity
  drift or a changed manifest digest trigger SHA-256 verification and repair
  only when content differs. The first upgrade from the manifest-only stamp,
  and any corrupt/partial state, hashes every existing regular destination once.
- No source-file stat is recorded or used for skipping. A store update replaces
  the entire app folder and therefore all source inodes/mtimes, while leaving the
  prefix and its state intact. With an otherwise clean prefix and N changed
  manifest digests, only those N rows are hashed/repaired; unchanged digests and
  matching destination records skip content reads. Every copied destination is
  hash-verified against the manifest, not merely compared with the source, so
  corrupt shipped source bytes cannot be recorded as good.
- Managed copies normally use the atomic temporary-file path and are verified
  before recording their new metadata. If replacing a writable, foreign-owned
  regular destination is rejected specifically by `rename` with `EACCES`/`EPERM`,
  sync opens that destination without following symlinks or truncating first,
  checks its descriptor type/device/inode and link count, then truncates, writes,
  `fsync`s and hash-verifies it in place. Ordinary PS5 files report `st_nlink=0`,
  so both zero and one are accepted; multiple hard links are refused. This narrow
  fallback is not an atomic destination replacement, but normal Wine programs
  remain gated until it succeeds, and failures never publish new state.
  Symlink/FIFO destinations are replaced without following/opening them; empty
  directories can be replaced, but nonempty directories fail without recursive
  deletion. The stamp itself always stays atomic and retains the prior version
  on failure. Remaining permission failures retain the existing broker restart
  and retry path. Unlisted user files are outside this sync.
- Each sync attempt logs `forever: prefix sync checked=N hashed=H repaired=R`,
  followed by `forever: prefix sync repaired <prefix-relative-path>` for at most
  eight repairs. Every failed attempt additionally logs
  `forever: prefix sync failed op=<operation> errno=<errno> path=<prefix-relative-path>`
  **before** permission handling, including the first attempt and any retry.
  This identifies no-follow hash opens, temporary-file operations, rename,
  in-place validation/truncate/write/fsync, and state publication separately.
  `hashed` counts distinct destination rows whose contents were read, including
  verification of repaired copies; a drifted-and-repaired row is counted once.
  State diagnostics report `forever: prefix sync state read=B loaded=N
  publication=attempted|unchanged result=E bytes=W`, including publication failure.
  At most one first actionable mismatch is logged as `forever: prefix sync
  mismatch path=P field=F recorded=A current=B ignored=0|1`. If only excluded
  fields differ, that first difference is reported with `ignored=1` while
  preserving the cache hit; it never hides a changed size or other required field.
- Native title syscall audit: `.39` rejects `lstat` with `EPERM`, so sync does not
  call it or substitute the unproven `fstatat(AT_SYMLINK_NOFOLLOW)`. Host sync tests
  wrap `lstat` with an assertion to prevent its return. Existing title paths use
  `open`, `close`, `stat`, `fstat`, `read`, `write`, `rename`, `unlink` and `mkdir`.
  The ELF verifier already uses `O_RDONLY | O_CLOEXEC | O_NOFOLLOW`. Console
  build `1e10c883` additionally exercised prefix-file no-follow/nonblocking reads
  across all 1,591 rows and exclusive temporary-file creation during 88 repairs.
  Still **unproven on prefix files**: `O_WRONLY | O_NOFOLLOW | O_NONBLOCK`,
  `rmdir` for an empty directory, and `fsync` in the in-place fallback.
  `ftruncate` already services title desktop shared-memory descriptors
  (`runtime-parent.c`), but truncating a regular foreign-owned prefix file remains
  unproven. `chmod` and `utimensat` are host-fixture operations only, not sync
  dependencies. Each unproven operation has a distinct failure diagnostic;
  presence of a kernel import or a passing host test is not console qualification.
- Host cost (Xeon Silver 4116, clang-19 `-O2`, caches not flushed): the staged
  1,591-row inventory totals 1,062,672,384 bytes (1,013.44 MiB). A no-record scan
  took 6.262 s with zero repairs; the next descriptor-inspection scan took 0.0146 s
  with zero hashes. Reproduce with `ps5/build/psmkit-wine-host/test-forever-setup --benchmark
  ps5/build/title-psmkit-wine-b1/wine-title
  ps5/build/title-psmkit-wine-b1/wine-title/share/forever/prefix-files.txt`.
  Console build `1e10c883` on `.39` checked/hashed all 1,591 rows and repaired 88;
  its "Checking the app's Windows files" to Wine-start interval was 16.7 s.
  The corrected persistent-device policy is host-tested with an unstable device
  ID and `hashed=0`; its fast-path PS5 wall time still needs console qualification.
  Later unchanged starts hash no DLL contents.
- The maintenance desktop is opened and presents a frame before prefix sync;
  its independent frame/input thread continues while the startup thread scans.
  Direct-game handoff now also waits until `pw_forever_start` completes, so the
  first-upgrade scan runs behind that live window, never in its rendering loop.
  All manifest DLLs, including ntdll/kernel32 and their startup dependencies,
  are verified before the installer, Battle.net or a game starts; no unverified
  subset is deferred until those programs are running. The extra matching-record
  scan cost measured 14.6 ms on the host; PS5 metadata and maintenance-window
  initialization costs are not measured here.
- Fresh setup keeps its existing wineboot bootstrap path: `WINEBOOTSTRAPMODE=1`
  loads builtins directly from the same `wine-title` build, not an unfinished
  prefix. After wineboot finishes, the watcher stops its remaining processes,
  `forever_finish_init` finalizes the registries, and `pw_native_exit` automatically
  LoadExecs `eboot.bin` in the same user launch. That entry runs managed sync
  before choosing the installer/Battle.net or consuming the direct-game flag.
  No manual relaunch is required, and no normal Windows application starts
  between wineboot completion and this sync.
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

The matching PS5 startup measurement reproduced two crashes in four starts
before the complete builtin sync and observed ten clean starts in ten attempts
after it. This measurement is evidence for the sync change, not final `.012`
release acceptance.

01.000.010 was not published: its build failed on a compiler warning in the new cursor code.

The `01.000.007` workflow failed before publication because of stale import-policy
entries. The `.008` import-policy correction removes only the obsolete
`wineps5.so` `strlen` and `strstr` rows from the B1 policy; all other policy entries
and qualification requirements remain unchanged.

Native acceptance of the exact final `.012` archive is **pending**. These source
changes do not record a native pass; the C:/W:-only scope and release gates below
remain in force.

## Forever menu source changes

- The maintenance role links the CPU-composited Forever menu; the headless GPU
  role does not. Options and F12 are consumed before Wine while the menu is open;
  L2 supplies Battle.net's Escape mapping.
- The large menu uses committed alpha glyph tables generated deterministically
  from SHA-256-pinned Noto Sans Regular 30 px and Bold 40 px files, with Noto
  Sans Symbols 2 for the controller symbols. Builds stay network-free;
  `ps5/title/tools/make-menu-font.py` is the audited regeneration path.
- Menu settings use bounded, atomic file rewrites. The HDR output flag defaults
  to on, preserving the existing `PROTON_ENABLE_HDR=1` game environment; off
  explicitly removes that variable before the game runtime is built.
- Battle.net removal runs through the authenticated write broker and is
  restricted to its three fixed data roots. Reinstall also removes the cached
  installer and restarts into the existing installer path; World of Warcraft
  paths are never deletion roots.

## Add-ons source and packaging

- `deps.json` pins AddonForge commit `0dc9ab28f207f86e33770e359f2345043ef19585`
  and its tracked-content fingerprint. The numbered patches in
  `ps5/addonforge/patches/` add the Tauri-free loopback core, per-session bearer
  token, final-channel mapping and behavioural tests; the upstream checkout is
  copied before patching and is never modified.
- The container pins rustup-init 1.28.2 by SHA-256 and Rust 1.99.1 with the
  `x86_64-pc-windows-gnullvm` target. `cargo vendor --locked --versioned-dirs`
  populates the persistent cache, verifies each vendored `.cargo-checksum.json`,
  and all later Cargo commands use `--offline --locked`.
- The `addonforge` cache key covers the source pin, patches, `Cargo.lock`, Rust
  toolchain and container image. A cache miss builds in two empty target
  directories and refuses unequal `addonforge-core.exe` SHA-256 values. The
  staged `build-receipt.json` records both hashes.
- `wine-title/addonforge/addonforge-core.exe` and
  `forever-maintenance.exe` are Windows PE executables run by Wine. The homebrew
  catalog scan rejects payload ELF executables; it does not classify these PEs
  as payloads, just as it does not classify Wine's other Windows PEs that way.
- Add-on changes load the next time WoW starts, or after typing `/reload` in
  WoW. The manager remains available only in the Battle.net maintenance role.

## Pinned inputs and limits

`Dockerfile` pins the Ubuntu 24.04 image by digest and Ubuntu apt packages
(including LLVM/Clang 18 and 19, SPIR-V translator/libclc 19, glslang, Python,
bison, CMake and autotools) by the immutable 2026-10-01 archive snapshot.
LLVM 19 builds native/title code and RADV's LLVM-dependent host tools.
llvm-mingw 20260922/UCRT (LLVM 23.1.2) and rustup-init 1.28.2 are SHA-256
checked; Rust 1.99.1 and the `x86_64-pc-windows-gnullvm` target are exact
toolchain pins. Python build packages have explicit versions. Ubuntu's fixed
`ca-certificates=20240203` package is unpacked only to bootstrap HTTPS trust;
no packages or dependencies are installed from the live Ubuntu archive before
switching to the snapshot.
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

The `.012` sources retain the two independent drive-reporting fixes:

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
Linux x86-64 runner and defaults to the container's `nproc` value (override with
the `FOREVER_JOBS` repository variable). RADV Ninja and Wine's `install-lib`
honor the same cap.
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

