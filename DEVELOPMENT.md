# Developing BuffrX

Start with the route your change needs. Run commands from the repository root.
The scripts work with Windows PowerShell 5.1; for Docs/Editor checks on Linux use
PowerShell 7 (`pwsh`) instead of `powershell`, or use the npm commands below.
For the native Linux port, use the separate [Linux guide](DEVELOPMENT_LINUX.md).

## Documentation

Edit the relevant Markdown file, then check local links and whitespace:

```powershell
powershell -File tools/verify.ps1 -Area Docs
```

No app, compiler, Node or OBS setup is needed.

## Editor

Install Node.js 24, then:

```powershell
npm ci --prefix ui
powershell -File tools/verify.ps1 -Area Editor
```

This runs TypeScript validation, tests and the production page build. To work on
the page, run `npm --prefix ui run dev` and open the local address Vite prints.
The browser uses mock data; it does not record your desktop. Real media/engine
integration needs the native app: the Windows route below or the experimental
[Linux port](DEVELOPMENT_LINUX.md). The native panel is ImGui, not this page.

On Linux you can run the same editor checks without PowerShell:

```bash
npm ci --prefix ui
npm --prefix ui run typecheck
npm --prefix ui test
npm --prefix ui run build
```

These are the editor commands on main. The Linux port branch's setup is documented
separately, including its currently pinned Node version and experimental status.

## Native logic

On 64-bit Windows 10 or later:

```powershell
powershell -File tools/bootstrap-toolchain.ps1 -Minimal
powershell -File tools/verify.ps1 -Area Native
```

The first command downloads pinned LLVM-MinGW, CMake and Ninja into `work/`.
The second builds into `build-contrib/` and runs the engine's existing
logic and regression checks. No Visual Studio, OBS runtime, Rust, GPU or
capture session is needed. Tests use an isolated temporary data directory.
Downloads are reused on later runs; no setup-time or download-size guarantee is made.

The 2,000-marker persistence/recovery checks always run. Their elapsed time is
reported in the native test log. To also enforce the five-second disk-write budget
on a dedicated performance-test machine, run:

```powershell
powershell -File tools/verify.ps1 -Area Native -Performance
```

Shared-runner CI checks correctness without enforcing that machine-dependent budget.
Running the test executable directly accepts `BUFFRX_TEST_PERFORMANCE=1` for the
same benchmark assertion. A functional test pass is not a performance result.

## Full Windows app

Use this route for capture, native rendering, Rust hosts, or real-media integration.
It needs more downloads than logic checks, Node.js 24, Rust, and WebView2 installed
on the machine. Close any development BuffrX instance using this checkout first.

```powershell
powershell -File tools/bootstrap-toolchain.ps1
powershell -File tools/bootstrap-rust.ps1
powershell -File tools/prepare-runtime.ps1
powershell -File tools/prepare-headers.ps1
npm ci --prefix ui
npm --prefix ui run build
powershell -File tools/build.ps1
```

Both Rust hosts use the repo-local gnullvm toolchain from `bootstrap-rust.ps1`.
Build the page before the shell so the shell embeds the current page. A native-only
rebuild can use `tools/build.ps1 -Target buffrx-engine` without rebuilding Rust.

Run with separate development state:

```powershell
$env:BUFFRX_DATA_ROOT = Join-Path $env:TEMP 'buffrx-development'
$env:BUFFRX_PIPE = 'buffrx-development'
runtime\bin\64bit\buffrx-engine.exe --show
```

Clear these two environment variables when finished, or close that PowerShell.
For a change that needs capture, keep the desktop changing during the check and
record the GPU/driver and capture mode you tested. Logic tests cannot verify a driver.
The existing `tools/preflight.ps1 -Quick` is a separate integration gate that
starts capture and exercises the app; run it only on a suitable Windows test setup.

## Checks and code map

`tools/verify.ps1` without `-Area` runs all software checks on Windows, including
full compilation. `-Area Build` runs just the page/full-app build after full setup.
PR CI uses the same commands, selecting areas from the changed paths. Full builds
cover Rust/native compilation; the PR gate does not verify hardware integration.

- `src/engine/`: native engine; `src/engine/ui/`: ImGui panel, Settings and quick panel.
- `tests/`: native regression checks; `ui/src/`: React editor and preview, with tests beside the code.
- `src-tauri/`: editor host; `src-share/`: sharing helper; `tools/`: build and test scripts.
- [Protocol](docs/protocol.md): request/event format and command reference.

The engine owns state; UI commands use the existing protocol and lock policy.
No telemetry/accounts/analytics, extra FFmpeg, or continuous hidden-window redraw.
Keep native/web themes and motion tokens consistent. Agent-assisted work can use
[CONTRIBUTOR_AGENT.md](CONTRIBUTOR_AGENT.md).

If a check reports missing dependencies, run the setup for that route. Rerun
`npm ci --prefix ui` after lockfile changes and bootstrap after toolchain pin changes.
Keep generated `work/`, `runtime/`, build directories and personal media out of PRs.
