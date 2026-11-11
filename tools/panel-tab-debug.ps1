param([string]$Engine = 'buffrx-engine.exe')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'probe-lib.ps1')
$root = Join-Path $env:TEMP ('buffrx-tab-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $root -Force | Out-Null
$shots = Join-Path $env:TEMP 'opencode\tab'
New-Item -ItemType Directory -Path $shots -Force | Out-Null
$config = @{ config_version = 7; start_buffering = $false; auto_game_capture = $false; notifications = $false
  min_free_gb = 1; buffer_seconds = 10; capture_fps = 60; codec = 'h264'; container = 'mp4'; audio_tracks = 1
  quality = 'standard'; codec_auto_picked = $true; timeline_minutes = 0 } | ConvertTo-Json
$e = Start-ProbeEngine -ConfigJson $config -Engine $Engine -DataRoot $root
try {
  [void](Ask-Engine $e 'ui.show' '{"view":"main"}')
  Start-Sleep -Seconds 2
  for ($i = 0; $i -lt 4; $i++) {
    [void](Ask-Engine $e 'ui.input' '{"window":"main","keys":["Tab"]}')
    Start-Sleep -Milliseconds 900
    $file = Join-Path $shots ("tab{0}.png" -f $i)
    if (Test-Path $file) { Remove-Item $file -Force }
    [void](Ask-Engine $e 'ui.capture' ("{`"view`":`"current`",`"file`":`"$($file.Replace('\','\\'))`"}"))
    Write-Host ("tab{0}: {1}" -f $i, (Get-FileHash $file).Hash.Substring(0,12))
  }
  $ui = Join-Path $root 'ui.json'
  if (Test-Path $ui) { Get-Content $ui -Raw | Select-Object -First 1 }
}
finally { Stop-ProbeEngine $e -KeepData }
