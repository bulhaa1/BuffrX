<#
.SYNOPSIS
  One command that answers "is this build safe to hand out?": build, unit checks, capture gate and acceptance, plus the
  short fault probes in the full run.

.DESCRIPTION
  Every step is a child process with its own exit code, so one failure never hides the next. The staged engine is checked
  against the one the build produced: a failed stage leaves the previous exe in runtime\bin\64bit, which runs and looks
  plausible (the 2026-11-06 black-clip investigation started from exactly that).

  Quick (default is full): build, unit checks, capture integrity, acceptance. Full adds chaos-restart, pipe-fuzz,
  settings-churn and a soak. Quick is about 3 minutes, full about 12, on the development machine. The encoder-refusal
  probe needs a real game window and is not run here; neither is the package smoke test (tools\package.ps1).

  Close BuffrX first: a running engine holds runtime\bin\64bit\buffrx-engine.exe and makes the stage fail. Tests keep
  their state under %TEMP%, never in the person's profile. -SkipBuild tests the staged binaries as they are.

.PARAMETER Quick
  Skip the fault probes and the soak; about 6 minutes instead of 20.

.PARAMETER SkipBuild
  Do not rebuild. Still checks that the staged exe matches build\buffrx-engine.exe.

.PARAMETER KeepData
  Keep the unit-test data root under %TEMP%\buffrx-preflight-tests.

.EXAMPLE
  powershell -File tools\preflight.ps1 -Quick
  powershell -File tools\preflight.ps1
#>
param(
  [switch]$Quick,
  [switch]$SkipBuild,
  [switch]$KeepData
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$failures = New-Object System.Collections.Generic.List[string]
$clock = [Diagnostics.Stopwatch]::StartNew()

# A display that has gone to sleep hands DXGI duplication no frames, and the capture gate then fails with
# "the buffer did not come on" for a reason that is not the build. Keep it awake for the run.
Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
public static class PreflightWake {
  [DllImport("kernel32.dll")]
  public static extern uint SetThreadExecutionState(uint flags);
}
'@
[void][PreflightWake]::SetThreadExecutionState(2147483651)

function Invoke-Step([string]$Name, [scriptblock]$Action) {
  Write-Host ''
  Write-Host ("== " + $Name)
  $stepClock = [Diagnostics.Stopwatch]::StartNew()
  $global:LASTEXITCODE = 0
  try {
    & $Action
  } catch {
    $failures.Add($Name + ': ' + $_.Exception.Message)
    Write-Host ("FAIL  " + $Name + ': ' + $_.Exception.Message)
    return $false
  }
  $stepClock.Stop()
  if ($global:LASTEXITCODE -ne 0) {
    $failures.Add(('{0} (exit {1})' -f $Name, $global:LASTEXITCODE))
    Write-Host ('FAIL  {0}  {1:N1}s' -f $Name, $stepClock.Elapsed.TotalSeconds)
    return $false
  }
  Write-Host ('PASS  {0}  {1:N1}s' -f $Name, $stepClock.Elapsed.TotalSeconds)
  return $true
}

function Invoke-Tool([string]$Name, [string]$Script, [string[]]$Arguments = @()) {
  Invoke-Step $Name {
    # A native child's stderr must not become a terminating error in this shell: the tool's own exit
    # code is the result, and several probes print engine warnings there on purpose.
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
      & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot $Script) @Arguments
    } finally {
      $ErrorActionPreference = $previous
    }
  }
}

if (-not $SkipBuild) {
  $running = @(Get-Process buffrx-engine -ErrorAction SilentlyContinue)
  if ($running.Count -gt 0) {
    throw "BuffrX is running (PID $($running[0].Id)); close it first, or pass -SkipBuild to test what is already staged"
  }
  if (-not (Invoke-Tool 'build' 'build.ps1')) {
    throw 'the build failed; nothing below can be trusted'
  }
}

$buildExe = Join-Path $repo 'build\buffrx-engine.exe'
$stagedExe = Join-Path $repo 'runtime\bin\64bit\buffrx-engine.exe'
if (-not (Invoke-Step 'staged engine is the built engine' {
      if (-not (Test-Path -LiteralPath $buildExe) -or -not (Test-Path -LiteralPath $stagedExe)) {
        throw 'build\buffrx-engine.exe or runtime\bin\64bit\buffrx-engine.exe is missing'
      }
      $a = (Get-FileHash -LiteralPath $buildExe -Algorithm SHA256).Hash
      $b = (Get-FileHash -LiteralPath $stagedExe -Algorithm SHA256).Hash
      if ($a -ne $b) { throw 'runtime\bin\64bit\buffrx-engine.exe is not the build output; run the build again' }
    })) {
  Write-Host 'FAILED: the staged binary is stale.'
  exit 1
}

$testRoot = Join-Path $env:TEMP 'buffrx-preflight-tests'
if (-not (Invoke-Step 'unit checks' {
      Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
      New-Item -ItemType Directory -Force -Path $testRoot | Out-Null
      $env:BUFFRX_DATA_ROOT = $testRoot
      try {
        & (Join-Path $repo 'build\buffrx-tests.exe')
      } finally {
        Remove-Item Env:\BUFFRX_DATA_ROOT -ErrorAction SilentlyContinue
        if (-not $KeepData) { Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue }
      }
    })) {
  # A unit-check failure is already named in its own output; the gate cannot make it more true.
  Write-Host 'FAILED: unit checks.'
  exit 1
}

if (-not (Invoke-Step 'ui typecheck' { & npm --prefix ui run typecheck })) {
  Write-Host 'FAILED: ui typecheck.'
  exit 1
}
if (-not (Invoke-Step 'ui tests' { & npm --prefix ui test })) {
  Write-Host 'FAILED: ui tests.'
  exit 1
}
if (-not (Invoke-Step 'ui build' { & npm --prefix ui run build })) {
  Write-Host 'FAILED: ui build.'
  exit 1
}
if (-not (Invoke-Step 'fuzz' { & (Join-Path $repo 'build\buffrx-fuzz.exe') 60 })) {
  Write-Host 'FAILED: fuzz.'
  exit 1
}

Invoke-Tool 'capture integrity' 'capture-integrity.ps1'
Invoke-Tool 'acceptance' 'acceptance.ps1'

if (-not $Quick) {
  # Both probes set timeline_minutes, and Timeline now writes only while a game runs. Without this a kill/restart
  # proves the index of recordings and silently writes no session; the game gate is the only reason to fake one.
  $env:BUFFRX_TEST_TIMELINE_GAME = '1'
  try {
    Invoke-Tool 'chaos restart' 'chaos-restart.ps1' @('-Cycles', '3', '-BufferSeconds', '10')
  } finally {
    Remove-Item Env:\BUFFRX_TEST_TIMELINE_GAME -ErrorAction SilentlyContinue
  }
  Invoke-Tool 'pipe fuzz' 'pipe-fuzz.ps1' @('-Minutes', '1')
  Invoke-Tool 'settings churn' 'settings-churn.ps1' @('-Minutes', '3', '-Motion')
  $env:BUFFRX_TEST_TIMELINE_GAME = '1'
  try {
    Invoke-Tool 'soak' 'soak-probe.ps1' @('-Minutes', '3', '-Every', '15', '-Cycle', '20', '-Motion', '-Timeline')
  } finally {
    Remove-Item Env:\BUFFRX_TEST_TIMELINE_GAME -ErrorAction SilentlyContinue
  }
}

$clock.Stop()
Write-Host ''
if ($failures.Count -gt 0) {
  Write-Host ('PREFLIGHT FAILED in {0:N1} min' -f ($clock.Elapsed.TotalMinutes))
  foreach ($f in $failures) { Write-Host ('  ' + $f) }
  exit 1
}
Write-Host ('PREFLIGHT PASS in {0:N1} min' -f ($clock.Elapsed.TotalMinutes))
exit 0
