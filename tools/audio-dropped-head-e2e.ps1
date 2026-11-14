# What this checks: a Timeline save or a range export whose chunks cross a stream-layout change keeps the
# audio labels of the chunks it really copies - and only those. The join drops the part before the layout
# that holds the range's end; the dropped part's stream count and lane names must not reach the output's
# sidecar, and a join whose sound cannot be described is refused before a clip is published.
#
# Real MP4s from work/frozen-audio-data/clips/Recordings are copied into a throwaway data root, the session
# index is written by hand (a closed Timeline session whose chunks point at those copies), and the native
# commands are asked: `timeline.save` and `clips.export_range`. Every check reads the published file, its
# sidecar or the command's own answer.
#
#   A. head with a different stream count  -> the head is dropped; the tail's labels stay complete.
#   B. head with the same count but a different lane -> the head is dropped; its microphone label and its
#      incomplete mix never touch the tail's labels.
#   C. no drop -> the labels of both chunks join as before.
#   D. a mix joined with an import -> refused before anything is published (no clip, no sidecar).
#   E. clips.export_range on the same session -> its published labels describe the chunks it copied and no
#      staging sidecar is left behind.
#
# The engine is run from a private copy of the exe inside a test runtime, its own data root and its own pipe.
# Nothing here stops another BuffrX: only the process this script started is ended.
param(
  [switch]$KeepData,
  [string]$Engine = 'runtime\bin\64bit\buffrx-engine.exe',
  # A test runtime (its bin\64bit has obs.dll and the FFmpeg DLLs, data\libobs and obs-plugins).
  [string]$RuntimeRoot = 'work\frozen-audio-runtime',
  [string]$Source = 'work\frozen-audio-data\clips\Recordings'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$engineGiven = if ([IO.Path]::IsPathRooted($Engine)) { $Engine } else { Join-Path $root $Engine }
$runtime = if ([IO.Path]::IsPathRooted($RuntimeRoot)) { $RuntimeRoot } else { Join-Path $root $RuntimeRoot }
$sourceDir = if ([IO.Path]::IsPathRooted($Source)) { $Source } else { Join-Path $root $Source }
$askScript = Join-Path $root 'tools\engine-ask.ps1'

if (-not (Test-Path $engineGiven)) { throw "no engine at $engineGiven" }
if (-not (Test-Path (Join-Path $runtime 'bin\64bit\obs.dll'))) { throw "no test runtime at $runtime" }

$dataRoot = Join-Path $env:TEMP "buffrx-dropped-head-$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
$pipe = "buffrx-dropped-head-$PID"
$clips = Join-Path $dataRoot 'clips'
$sessions = Join-Path $clips 'sessions'
$chunks = Join-Path $dataRoot 'chunks'
$privateExe = Join-Path $runtime "bin\64bit\buffrx-engine-dh-$PID.exe"

function Fail([string]$why) {
  Write-Host "audio dropped head e2e: FAIL $why"
  exit 1
}
function Pass([string]$what) { Write-Host "  ok   $what" }

function Ask([string]$cmd, [string]$json = '{}', [int]$timeout = 30000) {
  $out = & powershell -NoProfile -File $askScript -Command $cmd -Arguments $json -TimeoutMs $timeout 2>&1
  return ($out | Out-String).Trim()
}
function AskFile([string]$cmd, [string]$json, [int]$timeout = 30000) {
  $file = Join-Path $env:TEMP ("buffrx-dh-ask-" + [Guid]::NewGuid().ToString('N').Substring(0, 8) + '.json')
  [IO.File]::WriteAllText($file, $json, (New-Object System.Text.UTF8Encoding($false)))
  try {
    $out = & powershell -NoProfile -File $askScript -Command $cmd -ArgumentsFile $file -TimeoutMs $timeout 2>&1
    return ($out | Out-String).Trim()
  } finally { Remove-Item -Force $file -ErrorAction SilentlyContinue }
}
function ParseReply([string]$text) {
  $line = @($text -split "`r?`n" | Where-Object { $_.TrimStart().StartsWith('{') }) | Select-Object -Last 1
  if (-not $line) { return $null }
  try { return ($line | ConvertFrom-Json) } catch { return $null }
}
function RunExe([string[]]$arguments) {
  $tag = [Guid]::NewGuid().ToString('N').Substring(0, 8)
  $outFile = Join-Path $env:TEMP "buffrx-dh-$tag.out.txt"
  $errFile = Join-Path $env:TEMP "buffrx-dh-$tag.err.txt"
  $quoted = @($arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } })
  $process = Start-Process -FilePath $privateExe -ArgumentList $quoted -PassThru -Wait -WindowStyle Hidden `
    -RedirectStandardOutput $outFile -RedirectStandardError $errFile
  $text = ((Get-Content -Raw $outFile -ErrorAction SilentlyContinue) + "`n" +
           (Get-Content -Raw $errFile -ErrorAction SilentlyContinue))
  Remove-Item -Force $outFile, $errFile -ErrorAction SilentlyContinue
  return $text
}
function MediaInfo([string]$file) {
  $line = @((RunExe @('--media-info', $file)) -split "`r?`n" | Where-Object { $_.TrimStart().StartsWith('{') }) |
    Select-Object -Last 1
  if (-not $line) { Fail "no --media-info answer for $file" }
  return ($line | ConvertFrom-Json)
}
function Export([string]$sourceFile, [string]$output, [int]$width = 0, [int]$height = 0) {
  $job = @{ input = $sourceFile; output = $output; quality = 0; fail_if_exists = $true }
  if ($width -gt 0 -and $height -gt 0) { $job.out_width = $width; $job.out_height = $height }
  $spec = Join-Path $dataRoot ("job-" + [Guid]::NewGuid().ToString('N').Substring(0, 8) + '.json')
  [IO.File]::WriteAllText($spec, ($job | ConvertTo-Json -Depth 4), (New-Object System.Text.UTF8Encoding($false)))
  $log = RunExe @('--media-export', $spec)
  Remove-Item -Force $spec -ErrorAction SilentlyContinue
  if ($log -notmatch 'EXPORT_OK') { Fail "the fixture export to $output failed: $log" }
}
function Sidecar([string]$file) { return "$file.buffrx-audio.json" }
function Sources([object]$track) { return @($track.sources) }
function HasSource([object]$info, [string]$name) {
  foreach ($track in $info.audio) { if (@($track.sources) -contains $name) { return $true } }
  return $false
}
function NewSession([string]$id, [string[]]$files) {
  $dir = Join-Path $sessions $id
  New-Item -ItemType Directory -Force -Path $dir | Out-Null
  $startedAt = [int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
  $start = 1000
  $list = New-Object System.Collections.ArrayList
  foreach ($file in $files) {
    $end = $start + 6000
    [void]$list.Add([ordered]@{
        file = $file; bytes = (Get-Item $file).Length; start_ms = $start; end_ms = $end
        wall_start_ms = ($startedAt * 1000) + ($start - 1000); wall_end_ms = ($startedAt * 1000) + ($end - 1000)
        epoch = 1; published = $true
      })
    $start = $end
  }
  $index = [ordered]@{
    id = $id; kind = 'timeline'; app_id = ''; profile = ''; started_ms = 1000; started_at = $startedAt
    ended_ms = $start; closed = $true; chunks = @($list); markers = @()
  }
  [IO.File]::WriteAllText((Join-Path $dir 'index.json'), ($index | ConvertTo-Json -Depth 6),
    (New-Object System.Text.UTF8Encoding($false)))
}
function Save([string]$id) {
  $reply = ParseReply (AskFile 'timeline.save' (@{ session = $id; start_ms = 0; end_ms = 12000 } | ConvertTo-Json -Compress))
  if (-not $reply) { Fail "timeline.save of $id answered nothing" }
  return $reply
}
function ExportRange([string]$id) {
  $reply = ParseReply (AskFile 'clips.export_range' (@{ id = $id; start_ms = 0; end_ms = 12000 } | ConvertTo-Json -Compress))
  if (-not $reply) { Fail "clips.export_range of $id answered nothing" }
  return $reply
}
function ClipCount { return @(Get-ChildItem -LiteralPath $clips -File -Filter *.mp4 -ErrorAction SilentlyContinue).Count }
function StagedSidecars {
  return @(Get-ChildItem -LiteralPath $clips -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '\.part\..*\.buffrx-audio\.json$' })
}
function AssertLabels([string]$file, [string]$what) {
  if (-not (Test-Path $file)) { Fail "$what : the clip is missing ($file)" }
  $side = Sidecar $file
  if (-not (Test-Path $side)) { Fail "$what : the clip has no audio layout beside it" }
  $labels = Get-Content -Raw $side | ConvertFrom-Json
  if ($labels.generator -ne 'BuffrX' -or $labels.version -ne 2) { Fail "$what : the sidecar is not a BuffrX layout" }
  if (-not $labels.media -or -not $labels.media.size) { Fail "$what : the sidecar is not bound to the clip's bytes" }
  $info = MediaInfo $file
  if (-not $info.ok -or -not $info.audio_buffrx -or -not $info.audio_aggregate) {
    Fail "$what : the published clip does not read back as a BuffrX aggregate: $($info | ConvertTo-Json -Compress)"
  }
  if ($info.audio.Count -ne 2) { Fail "$what : the clip has $($info.audio.Count) audio tracks, not 2" }
  if ($info.audio[0].role -ne 'mix' -or $info.audio[1].role -ne 'desktop') {
    Fail "$what : the labels are not mix/desktop: $($info | ConvertTo-Json -Compress)"
  }
  if ($info.audio_complete -ne $true) { Fail "$what : a complete desktop mix reads back incomplete" }
  if (HasSource $info 'microphone') { Fail "$what : the dropped head's microphone label reached the clip" }
  if (HasSource $info 'join:unlisted') { Fail "$what : the dropped head made the kept mix look unlisted" }
  foreach ($track in $info.audio) {
    foreach ($source in @($track.sources)) {
      if ($source -like 'join:track:*') { Fail "$what : the kept tail got a positional lane it did not need ($source)" }
    }
  }
  return $info
}

New-Item -ItemType Directory -Force -Path $clips, $sessions, $chunks | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $clips 'Screenshots') | Out-Null
$config = @{
  clips_dir = $clips
  screenshots_dir = (Join-Path $clips 'Screenshots')
  auto_game_capture = $false
  start_buffering = $false
  notifications = $false
  mic_enabled = $false
  desktop_enabled = $false
  audio_tracks = 2
  separate_tracks = $true
  container = 'mp4'
  timeline_minutes = 0
}
[IO.File]::WriteAllText((Join-Path $dataRoot 'config.json'), ($config | ConvertTo-Json -Depth 4),
  (New-Object System.Text.UTF8Encoding($false)))

Copy-Item -LiteralPath $engineGiven -Destination $privateExe -Force
$proc = $null
try {
  # Fixtures: the same real recordings the audio probes use.
  $tail = Join-Path $chunks 'tail.mp4'
  Copy-Item -LiteralPath (Join-Path $sourceDir 'Desktop Record.mp4') -Destination $tail
  Copy-Item -LiteralPath (Join-Path $sourceDir 'Desktop Record.mp4.buffrx-audio.json') -Destination (Sidecar $tail)
  $headStreams = Join-Path $chunks 'head-six.mp4'
  Copy-Item -LiteralPath (Join-Path $sourceDir 'Six Tracks.mp4') -Destination $headStreams
  $headImport = Join-Path $chunks 'head-import.mp4'
  # The tail's own bytes without a sidecar: an import with the identical stream layout, so nothing is
  # dropped and only the first stream's meaning (a mix versus a plain track) can refuse the join.
  Copy-Item -LiteralPath (Join-Path $sourceDir 'Desktop Record.mp4') -Destination $headImport
  # A 1280x720 copy of the incomplete mix: two tracks, a lane that names the microphone, and a different
  # video signature from the 2560x1440 tail, so the join drops it.
  $headLanes = Join-Path $chunks 'head-lanes.mp4'
  Export (Join-Path $sourceDir 'Incomplete Mix.mp4') $headLanes 1280 720
  # Two independent exports of the same recording: identical stream signatures, no drop.
  $tailA = Join-Path $chunks 'tail-a.mp4'
  $tailB = Join-Path $chunks 'tail-b.mp4'
  Export (Join-Path $sourceDir 'Desktop Record.mp4') $tailA 1920 1080
  Export (Join-Path $sourceDir 'Desktop Record.mp4') $tailB 1920 1080

  NewSession 'BuffrX DH Streams' @($headStreams, $tail)
  NewSession 'BuffrX DH Lanes' @($headLanes, $tail)
  NewSession 'BuffrX DH NoDrop' @($tailA, $tailB)
  NewSession 'BuffrX DH Refuse' @($headImport, $tail)

  $env:BUFFRX_DATA_ROOT = $dataRoot
  $env:BUFFRX_PIPE = $pipe
  $logOut = Join-Path $dataRoot 'out.txt'
  $logErr = Join-Path $dataRoot 'err.txt'
  $proc = Start-Process -FilePath $privateExe -PassThru -WindowStyle Hidden -WorkingDirectory (Split-Path $privateExe) `
    -RedirectStandardOutput $logOut -RedirectStandardError $logErr
  $hello = $null
  for ($i = 0; $i -lt 60 -and -not $hello; $i++) { Start-Sleep -Milliseconds 250; $hello = Ask 'hello' '{}' 3000 }
  if (-not ($hello -match '"protocol"')) { Fail 'the engine did not answer on its pipe' }

  # A. A dropped head with another stream count: its six unlabelled tracks are not read as two.
  $saved = Save 'BuffrX DH Streams'
  if (-not $saved.ok) { Fail "the save across a stream-count change was refused: $($saved.error)" }
  if ($saved.data.chunks -ne 1) { Fail "the save reports $($saved.data.chunks) chunks, not the one it copied" }
  if ($saved.data.cut_head_ms -ne 6000) { Fail "the save does not say what it left out ($($saved.data.cut_head_ms) ms)" }
  AssertLabels $saved.data.file 'a dropped head with another stream count' | Out-Null
  Pass 'a dropped six-stream head leaves the tail complete, bound and mix/desktop'

  # B. A dropped head with the same stream count but another lane: its microphone label does not follow.
  $savedLanes = Save 'BuffrX DH Lanes'
  if (-not $savedLanes.ok) { Fail "the save across a lane change was refused: $($savedLanes.error)" }
  if ($savedLanes.data.chunks -ne 1) { Fail "the lane change saved $($savedLanes.data.chunks) chunks, not 1" }
  AssertLabels $savedLanes.data.file 'a dropped head with another lane' | Out-Null
  Pass 'a dropped same-width head cannot mark the kept mix incomplete or name its microphone'

  # C. Nothing dropped: both chunks still join and keep their own layout.
  $savedBoth = Save 'BuffrX DH NoDrop'
  if (-not $savedBoth.ok) { Fail "the unchanged save was refused: $($savedBoth.error)" }
  if ($savedBoth.data.chunks -ne 2) { Fail "the unchanged save reports $($savedBoth.data.chunks) chunks, not 2" }
  AssertLabels $savedBoth.data.file 'two chunks with one layout' | Out-Null
  Pass 'two chunks that share one layout still join with it unchanged'

  # D. A mix joined with an import cannot be described: refused before a clip is published.
  $before = ClipCount
  $refused = Save 'BuffrX DH Refuse'
  if ($refused.ok) { Fail 'a mix joined with an import was published instead of refused' }
  if ($refused.error -notmatch 'hold their sound differently') {
    Fail "the refusal does not say what is incompatible: $($refused.error)"
  }
  if ((ClipCount) -ne $before) { Fail 'a refused join left a clip in the library' }
  Pass 'a mix joined with an import is refused before anything is published'

  # E. The native range export: its labels describe the chunks it copied, and no staging sidecar is left.
  $exported = ExportRange 'BuffrX DH Lanes'
  if (-not $exported.ok) { Fail "the range export was refused: $($exported.error)" }
  if ($exported.data.chunks -ne 2) { Fail "the range export reports $($exported.data.chunks) chunks, not 2" }
  if (-not (Test-Path $exported.data.file)) { Fail 'the range export published no file' }
  $rangeInfo = MediaInfo $exported.data.file
  if (-not $rangeInfo.audio_buffrx -or $rangeInfo.audio.Count -ne 2) {
    Fail "the range export's labels do not describe a two-track file: $($rangeInfo | ConvertTo-Json -Compress)"
  }
  $staged = @(StagedSidecars)
  if ($staged.Count -ne 0) { Fail "a staging sidecar was left behind: $($staged[0].FullName)" }
  Pass 'a range export labels the chunks it copied and cleans its staging sidecar'

  Write-Host 'audio dropped head e2e: all checks passed'
} finally {
  if ($proc -and -not $proc.HasExited) {
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    try { Wait-Process -Id $proc.Id -Timeout 5 -ErrorAction SilentlyContinue } catch { }
  }
  Remove-Item Env:\BUFFRX_DATA_ROOT -ErrorAction SilentlyContinue
  Remove-Item Env:\BUFFRX_PIPE -ErrorAction SilentlyContinue
  for ($i = 0; $i -lt 10 -and (Test-Path -LiteralPath $privateExe); $i++) {
    Remove-Item -Force -LiteralPath $privateExe -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $privateExe) { Start-Sleep -Milliseconds 200 }
  }
  if (-not $KeepData) { Start-Sleep -Milliseconds 300; Remove-Item -Recurse -Force $dataRoot -ErrorAction SilentlyContinue }
}
