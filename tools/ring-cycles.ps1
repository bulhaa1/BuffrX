# Does the ring's file or its threads leak across buffer stops and starts? A throwaway engine buffers game-like footage over a small RAM limit
# (so a ring file is made and written every cycle), stops the buffer, starts it again, and counts the process's handles, threads and private
# bytes. The same run with -NoSpill is the comparison: libobs itself leaves one thread handle per output stop (AGENTS.md), so the ring is
# clean when the two runs grow by the same.
#
#   powershell -File tools\ring-cycles.ps1 -Engine buffrx-engine-ring.exe
#   powershell -File tools\ring-cycles.ps1 -Engine buffrx-engine-ring.exe -NoSpill
param(
  [int]$Cycles = 20,
  [int]$RamMb = 32,
  [int]$HoldSeconds = 8,           # buffering per cycle: long enough for the footage to pass the spill start and a block to be written
  [string]$Engine = 'buffrx-engine.exe',
  [switch]$NoSpill
)
. (Join-Path $PSScriptRoot 'probe-lib.ps1')
$loop = Start-Process (Join-Path $script:ProbeRoot 'build\buffrx-present-loop.exe') -ArgumentList '--seconds', ($Cycles * ($HoldSeconds + 12) + 120), '--width', '1280', '--height', '720', '--motion', 1 -PassThru
$e = $null
try {
  $envs = @{ BUFFRX_RING_RAM_MB = [string]$RamMb }
  if ($NoSpill) { $envs['BUFFRX_NO_SPILL'] = '1' }
  $e = Start-ProbeEngine '{"config_version":7,"start_buffering":false,"auto_game_capture":false,"timeline_minutes":0,"capture_fps":60,"buffer_seconds":20,"codec":"h264","codec_auto_picked":true}' $Engine $envs
  Wait-Asleep $e
  $ringDir = Join-Path $e.DataRoot 'clips\.buffrx'
  $rows = @(); $files = 0; $leftover = 0; $spilledTotal = 0
  for ($i = 1; $i -le $Cycles; $i++) {
    $null = Ask-Engine $e 'buffer.start'; Wait-Buffering $e
    Start-Sleep -Seconds $HoldSeconds
    $s = Ask-Engine $e 'status.get'
    if (Get-ChildItem $ringDir -Filter 'replay-*.ring' -ErrorAction SilentlyContinue) { $files++ }
    $spilledTotal += (Num $s 'spilled_bytes')
    $null = Ask-Engine $e 'buffer.stop'
    for ($k = 0; $k -lt 20; $k++) { Start-Sleep -Milliseconds 500; if (-not (Get-ChildItem $ringDir -Filter 'replay-*.ring' -ErrorAction SilentlyContinue)) { break } }
    if (Get-ChildItem $ringDir -Filter 'replay-*.ring' -ErrorAction SilentlyContinue) { $leftover++ }
    $e.Process.Refresh()
    if ($i -eq 1 -or $i % 5 -eq 0) {
      $rows += [pscustomobject]@{ Cycle = $i; Handles = $e.Process.HandleCount; Threads = $e.Process.Threads.Count; 'Private MB' = [int]($e.Process.PrivateMemorySize64 / 1MB)
        'Ring file seen' = $files; 'Left behind' = $leftover; 'Spilled MB (sum)' = [int]($spilledTotal / 1MB) }
    }
  }
  $rows | Format-Table -AutoSize | Out-String -Width 200
  $first = $rows[0]; $last = $rows[-1]
  "{0}: handles {1} -> {2} (+{3}), threads {4} -> {5}, ring file left behind after {6} stops: {7}, cycles that made a file: {8}" -f $(if ($NoSpill) { 'RAM only' } else { 'with the disk part' }), $first.Handles, $last.Handles, ($last.Handles - $first.Handles), $first.Threads, $last.Threads, $Cycles, $leftover, $files
} finally {
  Stop-ProbeEngine $e
  if ($loop) { Stop-Process -Id $loop.Id -Force -ErrorAction SilentlyContinue }
}
