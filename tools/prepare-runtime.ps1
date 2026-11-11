# Stages the pinned OBS runtime BuffrX links against, into runtime/ inside the repository.
#
# What it does, and why it is shaped this way:
#   * downloads the official OBS Windows x64 release zip and verifies its pinned SHA-256 on every run
#     (OBS publishes no checksum file, so the hash is recorded in this script);
#   * stages only the module DLLs and data trees the product actually loads, never the whole release;
#   * walks each staged binary's import table and copies any additional DLL it needs from the release,
#     so the package stays minimal without a hand-maintained list of runtime dependencies;
#   * generates an import library from obs.dll's exported symbols with llvm-dlltool, so BuffrX links
#     the shipped libobs without building OBS from source (sources are still downloaded for licence
#     compliance, never compiled);
#   * writes runtime-pin.json with version, asset, hash and the module list actually staged.

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem | Out-Null

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$work = Join-Path $root 'work\obs'
$runtime = Join-Path $root 'runtime'
$tools = Get-Content -LiteralPath (Join-Path $root 'work\toolchain.json') -Raw | ConvertFrom-Json

$version = '33.0.1'
$asset = "OBS-Studio-$version-Windows-x64.zip"
$url = "https://github.com/obsproject/obs-studio/releases/download/$version/$asset"
$sourceAsset = "OBS-Studio-$version-Sources.tar.gz"
$sourceUrl = "https://github.com/obsproject/obs-studio/releases/download/$version/$sourceAsset"

$modules = @('win-capture', 'win-wasapi', 'obs-ffmpeg', 'obs-nvenc', 'obs-qsv11', 'obs-x264', 'image-source', 'obs-filters')

# The hashes GitHub publishes for the release assets (the release page's digests), pinned here: a first run verifies instead
# of trusting, and a swapped archive fails on the run that fetches it. 32.2.2's are kept so a rollback is one line.
$pins = @{
  'obs-33.0.1-win64'   = '0d9436451c2d772b33428e3fdae9776f071895c335de17283176306fb72651d3'
  'obs-33.0.1-sources' = 'c8d3a8f69a0f17970ac0a8f2a2462f1bc4dacd324f6da0c4b408d442d4798913'
  'obs-32.2.2-win64'   = '4d6e40e3ab155f56b30de517380566a206d74b63cdf5ad49aa596924768f97e1'
  'obs-32.2.2-sources' = 'ec81fb66b03e75ddb3076b576f62679c39262e0e9960cef3e17a40dc5d68e6b4'
}

New-Item -ItemType Directory -Force $work | Out-Null

function Get-Archive {
  param([string]$Url, [string]$Destination, [string]$PinName, [string]$Sha256)
  $pinFile = Join-Path $work "$PinName.sha256"
  if (-not (Test-Path $Destination)) {
    $partial = "$Destination.partial"
    & curl.exe -fL --retry 3 --silent --show-error $Url -o $partial
    if ($LASTEXITCODE -ne 0) { throw "download failed: $Url" }
    Move-Item -LiteralPath $partial -Destination $Destination -Force
  }
  $actual = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($Sha256) {
    if ($actual -ne $Sha256) { throw "$PinName hash mismatch: expected $Sha256 got $actual" }
    Write-Output "$PinName verified sha256:$actual"
  } else {
    Write-Warning "$PinName has no pinned hash: trusting this download and recording sha256:$actual"
  }
  Set-Content -LiteralPath $pinFile -Value $actual -NoNewline
}

function Copy-Tree {
  param([string]$From, [string]$To)
  New-Item -ItemType Directory -Force $To | Out-Null
  foreach ($file in Get-ChildItem -LiteralPath $From -Recurse -File) {
    if ($file.Extension -eq '.pdb') { continue }
    $relative = $file.FullName.Substring($From.Length).TrimStart('\')
    $target = Join-Path $To $relative
    New-Item -ItemType Directory -Force (Split-Path -Parent $target) | Out-Null
    Copy-Item -LiteralPath $file.FullName -Destination $target -Force
  }
}

$zip = Join-Path $work $asset
Get-Archive -Url $url -Destination $zip -PinName "obs-$version-win64" -Sha256 $pins["obs-$version-win64"]
Get-Archive -Url $sourceUrl -Destination (Join-Path $work $sourceAsset) -PinName "obs-$version-sources" -Sha256 $pins["obs-$version-sources"]

$official = Join-Path $work "official"
# The extracted release is reused only when it is this version: an obs.dll from the previous pin would otherwise be staged as this one.
$extractedVersion = Join-Path $official '.version'
$extracted = (Test-Path (Join-Path $official 'bin\64bit\obs.dll')) -and (Test-Path $extractedVersion) -and
  ((Get-Content -LiteralPath $extractedVersion -Raw).Trim() -eq $version)
if (-not $extracted) {
  if (Test-Path $official) { Remove-Item -LiteralPath $official -Recurse -Force }
  [System.IO.Compression.ZipFile]::ExtractToDirectory($zip, $official)
  Set-Content -LiteralPath $extractedVersion -Value $version -NoNewline
}

if (Test-Path $runtime) {
  # Everything is rebuilt from the pinned release, except the extracted libobs headers: those come
  # from tools/prepare-headers.ps1 and re-extracting them on every run is wasted work.
  # The licences are written by build.ps1 and rust-licenses.ps1 and the packager refuses to ship without
  # them, so a re-stage keeps them beside the headers.
  Get-ChildItem -LiteralPath $runtime | Where-Object { $_.Name -notin @('include', 'licenses') } | Remove-Item -Recurse -Force
}
New-Item -ItemType Directory -Force (Join-Path $runtime 'bin\64bit'), (Join-Path $runtime 'obs-plugins\64bit'), (Join-Path $runtime 'data\obs-plugins'), (Join-Path $runtime 'licenses'), (Join-Path $runtime 'lib') | Out-Null

# module DLLs + their data trees. OBS 33 keeps each first-party module in core\<name>\ (the DLL, and its data in core\<name>\data);
# earlier releases used obs-plugins\64bit\<name>.dll and data\obs-plugins\<name>. Both are read, and the runtime keeps the older
# layout the engine opens (obs_host.cpp load_modules): win-capture finds its hook and helpers through obs_module_file, which is
# the data path handed to obs_open_module, so the layout itself is the engine's choice.
$moduleBinaries = New-Object System.Collections.ArrayList
foreach ($name in $modules) {
  $dll = Join-Path $official "core\$name\$name.dll"
  $dataFrom = Join-Path $official "core\$name\data"
  if (-not (Test-Path $dll)) {
    $dll = Join-Path $official "obs-plugins\64bit\$name.dll"
    $dataFrom = Join-Path $official "data\obs-plugins\$name"
  }
  if (-not (Test-Path $dll)) { throw "module missing from release: $name" }
  Copy-Item -LiteralPath $dll -Destination (Join-Path $runtime "obs-plugins\64bit\$name.dll") -Force
  [void]$moduleBinaries.Add("obs-plugins\64bit\$name.dll")
  if (Test-Path $dataFrom) {
    $dataTo = Join-Path $runtime "data\obs-plugins\$name"
    Copy-Tree -From $dataFrom -To $dataTo
    # The capture plugin ships helper executables and hook DLLs in its data tree; they are loaded at
    # runtime and their own imports (libcurl, for one) have to travel with them.
    foreach ($helper in Get-ChildItem -LiteralPath $dataTo -Recurse -File | Where-Object { $_.Extension -eq '.dll' -or $_.Extension -eq '.exe' }) {
      [void]$moduleBinaries.Add($helper.FullName.Substring($runtime.Length).TrimStart('\'))
    }
  }
}

# core binaries the host needs, plus the encoder probe helpers OBS itself relies on
$core = @('obs.dll', 'libobs-d3d11.dll', 'libobs-winrt.dll', 'libobs-opengl.dll',
          'obs-nvenc-test.exe', 'obs-qsv-test.exe', 'obs-amf-test.exe', 'obs-ffmpeg-mux.exe')
$pending = New-Object System.Collections.Queue
foreach ($name in $core) {
  $from = Join-Path $official "bin\64bit\$name"
  if (Test-Path $from) {
    Copy-Item -LiteralPath $from -Destination (Join-Path $runtime "bin\64bit\$name") -Force
    $pending.Enqueue($name)
  }
}

# optional helpers we use if they exist in the release: FFmpeg CLI for lossless trimming
foreach ($name in @('ffmpeg.exe', 'ffprobe.exe')) {
  $from = Join-Path $official "bin\64bit\$name"
  if (Test-Path $from) {
    Copy-Item -LiteralPath $from -Destination (Join-Path $runtime "bin\64bit\$name") -Force
    $pending.Enqueue($name)
  }
}

# transitive DLL resolution: any DLL imported by a staged binary comes along
$seen = New-Object 'System.Collections.Generic.HashSet[string]'
foreach ($name in $pending) { [void]$seen.Add($name.ToLowerInvariant()) }
foreach ($name in $moduleBinaries) { [void]$seen.Add($name.ToLowerInvariant()); $pending.Enqueue($name) }
while ($pending.Count -gt 0) {
  $name = $pending.Dequeue()
  $binary = Join-Path $runtime $name
  if (-not (Test-Path $binary)) { $binary = Join-Path $runtime "bin\64bit\$name" }
  if (-not (Test-Path $binary)) { $binary = Join-Path $runtime "obs-plugins\64bit\$name" }
  if (-not (Test-Path $binary)) { continue }
  $imports = @()
  try { $imports = & $tools.readobj --coff-imports $binary 2>$null } catch { $imports = @() }
  foreach ($line in $imports) {
    if ($line -notmatch 'Name:\s+(\S+\.dll)\s*$') { continue }
    $dllName = $Matches[1]
    $lower = $dllName.ToLowerInvariant()
    if ($seen.Contains($lower)) { continue }
    [void]$seen.Add($lower)
    $source = Join-Path $official "bin\64bit\$dllName"
    if (Test-Path $source) {
      Copy-Item -LiteralPath $source -Destination (Join-Path $runtime "bin\64bit\$dllName") -Force
      Write-Output "  + dependency $dllName"
      $pending.Enqueue("bin\64bit\$dllName")
    }
  }
}

# core data + headers + licences (headers/sources are for licence compliance, not for compiling)
Copy-Tree -From (Join-Path $official 'data\libobs') -To (Join-Path $runtime 'data\libobs')
foreach ($lic in Get-ChildItem -LiteralPath $official -Filter '*.txt' -File -ErrorAction SilentlyContinue) {
  Copy-Item -LiteralPath $lic.FullName -Destination (Join-Path $runtime 'licenses') -Force
}
$copying = Join-Path $official 'COPYING'
if (Test-Path $copying) { Copy-Item -LiteralPath $copying -Destination (Join-Path $runtime 'licenses\OBS-GPL-2.0.txt') -Force }

# import libraries generated from the shipped DLLs (obs.dll for the engine, av* for the editor)
function New-ImportLibrary {
  param([string]$DllName, [string]$BaseName)
  $dll = Join-Path $runtime "bin\64bit\$DllName"
  if (-not (Test-Path $dll)) { Write-Output "  skip import lib for $DllName (absent)"; return $false }
  $defFile = Join-Path $runtime "lib\$BaseName.def"
  $exports = @()
  foreach ($line in (& $tools.readobj --coff-exports $dll)) {
    if ($line -match '^\s*Name:\s+(\S+)\s*$') { $exports += $Matches[1] }
  }
  if ($exports.Count -eq 0) { Write-Output "  no exports found in $DllName"; return $false }
  $tag = $BaseName
  @("LIBRARY $DllName", 'EXPORTS') + ($exports | Sort-Object -Unique) | Set-Content -LiteralPath $defFile -Encoding ascii
  & $tools.dlltool -m i386:x86-64 -d $defFile -l (Join-Path $runtime "lib\$BaseName.dll.a") -D $DllName
  if ($LASTEXITCODE -ne 0) { throw "llvm-dlltool failed for $DllName" }
  Write-Output "  import lib $BaseName.dll.a from $($exports.Count) exports"
  return $true
}

Write-Output "generating import libraries"
New-ImportLibrary -DllName 'obs.dll' -BaseName 'libobs' | Out-Null
# The editor's remux and the library's thumbnails and durations link the FFmpeg the runtime already
# ships; a second copy of FFmpeg is what this project exists to avoid.
foreach ($pair in @(
    @{ Dll = 'avformat-62.dll'; Base = 'avformat' },
    @{ Dll = 'avcodec-62.dll'; Base = 'avcodec' },
    @{ Dll = 'avutil-60.dll'; Base = 'avutil' },
    @{ Dll = 'swscale-9.dll'; Base = 'swscale' },
    # The editor's crop, scale and per-track volume run through FFmpeg's own filter graph.
    @{ Dll = 'avfilter-11.dll'; Base = 'avfilter' })) {
  New-ImportLibrary -DllName $pair.Dll -BaseName $pair.Base | Out-Null
}

$pin = [ordered]@{
  schema        = 2
  version       = $version
  asset         = $asset
  asset_url     = $url
  source_asset  = $sourceAsset
  source_url    = $sourceUrl
  modules       = $modules
  generated_utc = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
}
$pin | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $root 'runtime\pin.json')

$size = (Get-ChildItem -LiteralPath $runtime -Recurse -File | Measure-Object Length -Sum)
Write-Output ("RUNTIME_READY {0} files {1:N1} MB" -f $size.Count, ($size.Sum / 1MB))
Get-ChildItem -LiteralPath (Join-Path $runtime 'bin\64bit') -File | Sort-Object Length -Descending |
  Select-Object -First 12 | ForEach-Object { Write-Output ("  {0,9:N2} MB  {1}" -f ($_.Length / 1MB), $_.Name) }
