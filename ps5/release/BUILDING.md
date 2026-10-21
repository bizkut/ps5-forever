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
JOBS=8 bash ps5/release/build-linux.sh /absolute/SOURCE_ROOT 01.000.016
```

Source validation and the full host suite run before the SDK and runtime builds.

The tag must equal the source `sce_sys/param.json` content version. This builds
the Docker image, then builds the SDK, native title tooling/clean-room libc,
patched RADV, whisper.cpp/ggml, FreeType, GnuTLS, Wine, vkd3d-proton, DXVK, the patched
AddonForge headless core, both B1 roles, and the release archive. Generated
Wine server headers are restored on exit. Use an empty source/output directory
for a cold build; rerunning can reuse outputs.

Outputs under `ps5-wow/ps5/build/`:

- `whisper/{include,lib}`: target speech-engine headers and five static archives.
- `title-psmkit-wine-b1/`: independently qualified maintenance and game roles.
- `release/PPSA99030.zip`: catalog archive, with size and SHA-256 in the output.
- `release/logs/`: complete per-stage logs and `timings.tsv` (seconds/status).

### Voice-to-Text dependencies

`ps5/build-whisper.sh` runs after SDK, native tooling and RADV, before the title
runtime build. Its source archives are SHA-256-pinned in
`ps5/release/deps.json`: whisper.cpp 1.9.5 at
`d1be6fde11ac6e0407606b4e42fe72d34add8037` and ggml 0.26.0 at
`d7cb574130e6f01ad25b3289685489200febcd74`. whisper's bundled ggml is not used.
The install is `ps5/build/whisper/{include,lib}` and contains `libwhisper.a`,
`libggml.a`, `libggml-base.a`, `libggml-cpu.a` and `libggml-vulkan.a`.

After the SDK and RADV outputs exist, rerun just the real release speech stage:

```sh
JOBS=8 bash ps5/release/build-linux.sh /absolute/SOURCE_ROOT 01.000.016 --whisper-only
```

This uses the same `build_whisper_cached` function and cache as a full release,
without running host tests, rebuilding prerequisites, or touching Wine/title
outputs. It is not runtime qualification. Inside the release container, the
equivalent entry is `bash ps5/release/build-linux.sh --container 01.000.016 --whisper-only`.
Both use `PS5_PAYLOAD_SDK=/sources/PS5_Vulkan/.deps/native/ps5-payload-sdk`.
The underlying `ps5/build-whisper.sh` also supports direct invocation:
`PS5_WHISPER_BUILD` and `PS5_WHISPER_INSTALL` select empty work/install
directories for a cold build; defaults are `ps5/build/whisper-build` and
`ps5/build/whisper`. A completed install with a matching input stamp is skipped.

The CMake toolchain targets `x86_64-sie-ps5`, libc++, PIC and emulated TLS.
CPU AVX/AVX2/FMA/F16C and Vulkan are enabled; host-native tuning, AVX-512,
OpenMP, BLAS and ccache are disabled. The container's pinned `glslc` generates
Vulkan shaders with a host shader-generator executable. Vulkan C/C++ headers
come from the pinned vkd3d-proton Vulkan-Headers checkout; a private include
shim exposes only SPIR-V headers, not host libc headers. The recipe preserves
the SDK `isascii` compatibility header, no-host-libm override, `GGML_MAX_NAME=160`,
and 192 MiB CPU / 128 MiB Vulkan allocation-block limits. The speech model is
downloaded at runtime, never bundled into the dependency or release archives.

The game-parent hold classifier allows an 80-touch-unit radial excursion from
the press origin before 400 ms (not accumulated finger travel). A change in
contact ID or touch count rebases that origin instead of counting as movement.
After recording starts, touchpad motion and button state stay suppressed until
release, even if the finger moves beyond the slop.

The native game-parent present bridge draws a centred red-dot **Listening**
cue only while the recording hold is active. It reuses the compositor's 5x7
bitmap font and one immutable host-visible raster per swapchain. Visible
presents submit a cached transfer command buffer on the present queue, waiting
on the application's original semaphores and signalling an image-owned semaphore
for the real present. With voice enabled, the raster and every image's command
buffer/fence/semaphore are prepared at swapchain creation when the present family
is known, otherwise at the first hidden present. Copy commands are pre-recorded
there to warm the RADV meta-copy path before the first cue. A live enable requests
the same preparation during the 400-ms press deferral. Visible holds never allocate,
rasterize or record resources. Preparation logs
`voice overlay warmup format=<VkFormat> images=<count> status=<VkResult>`.
Hidden presents retain the original wait list and never submit or copy overlay
work; after this one-time preparation they remain direct presents.
The copy is submitted after the FG output deadline hold, so it can wait behind
intervening game rendering (`vkd3d-proton/libs/vkd3d/swapchain.c:4170-4183`).
That visible-only scheduling delay is accepted for brief recording holds;
outside holds the direct present path and FG scheduling are unchanged.
Swapchain recreation/destroy retires the overlay resources with the device idle.
Supported formats are BGRA8/RGBA8 UNORM/SRGB and A2B10G10R10/A2R10G10B10;
ST2084 10-bit uses the PQ palette (200-nit white). Other formats skip the cue.
Each visible recording session logs `voice overlay format=<VkFormat> shown=<0|1>`
once, with no transcript or microphone data.

At the first touchpad press the game parent logs
`voice setting path=/data/forever/prefix/drive_c/wineps5-voice.txt rc=<errno>`.
Every press end logs
`voice hold enabled=<0|1> result=<passthrough|click|drag|record> held_ms=<uint> max_move=<uint> setting_rc=<errno>`.
The setting result is the `fopen` errno (0 on success, -1 if not attempted);
`max_move` is the peak radial excursion rounded up to an integer touch unit.
These diagnostics contain neither setting-file contents nor audio/transcript text.

All native downloads disable sceHttp2 automatic redirects, which can return
the final status/length without its body. The shared stream follows
301/302/303/307/308 with fresh GET requests, at most five redirects per attempt.
Locations must resolve to HTTPS (relative paths use the current URL directory);
empty, overlong and downgrade locations are rejected. Redirect bodies are
discarded. Every voice-model retry starts again at the pinned resolve URL,
and Range is reapplied at each hop instead of caching an expiring CDN URL.
Both installer and model downloads log
`forever: download hop=<n> status=<http> host=<host> length=<bytes>`;
only the hostname is logged, never a CDN path or presigned query.
Voice attempts also log `forever: voice model first_read=<result>`.
The installer still tries its direct versioned fallback if the Live URL fails.

Only voice-model transfers retry premature EOF/receive errors: at most six
attempts with cancelable 1/2/4/8/8-second backoff. A resumed response must be
206 with a matching `Content-Range` before bytes enter the streaming SHA-256;
a 200 response to a Range request resets/truncates the staged download.
Logs contain resume offsets and failure byte/status/attempt fields. The
Battle.net installer remains non-resuming.

### Persistent release-build cache

Linux release builds keep compiled outputs in `SOURCE_ROOT/.cache` so the
self-hosted runner can reuse them even though checkout cleans
`ps5-wow/ps5/build`. RADV, whisper.cpp/ggml, FreeType, GnuTLS, vkd3d-proton, DXVK
and AddonForge entries are keyed by their pinned sources and downloads, build
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

## All WoW channels (014)

`ps5/probes/psmkit-wine/wow-channels.h` owns C-side channel discovery and path
parsing. An immediate `_<name>_` folder under `World of Warcraft` is listed when
it has `.flavor.info` or an executable matching `^Wow[A-Za-z]*\.exe$` without
case sensitivity. Direct start additionally requires the executable and its
exact-stem or lowercase-stem `_loader.dll`. `.flavor.info` is authoritative;
the optional `.build.info` fallback uses only an identified matching row.
Battle.net can replace `.build.info` with just one product row, so it never
controls discovery and never supplies another channel's product.

Direct selection maps the TACT products `wowt`/`wowxptr` to Agent's
`wow_ptr`/`wow_ptr_x` uids. Play's arguments/uid are preserved; only the
executable leaf's case is normalized to its actual on-disk spelling.

Discovery intentionally scans only the default
`C:\Program Files (x86)\World of Warcraft` root. Battle.net's binary
`.product.db` / `Launcher.db` are not parsed. Play handoffs accept structural
channel paths at any C:/W: install root; custom roots are outside automatic
settings/Add-ons/direct-start discovery for now.

Both the maintenance handoff and GPU startup resolve the requested executable
and sibling loader with ASCII case-insensitive directory matching. Each must
have exactly one native spelling; two case variants or an incomplete/erroring
directory scan refuse launch. The resolved on-disk executable spelling enters
the game wire and recorded replay, and existence checks use the resolved
loader spelling. This covers Battle.net's `WoWClassic.exe` against installed
`WowClassic.exe`/`WowClassic_loader.dll`, as well as `Wow.exe`/`wow_loader.dll`
and `WowB.exe`/`WowB_loader.dll`, without special-casing an executable.
Discovery still lists incomplete/ambiguous channels for settings and Add-ons
with an empty loader; they do not block maintenance startup or other channels.

The maintenance parent records each accepted Battle.net Play launch under
`/data/forever/state/last-launch-_<channel>_.bin`. The record contains the
existing versioned, NUL-separated argv/cwd wire (at most 4,096 bytes and 64
game arguments), not native pointers. Direct start replays that channel's
record, preserving safe game options and Agent UID differences without
a per-product launch table. Until Play has supplied a record, direct start
keeps the existing `-launcherlogin -uid <flavor>` fallback; that fallback
cannot supply product-specific launch options such as Anniversary's
`-initialgamemode=bccfresh`.

Only Play publishes records; replay/fallback never overwrites them. Publishing
is atomic and a save failure does not block the original Play launch. Malformed,
oversized, wrong-channel, different-installation or unsafe-option records are
refused, not silently replayed as another channel or downgraded to fallback.
Recorded executable and both cwd forms must match the selected installation;
DOS paths compare case/slash-insensitively and cwd trailing separators are
ignored for identity only. The exact saved cwd is replayed. Every replay still
passes the normal prefix, executable and sibling-loader admission.

Persistence uses a generic option/type allow-list, not a keyword deny-list:
the standalone `-launcherlogin`, `-console` and `-windowed` booleans, `-uid`
with a validated `wow*` product identifier, and `-initialgamemode` with a
symbolic mode value of at most 32
letters/digits/underscores/hyphens (starting with a letter). Inline `=`/`:` and
separated values are covered. Unknown/config options, login/session tokens,
attached login values, invalid typed values and unexplained positional values
are omitted. An unknown option immediately followed by another switch has
ambiguous arity: recording is refused without replacing the previous record,
instead of losing required game configuration or interpreting a secret as a
safe option. This intentionally does not preserve arbitrary launch options:
opaque values cannot be proven free of credentials. Replay rechecks the same
allow-list. Only argument counts and redacted argv/cwd markers are logged.

The single `b1-game-wow.json` has `runtime_game: true`, with no configured
executable, argv, uid or channel cwd. `runtime-b1-dual` still separately builds
and qualifies maintenance and GPU roles; profile reconstruction validates both
standalone and combined runtime-game receipts. The GPU parent requires the
validated Play/direct-game wire before entering Wine. No channel overrides or
rendering changes were added. The exact `Wow.exe` dynamic-MSAA workaround in
vkd3d already covers both Retail and Forever and is unchanged. Wine's CPU
diagnostic bucket groups `Wow*.exe` without interpreting it as a product.

Every parent start (Play handoff, direct selection, or game-role relaunch)
passes `main`'s `pw_forever_start()` gate before `wine_public_title_main`.
`pw_forever_start` calls `pw_forever_sync_managed`, logs
`forever: prefix sync checked=… hashed=… repaired=…` in either role, and returns
failure on an unrepaired sync error; `main` then returns without entering Wine.
Direct start is only attempted after this gate and the new GPU process repeats
it. `build-runtime.py` generates app-owned destinations for the manifest-owned
system32/syswow64 builtin PEs (retaining wineboot's AMD AGS exception),
system32 d3d12/d3d12core/dxgi and the two AddonForge PEs.
There are no channel-specific DLL destinations. FG/HUD/tune/HDR switches are
shared `drive_c/wineps5-*.txt` user settings, not managed game-folder files.
The missing-only Config.wtf preset is the only app-supplied channel-local file.

The combined private test build `allwow-a344643a.zip` rendered Retail
(`_retail_/Wow.exe`) through direct selection at 90 fps. An unavailable `_ptr_`
selector refused without fallback. Classic Era started `WowClassic.exe` after
prefix sync checked 1,591 files, then `game.bin` faulted just after display-driver
initialization with `SYSTEM_XO_VIOLATION` (`0xa0020328`, RIP `0x8000003e`).
That crash is under investigation; the observation does not qualify Classic Era,
other channels, per-channel Add-ons, or online compatibility. Battle.net had
rewritten `.build.info` to a Retail-only row; whether that contributes to the
Classic Era failure is not established.

## Add-ons source and packaging

- `deps.json` pins AddonForge commit `0dc9ab28f207f86e33770e359f2345043ef19585`
  and its tracked-content fingerprint. The numbered patches in
  `ps5/addonforge/patches/` add the Tauri-free loopback core, per-session bearer
  token, final-channel mapping, install safety and behavioural tests.
  `0006-wago-key-control.patch` follows 0005 on that unchanged upstream base:
  `set_wago_key` trims and persists (empty clears), and `wago_key_status` returns
  only `set` and `valid`. One bounded, authenticated add-on metadata request
  rejects 401/403 without saving; an unavailable check saves with `valid: null`.
  Redirects and HTML pages never count as successful validation. Neither
  response nor log includes key text. The upstream checkout is copied before patching.
  `0007-all-wow-channels.patch` extends product labels, reads the selected
  executable's PE `VS_VERSION_INFO` FileVersion, and derives Interface as
  `major*10000 + minor*100 + patch`, with a product-only fallback when unavailable.
  Its source capability table uses Wago `retail/classic/bc/wotlk/cata/mop/forever`;
  GitHub uses release.json flavour/interface metadata and familiar asset tags
  (`mainline`, `classic`, `bcc`/`tbc`, `wrath`, `cata`, `mists`/`mop`, `forever`).
  WoWInterface/Tukui metadata is checked against that same target family.
  Every archive's TOCs are checked before replacing any installed folder.
  Archive admission and installed scanning share the selected-flavour TOC
  selector: Mainline, Classic/Vanilla, BCC/TBC, Wrath/WOTLKC, Cata, Mists and
  Forever/Camelot take precedence over the generic TOC. Version and source
  identifiers are never merged from a different flavour.
  Unknown products or unsupported engine families fail before source resolution;
  a future Classic expansion cannot silently select Retail add-ons.
  Per-channel installed records,
  pins and ignores are switched with the selected path. A Forever-only catalogue
  or starter pack list is never offered as another flavour.
  Key validation uses an authenticated metadata URL without `game_version`.
  Loopback Find/catalogue and starter-pack results use `{rows, notice}`.
  Unavailable catalogue data is a successful empty informational view, so a
  completed GitHub install or Wago-key change still reports success. Actual
  failures preserve the known global Wago-key status unless the key operation
  reports rejection or removal.
  Mapping references (facts only, no copied implementation):
  [Wago provider](https://github.com/WowUp/WowUp/blob/5baca162dacdbdf86044ae25933ddbf0fa9bcdc9/wowup-electron/src/app/addon-providers/wago-addon-provider.ts),
  [client groups](https://github.com/WowUp/WowUp/blob/5baca162dacdbdf86044ae25933ddbf0fa9bcdc9/wowup-lib/src/utils/wow-client.utils.ts),
  [Tukui provider](https://github.com/WowUp/WowUp/blob/5baca162dacdbdf86044ae25933ddbf0fa9bcdc9/wowup-lib/src/addon-providers/tukui-addon-provider.ts).
- `0008-all-flavour-catalogues.patch` makes Find's primary source the
  [GPL-3.0 GitHub catalogue by layday](https://github.com/layday/github-wow-addon-catalogue).
  CSV parsing uses the maintained `csv` crate, including quoted commas,
  doubled quotes and multiline descriptions. Exact comma-separated flavour
  tokens map `mainline/bcc/classic/mists/forever` to the selected channel;
  repositories deduplicate case-insensitively against the existing Forever
  catalogue while preserving its stable IDs and folder mappings.
  GitHub resolution continues to prefer the packager's `release.json`
  flavour/interface metadata, falling back to the existing tagged-asset logic.
  The runtime GitHub cache has a 24-hour TTL, conditional `If-None-Match`/304
  refreshes and last-good offline fallback. The committed
  `ps5/addonforge/github-catalogue.csv` is compiled in for first-run offline use
  and packaged as `wine-title/addonforge/github-catalogue.csv`. The receipt pins
  its SHA-256, byte size and row count. `build-linux.sh` copies this exact input
  before cache lookup and includes its hash in the AddonForge cache key; it
  never fetches a live CSV. `build.py --catalogue-snapshot` replays the same
  bytes for both cold builds.
  Generated notices include the catalogue attribution and full GPL-3.0 text.
- `0009-own-catalogue-release-streams.patch` switches both the runtime endpoint
  and bundled input schema to
  [Forever's catalogue fork](https://github.com/bizkut/github-wow-addon-catalogue),
  derived from layday's GPL-3.0 data. Optional trailing CSV fields select a
  literal release-tag prefix, an exact or single-wildcard asset name, and the
  installed folder set. Plain layday-format rows still parse with empty
  selectors. Install and update pass the same selectors, so monorepo packs
  never cross tag streams; folder-first reconciliation attaches hand-installed
  copies to the matching row even when several rows share one repository.
  Exact asset rows also separate distinct add-ons published in one release.
  Add-on downloads stream to disk with a 4 GiB safety cap; the title allows
  30 minutes for install/update/remove operations so the largest catalogue
  audio packs can download and extract without the loopback request expiring.
  [WoWInterface v4 filelist](https://api.mmoui.com/v4/game/WOW/filelist.json) and
  [Tukui v1 addons](https://api.tukui.org/v1/addons) are secondary indexes,
  cached for six hours with last-good fallback. WoWInterface downloads use
  v4 `filedetails/<id>.json`'s `downloadUri`; compatibility comes from
  filelist `gameVersions` and the downloaded archive's selected-flavour TOCs.
  All cache updates flush a sibling temporary file and atomically replace the
  destination with `std::fs::rename` (including Windows replacement semantics);
  failed replacement never unlinks the last-good cache. Index bodies are
  streamed with an 8 MiB cap independent of missing/misleading Content-Length.
  Cache reads and JSON encoding are capped at 16 MiB before parsing/promotion.
  Oversized responses retain last-good data; oversized cache files fall back
  to the bundled GitHub snapshot or the curated starter pointers.
  Secondary compatibility filters require current 12.x/5.5.x/2.5.x/1.15.x/
  1.60.x families, not obsolete Retail or original-MoP builds.
  Starter packs use verified author GitHub releases (BetterBags, BagSync,
  ImproveAny, MinimapButtonButton, XanChat and Immersion), plus ElvUI from
  Tukui for Retail/MoP/Anniversary/Era. They need no Wago key. CurseForge is
  still information-only; no CurseForge download path is introduced.
  The C client keeps all rows for local Find search: 4,096 bounded rows and a
  2 MiB response limit, with an explicit overflow error instead of truncation.
  Large host fixtures use static storage, as the production menu already does.
  Run `build.py ... --check-catalogue` to resolve every starter for every
  family, download its ZIP and exercise the production TOC-admission/install
  path in temporary directories. It writes `catalogue-check.json` with counts,
  actual download URLs, installed folders and serialized Find response sizes.
  This is host source/download qualification, not console or in-game validation.
- The container pins rustup-init 1.28.2 by SHA-256 and Rust 1.99.1 with the
  `x86_64-pc-windows-gnullvm` target. `cargo vendor --locked --versioned-dirs`
  populates the persistent cache, verifies each vendored `.cargo-checksum.json`,
  and all later Cargo commands use `--offline --locked`.
- The `addonforge` cache key covers the source pin, patches, `Cargo.lock`, Rust
  toolchain, container image and exact GitHub catalogue snapshot. A cache miss
  builds in two empty target directories and refuses unequal
  `addonforge-core.exe` SHA-256 values. The
  staged `build-receipt.json` records both hashes.
  Rust source paths map to `/usr/src/addonforge` and vendor-cache paths to
  `/cache`, including panic-location strings. The first pre-tag CI build exposed
  the missing vendor remap: `/sources/.cache` produced different bytes from a
  private `/cache` build despite matching source, patches, lockfile and CSV.
  The explicit cache remap removes that physical-path dependency.
  The 0008 receipt includes patch SHA-256
  `2e47655f98d1cd7702ffd9b372711d5c7ed340bdbe290ac315be5881a8539a2b`;
  its two cold Windows builds both produced
  `346dc4260717db86dbe05a5f5c28fa89bc8f0ce2e4196cec369ec0f66defefbc`
  (3,286,016 bytes). The unmodified CSV contains 2,421 rows, is 609,505 bytes,
  and has SHA-256
  `8d5b512a8bf7b2d6107f2b2a2407a0f24485752cca6e58b6771c7a7b856dbd09`.
  All 37 offline headless Rust tests passed with an empty Cargo home; the
  separately enabled live qualification passed 34 starter/flavour archive
  installs and three WoWInterface download/TOC smoke checks (Retail, BC, Era).
  Find row counts were Retail 2,320; MoP 640; Anniversary 1,040; Era 1,131;
  Forever 115. These include catalogue information rows; only the starter
  entries and named smoke downloads received real-archive qualification.
  The 0009 receipt pins patch SHA-256
  `8282c3d53d47a279bae4679d45e0b58db087e44ede95c08b56b50f03021a0a78`.
  Both cold Windows builds produced
  `37a72c503f4455a7ab42aa0c7f116a16e0966cbcdbaa9b986e68de9e840d6e1d`
  (3,430,400 bytes). Forever's customized snapshot contains 2,674 rows,
  is 741,858 bytes, and has SHA-256
  `538c62389b307b1f3dedc28a6ab72cff9191b7d0ac365ef6536810bcd6fdc6fa`.
  All 42 enabled offline headless Rust tests passed; the live endpoint/archive
  qualification remains separately opt-in.
  The C client test passed a >512 KiB, 3,000-row catalogue, searches for rows
  129 and 2,999, and explicit overflow rejection. Menu and desktop-surface
  host tests also passed after moving their enlarged fixtures off the stack.
  ReviewCatalog cleared the final patch, including atomic cache replacement,
  forced replacement failure and bounded live/cache input regressions.
  The receipt, live report, raw CSV, executables and notices are in
  `private-builds/catalogue-014-addonforge/` and staged in both canonical
  `ps5/build/addonforge` and the rc-013b build tree. No console operation or
  in-game add-on behavior was qualified by this work.
  Private menu-diag input can select `addon menu:wago`, open it with `pad CROSS`,
  choose Type key with another `pad CROSS`, then submit `text <key>`.
  Text command contents are never logged, and frame dumps are refused while
  the Wago keyboard is open. The current inline IME wrapper exposes no
  qualified password mode, so it uses the existing plain keyboard.
- `wine-title/addonforge/addonforge-core.exe` and
  `forever-maintenance.exe` are Windows PE executables run by Wine. The homebrew
  catalog scan rejects payload ELF executables; it does not classify these PEs
  as payloads, just as it does not classify Wine's other Windows PEs that way.
  `runtime-b1-dual` does not build AddonForge: `build-runtime.py` copies these
  files, their receipt and the exact `github-catalogue.csv` source snapshot from
  `ps5/build/addonforge` into each qualified title root. The raw CSV accompanies
  its GPL notice and permits exact receipt-pinned offline rebuilds.
  `package.py` packages that already-qualified root and does not reread or
  rebuild the staging directory. Rebuild the dual title after replacing staged
  AddonForge files. The full `build-linux.sh` pipeline has a preceding,
  patch-content-keyed AddonForge build/cache stage; direct private runtime
  builds must supply the fresh artifacts themselves. If a staging path is a
  cache-output symlink, replace that symlink rather than modifying a
  content-addressed cache entry.
- Add-on changes load the next time WoW starts, or after typing `/reload` in
  WoW. The manager remains available only in the Battle.net maintenance role.

## Pinned inputs and limits

`Dockerfile` pins the Ubuntu 24.04 image by digest and Ubuntu apt packages
(including LLVM/Clang 18 and 19, SPIR-V translator/libclc 19, glslang, glslc,
SPIR-V headers, Python, bison, CMake and autotools) by the immutable 2026-10-01 archive snapshot.
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

