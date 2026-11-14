# Repo-local Rust for the full app. Native logic tests do not need this.
$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw 'The full app build requires Windows.' }
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$cargoHome = Join-Path $root 'work\cargo'
$rustupHome = Join-Path $root 'work\rustup'
$previousCargo = $env:CARGO_HOME
$previousRustup = $env:RUSTUP_HOME
try {
  $env:CARGO_HOME = $cargoHome
  $env:RUSTUP_HOME = $rustupHome
  $rustup = Join-Path $cargoHome 'bin\rustup.exe'
  if (-not (Test-Path -LiteralPath $rustup)) {
    $installer = Join-Path $root 'work\rustup-init-1.28.2.exe'
    New-Item -ItemType Directory -Force (Split-Path $installer) | Out-Null
    if (-not (Test-Path -LiteralPath $installer)) {
      & curl.exe -fL --retry 3 --silent --show-error 'https://static.rust-lang.org/rustup/archive/1.28.2/x86_64-pc-windows-gnu/rustup-init.exe' -o "$installer.partial"
      if ($LASTEXITCODE -ne 0) { throw 'Rustup download failed.' }
      Move-Item -LiteralPath "$installer.partial" -Destination $installer -Force
    }
    $expected = 'ccbfd951d8024856043b3a0c3903a59f39937bce8d3074768b0d3da55f21e817'
    if ((Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) { throw 'Rustup installer hash mismatch.' }
    & $installer -y --no-modify-path --profile minimal --default-host x86_64-pc-windows-gnullvm --default-toolchain none
    if ($LASTEXITCODE -ne 0) { throw 'Repo-local Rustup setup failed.' }
  }
  $version = '1.99.1'
  $toolchain = "$version-x86_64-pc-windows-gnullvm"
  # Reuse an identical repo-local stable compiler instead of downloading it a second time.
  $stableCompiler = Join-Path $rustupHome 'toolchains\stable-x86_64-pc-windows-gnullvm\bin\rustc.exe'
  if (Test-Path -LiteralPath $stableCompiler) {
    $installedVersion = & $stableCompiler --version
    if ($LASTEXITCODE -eq 0 -and $installedVersion.StartsWith("rustc $version ")) { $toolchain = 'stable-x86_64-pc-windows-gnullvm' }
  }
  if ($toolchain -ne 'stable-x86_64-pc-windows-gnullvm') {
    & $rustup toolchain install $toolchain --profile minimal --no-self-update
    if ($LASTEXITCODE -ne 0) { throw 'Pinned Rust toolchain setup failed.' }
  }
  @{ version = $version; toolchain = $toolchain } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $root 'work\rust-toolchain.json')
  Write-Host 'Repo-local Rust ready. No system PATH changes were made.'
} finally {
  $env:CARGO_HOME = $previousCargo
  $env:RUSTUP_HOME = $previousRustup
}
