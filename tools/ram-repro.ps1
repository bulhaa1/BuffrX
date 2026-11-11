# Reproduces one buffer configuration on a throwaway engine and samples what the ring actually costs:
# the process's private bytes and working set beside the bytes the ring holds in RAM, in its file and
# in Timeline chunks, the encoder's pace, and the machine's GPU. Made for the 5-minute / 1 GB report:
# the question is whether memory is the footage (ring bytes climb, private follows) or a leak
# (private climbs while ring bytes stay flat).
#
#   powershell -File tools/ram-repro.ps1 -Minutes 60                       # the person's 5 min / 120 fps / AV1
#   powershell -File tools/ram-repro.ps1 -Minutes 15 -TimelineMinutes 30   # the same with Timeline writing
#
# CSV: <TEMP>\buffrx-ram-<stamp>.csv. The engine is this run's own (own data root and pipe); the
# person's BuffrX is never touched. Run on a quiet machine, with the picture changing (the probe
# starts tools\screen-noise.ps1 and a moving window itself).
param(
  [int]$Minutes = 60,
  [int]$Every = 30,
  [int]$Fps = 120,
  [string]$Codec = 'av1',
  [string]$Quality = 'highest',
  [int]$Kbps = 50000,
  [int]$BufferSeconds = 300,
  [int]$TimelineMinutes = 0,
  [string]$EncoderMode = 'fast',
  [string]$CaptureMode = 'display',
  [string]$Engine = 'buffrx-engine-night.exe',
  [string]$Label = '',
  [switch]$NoMotion,
  [switch]$Fullscreen,
  [switch]$KeepData
)
. (Join-Path $PSScriptRoot 'probe-lib.ps1')
$ErrorActionPreference = 'Stop'

# Display capture is DXGI duplication: when Windows turns the monitor off, duplication stops handing
# over frames and the ring stops growing. A probe can be the thing that keeps the display on, for its
# own lifetime only, without changing the machine's power settings.
Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
public static class Awake {
  [DllImport("kernel32.dll")] public static extern uint SetThreadExecutionState(uint flags);
}
'@
$null = [Awake]::SetThreadExecutionState([uint32]2147483651)

function Gpu() {
  $line = & nvidia-smi --query-gpu=utilization.gpu,memory.used --format=csv,noheader,nounits 2>$null | Select-Object -First 1
  if (-not $line) { return @(-1, -1) }
  $parts = $line -split ','
  return @([double]$parts[0].Trim(), [double]$parts[1].Trim())
}

$noise = if ($NoMotion) { $null } else {
  Start-Process powershell -ArgumentList '-NoProfile', '-File', (Join-Path $PSScriptRoot 'screen-noise.ps1') -PassThru -WindowStyle Hidden
}
$loop = if ($NoMotion) { $null } else {
  $loopArgs = @('--seconds', ($Minutes * 60 + 180), '--width', '1280', '--height', '720', '--motion', '1')
  if ($Fullscreen) { $loopArgs += @('--fullscreen', '1') }
  Start-Process (Join-Path $script:ProbeRoot 'build\buffrx-present-loop.exe') -ArgumentList $loopArgs -PassThru
}
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$name = if ($Label) { $Label } else { 'ram' }
$csv = Join-Path $env:TEMP ("buffrx-$name-$stamp.csv")
$e = $null
try {
  $seed = @{
    start_buffering = $false; auto_game_capture = $false; buffer_seconds = $BufferSeconds
    capture_fps = $Fps; codec = $Codec; quality = $Quality; peak_mode = 'fixed'; custom_kbps = $Kbps
    encoder_mode = $EncoderMode; capture_mode = $CaptureMode; timeline_minutes = $TimelineMinutes
  } | ConvertTo-Json -Compress
  $e = Start-ProbeEngine $seed $Engine
  Wait-Asleep $e
  if ($CaptureMode -eq 'window' -and -not $NoMotion) { $null = Set-WindowCapture $e }
  $null = Ask-Engine $e 'buffer.start'
  try { $null = Wait-Buffering $e } catch { "buffer did not start: $_"; throw }
  $ringDir = Join-Path $e.DataRoot 'clips\.buffrx'
  $started = Get-Date; $end = $started.AddMinutes($Minutes)
  $lastCpu = $e.Process.TotalProcessorTime.TotalMilliseconds; $lastCpuAt = Get-Date
  while ((Get-Date) -lt $end) {
    $e.Process.Refresh()
    $s = Ask-Engine $e 'status.get'
    $drive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($e.DataRoot))
    $gpu = Gpu
    $now = Get-Date; $cpu = $e.Process.TotalProcessorTime.TotalMilliseconds
    $cpuPct = if (($now - $lastCpuAt).TotalMilliseconds -gt 0) { ($cpu - $lastCpu) / ($now - $lastCpuAt).TotalMilliseconds * 100 } else { -1 }
    $lastCpu = $cpu; $lastCpuAt = $now
    $state = if ($s -match '"buffer_state":\s*"([^"]+)"') { $Matches[1] } else { '?' }
    $capMode = if ($s -match '"capture":\s*\{\s*"mode":\s*"([^"]+)"') { $Matches[1] } else { '?' }
    $capW = if ($s -match '"capture":\s*\{[^}]*"width":\s*(\d+)') { [int]$Matches[1] } else { -1 }
    $capH = if ($s -match '"capture":\s*\{[^}]*"height":\s*(\d+)') { [int]$Matches[1] } else { -1 }
    $held = Num $s 'held_seconds'
    $usedMb = (Num $s 'used_bytes') / 1MB
    $row = [pscustomobject][ordered]@{
      minute = [math]::Round(($now - $started).TotalMinutes, 1)
      private_mb = [math]::Round($e.Process.PrivateMemorySize64 / 1MB, 1)
      ws_mb = [math]::Round($e.Process.WorkingSet64 / 1MB, 1)
      handles = $e.Process.HandleCount
      threads = $e.Process.Threads.Count
      ring_used_mb = [math]::Round($usedMb, 1)
      ring_ram_mb = [math]::Round((Num $s 'ram_bytes') / 1MB, 1)
      ring_disk_mb = [math]::Round((Num $s 'disk_bytes') / 1MB, 1)
      held_s = [math]::Round($held, 1)
      ring_mbps = if ($held -gt 1) { [math]::Round($usedMb * 8 / $held, 2) } else { 0 }
      avg_kbps = [math]::Round((Num $s 'avg_kbps'), 0)
      measured = [math]::Round((Num $s 'measured'), 0)
      spill_files = @(Get-ChildItem $ringDir -Filter 'replay-*.ring' -ErrorAction SilentlyContinue).Count
      free_gb = [math]::Round($drive.AvailableFreeSpace / 1GB, 1)
      gpu_pct = $gpu[0]
      gpu_mem_mb = $gpu[1]
      cpu_pct = [math]::Round($cpuPct, 1)
      capture_fps = [math]::Round((Num $s 'capture_fps'), 1)
      encoder_fps = [math]::Round((Num $s 'encoder_fps'), 1)
      video_frames = [math]::Round((Num $s 'video_frames'), 0)
      cap_mode = $capMode
      cap_w = $capW
      cap_h = $capH
      encoder_behind = [math]::Round((Num $s 'encoder_behind'), 0)
      dropped = [math]::Round((Num $s 'dropped'), 0)
      video_lagged = [math]::Round((Num $s 'video_lagged'), 0)
      asleep = if ($s -match '"asleep":\s*true') { 1 } else { 0 }
      buffer_state = $state
    }
    $row | Export-Csv -LiteralPath $csv -Append -NoTypeInformation -Encoding ascii
    Start-Sleep -Seconds $Every
  }
  "ram probe finished: $csv"
  $rows = Import-Csv $csv
  $first = $rows | Where-Object { [double]$_.minute -ge 3 } | Select-Object -First 1
  if (-not $first) { $first = $rows | Select-Object -First 1 }
  $last = $rows | Select-Object -Last 1
  foreach ($col in 'private_mb', 'ws_mb', 'handles', 'threads', 'ring_used_mb', 'ring_ram_mb', 'ring_disk_mb', 'free_gb', 'gpu_mem_mb', 'held_s', 'ring_mbps') {
    '{0,-14} after 3 min {1,10}   at the end {2,10}   change {3,8:N1}' -f $col, $first.$col, $last.$col, ([double]$last.$col - [double]$first.$col)
  }
  $peak = ($rows | Measure-Object -Property private_mb -Maximum).Maximum
  $ringAtPeak = ($rows | Sort-Object { [double]$_.private_mb } | Select-Object -Last 1).ring_used_mb
  'peak private {0:N1} MB at ring {1:N1} MB; last ring {2:N1} MB of {3} s buffer' -f $peak, $ringAtPeak, $last.ring_used_mb, $BufferSeconds
} finally {
  $null = [Awake]::SetThreadExecutionState([uint32]2147483648)
  Stop-ProbeEngine $e -KeepData:$KeepData
  if ($loop) { Stop-Process -Id $loop.Id -Force -ErrorAction SilentlyContinue }
  if ($noise) { Stop-Process -Id $noise.Id -Force -ErrorAction SilentlyContinue }
}
