# Forever Launcher

Run **Battle.net** on a jailbroken **PlayStation 5**.

Forever Launcher (`PPSA99030`) runs Battle.net and World of Warcraft through
Wine, using DirectX 12 on the PS5's own GPU. The default game is
**World of Warcraft Classic beta** (`WowB.exe` in `_classic_beta_`).
**World of Warcraft: Forever** final (`Wow.exe` in `_forever_`) is also
supported when selected in Battle.net. The supplied settings use 4K output
with HDR and frame generation.

> [!NOTE]
> This Battle.net launcher is designed for **World of Warcraft: Forever**. It
> might be used to install other games. A USB keyboard and mouse work in Battle.net and WoW.

> [!WARNING]
> **Alpha software.** Expect bugs. Forever Launcher is not made, approved or
> supported by Blizzard Entertainment, Sony Interactive Entertainment or Valve.
> Using unofficial software with an online game is at your own risk.

## Video

[![Watch Forever Launcher running on PS5](https://img.youtube.com/vi/hlHsBxf-G6Y/maxresdefault.jpg)](https://www.youtube.com/watch?v=hlHsBxf-G6Y)

## Changes in 01.000.013

- The write broker now repairs stale root-owned permissions in
  `/data/forever` and older install locations before it starts serving the
  launcher. Repair is limited to fixed launcher-data roots and does not follow
  symbolic links or change hard-linked files.

## Changes in 01.000.012

- Fixes WoW freezing on the loading screen about half the time at start (a
  startup crash): the app now keeps Wine's system DLLs in `/data/forever`
  identical to the ones it loads. The first start after updating copies these
  files once and takes about 2 minutes.
- Battle.net no longer asks to update installed games on every start: Wine now
  reports the folder permissions its Agent expects.
- WoW and Battle.net show their own mouse cursors, including WoW's gauntlet and
  Battle.net's text and link cursors, with the click point in the right place.
- On most Battle.net pages, the controller's D-pad moves between buttons and
  links, and Triangle presses.
- World of Warcraft Classic beta is the default game again; the final
  WoW: Forever channel still works when selected in Battle.net.
- Fixes the final channel's executable name (`_forever_/Wow.exe`), so its
  recommended settings install and direct start finds it.
- Faster parallel file writes: write-broker requests now use 16 connections
  instead of one shared by all Windows threads. PS5 benchmark: 32 KiB writes from
  4–8 threads improved from ~16 to ~30 MiB/s; single-thread writes are unchanged.
  Real Battle.net download/update speedups have not yet been measured.
- Installs recommended `Config.wtf` independently for final and beta without
  replacing user settings.
- Fixes WoW 1.60.1.70425 black screen/Error #132 before D3D12.
- Reports a generic Wine/Linux system identity instead of Sony/PlayStation 5
  branding while preserving real hardware capabilities.
- Release logs no longer include raw keyboard records.
- Fixes endless loading on Battle.net's password sign-in page.
- If the Battle.net installer's Live download fails, the launcher tries the
  direct versioned download link.
- Startup creates missing `Program Files` and `Program Files (x86)` folders
  without replacing existing user files.
- Window surfaces are published only when drawing is flushed, fixing the
  installer's crawling flicker.
- Windows sees the real free disk space through the write broker. An older or
  unavailable broker falls back to the previous fixed 16 GiB answer.
- The drawn purple desktop background is restored.

01.000.010 was not published: its build failed on a compiler warning in the new cursor code.

The `01.000.007` workflow failed before publication because of stale import-policy
entries. `01.000.008` removed only the obsolete `wineps5` `strlen`/`strstr`
policy rows; the remaining import checks stay enabled.

Native PS5 acceptance of this release is **pending**. Drive support remains
limited to `C:` and `W:`; `Z:` and UNC/network-share paths are not supported.

## What you need

- A **jailbroken PS5** that can run homebrew apps. Tested on firmware 5.50.
- **[ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus)**, in a version that lets
  apps use `/data`. It installs the app; your games and settings live in `/data`.
- About **55 GB of free space** on the console (WoW Classic is about 50 GB).
- A **Battle.net account** with access to WoW Classic, with an **authenticator** enabled
  (see [Signing in to Battle.net](#signing-in-to-battlenet)).
- A **DualSense** controller. A USB keyboard also works.
- An **FTP server** on the console, only to add your own `Lossless.dll` or
  change settings files by hand. FSR 3 frame generation needs neither.

## Install

Once listed in the homebrew catalog, **ProsperoStore** can install and update it.

**By hand:**

1. Download `PPSA99030.zip` from the [latest release](https://github.com/bizkut/ps5-forever/releases/latest).
2. Unzip it. Copy the `PPSA99030` folder to a location ShadowMountPlus scans,
   such as `/data/homebrew/PPSA99030`. The app folder can also be on an external drive;
   games and settings remain in `/data/forever`.
3. Let ShadowMountPlus register it, then start **Forever Launcher** from the home screen.

## First start

1. The first start sets up the Windows files. This takes a few minutes. The app
   restarts by itself when it is done.
2. Forever Launcher downloads Blizzard's Battle.net installer. Follow its
   on-screen prompts to install Battle.net.
3. Sign in to Battle.net (see below).
4. Battle.net opens on **World of Warcraft Classic beta** (`WowB.exe` in
   `_classic_beta_`) by default. Press **Install**, wait for the download (about 50 GB),
   then press **Play**. The final Forever game also works if you select it in Battle.net.

### Faster: copy WoW from a PC

If WoW is already installed on a PC, you can copy it to the console over your
home network instead of downloading it again. Battle.net then only checks the
files and downloads what is missing.

1. Start Forever Launcher once and install Battle.net (steps 1–3 above), then
   close the app.
2. On the PC, find your `World of Warcraft` folder. On Windows it is usually
   `C:\Program Files (x86)\World of Warcraft`. A Mac install
   (`/Applications/World of Warcraft`) works too; Battle.net replaces the Mac
   game files with the Windows ones.
3. With an FTP program, copy the whole folder, including the hidden files
   `.build.info` and `.product.db`, to the console as:

   ```text
   /data/forever/prefix/drive_c/Program Files (x86)/World of Warcraft
   ```

   Use a wired network if you can. Classic is about 50 GB; Retail is much larger.
4. Start Forever Launcher. In Battle.net, select the game. If it shows
   **Install**, use **Locate the game** below the button and choose
   `C:\Program Files (x86)\World of Warcraft`.
5. Battle.net checks the files and downloads only what is missing or newer.
   For a full check, use the gear next to **Play** → **Scan and Repair**.

Forever Launcher fixes the file permissions of copied folders at startup, so
you normally don't need to change them by hand. If it still says it can't
write to `/data/forever/prefix`, see [Troubleshooting](#troubleshooting).

## Signing in to Battle.net

- Sign in with your **email address and password**, and choose to get a **code by email**
  when Battle.net asks to confirm the sign-in.
- Your account needs an **authenticator** enabled.
- Signing in with **Google, Apple, Facebook, Xbox, PlayStation or any other
  third-party account is not supported.**
- Forever Launcher does not implement credential capture. Battle.net keeps its
  own sign-in token in your user data folder so you stay signed in.

## Controls

| Press | Does |
| --- | --- |
| **Options** | Opens or closes the Forever menu while Battle.net is open |
| **F12** on a USB keyboard | Opens or closes the Forever menu while Battle.net is open |
| **L2** | Escape / back in Battle.net |
| **L3 + R3** (press both sticks briefly) | Opens the on-screen keyboard |

The controller is turned on in WoW by default (WoW's own gamepad mode).

In Battle.net, the mouse cursor hides after three seconds without pointer
movement. Move the pointer or click to show it again. A new visible cursor
shape also restarts the timer; the game's or app's own hidden cursor stays hidden.

## Forever menu

Open the menu over Battle.net with **Options**, or press **F12** on a USB
keyboard. Use the D-pad and Cross; press Circle or Options at the top level to
close, or click with a mouse. The menu can:

- turn frame generation, HDR and the performance overlay on or off;
- browse, install, update and remove add-ons for Forever beta and final;
- install a curated starter pack;
- remove Battle.net while keeping installed games and their settings;
- remove and reinstall Battle.net from a fresh installer download.

Until you open the Forever menu once, each start shows an eight-second hint for
**Options** and **L2**.

Open **Add-ons** before pressing Play. L1/R1 switches between installed beta
(`_classic_beta_`) and final (`_forever_`); Left/Right switches Installed,
Updates, Find and Starter packs. Cross opens or installs, Square updates the
selected add-on, Triangle updates all, and R3 removes after a confirmation whose
default is Cancel. In Find, type on a USB keyboard or choose **Search…** for the
on-screen keyboard; **Add from GitHub link…** accepts `owner/repo` or its GitHub
URL. CurseForge-only rows say so and cannot be installed. GitHub, WoWInterface
and TukUI entries work without a key. Changes load the next time WoW starts, or
after typing `/reload` in WoW.

For Wago add-ons, choose **Wago key** on the Add-ons page. Get a free personal
key at [addons.wago.io/account/apikeys](https://addons.wago.io/account/apikeys),
then type it or choose **Read from file** after saving it as the first line of
`C:\Forever\Addons\wago-key.txt` (up to 256 characters). A successful import
deletes the file. The key is saved locally by AddonForge; **Remove key** clears
it. If Wago cannot be reached, it is saved as **set (not checked)**.

If frame generation currently uses `fg=auto`, selecting it turns it off;
selecting it again turns it on. Frame generation and the performance overlay
use their live settings files; HDR applies the next time WoW starts. After
removing Battle.net, close and start Forever Launcher to install it again.

## Frame generation

Frame generation shows extra, generated frames between the frames WoW draws,
so motion looks smoother at up to 120 Hz. It is **on by default** (2x) and picks
its method by itself:

- With **your own copy of `Lossless.dll`** from
  [Lossless Scaling](https://store.steampowered.com/app/993090/Lossless_Scaling/)
  (version 3.2.2.0, or a build with the same shaders), it uses Lossless Scaling's
  frame generation. The file is not included and never downloaded.
- Without it, it uses **AMD FSR 3** frame generation, which needs no extra file.

To use Lossless Scaling:

1. On your PC, find `Lossless.dll` in your Lossless Scaling folder
   (Steam: `steamapps/common/Lossless Scaling/`).
2. Copy it by FTP to the console as:

   ```text
   /data/forever/prefix/drive_c/Lossless.dll
   ```

3. Restart WoW.

Settings live in `/data/forever/prefix/drive_c/wineps5-fg.txt`. Changes apply
while the game runs, within a second.

```text
fg=on method=auto mult=2 flow=1.0
```

| Option | Values |
| --- | --- |
| `fg` | `on`, `off`, or `auto` (on only while WoW renders below 70 fps) |
| `mult` | `2`, `3` or `4` frames shown for every frame WoW draws |
| `flow` | `0.25` to `1.0`: lower is cheaper, `1.0` looks best (Lossless Scaling only) |
| `method` | `auto` (Lossless Scaling when `Lossless.dll` is there, else FSR 3), `lossless`, or `fsr3` |

The on-screen display settings are in `/data/forever/prefix/drive_c/wineps5-hud.txt`
(`hud=on` or `hud=off`, `pos=tl`, `scale=2`); the Forever menu changes `hud`
without replacing the other options.

## WoW settings

WoW starts with settings tuned for the PS5: 4K output at half render scale,
HDR on, and the controller enabled. The preset covers both Forever final
(`_forever_`) and Classic beta (`_classic_beta_`) independently: it is installed
only when that channel's executable is installed and its own `Config.wtf` is
absent. Change anything you like in WoW's options; neither channel's existing
settings are overwritten. The Forever menu changes only `HDROutput` for both
installed channels and leaves every other setting, including `HDRFactor`, alone.

## Where your data is

Everything that is yours is in **`/data/forever`**: Battle.net, WoW, your
settings, `Lossless.dll` and the shader cache. The app folder
(`/data/homebrew/PPSA99030`) holds only the app.

Updating the app replaces only the app folder. Your games and settings stay.

## Updating

- **ProsperoStore** shows updates and installs them.
- **By hand:** close the app and replace only its `PPSA99030` app folder
  with the folder from the new zip. Do not delete `/data/forever`.

The first start after an update synchronizes the app's Windows files into
`/data/forever`, including Wine's system DLLs. This keeps the on-disk DLLs
identical to the builtin images loaded by Wine; mismatched copies can crash
protected games during startup. Only changed, missing or wrong-size managed
files are copied. Existing native DLL replacements are preserved, while the
app's managed DXVK/vkd3d graphics files take precedence. The first sync after
updating to this version runs once and takes about 2 minutes.

## Uninstalling

Delete the app from the home screen (or delete `/data/homebrew/PPSA99030`), then
delete `/data/forever` to remove Battle.net, WoW and your settings.

## Privacy and support data

- Release logs retain aggregate input-health counters, not raw keyboard records,
  key usages or modifiers.
- Treat `/data/forever` as sensitive shared Wine-prefix storage. Never include
  registry hives, browser profiles, session/account files or saved credentials in
  support bundles, and review Wine/application logs and screenshots before sharing.

## Troubleshooting

- **"Forever Launcher can't write to ..." (permission denied)**: 01.000.013
  restarts the write broker once so it can repair the fixed launcher-data
  trees, waits for it to become ready, then retries the failed step once. If
  the message remains, close the app and, on a PC, use FTP to set
  `/data/forever` and everything in it to permission `777`; then start Forever
  Launcher again.
- **"Forever Launcher cannot use /data" (error 2 or 30)**: `/data` is missing
  or read-only. Update ShadowMountPlus to a version that lets apps use `/data`.
- **Battle.net sign-in fails**: use the email code and make sure an authenticator is
  enabled on your account. Third-party sign-in is not supported.
- **Battle.net reports only about 17 GB free**: Windows disk-space queries use the
  write broker's real storage counters for `/data/forever`. If the broker is
  unavailable or is from an older app version, they fall back to the title's
  synthetic 16 GiB free-space answer. The kernel log identifies the source with
  `wine: title disk space source=broker` or `source=synthetic`; restart the updated
  app so it can replace an older broker.
- **Frame generation does nothing**: check that `wineps5-fg.txt` says `fg=on`.
  Open the Forever menu from Battle.net to turn the performance overlay on; it
  shows `FG LOSSLESS` or `FG FSR3`. For Lossless Scaling, the file must be
  exactly `/data/forever/prefix/drive_c/Lossless.dll`.
- **Logs**: the console's kernel log (port 3232) shows what the app is doing.
  Lines starting with `forever:` describe setup.

## Building from source

The release is built by [GitHub Actions](.github/workflows/release.yml) from the
public sources listed in [`ps5/release/deps.json`](ps5/release/deps.json).
See [BUILDING.md](BUILDING.md).

## Credits

Forever Launcher is built on the work of many projects. Thank you to all of them.
Licences and details are in [NOTICE.md](NOTICE.md).

- [Proton](https://github.com/ValveSoftware/Proton) by Valve: the base this project starts from.
- [Wine](https://www.winehq.org/): runs Windows programs (LGPL-2.1-or-later).
- [vkd3d-proton](https://github.com/HansKristian-Work/vkd3d-proton) by Hans-Kristian Arntzen
  and contributors: DirectX 12 on Vulkan (upstream LGPL-2.1-or-later;
  this frame-generation build GPL-3.0-or-later).
- [DXVK](https://github.com/doitsujin/dxvk) by Philip Rebohle and contributors: DXGI (zlib).
- [Mesa](https://mesa3d.org/) and its RADV Vulkan driver (MIT).
- [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa), [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)
  and [PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK) by mihawk-99: RADV, Vulkan
  and the SDK on PS5.
- [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) by John Törnblom.
- [Eden](https://eden-emu.dev/) emulator: the frame-generation port this project adapts (GPL-3.0-or-later).
- [ProsperoEden](https://github.com/blackbearreloaded/ProsperoEden): frame generation on PS5.
- [ps5-framegen](https://github.com/swordpdf/ps5-framegen) by swordpdf: FSR 3 frame generation host
  (GPL-3.0-or-later), with AMD [FidelityFX](https://gpuopen.com/fidelityfx-super-resolution-3/) shaders (MIT).
- [lsfg-vk](https://github.com/PancakeTAS/lsfg-vk) by PancakeTAS: the Vulkan Lossless Scaling frame-generation
  code that Eden's port builds on (GPL-3.0-or-later).
- [Lossless Scaling](https://store.steampowered.com/app/993090/Lossless_Scaling/) by THS: frame-generation shaders,
  used only from your own copy.
- [FreeType](https://freetype.org/), [GnuTLS](https://www.gnutls.org/),
  [Nettle](https://www.lysator.liu.se/~nisse/nettle/), [libtasn1](https://www.gnu.org/software/libtasn1/)
  and the [pacbrew](https://github.com/OpenAGC/pacbrew-repo) recipes that build them.
- [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) by drakmor,
  [ProsperoStore](https://github.com/blackbearreloaded/ProsperoStore) and the
  [PS5 homebrew catalog](https://github.com/blackbearreloaded/ps5-homebrew-catalog).
- Noto and Droid fonts (OFL / Apache-2.0) and Mozilla's root certificates (MPL-2.0).
- Mihawk's earlier Forever Launcher: generated placeholder artwork and runtime icon extraction (MIT).
- [stb_image](https://github.com/nothings/stb) by Sean Barrett and contributors: PNG decoding (MIT or public domain).

## Licence

Forever Launcher is licensed under the [GNU General Public License v3.0 or later](LICENSE).
Third-party components keep their own licences; see [NOTICE.md](NOTICE.md).

World of Warcraft, Battle.net and Blizzard are trademarks of Blizzard Entertainment, Inc.
PlayStation and PS5 are trademarks of Sony Interactive Entertainment Inc. All other
trademarks belong to their owners.
