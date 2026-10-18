# Homebrew catalog texts

## Record description (`apps/PPSA99030.json`, `description`)

Battle.net launcher for World of Warcraft. Options opens the Forever menu; L3+R3 opens the on-screen keyboard. USB keyboard and mouse work too.

## Release notes (GitHub release description, shown by the catalog)

### Forever Launcher 01.000.014 (in development)

- Detects installed WoW channels without a launch-version list, including Retail, Classic, Classic Era, Anniversary, Forever, beta, PTR and future channel names.
- A one-shot `wine/direct-game` file can select a channel folder such as `_retail_`; an unavailable selection never starts another version.
- Missing-only Config.wtf presets and the HDR switch apply to every detected channel.
- Add-ons lists installed channels and selects source flavours from the product and the client's own version resource. Installs, update records and removals stay in the selected channel. Unknown products are not supported for add-ons.
- The curated catalogue and starter packs currently contain Forever data only; other flavours can use GitHub links and known installed sources.
- Automatic discovery is limited to the default WoW install root. Custom C:/W: paths can launch from Battle.net Play.
- Private-build console observations: Retail renders via the direct-game selector at 90 fps; an unavailable PTR selection refuses without fallback. Classic Era crashes after display initialization and remains under investigation. Anniversary, Classic progression, installed PTR/beta channels and per-channel Add-ons still need new console qualification; no general online-compatibility claim.

### Forever Launcher 01.000.013

- Options opens the Forever menu over Battle.net; F12 opens it from a USB keyboard. The menu controls frame generation, HDR, the performance overlay and Add-ons, and can Reinstall or Uninstall Battle.net.
- L2 is now Back in Battle.net. The old in-WoW overlay hold has been removed.
- Add-ons (AddonForge) has Find search by name or description, GitHub link installation and a personal Wago API key page. Installing from Wago with a real key is not yet tested.
- A short start-up hint shows the Options menu and L2 Back controls until the Forever menu has been opened once.
- The recommended `Config.wtf` now uses `RenderFormat 1`.
- Start-up repairs stale root-owned permissions under `/data/forever` without following symbolic links or changing hard-linked files.
- All prefix DLLs are kept identical to the app's Wine build.
- Faster Battle.net downloads with write buffering: measured about 3.9 → 12 MB/s on one console.
- Reinstall/Uninstall Battle.net has not been tested on a console yet.

### Forever Launcher 01.000.012

- Fixes WoW freezing on the loading screen about half the time at start (a startup crash): the app now keeps Wine's system DLLs in `/data/forever` identical to the ones it loads. The first start after updating copies these files once and takes about 2 minutes.
- Battle.net no longer asks to update installed games on every start: Wine now reports the folder permissions its Agent expects.
- WoW and Battle.net show their own mouse cursors, including WoW's gauntlet and Battle.net's text and link cursors, with the click point in the right place.
- On most Battle.net pages, the controller's D-pad moves between buttons and links, and Triangle presses.
- World of Warcraft Classic beta is the default game again; the final WoW: Forever channel still works when selected in Battle.net.
- Fixes the final channel's executable name (`_forever_/Wow.exe`), so its recommended settings install and direct start finds it.
- Faster parallel file writes: write-broker requests now use 16 connections instead of one shared by all Windows threads. PS5 benchmark: 32 KiB writes from 4–8 threads improved from ~16 to ~30 MiB/s; single-thread writes are unchanged.
  Real Battle.net download/update speedups have not yet been measured.
- Installs recommended `Config.wtf` independently for final and beta without replacing user settings.
- Fixes WoW 1.60.1.70425 black screen/Error #132 before D3D12.
- Reports a generic Wine/Linux system identity instead of Sony/PlayStation 5 branding while preserving real hardware capabilities.
- Release logs no longer include raw keyboard records.
- Fixes endless loading on Battle.net's password sign-in page.
- If the Battle.net installer download fails, the launcher retries from Blizzard's direct download link.
- Missing `Program Files` and `Program Files (x86)` folders are created at start, so the installer accepts its default location.
- The installer no longer flickers.
- Windows programs see the console's real free disk space.
- The purple desktop background is back.
- Drives: `C:` and `W:` only; `Z:` and network paths are not supported.

01.000.010 was not published: its build failed on a compiler warning in the new cursor code.

The `01.000.007` workflow failed before publication because of stale import-policy entries. `01.000.008` removed only the obsolete `wineps5` `strlen`/`strstr` policy rows; the remaining import checks stay enabled.

### About Forever Launcher

- This Battle.net launcher is designed for World of Warcraft: Forever.
- It might be used to install other games. A USB keyboard and mouse work in Battle.net and WoW.
- Your games and settings live in `/data/forever`; updating the app keeps them.
- Frame generation is on by default: AMD FSR 3, or Lossless Scaling when you add your own `Lossless.dll` at `/data/forever/prefix/drive_c/Lossless.dll`.
- Sign in to Battle.net with your email and a code sent by email; your account needs an authenticator. Third-party sign-in is not supported.
