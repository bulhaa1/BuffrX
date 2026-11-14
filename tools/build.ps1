# Builds BuffrX with the pinned toolchain. One entry point for humans and CI:
#   powershell -File tools/build.ps1            # configure + build everything staged
#   powershell -File tools/build.ps1 -Target buffrx-engine
#
# The MinGW C++ runtime DLLs are copied next to the executables because the Windows loader needs them
# before any of our code runs. The package layout keeps the engine in bin/64bit beside them, exactly
# like the launcher expects.

param(
  [string]$Target = '',
  [switch]$ConfigureOnly,
  [switch]$NoStage,
  # Automatic download and install are on by default; pass -AutoUpdate:$false to build check-and-notify only.
  [switch]$AutoUpdate = $true
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$pathsFile = Join-Path $root 'work\paths.json'
if (-not (Test-Path $pathsFile)) { throw 'work/paths.json missing: run tools/bootstrap-toolchain.ps1, tools/prepare-runtime.ps1 and tools/prepare-headers.ps1' }
$paths = Get-Content -LiteralPath $pathsFile -Raw | ConvertFrom-Json
$tools = Get-Content -LiteralPath (Join-Path $root 'work\toolchain.json') -Raw | ConvertFrom-Json
if (-not $tools.imgui_root) { throw 'work/toolchain.json has no imgui_root: run tools/bootstrap-toolchain.ps1' }

function Invoke-Step([string]$label, [scriptblock]$action) {
  Write-Host "== $label"
  & $action
  if ($LASTEXITCODE -ne 0 -and $null -ne $LASTEXITCODE) { throw "$label failed with exit code $LASTEXITCODE" }
}

function Copy-IfChanged([string]$source, [string]$target) {
  # A development process may have an unchanged runtime DLL loaded. Leave it in place.
  if ((Test-Path -LiteralPath $target) -and
      (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -eq (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash) { return }
  Copy-Item -LiteralPath $source -Destination $target -Force
}

Invoke-Step 'configure' {
  & $paths.cmake -S $root -B (Join-Path $root 'build') -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$($paths.ninja)" `
    "-DCMAKE_C_COMPILER=$($paths.clang)" `
    "-DCMAKE_CXX_COMPILER=$($paths.clangxx)" `
    "-DCMAKE_RC_COMPILER=$(($paths.llvm_bin -replace '\\', '/') + '/llvm-windres.exe')" `
    -DCMAKE_SYSTEM_NAME=Windows `
    -DCMAKE_BUILD_TYPE=Release `
    "-DBUFFRX_RUNTIME=$($paths.runtime_root)" `
    "-DBUFFRX_WEBVIEW2_INCLUDE=$($paths.webview2_include)" `
    "-DBUFFRX_LLVM_RC=$($paths.llvm_bin)\llvm-rc.exe" `
    "-DBUFFRX_LLVM_INCLUDE=$(Split-Path $paths.llvm_bin)\include" `
    "-DBUFFRX_IMGUI_ROOT=$($tools.imgui_root -replace '\\', '/')" `
    "-DBUFFRX_AUTO_UPDATE=$(if ($AutoUpdate) { '1' } else { '0' })"
}

# The engine's interface faces live in the runtime's data tree (packaged whole), their licences beside
# OBS's. Staged on every build because prepare-runtime.ps1 rebuilds runtime/ from scratch.
$fontDir = Join-Path $root 'runtime\data\buffrx\fonts'
New-Item -ItemType Directory -Force $fontDir | Out-Null
# The faces of the earlier interface leave the runtime, so a package never ships them.
foreach ($old in @('Michroma-Regular.ttf', 'Inter-Regular.ttf', 'Inter-SemiBold.ttf')) {
  Remove-Item -LiteralPath (Join-Path $fontDir $old) -Force -ErrorAction SilentlyContinue
}
# The NL ("no ligatures") cut: ImGui draws no ligatures, and these files are a quarter smaller.
foreach ($face in @('JetBrainsMonoNL-Regular.ttf', 'JetBrainsMonoNL-Medium.ttf', 'JetBrainsMonoNL-Bold.ttf', 'JetBrainsMonoNL-ExtraBold.ttf')) {
  Copy-Item -LiteralPath (Join-Path $tools.jetbrains_mono_root "fonts\ttf\$face") -Destination (Join-Path $fontDir $face) -Force
}
$licenseDir = Join-Path $root 'runtime\licenses'
New-Item -ItemType Directory -Force $licenseDir | Out-Null
foreach ($old in @('Michroma-OFL.txt', 'Inter-OFL.txt')) {
  Remove-Item -LiteralPath (Join-Path $licenseDir $old) -Force -ErrorAction SilentlyContinue
}
Copy-Item -LiteralPath (Join-Path $tools.jetbrains_mono_root 'OFL.txt') -Destination (Join-Path $licenseDir 'JetBrainsMono-OFL.txt') -Force
Copy-Item -LiteralPath (Join-Path $tools.imgui_root 'LICENSE.txt') -Destination (Join-Path $licenseDir 'DearImGui-MIT.txt') -Force
# The engine links FFmpeg (avformat, avcodec, avutil, swscale, swresample, avfilter) out of the OBS release,
# and that release carries no FFmpeg licence text of its own, so the LGPL notice is written here.
$obsVersion = (Get-Content -LiteralPath (Join-Path $root 'runtime\pin.json') -Raw | ConvertFrom-Json).version
$ffmpegNotice = @(
  'FFmpeg licence notice',
  '',
  "BuffrX links the FFmpeg libraries that ship with OBS Studio ${obsVersion}: avformat, avcodec,",
  'avutil, swscale, swresample and avfilter. FFmpeg is free software, licensed under the',
  'GNU Lesser General Public License, version 2.1 or later.',
  '',
  '  Project: https://ffmpeg.org',
  '  Licence: https://ffmpeg.org/legal.html',
  '           LGPL v2.1: https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html',
  '  Source:  https://ffmpeg.org/download.html - the FFmpeg version built into OBS Studio',
  "           ${obsVersion}, named in that release notices",
  '',
  'BuffrX does not modify FFmpeg. The libraries are loaded at run time from the DLLs beside the',
  'executables (dynamic linking), so they can be replaced with a modified, compatible build',
  'without relinking BuffrX; this is how the relink right of the LGPL is met.',
  '',
  'External libraries that the FFmpeg build links carry their own licences: libx264 is',
  'GPL-2.0-or-later, librist BSD-2-Clause, and srt MPL-2.0.'
)
Set-Content -LiteralPath (Join-Path $licenseDir 'FFmpeg-LGPL-2.1.txt') -Value $ffmpegNotice -Encoding ascii
Write-Host "interface fonts staged"

if ($ConfigureOnly) { return }

if ($Target -notin @('buffrx-ui', 'buffrx-share')) {
  $buildArgs = @('--build', (Join-Path $root 'build'))
  if ($Target) { $buildArgs += @('--target', $Target) }
  Invoke-Step 'build' { & $paths.cmake @buildArgs }
}

# Build the Tauri shell (Rust). It replaces the old C++ WebView2 host; the binary keeps the
# same name so the package layout and the launcher keep working.
if (-not $Target -or $Target -in @('buffrx-ui', 'buffrx-share')) {
$cargoHome = Join-Path $root 'work\cargo'
$rustupHome = Join-Path $root 'work\rustup'
$mingwBin = $paths.llvm_bin
$env:RUSTUP_HOME = $rustupHome
$env:CARGO_HOME = $cargoHome
$rustManifest = Join-Path $root 'work\rust-toolchain.json'
if (-not (Test-Path $rustManifest) -or -not (Test-Path (Join-Path $cargoHome 'bin\cargo.exe'))) { throw 'Run: powershell -File tools/bootstrap-rust.ps1' }
$rust = Get-Content -LiteralPath $rustManifest -Raw | ConvertFrom-Json
$env:RUSTUP_TOOLCHAIN = $rust.toolchain
$compiler = Join-Path $rustupHome "toolchains\$($rust.toolchain)\bin\rustc.exe"
if (-not (Test-Path $compiler)) { throw 'Rust toolchain missing. Run: powershell -File tools/bootstrap-rust.ps1' }
$compilerVersion = & $compiler --version
if ($LASTEXITCODE -ne 0 -or -not $compilerVersion.StartsWith("rustc $($rust.version) ")) { throw 'Rust version changed. Run: powershell -File tools/bootstrap-rust.ps1' }
$env:BUFFRX_LLVM_RC = Join-Path $paths.llvm_bin 'llvm-rc.exe'
$env:RC = Join-Path $paths.llvm_bin 'llvm-rc.exe'
$env:PATH = (Join-Path $cargoHome 'bin') + ';' + $mingwBin + ';' + $env:PATH
# Panic locations carry the path a crate was compiled from. Without this, published binaries name the
# build machine's folder; map the repository (and the Cargo registry inside it) to ".".
$env:RUSTFLAGS = (@($env:RUSTFLAGS, "--remap-path-prefix=$root=.") -join ' ').Trim()
# The page is embedded into the shell when main.rs is compiled, and cargo does not notice that ui/dist changed: a shell built
# after a page change carried the old page (the viewer ran the editor's bundle and never got to its first frame). Touching
# main.rs makes cargo embed what is in ui/dist now.
if (-not $Target -or $Target -eq 'buffrx-ui') {
if (-not (Test-Path (Join-Path $root 'ui\dist\index.html'))) { throw 'Build the editor page first: npm ci --prefix ui, then npm --prefix ui run build' }
$shellExe = Join-Path $root 'src-tauri\target\release\buffrx-ui.exe'
$pageFiles = Get-ChildItem (Join-Path $root 'ui\dist') -Recurse -File -ErrorAction SilentlyContinue
if (-not (Test-Path $shellExe) -or ($pageFiles | Where-Object { $_.LastWriteTime -gt (Get-Item $shellExe).LastWriteTime })) {
  (Get-Item (Join-Path $root 'src-tauri\src\main.rs')).LastWriteTime = Get-Date
}
Invoke-Step 'cargo build (tauri shell)' {
  Push-Location (Join-Path $root 'src-tauri')
  try { & (Join-Path $cargoHome 'bin\cargo.exe') build --release --locked }
  finally { Pop-Location }
}
# Stage the rust binary into build/ so the normal staging loop picks it up.
Copy-Item -LiteralPath (Join-Path $root 'src-tauri\target\release\buffrx-ui.exe') -Destination (Join-Path $root 'build\buffrx-ui.exe') -Force
}

# The sharing helper (BuffrX links, src-share): iroh over QUIC, started by the engine only while a link
# is live or a clip is arriving. Its crates are pinned by src-share/Cargo.lock.
if (-not $Target -or $Target -eq 'buffrx-share') {
Invoke-Step 'cargo build (sharing helper)' {
  Push-Location (Join-Path $root 'src-share')
  try { & (Join-Path $cargoHome 'bin\cargo.exe') build --release --locked }
  finally { Pop-Location }
}
Copy-Item -LiteralPath (Join-Path $root 'src-share\target\release\buffrx-share.exe') -Destination (Join-Path $root 'build\buffrx-share.exe') -Force
}
# Notices for the helper's crates (MIT, Apache and BSD require them to ship), and the GPL text the OBS release keeps
# under data (it has no COPYING at its root, so prepare-runtime.ps1 never staged one).
if (-not $Target -or $Target -eq 'buffrx-share') {
& (Join-Path $PSScriptRoot 'rust-licenses.ps1') -CargoHome $cargoHome
}
if (-not $Target -or $Target -eq 'buffrx-ui') {
& (Join-Path $PSScriptRoot 'rust-licenses.ps1') -CargoHome $cargoHome -Manifest 'src-tauri' `
  -Out (Join-Path $root 'runtime\licenses\buffrx-editor-THIRD-PARTY.txt') `
  -Title 'BuffrX editor (buffrx-ui.exe): third-party Rust crates'
& node (Join-Path $PSScriptRoot 'ui-licenses.mjs')
}
}
Copy-Item -LiteralPath (Join-Path $root 'work\obs\official\data\obs-studio\license\gplv2.txt') -Destination (Join-Path $root 'runtime\licenses\OBS-GPL-2.0.txt') -Force

# Ship the C++ runtime beside the binaries: both the build directory (for local runs) and the
# staging directory the packager uses.
$mingwBin = Join-Path $paths.llvm_bin '..\x86_64-w64-mingw32\bin'
$runtimeDlls = Get-ChildItem -LiteralPath (Resolve-Path $mingwBin).Path -Filter '*.dll' -ErrorAction SilentlyContinue |
  Where-Object { $_.Name -match '^(libc\+\+|libunwind|libwinpthread)\.dll$' }
# The Rust shell is linked against the gnullvm target and needs libunwind at runtime; the MinGW
# runtime DLLs above already cover it.
$destinations = @((Join-Path $root 'build'))
if (-not $NoStage) { $destinations += (Join-Path $root 'runtime\bin\64bit') }
foreach ($dll in $runtimeDlls) {
  foreach ($destination in $destinations) {
    Copy-IfChanged $dll.FullName (Join-Path $destination $dll.Name)
  }
}
Write-Host ("runtime dlls staged: " + (($runtimeDlls | ForEach-Object { $_.Name }) -join ', '))

# WebView2Loader.dll is a load-time import of buffrx-ui.exe (verified with llvm-objdump -p). Without it
# beside the executable the Windows loader cannot start the shell's window, so it is staged next to the
# binaries and the packager refuses a package that does not carry it.
if (-not (Test-Path -LiteralPath $paths.webview2_loader)) {
  throw "WebView2 loader missing: $($paths.webview2_loader)"
}
foreach ($destination in $destinations) {
  Copy-IfChanged $paths.webview2_loader (Join-Path $destination 'WebView2Loader.dll')
}
Write-Host "staged: WebView2Loader.dll"

# OBS resolves encoder capability helpers beside the engine executable, even when its DLLs come from
# runtime/. Keep development builds usable on NVIDIA, Intel and AMD as well as packaged builds.
foreach ($helper in @('obs-nvenc-test.exe', 'obs-qsv-test.exe', 'obs-amf-test.exe')) {
  $source = Join-Path $root "runtime\bin\64bit\$helper"
  if (Test-Path -LiteralPath $source) {
    Copy-Item -LiteralPath $source -Destination (Join-Path $root "build\$helper") -Force
  }
}

# Stage the executables into the packaged layout (bin/64bit beside obs.dll) so local runs and the
# smoke tests exercise the same directory structure users get. Measurement and test tools stay in build/.
$devTools = @('buffrx-present-loop.exe', 'buffrx-tests.exe', 'buffrx-fuzz.exe',
  'buffrx-admin-probe.exe', 'ring_save_tests.exe')
if (-not $NoStage) {
  $stage = Join-Path $root 'runtime\bin\64bit'
  New-Item -ItemType Directory -Force $stage | Out-Null
  foreach ($exe in Get-ChildItem -LiteralPath (Join-Path $root 'build') -Filter '*.exe' -ErrorAction SilentlyContinue |
             Where-Object { $devTools -notcontains $_.Name -and (-not $Target -or $_.BaseName -eq $Target) }) {
    Copy-Item -LiteralPath $exe.FullName -Destination $stage -Force
  }
  Write-Host "executables staged to runtime\bin\64bit"
}

$exes = Get-ChildItem -LiteralPath (Join-Path $root 'build') -Filter '*.exe' -ErrorAction SilentlyContinue |
  Sort-Object Name
foreach ($exe in $exes) { Write-Host ("  {0,10:N2} MB  {1}" -f ($exe.Length / 1MB), $exe.Name) }



