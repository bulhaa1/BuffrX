# Guards against the "update deletes the clips" bug seen in another capture app, where the clips folder
# had been set to the folder the program lives in and the update wiped it.
# 1. tools/buffrx.iss must carry no delete directive, so Setup and Uninstall only touch files they installed.
# 2. A fixture with the product's [Files] flags installs into a folder that already holds a clip, updates over
#    it, then uninstalls: the clip and a clip subfolder must survive all three.
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$product = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'buffrx.iss') -Raw
foreach ($bad in '[InstallDelete]', '[UninstallDelete]', 'filesandordirs', 'DelTree') {
  if ($product.Contains($bad)) { throw "buffrx.iss contains '$bad': an update or uninstall could delete user clips" }
}
Write-Output 'PASS buffrx.iss has no delete directive'

$paths = Get-Content -LiteralPath (Join-Path $repo 'work/paths.json') -Raw | ConvertFrom-Json
$root = Join-Path $repo ('work/installer-clips-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
$source = Join-Path $root 'launcher.cpp'
'int main() { return 0; }' | Set-Content -LiteralPath $source -Encoding ascii
& $paths.clangxx -static $source -o (Join-Path $root 'BuffrX.exe')
if ($LASTEXITCODE -ne 0) { throw 'Fixture build failed' }
$files = [regex]::Match($product, '(?m)^Source: "\{#Src\}\\\*".*$').Value
if (-not $files) { throw 'The product [Files] line was not found' }
$flags = [regex]::Match($files, 'Flags: .*$').Value
$fixture = @"
[Setup]
AppId={{0B6E2D54-71C4-4F0A-9A53-6C1D3E9B2F10}
AppName=BuffrX clips probe
AppVersion=1.0
DefaultDirName={src}\installed
PrivilegesRequired=lowest
UsePreviousAppDir=no
DisableDirPage=yes
DisableProgramGroupPage=yes
OutputBaseFilename=clips-probe
[Files]
Source: "BuffrX.exe"; DestDir: "{app}"; $flags
"@
$script = Join-Path $root 'fixture.iss'
$fixture | Set-Content -LiteralPath $script -Encoding ascii
$compiler = Get-ChildItem -LiteralPath (Join-Path $repo 'work/toolchain') -Filter ISCC.exe -Recurse | Select-Object -First 1
& $compiler.FullName /Qp ('/O' + $root) $script
if ($LASTEXITCODE -ne 0) { throw 'Fixture installer build failed' }

$app = Join-Path $root 'clips-and-program'
New-Item -ItemType Directory -Path (Join-Path $app 'Screenshots') | Out-Null
$clip = Join-Path $app 'clip.mp4'
$shot = Join-Path $app 'Screenshots\shot.png'
'clip' | Set-Content -LiteralPath $clip
'shot' | Set-Content -LiteralPath $shot
function Run($exe, $arguments) {
  $p = Start-Process -FilePath $exe -ArgumentList $arguments -WindowStyle Hidden -PassThru
  if (-not $p.WaitForExit(60000)) { $p.Kill(); throw "$exe timed out" }
  if ($p.ExitCode -ne 0) { throw "$exe exited $($p.ExitCode)" }
}
function Need($step) {
  foreach ($f in $clip, $shot) { if (-not (Test-Path -LiteralPath $f)) { throw "$step deleted $f" } }
  Write-Output "PASS clips survive $step"
}
$setup = Join-Path $root 'clips-probe.exe'
$install = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', ('/DIR="' + $app + '"'))
try {
  Run $setup $install
  Need 'install'
  Run $setup $install
  Need 'update'
} finally {
  $uninstaller = Get-ChildItem -LiteralPath $app -Filter 'unins*.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($uninstaller) { Run $uninstaller.FullName @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART') }
}
Need 'uninstall'
Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
