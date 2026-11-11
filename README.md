# BuffrX

A Windows replay-buffer tool for gamers: press a key, keep the last N seconds. Built on libobs
linked directly into a small native engine — no Electron, no Node, no Streamlabs bindings, no
second copy of FFmpeg.

## Status: alpha, one process

`BuffrX.exe` starts `buffrx-engine.exe`, which is the whole app: libobs 33.0.1 in-process (capture,
audio, encoders, the replay ring, recording, the Timeline), the tray, the hotkeys, sharing, and the
panel itself, drawn with Dear ImGui. The editor, the clip preview and the Alt+4 Timeline preview are a
separate WebView2 process that starts when you open one and is gone when you close it. See
`docs/issues-consolidated-2026-11-05.md` for what is verified and what is not.

## Five pillars

1. **Lightweight** — one engine executable (debug symbols stripped at package time) plus the pinned
   OBS runtime (DLLs only). No Chromium while no editor is open, no Node, no duplicated FFmpeg.
   Opening the panel costs about 20 MB; closing it gives that back (measured, ISSUES.md item 25).
2. **Un-intrusive** — nothing drawn while no window is open (the interface's graphics device is
   released 10 s after it closes), no input hook unless the engine is running hotkeys, no listening
   socket except the sharing helper while a BuffrX link is live.
3. **Private** — no accounts, no telemetry, no crash reporting. BuffrX makes an outbound request only
   when the user presses a button for it: a manual update check (this build has none), Share's "Post
   to Discord", which sends the one clip to the webhook the user set, and a BuffrX link, which runs
   `buffrx-share.exe` only while that link lives or a clip arrives. "Copy for Discord" uses no
   network at all: the clip is made to fit and pasted by hand.
4. **Simple** — one config file, one protocol, one process.
5. **Powerful** — the control surface is the engine's own, not a binding's subset: codec and vendor
   selection, quality tiers that map to real encoder properties, duration-first bitrate budgeting,
   per-track audio, and a watchdog that recovers a stopped buffer without asking the user.

## Build

```powershell
powershell -File tools/bootstrap-toolchain.ps1   # pinned LLVM-MinGW, CMake, Ninja, Windows SDK, WebView2, Dear ImGui, fonts
powershell -File tools/prepare-runtime.ps1       # pinned OBS release -> runtime/ (DLLs, modules, data, import lib)
powershell -File tools/prepare-headers.ps1       # libobs public headers from the pinned sources
powershell -File tools/build.ps1                 # configure + build + stage into runtime/bin/64bit
```

Everything downloads once and is verified by SHA-256 on every later run; nothing is fetched at
runtime and nothing is installed system-wide.

The editor's page is built separately, then hosted by the editor process:

```powershell
cd ui; npm install; npm run build; cd ..          # produces ui/dist, which buffrx-ui.exe serves
powershell -File tools/package.ps1                # portable package (BuffrX.exe at the root) + manifest + smoke test
```

## Run

```powershell
$env:BUFFRX_DATA_ROOT = "$env:TEMP\buffrx-test"     # keep test state out of the real profile
runtime\bin\64bit\buffrx-engine.exe --package-smoke-test   # start libobs, load modules, exit cleanly
runtime\bin\64bit\buffrx-engine.exe --video-probe          # name the graphics resource that fails, if any
runtime\bin\64bit\buffrx-engine.exe --capture-smoke-test   # capture ~6 s, save a clip, verify it
runtime\bin\64bit\buffrx-engine.exe --benchmark-seconds 60 # settled CPU, memory, frames, lag
runtime\bin\64bit\buffrx-engine.exe --show                 # run for real: tray, hotkeys, protocol, the panel open
runtime\bin\64bit\buffrx-ui.exe --editor clip.mkv          # the editor window; files can be dropped on it
powershell -File tools\engine-ask.ps1 status.get           # one protocol command to a running engine
build\buffrx-tests.exe                                     # unit checks: 2236, 0 failures
powershell -File tools\acceptance.ps1                      # every protocol command against a live engine
```

The panel (deck, library, Settings), the quick panel and the tray menu are drawn by the engine in
`src/engine/ui` with Dear ImGui; only one window is open at a time. The editor, the clip preview and
the Timeline preview live in `ui/` (Vite + React + React Aria + Tailwind) inside the Tauri process; in
a browser that page runs on a mock, or on the live engine through `node tools/dev-bridge.mjs`
(http://127.0.0.1:8787/, `?view=preview`). The Timeline preview's own page is served from the same
bundle and proven over the DevTools protocol (`tools/timeline-cdp-probe.ps1`).

Clips are filed per game: `Videos\BuffrX\<Game>\Replay <date time>.mp4`, `Display Capture\` when no
game is captured, `Recordings\`, `Screenshots\` and `Edits\` for editor exports.

## Layout

```
tools/                  pinned bootstrap, build and probe scripts
src/engine/             the libobs host: config, capture, encoders, replay, Timeline, library, protocol, tray
src/engine/ui/          the deck, library, Settings, quick panel and tray menu (Dear ImGui, in-process)
src/launcher/           BuffrX.exe: checks the package (VC++ runtime), starts the engine with its panel
src/share/              buffrx-share.exe (Rust, iroh): the BuffrX link, alive only while one is
src/shared/             JSON, logging, length-prefixed named-pipe transport
src-tauri/              the editor process (Tauri): pipe bridge, media/thumbnail schemes, preview
ui/                     the editor, clip-preview and Timeline-preview page (React)
site/                   the landing page and the /l/ link page (GitHub Pages)
runtime/                pinned OBS runtime + generated import libraries (rebuilt by the scripts)
docs/protocol.md        the engine protocol every client speaks
```

## License

BuffrX is free software under the GNU General Public License, version 2 or later. See [LICENSE](LICENSE).

It links libobs from [OBS Studio 33.0.1](https://github.com/obsproject/obs-studio/tree/33.0.1), also
GPL-2.0-or-later. Third-party notices ship in `licenses/` with every release.
