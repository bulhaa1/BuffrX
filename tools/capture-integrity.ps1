<#
.SYNOPSIS
  Hands-free regression gate for display capture: a saved clip must be the size of the display and must carry a real picture.

.DESCRIPTION
  Guards the 2026-11-06 regression, where an automatic WGC fallback replaced a still-desktop DXGI source with a WGC source that
  reported 2560x1440 and delivered no frames: the clips saved were 1440x1008 black at ~37 kbps. The root fix is in 8015cf4
  (`win32_target_size` no longer sizes a display capture from a stale window id).

  Every case starts its own engine on its own data root and pipe, with clips and screenshots under a temp folder, so the person's
  library is never touched; only the processes started here are stopped. Display capture is DXGI duplication, so the screen must
  change: a `buffrx-present-loop.exe --motion 1` window is started for the run, and its window supplies the foreign window id of
  case 2. Case 4 then stops that window, because a still screen is what used to make the engine swap the DXGI source for WGC.

  Exit code 0 when every assertion passes, 1 when any fails (each failure is named and printed).

.PARAMETER Engine
  The engine exe to test. Default: <repo>\runtime\bin\64bit\buffrx-engine.exe. Its runtime root is taken as the two folders above
  it, so a copy of the staged runtime can be tested instead (the staged one must not be locked during a build).

.PARAMETER Cases
  'all' (default) or the numbers to run, e.g. '1,3'.
    1  display 120 fps, fresh config (no window id)
    2  display 120 fps with a FOREIGN stale window_id/window_name (a real window that is not the target)
    3  display 60 fps
    4  display, live first and then a still picture after the motion window stops: nothing may fall back to WGC

.PARAMETER ExpectWidth
  The display's width, and so the width every clip must have. Default 2560.

.PARAMETER ExpectHeight
  The display's height. Default 1440.

.PARAMETER KeepData
  Keep each case's data root (config, engine log, clips) for inspection: %TEMP%\buffrx-capture-gate-*.

.EXAMPLE
  powershell -File tools\capture-integrity.ps1
  powershell -File tools\capture-integrity.ps1 -Cases 1,2
  powershell -File tools\capture-integrity.ps1 -Cases 4 -ExpectWidth 1920 -ExpectHeight 1080
  powershell -File tools\capture-integrity.ps1 -Engine "$env:TEMP\opencode\hard-capture-runtime\bin\64bit\buffrx-engine.exe"
#>
param(
  [string]$Engine = '',
  [string]$Cases = 'all',
  [int]$ExpectWidth = 2560,
  [int]$ExpectHeight = 1440,
  [switch]$KeepData
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $Engine) { $Engine = Join-Path $repo 'runtime\bin\64bit\buffrx-engine.exe' }
$Engine = (Resolve-Path -LiteralPath $Engine).Path
# <runtime>\bin\64bit\buffrx-engine.exe: the engine wants BUFFRX_RUNTIME_ROOT, and obs.dll must sit beside the exe.
$runtimeRoot = Split-Path (Split-Path (Split-Path $Engine))
if (-not (Test-Path -LiteralPath (Join-Path $runtimeRoot 'bin\64bit\obs.dll'))) {
  throw "no obs.dll beside $Engine (its runtime root is not the two folders above it): pass -Engine <runtime>\bin\64bit\buffrx-engine.exe"
}

# The pipe client and its one-command helper, from the shared probe library. It starts nothing by itself.
. (Join-Path $PSScriptRoot 'probe-lib.ps1')

$wanted = if ($Cases -eq 'all') { @(1, 2, 3, 4) } else { @($Cases.Split(',') | ForEach-Object { [int]$_.Trim() }) }
$unknown = @($wanted | Where-Object { @(1, 2, 3, 4) -notcontains $_ })
if ($unknown.Count -gt 0) { throw ("unknown case(s): " + ($unknown -join ',') + " (the cases are 1, 2, 3, 4)") }
# Case 4 stops the motion window, so nothing that needs a moving picture can run after it.
if ($wanted -contains 4) { $wanted = @($wanted | Where-Object { $_ -ne 4 }) + @(4) }

$logForbidden = @('trying Windows Graphics Capture', 'display capture fallback')
# Built from the size parameters: these are the two info lines the engine prints at start, and the size every clip must be.
$canvasLine = 'canvas plan: target ' + $ExpectWidth + 'x' + $ExpectHeight
$videoLine = 'video initialised: canvas ' + $ExpectWidth + 'x' + $ExpectHeight

$failures = New-Object System.Collections.Generic.List[string]
$motion = $null

function Write-Row([string]$text) { Write-Host $text }

# The changing picture every case needs: DXGI duplication hands over a frame only when the screen changes.
function Start-Motion {
  $loop = Join-Path $repo 'build\buffrx-present-loop.exe'
  if (-not (Test-Path -LiteralPath $loop)) {
    throw "no $loop; build it (tools\build.ps1 -Target buffrx-present-loop) or keep tools\screen-noise.ps1 running yourself"
  }
  $p = Start-Process -FilePath $loop -ArgumentList @('--seconds', '400', '--width', '1280', '--height', '720', '--motion', '1', '--fullscreen', '1') -PassThru
  Start-Sleep -Seconds 2
  $p
}

function Start-CaseEngine([string]$dataRoot, [string]$configJson) {
  New-Item -ItemType Directory -Force (Join-Path $dataRoot 'clips') | Out-Null
  New-Item -ItemType Directory -Force (Join-Path $dataRoot 'shots') | Out-Null
  New-Item -ItemType Directory -Force (Join-Path $dataRoot 'logs') | Out-Null
  [IO.File]::WriteAllText((Join-Path $dataRoot 'config.json'), $configJson, (New-Object Text.UTF8Encoding($false)))
  $pipe = 'buffrx-gate-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
  $env:BUFFRX_DATA_ROOT = $dataRoot
  $env:BUFFRX_PIPE = $pipe
  $env:BUFFRX_RUNTIME_ROOT = $runtimeRoot
  try {
    # The log file keeps failures only; every info line (the canvas plan, the source that was chosen) goes to stderr, so a
    # redirected stderr file is the only place the gate can read them.
    $proc = Start-Process -FilePath $Engine -WorkingDirectory (Join-Path $runtimeRoot 'bin\64bit') -PassThru -WindowStyle Hidden `
      -RedirectStandardError (Join-Path $dataRoot 'logs\stderr.log') -RedirectStandardOutput (Join-Path $dataRoot 'logs\stdout.log')
  } finally {
    Remove-Item Env:\BUFFRX_DATA_ROOT, Env:\BUFFRX_PIPE, Env:\BUFFRX_RUNTIME_ROOT -ErrorAction SilentlyContinue
  }
  $e = [pscustomobject]@{ Process = $proc; DataRoot = $dataRoot; Pipe = $pipe }
  for ($i = 0; $i -lt 40; $i++) {
    Start-Sleep -Milliseconds 500
    if ($e.Process.HasExited) { throw "the engine exited at start (code $($e.Process.ExitCode)); log: $(Join-Path $dataRoot 'logs\engine.log')" }
    try { (New-Object PanelPipe($pipe, 500)).Dispose(); return $e } catch { }
  }
  throw 'the engine did not answer on its pipe'
}

function Stop-CaseEngine($e) {
  if ($e -and $e.Process -and -not $e.Process.HasExited) {
    Stop-Process -Id $e.Process.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
  }
}

# Waits until the buffer reports on with a frame from the capture source.
function Wait-Capture($e) {
  for ($i = 0; $i -lt 60; $i++) {
    $s = Ask-Engine $e 'status.get'
    if ($s -match '"buffer_state":\s*"on"' -and $s -match '"capture_ready":\s*true') { return $s }
    Start-Sleep -Milliseconds 500
  }
  throw 'the buffer did not come on with a capture frame (is the picture changing?)'
}

# Waits until the ring holds enough footage for the clip asked for, and returns the newest status.
function Wait-Ring($e, [double]$Seconds) {
  $held = 0.0
  for ($i = 0; $i -lt 60; $i++) {
    $s = Ask-Engine $e 'status.get'
    $m = [regex]::Match($s, '"held_seconds":\s*([0-9.]+)')
    if ($m.Success) { $held = [double]$m.Groups[1].Value }
    if ($held -ge $Seconds) { return $s }
    Start-Sleep -Milliseconds 500
  }
  throw "the ring never held $Seconds s of footage (it held $held s): the capture is not delivering frames"
}

# Settings come back on stdout as one JSON object; retried while the muxer or the engine still has the file open.
# The exe is the whole engine, so it is given a data root of its own: otherwise it writes its log into the person's profile.
function Get-MediaInfo([string]$file, [string]$dataRoot) {
  for ($i = 0; $i -lt 20; $i++) {
    $env:BUFFRX_DATA_ROOT = $dataRoot
    $env:BUFFRX_PIPE = 'buffrx-gate-media'
    $env:BUFFRX_RUNTIME_ROOT = $runtimeRoot
    try {
      # Windows PowerShell turns a native command's stderr into an error record, and $ErrorActionPreference = 'Stop' would
      # make the engine's own startup line a terminating error: suppressed and softer here.
      $soft = $ErrorActionPreference
      $ErrorActionPreference = 'Continue'
      try { $out = (& $Engine --media-info $file 2>$null | Out-String) } finally { $ErrorActionPreference = $soft }
    } finally {
      Remove-Item Env:\BUFFRX_DATA_ROOT, Env:\BUFFRX_PIPE, Env:\BUFFRX_RUNTIME_ROOT -ErrorAction SilentlyContinue
    }
    $line = ($out -split "`r?`n" | Where-Object { $_.TrimStart().StartsWith('{') } | Select-Object -First 1)
    if ($line) {
      try {
        $info = $line | ConvertFrom-Json
        if ($info.ok) { return $info }
      } catch { }
    }
    Start-Sleep -Milliseconds 500
  }
  $null
}

# The library list is where a finished clip lands: the engine files it into a target-named folder, so the path it reports is the truth.
function Get-ClipFiles($e) {
  $reply = Ask-Engine $e 'clips.list'
  $rows = ($reply | ConvertFrom-Json).data
  @($rows | ForEach-Object { $_.file })
}

function Invoke-CaptureCase([int]$Number, [string]$Label, [int]$Fps, [string]$ForeignWindowId, [string]$ForeignWindowName,
  [int]$ClipSeconds = 6, [double]$RingSeconds = 8, [int]$MinKbps = 200, [switch]$Still) {
  $dataRoot = Join-Path $env:TEMP ('buffrx-capture-gate-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
  $config = [ordered]@{
    capture_mode    = 'display'
    capture_fps     = $Fps
    buffer_seconds  = 20
    start_buffering = $true
    auto_game_capture = $false
    clips_dir       = ((Join-Path $dataRoot 'clips') -replace '\\', '/')
    screenshots_dir = ((Join-Path $dataRoot 'shots') -replace '\\', '/')
  }
  # Case 2's whole point: a window id that belongs to another, real window, left over from an earlier window capture.
  if ($ForeignWindowId) {
    $config.window_id = $ForeignWindowId
    $config.window_name = $ForeignWindowName
  }

  $e = $null
  $width = 0; $height = 0; $duration = 0.0; $kbps = 0; $file = $null
  $problems = New-Object System.Collections.Generic.List[string]
  try {
    $e = Start-CaseEngine $dataRoot ($config | ConvertTo-Json -Compress)
    $status = Wait-Capture $e

    # The size the engine sized the capture for, before any clip exists.
    $m = [regex]::Match($status, '"capture":\s*\{[^}]*"width":\s*(-?\d+)'); if ($m.Success) { $width = [int]$m.Groups[1].Value }
    $m = [regex]::Match($status, '"capture":\s*\{[^}]*"height":\s*(-?\d+)'); if ($m.Success) { $height = [int]$m.Groups[1].Value }
    if ($width -ne $ExpectWidth -or $height -ne $ExpectHeight) {
      $problems.Add("capture is ${width}x${height}, expected ${ExpectWidth}x${ExpectHeight}")
    }

    $null = Wait-Ring $e $RingSeconds

    # Case 4: the capture is proven live above; now the screen goes still, which is the state that used to make the engine swap
    # the DXGI source for WGC after 4 s and save black frames from it.
    if ($Still) {
      if ($motion -and -not $motion.HasExited) {
        Stop-Process -Id $motion.Id -Force -ErrorAction SilentlyContinue
        for ($i = 0; $i -lt 20 -and -not $motion.HasExited; $i++) { Start-Sleep -Milliseconds 250; $motion.Refresh() }
      }
      Start-Sleep -Seconds 6      # the old build's fallback fired 4 s into a still picture
    }

    $before = Get-ClipFiles $e
    $reply = Ask-Engine $e 'buffer.save' (@{ seconds = $ClipSeconds } | ConvertTo-Json -Compress)
    if ($reply -notmatch '"ok":\s*true') { $problems.Add("buffer.save refused: $reply") }

    # The muxer writes a working file first and the engine verifies the container before it files the clip into the library:
    # wait for the new card, then let its size settle before anything reads it.
    $file = $null
    for ($i = 0; $i -lt 90 -and -not $file; $i++) {
      Start-Sleep -Milliseconds 500
      $new = @(Get-ClipFiles $e | Where-Object { $before -notcontains $_ })
      if ($new.Count -eq 0) { continue }
      $candidate = $new[0]
      $size = (Get-Item -LiteralPath $candidate -ErrorAction SilentlyContinue).Length
      Start-Sleep -Milliseconds 700
      $settled = (Get-Item -LiteralPath $candidate -ErrorAction SilentlyContinue).Length
      if ($size -gt 0 -and $size -eq $settled) { $file = $candidate }
    }
    if (-not $file) {
      $problems.Add('no clip landed in the library within 45 s of buffer.save')
    } else {
      $info = Get-MediaInfo $file $dataRoot
      if (-not $info) {
        $problems.Add("--media-info could not read $([IO.Path]::GetFileName($file))")
        $width = 0; $height = 0
      } else {
        $width = [int]$info.width; $height = [int]$info.height
        $duration = [double]$info.duration; $kbps = [int]$info.video_kbps
        if ($width -ne $ExpectWidth -or $height -ne $ExpectHeight) {
          $problems.Add("clip is ${width}x${height}, expected ${ExpectWidth}x${ExpectHeight}")
        }
        # A still screen's last frames sit further apart, so the trim can span more time than was asked for (case 4).
        $slack = if ($Still) { 3.0 } else { 2.0 }
        if ($duration -lt ($ClipSeconds - 0.5) -or $duration -gt ($ClipSeconds + $slack)) {
          $problems.Add(('clip is {0:N2} s, expected {1:N1}-{2:N1} s' -f $duration, ($ClipSeconds - 0.5), ($ClipSeconds + $slack)))
        }
        # A floor is only right where the screen moves: a legitimately still picture is low-bitrate too (case 4).
        if ($MinKbps -gt 0 -and $kbps -lt $MinKbps) {
          $problems.Add("clip is $kbps kbps, expected at least $MinKbps (a black picture)")
        }
      }
    }

    # The info lines name the canvas and the source that was chosen; the failure record keeps every warning.
    $text = ''
    foreach ($name in @('stderr.log', 'engine.log')) {
      $p = Join-Path $dataRoot ('logs\' + $name)
      if (Test-Path -LiteralPath $p) { $text += (Get-Content -LiteralPath $p -Raw) }
    }
    if ($text.Contains($canvasLine) -eq $false) { $problems.Add('log has no "' + $canvasLine + '"') }
    if ($text.Contains($videoLine) -eq $false) { $problems.Add('log has no "' + $videoLine + '"') }
    if ($text -notmatch 'capture source monitor_capture') { $problems.Add('log: the display capture did not come from monitor_capture') }
    if ($text -match 'capture source (window_capture|game_capture)') { $problems.Add('log: a window/game source captured in display mode') }
    foreach ($phrase in $logForbidden) {
      if ($text.Contains($phrase)) { $problems.Add("log has '$phrase'") }
    }
    # Warnings that touch the capture, reported rather than asserted: the person reading the gate wants to see them.
    foreach ($line in ($text -split "`r?`n")) {
      if ($line -match '\[warn\]' -and $line -match 'capture|display|encoder') { Write-Row ("        warn      $line") }
    }
  } catch {
    $problems.Add($_.Exception.Message)
  } finally {
    Stop-CaseEngine $e
    if (-not $KeepData -and $dataRoot) { Remove-Item -LiteralPath $dataRoot -Recurse -Force -ErrorAction SilentlyContinue }
  }

  $ok = $problems.Count -eq 0
  $verdict = if ($ok) { 'PASS' } else { 'FAIL' }
  Write-Row ('{0}  case {1}  {2,-30} {3}x{4}  {5,5:N2}s  {6,6} kbps' -f $verdict, $Number, $Label, $width, $height, $duration, $kbps)
  foreach ($p in $problems) {
    Write-Row "        case $Number : $p"
    $failures.Add("case $Number : $p")
  }
  $ok
}

# A real window that is not the capture target, for case 2: its id is what a stale window capture would leave in the config.
function Get-ForeignWindow {
  $dataRoot = Join-Path $env:TEMP ('buffrx-capture-gate-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
  $config = [ordered]@{
    capture_mode    = 'display'
    capture_fps     = 60
    start_buffering = $false           # nothing is captured here: the window list is all this engine is for
    clips_dir       = ((Join-Path $dataRoot 'clips') -replace '\\', '/')
    screenshots_dir = ((Join-Path $dataRoot 'shots') -replace '\\', '/')
  }
  $e = $null
  try {
    $e = Start-CaseEngine $dataRoot ($config | ConvertTo-Json -Compress)
    $rows = ((Ask-Engine $e 'sources.windows') | ConvertFrom-Json).data
    # Matched on the executable: a window's title can be any text (a terminal shows the command line that started the loop).
    $target = $rows | Where-Object { $_.exe -like '*buffrx-present-loop*' } | Select-Object -First 1
    if (-not $target) { throw 'no buffrx-present-loop window to use as the foreign window id' }
    $target
  } finally {
    Stop-CaseEngine $e
    if (-not $KeepData -and $dataRoot) { Remove-Item -LiteralPath $dataRoot -Recurse -Force -ErrorAction SilentlyContinue }
  }
}

$clock = [Diagnostics.Stopwatch]::StartNew()
try {
  Write-Row "engine  $Engine"
  Write-Row ("cases   {0}" -f ($wanted -join ','))
  $motion = Start-Motion

  $foreign = $null
  if ($wanted -contains 2) {
    $foreign = Get-ForeignWindow
    Write-Row ("foreign window id  {0}" -f $foreign.id)
  }

  if ($wanted -contains 1) { $null = Invoke-CaptureCase 1 'display 120 fps, fresh config' 120 '' '' }
  if ($wanted -contains 2) { $null = Invoke-CaptureCase 2 'display 120 fps, stale window id' 120 $foreign.id $foreign.name }
  if ($wanted -contains 3) { $null = Invoke-CaptureCase 3 'display 60 fps' 60 '' '' }
  # Last, because it stops the motion window: the screen goes still, which is the state the old build fell back from.
  if ($wanted -contains 4) {
    $null = Invoke-CaptureCase 4 'display, then a still picture' 120 '' '' -ClipSeconds 3 -RingSeconds 8 -MinKbps 0 -Still
  }
} catch {
  $failures.Add('gate : ' + $_.Exception.Message)
  Write-Row ('FAIL  gate: ' + $_.Exception.Message)
} finally {
  # The motion window is this run's own process; a forced stop can fail once while it tears down its swap chain.
  if ($motion) {
    for ($i = 0; $i -lt 5; $i++) {
      $motion.Refresh()
      if ($motion.HasExited) { break }
      Stop-Process -Id $motion.Id -Force -ErrorAction SilentlyContinue
      Start-Sleep -Milliseconds 300
    }
  }
}

Write-Row ("{0}  {1:N1} s total" -f $(if ($failures.Count -eq 0) { 'ALL PASS' } else { 'FAILED' }), $clock.Elapsed.TotalSeconds)
if ($failures.Count -gt 0) {
  foreach ($f in $failures) { Write-Row "  $f" }
  exit 1
}
exit 0
