# Builds the portable BuffrX package from the staged runtime and ui/dist.
#
#   powershell -File tools/package.ps1 [-Version 0.5.0] [-OutDir dist]
#
# The package is not shippable until the smoke test passes *from the staged folder*: a package whose
# engine cannot start is a broken package, and testing it where it was built proves nothing.
#
# Output: <OutDir>\BuffrX-<version>\ with BuffrX.exe (the entry) at the root, the engine and the editor
# in bin\64bit beside the pinned runtime, ui\dist for the editor, licenses\, a per-file SHA-256 manifest
# and SHA256SUMS.txt.

param(
  [string]$Version = '0.5.0',
  [string]$OutDir = 'dist'
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$runtime = Join-Path $root 'runtime'
$stage = Join-Path $OutDir "BuffrX-$Version"
$stageFull = Join-Path $root $stage

# A failed build leaves an older executable behind, which would package and pass. Insist on the pieces
# the build just produced.
$required = @(
  'runtime\bin\64bit\buffrx-engine.exe',
  'runtime\bin\64bit\buffrx-launcher.exe',
  'runtime\bin\64bit\buffrx-ui.exe',
  'runtime\bin\64bit\buffrx-share.exe',
  'runtime\licenses\buffrx-share-THIRD-PARTY.txt',
  'runtime\licenses\buffrx-editor-THIRD-PARTY.txt',
  'runtime\licenses\buffrx-page-THIRD-PARTY.txt',
  'runtime\licenses\OBS-GPL-2.0.txt',
  'runtime\licenses\FFmpeg-LGPL-2.1.txt',
  'runtime\bin\64bit\WebView2Loader.dll',
  'runtime\bin\64bit\obs.dll',
  'ui\dist\index.html'
)
# A failed link leaves the previous executable in runtime\bin\64bit, so Test-Path alone cannot tell this
# build's output from the last one's: the timestamps can. Each piece a source produces is compared with
# the newest file under the sources that produce it. Files the pinned OBS release supplies (obs.dll,
# WebView2Loader.dll, the licences) are copied by its own scripts, not built here, so they are only
# checked for presence.
$producers = @{
  'runtime\bin\64bit\buffrx-engine.exe'   = @('src\engine', 'src\shared', 'CMakeLists.txt')
  'runtime\bin\64bit\buffrx-launcher.exe' = @('src\launcher', 'CMakeLists.txt')
  'runtime\bin\64bit\buffrx-share.exe'    = @('src-share\src', 'src-share\Cargo.toml', 'src-share\Cargo.lock')
  'runtime\bin\64bit\buffrx-ui.exe'       = @('src-tauri\src', 'src-tauri\Cargo.toml', 'src-tauri\Cargo.lock',
                                             'ui\src', 'ui\index.html', 'ui\timeline.html', 'ui\package.json',
                                             'ui\package-lock.json', 'ui\vite.config.ts')
  'ui\dist\index.html'                    = @('ui\src', 'ui\index.html', 'ui\timeline.html', 'ui\package.json',
                                             'ui\package-lock.json', 'ui\vite.config.ts')
}
foreach ($item in $required) {
  $path = Join-Path $root $item
  if (-not (Test-Path -LiteralPath $path)) {
    throw "missing $item - build first (tools/build.ps1) and build the interface (npm run build in ui/)"
  }
  if (-not $producers.ContainsKey($item)) { continue }
  $newest = $null
  foreach ($source in $producers[$item]) {
    $sourcePath = Join-Path $root $source
    if (-not (Test-Path -LiteralPath $sourcePath)) { continue }
    if (Test-Path -LiteralPath $sourcePath -PathType Container) {
      $sourceFile = Get-ChildItem -LiteralPath $sourcePath -Recurse -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    } else {
      $sourceFile = Get-Item -LiteralPath $sourcePath
    }
    if ($sourceFile -and (-not $newest -or $sourceFile.LastWriteTime -gt $newest.LastWriteTime)) { $newest = $sourceFile }
  }
  if ($newest -and ((Get-Item -LiteralPath $path).LastWriteTime -lt $newest.LastWriteTime)) {
    throw "$item is older than $($newest.FullName) - this build did not produce it, so packaging would ship the previous binary"
  }
}

if (Test-Path -LiteralPath $stageFull) { Remove-Item -LiteralPath $stageFull -Recurse -Force }
New-Item -ItemType Directory -Force $stageFull | Out-Null

Write-Host "staging into $stage"
foreach ($folder in @('bin\64bit', 'obs-plugins\64bit', 'data', 'licenses')) {
  $from = Join-Path $runtime $folder
  if (-not (Test-Path -LiteralPath $from)) { continue }
  $to = Join-Path $stageFull $folder
  New-Item -ItemType Directory -Force (Split-Path -Parent $to) | Out-Null
  Copy-Item -LiteralPath $from -Destination $to -Recurse -Force
}
# Measurement and test tools are not part of the product.
Get-ChildItem -LiteralPath (Join-Path $stageFull 'bin\64bit') -Filter 'buffrx-*-loop.exe' -ErrorAction SilentlyContinue |
  Remove-Item -Force
Remove-Item -LiteralPath (Join-Path $stageFull 'bin\64bit\buffrx-tests.exe') -Force -ErrorAction SilentlyContinue
# The runtime folder also holds every earlier dev engine and diagnostic tool. Only the current binaries
# ship: an old engine is ~55 MB each, and those leftovers were most of a 1.3 GB package.
$devBinaries = @(
  '^buffrx-engine-.+\.exe$',            # a3, e1, dev, fix, night, ring
  '^buffrx-engine\.(old|prev.*|previous-.+)$',
  '^buffrx-engine\.exe\.(old|prev.*|previous-.+)$',
  '^buffrx-share\.exe\.old$',
  '^buffrx-fuzz\.exe$',
  '^buffrx-present-loop\.exe$',
  '^ring_save_tests\.exe$',
  '^buffrx-ui-.+\.exe$'                  # baseline, p4g and every other ui build beside the product one
  '^buffrx-admin-probe.*\.exe$',         # measurement probes, never the product
  '^buffrx-webhook-probe.*\.exe$',
  '^buffrx-.*-probe.*\.exe$'
)
Get-ChildItem -LiteralPath (Join-Path $stageFull 'bin\64bit') -File |
  Where-Object { $name = $_.Name; $devBinaries | Where-Object { $name -match $_ } } |
  Remove-Item -Force
# obs-nvenc-test.exe, obs-qsv-test.exe and obs-amf-test.exe are runtime helpers the encoders launch,
# not test tools: removing them left the package on x264 only.

Copy-Item -LiteralPath (Join-Path $root 'ui\dist') -Destination (Join-Path $stageFull 'ui\dist') -Recurse -Force

# BuffrX's own licence, which the runtime folder never carries. The installer shows this file and the
# installed copy keeps it beside the third-party notices.
New-Item -ItemType Directory -Force (Join-Path $stageFull 'licenses') | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination (Join-Path $stageFull 'licenses\BuffrX-GPL-2.0.txt') -Force

# The entry point: BuffrX.exe at the root starts the engine, which draws its own panel.
Copy-Item -LiteralPath (Join-Path $stageFull 'bin\64bit\buffrx-launcher.exe') -Destination (Join-Path $stageFull 'BuffrX.exe') -Force
Remove-Item -LiteralPath (Join-Path $stageFull 'bin\64bit\buffrx-launcher.exe') -Force

# Local builds keep their debug information (crash dumps symbolise); the package does not carry it.
$paths = Get-Content -LiteralPath (Join-Path $root 'work\paths.json') -Raw | ConvertFrom-Json
# No executable packing: a packed binary looks like malware to every engine, and it makes crash reports
# useless. Inno's LZMA compression of the files inside the installer is normal and not this.
$objdump = Join-Path $paths.llvm_bin 'llvm-objdump.exe'
$shipped = @((Join-Path $stageFull 'BuffrX.exe'))
$shipped += Get-ChildItem -LiteralPath (Join-Path $stageFull 'bin\64bit') -Filter '*.exe' -File | Select-Object -ExpandProperty FullName
$packed = @()
foreach ($exe in $shipped) {
  $sections = & $objdump -h $exe | Out-String
  if ($sections -match 'UPX0|UPX1|MPRESS|ASPack|PECompact|\.petite') { $packed += (Split-Path $exe -Leaf) }
}
if ($packed.Count -gt 0) {
  throw "packed executables are not allowed in a release: $($packed -join ', ')"
}
# The file properties a person sees in Explorer must name the product and the version being packaged.
foreach ($binary in @('BuffrX.exe', 'bin\64bit\buffrx-engine.exe')) {
  $info = (Get-Item -LiteralPath (Join-Path $stageFull $binary)).VersionInfo
  if ($info.ProductName -ne 'BuffrX' -or $info.CompanyName -ne 'BuffrX' -or
      ($info.ProductVersion -ne $Version -and $info.ProductVersion -ne "$Version.0")) {
    throw "$binary has no BuffrX version metadata for $Version (Product=$($info.ProductName) Company=$($info.CompanyName) Version=$($info.ProductVersion))"
  }
}
$strip = Join-Path $paths.llvm_bin 'llvm-strip.exe'
foreach ($binary in @('bin\64bit\buffrx-engine.exe', 'bin\64bit\buffrx-ui.exe', 'bin\64bit\buffrx-share.exe', 'BuffrX.exe')) {
  & $strip --strip-debug (Join-Path $stageFull $binary)
  if ($LASTEXITCODE -ne 0) { throw "llvm-strip failed on $binary" }
}
# BuffrX.exe runs from the root, away from the MinGW runtime in bin\64bit: it may import Windows only.
$imports = & (Join-Path $paths.llvm_bin 'llvm-objdump.exe') -p (Join-Path $stageFull 'BuffrX.exe') | Select-String 'DLL Name'
if ($imports -match 'libc\+\+|libunwind|libwinpthread') { throw "BuffrX.exe imports the MinGW runtime and would not start from the package root" }

# Per-file hashes, and the same list in the format every checksum tool reads.
$files = Get-ChildItem -LiteralPath $stageFull -Recurse -File | Sort-Object FullName
$manifest = [ordered]@{
  product = 'BuffrX'
  version = $Version
  generated_utc = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
  files = @()
}
$sums = New-Object System.Collections.Generic.List[string]
foreach ($file in $files) {
  if ($file.Name -in @('manifest.json', 'SHA256SUMS.txt')) { continue }
  $relative = $file.FullName.Substring($stageFull.Length + 1).Replace('\', '/')
  $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
  $manifest.files += [ordered]@{ path = $relative; bytes = $file.Length; sha256 = $hash }
  $sums.Add("$hash  $relative")
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $stageFull 'manifest.json') -Encoding utf8
$sums | Set-Content -LiteralPath (Join-Path $stageFull 'SHA256SUMS.txt') -Encoding ascii

# The package must start where it stands, with its own data directory, and say so.
$dataRoot = Join-Path $stageFull 'data-root'
New-Item -ItemType Directory -Force $dataRoot | Out-Null
$previousDataRoot = $env:BUFFRX_DATA_ROOT
$env:BUFFRX_DATA_ROOT = $dataRoot
try {
  # The engine's own log goes to stderr; only the machine-readable line on stdout matters here, and the
  # output is captured through files so a log line can never be mistaken for a PowerShell error.
  $outFile = Join-Path $stageFull 'smoke.out'
  $errFile = Join-Path $stageFull 'smoke.err'
  $smokeProcess = Start-Process -FilePath (Join-Path $stageFull 'bin\64bit\buffrx-engine.exe') `
    -ArgumentList '--package-smoke-test' -WorkingDirectory $stageFull -PassThru -Wait -RedirectStandardOutput $outFile -RedirectStandardError $errFile
  $smoke = (Get-Content -LiteralPath $outFile -Raw -ErrorAction SilentlyContinue)
  Remove-Item -LiteralPath $outFile, $errFile -Force -ErrorAction SilentlyContinue
} finally {
  # The caller's session must not be left pointing at a directory this script deletes.
  $env:BUFFRX_DATA_ROOT = $previousDataRoot
  Remove-Item -LiteralPath $dataRoot -Recurse -Force -ErrorAction SilentlyContinue
}
if ($smokeProcess.ExitCode -ne 0 -or $smoke -notmatch 'BUFFRX_PACKAGE_SMOKE_OK') {
  throw "the package smoke test failed from ${stage} (exit $($smokeProcess.ExitCode)): $smoke"
}
# The folder name and manifest say $Version; the engine inside says its own. A release bump that
# touches one and not the other packages a release nobody can identify, so name the disagreement here.
if ($smoke -match 'version=(\S+)' -and $Matches[1] -ne $Version) {
  throw "the engine reports version $($Matches[1]) but this package is named $Version"
}

$total = (Get-ChildItem -LiteralPath $stageFull -Recurse -File | Measure-Object Length -Sum)
Write-Host ("PACKAGE_OK {0} files {1:N1} MB" -f $total.Count, ($total.Sum / 1MB))
Write-Host ("  $stage\manifest.json")
Write-Host ("  $stage\SHA256SUMS.txt")
Write-Host ("  $($smoke.Trim())")

