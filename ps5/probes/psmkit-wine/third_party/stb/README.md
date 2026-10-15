`stb_image.h` v2.30 from https://github.com/nothings/stb at commit
`2c980bb59875b0d32144a71867fbdebb2f77cd20`.
The upstream file SHA-256 is
`594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3`.

Local change: thread-local function declarations are omitted when
`STBI_NO_THREAD_LOCALS` disables their implementations. This makes the static,
PNG-only build valid under GCC without declaring unavailable functions.
The patched file SHA-256 is
`a1971cd0bc3555cb345ad3a7cba562a6f59f517eaab853eab1887394cbcd24c5`.

Public domain or MIT, at the user's choice; the licence is at the end of the file.
`title-icon.h` uses its PNG decoder to read the icon resource of the installed
`Battle.net Launcher.exe`, with GIF and thread-local support disabled.
