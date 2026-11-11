# The sleep patch A/B (P2-1) on window capture of the present loop: one engine per run, the patch off and on alternating.
#   powershell -File tools\sleep-ab.ps1 -Fps 144 -Rounds 3 -Seconds 30 -Engine buffrx-engine-night.exe
# Prints one CSV line per run: patch,fps,round,cpu_percent,rendered_per_s,lagged,dropped,enc_ratio,ws_mb,note
param(
  [int]$Fps = 144,
  [int]$Rounds = 3,
  [int]$Seconds = 30,
  [string]$Engine = 'buffrx-engine.exe'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'probe-lib.ps1')

$loopArgs = '--seconds ' + ($Rounds * 2 * ($Seconds + 40) + 120) + ' --width 1280 --height 720 --motion 1'
$loop = Start-Process -FilePath (Join-Path $ProbeRoot 'build\buffrx-present-loop.exe') -ArgumentList $loopArgs -PassThru -WindowStyle Normal
Write-Output ("present-loop PID {0}" -f $loop.Id)
Start-Sleep -Seconds 2
Write-Output 'patch,fps,round,cpu_percent,rendered_per_s,lagged,dropped,enc_ratio,ws_mb,note'
try {
  $target = $null
  for ($round = 1; $round -le $Rounds; $round++) {
    foreach ($patch in @($false, $true)) {
      $e = $null
      try {
        if ($patch) { Remove-Item Env:BUFFRX_NO_SLEEP_PATCH -ErrorAction SilentlyContinue } else { $env:BUFFRX_NO_SLEEP_PATCH = '1' }
        $e = Start-ProbeEngine ('{"config_version":7,"start_buffering":false,"auto_game_capture":false,"timeline_minutes":0,"capture_fps":' + $Fps + ',"codec":"h264","output_width":1280,"output_height":720,"buffer_seconds":10}') $Engine
        if (-not $target) {
          $rows = (Ask-Engine $e 'sources.windows' | ConvertFrom-Json).data
          $target = $rows | Where-Object { $_.exe -like '*present-loop*' } | Select-Object -First 1
          if (-not $target) { throw 'no present-loop window to capture' }
        }
        $null = Ask-Engine $e 'capture.set' (@{ mode = 'window'; window_id = $target.id } | ConvertTo-Json -Compress)
        $null = Ask-Engine $e 'buffer.start' '{}'
        Wait-Buffering $e
        Start-Sleep -Seconds 5
        $s0 = Ask-Engine $e 'status.get'; $c0 = Cpu-Ms $e; $t0 = Get-Date
        Start-Sleep -Seconds $Seconds
        $s1 = Ask-Engine $e 'status.get'; $cpu = (Cpu-Ms $e) - $c0; $ms = ((Get-Date) - $t0).TotalMilliseconds
        $frames = (Num $s1 'video_frames') - (Num $s0 'video_frames')
        $note = if (Test-Quiet ($ms / 1000)) { '' } else { 'user active: not publishable' }
        '{0},{1},{2},{3},{4},{5},{6},{7},{8},{9}' -f $(if ($patch) { 'on' } else { 'off' }), $Fps, $round,
          [math]::Round(100.0 * $cpu / $ms, 2), [math]::Round($frames / ($ms / 1000), 1),
          [int]((Num $s1 'video_lagged') - (Num $s0 'video_lagged')), [int]((Num $s1 'dropped') - (Num $s0 'dropped')),
          [math]::Round((Num $s1 'encoder_ratio'), 3), [int]($e.Process.WorkingSet64 / 1MB), $note
      } finally {
        Stop-ProbeEngine $e; $e = $null
        Remove-Item Env:BUFFRX_NO_SLEEP_PATCH -ErrorAction SilentlyContinue
      }
    }
  }
} finally {
  Stop-Process -Id $loop.Id -Force -ErrorAction SilentlyContinue
  Remove-Item Env:BUFFRX_NO_SLEEP_PATCH -ErrorAction SilentlyContinue
}
