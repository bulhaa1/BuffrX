# What this checks: a BuffrX recording keeps its own audio layout through the editor and the export.
#
# The tracks are recorded for real (a program plays a tone while a two-track recording runs), then:
#   1. the sidecar beside the clip names the mix and the desktop, and --media-info reads it from the file;
#   2. muting the desktop rebuilds the ordinary mix, so a player that reads only track 1 hears nothing;
#   3. an untouched export keeps the recorded mix;
#   4. the same file without its sidecar is an import: no track is assumed to be a mix of the others;
#   5. a sidecar that claims a mix with no track for one of its sources refuses a level change;
#   6. a sequence join carries the layout, and the clip's labels follow a rename.
#
# Silence is measured with tools/mp4-samples.js: an AAC frame of silence is a few bytes, real sound is
# hundreds, so the two are told apart without decoding anything.
param([switch]$KeepData, [string]$Engine = 'buffrx-engine.exe')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$engineExe = if ([IO.Path]::IsPathRooted($Engine)) { $Engine } else { Join-Path $root ('runtime\bin\64bit\' + $Engine) }
$askScript = Join-Path $root 'tools\engine-ask.ps1'
$samplesScript = Join-Path $root 'tools\mp4-samples.js'
$dataRoot = Join-Path $env:TEMP "buffrx-audio-tracks-$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
$pipe = "buffrx-audio-tracks-$PID"

function Ask([string]$cmd, [string]$json = '{}', [int]$timeout = 15000) {
  $out = & powershell -NoProfile -File $askScript -Command $cmd -Arguments $json -TimeoutMs $timeout 2>&1
  return ($out | Out-String).Trim()
}

# A request whose arguments hold a path: the Windows command line cannot carry JSON quotes, so it goes
# through a file (engine-ask.ps1's own reason for -ArgumentsFile).
function AskFile([string]$cmd, [string]$json, [int]$timeout = 15000) {
  $file = Join-Path $env:TEMP ("buffrx-ask-" + [Guid]::NewGuid().ToString('N').Substring(0, 8) + '.json')
  [IO.File]::WriteAllText($file, $json, (New-Object System.Text.UTF8Encoding($false)))
  try {
    $out = & powershell -NoProfile -File $askScript -Command $cmd -ArgumentsFile $file -TimeoutMs $timeout 2>&1
    return ($out | Out-String).Trim()
  } finally {
    Remove-Item -Force $file -ErrorAction SilentlyContinue
  }
}

function Fail([string]$why) {
  Write-Host "audio tracks e2e: FAIL $why"
  exit 1
}

function Pass([string]$what) { Write-Host "  ok   $what" }

# The per-track audio lines of mp4-samples.js, in the file's own track order.
function TrackStats([string]$file) {
  $report = & node $samplesScript $file | Out-String
  $lines = @($report -split "`r?`n" | Where-Object { $_ -match '^\s+soun ' })
  return @($lines | ForEach-Object {
      $m = [regex]::Match($_, 'samples=(\d+) bytes=(\d+) avg=([\d.]+)')
      if (-not $m.Success) { Fail "could not read the audio tracks of $file" }
      [pscustomobject]@{ samples = [int]$m.Groups[1].Value; avg = [double]$m.Groups[3].Value }
    })
}

function Sidecar([string]$file) { return "$file.buffrx-audio.json" }

# A native run with both streams captured to files: the engine logs to stderr, and a redirected native
# command's stderr reaches PowerShell as an error record that would stop this script.
function RunEngine([string[]]$arguments) {
  $tag = [Guid]::NewGuid().ToString('N').Substring(0, 8)
  $outFile = Join-Path $env:TEMP "buffrx-run-$tag.out.txt"
  $errFile = Join-Path $env:TEMP "buffrx-run-$tag.err.txt"
  # Start-Process joins the list with spaces and quotes nothing: a "Display Capture" folder would end
  # the path at the space.
  $quoted = @($arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } })
  $process = Start-Process -FilePath $engineExe -ArgumentList $quoted -PassThru -Wait -NoNewWindow `
    -RedirectStandardOutput $outFile -RedirectStandardError $errFile
  $text = ((Get-Content -Raw $outFile -ErrorAction SilentlyContinue) + "`n" +
           (Get-Content -Raw $errFile -ErrorAction SilentlyContinue))
  Remove-Item -Force $outFile, $errFile -ErrorAction SilentlyContinue
  return $text
}

function Export([string]$specFile, [string]$output) {
  if (Test-Path $output) { Remove-Item -Force $output }
  return (RunEngine @('--media-export', $specFile))
}

function WriteJob([string]$file, [hashtable]$job) {
  [IO.File]::WriteAllText($file, ($job | ConvertTo-Json -Depth 6), (New-Object System.Text.UTF8Encoding($false)))
}

New-Item -ItemType Directory -Force -Path (Join-Path $dataRoot 'clips\Screenshots') | Out-Null
# Two mixer tracks: the mix, and the desktop on its own. No microphone, so the plan is exactly the pair
# BuffrX records (audio_routing.h).
$config = @{
  clips_dir = (Join-Path $dataRoot 'clips')
  screenshots_dir = (Join-Path $dataRoot 'clips\Screenshots')
  auto_game_capture = $false
  start_buffering = $false
  notifications = $false
  audio_tracks = 2
  separate_tracks = $true
  mic_enabled = $false
  desktop_enabled = $true
}
[IO.File]::WriteAllText((Join-Path $dataRoot 'config.json'), ($config | ConvertTo-Json -Depth 4), (New-Object System.Text.UTF8Encoding($false)))

# A 4 s 440 Hz stereo tone at 48 kHz, played on loop while the clip records.
$wav = Join-Path $env:TEMP 'buffrx-audio-tracks-tone.wav'
if (-not (Test-Path $wav)) {
  $rate = 48000; $n = $rate * 4
  $ms = New-Object System.IO.MemoryStream
  $bw = New-Object System.IO.BinaryWriter($ms)
  $bw.Write([Text.Encoding]::ASCII.GetBytes('RIFF')); $bw.Write([int](36 + $n * 4))
  $bw.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt ')); $bw.Write([int]16); $bw.Write([int16]1)
  $bw.Write([int16]2); $bw.Write([int]$rate); $bw.Write([int]($rate * 4)); $bw.Write([int16]4); $bw.Write([int16]16)
  $bw.Write([Text.Encoding]::ASCII.GetBytes('data')); $bw.Write([int]($n * 4))
  for ($i = 0; $i -lt $n; $i++) {
    $v = [int16](16000 * [Math]::Sin(2 * [Math]::PI * 440 * $i / $rate))
    $bw.Write($v); $bw.Write($v)
  }
  $bw.Flush(); [IO.File]::WriteAllBytes($wav, $ms.ToArray()); $bw.Close()
}

$env:BUFFRX_DATA_ROOT = $dataRoot
$env:BUFFRX_PIPE = $pipe
$player = Start-Process powershell -ArgumentList @('-NoProfile', '-Command',
  "Add-Type -AssemblyName System.Windows.Forms; `$p = New-Object System.Media.SoundPlayer '$wav'; while (`$true) { `$p.PlaySync() }") `
  -WindowStyle Hidden -PassThru
$proc = $null
try {
  $proc = Start-Process -FilePath $engineExe -PassThru -WindowStyle Hidden -WorkingDirectory (Split-Path $engineExe) `
    -RedirectStandardOutput (Join-Path $dataRoot 'out.txt') -RedirectStandardError (Join-Path $dataRoot 'err.txt')
  $hello = $null
  for ($i = 0; $i -lt 40 -and -not $hello; $i++) { Start-Sleep -Milliseconds 250; $hello = Ask 'hello' '{}' 3000 }
  if (-not ($hello -match '"protocol"')) { Fail 'the engine did not answer on its pipe' }

  # Legacy files now migrate to Auto. This fixture specifically exercises the two-track Desktop mode.
  $modeReply = AskFile 'config.set' '{"patch":{"audio_capture_mode":"desktop"}}' | ConvertFrom-Json
  if (-not $modeReply.ok -or $modeReply.data.audio_capture_mode -ne 'desktop') { Fail 'could not select Desktop audio for the fixture' }

  # The tone must already be playing: a quiet room is legitimately silence.
  Start-Sleep -Seconds 4
  Ask 'recording.start' '{}' | Out-Null
  Start-Sleep -Seconds 9
  Ask 'recording.stop' '{}' | Out-Null
  Start-Sleep -Seconds 3

  $clip = Get-ChildItem -Path (Join-Path $dataRoot 'clips') -Recurse -Filter *.mp4 | Sort-Object LastWriteTime | Select-Object -Last 1
  if (-not $clip) { Fail 'the recording wrote no clip' }
  $clipPath = $clip.FullName

  # 1. The recorder's own layout, and the editor's read of it from the file.
  if (-not (Test-Path (Sidecar $clipPath))) { Fail 'the recording wrote no audio layout beside the clip' }
  $labels = Get-Content -Raw (Sidecar $clipPath) | ConvertFrom-Json
  if ($labels.generator -ne 'BuffrX' -or $labels.version -ne 2) { Fail 'the sidecar is not a BuffrX layout' }
  if (-not $labels.media -or $labels.media.size -le 0 -or -not $labels.media.head -or -not $labels.media.tail) {
    Fail "the sidecar is not bound to the clip's bytes: $($labels | ConvertTo-Json -Compress)"
  }
  if ($labels.tracks.Count -ne 2 -or $labels.tracks[0].role -ne 'mix' -or $labels.tracks[1].role -ne 'desktop') {
    Fail "the sidecar names the wrong tracks: $($labels | ConvertTo-Json -Compress)"
  }
  Pass 'the recording left a mix/desktop layout beside the clip'

  $info = RunEngine @('--media-info', $clipPath)
  if ($info -notmatch '"audio_buffrx":true' -or $info -notmatch '"audio_aggregate":true' -or $info -notmatch '"audio_complete":true') {
    Fail "--media-info did not report the recording's own aggregate layout: $info"
  }
  if ($info -notmatch '"role":"desktop"') { Fail "--media-info did not report the desktop track's role: $info" }
  Pass '--media-info reads the layout from the file, not from the settings'

  $recorded = TrackStats $clipPath
  if ($recorded.Count -ne 2) { Fail "the recording has $($recorded.Count) audio tracks, not 2" }
  if ($recorded[0].avg -lt 100 -or $recorded[1].avg -lt 100) { Fail 'the recording is silent on a track the tone should reach' }
  Pass 'the recording carries the tone on both its mix and its desktop track'

  # 2. Muting the desktop has to reach the ordinary mix too.
  $out = Join-Path $dataRoot 'clips\Edits'
  New-Item -ItemType Directory -Force -Path $out | Out-Null
  $spec = Join-Path $dataRoot 'job-mute.json'
  WriteJob $spec @{ input = $clipPath; output = (Join-Path $out 'muted.mp4'); volumes = @(1, 0); quality = 0; fail_if_exists = $true }
  $log = Export $spec (Join-Path $out 'muted.mp4')
  if ($log -notmatch 'EXPORT_OK') { Fail "the muted export failed: $log" }
  $muted = TrackStats (Join-Path $out 'muted.mp4')
  if ($muted.Count -ne 2) { Fail "the muted export wrote $($muted.Count) audio tracks, not 2" }
  if ($muted[0].avg -ge 100) { Fail "the exported mix still holds the muted desktop (avg $($muted[0].avg))" }
  if ($muted[1].avg -ge 100) { Fail "the muted desktop track is not silent (avg $($muted[1].avg))" }
  if (-not (Test-Path (Sidecar (Join-Path $out 'muted.mp4')))) { Fail 'the export wrote no layout beside its own file' }
  Pass 'a muted desktop is silent in the rebuilt mix and on its own track'

  # Resampling must describe decoded samples to abuffer, then convert at the filter's output.
  # Check both the recorded mix and a rebuilt mix, including the unchanged-level export plan.
  foreach ($levels in @(@(1, 1), @(1, 0.5))) {
    $resampled = Join-Path $out ('resampled-' + $levels[1] + '.mp4')
    $resampleSpec = Join-Path $dataRoot 'job-resample.json'
    WriteJob $resampleSpec @{ input = $clipPath; output = $resampled; volumes = $levels; audio_rate = 44100; end = 0.4 }
    $log = Export $resampleSpec $resampled
    if ($log -notmatch 'EXPORT_OK') { Fail "the resampled export failed: $log" }
    $text = RunEngine @('--media-info', $resampled)
    $line = $text -split "`r?`n" | Where-Object { $_.TrimStart().StartsWith('{') } | Select-Object -Last 1
    $details = $line | ConvertFrom-Json
    if ($details.audio.Count -ne 2 -or @($details.audio | Where-Object { $_.sample_rate -ne 44100 }).Count -ne 0) {
      Fail 'resampling did not keep both tracks at 44100 Hz'
    }
  }
  Pass 'resampling preserves the recorded and rebuilt mix at the requested sample rate'

  # The waveform lanes the editor draws come from --media-peaks: one lane per audio stream, in stream
  # order, so lane 1 is track 1. A muted export has two flat lanes, not one missing lane.
  function Peaks([string]$file) {
    $text = RunEngine @('--media-peaks', $file, '120')
    $line = ($text -split "`r?`n" | Where-Object { $_.TrimStart().StartsWith('{') } | Select-Object -Last 1)
    if (-not $line) { Fail "no waveform for $file" }
    return ($line | ConvertFrom-Json)
  }
  $lanes = Peaks (Join-Path $out 'muted.mp4')
  if ($lanes.tracks.Count -ne 2) { Fail "the muted export drew $($lanes.tracks.Count) waveform lanes, not 2" }
  $mean = ($lanes.tracks[0] | Measure-Object -Average).Average
  if ($mean -gt 0.01) { Fail "the muted mix's waveform is not flat (mean $mean)" }
  $keptLanes = Peaks $clipPath
  if ($keptLanes.tracks.Count -ne 2) { Fail "the recorded clip drew $($keptLanes.tracks.Count) waveform lanes, not 2" }
  $keptMean = ($keptLanes.tracks[0] | Measure-Object -Average).Average
  if ($keptMean -lt 0.05) { Fail "an audible mix's waveform is flat (mean $keptMean)" }
  Pass 'waveform lanes match the audio ordinals and follow the levels'

  # The editor asks for un-normalised lanes (`raw`) so lanes drawn together keep their relative levels,
  # and for a per-lane decoded map so an unreadable lane is not drawn as silence. The ordinary answer
  # still normalises each lane to its own loudest point, which older callers rely on.
  function Peaks([string]$file, [string[]]$extra = @()) {
    $text = RunEngine (@('--media-peaks', $file, '120') + $extra)
    $line = ($text -split "`r?`n" | Where-Object { $_.TrimStart().StartsWith('{') } | Select-Object -Last 1)
    if (-not $line) { Fail "no waveform for $file" }
    return ($line | ConvertFrom-Json)
  }
  $raw = Peaks $clipPath @('raw')
  if ($raw.raw -ne $true -or $keptLanes.raw -ne $false) { Fail '--media-peaks did not say which scale it used' }
  if ($raw.tracks.Count -ne 2 -or $raw.decoded.Count -ne 2) { Fail 'the raw answer did not keep one lane and one decoded flag a track' }
  if (-not ($raw.decoded[0] -and $raw.decoded[1])) { Fail "a lane that decoded was reported unreadable: $($raw.decoded -join ',')" }
  foreach ($lane in $raw.tracks) {
    $top = ($lane | Measure-Object -Maximum).Maximum
    if ($top -gt 1.0) { Fail "a raw peak passed full scale ($top)" }
  }
  foreach ($lane in $keptLanes.tracks) {
    $mean = ($lane | Measure-Object -Average).Average
    if ($mean -le 0.01) { continue }  # a silent lane stays at zero in both scales
    $top = ($lane | Measure-Object -Maximum).Maximum
    if ($top -lt 0.9) { Fail "the ordinary answer stopped normalising a lane to its own maximum (top $top)" }
  }
  Pass 'raw lanes stay on one scale, the ordinary answer still normalises, and each lane says whether it decoded'

  # 3. An untouched level change keeps the recorded mix.
  $spec = Join-Path $dataRoot 'job-keep.json'
  WriteJob $spec @{ input = $clipPath; output = (Join-Path $out 'kept.mp4'); volumes = @(1, 1); quality = 0; fail_if_exists = $true }
  $log = Export $spec (Join-Path $out 'kept.mp4')
  if ($log -notmatch 'EXPORT_OK') { Fail "the untouched export failed: $log" }
  $kept = TrackStats (Join-Path $out 'kept.mp4')
  if ($kept.Count -ne 2 -or $kept[0].avg -lt 100 -or $kept[1].avg -lt 100) { Fail 'an untouched export lost a track or its sound' }
  Pass 'an untouched aggregate keeps both its mix and its track'

  # 4. The same bytes without a sidecar are an import: track 1 keeps its own sound, and muting track 2
  #    silences only track 2. Nothing is inferred from the number of tracks.
  $bare = Join-Path $out 'imported.mp4'
  Copy-Item $clipPath $bare
  $spec = Join-Path $dataRoot 'job-import.json'
  WriteJob $spec @{ input = $bare; output = (Join-Path $out 'imported-export.mp4'); volumes = @(1, 0); quality = 0; fail_if_exists = $true }
  $log = Export $spec (Join-Path $out 'imported-export.mp4')
  if ($log -notmatch 'EXPORT_OK') { Fail "the import export failed: $log" }
  $imported = TrackStats (Join-Path $out 'imported-export.mp4')
  if ($imported.Count -ne 2) { Fail "the import export wrote $($imported.Count) audio tracks, not 2" }
  if ($imported[0].avg -lt 100) { Fail 'an imported first track was treated as an aggregate and rebuilt' }
  if ($imported[1].avg -ge 100) { Fail 'the imported second track was not silenced by its level' }
  if (Test-Path (Sidecar (Join-Path $out 'imported-export.mp4'))) { Fail 'an import was given a BuffrX layout it never had' }
  Pass 'a file without a sidecar is levelled as it is, with nothing inferred'

  # 5. A mix that names a source no track holds cannot be rebuilt: a level change is refused, and no
  #    output is left behind. The bound sidecar is edited in place (its media identity still names the
  #    clip's own bytes) and put back afterwards.
  $originalSidecar = Get-Content -Raw (Sidecar $clipPath)
  try {
    $bad = $originalSidecar | ConvertFrom-Json
    $bad.tracks[0].sources = @('desktop', 'microphone')
    $bad.tracks[1].sources = @('desktop')
    [IO.File]::WriteAllText((Sidecar $clipPath), ($bad | ConvertTo-Json -Depth 6),
      (New-Object System.Text.UTF8Encoding($false)))
    $spec = Join-Path $dataRoot 'job-partial.json'
    WriteJob $spec @{ input = $clipPath; output = (Join-Path $out 'partial-export.mp4'); volumes = @(1, 0.5); quality = 0; fail_if_exists = $true }
    $log = Export $spec (Join-Path $out 'partial-export.mp4')
    if ($log -notmatch 'EXPORT_FAIL') { Fail "a mix that cannot be rebuilt was exported anyway: $log" }
    if ($log -notmatch 'no track of its own') { Fail "the refusal did not say why: $log" }
    if (Test-Path (Join-Path $out 'partial-export.mp4')) { Fail 'a refused export left a file behind' }
  } finally {
    [IO.File]::WriteAllText((Sidecar $clipPath), $originalSidecar,
      (New-Object System.Text.UTF8Encoding($false)))
  }
  Pass 'an incomplete mix refuses a level change with a reason and no file'

  # 5b. A different file with the same track count does not inherit the labels: one byte is changed in a
  #     copy, and the old layout (bound to the original bytes) is refused.
  $sameBytes = Join-Path $out 'same-bytes.mp4'
  Copy-Item $clipPath $sameBytes
  Copy-Item (Sidecar $clipPath) (Sidecar $sameBytes)
  $bytes = [IO.File]::ReadAllBytes($sameBytes)
  $at = [int]($bytes.Length / 2)
  $bytes[$at] = $bytes[$at] -bxor 0x01
  [IO.File]::WriteAllBytes($sameBytes, $bytes)
  $copyInfo = RunEngine @('--media-info', $sameBytes)
  if ($copyInfo -match '"audio_buffrx":true') {
    Fail "labels described a different file with the same track count: $copyInfo"
  }
  Pass 'a sidecar does not describe a different file that happens to share its track count'

  # 6. A join carries the layout, and a rename carries it too.
  $joined = Join-Path $out 'joined.mp4'
  $spec = Join-Path $dataRoot 'job-join.json'
  WriteJob $spec @{
    output = $joined; fit_pad = $true; quality = 0; fail_if_exists = $true
    inputs = @(
      @{ input = $clipPath; segments = @(@(0, 4)); volumes = @(1, 1) },
      @{ input = $clipPath; segments = @(@(0, 4)); volumes = @(1, 1) }
    )
  }
  $log = Export $spec $joined
  if ($log -notmatch 'EXPORT_OK') { Fail "the join failed: $log" }
  $joinedStats = TrackStats $joined
  if ($joinedStats.Count -ne 2) { Fail "the join wrote $($joinedStats.Count) audio tracks, not 2" }
  if (-not (Test-Path (Sidecar $joined))) { Fail 'the join wrote no layout beside its file' }
  $joinInfo = RunEngine @('--media-info', $joined)
  if ($joinInfo -notmatch '"audio_aggregate":true') { Fail "the joined file is not an aggregate mix: $joinInfo" }
  Pass 'a sequence join keeps both tracks and their layout'

  $renamed = AskFile 'clips.rename' (@{ file = $joined; name = 'Joined Renamed' } | ConvertTo-Json -Compress)
  $newPath = ($renamed | ConvertFrom-Json).data.file
  if (-not $newPath) { Fail "the rename was refused: $renamed" }
  if (-not (Test-Path (Sidecar $newPath))) { Fail 'the layout did not follow the rename' }
  if (Test-Path (Sidecar $joined)) { Fail 'the old layout was left behind after a rename' }
  Pass 'the layout follows a rename'

  Write-Host 'audio tracks e2e: all checks passed'
}
finally {
  if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
  Stop-Process -Id $player.Id -Force -ErrorAction SilentlyContinue
  Remove-Item Env:\BUFFRX_DATA_ROOT -ErrorAction SilentlyContinue
  Remove-Item Env:\BUFFRX_PIPE -ErrorAction SilentlyContinue
  if (-not $KeepData) { Start-Sleep -Milliseconds 500; Remove-Item -Recurse -Force $dataRoot -ErrorAction SilentlyContinue }
}
