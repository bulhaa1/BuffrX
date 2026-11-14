# Working on a BuffrX contribution

Use this file when a contributor asks you to implement a change. Their task defines
the outcome; this guide explains how to work on BuffrX. Features, experiments and
platform work are welcome. Read [CONTRIBUTING.md](CONTRIBUTING.md), then follow this
sequence. Use the current checkout and scripts as the source of truth when a guide
describes an older revision; explain any mismatch instead of guessing commands.

1. **Establish scope.** Check the repository root, branch, host OS and Git status.
   Preserve existing tracked and untracked work. State the requested outcome and
   what observable behavior will prove it works. Use the user's chosen base, or
   the current checkout when none is specified; do not switch branches or reset
   existing work to make setup easier. Find the relevant code and its tests before
   editing. If a `.codegraph/` index exists, use CodeGraph first; otherwise use
   ordinary search. CodeGraph is optional and must not be installed just for a contribution.
2. **Choose a route.** Read only the relevant setup in [DEVELOPMENT.md](DEVELOPMENT.md).
   Docs need no app build; editor checks use Node; Windows logic checks use the
   minimal toolchain. For Linux tasks, follow the Linux section below.
   Features, experiments and platform work are welcome when they are the user's
   requested task. Make routine implementation decisions and continue within that
   scope; clarify only when the desired outcome is ambiguous or needs expanding.
3. **Make the change.** Reuse the existing code and match nearby conventions.
   For bugs, reproduce the reported behavior or add a failing regression check
   before the fix. If reproduction needs unavailable hardware, state that limitation
   and identify what the available tests can prove. For new engine behavior, write
   the failing check first. Keep the diff focused and update affected docs. Avoid
   unrelated cleanup, dependency upgrades and lockfile churn.
4. **Verify.** Run the commands for the chosen platform in its development guide.
   Choose checks using the routes below. Capture, rendering, audio
   devices and driver behavior also need relevant full-app testing. Report
   missing hardware or prerequisites explicitly; never claim an unrun check passed.
   Separate failures present before your change from failures caused by it, using
   relevant baseline results where available. Keep assertions intact; fix the
   cause of a failure rather than disabling a check to get a passing result.
5. **Hand back.** Review the diff and report what changed, commands/results,
   and remaining limitations using the handoff below. Prepare a short PR description.
   Keep edits local unless the user authorizes committing or publishing. A request
   to edit/test alone does not authorize commits, pushes, PRs, GitHub comments/issues
   or repository settings changes. Honor permissions already given without asking again.

## Choose checks

- **Docs only:** the Docs route in [DEVELOPMENT.md](DEVELOPMENT.md); no app build.
- **Editor page (`ui/`):** the Editor route, including typecheck, tests and page build.
- **Windows engine/panel (`src/engine/`, `tests/`):** Native logic checks. Production
  native changes also need the full Windows compilation route; tests-only builds
  do not compile the complete engine. Rust host/helper changes need full compilation too.
- **Native Linux:** the build and relevant checks in [DEVELOPMENT_LINUX.md](DEVELOPMENT_LINUX.md).
  Its editor changes also need editor checks. Windows test executables are not Linux proof.
- **Shared contracts, build scripts or changes spanning areas:** check every affected
  route. A protocol change must keep the command registry, lock policy, public
  reference and affected clients consistent. A shared platform change needs both
  platforms checked, or an explicit note that the other platform remains unverified.

Use isolated development state for runtime checks. Stop only processes you started
for the task; preserve the contributor's running app, recordings and personal media.
If a prerequisite or permission is missing, report the exact blocker and continue
independent work. Do not substitute mocks for real capture/driver verification.

## Handoff and completion

Finish with a short report:

- **Result:** requested behavior implemented, with the relevant files.
- **Checks:** exact commands and their results (passed, failed, or not run).
- **Limits:** remaining work, baseline failures, or missing platform/hardware coverage.
- **Review:** a brief PR description and any tradeoff the maintainer should assess.

Call the task complete only when the requested outcome and applicable checks are
satisfied. Otherwise describe what is finished and what remains. Never report a
contribution as merged, published or fully hardware-verified without that evidence.

## Linux contributions

Read [DEVELOPMENT_LINUX.md](DEVELOPMENT_LINUX.md) before Linux setup or native changes.
Check the branch and current PR status first: the Linux guide records the port's
source branch and revision. If `linux/CMakeLists.txt` and `tools/bootstrap-linux.sh`
are absent, confirm the intended port base before setting up a separate checkout.
If GitHub is unavailable, report that PR status could not be refreshed; use the
available source without assuming an old PR is still open or merged.

Reuse the port's `src/linux/` layers and existing platform interfaces. Inspect the
actual branch before applying Windows assumptions about paths, IPC, capture,
audio or editor media delivery. Preserve Windows behavior in shared code, and keep
the project's privacy and idle-cost requirements when extending platform support.
Use that branch's documented toolchain; do not run Windows bootstrap scripts or
claim the Windows `-Area Native` gate checks Linux. Report the distro, session
(X11/Wayland), relevant GPU/driver, build/check commands and observed results.
Separate working features, stubs and untested hardware; missing Linux hardware
does not prevent a source contribution, but its verification limits must be clear.

## Architecture and product requirements

These describe the existing product. An architecture experiment should name the
intended change and its tradeoffs; do not re-pick the stack as incidental cleanup.

- `src/engine/` owns capture, config, audio, replay, recording and protocol.
  `src/engine/ui/` draws the native panel with Dear ImGui.
- `ui/` is the React editor/preview; `src-tauri/` is its on-demand native host
  (WebView2 on Windows, WebKitGTK in the Linux port).
  `src-share/` is the short-lived Rust sharing helper. Keep those ownership boundaries.
- BuffrX links libobs directly. Keep one native engine process and the existing
  FFmpeg runtime. Hidden UI must remain idle; stopped buffering must do no GPU work.
- Preserve privacy: no telemetry, accounts or analytics. Keep networking within
  existing update and explicit-sharing behavior.
- Engine/client changes follow [docs/protocol.md](docs/protocol.md) and the command
  registry/lock policy. Keep native and web theme tokens and motion tokens aligned.
- Keep the existing uppercase UI rendering and slider-commit-on-release behavior.
  Use the existing UI libraries; Radix and Base UI are excluded by project design.
- Publish only task-relevant source and public docs. Keep machine paths, personal
  data, credentials, generated output and private agent instructions out of the diff.
