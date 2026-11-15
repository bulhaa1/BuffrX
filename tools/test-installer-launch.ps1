# Exercises the product's actual Inno [Run] rules with a harmless launcher fixture.
# The fixture installs only under work/, without an uninstall entry or shortcuts.
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$paths = Get-Content -LiteralPath (Join-Path $repo 'work/paths.json') -Raw | ConvertFrom-Json
$root = Join-Path $repo ('work/installer-launch-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
$source = Join-Path $root 'launcher.cpp'
@'
#include <cstdio>
#include <cstdlib>
int main() {
  const char* path = std::getenv("BUFFRX_INSTALLER_LAUNCH_LOG");
  if (!path) return 2;
  FILE* file = std::fopen(path, "ab");
  if (!file) return 3;
  std::fputs("launch\n", file);
  return std::fclose(file) == 0 ? 0 : 4;
}
'@ | Set-Content -LiteralPath $source -Encoding ascii
& $paths.clangxx -static $source -o (Join-Path $root 'BuffrX.exe')
if ($LASTEXITCODE -ne 0) { throw 'Launcher fixture build failed' }
$product = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'buffrx.iss') -Raw
$rules = [regex]::Match($product, '(?s)\[Run\].*?(?=\{ The editor)').Value
if (-not $rules.Contains('function RelaunchAfterSilent')) { throw 'Installer launch rules not found' }
$fixture = @'
#define AppName "BuffrX launch probe"
#define AppExe "BuffrX.exe"
[Setup]
AppName={#AppName}
AppVersion=1.0
DefaultDirName={src}\installed
PrivilegesRequired=lowest
Uninstallable=no
CreateUninstallRegKey=no
UsePreviousAppDir=no
DisableDirPage=yes
DisableProgramGroupPage=yes
OutputBaseFilename=launch-probe
[Files]
Source: "BuffrX.exe"; DestDir: "{app}"; Flags: ignoreversion
'@
$script = Join-Path $root 'fixture.iss'
($fixture + "`r`n" + $rules) | Set-Content -LiteralPath $script -Encoding ascii
$compiler = Get-ChildItem -LiteralPath (Join-Path $repo 'work/toolchain') -Filter ISCC.exe -Recurse | Select-Object -First 1
& $compiler.FullName /Qp ('/O' + $root) $script
if ($LASTEXITCODE -ne 0) { throw 'Installer fixture build failed' }
$previous = $env:BUFFRX_INSTALLER_LAUNCH_LOG
try {
  foreach ($relaunch in @(0, 1)) {
    $log = Join-Path $root ('launch-' + $relaunch + '.log')
    $env:BUFFRX_INSTALLER_LAUNCH_LOG = $log
    $process = Start-Process -FilePath (Join-Path $root 'launch-probe.exe') -ArgumentList @(
      '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', ('/relaunch=' + $relaunch),
      ('/DIR="' + (Join-Path $root ('installed-' + $relaunch)) + '"'),
      ('/LOG="' + (Join-Path $root ('setup-' + $relaunch + '.log')) + '"')
    ) -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit(30000)) { $process.Kill(); throw 'Installer fixture timed out' }
    if ($process.ExitCode -ne 0) { throw 'Installer fixture failed' }
    Start-Sleep -Milliseconds 500
    $count = 0
    if (Test-Path -LiteralPath $log) { $count = @(Get-Content -LiteralPath $log).Count }
    if ($count -ne $relaunch) { throw "Silent /relaunch=$relaunch started $count copies; expected $relaunch" }
    Write-Output "PASS silent /relaunch=$relaunch starts $count copies"
  }
} finally { $env:BUFFRX_INSTALLER_LAUNCH_LOG = $previous }
