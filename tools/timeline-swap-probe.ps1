# Does a Timeline session survive the encoder controller's swaps (plan 008), and is a save across them a good clip?
#
#   powershell -File tools/timeline-swap-probe.ps1 -Engine <private runtime>\bin\64bit\buffrx-engine.exe -Ffprobe <ffprobe.exe>
#
# A throwaway engine (own pipe and data root) writes Timeline everywhere at 120 fps while BUFFRX_TEST_ENCODER_SWAP_MS asks for two swaps:
# an effort step, then a frame-rate divisor (120 -> 60 fps), then back to 120. None of them changes the stream's layout, so the epoch
# stays and a save joins across all of them, rescaled. Every chunk is read for its rate, then two saves: the effort seam alone, and
# the whole session, which must hold every chunk's frames. Each clip is decoded frame by frame with ffprobe.
# The picture is kept changing by tools/screen-noise.ps1. -Keep leaves the data root for a look at the chunks afterwards.
param(
  [string]$Engine = 'buffrx-engine.exe',
  [string]$Ffprobe = $env:BUFFRX_FFPROBE,
  [string]$Codec = 'h264',
  [switch]$Keep
)
. (Join-Path $PSScriptRoot 'probe-lib.ps1')
if (-not $Ffprobe -or -not (Test-Path -LiteralPath $Ffprobe)) { throw 'pass -Ffprobe (or set BUFFRX_FFPROBE): every saved clip is decoded with it' }
$failures = 0
function Check([string]$what, [bool]$ok, [string]$detail = '') {
  if (-not $ok) { $script:failures++ }
  Write-Host ('{0} {1,-64} {2}' -f $(if ($ok) { 'ok  ' } else { 'FAIL' }), $what, $detail)
}
function Decode([string]$file) {
  $keep = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
  $lines = (& $Ffprobe -v error -select_streams v:0 -show_entries frame=pts_time -of csv=p=0 $file 2>&1 | Out-String) -split "`r?`n"
  $info = (& $Ffprobe -v error -show_entries 'format=duration:stream=codec_type,r_frame_rate' -of json $file 2>$null | Out-String) | ConvertFrom-Json
  $ErrorActionPreference = $keep
  $frames = @($lines | Where-Object { $_ -match '^[0-9.]+,?$' }).Count
  $errors = @($lines | Where-Object { $_.Trim() -and $_ -notmatch '^[0-9.]+,?$' })
  [pscustomobject]@{ Frames = $frames; Errors = $errors.Count; ErrorText = (($errors | Select-Object -First 2) -join ' | '); Duration = [double]$info.format.duration
    Rate = (@($info.streams | Where-Object { $_.codec_type -eq 'video' })[0]).r_frame_rate; Audio = @($info.streams | Where-Object { $_.codec_type -eq 'audio' }).Count }
}

$noise = Start-Process powershell -ArgumentList '-NoProfile', '-File', (Join-Path $PSScriptRoot 'screen-noise.ps1') -PassThru -WindowStyle Hidden
$e = $null
try {
  $cfg = '{"config_version":7,"codec_auto_picked":true,"start_buffering":false,"auto_game_capture":false,"timeline_minutes":30,"timeline_everywhere":true,' +
         '"capture_fps":120,"buffer_seconds":30,"codec":"' + $Codec + '","encoder":"nvenc","container":"mp4","audio_tracks":1}'
  $e = Start-ProbeEngine $cfg $Engine @{ BUFFRX_TEST_ENCODER_SWAP_MS = '14000,26000,38000'; BUFFRX_TEST_ENCODER_SWAP_LEVELS = '1,1/2,1' }
  Wait-Asleep $e
  $null = Ask-Engine $e 'buffer.start'; $null = Wait-Buffering $e
  $started = Get-Date
  $swaps = 0
  while (((Get-Date) - $started).TotalSeconds -lt 90) {
    Start-Sleep -Milliseconds 1000
    $swaps = [int](Num (Ask-Engine $e 'status.get') 'swaps')
    if ($swaps -ge 3 -and ((Get-Date) - $started).TotalSeconds -gt 52) { break }
  }
  Check 'three swaps happened under a writing Timeline' ($swaps -ge 3) "swaps=$swaps"
  $log = Get-Content (Join-Path $e.DataRoot 'logs\engine.log') -Raw
  foreach ($line in @([regex]::Matches($log, 'encoder swap: [^\r\n]*') | ForEach-Object { $_.Value })) { Write-Host "INFO $line" }

  $got = (Ask-Engine $e 'timeline.get') | ConvertFrom-Json
  $session = $got.data.session
  $chunks = @($got.data.chunks)
  Check 'the session kept one id through the swaps' ([bool]$session -and $chunks.Count -ge 4) "$session, $($chunks.Count) chunk(s)"
  # Every chunk's own rate: the swap to divisor 2 is where the files change from 120 to 60 fps.
  $rates = @()
  foreach ($chunk in $chunks) {
    $d = Decode $chunk.file
    $rates += [pscustomobject]@{ Start = [int64]$chunk.start_ms; End = [int64]$chunk.end_ms; Rate = $d.Rate; Errors = $d.Errors; Audio = $d.Audio; Epoch = [int]$chunk.epoch; Frames = $d.Frames }
    Write-Host ("INFO chunk {0,7}-{1,7} ms  rate {2}  frames {3}  decode errors {4}  epoch {5}" -f $chunk.start_ms, $chunk.end_ms, $d.Rate, $d.Frames, $d.Errors, $chunk.epoch)
  }
  Check 'every chunk decodes cleanly' (-not ($rates | Where-Object { $_.Errors -gt 0 })) ''
  $fast = @($rates | Where-Object { $_.Rate -eq '120/1' })
  # The 120 fps run before the rate change: the effort seam lies inside it.
  $lead = @(); foreach ($r in $rates) { if ($r.Rate -ne '120/1') { break }; $lead += $r }
  $slow = @($rates | Where-Object { $_.Rate -eq '60/1' })
  Check 'chunks are written at 120, then 60, then 120 fps' ($fast.Count -ge 3 -and $slow.Count -ge 1 -and $rates[-1].Rate -eq '120/1') "$($fast.Count) at 120, $($slow.Count) at 60"
  # The epoch is the line a save cannot cross: neither an effort swap nor a frame-rate step is one.
  $epochs = @($rates | ForEach-Object { $_.Epoch } | Sort-Object -Unique)
  Check 'the epoch holds through effort and frame-rate swaps' ($epochs.Count -eq 1) "epochs: $($epochs -join ',')"

  $save = {
    param([int64]$from, [int64]$to, [string]$name)
    $reply = (Ask-Engine $e 'timeline.save' (@{ session = $session; start_ms = $from; end_ms = $to; name = $name } | ConvertTo-Json -Compress)) | ConvertFrom-Json
    if (-not $reply.ok) { return [pscustomobject]@{ Ok = $false; Error = $reply.error } }
    $d = Decode $reply.data.file
    [pscustomobject]@{ Ok = $true; File = $reply.data.file; CutHead = [int64]$reply.data.cut_head_ms; Chunks = [int]$reply.data.chunks; Decoded = $d }
  }
  if ($lead.Count -ge 2) {
    # The effort seam: the 120 fps chunks span the first swap (effort 0 -> 1). Nothing may be cut and every frame must decode.
    $a = & $save $lead[0].Start ($lead[-1].End - 200) 'effort seam'
    Check 'a save across the effort swap joins every chunk' ($a.Ok -and $a.CutHead -eq 0 -and $a.Chunks -eq $lead.Count) $(if ($a.Ok) { "chunks=$($a.Chunks) cut_head=$($a.CutHead)" } else { $a.Error })
    if ($a.Ok) { Check 'that clip decodes with no error' ($a.Decoded.Errors -eq 0 -and $a.Decoded.Frames -gt 0) "frames=$($a.Decoded.Frames) duration=$($a.Decoded.Duration) $($a.Decoded.ErrorText)" }
  }
  if ($slow.Count -ge 1) {
    # Across both rate changes (120 -> 60 -> 120): every chunk joined, nothing cut, every frame of every chunk in the clip.
    $b = & $save $rates[0].Start ($rates[-1].End - 200) 'whole session'
    $want = ($rates | Measure-Object -Property Frames -Sum).Sum - 24   # the range ends 200 ms before the last chunk (24 frames at 120 fps)
    Check 'a save across both rate changes joins every chunk' ($b.Ok -and $b.CutHead -eq 0 -and $b.Chunks -eq $rates.Count) $(if ($b.Ok) { "chunks=$($b.Chunks)/$($rates.Count) cut_head=$($b.CutHead)" } else { $b.Error })
    if ($b.Ok) {
      Check 'that clip decodes and holds every chunk''s frames' ($b.Decoded.Errors -eq 0 -and $b.Decoded.Frames -ge $want - 30) "frames=$($b.Decoded.Frames) of $want, duration=$([math]::Round($b.Decoded.Duration, 2)) $($b.Decoded.ErrorText)"
      $pref = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
      $times = @((& $Ffprobe -v error -select_streams v:0 -show_entries frame=pts_time -of csv=p=0 $b.File 2>$null) | Where-Object { $_ -match '^[0-9.]+' } | ForEach-Object { [double]($_.TrimEnd(',')) })
      $ErrorActionPreference = $pref
      $back = 0; $dupes = 0
      for ($k = 1; $k -lt $times.Count; $k++) { if ($times[$k] -lt $times[$k - 1]) { $back++ } elseif ($times[$k] -eq $times[$k - 1]) { $dupes++ } }
      Check 'its pictures never step back or share a time' ($back -eq 0 -and $dupes -eq 0) "backwards=$back same-time=$dupes of $($times.Count)"
      # Where the clip's pictures are spaced unlike a 120 or 60 fps frame: the seams, and anything lost at them.
      $odd = @(); for ($k = 1; $k -lt $times.Count; $k++) { $d = [math]::Round(($times[$k] - $times[$k - 1]) * 1000, 1); if ([math]::Abs($d - 8.3) -gt 1.5 -and [math]::Abs($d - 16.7) -gt 1.5) { $odd += "$([math]::Round($times[$k], 2))s:+${d}ms" } }
      Write-Host "INFO irregular spacing: $($odd -join ' ')"
    }
  }
  $status = Ask-Engine $e 'status.get'
  Check 'the buffer is still on and holding footage' ($status -match '"buffer_state":\s*"on"' -and (Num $status 'held_seconds') -gt 10) "held=$([math]::Round((Num $status 'held_seconds'), 1)) s"
  $errorsInLog = @([regex]::Matches((Get-Content (Join-Path $e.DataRoot 'logs\engine.log') -Raw), '[^\r\n]*\] \[error\][^\r\n]*') | ForEach-Object { $_.Value })
  Check 'no error in the engine log' ($errorsInLog.Count -eq 0) (($errorsInLog | Select-Object -First 2) -join ' | ')
  if ($Keep) { Write-Host "data root kept: $($e.DataRoot)" }
} finally {
  if ($e) { if ($Keep) { Stop-ProbeEngine $e -KeepData } else { Stop-ProbeEngine $e } }
  if ($noise) { Stop-Process -Id $noise.Id -Force -ErrorAction SilentlyContinue }
}
Write-Host $(if ($failures) { "timeline swap probe: $failures check(s) failed" } else { 'timeline swap probe: all checks passed' })
exit $(if ($failures) { 1 } else { 0 })
