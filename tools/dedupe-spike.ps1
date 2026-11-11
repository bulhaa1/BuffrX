# Gate 0 of the Timeline dedupe plan (docs/review-timeline-mux-2026-11-02.md): can a Timeline chunk be tied to the replay ring by what is in it?
# For each codec x audio tracks, a throwaway engine runs a Timeline session beside the buffer with BUFFRX_RING_KEYLOG on (one line per ring packet with the
# hash of packet_hash.h), saves a replay clip, stops, and then every chunk is read back with `--media-packets` and matched to the ring's log:
#   (a) the chunk's first video packet is a keyframe the ring also saw (same picture hash and size). A still picture makes identical keyframes, so a hash can
#       match several ring positions; those are resolved by position: a chunk begins where the one before it ended, so one unique link anchors its neighbours;
#   (a1) every video packet of the chunk equals the ring's run from there (nothing dropped, duplicated or re-ordered), and each chunk starts right after the last;
#   (a2) each audio track of the chunk is the ring's run that starts at the chunk's first video time (silent AAC frames are identical, so audio is tied by
#       time, not by content), give or take one frame, and its bytes are that run's;
#   (b) the chunk and the replay clip carry the same stream signature (what `media_concat` checks) and `--media-concat chunk chunk replay` joins them.
# Needs the picture to change; the person using the machine is enough.
#
#   powershell -File tools\dedupe-spike.ps1 -Engine buffrx-engine-ring.exe -Codecs h264,hevc,av1 -Tracks 1,3 -Seconds 45
param(
  [string[]]$Codecs = @('h264', 'hevc', 'av1'),
  [string[]]$Tracks = @('1', '3'),
  [int]$Seconds = 45,
  [string]$Engine = 'buffrx-engine.exe',
  [switch]$NoNoise,
  [switch]$Detail   # print, per chunk, its packet count, where it links in the ring's log and where the next one links
)
. (Join-Path $PSScriptRoot 'probe-lib.ps1')
$Codecs = @($Codecs | ForEach-Object { $_ -split ',' }); $Tracks = @($Tracks | ForEach-Object { [string]$_ -split ',' } | ForEach-Object { [int]$_ })
$exe = Join-Path $script:ProbeBin $Engine
$noise = if ($NoNoise) { $null } else { Start-Process powershell -ArgumentList '-NoProfile', '-File', (Join-Path $PSScriptRoot 'screen-noise.ps1') -PassThru -WindowStyle Hidden }
$rows = @(); $failed = 0

function Read-Packets([string]$file) {
  $before = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
  $text = & $exe --media-packets $file 2>$null
  $ErrorActionPreference = $before
  $sig = ''; $video = New-Object System.Collections.Generic.List[object]; $audio = @{}
  foreach ($line in $text) {
    $p = "$line" -split ' '
    if ($p[0] -eq 'sig') { $sig = ($p[1..($p.Count - 1)] -join ' ') }
    elseif ($p[0] -eq 'v') { $video.Add([pscustomobject]@{ Key = $p[1] -eq '1'; Size = [uint32]$p[2]; Vcl = [uint32]$p[3]; Hash = $p[4] }) }
    elseif ($p[0] -eq 'a') { if (-not $audio.ContainsKey($p[1])) { $audio[$p[1]] = New-Object System.Collections.Generic.List[string] }; $audio[$p[1]].Add($p[3]) }
  }
  [pscustomobject]@{ Signature = $sig; Video = $video; Audio = $audio }
}

try {
  foreach ($codec in $Codecs) { foreach ($trackCount in $Tracks) {
    $label = "$codec x$trackCount"
    $e = $null
    $keylog = Join-Path $env:TEMP ('ring-keys-' + [guid]::NewGuid().ToString('N').Substring(0, 8) + '.txt')
    try {
      $cfg = '{"config_version":7,"start_buffering":false,"auto_game_capture":false,"timeline_minutes":30,"keyframe_seconds":4,"capture_fps":30,"output_width":1280,"output_height":720,"codec":"' + $codec + '","audio_tracks":' + $trackCount + '}'
      $e = Start-ProbeEngine $cfg $Engine @{ BUFFRX_RING_KEYLOG = $keylog }
      Wait-Asleep $e
      $null = Ask-Engine $e 'buffer.start'; Wait-Buffering $e
      for ($i = 0; $i -lt 60 -and -not ((Ask-Engine $e 'status.get') -match '"writer":\s*true'); $i++) { Start-Sleep -Milliseconds 500 }
      Start-Sleep -Seconds $Seconds
      $session = [regex]::Match((Ask-Engine $e 'timeline.get'), '"current_session":"([^"]*)"').Groups[1].Value
      $null = Ask-Engine $e 'buffer.save' '{"seconds":15}'
      $clips = Join-Path $e.DataRoot 'clips'
      $replay = $null
      for ($i = 0; $i -lt 60 -and -not $replay; $i++) {
        Start-Sleep -Milliseconds 500
        $f = Get-ChildItem $clips -Recurse -File -Include *.mp4 -ErrorAction SilentlyContinue | Where-Object { $_.FullName -notlike '*\sessions\*' } | Select-Object -First 1
        if ($f -and $f.Length -gt 0) { $size = $f.Length; Start-Sleep -Milliseconds 1200; $f.Refresh(); if ($f.Length -eq $size) { $replay = $f.FullName } }
      }
      $null = Ask-Engine $e 'buffer.stop'
      Start-Sleep -Seconds 10
      $media = Join-Path $clips "sessions\$session\media"
      # In the order they were written. Not by name: the first file of a session is "Recording <stamp>.mp4" and the splitter names the rest from the file name format.
      $chunks = @(Get-ChildItem $media -Filter *.mp4 -ErrorAction SilentlyContinue | Sort-Object CreationTimeUtc | Select-Object -ExpandProperty FullName)
      # The ring's log.
      $ringVideo = New-Object System.Collections.Generic.List[object]; $ringAudio = @{}
      foreach ($line in [IO.File]::ReadAllLines($keylog)) {
        $p = $line -split ' '
        if ($p[0] -eq 'v') { $ringVideo.Add([pscustomobject]@{ Dts = [int64]$p[1]; Key = $p[2] -eq '1'; Size = [uint32]$p[3]; Vcl = [uint32]$p[4]; Hash = $p[5] }) }
        elseif ($p[0] -eq 'a') { if (-not $ringAudio.ContainsKey($p[1])) { $ringAudio[$p[1]] = New-Object System.Collections.Generic.List[object] }; $ringAudio[$p[1]].Add([pscustomobject]@{ Dts = [int64]$p[2]; Hash = $p[4] }) }
      }
      # Every ring position of each keyframe picture.
      $ringKeys = @{}
      for ($i = 0; $i -lt $ringVideo.Count; $i++) {
        if ($ringVideo[$i].Key -and $ringVideo[$i].Vcl -gt 0) { $k = "$($ringVideo[$i].Vcl)/$($ringVideo[$i].Hash)"; if (-not $ringKeys.ContainsKey($k)) { $ringKeys[$k] = New-Object System.Collections.Generic.List[int] }; $ringKeys[$k].Add($i) }
      }
      $reports = @($chunks | ForEach-Object { Read-Packets $_ })
      $n = $chunks.Count
      # Candidates for each chunk's first picture, then resolution by position: a unique link anchors the chunks around it.
      $cands = @(); $resolved = @(); $ambiguous = 0
      for ($c = 0; $c -lt $n; $c++) {
        $r = $reports[$c]
        $list = @()
        if ($r.Video.Count -gt 0 -and $r.Video[0].Key) { $k = "$($r.Video[0].Vcl)/$($r.Video[0].Hash)"; if ($ringKeys.ContainsKey($k)) { $list = @($ringKeys[$k]) } }
        $cands += , $list
        $resolved += $(if ($list.Count -eq 1) { $list[0] } else { -1 })
        if ($list.Count -gt 1) { $ambiguous++ }
      }
      for ($pass = 0; $pass -lt $n; $pass++) {
        for ($c = 0; $c -lt $n; $c++) {
          if ($resolved[$c] -lt 0) { continue }
          if ($c + 1 -lt $n -and $resolved[$c + 1] -lt 0) { $want = $resolved[$c] + $reports[$c].Video.Count; if ($cands[$c + 1] -contains $want) { $resolved[$c + 1] = $want } }
          if ($c -gt 0 -and $resolved[$c - 1] -lt 0) { $want = $resolved[$c] - $reports[$c - 1].Video.Count; if ($cands[$c - 1] -contains $want) { $resolved[$c - 1] = $want } }
        }
      }
      if ($Detail) {
        "$label ring log: $($ringVideo.Count) video packets, last dts $($ringVideo[$ringVideo.Count - 1].Dts)"
        for ($c = 0; $c -lt $n; $c++) {
          $at = $resolved[$c]
          "  chunk $c $([IO.Path]::GetFileName($chunks[$c])): $($reports[$c].Video.Count) video packets, links at $at (ends at $($at + $reports[$c].Video.Count)), candidates $($cands[$c] -join ','), next links at $(if ($c + 1 -lt $n) { $resolved[$c + 1] } else { '-' })"
        }
      }
      $firstOk = 0; $videoOk = 0; $audioOk = 0; $contiguous = $true; $note = ''
      $replaySig = if ($replay) { (Read-Packets $replay).Signature } else { '' }
      $sigOk = 0
      for ($c = 0; $c -lt $n; $c++) {
        $r = $reports[$c]; $name = [IO.Path]::GetFileName($chunks[$c]); $at = $resolved[$c]
        if ($r.Video.Count -eq 0) { $note += " empty:$name"; continue }
        if ($at -ge 0) { $firstOk++ } else { $note += " nolink($($cands[$c].Count)):$name" }
        if ($at -ge 0) {
          # Every video packet of the chunk is the ring's packet at the same place. A chunk may run past the end of the ring's log (the ring stops a moment
          # before the last file does): the packets both have must agree, and the shortfall is said.
          $have = [math]::Min($r.Video.Count, $ringVideo.Count - $at)
          $same = $true
          for ($i = 0; $i -lt $have; $i++) { $a = $r.Video[$i]; $b = $ringVideo[$at + $i]; if ($a.Vcl -ne $b.Vcl -or $a.Hash -ne $b.Hash -or $a.Key -ne $b.Key) { $same = $false; $note += " vdiff@$($i):$name"; break } }
          if ($same -and $have -lt $r.Video.Count) { $note += " short($($r.Video.Count - $have)):$name" }
          if ($same) { $videoOk++ }
          if ($c + 1 -lt $n -and $resolved[$c + 1] -ge 0 -and $resolved[$c + 1] -ne $at + $r.Video.Count) { $contiguous = $false; $note += " gap-after:$name" }
        }
        # Audio is tied to the video by time, not by content.
        $allAudio = $true
        if ($at -ge 0) {
          $d0 = $ringVideo[$at].Dts
          foreach ($t in $r.Audio.Keys) {
            $list = $r.Audio[$t]; $ring = $ringAudio[$t]
            if (-not $ring -or $list.Count -eq 0) { $allAudio = $false; continue }
            $s = 0; while ($s -lt $ring.Count -and $ring[$s].Dts -lt $d0 - 30000) { $s++ }
            $matched = $false
            foreach ($shift in 0, 1, -1, 2, -2, 3) {
              $from = $s + $shift
              if ($from -lt 0 -or $from -ge $ring.Count) { continue }
              $m = [math]::Min($list.Count, $ring.Count - $from); $ok = $true
              for ($i = 0; $i -lt $m; $i++) { if ($list[$i] -ne $ring[$from + $i].Hash) { $ok = $false; break } }
              if ($ok) { $matched = $true; if ($shift -ne 0) { $note += " ashift(t$t=$shift)" }; break }
            }
            if (-not $matched) { $allAudio = $false; $note += " adiff(t${t}):$name" }
          }
        } else { $allAudio = $false }
        if ($allAudio -and $r.Audio.Count -eq $trackCount) { $audioOk++ }
        if ($replaySig -and $r.Signature -eq $replaySig) { $sigOk++ }
      }
      # (b) a real join: two chunks and the replay clip.
      $join = 'skipped'
      if ($replay -and $n -ge 2) {
        $out = Join-Path $e.DataRoot 'joined.mp4'
        $before = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
        $json = (& $exe --media-concat $out $chunks[0] $chunks[1] $replay 2>$null | Select-Object -Last 1)
        $ErrorActionPreference = $before
        $join = if ($json -match '"ok":true') { 'ok ' + [regex]::Match($json, '"duration":([0-9.]+)').Groups[1].Value + ' s' } else { 'FAIL ' + $json }
      }
      $good = ($n -ge 3) -and ($firstOk -eq $n) -and ($videoOk -eq $n) -and ($audioOk -eq $n) -and $contiguous -and ($sigOk -eq $n) -and ($join -like 'ok*')
      if (-not $good) { $failed++ }
      $rows += [pscustomobject]@{ Case = $label; Result = $(if ($good) { 'ok' } else { 'FAIL' }); Chunks = $n; 'First key linked' = "$firstOk/$n"; 'Ambiguous by hash' = "$ambiguous/$n"; 'Video runs equal' = "$videoOk/$n"
        'Chunks back to back' = $contiguous; 'Audio runs equal' = "$audioOk/$n"; 'Signature = replay' = "$sigOk/$n"; 'Join' = $join; 'Ring packets' = $ringVideo.Count; Note = $note.Trim() }
    } catch {
      $failed++
      $rows += [pscustomobject]@{ Case = $label; Result = 'FAIL: ' + $_.Exception.Message }
    } finally { Stop-ProbeEngine $e; Remove-Item $keylog -Force -ErrorAction SilentlyContinue }
  } }
} finally { if ($noise) { Stop-Process -Id $noise.Id -Force -ErrorAction SilentlyContinue } }
$rows | ForEach-Object { ($_.PSObject.Properties | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join '  ' }
"dedupe spike: $($rows.Count - $failed) of $($rows.Count) cases passed"
exit $(if ($failed) { 1 } else { 0 })
