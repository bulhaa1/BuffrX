# What the engine costs a running game, on a GPU-bound stand-in: the frame rate and the 1% low of
# `buffrx-present-loop --load N` with the engine off and with it capturing the loop's window under each configuration,
# plus the engine's own counters and the smoothness of a clip saved from the run.
#
#   powershell -File tools/perf-overhead.ps1 -Rounds 2
#   powershell -File tools/perf-overhead.ps1 -Configs off,normal,normal+gpu -Rounds 1 -Seconds 30
#
# The loop covers a large part of the screen and the GPU is the thing being measured: do not use the PC while it runs.
# Every number it prints comes from the present loop, not from a game, and is labelled so.
#
# Configurations (-Configs, in this order, interleaved in every round so drift hits all of them):
#   off            no engine
#   normal         the engine as a user typically runs it: AV1, 120 fps, native size, balanced
#   normal+gpu / +eco / +below / +all     normal with BUFFRX_TUNE (process_tuning.h): gpu:-7, eco, below, all of them
#   perf           the Performance mode plan, as plain settings: H.264, 60 fps, 1080p box, fast tier, bilinear, 1 audio track
#   perf+all       perf with all BUFFRX_TUNE candidates
#   mode           normal settings with Performance mode on (the engine's own layer, at 60 fps); mode+canvas etc. add a BUFFRX_TUNE word
#   <config>+viewer  the configuration with the clip viewer open and playing (needs -ViewerClip)
#   p1080 / p60 / ph264     normal with only the size, only the rate, or only the codec and tier of the plan
param(
  [string[]]$Configs = @('off', 'normal', 'normal+gpu', 'normal+eco', 'normal+below', 'normal+all', 'perf', 'perf+all'),
  [int]$Rounds = 2,
  [int]$Load = 260,
  [int]$Width = 2400,
  [int]$Height = 1300,
  [int]$Settle = 10,
  [int]$Seconds = 30,
  [string]$Engine = '',
  [string]$OutDir = '',
  # A clip to put in each run's library so the viewer (Alt+4) can be opened mid-run: configurations ending in +viewer open it
  # after the settle and leave it playing, so the loop's frame rate says what an open viewer costs the game.
  [string]$ViewerClip = ''
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'probe-guard.ps1')
# powershell -File passes -Configs a,b as one string.
$Configs = @($Configs | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $Engine) { $Engine = Join-Path $root 'runtime\bin\64bit\buffrx-engine.exe' }
$loopExe = Join-Path $root 'build\buffrx-present-loop.exe'
if (-not (Test-Path $Engine)) { throw "engine not found: $Engine" }
if (-not (Test-Path $loopExe)) { throw "present loop not built: $loopExe" }
if (-not $OutDir) { $OutDir = Join-Path $env:TEMP 'buffrx-perf-overhead' }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$ask = Join-Path $root 'tools\engine-ask.ps1'
$engineDir = Split-Path $Engine

$windowId = 'BuffrX present loop:BuffrX.PresentLoop:buffrx-present-loop.exe'
$base = [ordered]@{
  capture_mode = 'window'; window_id = $windowId; window_name = 'present loop'; capture_cursor = $false
  start_buffering = $true; mic_enabled = $false; auto_game_capture = $false; buffer_seconds = 30
  desktop_enabled = $true; notifications = $false; hotkey_toggle_ui = ''
}
$profiles = @{
  normal = @{ capture_fps = 120; codec = 'av1'; quality = 'standard'; encoder_mode = 'balanced'; output_width = 0; output_height = 0; scale_filter = 'bicubic' }
  perf = @{ capture_fps = 60; codec = 'h264'; quality = 'standard'; encoder_mode = 'fast'; output_width = 1920; output_height = 1040; scale_filter = 'bilinear'; audio_tracks = 1; separate_tracks = $false; audio_bitrate_kbps = 128 }
}
# The Performance plan taken apart, to see which piece buys what: size alone, rate alone, codec and tier alone.
function Merge-Settings($base, $over) { $out = @{}; foreach ($k in $base.Keys) { $out[$k] = $base[$k] }; foreach ($k in $over.Keys) { $out[$k] = $over[$k] }; $out }
# The real thing: the user's normal settings with Performance mode on (the engine layers its plan over them).
$profiles['mode'] = Merge-Settings $profiles['normal'] @{ performance_when = 'always' }
$profiles['p1080'] = Merge-Settings $profiles['normal'] @{ output_width = 1920; output_height = 1040; scale_filter = 'bilinear' }
$profiles['p60'] = Merge-Settings $profiles['normal'] @{ capture_fps = 60 }
$profiles['ph264'] = Merge-Settings $profiles['normal'] @{ codec = 'h264'; encoder_mode = 'fast' }
$tunes = @{ gpu = 'gpu:-7'; eco = 'eco'; below = 'below'; all = 'gpu:-7,eco,below,io'; cpuall = 'eco,below' }

function Ask($name, $cmd, $arguments = '{}') {
  # Each run's engine has a pipe of its own: a BuffrX somebody is using never receives these commands.
  $env:BUFFRX_PIPE = "buffrx-perf-$name"
  try { (& $ask -Command $cmd -Arguments $arguments -TimeoutMs 30000 | ConvertFrom-Json) } catch { $null }
}

function Run-One([string]$config, [int]$round) {
  $label = ("$config-r$round") -replace '[^a-zA-Z0-9-]', '_'
  $env:BUFFRX_PIPE = "buffrx-perf-$label"
  $dataRoot = Join-Path $OutDir "data-$label"
  Assert-ProbeDataRoot $dataRoot | Out-Null
  if (Test-Path $dataRoot) { Remove-Item $dataRoot -Recurse -Force }
  New-Item -ItemType Directory -Force "$dataRoot\clips" | Out-Null
  $total = $Settle + $Seconds
  $loop = Start-Process $loopExe -ArgumentList @('--seconds', ($total + 6), '--width', $Width, '--height', $Height, '--vsync', 0, '--load', $Load, '--settle', $Settle) `
    -PassThru -RedirectStandardOutput (Join-Path $OutDir "loop-$label.txt")
  $engineProcess = $null
  $stats = $null
  $cpuPercent = $null
  $clip = $null
  if ($config -ne 'off') {
    $parts = $config.Split('+')
    $withViewer = $parts -contains 'viewer'
    $profile = $profiles[$parts[0]]
    $settings = [ordered]@{}
    foreach ($k in $base.Keys) { $settings[$k] = $base[$k] }
    foreach ($k in $profile.Keys) { $settings[$k] = $profile[$k] }
    $settings['clips_dir'] = "$dataRoot\clips"
    $settings['screenshots_dir'] = "$dataRoot\clips\shots"
    [System.IO.File]::WriteAllText("$dataRoot\config.json", ($settings | ConvertTo-Json), (New-Object System.Text.UTF8Encoding($false)))
    $env:BUFFRX_DATA_ROOT = $dataRoot
    $env:BUFFRX_NO_GOVERNOR = '1'
    $tuneWord = $parts | Where-Object { $_ -ne 'viewer' } | Select-Object -Skip 1 -First 1
    if ($tuneWord) { $env:BUFFRX_TUNE = $tunes[$tuneWord] } else { Remove-Item Env:BUFFRX_TUNE -ErrorAction SilentlyContinue }
    if ($withViewer) {
      if (-not $ViewerClip -or -not (Test-Path $ViewerClip)) { throw '-ViewerClip is needed for a +viewer configuration' }
      New-Item -ItemType Directory -Force "$dataRoot\clips\Viewer" | Out-Null
      Copy-Item $ViewerClip "$dataRoot\clips\Viewer\Replay 2026-01-01 00-00-00.mp4"
    }
    $engineProcess = Start-Process $Engine -PassThru -WindowStyle Hidden -WorkingDirectory $engineDir `
      -RedirectStandardError (Join-Path $OutDir "engine-$label.log") -RedirectStandardOutput (Join-Path $OutDir "engine-out-$label.log")
    Remove-Item Env:BUFFRX_TUNE -ErrorAction SilentlyContinue
    Remove-Item Env:BUFFRX_NO_GOVERNOR -ErrorAction SilentlyContinue
  }
  # the settle period, then the engine's CPU over the measured part, and its counters just before the loop ends
  Start-Sleep -Seconds $Settle
  if ($engineProcess -and $withViewer) { $null = Ask $label 'viewer.open' }
  $cpuStart = $null
  if ($engineProcess) { $engineProcess.Refresh(); $cpuStart = $engineProcess.TotalProcessorTime.TotalMilliseconds }
  Start-Sleep -Seconds ($Seconds - 3)
  if ($engineProcess) {
    $engineProcess.Refresh()
    $cpuPercent = ($engineProcess.TotalProcessorTime.TotalMilliseconds - $cpuStart) / (($Seconds - 3) * 10.0)
    $status = Ask $label 'status.get'
    $stats = $status.data.stats
    if ($stats) { $stats | Add-Member -NotePropertyName working_set_mb -NotePropertyValue ([math]::Round($engineProcess.WorkingSet64 / 1MB, 0)) -Force }
    $null = Ask $label 'buffer.save' '{"seconds":10}'
  }
  $null = $loop.WaitForExit(($total + 20) * 1000)
  if ($engineProcess) {
    Start-Sleep -Seconds 3
    $null = Ask $label 'buffer.stop'
    Start-Sleep -Seconds 3
    Stop-Process -Id $engineProcess.Id -Force -ErrorAction SilentlyContinue
    $file = Get-ChildItem "$dataRoot\clips" -Recurse -Filter '*.mp4' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($file) {
      $previous = $ErrorActionPreference
      $ErrorActionPreference = 'Continue'  # the tool writes its log to stderr
      try { $clip = (& $Engine --media-frames $file.FullName 8 2>$null | Out-String | ConvertFrom-Json) } catch { $clip = $null }
      $ErrorActionPreference = $previous
    }
  }
  if (-not $loop.HasExited) { Stop-Process -Id $loop.Id -Force }
  $line = (Get-Content (Join-Path $OutDir "loop-$label.txt") | Select-String 'PRESENT_LOOP ' | Select-Object -Last 1).Line
  $fps = if ($line -match 'fps_avg=([\d.]+)') { [double]$Matches[1] } else { $null }
  $low = if ($line -match 'fps_1pct_low=([\d.]+)') { [double]$Matches[1] } else { $null }
  [pscustomobject]@{
    config = $config; round = $round; fps = $fps; low = $low
    cpu = if ($null -ne $cpuPercent) { [math]::Round($cpuPercent, 1) } else { $null }
    ws_mb = if ($stats) { $stats.working_set_mb } else { $null }
    dropped = if ($stats) { $stats.dropped } else { $null }
    lagged = if ($stats) { "$($stats.video_lagged)/$($stats.video_frames)" } else { $null }
    enc_ratio = if ($stats) { [math]::Round([double]$stats.encoder_ratio, 3) } else { $null }
    clip_gaps = if ($clip) { $clip.gaps } else { $null }
    clip_repeat_pct = if ($clip) { $clip.repeat_percent } else { $null }
    clip_min_fps = if ($clip) { $clip.fps_min_second } else { $null }
  }
}

$results = @()
for ($round = 1; $round -le $Rounds; $round++) {
  foreach ($config in $Configs) {
    Write-Host ("round {0}: {1}" -f $round, $config)
    $results += Run-One $config $round
  }
}
$results | Export-Csv -NoTypeInformation (Join-Path $OutDir 'results.csv')

"Present-loop stand-in (GPU-bound, --load $Load, ${Width}x${Height} window), not a game. Capture: window (WGC). Settle $Settle s, measured $Seconds s."
$offFps = ($results | Where-Object config -eq 'off' | Measure-Object fps -Average).Average
$summary = foreach ($group in ($results | Group-Object config)) {
  $fpsValues = @($group.Group | ForEach-Object { $_.fps })
  $lowValues = @($group.Group | ForEach-Object { $_.low })
  $avg = ($fpsValues | Measure-Object -Average).Average
  [pscustomobject]@{
    config = $group.Name
    fps = ('{0:N1}-{1:N1}' -f ($fpsValues | Measure-Object -Minimum).Minimum, ($fpsValues | Measure-Object -Maximum).Maximum)
    low_1pct = ('{0:N1}-{1:N1}' -f ($lowValues | Measure-Object -Minimum).Minimum, ($lowValues | Measure-Object -Maximum).Maximum)
    loss_vs_off = if ($offFps) { ('{0:N2}%' -f (($offFps - $avg) / $offFps * 100)) } else { 'n/a' }
    cpu_pct = (($group.Group | ForEach-Object { $_.cpu }) -join '/')
    dropped = (($group.Group | ForEach-Object { $_.dropped }) -join '/')
    lagged = (($group.Group | ForEach-Object { $_.lagged }) -join ' ')
    enc_ratio = (($group.Group | ForEach-Object { $_.enc_ratio }) -join '/')
    clip_gaps = (($group.Group | ForEach-Object { $_.clip_gaps }) -join '/')
    clip_repeat_pct = (($group.Group | ForEach-Object { $_.clip_repeat_pct }) -join '/')
    ws_mb = (($group.Group | ForEach-Object { $_.ws_mb }) -join '/')
  }
}
$summary | Format-Table -AutoSize -Wrap | Out-String -Width 250
"raw results: $(Join-Path $OutDir 'results.csv')"
