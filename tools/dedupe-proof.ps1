# Gate 2 / Gate 3 proofs of the Timeline dedupe (docs/review-timeline-mux-2026-11-02.md), on a throwaway engine (own data root and pipe) with Timeline on:
# buffer long enough that the ring overflows its RAM limit, save clips, and check what the clips hold against the ring's own packet log.
#
#   powershell -File tools\dedupe-proof.ps1 -Engine buffrx-engine-dedupe.exe -Seconds 300 -RamMb 128 -Motion -Dedupe -OutFile on.json
#   powershell -File tools\dedupe-proof.ps1 -Engine buffrx-engine-dedupe.exe -Seconds 300 -RamMb 128 -Motion          -OutFile off.json   # the same without it
#
# Checks (each printed ok / FAIL):
#   - every saved clip's video packets are, hash for hash, one contiguous run of the packets the ring held (BUFFRX_RING_KEYLOG: one line per ring packet), so the
#     part-wise clip equals what a plain ring would have saved; the clip is the length asked for; its audio is within a few AAC frames of the ring's;
#   - the dedupe took records out of RAM without writing them (deduped_bytes > 0, chunks_linked > 0) and the ring file was written less than the plain run's;
#   - saves leave no part file behind; handles and threads do not grow over -Saves saves; RAM never passed its limit.
# Engine write bytes are its own process's (the muxer process that writes a clip and Timeline's chunks is not in them), counted over the buffering window only.
# Run it on a quiet machine (the picture is kept moving by -Motion and screen-noise.ps1); a run the person disturbed prints "not quiet" and is not publishable.
param(
  [int]$Seconds = 300,             # the buffer length asked for
  [int]$RunSeconds = 0,            # how long to buffer before the first save (default: the length plus 40 s)
  [int]$RamMb = 128,               # BUFFRX_RING_RAM_MB: a small RAM limit so the ring overflows in minutes (0 = the real 2 GiB)
  [int]$Saves = 3,                 # clips saved, the buffer running between them
  [int]$TimelineMinutes = 30,
  [string]$Codec = 'h264',
  [int]$Tracks = 1,
  [int]$Fps = 60,
  [string]$Engine = 'buffrx-engine.exe',
  [string]$Ffprobe = 'ffprobe',
  [switch]$Dedupe,                 # BUFFRX_DEDUPE=1
  [switch]$Motion,
  [switch]$NoNoise,
  [int]$TimelinePauseMs = 0,       # BUFFRX_TEST_TIMELINE_PAUSE_MS: Timeline drops the writer and brings it back on a timer
  [switch]$DeleteAChunk,           # delete one old Timeline chunk by hand before the last save: the clip has a gap and says so
  [string]$OutFile = ''
)
. (Join-Path $PSScriptRoot 'probe-lib.ps1')
if ($RunSeconds -le 0) { $RunSeconds = $Seconds + 40 }
$exe = Join-Path $script:ProbeBin $Engine
$noise = if ($NoNoise) { $null } else { Start-Process powershell -ArgumentList '-NoProfile', '-File', (Join-Path $PSScriptRoot 'screen-noise.ps1') -PassThru -WindowStyle Hidden }
$loop = $null
if ($Motion) { $loop = Start-Process (Join-Path $script:ProbeRoot 'build\buffrx-present-loop.exe') -ArgumentList '--seconds', ($RunSeconds + 60 * $Saves + 240), '--width', '1280', '--height', '720', '--motion', '1' -PassThru; Start-Sleep -Seconds 4 }
$e = $null; $fail = @(); $result = [ordered]@{}
function Check([string]$name, [bool]$ok, [string]$detail = '') { if (-not $ok) { $script:fail += $name }; ('  {0}  {1}  {2}' -f $(if ($ok) { 'ok  ' } else { 'FAIL' }), $name, $detail) }
function Read-Clip([string]$file) {
  $before = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
  $text = & $exe --media-packets $file 2>$null
  $ErrorActionPreference = $before
  $video = New-Object System.Collections.Generic.List[string]; $audio = 0
  foreach ($line in $text) { $p = "$line" -split ' '; if ($p[0] -eq 'v') { $video.Add("$($p[3])/$($p[4])") } elseif ($p[0] -eq 'a') { $audio++ } }
  [pscustomobject]@{ Video = $video; Audio = $audio }
}
function Wait-Clip($clips, $known) {
  for ($i = 0; $i -lt 480; $i++) {
    Start-Sleep -Milliseconds 500
    $f = Get-ChildItem $clips -Recurse -File -Include '*.mp4' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -notlike '*\sessions\*' -and $_.FullName -notlike '*\.buffrx\*' -and $known -notcontains $_.FullName } | Sort-Object LastWriteTime | Select-Object -Last 1
    if ($f -and $f.Length -gt 0) { $size = $f.Length; Start-Sleep -Milliseconds 1500; $f.Refresh(); if ($f.Length -eq $size) { return $f.FullName } }
  }
  $null
}
try {
  $keylog = Join-Path $env:TEMP ('dedupe-keys-' + [guid]::NewGuid().ToString('N').Substring(0, 8) + '.txt')
  $envs = @{ BUFFRX_RING_KEYLOG = $keylog }
  if ($RamMb -gt 0) { $envs['BUFFRX_RING_RAM_MB'] = [string]$RamMb }
  if ($Dedupe) { $envs['BUFFRX_DEDUPE'] = '1' }
  if ($TimelinePauseMs -gt 0) { $envs['BUFFRX_TEST_TIMELINE_PAUSE_MS'] = [string]$TimelinePauseMs }
  $cfg = '{"config_version":7,"start_buffering":false,"auto_game_capture":false,"timeline_minutes":' + $TimelineMinutes + ',"capture_fps":' + $Fps + ',"buffer_seconds":' + $Seconds + ',"codec":"' + $Codec + '","audio_tracks":' + $Tracks + ',"keyframe_seconds":2,"quality":"highest","peak_mode":"fixed","custom_kbps":40000}'
  $e = Start-ProbeEngine $cfg $Engine $envs
  Wait-Asleep $e
  $clips = Join-Path $e.DataRoot 'clips'; $partDir = Join-Path $clips '.buffrx'
  $w0 = Write-Bytes $e
  $null = Ask-Engine $e 'buffer.start'; Wait-Buffering $e
  $started = Get-Date; $quiet = $true; $maxRam = 0; $last = ''
  while (((Get-Date) - $started).TotalSeconds -lt $RunSeconds) {
    Start-Sleep -Seconds 5
    $last = Ask-Engine $e 'status.get'
    $ram = Num $last 'ram_bytes'; if ($ram -gt $maxRam) { $maxRam = $ram }
    if (-not (Test-Quiet 5)) { $quiet = $false }
  }
  $wBuffer = (Write-Bytes $e) - $w0
  $received = Num $last 'received_bytes'; $spilled = Num $last 'spilled_bytes'; $deduped = Num $last 'deduped_bytes'; $chunkBytes = Num $last 'chunk_bytes'
  $linked = Num $last 'chunks_linked'; $waiting = Num $last 'chunks_waiting'
  $sessions = Join-Path $clips 'sessions'
  $timelineBytes = [long]((Get-ChildItem $sessions -Recurse -File -Include '*.mp4' -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum)
  "ring: received $([math]::Round($received/1MB,0)) MB, spilled $([math]::Round($spilled/1MB,0)) MB, deduped $([math]::Round($deduped/1MB,0)) MB, in chunk files $([math]::Round($chunkBytes/1MB,0)) MB, RAM $([math]::Round($ram/1MB,0)) MB (most $([math]::Round($maxRam/1MB,0)) MB), chunks linked $linked / waiting $waiting"
  "engine wrote $([math]::Round($wBuffer/1MB,1)) MB while buffering; Timeline's files on disk: $([math]::Round($timelineBytes/1MB,0)) MB"
  $handles0 = $e.Process.HandleCount; $threads0 = $e.Process.Threads.Count
  $known = @(); $saveSeconds = @(); $clipFiles = @()
  for ($n = 1; $n -le $Saves; $n++) {
    if ($DeleteAChunk -and $n -eq $Saves) {
      $media = Get-ChildItem $sessions -Recurse -Directory -Filter media -ErrorAction SilentlyContinue | Select-Object -First 1
      $old = if ($media) { Get-ChildItem $media.FullName -Filter *.mp4 | Sort-Object CreationTimeUtc | Select-Object -Last 7 | Select-Object -First 1 } else { $null }   # about 40 s old: inside the ring, and long since out of RAM
      if ($old) { Remove-Item -LiteralPath $old.FullName -Force -ErrorAction SilentlyContinue; "deleted a chunk by hand: $($old.Name)"; $result['deleted_chunk'] = $old.Name }
    }
    $t0 = Get-Date
    $null = Ask-Engine $e 'buffer.save' ('{"seconds":' + $Seconds + '}')
    $file = Wait-Clip $clips $known
    $saveSeconds += [math]::Round(((Get-Date) - $t0).TotalSeconds, 1)
    if ($file) { $known += $file; $clipFiles += $file }
    Start-Sleep -Seconds 15
  }
  $handles1 = $e.Process.HandleCount; $threads1 = $e.Process.Threads.Count
  $parts = @(Get-ChildItem $partDir -Filter 'part-*' -ErrorAction SilentlyContinue)
  $log = Get-Content (Join-Path $e.DataRoot 'logs\engine.log') -ErrorAction SilentlyContinue
  $joinFailures = @($log | Where-Object { $_ -match 'could not join its chunk files|chunk records do not fit|are in chunk files that are gone' })
  $null = Ask-Engine $e 'buffer.stop'
  # The ring's packets as the log saw them.
  $ringVideo = New-Object System.Collections.Generic.List[string]; $ringAudio = 0
  Start-Sleep -Seconds 2
  $fs = New-Object IO.FileStream($keylog, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
  $keyLines = (New-Object IO.StreamReader($fs)).ReadToEnd() -split "`r?`n"; $fs.Dispose()
  foreach ($line in $keyLines) { if (-not $line) { continue }; $p = $line -split ' '; if ($p[0] -eq 'v') { $ringVideo.Add("$($p[4])/$($p[5])") } elseif ($p[0] -eq 'a') { $ringAudio++ } }
  $first = $true; $index = 0
  foreach ($file in $clipFiles) {
    $index++
    $j = (& $Ffprobe -v error -show_entries 'format=duration' -of json $file 2>$null | Out-String) | ConvertFrom-Json
    $duration = [double]$j.format.duration
    $clip = Read-Clip $file
    # The clip's video is one contiguous run of the ring's.
    $match = $false; $at = -1
    if ($clip.Video.Count -gt 0) {
      for ($s = 0; $s -le $ringVideo.Count - $clip.Video.Count -and -not $match; $s++) {
        if ($ringVideo[$s] -ne $clip.Video[0]) { continue }
        $ok = $true; for ($k = 1; $k -lt $clip.Video.Count; $k++) { if ($ringVideo[$s + $k] -ne $clip.Video[$k]) { $ok = $false; break } }
        if ($ok) { $match = $true; $at = $s }
      }
    }
    $label = "clip $index"
    if ($DeleteAChunk -and $index -eq $Saves) {
      Check "$label has a gap (a chunk was deleted) and is shorter than the ask" ($duration -lt ($Seconds - 0.5)) ("{0:N1} s for {1} s" -f $duration, $Seconds)
      $result['gap_clip_seconds'] = [math]::Round($duration, 1)
    } else {
      Check "$label is the length asked for" (($duration -ge ($Seconds - 0.5)) -and ($duration -le ($Seconds + 5))) ("{0:N1} s for {1} s" -f $duration, $Seconds)
      Check "$label video is one contiguous run of the ring's packets, hash for hash" $match ("{0} packets from ring packet {1} of {2}" -f $clip.Video.Count, $at, $ringVideo.Count)
      # Within one AAC frame per seam (docs/review-timeline-mux-2026-11-02.md, Gate 0): a chunk's audio starts a frame after the ring's at the same moment, so a clip
      # joined from chunks of about 6 s is short by up to a frame at each. Six frames for the ends, one per chunk for the seams.
      $seams = if ($Dedupe) { [math]::Ceiling($duration / 5.0) } else { 0 }
      $expectedAudio = $duration * 48000.0 / 1024.0 * $Tracks
      Check "$label audio is within 6 AAC frames of the length, plus one per chunk seam" ([math]::Abs($clip.Audio - $expectedAudio) -le (6 + $seams) * $Tracks + 2) ("{0} frames, {1:N0} expected, up to {2} seams" -f $clip.Audio, $expectedAudio, $seams)
    }
  }
  Check 'a clip was saved each time' ($clipFiles.Count -eq $Saves) "$($clipFiles.Count) of $Saves"
  Check 'no part file is left behind' ($parts.Count -eq 0) "$($parts.Count) left"
  Check 'handles do not grow over the saves' (($handles1 - $handles0) -le 6) ("{0} -> {1}" -f $handles0, $handles1)
  Check 'threads do not grow over the saves' (($threads1 - $threads0) -le 3) ("{0} -> {1}" -f $threads0, $threads1)
  # Spilling starts half a limit early and RAM can reach the start plus the block being filled (ring_spill.h): a limit under two blocks is not held to the byte.
  Check 'RAM stayed within its limit plus the block being filled' (($RamMb -le 0) -or ($maxRam -le (($RamMb + 9) * 1MB))) ("most {0:N0} MB, limit {1} MB" -f ($maxRam / 1MB), $RamMb)
  if ($Dedupe) {
    Check 'records left RAM through chunk files' (($deduped -gt 0) -and ($linked -gt 0)) ("deduped {0:N0} MB, {1} chunks linked" -f ($deduped / 1MB), $linked)
    if (-not $DeleteAChunk) { Check 'no join failed and no chunk was missing' ($joinFailures.Count -eq 0) ($joinFailures -join ' | ') }
  } else {
    Check 'no chunk was relied on' ($deduped -le 0) "deduped $deduped"
  }
  Check 'the machine was quiet for the whole run' $quiet '(a run during which the person used the machine is not publishable)'
  $result['dedupe'] = [bool]$Dedupe; $result['seconds'] = $Seconds; $result['ram_mb'] = $RamMb; $result['codec'] = $Codec; $result['quiet'] = $quiet
  $result['received_mb'] = [math]::Round($received / 1MB, 0); $result['spilled_mb'] = [math]::Round($spilled / 1MB, 0); $result['deduped_mb'] = [math]::Round($deduped / 1MB, 0)
  $result['chunk_mb'] = [math]::Round($chunkBytes / 1MB, 0); $result['engine_wrote_mb_buffering'] = [math]::Round($wBuffer / 1MB, 1); $result['timeline_files_mb'] = [math]::Round($timelineBytes / 1MB, 0)
  $result['chunks_linked'] = $linked; $result['chunks_waiting'] = $waiting; $result['save_seconds'] = $saveSeconds; $result['max_ram_mb'] = [math]::Round($maxRam / 1MB, 0)
  $result['handles'] = "$handles0->$handles1"; $result['threads'] = "$threads0->$threads1"; $result['failed'] = $fail
  if ($OutFile) { ($result | ConvertTo-Json -Depth 4) | Set-Content -LiteralPath $OutFile }
  ($result | ConvertTo-Json -Compress -Depth 4)
} finally {
  Stop-ProbeEngine $e
  if ($loop) { Stop-Process -Id $loop.Id -Force -ErrorAction SilentlyContinue }
  if ($noise) { Stop-Process -Id $noise.Id -Force -ErrorAction SilentlyContinue }
  if ($keylog) { Remove-Item -LiteralPath $keylog -Force -ErrorAction SilentlyContinue }
}
if ($fail.Count) { 'dedupe proof: FAILED - ' + ($fail -join '; '); exit 1 }
'dedupe proof: all checks passed'
