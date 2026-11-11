param([switch]$KeepData)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'probe-lib.ps1')
$cfg = @{ config_version=7; start_buffering=$false; auto_game_capture=$false; notifications=$false; notification_sound=$false; min_free_gb=1; buffer_seconds=15; capture_fps=60; codec='h264'; container='mp4'; quality='standard'; audio_tracks=1; timeline_minutes=0; codec_auto_picked=$true } | ConvertTo-Json -Compress
$e = Start-ProbeEngine -ConfigJson $cfg
try {
  $dir = Join-Path $env:TEMP 'opencode\deck'
  New-Item -ItemType Directory -Path $dir -Force | Out-Null
  foreach ($p in @('buffer','video','audio')) {
    $f = Join-Path $dir ("page-$p.png")
    [void](Ask-Engine $e 'ui.capture' ("{`"view`":`"settings:$p`",`"file`":`"$($f.Replace('\','\\'))`"}"))
    Write-Host ("$p $((Get-FileHash $f).Hash.Substring(0,12)) $((Get-Item $f).Length)")
  }
} finally { Stop-ProbeEngine $e -KeepData:$KeepData }
