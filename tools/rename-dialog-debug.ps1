# Rename dialog: does Enter close it? Seeds one clip, opens the rail, F2, types, Enter, then captures
# the panel every second for five seconds so a lingering dialog is visible.
param(
  [string]$Engine = 'buffrx-engine.exe',
  [switch]$KeepData
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'probe-lib.ps1')

function Reply($cmd, $args = '{}') { return (Ask-Engine $e $cmd $args) | ConvertFrom-Json }
function Shot($name) {
  $file = Join-Path $env:TEMP "opencode\rename\$name.png"
  $dir = Split-Path $file
  if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
  $reply = Ask-Engine $e 'ui.capture' ('{"view":"current","file":"' + $file.Replace('\', '\\') + '"}')
  return $file
}

Start-Process -FilePath 'powershell' -ArgumentList '-NoProfile', '-File', (Join-Path $script:ProbeRoot 'tools\screen-noise.ps1') -WindowStyle Hidden | Out-Null
Start-Sleep -Seconds 3
$config = @{
  config_version = 7; start_buffering = $false; auto_game_capture = $false; notifications = $false
  notification_sound = $false; min_free_gb = 1; buffer_seconds = 10; capture_fps = 60; codec = 'h264'
  container = 'mp4'; quality = 'standard'; audio_tracks = 1; timeline_minutes = 0; codec_auto_picked = $true
} | ConvertTo-Json -Compress
$e = Start-ProbeEngine -ConfigJson $config -Engine $Engine
try {
  [void](Ask-Engine $e 'buffer.start')
  Wait-Buffering $e
  Start-Sleep -Seconds 3
  [void](Ask-Engine $e 'buffer.save' '{"seconds":4}')
  Start-Sleep -Seconds 6
  $clips = @(((Ask-Engine $e 'clips.list') | ConvertFrom-Json).data)
  "clips: " + (($clips | ForEach-Object { $_.name }) -join ' | ')

  [void](Ask-Engine $e 'ui.show' '{"view":"main"}')
  Start-Sleep -Seconds 2
  foreach ($keys in @('["Tab"]', '["Tab"]', '["Right"]', '["F2"]')) {
    [void](Ask-Engine $e 'ui.input' ('{"window":"main","keys":' + $keys + '}'))
    Start-Sleep -Milliseconds 900
  }
  [void](Ask-Engine $e 'ui.input' '{"window":"main","keys":["A"],"mods":"ctrl"}')
  Start-Sleep -Milliseconds 400
  [void](Ask-Engine $e 'ui.input' '{"window":"main","text":"DIALOG TEST"}')
  Start-Sleep -Milliseconds 600
  "before enter: " + (Shot '0-before-enter')
  [void](Ask-Engine $e 'ui.input' '{"window":"main","keys":["Enter"]}')
  for ($i = 1; $i -le 5; $i++) {
    Start-Sleep -Seconds 1
    "after enter +${i}s: " + (Shot "after-enter-$i")
  }
  $clips = @(((Ask-Engine $e 'clips.list') | ConvertFrom-Json).data)
  "clips: " + (($clips | ForEach-Object { $_.name }) -join ' | ')
  # With the dialog gone, Delete must open the delete dialog and Enter must remove one clip.
  [void](Ask-Engine $e 'ui.input' '{"window":"main","keys":["Right"]}')
  Start-Sleep -Milliseconds 800
  [void](Ask-Engine $e 'ui.input' '{"window":"main","keys":["Delete"]}')
  Start-Sleep -Seconds 1
  "delete dialog: " + (Shot 'delete-dialog')
  [void](Ask-Engine $e 'ui.input' '{"window":"main","keys":["Enter"]}')
  Start-Sleep -Seconds 4
  $after = @(((Ask-Engine $e 'clips.list') | ConvertFrom-Json).data)
  "clips after delete: " + (($after | ForEach-Object { $_.name }) -join ' | ')
  "delete removed a clip: " + ($after.Count -eq $clips.Count - 1)
} finally {
  Stop-ProbeEngine $e -KeepData:$KeepData
  Get-Process screen-noise -EA 0 | Stop-Process -Force -EA 0
}

