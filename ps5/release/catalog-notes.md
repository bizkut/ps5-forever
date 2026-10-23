# Homebrew catalog texts

## Record description (`apps/PPSA99030.json`, `description`)

Battle.net launcher for World of Warcraft (Retail, Classic and Forever) with a built-in add-on manager; press Options for the Forever menu and L3+R3 for the on-screen keyboard.

## Release notes (GitHub release description, shown by the catalog)

### Forever Launcher 01.000.019

**Repairs Battle.net signature setup and completes large add-on installs.**

- Incomplete Wine Trust and PE signature-provider registration is repaired
  through normal Wine setup before Battle.net starts. Verification now fails
  closed if no provider is registered.
- Continuously progressing add-on archives may download for up to 15 minutes,
  with a 30-second idle timeout. Errors identify body-transfer versus JSON
  failures.
- Polestar's full package includes all 24 folders, including the requested
  Brazilian Portuguese and database/navigation modules; Recorder remains a
  separate optional package.
- A bounded native worker hashes, extracts, synchronizes and activates add-ons
  outside Wine. Interrupted install and removal cleanup retains durable recovery
  proof instead of restoring partial folders.
- Existing 01.000.018 catalogue caches migrate automatically to the corrected
  package list.

### Forever Launcher 01.000.018

**Fixes missing multi-pack add-ons in Find.**

- Repositories that publish several independent add-ons now keep every release stream instead of showing only the first one.
- Spoken Books, Quests and Zones audio packs appear as separate Find entries, including **Spoken Zones Audio (All)** and **Spoken Zones Audio (Portuguese)**.
- Includes complete GitHub release pagination from 01.000.017, so older release streams install without silently selecting the wrong release.
- 01.000.017 was superseded before its homebrew-catalog submission after native testing found the same-repository listing bug.

### Forever Launcher 01.000.017

**More Spoken audio packs and complete GitHub add-on release discovery.**

- Find now includes **Spoken Zones Audio (All)** and **Spoken Zones Audio (Portuguese)** for English and Brazilian Portuguese place narration.
- Add-ons can now install from release streams older than a GitHub repository's first 100 releases. Forever follows every release page until it finds the requested stream.
- GitHub API, connection and malformed-response errors remain visible instead of silently installing the wrong release.

### Forever Launcher 01.000.016

**Faster Battle.net installs and Blizzard game downloads.**

- Reinstalling Battle.net took about 3 minutes instead of 12 on a test console: the update agent's many small writes are now combined into large ones, including its overlapped writes, which were never combined before.
- Downloads of every Blizzard game now combine their small writes too, not only WoW's. A StarCraft II download that spent 6–9 s of every 10 s waiting on the disk now spends about 3 s, so the connection is the limit.
- **Exit Battle.net** at the bottom of the Forever menu closes Battle.net properly and returns to the PS5 home screen, after a confirmation (Cancel is the default).
- **Invert mouse scroll** in the Forever menu reverses the USB mouse wheel. It is on by default; touchpad and stick scrolling are unchanged.
- Fixes a rare crash during Battle.net start-up when one of its processes ended at an unlucky moment.
- Battle.net's installer now continues automatically past its fixed install-location page, and its download follows web redirects itself, so a fresh install needs no controller input at that step.
- If Battle.net is reinstalled, Classic Era and Anniversary may need **Update** or **Scan and Repair** in Battle.net once before they start again.

**Add-ons: our own list, faster installs, instant removal.**

- The add-on list is now Forever's own, refreshed daily, with 408 Forever entries. Find and Starter packs include the Polestar, Lore Forever and Spoken packs, and each pack can be updated by itself.
- Large add-on installs are much faster: Lore Forever installs in about 2-3 minutes instead of more than 10.
- Installs and updates show progress text (download percentage and unpacked files).
- Removing an add-on is instant. The leftover files are deleted in the background once the Add-ons page is closed, and reopening the page pauses that cleanup.

### Forever Launcher 01.000.015

**New: Voice-to-Text for WoW chat.** Hold the touchpad and speak; your words are typed into WoW's chat box as you talk.

- Turn it on in the Forever menu (**Voice-to-Text: On**). The first time, it downloads a 190 MB speech model (Whisper small, multilingual) and checks it before use. It is off by default.
- In WoW, hold the touchpad still for about half a second. The controller buzzes, a red-dot **Listening** panel appears in the middle of the screen, and WoW's chat box opens on your last chat channel. Release when you finish speaking.
- The text waits in the chat box: **Cross** sends it, **Circle** cancels, **Square** opens the on-screen keyboard to correct it, and **L1/R1** switch the channel through /s, /p, /i, /g, /raid and /r.
- Speech is recognised on the PS5 itself, on the GPU, using whisper.cpp. Nothing is uploaded, and audio is never saved. The microphone is only open while you hold the touchpad.
- The DualSense **Mute** button still mutes the microphone: a muted microphone types nothing.
- Touchpad clicks and drags work as before. With Voice-to-Text on, a touchpad press held still for half a second starts dictation instead of holding the mouse button.
- Uses about 1 GB of graphics memory once you first dictate, until WoW closes. Tested in WoW on one console with English speech; other languages are recognised by the same model but have not been tested.
- **Proximity Voice Chat** is listed in the Forever menu as coming later.

Other changes:

- Add-ons Find: results now filter as you type with the on-screen keyboard, not only after you close it.
- Downloads follow web redirects themselves: the PS5's built-in redirect handling returned empty files. This fixes the speech-model download. The Battle.net installer uses the same handling but has not been retested on a console.

### Forever Launcher 01.000.014

- Detects installed WoW channels without a launch-version list, including Retail, Classic, Classic Era, Anniversary, Forever, beta, PTR and future channel names.
- A one-shot `wine/direct-game` file can select a channel folder such as `_retail_`; an unavailable selection never starts another version.
- Missing-only Config.wtf presets and the HDR switch apply to every detected channel.
- Add-ons lists installed channels and selects source flavours from the product and the client's own version resource. Installs, update records and removals stay in the selected channel. Unknown products are not supported for add-ons.
- Add-ons Find and Starter packs now cover every supported version: Retail, Classic (MoP), Classic Era, Anniversary and Forever. Find uses the community GitHub add-on catalogue (GPL-3.0), WoWInterface and Tukui. The GitHub list refreshes at most once a day and has a built-in copy for offline use.
- Pressing Play on Classic, Classic Era and Anniversary works again: Battle.net's `WoWClassic.exe` spelling now matches the files on disk.
- After a Play, the app remembers that version's safe start options (no sign-in data) for later direct starts.
- Fixes Battle.net crashing while the Add-ons page was open: Battle.net's own windows now get priority over the add-on manager.
- The start-up hint "OPTIONS Forever menu" shows on every start for 10 seconds.
- The mouse cursor hides after 3 seconds without movement on the Battle.net screen too.
- Faster Battle.net downloads and Scan and Repair: game data writes are buffered across several files and positions, and Battle.net's update agent reuses memory instead of preparing new memory each time. On one console a Retail download ran 21 MB/s against 12 MB/s on 01.000.013 when the disk was the limit.
- Automatic discovery is limited to the default WoW install root. Custom C:/W: paths can launch from Battle.net Play.
- Add-ons needs a recognized game version; an unknown or future version can launch but may not have a compatible catalogue. Installing from Wago with a real key, Update all and Reinstall/Uninstall Battle.net are not yet console-tested.
- Console checks on a private build: Retail, Classic (MoP), Classic Era, Anniversary and the Forever beta all start from Battle.net Play.

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
