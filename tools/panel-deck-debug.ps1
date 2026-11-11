param([switch]$KeepData)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'probe-lib.ps1')
$cfg = @{ config_version = 7; start_buffering = $false; auto_game_capture = $false; notifications = $false
  notification_sound = $false; min_free_gb = 1; buffer_seconds = 15; capture_fps = 60; codec = 'h264'
  container = 'mp4'; quality = 'standard'; audio_tracks = 1; timeline_minutes = 0; codec_auto_picked = $true } | ConvertTo-Json -Compress
$loop = Start-Process -FilePath (Join-Path $script:ProbeRoot 'build\buffrx-present-loop.exe') -ArgumentList '--seconds','900','--width','1280','--height','720','--motion','1' -PassThru
$noise = Start-Process -FilePath 'powershell' -ArgumentList '-NoProfile','-File',(Join-Path $script:ProbeRoot 'tools\screen-noise.ps1') -PassThru
Start-Sleep -Seconds 2
$e = Start-ProbeEngine -ConfigJson $cfg
try {
  $dir = Join-Path $env:TEMP 'opencode\deck'
  New-Item -ItemType Directory -Path $dir -Force | Out-Null
  function Shot($name) {
    $f = Join-Path $dir ($name + '.png')
    [void](Ask-Engine $e 'ui.capture' ("{`"view`":`"current`",`"file`":`"$($f.Replace('\','\\'))`"}"))
    Write-Host "$name -> $((Get-Item $f -EA 0).Length) bytes"
  }
  [void](Ask-Engine $e 'ui.show' '{"view":"main"}')
  [void](Ask-Engine $e 'buffer.start' '{}')
  Wait-Buffering $e
  Start-Sleep -Seconds 4
  Write-Host ('clips before: ' + (Ask-Engine $e 'clips.list'))
  Shot 'buf-before'
  Write-Host ('enter: ' + (Ask-Engine $e 'ui.input' '{"window":"main","keys":["Enter"]}'))
  Start-Sleep -Seconds 2
  Shot 'buf-enter'
  Write-Host ('clips after enter: ' + (Ask-Engine $e 'clips.list'))
  Write-Host ('down+enter: ' + (Ask-Engine $e 'ui.input' '{"window":"main","keys":["Down","Enter"]}'))
  Start-Sleep -Seconds 2
  Shot 'buf-down-enter'
  Write-Host ('click test: ' + (Ask-Engine $e 'ui.input' '{"window":"main","click":[200,50]}'))
  Start-Sleep -Seconds 2
  Shot 'buf-click'
  Write-Host ('status: ' + (Ask-Engine $e 'status.get'))
} finally {
  Stop-ProbeEngine $e -KeepData:$KeepData
  Stop-Process -Id $loop.Id -Force -ErrorAction SilentlyContinue
  Stop-Process -Id $noise.Id -Force -ErrorAction SilentlyContinue
}
