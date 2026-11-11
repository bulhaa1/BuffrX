# A real-game soak on a throwaway engine: it captures a running game through the game hook (or WGC
# fallback), buffers, writes Timeline, adds markers, saves clips at a cadence, runs one manual
# recording span in the middle, saves a Timeline range at the end, and samples the engine every
# `Every` seconds. Clips and recordings are checked with the engine's own --media-info.
#
#   powershell -File tools\game-capture-soak.ps1 -Minutes 30
#   powershell -File tools\game-capture-soak.ps1 -Minutes 2 -Every 15 -ClipsEvery 0 -RecordEvery 0 -TimelineMinutes 0
#
# CSV: <TEMP>\buffrx-game-soak-<stamp>.csv. The engine root is printed for its engine.log, which is
# where a hook fallback is recorded. Exit 0 = every check passed, 1 = something failed.
param(
  [int]$Minutes = 30,
  [int]$Every = 30,
  [string]$Engine = 'buffrx-engine.exe',
  [string]$GameLike = 'KovaaK',
  [int]$ClipsEvery = 8,       # minutes between a saved clip; 0 = none
  [int]$RecordEvery = 15,     # minutes between a manual recording span; 0 = none
  [int]$RecordSeconds = 45,
  [int]$MarkersEvery = 2,     # minutes between Timeline markers; 0 = none
  [int]$TimelineMinutes = 30,
  [int]$TimelineSaveAt = 0,   # minute at which a Timeline range is saved; 0 = at the end
  [string]$Codec = 'h264',
  [int]$Fps = 60,
  [switch]$KeepData
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'probe-lib.ps1')

Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class SoakAwake { [DllImport("kernel32.dll")] public static extern uint SetThreadExecutionState(uint f); }
'@
$null = [SoakAwake]::SetThreadExecutionState(2147483651)  # system + display awake for the run
$stamp = Get-Date -Format 'MMdd-HHmmss'
$csvPath = Join-Path $env:TEMP "buffrx-game-soak-$stamp.csv"
$config = @{
  start_buffering = $false; auto_game_capture = $false; notifications = $false; notification_sound = $false
  min_free_gb = 1; buffer_seconds = 30; capture_fps = $Fps; codec = $Codec; quality = 'high'
  keyframe_seconds = 2; timeline_minutes = $TimelineMinutes; capture_mode = 'game'
} | ConvertTo-Json
$e = Start-ProbeEngine -ConfigJson $config -Engine $Engine
$clipsDir = Join-Path $e.DataRoot 'clips'
$logPath = Join-Path $e.DataRoot 'logs\engine.log'
$rows = New-Object System.Collections.Generic.List[object]
$failures = New-Object System.Collections.Generic.List[string]
$warnings = New-Object System.Collections.Generic.List[string]
$started = Get-Date
$prevCpu = $e.Process.TotalProcessorTime
$prevAt = Get-Date
$wasReady = $false
$gaps = 0
$bufferStops = 0
$lastClipAt = 0
$lastRecordAt = 0
$lastMarkerAt = 0
$timelineSaved = $null
$recordings = 0
$clips = 0

function Media-Info([string]$file) {
  $previous = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try { $info = & (Join-Path $ProbeBin 'buffrx-engine.exe') --media-info $file 2>$null } finally { $ErrorActionPreference = $previous }
  if ($LASTEXITCODE -ne 0) { return $null }
  try { return ($info -join "`n" | ConvertFrom-Json) } catch { return $null }
}

function Wait-NewFile([string]$beforeJson, [string]$filter = '*.mp4', [int]$seconds = 45) {
  $before = @($beforeJson | ConvertFrom-Json)
  $deadline = (Get-Date).AddSeconds($seconds)
  while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 500
    $new = @(Get-ChildItem $clipsDir -File -Filter $filter -Recurse -ErrorAction SilentlyContinue | Where-Object { $before -notcontains $_.FullName })
    if ($new.Count -gt 0) {
      $file = $new | Sort-Object LastWriteTime -Descending | Select-Object -First 1
      try {
        $size = (Get-Item -LiteralPath $file.FullName).Length; Start-Sleep -Milliseconds 700
        if ((Get-Item -LiteralPath $file.FullName).Length -eq $size) { return $file.FullName }
      } catch { }  # a finished save is moved into its game folder, so its first path can vanish mid-poll
    }
  }
  return $null
}

function Capture-FileNames {
  return (ConvertTo-Json -InputObject @(Get-ChildItem $clipsDir -File -Filter '*.mp4' -Recurse -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName) -Compress)
}

function Sample([double]$minute) {
  $s = $null
  try { $s = (Ask-Engine $e 'status.get') | ConvertFrom-Json } catch { }
  $d = $s.data
  $now = Get-Date
  $cpu = 0.0
  try {
    $p = Get-Process -Id $e.Process.Id
    $cpu = (($p.TotalProcessorTime - $script:prevCpu).TotalSeconds / [Math]::Max(($now - $script:prevAt).TotalSeconds, 0.1) / [Environment]::ProcessorCount) * 100
    $script:prevCpu = $p.TotalProcessorTime; $script:prevAt = $now
  } catch { }
  $gpu = ''; $vram = ''
  try { $nv = (& nvidia-smi --query-gpu=utilization.gpu,memory.used --format=csv,noheader,nounits 2>$null) -split ','; $gpu = [double]$nv[0].Trim(); $vram = [double]$nv[1].Trim() } catch { }
  $sessionFiles = 0; $sessionMb = 0
  try { $f = Get-ChildItem (Join-Path $e.DataRoot 'sessions') -Recurse -File -ErrorAction SilentlyContinue; $sessionFiles = @($f).Count; $sessionMb = [Math]::Round((($f | Measure-Object Length -Sum).Sum) / 1MB, 1) } catch { }
  $markers = 0
  try { $m = Ask-Engine $e 'markers.list'; $markers = ([regex]::Matches($m, '"at_ms"')).Count } catch { }
  $ready = $false; try { $ready = [bool]$d.capture_ready } catch { }
  if ($script:wasReady -and -not $ready) { $script:gaps++ }
  $script:wasReady = $ready
  $state = ''; try { $state = [string]$d.buffer_state } catch { }
  if ($state -and $state -ne 'on' -and $minute -gt 1) { $script:bufferStops++ }
  $row = [pscustomobject]@{
    minute = [Math]::Round($minute, 2); buffer = $state; capture_ready = $ready
    capture_mode = [string]$d.capture.mode; active_fps = $d.stats.active_fps
    video_frames = $d.stats.video_frames; video_lagged = $d.stats.video_lagged
    dropped = $d.stats.dropped; encoder_fps = $d.stats.encoder_fps; encoder_frames = $d.stats.encoder_frames
    encoder_ratio = $d.stats.encoder_ratio; encoder_behind = $d.stats.encoder_behind
    private_mb = [Math]::Round($d.stats.process_mb, 1); ws_mb = [Math]::Round($e.Process.WorkingSet64 / 1MB, 1)
    handles = $e.Process.HandleCount; threads = $e.Process.Threads.Count; cpu_pct = [Math]::Round($cpu, 2)
    gpu_pct = $gpu; vram_mb = $vram; free_gb = [Math]::Round((Get-PSDrive C).Free / 1GB, 1)
    ring_used_mb = $d.stats.ring.used_bytes / 1MB; ring_held_s = $d.stats.ring.held_seconds; ring_disk = [string]$d.stats.ring.disk
    timeline_state = [string]$d.timeline.state; timeline_writer = [string]$d.timeline.writer
    session_files = $sessionFiles; session_mb = $sessionMb; markers = $markers
    clips = $clips; recordings = $recordings; recording = [bool]$d.recording
  }
  $rows.Add($row) | Out-Null
  $row | Export-Csv -Path $csvPath -Append -NoTypeInformation
  return $row
}

try {
  $games = Ask-Engine $e 'sources.games'
  $id = $null
  if ($games -match ('"title":"([^"]*' + $GameLike + '[^"]*)"')) { $id = $Matches[1] }
  if (-not $id) {
    $wins = Ask-Engine $e 'sources.windows'
    if ($wins -match ('"title":"([^"]*' + $GameLike + '[^"]*)"')) { $id = $Matches[1] }
  }
  if (-not $id) { throw "no game window matching '$GameLike' in sources.games or sources.windows" }
  "game target: $id"
  $set = Ask-Engine $e 'capture.set' (@{ mode = 'game'; game_title = $id } | ConvertTo-Json -Compress)
  if ($set -notmatch '"ok":true') { throw "capture.set refused: $set" }
  $mode = ((Ask-Engine $e 'status.get') | ConvertFrom-Json).data.capture.mode
  if ($mode -eq 'display') {
    if ($TimelineMinutes -gt 0) { $warnings.Add('capture is following the display because Timeline writes and the game is not in front') }
    else { throw "capture.set left the capture on the display, not the game" }
  }
  "capture mode: $mode"
  $null = Ask-Engine $e 'buffer.start'
  $null = Wait-Buffering $e
  "buffering; CSV $csvPath"

  $deadline = $started.AddMinutes($Minutes)
  while ((Get-Date) -lt $deadline) {
    $minute = ((Get-Date) - $started).TotalMinutes
    $row = Sample $minute
    if ($ClipsEvery -gt 0 -and $minute -ge 1 -and ($minute - $lastClipAt) -ge $ClipsEvery) {
      $lastClipAt = $minute
      $before = Capture-FileNames
      $r = Ask-Engine $e 'buffer.save' '{"seconds":20}'
      $file = Wait-NewFile $before '*.mp4' 45
      if (-not $file) { $failures.Add("clip at minute $([Math]::Round($minute,1)): no file appeared") }
      else {
        $info = Media-Info $file
        if (-not $info -or -not $info.ok) { $failures.Add("clip $([IO.Path]::GetFileName($file)): media-info refused") }
        elseif ([double]$info.duration -lt 17 -or [double]$info.duration -gt 24) { $failures.Add("clip $([IO.Path]::GetFileName($file)): duration $($info.duration) s") }
        else { $clips++ }
      }
    }
    if ($RecordEvery -gt 0 -and $minute -ge 3 -and ($minute - $lastRecordAt) -ge $RecordEvery) {
      $lastRecordAt = $minute
      if ($row.recording) { $warnings.Add("recording already on at minute $([Math]::Round($minute,1))") }
      else {
        $before = Capture-FileNames
        $rs = Ask-Engine $e 'recording.start'
        if ($rs -notmatch '"recording":true') { $failures.Add("recording did not start at minute $([Math]::Round($minute,1))") }
        else {
          Start-Sleep -Seconds $RecordSeconds
          $null = Ask-Engine $e 'recording.stop'
          $file = Wait-NewFile $before '*.mp4' 30
          if (-not $file) { $failures.Add("recording at minute $([Math]::Round($minute,1)): no file") }
          else {
            $info = Media-Info $file
            if (-not $info -or -not $info.ok) { $failures.Add("recording $([IO.Path]::GetFileName($file)): media-info refused") }
            elseif ([double]$info.duration -lt ($RecordSeconds - 8) -or [double]$info.duration -gt ($RecordSeconds + 10)) { $failures.Add("recording $([IO.Path]::GetFileName($file)): duration $($info.duration) s, wanted about $RecordSeconds") }
            else { $recordings++ }
          }
        }
      }
    }
    if ($MarkersEvery -gt 0 -and $minute -ge 1 -and ($minute - $lastMarkerAt) -ge $MarkersEvery) {
      $lastMarkerAt = $minute
      $m = Ask-Engine $e 'markers.add' (@{ label = "soak $([Math]::Round($minute,1))" } | ConvertTo-Json -Compress)
      if ($m -notmatch '"ok":true') { $warnings.Add("marker at minute $([Math]::Round($minute,1)) was refused: $m") }
    }
    if ($TimelineSaveAt -gt 0 -and $minute -ge $TimelineSaveAt -and -not $timelineSaved) {
      $tl = (Ask-Engine $e 'timeline.get') | ConvertFrom-Json
      $end = [int64]$tl.data.available_end_ms
      if ($end -lt 25000) { $failures.Add('timeline range too short to save') }
      else {
        $save = Ask-Engine $e 'timeline.save' (@{ start_ms = ($end - 20000); end_ms = ($end - 1000); name = 'overnight-game-soak' } | ConvertTo-Json -Compress)
        $saved = $null
        try { $saved = ($save | ConvertFrom-Json).data.file } catch { }
        if (-not $saved) { $failures.Add("timeline.save at minute $([Math]::Round($minute,1)) failed: $save") }
        else {
          $info = Media-Info $saved
          if (-not $info -or -not $info.ok) { $failures.Add('timeline save: media-info refused') }
          elseif ([double]$info.duration -lt 12) { $failures.Add("timeline save: duration $($info.duration) s") }
          else { $timelineSaved = $saved }
        }
      }
    }
    $left = [Math]::Round(($deadline - (Get-Date)).TotalMinutes, 1)
    "minute $([Math]::Round($minute,1)): buffer $($row.buffer), ready $($row.capture_ready), active $($row.active_fps) fps, encoder $($row.encoder_fps) fps, private $($row.private_mb) MB, cpu $($row.cpu_pct)%, gpu $($row.gpu_pct)%, markers $($row.markers), clips $clips, recordings $recordings, $left min left"
    if ($left -le 0) { break }
    $sleep = [Math]::Min($Every, [Math]::Max(1, ($deadline - (Get-Date)).TotalSeconds))
    Start-Sleep -Seconds $sleep
  }

  if (-not $timelineSaved -and $TimelineMinutes -gt 0) {
    $tl = (Ask-Engine $e 'timeline.get') | ConvertFrom-Json
    $end = [int64]$tl.data.available_end_ms
    if ($end -ge 25000) {
      $save = Ask-Engine $e 'timeline.save' (@{ start_ms = ($end - 20000); end_ms = ($end - 1000); name = 'overnight-game-soak' } | ConvertTo-Json -Compress)
      try { $saved = ($save | ConvertFrom-Json).data.file } catch { $saved = $null }
      if (-not $saved) { $failures.Add("timeline.save at the end failed: $save") }
      else { $info = Media-Info $saved; if ($info -and $info.ok -and [double]$info.duration -ge 12) { $timelineSaved = $saved } else { $failures.Add('timeline save at the end: media-info refused or short') } }
    } else { $warnings.Add("timeline had only $end ms at the end; no range saved") }
  }

  $fallbacks = 0
  if (Test-Path $logPath) { $fallbacks = ([regex]::Matches((Get-Content $logPath -Raw), 'capturing the game.s window instead')).Count }
  if ($fallbacks -gt 0) { $warnings.Add("the game hook fell back to window capture $fallbacks time(s)") }
  if ($bufferStops -gt 0) { $failures.Add("the buffer left the on state in $bufferStops sample(s)") }
  if ($gaps -gt 0) { $failures.Add("capture_ready went false $gaps time(s) after being true") }
  if ($rows.Count -gt 0) {
    $last = $rows[$rows.Count - 1]
    if ($last.private_mb -gt 900) { $warnings.Add("private bytes reached $($last.private_mb) MB") }
  }
  ""
  "SOAK $stamp on '$id': $($rows.Count) samples, $clips clips, $recordings recordings, timeline save $(if ($timelineSaved) { 'ok' } else { 'none' }), hook fallbacks $fallbacks"
  "CSV: $csvPath"
  "engine root: $($e.DataRoot)"
  if (Test-Quiet ($Minutes * 60)) { "quiet: nobody touched the machine during the run" } else { $warnings.Add('the machine was not quiet (input arrived during the run)') }
  foreach ($w in $warnings) { "WARN: $w" }
  foreach ($f in $failures) { "FAIL: $f" }
  if ($failures.Count -gt 0 -and (Test-Path $logPath)) { "engine log tail:"; Get-Content $logPath -Tail 15 }
  if ($failures.Count -gt 0) { exit 1 }
  "PASS"
  exit 0
}
finally {
  $null = [SoakAwake]::SetThreadExecutionState(2147483648)
  if ($e) { Stop-ProbeEngine $e -KeepData:$KeepData }
}
