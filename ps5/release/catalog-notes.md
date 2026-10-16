# Homebrew catalog texts

## Record description (`apps/PPSA99030.json`, `description`)

Battle.net launcher designed for World of Warcraft: Forever; other games may install, but keyboard support is in development.

## Release notes (GitHub release description, shown by the catalog)

### Forever Launcher 01.000.008

- Supports the final World of Warcraft: Forever channel (`wowf`, `_forever_/WoW.exe`) by default while retaining Classic beta support.
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

The `01.000.007` workflow failed before publication because of stale import-policy entries. This release removes only the obsolete `wineps5` `strlen`/`strstr` policy rows; the remaining import checks stay enabled.

### About Forever Launcher

- This Battle.net launcher is designed for World of Warcraft: Forever.
- It might be used to install other games, but keyboard support is in development.
- Your games and settings live in `/data/forever`; updating the app keeps them.
- Frame generation is on by default: AMD FSR 3, or Lossless Scaling when you add your own `Lossless.dll` at `/data/forever/prefix/drive_c/Lossless.dll`.
- Sign in to Battle.net with your email and a code sent by email; your account needs an authenticator. Third-party sign-in is not supported.
