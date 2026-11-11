# Does window capture of the present loop deliver frames? One throwaway engine, no display capture.
[CmdletBinding()]
param([string]$Engine = 'buffrx-engine-night.exe', [switch]$KeepData)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'probe-lib.ps1')

$loop = Start-Process -FilePath (Join-Path $ProbeRoot 'build\buffrx-present-loop.exe') -ArgumentList '--seconds','3600','--width','1280','--height','720','--motion','1' -PassThru -WindowStyle Normal
Write-Output ("present-loop PID {0}" -f $loop.Id)
Start-Sleep -Seconds 2

$config = '{"config_version":7,"start_buffering":false,"auto_game_capture":false,"capture_mode":"display","codec":"h264","capture_fps":30,"output_width":1280,"output_height":720,"buffer_seconds":10}'
$e = $null
try {
  $e = Start-ProbeEngine $config $Engine @{}
  Write-Output ("engine PID {0} root {1}" -f $e.Process.Id, $e.DataRoot)
  $windows = ((Ask-Engine $e 'sources.windows') | ConvertFrom-Json).data
  $target = ($windows | Where-Object { $_.exe -like '*present-loop*' } | Select-Object -First 1)
  if (-not $target) { Write-Output 'NO PRESENT-LOOP WINDOW'; $windows | ForEach-Object { "  {0} | {1} | {2}" -f $_.id, $_.exe, $_.name }; exit 2 }
  Write-Output ("capture target: {0} | {1} | {2}" -f $target.id, $target.exe, $target.name)
  $set = (Ask-Engine $e 'capture.set' (@{ mode = 'window'; window_id = $target.id } | ConvertTo-Json -Compress) | ConvertFrom-Json).data
  Write-Output ("capture.set -> mode {0} error {1}" -f $set.capture.mode, $set.last_error)
  $start = Ask-Engine $e 'buffer.start' '{}'
  $ok = $false
  for ($i = 0; $i -lt 60; $i++) {
    Start-Sleep -Milliseconds 500
    $s = (Ask-Engine $e 'status.get' '{}' | ConvertFrom-Json).data
    if ($s.buffering -and $s.capture_ready) { $ok = $true; break }
  }
  $s = (Ask-Engine $e 'status.get' '{}' | ConvertFrom-Json).data
  Write-Output ("buffering {0} capture_ready {1} size {2}x{3} last_error {4}" -f $s.buffering, $s.capture_ready, $s.capture.width, $s.capture.height, $s.last_error)
  if (-not $ok) { Write-Output 'WINDOW CAPTURE DELIVERED NOTHING'; exit 3 }
  Write-Output 'WINDOW CAPTURE OK'
} finally {
  if ($e) { Stop-ProbeEngine $e -KeepData:$KeepData }
  Stop-Process -Id $loop.Id -Force -ErrorAction SilentlyContinue
}
