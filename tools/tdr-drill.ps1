# Drill for a graphics driver reset (TDR) under a running buffer: does the engine survive it and bring the buffer back?
#
#   powershell -File tools/tdr-drill.ps1            # forces a real TDR with dxcap -forcetdr: the screen goes black for a moment
#   powershell -File tools/tdr-drill.ps1 -DryRun    # everything except dxcap (checks the harness and a healthy engine)
#
# The reset blanks every screen on the machine and can take other programs' GPU work with it: run it when nobody is using the PC.
# It starts a throwaway engine on its own pipe and data root, and stops only the processes it started. It never touches another
# buffrx process. The checks (60 s after the reset): the engine pid answers status, the buffer is on again, engine.log carries
# libobs's "Device Remove/Reset" line (not on -DryRun) and no governor step line.
param(
  [switch]$DryRun,
  [int]$WatchSeconds = 60
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $repo 'runtime\bin\64bit'
$engineExe = Join-Path $bin 'buffrx-engine.exe'
if (-not (Test-Path $engineExe)) { throw "no staged engine at $engineExe (run tools/build.ps1 first)" }

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$dataRoot = Join-Path $env:TEMP "buffrx-tdr-drill-$stamp"
$clips = Join-Path $dataRoot 'clips'
New-Item -ItemType Directory -Force $clips | Out-Null
$pipe = "buffrx-tdr-drill-$stamp"
$configJson = @{ start_buffering = $true; auto_game_capture = $false; capture_mode = 'display'; clips_dir = $clips } | ConvertTo-Json
[System.IO.File]::WriteAllText((Join-Path $dataRoot 'config.json'), $configJson)

$engine = $null
$noise = $null
$results = New-Object System.Collections.Generic.List[object]
function Report([string]$name, [bool]$ok, [string]$detail = '') {
  $results.Add([pscustomobject]@{ Check = $name; Pass = $ok; Detail = $detail })
  Write-Host ('{0}  {1}  {2}' -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $name, $detail)
}

# engine-ask.ps1 reads BUFFRX_PIPE from its own process environment.
$previousPipe = $env:BUFFRX_PIPE
$previousPath = $env:PATH
function Ask([string]$command, [string]$arguments = '{}') {
  $file = Join-Path $dataRoot 'ask.json'
  [System.IO.File]::WriteAllText($file, $arguments)
  try { return (& powershell -NoProfile -File (Join-Path $PSScriptRoot 'engine-ask.ps1') -Command $command -ArgumentsFile $file -TimeoutMs 8000) -join "`n" }
  catch { return '' }
}
function BufferIsOn() { return ((Ask 'status.get') -match '"buffer_state"\s*:\s*"on"') }

try {
  $env:BUFFRX_PIPE = $pipe
  $env:BUFFRX_DATA_ROOT = $dataRoot
  $env:BUFFRX_RUNTIME_ROOT = Join-Path $repo 'runtime'
  $env:PATH = "$bin;$previousPath"
  $engine = Start-Process -FilePath $engineExe -WorkingDirectory $bin -PassThru -WindowStyle Hidden
  $noise = Start-Process -FilePath 'powershell.exe' -ArgumentList @('-NoProfile', '-File', (Join-Path $PSScriptRoot 'screen-noise.ps1')) -PassThru -WindowStyle Hidden
  Write-Host "engine pid $($engine.Id), data root $dataRoot, pipe $pipe"

  $on = $false
  for ($i = 0; $i -lt 40 -and -not $on; $i++) {
    Start-Sleep -Milliseconds 1000
    if ($engine.HasExited) { break }
    $on = BufferIsOn
  }
  Report 'buffer is on before the drill' $on
  if (-not $on) { throw 'the buffer never came on: nothing to drill' }

  if ($DryRun) {
    Write-Host 'dry run: dxcap skipped'
  } else {
    Write-Host 'forcing a driver reset (dxcap -forcetdr)'
    # dxcap needs an elevated shell and says so on stdout with exit code 0: without this check the drill "passed" with no reset.
    $dxcap = (& "$env:SystemRoot\System32\dxcap.exe" -forcetdr 2>&1) -join ' '
    $forced = $dxcap -notmatch 'failed'
    Report 'the driver reset was forced' $forced $(if (-not $forced) { $dxcap.Trim() } else { '' })
    if (-not $forced) { throw 'dxcap could not force a reset: run this script from an administrator PowerShell' }
  }

  $answered = $false
  $backOn = $false
  $deadline = (Get-Date).AddSeconds($WatchSeconds)
  while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 2000
    if ($engine.HasExited) { break }
    if ((Ask 'status.get') -match '"buffer_state"') { $answered = $true }
    if (BufferIsOn) { $backOn = $true }
  }
  $alive = -not $engine.HasExited
  Report 'the engine pid is alive' $alive $(if (-not $alive) { "exit code $($engine.ExitCode)" } else { '' })
  Report 'the engine answers status' $answered
  Report 'the buffer is on again' ($alive -and $backOn -and (BufferIsOn))

  $log = Join-Path $dataRoot 'logs\engine.log'
  $text = if (Test-Path $log) { Get-Content $log -Raw } else { '' }
  if ($DryRun) {
    Report 'engine.log has the libobs device reset line' $true 'skipped on a dry run'
  } else {
    Report 'engine.log has the libobs device reset line' ($text -match 'libobs: Device Remove/Reset')
  }
  Report 'engine.log has no governor step line' ($text -notmatch 'lowering its speed settings to step')
  if (Test-Path (Join-Path $dataRoot 'driver_crash.json')) { Write-Host 'note: driver_crash.json exists, so libobs ended the engine (see its message)' }
}
finally {
  # Only the processes started above, by pid.
  foreach ($started in @($engine, $noise)) {
    if ($started -and -not $started.HasExited) { try { Stop-Process -Id $started.Id -Force } catch { } }
  }
  $env:BUFFRX_PIPE = $previousPipe
  $env:PATH = $previousPath
  Remove-Item Env:BUFFRX_DATA_ROOT -ErrorAction SilentlyContinue
  Remove-Item Env:BUFFRX_RUNTIME_ROOT -ErrorAction SilentlyContinue
  Write-Host "log kept in $dataRoot\logs"
}

$failed = @($results | Where-Object { -not $_.Pass }).Count
Write-Host ("{0} checks, {1} failed" -f $results.Count, $failed)
exit $(if ($failed -eq 0) { 0 } else { 1 })
