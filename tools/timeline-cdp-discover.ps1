# Discovers the Timeline preview page: starts a probe engine on a copy of a real recorded session, opens the
# preview the way Alt+4 does, and prints every element with an id plus the page's text. Used to write the checks
# in tools/timeline-cdp-probe.mjs; not a test by itself.
param(
  [string]$SourceRoot = "$env:TEMP\buffrx-probe-65674214",
  [int]$Port = 9334
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\probe-lib.ps1"

# The session's chunks are named by absolute path, so the engine must run on the root they came from: a copy
# elsewhere makes the media scheme answer 403 for every chunk.
$root = $SourceRoot
Remove-Item (Join-Path $root 'logs') -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $root 'capacity.json') -Force -ErrorAction SilentlyContinue

$config = @{
  config_version      = 7
  start_buffering     = $false
  auto_game_capture   = $false
  notifications       = $false
  notification_sound  = $false
  min_free_gb         = 1
  timeline_minutes    = 30
  buffer_seconds      = 20
  capture_fps         = 60
  codec               = 'h264'
  codec_auto_picked   = $true
  container           = 'mp4'
  quality             = 'standard'
} | ConvertTo-Json -Compress
$engine = Start-ProbeEngine -ConfigJson $config -DataRoot $root -Env @{ BUFFRX_WEBVIEW_DEBUG_PORT = "$Port" }
try {
  Start-Sleep -Seconds 2
  $shown = Ask-Engine $engine 'ui.show' '{"view":"timeline"}'
  Write-Host "ui.show: $shown"
  for ($i = 0; $i -lt 40; $i++) {
    Start-Sleep -Milliseconds 500
    try { $pages = (Invoke-RestMethod "http://127.0.0.1:$Port/json" -TimeoutSec 2) } catch { continue }
    $page = @($pages | Where-Object { $_.type -eq 'page' }) | Select-Object -First 1
    if ($page) { Write-Host "page: $($page.url)"; break }
  }
  if (-not $page) { Write-Host 'no page answered the debug port'; exit 1 }
  $js = "JSON.stringify({ ids: [...document.querySelectorAll('[id]')].map(e => e.id + ':' + (e.innerText||'').trim().slice(0,28).replace(/\s+/g,' ')), text: document.body.innerText.replace(/\s+/g,' ').slice(0, 900) })"
  & node (Join-Path $PSScriptRoot 'editor-cdp.mjs') "--port=$Port" "--eval=$js"
}
finally {
  Stop-ProbeEngine $engine
}
