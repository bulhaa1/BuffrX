# One settings change on a buffering window capture, then the engine's own answer: does the capture come back?
# N8 found this: after a resolution change the churn engine stayed at "waiting for the capture source to produce frames"
# for the rest of the run. This isolates one change per fresh engine so the culprit and the recovery are visible.
# Usage: powershell -File tools\win-capture-resize-repro.ps1 -Change '{"output_width":2560,"output_height":1440}'
param(
  [Parameter(Mandatory = $true)][string]$Change,
  [string]$Engine = 'buffrx-engine.exe',
  [int]$WaitSeconds = 25,
  [switch]$KeepData
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'probe-lib.ps1')

$loop = $null
$e = $null
try {
  $loop = Start-Process -FilePath (Join-Path $ProbeRoot 'build\buffrx-present-loop.exe') -PassThru `
    -ArgumentList '--seconds', '600', '--width', '1280', '--height', '720', '--motion', '1'
  Start-Sleep -Seconds 2
  "motion PID $($loop.Id)"

  $config = '{"config_version":7,"start_buffering":false,"auto_game_capture":false,"timeline_minutes":0,"buffer_seconds":10,"codec":"h264","capture_fps":30,"output_width":1280,"output_height":720,"quality":"standard","peak_mode":"fixed","custom_kbps":12000}'
  $e = Start-ProbeEngine $config $Engine
  "engine PID $($e.Process.Id) root $($e.DataRoot)"
  Wait-Asleep $e
  $captured = Set-WindowCapture $e
  "capturing: $captured"
  $null = Ask-Engine $e 'buffer.start'
  Wait-Buffering $e
  $s = (Ask-Engine $e 'status.get' | ConvertFrom-Json).data
  "before: buffering $($s.buffering) capture_ready $($s.capture_ready) last_error '$($s.last_error)'"

  $null = Ask-Engine $e 'config.set' ('{"patch":' + $Change + '}')
  "applied: $Change"

  $recoveredAt = -1
  for ($i = 0; $i -le $WaitSeconds; $i++) {
    Start-Sleep -Seconds 1
    $s = (Ask-Engine $e 'status.get' | ConvertFrom-Json).data
    $ready = $false
    if ($s.capture) { $ready = [bool]$s.capture_ready }
    "t+{0,2}s buffering {1} capture {2} ready {3} last_error '{4}'" -f $i, $s.buffering, "$($s.capture.width)x$($s.capture.height)", $ready, $s.last_error
    if ($ready -and $recoveredAt -lt 0) { $recoveredAt = $i; break }
  }
  if ($recoveredAt -ge 0) {
    "capture came back after $recoveredAt s"
  } else {
    "capture did NOT come back within $WaitSeconds s"
    # Does a stop/start of the buffer bring the same engine back? If it does, the source is alive and only
    # the start order is wrong; if it does not, the source itself stopped delivering.
    $null = Ask-Engine $e 'buffer.stop'
    Start-Sleep -Seconds 2
    $null = Ask-Engine $e 'buffer.start'
    $back = -1
    for ($i = 0; $i -le 20; $i++) {
      Start-Sleep -Seconds 1
      $s = (Ask-Engine $e 'status.get' | ConvertFrom-Json).data
      $ready = $false
      if ($s.capture) { $ready = [bool]$s.capture_ready }
      "restart t+{0,2}s buffering {1} capture {2} ready {3} last_error '{4}'" -f $i, $s.buffering, "$($s.capture.width)x$($s.capture.height)", $ready, $s.last_error
      if ($ready) { $back = $i; break }
    }
    if ($back -ge 0) {
      "capture came back only after a buffer stop/start, at $back s"
      exit 2
    }
    "capture did not come back after a buffer stop/start either"
    # Does the same engine recover when the setting is put back?
    $null = Ask-Engine $e 'config.set' '{"patch":{"output_width":1280,"output_height":720,"capture_fps":30,"performance_when":"off"}}'
    for ($i = 0; $i -le 20; $i++) {
      Start-Sleep -Seconds 1
      $s = (Ask-Engine $e 'status.get' | ConvertFrom-Json).data
      $ready = $false
      if ($s.capture) { $ready = [bool]$s.capture_ready }
      "back t+{0,2}s capture {1} ready {2} last_error '{3}'" -f $i, "$($s.capture.width)x$($s.capture.height)", $ready, $s.last_error
      if ($ready) { "recovered only after the setting was put back, at $i s"; break }
    }
    exit 1
  }
} finally {
  if ($e) { Stop-ProbeEngine $e -KeepData:$KeepData }
  if ($loop) { Stop-Process -Id $loop.Id -Force -ErrorAction SilentlyContinue }
}
