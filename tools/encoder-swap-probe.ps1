# Plan 008, phase 1: does the replay ring keep its footage through an encoder swap, and is a clip that crosses swaps a good clip?
#
#   powershell -File tools/encoder-swap-probe.ps1 -Codecs h264,hevc,av1 -Encoders nvenc,software
#   powershell -File tools/encoder-swap-probe.ps1 -Codecs h264 -Encoders nvenc -Levels 0,1,3      # swaps that also change the effort step
#   powershell -File tools/encoder-swap-probe.ps1 -Codecs h264 -Encoders nvenc -FailMode 2         # the new and the old encoder both refuse to start
#
# A throwaway engine (own pipe and data root, beside any other engine) buffers the display with a long buffer. BUFFRX_TEST_ENCODER_SWAP_MS makes
# its tick ask for a swap at each of the given times after the buffer first ran; BUFFRX_TEST_ENCODER_SWAP_LEVELS gives each swap its effort
# step. When the last swap has settled, clips of increasing length are saved: the shortest reaches back to no swap, the longest across all of them.
# Each clip is read back with ffprobe (every frame is decoded, so a wrong header at a seam shows) and with the engine's own packet report.
# A codec or encoder this PC does not have is reported as skipped, not failed. The picture is kept changing by tools/screen-noise.ps1.
param(
  [string[]]$Codecs = @('h264', 'hevc', 'av1'),
  [string[]]$Encoders = @('nvenc', 'software'),
  [int[]]$SwapAtSeconds = @(18, 22, 26),
  [string]$Levels = '',                 # effort step of each swap, e.g. 0,1,3 (default: every swap keeps the settings)
  [int]$BufferSeconds = 30,
  [int]$Fps = 60,
  [int]$FailMode = 0,                    # BUFFRX_TEST_SWAP_FAIL
  [string]$Engine = 'buffrx-engine.exe',
  # Any ffprobe from FFmpeg 5 or later (the one in Rewind's portable build is n7.0.2); ffprobe-static's 4.0.2 prints no frame times for these clips.
  [string]$Ffprobe = $env:BUFFRX_FFPROBE,
  [switch]$Timeline,                     # Timeline writes the desktop too (timeline_everywhere): its segment must close at each swap and open again
  [switch]$NoSwaps,                      # the baseline: the same saves with no swap at all
  [switch]$NoNoise,
  [switch]$Motion                         # a window of moving rectangles (buffrx-present-loop --motion 1) for footage at game-like rates
)
. (Join-Path $PSScriptRoot 'probe-lib.ps1')
$Codecs = @($Codecs | ForEach-Object { $_ -split ',' }); $Encoders = @($Encoders | ForEach-Object { $_ -split ',' })
$SwapAtSeconds = @($SwapAtSeconds | ForEach-Object { [string]$_ -split ',' } | ForEach-Object { [int]$_ })
if (-not $Ffprobe -or -not (Test-Path $Ffprobe)) { throw "no ffprobe at '$Ffprobe': pass -Ffprobe or set BUFFRX_FFPROBE (FFmpeg 5 or later)" }
$noise = if ($NoNoise -or $Motion) { $null } else { Start-Process powershell -ArgumentList '-NoProfile', '-File', (Join-Path $PSScriptRoot 'screen-noise.ps1') -PassThru -WindowStyle Hidden }
$loop = $null
if ($Motion) { $loop = Start-Process (Join-Path $script:ProbeRoot 'build\buffrx-present-loop.exe') -ArgumentList '--seconds', 1800, '--width', '1280', '--height', '720', '--motion', 1 -PassThru }
$rows = New-Object System.Collections.Generic.List[string]
$failures = 0
$skipped = 0

function Probe-Clip([string]$file) {
  $keepPreference = $ErrorActionPreference; $ErrorActionPreference = 'Continue'   # ffprobe reports decode errors on stderr, which is the finding, not a failure of the probe
  # Every frame is decoded (-show_frames), so a stream that does not decode at a seam prints its errors; the frame times say where the picture jumped.
  $errors = (& $Ffprobe -v error -select_streams v:0 -show_entries frame=pts_time -of csv=p=0 $file 2>&1 | Out-String)
  $times = @(); $bad = 0; $errorText = ''
  foreach ($line in ($errors -split "`r?`n")) {
    if ($line -match '^[0-9.]+,?$') { $times += [double]($line.TrimEnd(',')) } elseif ($line.Trim()) { $bad++; if ($errorText.Length -lt 200) { $errorText += $line.Trim() + ' | ' } }
  }
  $info = (& $Ffprobe -v error -show_entries 'format=duration:stream=codec_type,codec_name,width,height,duration' -of json $file 2>$null | Out-String) | ConvertFrom-Json
  $video = @($info.streams | Where-Object { $_.codec_type -eq 'video' })[0]
  $audio = @($info.streams | Where-Object { $_.codec_type -eq 'audio' })
  $sorted = $times | Sort-Object
  $period = 1.0 / $Fps
  $gaps = @(); $backwards = 0
  for ($i = 1; $i -lt $times.Count; $i++) {
    $delta = $times[$i] - $times[$i - 1]
    if ($delta -lt 0) { $backwards++ }
  }
  for ($i = 1; $i -lt $sorted.Count; $i++) {
    $delta = $sorted[$i] - $sorted[$i - 1]
    if ($delta -gt 2.5 * $period) { $gaps += [math]::Round($delta, 3) }
  }
  $ErrorActionPreference = $keepPreference
  # How the pictures are spaced: a clip across a frame-rate swap has two spacings (1/120 s and 1/60 s), each even on its own side.
  $spacing = @{}
  for ($i = 1; $i -lt $sorted.Count; $i++) { $k = [math]::Round(($sorted[$i] - $sorted[$i - 1]) * 1000, 0); $spacing[$k] = 1 + [int]$spacing[$k] }
  # Even spacing: the common spacings (a frame-rate swap leaves two) and the pictures whose distance from the one before is none of them.
  $modal = @($spacing.GetEnumerator() | Where-Object { $_.Value -ge 20 } | ForEach-Object { [int]$_.Key })
  $irregular = 0
  for ($i = 1; $i -lt $sorted.Count; $i++) { $k = [math]::Round(($sorted[$i] - $sorted[$i - 1]) * 1000, 0); if (-not ($modal | Where-Object { [math]::Abs($_ - $k) -le 1 })) { $irregular++ } }
  $spacingText = (($spacing.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 4 | ForEach-Object { "$($_.Key)ms x$($_.Value)" }) -join ' ')
  [pscustomobject]@{ Spacing = $spacingText; Irregular = $irregular; Modal = $modal;
    Duration = [double]$info.format.duration; Frames = $times.Count; DecodeErrors = $bad; ErrorText = $errorText; Backwards = $backwards; Gaps = $gaps
    Codec = $video.codec_name; Size = "$($video.width)x$($video.height)"; Audio = $audio.Count
    AudioDuration = if ($audio.Count) { [double]$audio[0].duration } else { 0.0 }; VideoDuration = [double]$video.duration
    First = if ($sorted.Count) { $sorted[0] } else { 0 }; Last = if ($sorted.Count) { $sorted[-1] } else { 0 }
  }
}

try {
  foreach ($codec in $Codecs) { foreach ($encoder in $Encoders) {
    $label = "$codec/$encoder"
    $e = $null
    try {
      $envs = @{}
      if (-not $NoSwaps) { $envs['BUFFRX_TEST_ENCODER_SWAP_MS'] = (($SwapAtSeconds | ForEach-Object { $_ * 1000 }) -join ',') }
      if ($Levels) { $envs['BUFFRX_TEST_ENCODER_SWAP_LEVELS'] = $Levels }
      if ($FailMode -gt 0) { $envs['BUFFRX_TEST_SWAP_FAIL'] = [string]$FailMode }
      $cfg = '{"config_version":7,"codec_auto_picked":true,"start_buffering":false,"auto_game_capture":false,"timeline_minutes":' + $(if ($Timeline) { '30,"timeline_everywhere":true' } else { '0' }) + ',"capture_fps":' + $Fps + ',"buffer_seconds":' + $BufferSeconds + ',"codec":"' + $codec + '","encoder":"' + $encoder + '","container":"mp4","audio_tracks":1}'
      $e = Start-ProbeEngine $cfg $Engine $envs
      Wait-Asleep $e
      if ($Motion) { $null = Set-WindowCapture $e }
      $null = Ask-Engine $e 'buffer.start'; Wait-Buffering $e
      $started = Get-Date
      # The engine's own clock starts at the first tick that saw the buffer on; allow the last swap a margin to finish.
      $want = if ($NoSwaps) { 0 } else { $SwapAtSeconds.Count }
      $waitFor = if ($NoSwaps) { 20 } else { $SwapAtSeconds[-1] }
      $swaps = 0
      for ($i = 0; $i -lt 240; $i++) {
        Start-Sleep -Milliseconds 500
        $s = Ask-Engine $e 'status.get'
        $swaps = [int](Num $s 'swaps')
        if ($swaps -ge $want -and ((Get-Date) - $started).TotalSeconds -gt ($waitFor + 3)) { break }
        if (((Get-Date) - $started).TotalSeconds -gt ($waitFor + 40)) { break }
      }
      $log = Join-Path $e.DataRoot 'logs\engine.log'
      $logText = if (Test-Path $log) { Get-Content $log -Raw } else { '' }
      if ($logText -match 'refused by BUFFRX_TEST_REFUSE_ENCODER' -or ($logText -match 'no usable encoder' -and $swaps -eq 0)) { $skipped++; $rows.Add("SKIP $label  no such encoder here"); continue }
      $running = ($s -match '"buffer_state":\s*"on"')
      $encoderUsed = [regex]::Match($s, '"encoder":\s*"([^"]*)"').Groups[1].Value
      if ($encoderUsed -and (($codec -eq 'av1' -and $encoderUsed -notmatch 'av1') -or ($codec -eq 'hevc' -and $encoderUsed -notmatch 'hevc|h265') )) { $skipped++; $rows.Add("SKIP $label  the engine used $encoderUsed instead"); continue }
      if ($encoder -eq 'software' -and $encoderUsed -notmatch 'x264') { if ($codec -ne 'h264') { $skipped++; $rows.Add("SKIP $label  no software encoder for $codec ($encoderUsed)"); continue } }
      $epochLines = @([regex]::Matches($logText, 'ring epoch \d+: [^\r\n]*') | ForEach-Object { $_.Value })
      $swapLines = @([regex]::Matches($logText, 'encoder swap: [^\r\n]*') | ForEach-Object { $_.Value })
      $held = [math]::Round((Num $s 'held_seconds'), 1)
      $rows.Add("INFO $label  encoder=$encoderUsed swaps=$swaps/$want buffer_on=$running held_seconds=$held epoch=$([int](Num $s 'epoch'))")
      foreach ($l in $swapLines) { $rows.Add("INFO $label  $l") }
      foreach ($l in $epochLines) { $rows.Add("INFO $label  $l") }
      if ($FailMode -eq 0 -and $swaps -lt $want) { $failures++; $rows.Add("FAIL $label  only $swaps of $want swaps happened"); continue }

            $spans = @(3, 7, 11, 15, 28) | Where-Object { $_ -le $BufferSeconds }
      # The longest clip needs the ring to hold that much first.
      for ($i = 0; $i -lt 120; $i++) { if ((Num (Ask-Engine $e 'status.get') 'held_seconds') -ge ($spans[-1] + 2)) { break }; Start-Sleep -Milliseconds 500 }
      $clips = Join-Path $e.DataRoot 'clips'
      foreach ($span in $spans) {
        $before = @(Get-ChildItem $clips -Recurse -File -Include '*.mp4' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -notlike '*\.buffrx\*' -and $_.Name -like 'Replay *' }).Count
        $null = Ask-Engine $e 'buffer.save' ('{"seconds":' + $span + '}')
        $file = $null
        for ($i = 0; $i -lt 90 -and -not $file; $i++) {
          Start-Sleep -Milliseconds 500
          $all = @(Get-ChildItem $clips -Recurse -File -Include '*.mp4' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -notlike '*\.buffrx\*' -and $_.Name -like 'Replay *' } | Sort-Object LastWriteTime)
          if ($all.Count -gt $before) {
            $f = $all[-1]; $size = $f.Length; Start-Sleep -Milliseconds 1200; $f.Refresh()
            if ($f.Length -eq $size -and $f.Length -gt 0) { $file = $f.FullName }
          }
        }
        if (-not $file) { $failures++; $rows.Add("FAIL $label  span ${span}s: no clip appeared"); continue }
        $keepDir = Join-Path $env:TEMP 'encoder-swap-clips'; New-Item -ItemType Directory -Force $keepDir | Out-Null
        Copy-Item $file (Join-Path $keepDir ("$codec-$encoder-${span}s.mp4")) -Force   # for a look afterwards; the probe's data root is removed
        $p = Probe-Clip $file
        # The clip starts on a keyframe at or before the cut, so it may be a keyframe interval (2 s) longer, never shorter by more than a frame or two.
        $okLength = ($p.Duration -ge ($span - 0.5)) -and ($p.Duration -le ($span + 4.5))
        $okAudio = ($p.Audio -ge 1) -and ([math]::Abs($p.AudioDuration - $p.VideoDuration) -lt 0.6)
        $expectedFrames = $p.Duration * $Fps
        $missing = [math]::Round($expectedFrames - $p.Frames, 1)
        # Pictures are evenly spaced on each side of a swap: only the seams (one picture per swap crossed) may be off the common spacings.
        $okFrames = ($p.Irregular -le 8) -and ($p.Modal.Count -le 2)
        $crossed = if ($p.Gaps.Count) { $p.Gaps.Count } else { 0 }
        $ok = $okLength -and $okAudio -and $okFrames -and ($p.DecodeErrors -eq 0) -and ($p.Backwards -eq 0) -and ($p.Codec -match $codec -or ($codec -eq 'hevc' -and $p.Codec -eq 'hevc'))
        if (-not $ok) { $failures++ }
        $keep = $ErrorActionPreference; $ErrorActionPreference = 'Continue'   # the engine's probes log to stderr
        $media = (& $e.EnginePath --media-frames $file 2>&1 | Where-Object { "$_" -like '{*' } | Select-Object -Last 1)
        $ErrorActionPreference = $keep
        $rows.Add(("{0} {1}  span={2}s  duration={3:N2}  frames={4}  irregular={5}  gaps={6}  decode_errors={7}  backwards={8}  audio={9} adur={10:N2} vdur={11:N2}  codec={12} {13}  media-frames: {14}  {15}  spacing: {16}" -f $(if ($ok) { 'ok  ' } else { 'FAIL' }), $label, $span, $p.Duration, $p.Frames, $p.Irregular, ($p.Gaps -join '/'), $p.DecodeErrors, $p.Backwards, $p.Audio, $p.AudioDuration, $p.VideoDuration, $p.Codec, $p.Size, $media, $p.ErrorText, $p.Spacing))
      }
      # The ring must still hold the whole buffer: the swaps did not empty it.
      $final = Ask-Engine $e 'status.get'
      if ($Timeline) { $rows.Add("INFO $label  timeline after the swaps: " + [regex]::Match($final, '"timeline":\{[^}]*\}').Value) }
      $rows.Add("INFO $label  after the saves: held_seconds=$([math]::Round((Num $final 'held_seconds'), 1)) buffer_on=$($final -match '"buffer_state":\s*"on"') swaps=$([int](Num $final 'swaps'))")
      $errors = @([regex]::Matches((Get-Content $log -Raw), '[^\r\n]*\] (error|ERROR)[^\r\n]*') | ForEach-Object { $_.Value })
      foreach ($l in @([regex]::Matches((Get-Content $log -Raw), 'a replay save crosses [^
]*') | ForEach-Object { $_.Value })) { $rows.Add("INFO $label  $l") }
      foreach ($l in ($errors | Select-Object -First 6)) { $rows.Add("INFO $label  log: $l") }
    } catch {
      $failures++
      $rows.Add("FAIL $label  line $($_.InvocationInfo.ScriptLineNumber): $($_.Exception.Message)")
    } finally {
      if ($e) { Copy-Item (Join-Path $e.DataRoot 'logs\engine.log') (Join-Path $env:TEMP "encoder-swap-$codec-$encoder.log") -Force -ErrorAction SilentlyContinue }
      Stop-ProbeEngine $e
    }
  } }
} finally {
  if ($loop) { Stop-Process -Id $loop.Id -Force -ErrorAction SilentlyContinue }
  if ($noise) { Stop-Process -Id $noise.Id -Force -ErrorAction SilentlyContinue }
}
$rows | ForEach-Object { $_ }
"encoder swap probe: $failures failed, $skipped skipped"
exit $(if ($failures) { 1 } else { 0 })
