# Builds BuffrX-Setup.exe (Inno Setup 6) from dist\BuffrX-<version>, and the portable zip beside it.
#
#   powershell -File tools/installer.ps1 -Version 0.5.0
#
# Inno Setup is pinned and staged into work/toolchain like every other build tool. Nothing here touches
# the machine outside the repository.
#
# The installer's size is the package's size: avcodec-62.dll alone is 36 MB of the ~76 MB folder, and
# LZMA2/ultra64 over the rest is the only lever left. Do not add files to the package for the installer's
# sake; trim from package.ps1 instead.

param(
  [string]$Version = '0.5.0',
  [string]$OutDir = 'dist',
  [switch]$NoZip
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem | Out-Null

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$work = Join-Path $root 'work\toolchain'
$files = Join-Path $work 'files'
New-Item -ItemType Directory -Force $files | Out-Null

$innoVersion = '6.7.4'
$innoUrl = 'https://github.com/jrsoftware/issrc/releases/download/is-' + ($innoVersion -replace '\.', '_') + '/innosetup-' + $innoVersion + '.exe'
# These hashes were pinned from the staged files after their publisher signatures were verified.
$innoSha = 'a549929319f9120716677c685535ca4671e4b937b5fdc87e2585f32d8eb30e8c'
$webviewUrl = 'https://go.microsoft.com/fwlink/p/?LinkId=2124703'
$webviewSha = 'cf2f2445fef0c5c14e372045f84998771eeb8bb01d59be4afd00692fa160082e'

function Get-PinnedFile {
  param([string]$Name, [string]$Url, [string]$Sha256, [string]$Publisher)
  $file = Join-Path $files $Name
  $pin = "$file.sha256"
  if (Test-Path $file) {
    $actual = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $Sha256) { throw "$Name hash mismatch: expected $Sha256 got $actual" }
    $signature = Get-AuthenticodeSignature -LiteralPath $file
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notlike "CN=$Publisher,*") {
      throw "$Name does not have a valid signature from $Publisher"
    }
    return $file
  }
  $partial = "$file.partial"
  & curl.exe -fL --retry 3 --silent --show-error $Url -o $partial
  if ($LASTEXITCODE -ne 0) { throw "download failed: $Url" }
  $actual = (Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($actual -ne $Sha256) {
    Remove-Item -LiteralPath $partial -Force
    throw "$Name hash mismatch: expected $Sha256 got $actual"
  }
  $signature = Get-AuthenticodeSignature -LiteralPath $partial
  if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notlike "CN=$Publisher,*") {
    Remove-Item -LiteralPath $partial -Force
    throw "$Name does not have a valid signature from $Publisher"
  }
  Move-Item -LiteralPath $partial -Destination $file -Force
  Set-Content -LiteralPath $pin -Value $actual -NoNewline
  Write-Host "$Name sha256:$actual"
  return $file
}

# Inno Setup: silent, into the repository. /PORTABLE=1 leaves no uninstall entry behind where the
# stager supports it; the compiler is the only thing kept.
$iscc = Join-Path $work ('innosetup-' + $innoVersion + '\ISCC.exe')
if (-not (Test-Path $iscc)) {
  $setup = Get-PinnedFile -Name ('innosetup-' + $innoVersion + '.exe') -Url $innoUrl -Sha256 $innoSha -Publisher 'Pyrsys B.V.'
  $dest = Join-Path $work ('innosetup-' + $innoVersion)
  New-Item -ItemType Directory -Force $dest | Out-Null
  $proc = Start-Process -FilePath $setup -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', '/PORTABLE=1', ('/DIR="' + $dest + '"') -PassThru -Wait
  if ($proc.ExitCode -ne 0 -or -not (Test-Path $iscc)) { throw "Inno Setup did not stage (exit $($proc.ExitCode)); looked for $iscc" }
  Write-Host ('innosetup ' + $innoVersion + ' staged')
}
$compilerSignature = Get-AuthenticodeSignature -LiteralPath $iscc
if ($compilerSignature.Status -ne 'Valid' -or $compilerSignature.SignerCertificate.Subject -notlike 'CN=Pyrsys B.V.,*') {
  throw 'the staged Inno compiler does not have a valid Pyrsys signature'
}

$src = Join-Path $root (Join-Path $OutDir ('BuffrX-' + $Version))
if (-not (Test-Path (Join-Path $src 'BuffrX.exe'))) { throw "no packaged folder at $src - run tools/package.ps1 first" }

# Verify the engine in the folder, not only its directory name or an old manifest.
$checkRoot = [IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetTempPath()) ('buffrx-installer-check-' + [guid]::NewGuid().ToString('N'))))
$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
if (-not $checkRoot.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'invalid installer check directory' }
New-Item -ItemType Directory -Path $checkRoot | Out-Null
$previousDataRoot = $env:BUFFRX_DATA_ROOT
try {
  $env:BUFFRX_DATA_ROOT = $checkRoot
  $stdout = Join-Path $checkRoot 'smoke.out'
  $stderr = Join-Path $checkRoot 'smoke.err'
  $engine = Join-Path $src 'bin\64bit\buffrx-engine.exe'
  $smokeProcess = Start-Process -FilePath $engine -ArgumentList '--package-smoke-test' -WorkingDirectory $src -PassThru -Wait -RedirectStandardOutput $stdout -RedirectStandardError $stderr
  $smoke = Get-Content -LiteralPath $stdout -Raw
  if ($smokeProcess.ExitCode -ne 0 -or $smoke -notmatch 'BUFFRX_PACKAGE_SMOKE_OK' -or
      $smoke -notmatch 'version=(\S+)' -or $Matches[1] -ne $Version) {
    throw "the staged engine does not report version $Version in a successful smoke test"
  }
} finally {
  $env:BUFFRX_DATA_ROOT = $previousDataRoot
  Remove-Item -LiteralPath $checkRoot -Recurse -Force -ErrorAction SilentlyContinue
}

$webview = Get-PinnedFile -Name 'MicrosoftEdgeWebView2Setup.exe' -Url $webviewUrl -Sha256 $webviewSha -Publisher 'Microsoft Corporation'

# The wizard wears the product: art is redrawn every build so it can never drift from the mark or palette.
& (Join-Path $PSScriptRoot 'make-installer-art.ps1') | Out-Host
$art = Join-Path $PSScriptRoot 'installer-art\wizard-164x314.png'
if (-not (Test-Path $art)) { throw "installer art missing: $art" }

$outAbs = (Resolve-Path (Join-Path $root $OutDir)).Path
Write-Host ('compressing ' + $src + ' ...')
& $iscc ('/DAppVersion=' + $Version) ('/DSrc=' + $src) ('/DWebView2=' + $webview) ('/O' + $outAbs) (Join-Path $PSScriptRoot 'buffrx.iss')
if ($LASTEXITCODE -ne 0) { throw "ISCC failed with exit $LASTEXITCODE" }

$setupExe = Join-Path $outAbs 'BuffrX-Setup.exe'
if (-not (Test-Path $setupExe)) { throw "ISCC reported success but wrote no $setupExe" }

if (-not $NoZip) {
  $zip = Join-Path $outAbs ('BuffrX-' + $Version + '.zip')
  if (Test-Path $zip) { Remove-Item -LiteralPath $zip -Force }
  [System.IO.Compression.ZipFile]::CreateFromDirectory($src, $zip, [System.IO.Compression.CompressionLevel]::Optimal, $false)
}

$releaseFiles = @($setupExe)
if (-not $NoZip) { $releaseFiles += $zip }
$releaseSums = @()
foreach ($file in $releaseFiles) {
  if (-not (Test-Path $file)) { continue }
  $hash = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
  Write-Host ('{0}  {1:N1} MB  sha256 {2}' -f (Split-Path $file -Leaf), ((Get-Item -LiteralPath $file).Length / 1MB), $hash)
  $releaseSums += ('{0}  {1}' -f $hash, (Split-Path $file -Leaf))
}
$releaseSums | Set-Content -LiteralPath (Join-Path $outAbs 'SHA256SUMS-release.txt') -Encoding ascii
Write-Host 'INSTALLER_OK'
