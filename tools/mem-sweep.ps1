# Measures the engine's memory across configurations. Each run: a fresh engine with one config
# written straight into its data root, the ring allowed to fill, then the working set and the private
# bytes.
# Usage: powershell -File tools/mem-sweep.ps1
param([string]$Root = "$env:TEMP\buffrx-sweep", [string]$Engine = 'buffrx-engine.exe')

$ErrorActionPreference = 'Stop'
$root_dir = Split-Path -Parent $PSScriptRoot
$engine = Join-Path $root_dir ('runtime\bin\64bit\' + $Engine)
$ask = Join-Path $root_dir 'tools\engine-ask.ps1'
. (Join-Path $PSScriptRoot 'probe-guard.ps1')

# This sweep gets a pipe of its own: a BuffrX the person is using never receives these commands, and a
# leftover engine cannot answer for this run. Nothing is stopped by name.
$env:BUFFRX_PIPE = 'buffrx-sweep-' + [guid]::NewGuid().ToString('N').Substring(0, 8)

$base = Get-Content "$env:LOCALAPPDATA\BuffrX\config.json" -Raw | ConvertFrom-Json
# Each scenario names its own quality level, not whatever the person's own config asks for: the sweep
# measures the engine's behaviour, not one person's settings. The keys a version 3 file carried are
# dropped, so a level is never overridden by an old explicit rate.
foreach ($old in 'bitrate_kbps', 'buffer_memory_mb', 'quality_tier', 'duration_first', 'cqp') {
  $base.PSObject.Properties.Remove($old)
}

function Scenario($name, $overrides, $fillSeconds) {
  Assert-ProbeDataRoot $Root | Out-Null
  Remove-Item $Root -Recurse -Force -ErrorAction SilentlyContinue
  New-Item -ItemType Directory -Force -Path $Root | Out-Null
  $env:BUFFRX_DATA_ROOT = $Root

  # The configuration is written straight into the data root: the engine reads it at startup, and a
  # shell-quoted JSON patch over the pipe loses its quotes on the way.
  $cfg = $base
  # The measurement root owns every path this run writes. The base is the person's own config, whose
  # clips_dir and timeline_minutes would otherwise put session chunks in their real library.
  $cfg | Add-Member -NotePropertyName clips_dir -NotePropertyValue "$Root\clips" -Force
  $cfg | Add-Member -NotePropertyName screenshots_dir -NotePropertyValue "$Root\clips\Screenshots" -Force
  $cfg | Add-Member -NotePropertyName recordings_dir -NotePropertyValue "$Root\clips\Recordings" -Force
  $cfg | Add-Member -NotePropertyName timeline_minutes -NotePropertyValue 0 -Force
  $cfg | Add-Member -NotePropertyName notifications -NotePropertyValue $false -Force
  New-Item -ItemType Directory -Force -Path "$Root\clips\Screenshots" | Out-Null
  foreach ($key in $overrides.Keys) { $cfg | Add-Member -NotePropertyName $key -NotePropertyValue $overrides[$key] -Force }
  # Written without a byte-order mark: the engine's JSON parser reads the file as UTF-8 and a
  # leading BOM is not JSON, so the whole configuration was silently ignored.
  [System.IO.File]::WriteAllText("$Root\config.json", ($cfg | ConvertTo-Json -Depth 8))

  $p = Start-Process $engine -PassThru -WindowStyle Hidden
  # The capture only has a size once its first frame arrives, and the engine answers slowly while it
  # initialises, so the measurement waits for that frame instead of sleeping a fixed time: a fixed sleep
  # measured an engine whose capture was still 0x0.
  $ready = $false
  for ($i = 0; $i -lt 40 -and -not $ready; $i++) {
    Start-Sleep -Seconds 1
    $probe = powershell -File $ask status.get '{}' -TimeoutMs 30000 2>$null
    if ($probe -match '"capture_ready":true') { $ready = $true }
  }
  Start-Sleep -Seconds $fillSeconds
  $status = powershell -File $ask status.get '{}' -TimeoutMs 30000 2>$null
  $live = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
  $ws = if ($live) { [math]::Round($live.WorkingSet64 / 1MB, 1) } else { 0 }
  $priv = if ($live) { [math]::Round($live.PrivateMemorySize64 / 1MB, 1) } else { 0 }
  $rate = 0; $filled = 0; $encoder = ''; $w = 0; $h = 0
  if ($status -match '"bitrate_kbps":(\d+)') { $rate = [math]::Round($matches[1] / 1000, 1) }
  if ($status -match '"filled":([\d.]+)') { $filled = [math]::Round([double]$matches[1], 1) }
  if ($status -match '"encoder":"([^"]+)"') { $encoder = $matches[1] }
  if ($status -match '"capture":\{[^}]*"width":(\d+)') { $w = $matches[1] }
  if ($status -match '"capture":\{[^}]*"height":(\d+)') { $h = $matches[1] }
  Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
  Start-Sleep -Seconds 2
  "{0,-36} {1,4}x{2,-4} {3,5} Mbps  WS={4,6} MB  private={5,6} MB  ring={6,5}s  {7}" -f $name, $w, $h, $rate, $ws, $priv, $filled, $encoder
}

# The capture's size, not the encoder's: the encoder scales the picture, so a 720p scenario still reads
# 2560x1440 here. The rate is the ring's plan and the working set is the number Task Manager shows.
"{0,-36} {1,-9} {2,-11} {3,-18} {4,-8} {5}" -f 'scenario', 'capture', 'rate', 'working set', 'ring', 'encoder'
Scenario '720p30 h264 standard, 30 s'     @{ output_width = 1280; output_height = 720; capture_fps = 30; codec = 'h264'; quality = 'standard'; custom_kbps = 0; buffer_seconds = 30 } 45
Scenario '1080p60 h264 high, 60 s'  @{ output_width = 1920; output_height = 1080; capture_fps = 60; codec = 'h264'; quality = 'high'; custom_kbps = 0; buffer_seconds = 60 } 75
Scenario '1440p60 h264 high, 60 s'  @{ output_width = 0; output_height = 0; capture_fps = 60; codec = 'h264'; quality = 'high'; custom_kbps = 0; buffer_seconds = 60 } 75
Scenario '1440p60 av1 high, 60 s'   @{ output_width = 0; output_height = 0; capture_fps = 60; codec = 'av1'; quality = 'high'; custom_kbps = 0; buffer_seconds = 60 } 75
Scenario '1440p60 h264 highest, 60 s'   @{ output_width = 0; output_height = 0; capture_fps = 60; codec = 'h264'; quality = 'highest'; custom_kbps = 0; buffer_seconds = 60 } 75
# The 300 s ring is measured once 100 s of it is filled: waiting the full five minutes adds no new
# information about memory, and the fill time is printed beside it.
Scenario '1440p60 h264 high, 300 s ring' @{ output_width = 0; output_height = 0; capture_fps = 60; codec = 'h264'; quality = 'high'; custom_kbps = 0; buffer_seconds = 300 } 100
