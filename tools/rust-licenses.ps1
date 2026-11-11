# Writes runtime\licenses\buffrx-share-THIRD-PARTY.txt: every crate the sharing helper links, its
# licence expression, and the licence texts it ships. MIT, Apache-2.0 and BSD require their notice to
# travel with the binary. Run by build.ps1 after the helper is built; needs the crates already fetched.
param(
  [string]$CargoHome = '',
  [string]$Manifest = 'src-share',
  [string]$Out = '',
  [string]$Title = 'BuffrX sharing helper (buffrx-share.exe): third-party Rust crates'
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $CargoHome) { $CargoHome = Join-Path $root 'work\cargo' }
if (-not $Out) { $Out = Join-Path $root 'runtime\licenses\buffrx-share-THIRD-PARTY.txt' }

$env:RUSTUP_HOME = Join-Path $root 'work\rustup'
$env:CARGO_HOME = $CargoHome
$env:PATH = (Join-Path $CargoHome 'bin') + ';' + $env:PATH
# Only the crates that reach the Windows build, not the other platforms' dependencies.
$host_triple = ((& rustc -vV) | Select-String '^host: (.+)$').Matches[0].Groups[1].Value
Push-Location (Join-Path $root $Manifest)
try {
  $json = & (Join-Path $CargoHome 'bin\cargo.exe') metadata --locked --format-version 1 --filter-platform $host_triple
  if ($LASTEXITCODE -ne 0) { throw 'cargo metadata failed' }
} finally { Pop-Location }
$meta = ($json -join "`n") | ConvertFrom-Json

$texts = @{}   # hash -> text
$byText = @{}  # hash -> list of crates
$lines = New-Object System.Collections.Generic.List[string]
$missing = @()
$packages = $meta.packages | Where-Object { $_.source } | Sort-Object name, version
foreach ($package in $packages) {
  $folder = Split-Path -Parent $package.manifest_path
  $files = Get-ChildItem -LiteralPath $folder -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^(LICENSE|LICENCE|COPYING|UNLICENSE)' }
  $license = if ($package.license) { $package.license } else { $package.license_file }
  $lines.Add("$($package.name) $($package.version) - $license")
  if (-not $files) { $missing += "$($package.name) $($package.version)"; continue }
  foreach ($file in $files) {
    $text = (Get-Content -LiteralPath $file.FullName -Raw).Trim()
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($text)
    $hash = [System.BitConverter]::ToString([System.Security.Cryptography.SHA256]::Create().ComputeHash($bytes))
    if (-not $texts.ContainsKey($hash)) { $texts[$hash] = $text; $byText[$hash] = New-Object System.Collections.Generic.List[string] }
    $byText[$hash].Add("$($package.name) $($package.version)")
  }
}

$report = New-Object System.Collections.Generic.List[string]
$report.Add($Title)
$report.Add('=================================================================')
$report.Add('')
$lines | ForEach-Object { $report.Add($_) }
$report.Add('')
$report.Add('Licence texts. Each text applies to the crates listed above it.')
foreach ($hash in ($texts.Keys | Sort-Object)) {
  $report.Add('')
  $report.Add('-----------------------------------------------------------------')
  $report.Add('Used by: ' + ((($byText[$hash] | Sort-Object -Unique) -join ', ')))
  $report.Add('')
  $report.Add($texts[$hash])
}
New-Item -ItemType Directory -Force (Split-Path -Parent $Out) | Out-Null
Set-Content -LiteralPath $Out -Value $report -Encoding utf8
Write-Host "wrote $Out ($($packages.Count) crates, $($texts.Count) distinct licence texts)"
# A crate with an SPDX expression and no file still names its licence above; say which ones, so a
# missing notice is a decision and not an accident.
if ($missing) { Write-Host "no licence file shipped in: $($missing -join ', ')" }
