# Contributing on Linux

Linux contributions are welcome: editor improvements, native port work, packaging,
audio/capture support, and testing on more distributions and hardware.

## Choose your starting point

- **Editor or docs on main:** follow [DEVELOPMENT.md](DEVELOPMENT.md). Editor checks
  use Node.js 24 and npm; they do not require a Windows build or a native Linux port.
- **Native Linux app:** build on [PR #7](https://github.com/bulhaa1/BuffrX/pull/7),
  currently open, from `seblov/BuffrX`, branch `linux-native-appimage`. Use a separate
  checkout and contribute from your own fork. Check the current PR status and source
  branch before starting so you can build on existing work.

The native instructions below describe PR revision
`19aa8ca1b1418c5fcce5354f4f101e8cdd7a7736`, inspected on 2026-11-14.
The port is not yet merged into main. If those scripts are absent, you are on a
different branch; follow that branch's README rather than trying Windows scripts.
Update this guide when the port lands or its setup changes.

## Build the native port

The PR reports a working build on Ubuntu 24.04, X11, Intel Iris Xe. Other distros,
drivers and desktop sessions are useful contributions, but are not verified here.
Review the commands and install these system prerequisites on your test machine:

```bash
sudo apt install build-essential cmake pkg-config libgtk-3-dev libx11-dev libxrandr-dev \
  libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev libavfilter-dev \
  libcurl4-openssl-dev libssl-dev libwebkit2gtk-4.1-0
```

From the port checkout's root, run each command only after the previous succeeds:

```bash
bash tools/bootstrap-linux.sh
export RUSTUP_HOME="$PWD/work/rustup" CARGO_HOME="$PWD/work/cargo"
export PATH="$PWD/work/cargo/bin:$PWD/work/toolchain-linux/node-v22.14.0-linux-x64/bin:$PATH"
cmake -S linux -B build-linux
cmake --build build-linux --parallel 2
(cd ui && npm_config_script_shell=/bin/bash npm install --package-lock=false && npm run build)
(cd src-tauri && PKG_CONFIG_PATH="$PWD/../work/linux-ui/pkgconfig" \
  LIBRARY_PATH="$PWD/../work/linux-ui/lib" cargo build --release)
bash tools/package-linux.sh
```

The expected package is `dist/BuffrX-0.5.0-x86_64.AppImage` for this revision.
Run it with `./dist/BuffrX-0.5.0-x86_64.AppImage --show` on a graphical desktop.
The editor needs GTK 3, WebKitGTK 4.1 and H.264-capable GStreamer on the host.

This recipe follows the PR's existing Node 22.14 bootstrap and page-install command.
Main's contributor tooling uses Node 24 and `npm ci`; these are different branch
setups. Avoid incidental lockfile or toolchain changes while fixing an unrelated
port issue. Check the branch's bootstrap before changing the version or paths above.

## Check your contribution

Rebuild the affected engine/editor target. For editor changes, also run these
from the port checkout after setup:

```bash
npm --prefix ui run typecheck
npm --prefix ui test
npm --prefix ui run build
```

Fix any failures or describe them accurately in the PR. These instructions were
checked against the PR's source and reported evidence; they have not been executed
on Linux by the author of this guide. The Windows `tools/verify.ps1 -Area Native`
and `-Area Build` commands do not validate this port.

For app changes, include your distro, X11/Wayland session, GPU/driver when relevant,
reproduction steps and actual results. Capture work should exercise the affected
source and play back a saved clip; audio work should check the affected tracks;
editor work should check the affected playback/export flow. Note checks that need
hardware you do not have. Never include private recordings or tokens in a report.

## Where help is useful

The PR uses `linux/CMakeLists.txt`, Linux platform layers in `src/linux/`, shared
engine logic in `src/engine/`, and Unix-specific Tauri host code in `src-tauri/`.
Keep platform changes behind the existing interfaces and preserve Windows behavior
when editing shared code. Read [CONTRIBUTOR_AGENT.md](CONTRIBUTOR_AGENT.md) for agent work.

The current port uses X11 capture; running through XWayland does not provide native
Wayland capture. Sharing/Discord, controller hotkeys and update installation still
have gaps or stubs. Useful work includes those features, Wayland support, hardware
encoder/audio testing, packaging portability, and addressing the PR's unauthenticated
loopback media service and fixed temporary muxer path. Check the current implementation
before choosing a task, since this list describes the linked revision.
