# One scenario, printed step by step: does Enter confirm the delete dialog in the panel's library?
param([string]$Engine = 'buffrx-engine.exe', [switch]$KeepData)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'probe-lib.ps1')

$root = Join-Path $env:TEMP ('buffrx-dialog-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
$config = @{
  config_version = 7; start_buffering = $false; auto_game_capture = $false
  notifications = $false; notification_sound = $false; min_free_gb = 1
  buffer_seconds = 10; capture_fps = 60; codec = 'h264'; container = 'mp4'
  audio_tracks = 1; quality = 'standard'; codec_auto_picked = $true; timeline_minutes = 0
} | ConvertTo-Json
function Reply($raw) { try { return ($raw | ConvertFrom-Json).data } catch { return $null } }
function Clips($e) { return @(Reply (Ask-Engine $e 'clips.list')) }
function Keys($e, [string]$keys) { $r = Ask-Engine $e 'ui.input' ("{`"window`":`"main`",`"keys`":[$keys]}"); return $r }

$loop = $null; $e = $null
try {
  $loop = Start-Process -FilePath (Join-Path $PSScriptRoot '..\build\buffrx-present-loop.exe') `
    -ArgumentList '--seconds','600','--width','1280','--height','720','--motion','1' -PassThru
  Start-Sleep -Seconds 2
  $e = Start-ProbeEngine -ConfigJson $config -Engine $Engine -DataRoot $root
  [void](Ask-Engine $e 'ui.show' '{"view":"main"}')
  Start-Sleep -Seconds 1
  [void](Ask-Engine $e 'buffer.start' '{}')
  [void](Wait-Buffering $e)
  Start-Sleep -Seconds 3
  for ($i = 0; $i -lt 2; $i++) { [void](Ask-Engine $e 'buffer.save' '{"seconds":4}'); Start-Sleep -Seconds 4 }
  Write-Host ("clips: " + (Clips $e).Count)

  Write-Host ("tab1: " + (Keys $e '"Tab"')); Start-Sleep -Milliseconds 900
  Write-Host ("tab2: " + (Keys $e '"Tab"')); Start-Sleep -Milliseconds 900
  Write-Host ("right: " + (Keys $e '"Right"')); Start-Sleep -Milliseconds 900
  Write-Host ("delete: " + (Keys $e '"Delete"')); Start-Sleep -Milliseconds 900
  $shot = Join-Path $root 'dialog.png'
  Write-Host ("capture: " + (Ask-Engine $e 'ui.capture' ("{`"view`":`"current`",`"file`":`"$($shot.Replace('\','\\'))`"}")))
  Write-Host ("enter: " + (Keys $e '"Enter"')); Start-Sleep -Seconds 2
  $rawAfter = Ask-Engine $e 'clips.list'
  Write-Host ("clips after Enter: [" + $rawAfter + "]")
  $after = Join-Path $root 'after.png'
  Write-Host ("capture: " + (Ask-Engine $e 'ui.capture' ("{`"view`":`"current`",`"file`":`"$($after.Replace('\','\\'))`"}")))
  if ((Clips $e).Count -ge 2) {
    Write-Host ("enter again: " + (Keys $e '"Enter"')); Start-Sleep -Seconds 2
    Write-Host ("clips after second Enter: " + (Clips $e).Count)
  }
  Get-Content (Join-Path $root 'logs\engine.log') | Where-Object { $_ -match 'trash|Recycle|delete|Delete' } | ForEach-Object { Write-Host ("log: " + $_) }
} finally {
  if ($e) { Stop-ProbeEngine $e -KeepData:$KeepData }
  if ($loop) { Stop-Process -Id $loop.Id -Force -EA 0 }
}
