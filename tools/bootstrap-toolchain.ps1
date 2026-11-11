# Pinned build toolchain for BuffrX: downloads once into work/toolchain, verifies every archive by
# SHA-256 (or pins the hash on first use for artifacts publishing no checksum), and writes the
# resolved paths to work/toolchain.json. Nothing outside the repository is touched.
#
# Windows PowerShell 5.1 compatible on purpose: no ternaries, no null-coalescing, no 3-arg Join-Path.
#
# -ShareSpike also fetches the peer-to-peer live-link spike's sources (libdatachannel and what it
# builds from: libjuice, usrsctp, plog, Mbed TLS). Nothing the product ships needs them.

param([switch]$ShareSpike)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem | Out-Null

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$work = Join-Path $root 'work\toolchain'
New-Item -ItemType Directory -Force $work | Out-Null

function Get-PinnedPackage {
  param([string]$Name, [string]$Url, [string]$Sha256)

  $zip = Join-Path $work ("$Name.pkg")
  $dest = Join-Path $work $Name
  $marker = Join-Path $dest '.complete'
  $pinFile = Join-Path $work "$Name.sha256"

  if (Test-Path $marker) {
    $recorded = (Get-Content -LiteralPath $pinFile -Raw).Trim()
    # The marker says the archive was unpacked once; it does not say the archive is still the pinned one,
    # and every later build trusts these files. Re-hash the archive while it is still here.
    if (Test-Path $zip) {
      $cached = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
      if ($Sha256 -ne 'PIN' -and $cached -ne $Sha256) { throw "$Name archive hash mismatch: expected $Sha256 got $cached" }
      if ($cached -ne $recorded) { throw "$Name archive changed since it was unpacked: recorded $recorded, now $cached" }
      Write-Host "$Name cached-ok sha256:$cached"
    } else {
      Write-Host "$Name cached-ok sha256:$recorded (archive removed, not re-hashed)"
    }
    return $dest
  }

  if (-not (Test-Path $zip)) {
    $partial = "$zip.partial"
    & curl.exe -fL --retry 3 --silent --show-error $Url -o $partial
    if ($LASTEXITCODE -ne 0) { throw "download failed: $Url" }
    Move-Item -LiteralPath $partial -Destination $zip -Force
  }

  $actual = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($Sha256 -eq 'PIN') {
    Set-Content -LiteralPath $pinFile -Value $actual -NoNewline
    Write-Warning "$Name has no pinned hash: trusted on first use, recorded sha256:$actual in $pinFile"
  } else {
    if ($actual -ne $Sha256) { throw "$Name hash mismatch: expected $Sha256 got $actual" }
    Set-Content -LiteralPath $pinFile -Value $actual -NoNewline
    Write-Host "$Name verified sha256:$actual"
  }

  if (Test-Path $dest) { Remove-Item -LiteralPath $dest -Recurse -Force }
  [System.IO.Compression.ZipFile]::ExtractToDirectory($zip, $dest)
  Set-Content -LiteralPath $marker -Value $actual -NoNewline
  return $dest
}

function Get-PinnedFile {
  param([string]$Name, [string]$Url, [string]$Sha256)

  $dir = Join-Path $work 'files'
  New-Item -ItemType Directory -Force $dir | Out-Null
  $file = Join-Path $dir $Name
  if (-not (Test-Path $file)) {
    $partial = "$file.partial"
    & curl.exe -fL --retry 3 --silent --show-error $Url -o $partial
    if ($LASTEXITCODE -ne 0) { throw "download failed: $Url" }
    Move-Item -LiteralPath $partial -Destination $file -Force
  }
  $actual = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($actual -ne $Sha256) {
    Remove-Item -LiteralPath $file -Force
    throw "$Name hash mismatch: expected $Sha256 got $actual"
  }
  Write-Host "$Name verified sha256:$actual"
  return $file
}

# A .tar.bz2 (Mbed TLS publishes its release, with its generated files, only that way), unpacked by
# Windows' own tar.
function Get-PinnedTarball {
  param([string]$Name, [string]$Url, [string]$Sha256)
  $dest = Join-Path $work $Name
  $marker = Join-Path $dest '.complete'
  if (Test-Path $marker) {
    Write-Host "$Name cached-ok sha256:$Sha256"
    return $dest
  }
  $file = Get-PinnedFile "$Name.tar.bz2" $Url $Sha256
  if (Test-Path $dest) { Remove-Item -LiteralPath $dest -Recurse -Force }
  New-Item -ItemType Directory -Force $dest | Out-Null
  & tar.exe -xjf $file -C $dest
  if ($LASTEXITCODE -ne 0) { throw "$Name could not be unpacked" }
  Set-Content -LiteralPath $marker -Value $Sha256 -NoNewline
  return $dest
}

function Get-SingleDirectory {
  param([string]$Path)
  $dirs = @(Get-ChildItem -LiteralPath $Path | Where-Object { $_.PSIsContainer })
  if ($dirs.Count -eq 0) { throw "no directory inside $Path" }
  return $dirs[0].FullName
}

$llvm  = Get-PinnedPackage 'llvm'  'https://github.com/mstorsjo/llvm-mingw/releases/download/20260922/llvm-mingw-20260922-ucrt-x86_64.zip' 'e3ad77d117a4bea19a7a3b333341824d79a5a371004a10e25b8504e7b3047666'
$cmake = Get-PinnedPackage 'cmake' 'https://github.com/Kitware/CMake/releases/download/v4.4.3/cmake-4.4.3-windows-x86_64.zip' '4d52ebab7193a698651639ed80d8d04fd903358843572cf44c7fd234cb7c26ab'
$ninja = Get-PinnedPackage 'ninja' 'https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip' '07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65'
$sdk   = Get-PinnedPackage 'winsdk' 'https://api.nuget.org/v3-flatcontainer/microsoft.windows.sdk.cpp/10.0.28000.2705/microsoft.windows.sdk.cpp.10.0.28000.2705.nupkg' 'a74ca8f9af98bd61925d9e2932ad98f746306123167b8f0bba99cd9ea9f03807'
$wv2   = Get-PinnedPackage 'webview2' 'https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.4258.31/microsoft.web.webview2.1.0.4258.31.nupkg' '56f7f4b8bf9aee4b8efefbbdd4f67d5f74ebd1b100ed0806da71bf76af481aa9'

# The engine's own interface (Dear ImGui, MIT) and its one face, JetBrains Mono (SIL OFL), from its
# release. ImGui reads the TTFs; the editor page carries the release's WOFF2s in ui/src/fonts.
$imgui = Get-PinnedPackage 'imgui' 'https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.zip' 'e1c46d676c2bcb7ced847ba27f50553e33a19db97b3cadaec7f8be64449139f8'
$jetbrainsMono = Get-PinnedPackage 'jetbrains-mono' 'https://github.com/JetBrains/JetBrainsMono/releases/download/v2.304/JetBrainsMono-2.304.zip' '6f6376c6ed2960ea8a963cd7387ec9d76e3f629125bc33d1fdcd7eb7012f7bbf'

$llvmRoot  = Get-SingleDirectory $llvm
$cmakeRoot = Get-SingleDirectory $cmake
$sdkRoot   = Get-SingleDirectory $sdk
$wv2Root   = Get-SingleDirectory $wv2

$ninjaExe = (Get-ChildItem -LiteralPath $ninja -Filter 'ninja.exe' -Recurse | Select-Object -First 1).FullName

$config = [ordered]@{
  schema           = 1
  llvm_root        = $llvmRoot
  clangxx          = (Join-Path $llvmRoot 'bin\x86_64-w64-mingw32-clang++.exe')
  clang            = (Join-Path $llvmRoot 'bin\x86_64-w64-mingw32-clang.exe')
  windres          = (Join-Path $llvmRoot 'bin\x86_64-w64-mingw32-windres.exe')
  readobj          = (Join-Path $llvmRoot 'bin\llvm-readobj.exe')
  dlltool          = (Join-Path $llvmRoot 'bin\llvm-dlltool.exe')
  cmake            = (Join-Path $cmakeRoot 'bin\cmake.exe')
  ninja            = $ninjaExe
  winsdk_um        = (Join-Path $sdkRoot 'c\Include\10.0.28000.0\um')
  winsdk_shared    = (Join-Path $sdkRoot 'c\Include\10.0.28000.0\shared')
  winsdk_ucrt      = (Join-Path $sdkRoot 'c\Include\10.0.28000.0\ucrt')
  winsdk_winrt     = (Join-Path $sdkRoot 'c\Include\10.0.28000.0\winrt')
  webview2_include = (Join-Path $wv2Root 'build\native\include')
  webview2_loader  = (Join-Path $wv2Root 'build\native\x64\WebView2Loader.dll')
  imgui_root       = (Get-SingleDirectory $imgui)
  jetbrains_mono_root = $jetbrainsMono
}

if ($ShareSpike) {
  # libdatachannel's release archive has no submodules; each is pinned at the commit that tag records.
  $ldc = Get-PinnedPackage 'libdatachannel' 'https://github.com/paullouisageneau/libdatachannel/archive/refs/tags/v0.24.6.zip' 'c78ca1b7ce45f08c7e6233eba4d714bde10c6e3a4e60aad0be891e2594807519'
  $juice = Get-PinnedPackage 'libjuice' 'https://github.com/paullouisageneau/libjuice/archive/b89c792e3612faf2f12cf35bcc56857313a06be3.zip' 'c937ec97110b559e4a27885a3098c7a65dad2459c813569441a1329f74662381'
  $sctp = Get-PinnedPackage 'usrsctp' 'https://github.com/paullouisageneau/usrsctp/archive/fec583d54493f879d2ae44a743423bf8a04371ab.zip' '752b4337bece1138f4f4151cec4240956a7230b007e11bc1a6915cbda58d7086'
  $plog = Get-PinnedPackage 'plog' 'https://github.com/SergiusTheBest/plog/archive/94899e0b926ac1b0f4750bfbd495167b4a6ae9ef.zip' '195415f664dccc6682c344ca0e1966674555f0be07e9552d4f760860acce719d'
  # Mbed TLS 3.6 LTS: libdatachannel asks for 3.x. The hash is the one published with the release.
  $mbed = Get-PinnedTarball 'mbedtls' 'https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2' 'a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6'
  $config['libdatachannel_root'] = (Get-SingleDirectory $ldc)
  $config['libjuice_root'] = (Get-SingleDirectory $juice)
  $config['usrsctp_root'] = (Get-SingleDirectory $sctp)
  $config['plog_root'] = (Get-SingleDirectory $plog)
  $config['mbedtls_root'] = (Get-SingleDirectory $mbed)
}

$out = Join-Path $root 'work\toolchain.json'
$config | ConvertTo-Json | Set-Content -LiteralPath $out
Write-Output "TOOLCHAIN_READY $out"
foreach ($key in $config.Keys) { Write-Output ("  {0,-18} {1}" -f $key, $config[$key]) }
