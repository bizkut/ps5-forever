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
JOBS=8 bash ps5/release/build-linux.sh /absolute/SOURCE_ROOT 01.000.019
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
JOBS=8 bash ps5/release/build-linux.sh /absolute/SOURCE_ROOT 01.000.019 --whisper-only
```

This uses the same `build_whisper_cached` function and cache as a full release,
without running host tests, rebuilding prerequisites, or touching Wine/title
outputs. It is not runtime qualification. Inside the release container, the
equivalent entry is `bash ps5/release/build-linux.sh --container 01.000.019 --whisper-only`.
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

Require two `RUNTIME_TITLE_QUALIFIED` records with `raw_syscalls=0`, followed by
`RUNTIME_TITLE_DUAL_QUALIFIED`. The qualifier's deliberate unknown-import and
win32u-identity rejection records are expected negative tests. Qualification
here is a host build/inspection gate, **not console execution**.

## 01.000.012 source changes

- B1 staging inventories every Wine builtin/placeholder PE from both installed
  architecture trees and writes its source, prefix destination and SHA-256 into
  `prefix-files.txt`; the existing AGS exception remains excluded. DXVK/vkd3d
  app-owned graphics overrides remain in the same manifest.
- Startup processes that manifest per entry. It copies changed entries, repairs
  missing or wrong-size destinations even when the manifest is unchanged, and
  preserves existing non-builtin native replacements. Copies are atomic; the
  `.forever-managed` manifest stamp is published only after every required copy
  succeeds, so interruption leaves the prior stamp intact.
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

`python3 ps5/writebroker/benchmark-writebroker.py --output /tmp/wb-native
--sdk /path/to/payload-sdk --wine /path/to/ps5-wine` prepares native and broker
probe ELFs without contacting a console. Add `--execute --host CONSOLE_HOST`
to run the native authid/write/pwrite matrix. `--mode broker-ab` requires exact
`--baseline-broker`, `--candidate-broker` and `--restore-broker` ELFs and restores
the supplied broker even after a failed run. `--mode ftp-network` measures the
invoking LAN host's FTP path, not isolated disk throughput.

Execution requires an idle title and at least 2 GiB free, uses a fresh synthetic
scratch root, records payload/log digests and separate preparation, write,
verification, flush and close times, and retains failed-run files for diagnosis.
It never launches, kills or installs a title or changes live tuning.

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

## Prefix signature-provider readiness

- A usable prefix requires the existing hives and timestamp, no in-progress
  marker, and an exact persistent `.wineboot-trust-ready` proof. Existing
  prefixes without that versioned proof are repaired in place; user registry,
  game and settings files are preserved.
- Title `wineboot -i` performs normal native and Wow64 Wine registration even
  when the timestamp is current. It requires successful registration children,
  exact Trust and PE-SIP entries in both registry views, checked server-owned
  hive saves, and an exact binary transient proof. The native title commits the
  persistent proof only after Wine and its registry hives have quiesced and the
  prefix directory is synchronized.
- `WinVerifyTrust` now returns `TRUST_E_PROVIDER_UNKNOWN` when an action has no
  verification callbacks instead of reporting success with zero signers. This
  does not bypass signatures or relax certificate policy.
- Console `.41` qualification used an isolated disposable prefix. With all 24
  Trust/PE-SIP registrations removed, the previous path returned
  `WinVerifyTrust=0` with zero signers and `CryptQueryObject=0x80092009`,
  reproducing Blizzard's incompatible success-then-no-signer state. The
  corrected build repaired both views through normal `wineboot`, persisted the
  exact 25-byte proof after checked hive saves, and accepted the reporter's
  signed Agent with one signer in both 32- and 64-bit probes. A changed signed
  byte, unsigned image and truncated image were rejected; an unknown action
  returned `0x800b0001`. The unchanged 01.000.018 user prefix then migrated in
  place in 36 seconds and started Battle.net. This verifies the mechanism on
  `.41`; it is not a claim that the reporter's firmware was tested.


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
- CI uses the committed `ps5/addonforge/github-catalogue.csv` snapshot, not a
  live fetch: 750,358 bytes, 2,699 rows, SHA-256
  `e25e6521ef0e2e8db05965abe95b12c05d92a06add4f74b220434ca28db9a590`.
  It is compiled into the core and packaged as
  `wine-title/addonforge/github-catalogue.csv`; runtime refreshes remain separate.
- `0009-own-catalogue-release-streams.patch` uses Forever's daily-refreshed
  [catalogue fork](https://github.com/bizkut/github-wow-addon-catalogue), which
  remains GPL-3.0 and credits layday. Optional tag-prefix, asset-name and folder
  columns keep monorepo and multi-pack installs and updates on their selected
  release stream; old layday-format rows remain valid. Cache records bind their
  format and source URL, so upgrades reject fresh legacy/foreign cache data and
  fall back to this bundled snapshot if immediate refresh fails.
  Tag-prefix resolution walks GitHub REST releases in order using 100 releases
  per page, stops at the first eligible match or a short/empty page, and caps
  traversal at 1,000 pages. API/transport errors and the safety cap are reported
  separately from exhaustive no-stream results; neither falls back to latest.
  `0010-distinct-github-release-streams.patch` deduplicates GitHub rows only
  when both repository and release selectors match, so independently packaged
  add-ons from one repository remain separate Find entries.
  `0011-native-addon-worker.patch` streams release bytes through the separately
  versioned `wine-title/addon-worker.elf` payload instead of Wine filesystem
  writes. A separate worker key travels over the inherited ELF-loader socket;
  mutual HMAC challenges authenticate loopback requests without sending it.
  The retained loader socket leases the worker to the Add-ons page/title.
  Archive writes stay in helper-owned, read-only-to-Wine
  `C:\Forever\AddonStaging\<operation>` trees. The helper enforces a 4-GiB
  archive, 16-GiB expansion, 32,768-entry, ASCII Windows-safe path/depth,
  implicit-directory/case collision, link/special-file, CRC, size and dual
  SHA-256 bounds. Hashing, metadata validation and parallel extraction share
  one opened archive identity. One-MiB coalescing feeds eight extraction workers,
  followed by a bounded file/directory durability pass. AddonForge scans executable
  content and bounded selected-flavour TOCs before authorizing activation.
  The helper then renames only into a validated channel below the fixed
  `C:\Program Files (x86)\World of Warcraft` root; a private inode-bound journal
  recovers interrupted replacements and retains unrestored backups. There is
  no silent Wine fallback after native staging starts.
  Removal still hides folders through same-parent renames and writes a
  completion marker. The native maintenance parent later removes completed
  trash outside Wine, one no-follow entry at a time with 5-ms pacing; it resumes
  completed crash leftovers on the next start. Markerless partial renames are
  rolled back by AddonForge. Title mutations allow 30 minutes.
- Private native-worker qualification on console `.41`: Lore Forever v0.26.0,
  373,473,339 ZIP bytes, 5,980 extracted files and four activated folders.
  Candidate ZIP SHA-256:
  `dd481a471027e4196637527b4e1449a697db3a669c00811c6ee83bcd795bc9ff`.

  | Observed phase | Time |
  | --- | ---: |
  | Download and durable archive reception | 53.427 s |
  | Extraction, validation and durability | 46.797 s |
  | Activation and commit | 10.675 s |
  | Total between core install log timestamps | 120 s |

  The total also includes core safety/admission and state work. This is one
  native install workload, not an old/new speed comparison on the same console,
  an in-game add-on test, or a console power-cut qualification. Host regressions
  cover rejected archives, UID isolation, rollback/recovery, cancellation,
  protocol deadlines and serialized request handoff.
- Add-on metadata retains a 120-second total timeout. Archive bodies use a
  separate 15-minute total, 30-second read-idle and 15-second connect budget,
  matching native STORE bounds. Body-transfer, timeout and JSON/schema errors
  are reported separately; no retry or source substitution is added.
- Catalogue cache generation `3` invalidates 01.000.018's obsolete package
  identities once, then retains the 24-hour no-network TTL. The bundled Polestar
  row is its complete 24-folder release; Recorder is an independent optional
  asset.
- In native-title mode, only the native parent cleans completed removal trash.
  Local/GUI cleanup remains available. Both delete and synchronize payload before
  removing the root `.complete` marker, so an interrupted removal cannot restore
  a partially deleted add-on.
- Production-profile 01.000.019 candidate
  `c13a770b1b6541a9ed00d8a2aeff350569cbdd215128b2f9193398ff024ce31e`
  was exercised on console `.41` with the existing 01.000.018 catalogue cache.
  Generation `3` refreshed to 558 rows and exposed separate Polestar/Recorder
  rows. The complete Polestar v0.2.6 asset activated all 24 expected folders.
  Spoken Quests Audio (All) v2.4.0 then crossed the former 120-second body limit
  without an error:

  | Package | ZIP bytes | Files | Download | Extract | Activate |
  | --- | ---: | ---: | ---: | ---: | ---: |
  | Polestar v0.2.6 | 38,395,132 | 447 | 7.114 s | 4.849 s | 0.891 s |
  | Spoken Quests Audio v2.4.0 | 1,479,238,126 | 14,981 | 164.295 s | 152.552 s | 22.694 s |

  The Spoken operation completed four folders in 361 seconds between core log
  timestamps; the independent monitor observed 354.017 seconds. Install and
  update share this archive-body client, but this console run was a fresh
  installation, not an old-version-to-new-version transition.
- Removing those 28 test folders left the root completion marker present while
  the 14,981-file payload was partially deleted. Native cleanup then removed
  15,499 entries across both committed roots in 380.439 seconds, removed each
  marker last, and left only the pre-existing HDRTuner plus an empty hidden
  staging directory. This is a live interruption-state observation and clean
  resumption, not a console power-cut test.
- The container pins rustup-init 1.28.2 by SHA-256 and Rust 1.99.1 with the
  `x86_64-pc-windows-gnullvm` target. `cargo vendor --locked --versioned-dirs`
  populates the persistent cache, verifies each vendored `.cargo-checksum.json`,
  and all later Cargo commands use `--offline --locked`.
- The `addonforge` cache key covers the source pin, patches, `Cargo.lock`, Rust
  toolchain, container image and exact GitHub catalogue snapshot. A cache miss
  builds in two empty target directories and refuses unequal
  `addonforge-core.exe` SHA-256 values. The staged `build-receipt.json` records
  both hashes.
  Rust source paths map to `/usr/src/addonforge` and vendor-cache paths to
  `/cache`, so the CI cache's physical location does not change the executable.
- `wine-title/addonforge/addonforge-core.exe` and
  `forever-maintenance.exe` are Windows PE executables run by Wine. The homebrew
  catalog scan rejects payload ELF executables; it does not classify these PEs
  as payloads, just as it does not classify Wine's other Windows PEs that way.
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
stay hidden on motion. Both the desktop compositor and GPU plane hide the cursor
image after three seconds idle. Desktop motion, a newly selected visible shape
or a mouse-button press wakes the image without overriding an app-hidden cursor;
an idle hide damages only its previous rectangle and leaves pointer ownership intact.
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

## WoW CASC download write buffering

The PS5 title ntdll buffers up to eight 1 MiB extent slots (8 MiB total) for
synchronous regular-file writes to exact case-insensitive `data.NNN` names under
WoW `Data/data/`, including directly nested `_*_` channels, on C: or W:. Each
handle may retain four nonoverlapping extents; sequential continuations append
to the matching extent, while independent files remain buffered concurrently.
Observations of a file drain its identity without flushing unrelated files.
Global barriers still drain archive data before `.idx` updates. An idle worker
checks each extent's 40 ms residence deadline at 10 ms intervals.

Each selected identity drains from highest offset to lowest, extending the
archive through normal writes before filling its earlier extents. Global drains
visit identities in first-pending order. Overlaps, per-handle capacity and slot
eviction drain the whole affected handle; writes are never pre-extended.

Normal flushes retain the existing write-broker routing. A pinned fd and
`(dev, ino)` identity protect retry state; partial errors retain their unwritten
suffix and are reported to the owner handle without discarding other extents.
Shutdown retries retained bytes once. Scoped or unresolved non-image mappings, in-process
hard-link creation and renames into scope disable buffering before draining.
`SEC_IMAGE` and resolved out-of-scope mappings do not disable it. Link counts
zero and one are admitted because Prospero reports zero for ordinary files;
counts above one are rejected. Cross-process mappings and external/pre-existing
hard-link aliases are outside this process-local guarantee.

`WINEPS5_WRITE_BUFFER=0` or live `write_buffer=off` in `C:\wineps5-tune.txt`
restores synchronous writes. A diagnostic build can set
`WINE_PS5_EXTRA_CFLAGS=-DWINE_PS5_WRITE_BUFFER_DIAG=1` when building Wine to
emit one-time `title writebuf state=... reason=...` decisions and 10-second
path-free write-size, handle/identity, barrier, actual drained-byte/run and bounded
offset-sample diagnostics. The host suite
runs both Wine's policy tests and `test-write-buffer`, which extracts the
production implementation from `WINE_SRC`; use a Wine revision containing this
implementation, not an older release pin.

`make -C ps5/probes/write-buffer-bench LLVM_MINGW=/opt/llvm-mingw` builds a
Windows probe that writes scratch `data.000` in 2–64 KiB chunks, verifies every
byte and removes its scratch tree. It accepts a MiB argument or
`C:\write-buffer-bench-mib.txt`, and reports through
`C:\write-buffer-bench-result.txt`. On console .39, the 128 MiB benchmark
improved from about 9 to 62 MiB/s; a real WoW Classic install improved from
3.9 to 12.4 MB/s median. Full Scan and Repair was not run.

An alternating Retail download comparison on console .39 used public
`01.000.013` and the combined private build `desc-a5e142bb`: 12.4 to 21.2 MB/s,
with time inside writes falling from 7.6 to 5.5 seconds per 10-second window.
The second pair was network-bound at 8.2 to 8.6 MB/s (1.8 to 1.5 seconds inside
writes). These are combined-build workload measurements, not an isolated claim
for the buffer alone or a Scan and Repair qualification.

## Scan / IOCP diagnostic probe

`make -C ps5/probes/scan-bench LLVM_MINGW=/opt/llvm-mingw` builds a Windows
probe independent of Battle.net. Install the executable as the maintenance
launcher and also as `C:\Agent.exe`: the launcher starts the second copy as a
child, matching Agent's cross-process wineserver transport rather than
benchmarking the title parent's direct-call path. This diagnostic filename also
exercises the Agent-only cache admission rule; back up and restore that prefix
file along with both maintenance-launcher copies.
Standalone fixture cleanup removes only scratch/result data, never a restored
`C:\Agent.exe`; the swap transaction owns restoration/removal of that executable.

The child creates `C:\scan-bench\data.bin` once (1 GiB), then four workers read
256 MiB in 64 KiB overlapped chunks, dequeue IOCP packets (alternating
`GetQueuedCompletionStatus` / `GetQueuedCompletionStatusEx`), and hash all bytes
with BCrypt MD5. Each chunk reserves/commits/releases a 32 MiB scratch workspace,
matching the allocation churn measured in Agent's scan workers. Build with
`WORKSPACE=0` for the IOCP/hash-only control (16 passes over the 1 GiB file).
A separate thread runs `MsgWaitForMultipleObjects` / `PeekMessage`.
`C:\scan-bench-result.txt` records throughput, completed/pending read counts,
aggregate allocation / release / hash / IOCP-wait time and message-loop counters.
The probe checks `VirtualQuery` after free/reuse and fresh-commit zeroes, and
waits 11 seconds outside each end of the timed interval so the sampler can
publish complete baseline/final cumulative CPU counters. Compile a distinct
`OUT` with `WORKERS=2` to match Agent's two scan workers; the four-worker default
also stresses a bounded cache whose retained capacity may be smaller than all
simultaneously active workspaces.
Timed work excludes scratch creation. Keep the scratch file between A/B runs;
afterward run the probe with `--cleanup` or remove the scratch directory and
result over FTP, and restore both maintenance-launcher copies. A launcher swap
must temporarily update the matching SHA-256 in `share/forever/prefix-files.txt`
and restore it afterward, otherwise prefix-integrity checks reject the probe.

For diagnostics, copy `b1-product.json`, set `environment.WINEPS5_PROFILE="1"`
and build the maintenance role using that temporary `B1_PROFILE_CONFIG`. The
key is accepted for maintenance and GPU roles; graphics-only keys remain gated.
CPU callbacks are shared by both parents and PsmKit children without admitting
graphics functions into CPU-only roles. Use the sampler for CPU time:
`GetThreadTimes` is not implemented by the PS5 platform.
`runtime-b1-dual` selects its own maintenance product profile, so compose
separately built roles when using a diagnostic maintenance profile.

The sampler's `title request latency` lines are cumulative per-process,
per-request send-to-reply histograms (not the subsequent blocked select wait).
`server queue` measures client send-start to handler-entry for the explicitly
layout-checked select, event, IOCP and message-mask requests; it includes
transport send time. `server requests` separately records handler time.
These diagnostics require profiling and do not change the request wire layout.

The PS5 direct-backed commit path batches its temporary NX/no-access protection
over each contiguous run of newly mapped 64 KiB units. It never includes an
already committed neighbouring unit, and retains per-unit ownership, zeroing
and transaction rollback. `direct_commit_batch=off` in `C:\wineps5-tune.txt`
restores per-unit protection calls; `direct_commit_batch=on` re-enables batching
(default). The key is reread at most once per second and does not affect payload
Wine. The `test-direct-commit` host gate extracts the production transaction and
tune parser and covers batching, old neighbours, partial-unit commits, zeroing,
allocation/map/protect failures and the live kill switch.
On a failed batched commit, any unhidden new run is closed before rollback;
an unmap failure keeps only inaccessible pending units. If both protection and
unmap refuse to make retained new memory inaccessible, a single klog line and
native `pthread_kill(pthread_self(), SIGKILL)` fail-stop terminate the affected
process rather than returning an accessible uncommitted mapping. This bypasses
Wine's orderly exit, child/menu waits and server joins, even under the VM lock.

The whole-region cache is restricted to the actual loaded `Agent.exe` image.
Startup captures its bounded UTF-16 `ImagePathName`, not the command line or
`argv[0]`, before entering Windows code. Matching is exact and case-insensitive
on the final path component. Unknown images, `Battle.net.exe`, CEF subprocesses,
`AgentHelper.exe`, and WoW keep the existing allocator path even with
`direct_cache=on`; direct-commit protection batching is independent of this gate.

The private direct-region cache retains at most four whole, unsplit allocations,
charged by native bytes to a 64.125 MiB per-process bound (two nominal 32 MiB
workspaces including alignment slack). Only completely committed private RW
regions of 1–64 MiB qualify; guards, write-watch, executable/section mappings,
split ownership and partly decommitted regions stay on the existing path.
Freed cached regions are native `PROT_NONE`, absent from Wine's view tree and
logically `MEM_FREE`. An exact-size reuse obeys base/alignment/address limits,
reopens NX/RW, zeros the complete native extent, recloses native tail slack and
only then republishes committed Wine pages. Thus kernel I/O buffers remain
physically backed; reuse does not depend on a kernel `read`/`recv` causing a
recoverable userspace page fault. `MEM_RESET` remains on its in-place path and
does not bypass zeroing of a later fresh allocation.
`MEM_TOP_DOWN` requests always use the normal highest-free-address search,
not a selection limited to retained cache entries.

`direct_cache=off` in `C:\wineps5-tune.txt` disables reuse and drains retained
regions at the next direct-view reservation or whole-view release;
`direct_cache=on` enables it for `Agent.exe` only (default). Cache admission/reuse
stops and drains under direct-memory pressure
(less than a 256 MiB contiguous hole or a failed availability query). A failed
physical allocation reclaims the process's cache before one retry. Overlapping
different allocations evict retained ranges before mapping; failed reclamation
retains ownership metadata and never permits an overlapping mapping.
A failed cache reopen/tail-protection rollback first recloses the native
extent, then evicts it if closing fails. If remaining mapped units cannot be
closed after a failed eviction either, the same native SIGKILL fail-stop applies;
it never returns an accessible address that Wine reports as `MEM_FREE`.
The production-extracted `test-direct-fail-fast` uses the real host signal
consumer in a child holding a recursive VM mutex with a blocked client. Its
parent watchdog cannot be mistaken for the consumer's own SIGKILL. This is host
termination evidence, not an exercised PS5-console fatal-injection result.

`title vm cpu` reports cumulative, per-process **completed-operation CPU
nanoseconds** for current-process allocation (including `NtAllocateVirtualMemoryEx`),
free and cache zeroing, independently of sampled-PC/top-25 truncation.
`cache-zero` is a subset of `allocate`, not an additive category. Difference
`cpu_ns` and `now_ns` between records to obtain one-core CPU percentage; require
`cpu_calls == calls` before treating the CPU result as complete. The
`title direct cache` census separately reports hit/miss/eviction/retained bytes
and bulk-zero time. Zeroing is mandatory; an allocation win must not be
reported as eliminating the cost if bulk zeroing becomes the new bottleneck.
The `test-direct-cache` and `test-vm-cpu` host gates compile the production cache,
delete-view cutover and CPU counters, including real no-access and kernel-I/O
checks, bounds/pressure/kill-switch cases and failed reclaim/reopen ownership.


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

