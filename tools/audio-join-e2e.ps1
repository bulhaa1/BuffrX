# What this checks: what a joined clip's sound really is, and that an editor cannot read it as an
# import (the mix and its stems played as independent streams, so every sound is heard twice).
#
#   powershell -File tools/audio-join-e2e.ps1 -Engine buffrx-engine.exe            # the fixed build
#   powershell -File tools/audio-join-e2e.ps1 -Engine buffrx-audio-review.exe -Expect bug   # the old one
#
# Real MP4s from work/frozen-audio-data/clips/Recordings are copied into a disposable data root, joined
# with the product's own export path, and read back with --media-info and --media-peaks raw:
#
#   1. same lanes, different cosmetic titles: the desktop lane keeps its identity and the output is a
#      bound, complete aggregate. The old engine dropped the labels and the file read as an import.
#   2. different lanes, one input incomplete: the output stays mix-first but locked, so playback uses the
#      mix alone and a level change is refused rather than taken as independent streams.
#   3. different lane identities, both complete: each output stem gets its own logical id, the mix can
#      still be rebuilt from the stems, and a level edit lands on both lanes once.
#   4. an aggregate joined with an import: equal track counts are not enough, so the join is refused
#      before the encode, with no output and no staging file left.
#   5. imports joined with imports stay exactly as they were, with no labels at all.
#   6. a level edit made before the join survives a reopen of the joined file: the mix follows the stems,
#      and the mix is never counted beside them.
#   7. the raw join child (`--media-join <spec>`, what a Timeline save runs) applies the same rule before
#      it copies: a compatible pair carries its labels, a mix with an import is refused before the copy
#      with a failed result and no output (a file already at that name is left alone), and two imports
#      join with no labels.
#
# Nothing is written to a real library: every file lives under a fresh data root. The fixture bytes and
# their sidecars are hashed before and after, and must be untouched.
param(
  [ValidateSet('fixed', 'bug')][string]$Expect = 'fixed',
  [string]$Engine = 'buffrx-engine.exe',
  [string]$Fixtures = '',
  [switch]$KeepData
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$engineExe = if ([IO.Path]::IsPathRooted($Engine)) { $Engine } else { Join-Path $root ('runtime\bin\64bit\' + $Engine) }
if (-not (Test-Path -LiteralPath $engineExe)) { throw "no engine: $engineExe" }
if (-not $Fixtures) { $Fixtures = Join-Path $root 'work\frozen-audio-data\clips\Recordings' }
if (-not (Test-Path -LiteralPath $Fixtures)) { throw "no fixtures: $Fixtures" }

$isFixed = $Expect -eq 'fixed'
$dataRoot = Join-Path $env:TEMP ("buffrx-audio-join-" + [Guid]::NewGuid().ToString('N').Substring(0, 8))
$work = Join-Path $dataRoot 'clips\Edits'
New-Item -ItemType Directory -Force -Path $work | Out-Null
$env:BUFFRX_DATA_ROOT = $dataRoot
$env:BUFFRX_PIPE = "buffrx-audio-join-$PID"
$failures = 0

function Check([bool]$condition, [string]$what) {
  if ($condition) { Write-Host "  ok   $what" }
  else { Write-Host "  FAIL $what"; $script:failures++ }
}
function Top($lane) { ($lane | Measure-Object -Maximum).Maximum }

# A native run with both streams captured: a redirected native command's stderr reaches PowerShell as an
# error record that would stop this script.
function RunEngine([string[]]$arguments) {
  $tag = [Guid]::NewGuid().ToString('N').Substring(0, 8)
  $outFile = Join-Path $env:TEMP "buffrx-join-$tag.out.txt"
  $errFile = Join-Path $env:TEMP "buffrx-join-$tag.err.txt"
  $quoted = @($arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } })
  Start-Process -FilePath $engineExe -ArgumentList $quoted -PassThru -Wait -NoNewWindow `
    -WorkingDirectory (Split-Path $engineExe) `
    -RedirectStandardOutput $outFile -RedirectStandardError $errFile | Out-Null
  $text = ((Get-Content -Raw $outFile -ErrorAction SilentlyContinue) + "`n" +
           (Get-Content -Raw $errFile -ErrorAction SilentlyContinue))
  Remove-Item -Force $outFile, $errFile -ErrorAction SilentlyContinue
  return $text
}

function LastJson([string]$text) {
  $line = ($text -split "`r?`n" | Where-Object { $_.TrimStart().StartsWith('{') } | Select-Object -Last 1)
  if (-not $line) { return $null }
  return ($line | ConvertFrom-Json)
}

function Info([string]$file) {
  $info = LastJson (RunEngine @('--media-info', $file))
  if (-not $info) { throw "no media info for $file" }
  return $info
}
function RawPeaks([string]$file) {
  $peaks = LastJson (RunEngine @('--media-peaks', $file, '200', 'raw'))
  if (-not $peaks -or -not $peaks.ok) { throw "no waveform for $file" }
  return $peaks
}

# A fixture's bytes and its sidecar, with the sidecar's own identity still true: a copy must keep the
# source's last-write time, or the identity no longer matches and the copy is read as an import.
function CopyFixture([string]$name, [string]$as) {
  $source = Join-Path $Fixtures $name
  if (-not (Test-Path -LiteralPath $source)) { throw "no fixture: $source" }
  $target = Join-Path $work $as
  Copy-Item -LiteralPath $source -Destination $target -Force
  (Get-Item -LiteralPath $target).LastWriteTimeUtc = (Get-Item -LiteralPath $source).LastWriteTimeUtc
  $sidecar = "$source.buffrx-audio.json"
  if (Test-Path -LiteralPath $sidecar) {
    Copy-Item -LiteralPath $sidecar -Destination "$target.buffrx-audio.json" -Force
  }
  return $target
}

# A test-only relabel of an existing copy: the media bytes and their identity block are untouched, so the
# copy stays bound to its own file while it describes a different lane identity.
function RelabelLane([string]$file, [string]$role, [string[]]$sources, [string]$title) {
  $path = "$file.buffrx-audio.json"
  $json = Get-Content -Raw -LiteralPath $path | ConvertFrom-Json
  $json.tracks[0].sources = @($sources)
  $json.tracks[1].role = $role
  $json.tracks[1].sources = @($sources)
  $json.tracks[1].title = $title
  $json | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $path -Encoding utf8
}

function Join([string[]]$files, [string]$name, [double[]]$volumes = $null) {
  $output = Join-Path $work $name
  $spec = Join-Path $dataRoot ("job-$name.json")
  $inputs = @()
  foreach ($file in $files) {
    $entry = @{ input = $file }
    if ($volumes) { $entry.volumes = @($volumes) }
    $inputs += $entry
  }
  @{ output = $output; inputs = $inputs; quality = 0; fit_pad = $true; fail_if_exists = $true; video_codec = 'h264' } |
    ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $spec -Encoding utf8
  $log = RunEngine @('--media-export', $spec)
  return [pscustomobject]@{ ok = ($log -match 'EXPORT_OK'); output = $output; log = $log }
}

function ExportOne([string]$file, [string]$name, [double[]]$volumes) {
  $output = Join-Path $work $name
  $spec = Join-Path $dataRoot ("job-$name.json")
  @{ input = $file; output = $output; volumes = @($volumes); quality = 0; fail_if_exists = $true } |
    ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $spec -Encoding utf8
  $log = RunEngine @('--media-export', $spec)
  return [pscustomobject]@{ ok = ($log -match 'EXPORT_OK'); output = $output; log = $log }
}

function Sidecar([string]$file) { return "$file.buffrx-audio.json" }

# The raw join the engine runs for a Timeline save: `--media-join <spec>` copies the files as they are
# (no export job, no re-encode) and writes `<output>.join.json` beside the copy. It is given the same
# carry_audio_labels wish the save gives it, so both kinds of join are held to the same rule.
function RawJoin([string[]]$files, [string]$name) {
  $output = Join-Path $work $name
  $spec = Join-Path $dataRoot ("raw-$name.json")
  $seconds = (Info $files[-1]).duration
  @{ files = @($files); output = $output; start_in_first = 0; end_in_last = $seconds; carry_audio_labels = $true } |
    ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $spec -Encoding utf8
  $log = RunEngine @('--media-join', $spec)
  $reply = LastJson $log
  $resultFile = "$output.join.json"
  $written = if (Test-Path -LiteralPath $resultFile) { Get-Content -Raw -LiteralPath $resultFile | ConvertFrom-Json } else { $null }
  return [pscustomobject]@{ ok = [bool]($reply -and $reply.ok); reply = $reply; written = $written; output = $output; log = $log }
}

# The product's own last word on an export, for a check's message.
function Verdict([string]$log) {
  $line = $log -split "`r?`n" | Where-Object { $_ -match '^EXPORT_(OK|FAIL)' } | Select-Object -Last 1
  if ($line) { return $line.Trim() }
  return 'no verdict'
}

# The fixtures are evidence: they must be byte-for-byte what they were.
$fixtureHashes = @{}
foreach ($file in Get-ChildItem -LiteralPath $Fixtures -File -Filter '*.mp4*') {
  $fixtureHashes[$file.FullName] = (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash
}

try {
  Write-Host "audio join e2e ($Expect): $engineExe"

  # ---- 1. same lanes, different cosmetic titles ------------------------------------------------
  Write-Host 'case 1: two aggregates, same lanes, different titles'
  $a = CopyFixture 'Desktop Record.mp4' 'a.mp4'
  $b = CopyFixture 'Long Desktop Name.mp4' 'b.mp4'
  Check ((Info $a).audio_buffrx) 'the first fixture is a bound BuffrX file'
  Check ((Info $b).audio_buffrx) 'the second fixture is a bound BuffrX file'
  $join = Join @($a, $b) 'same-lanes.mp4'
  Check $join.ok "the join is made ($(Verdict $join.log))"
  $info = Info $join.output
  if ($isFixed) {
    Check (Test-Path -LiteralPath (Sidecar $join.output)) 'the join carries its track labels'
    Check ($info.audio_buffrx -and $info.audio_aggregate -and $info.audio_complete) 'the output is a bound, complete aggregate'
    Check ($info.audio.Count -eq 2 -and $info.audio[1].role -eq 'desktop' -and @($info.audio[1].sources) -contains 'desktop') 'the common desktop lane keeps its identity'
    Check (@($info.audio[0].sources) -contains 'desktop' -and -not (@($info.audio[0].sources) -contains 'join:unlisted')) 'the mix names that lane once'
  } else {
    Check (-not (Test-Path -LiteralPath (Sidecar $join.output))) 'the old engine drops the labels when the titles differ (repro)'
    Check (-not $info.audio_buffrx) 'the old joined file reads as an import (repro)'
  }
  $peaks = RawPeaks $join.output
  $mixPeak = Top $peaks.tracks[0]
  $stemPeak = Top $peaks.tracks[1]
  Check ($peaks.tracks.Count -eq 2 -and $mixPeak -gt 0.01 -and $stemPeak -gt 0.01) 'both joined lanes carry sound'
  $double = ($mixPeak + $stemPeak) / [Math]::Max($stemPeak, 0.000001)
  Check ($double -ge 1.5) ("an import reading would play both lanes at once ({0:N2}x one lane)" -f $double)

  # ---- 2. different lanes, one input incomplete ------------------------------------------------
  Write-Host 'case 2: different lanes, one input incomplete'
  $c = CopyFixture 'Desktop Record.mp4' 'c.mp4'
  $d = CopyFixture 'Incomplete Mix.mp4' 'd.mp4'
  $di = Info $d
  Check ($di.audio_aggregate -and -not $di.audio_complete) 'the second fixture is an incomplete mix (baseline)'
  $join = Join @($c, $d) 'incomplete.mp4'
  Check $join.ok "the join is made ($(Verdict $join.log))"
  $info = Info $join.output
  if ($isFixed) {
    Check (Test-Path -LiteralPath (Sidecar $join.output)) 'the join keeps mix-first labels'
    Check ($info.audio_aggregate -and -not $info.audio_complete) 'the output stays mix-first and locked'
    Check (@($info.audio[0].sources) -contains 'join:unlisted') 'the mix says it holds sound no stem carries'
    Check ($info.audio[1].role -eq 'desktop' -and @($info.audio[1].sources) -contains 'desktop') 'the lane both clips share keeps its identity'
    $levelled = ExportOne $join.output 'incomplete-levelled.mp4' @(1, 0.5)
    Check (-not $levelled.ok) 'a level change on the locked join is refused'
    Check ($levelled.log -match 'mix holds sound') 'the refusal says why the levels cannot be changed'
    Check (-not (Test-Path -LiteralPath $levelled.output)) 'the refused export leaves no file'
  } else {
    Check (-not (Test-Path -LiteralPath (Sidecar $join.output))) 'the old engine drops the labels (repro)'
    Check (-not $info.audio_buffrx) 'the old joined file reads as an import, so the mix plays beside its stems (repro)'
    $levelled = ExportOne $join.output 'incomplete-levelled.mp4' @(1, 0.5)
    Check $levelled.ok 'the old engine treats the two lanes as independent streams (repro)'
  }

  # ---- 3. different lane identities, both complete ---------------------------------------------
  Write-Host 'case 3: different lane identities, both complete'
  $e = CopyFixture 'Desktop Record.mp4' 'e.mp4'
  $f = CopyFixture 'Desktop Record.mp4' 'f.mp4'
  RelabelLane $f 'microphone' @('microphone') 'BuffrX Microphone'
  $fi = Info $f
  Check ($fi.audio_aggregate -and $fi.audio_complete -and $fi.audio[1].role -eq 'microphone') 'the relabelled fixture is still a complete mix-first clip (baseline)'
  $base = Top (RawPeaks $e).tracks[0]
  $join = Join @($e, $f) 'changing-lane.mp4'
  Check $join.ok "the join is made ($(Verdict $join.log))"
  $info = Info $join.output
  if ($isFixed) {
    Check (Test-Path -LiteralPath (Sidecar $join.output)) 'the join carries an honest positional layout'
    Check ($info.audio_aggregate -and $info.audio_complete) 'the output is a complete aggregate'
    Check ($info.audio[1].role -eq 'combined' -and @($info.audio[1].sources) -contains 'join:track:2') 'the changed lane gets its own logical id, not one clip''s label'
    Check (@($info.audio[0].sources) -contains 'join:track:2' -and -not (@($info.audio[0].sources) -contains 'join:unlisted')) 'the mix names that one logical lane'
    $levelled = ExportOne $join.output 'changing-lane-levelled.mp4' @(1, 0.5)
    Check $levelled.ok 'the joined lanes can still be rebuilt from a level edit'
    if ($levelled.ok) {
      $lp = RawPeaks $levelled.output
      $mix = Top $lp.tracks[0]
      $stem = Top $lp.tracks[1]
      $ratio = $mix / [Math]::Max($stem, 0.000001)
      Check ($ratio -gt 0.75 -and $ratio -lt 1.33) ("the rebuilt mix follows the stem once ({0:N2}x)" -f $ratio)
      Check ($mix -lt 0.75 * $base) 'the half-level edit reached the joined mix'
    }
  } else {
    Check (-not (Test-Path -LiteralPath (Sidecar $join.output))) 'the old engine drops the labels (repro)'
    Check (-not $info.audio_buffrx) 'the old joined file reads as an import (repro)'
  }

  # ---- 4. an aggregate joined with an import ---------------------------------------------------
  Write-Host 'case 4: an aggregate joined with an import'
  $g = CopyFixture 'Desktop Record.mp4' 'g.mp4'
  $h = CopyFixture 'Imported Pair.mp4' 'h.mp4'
  Check (-not (Info $h).audio_buffrx) 'the second fixture is an import (baseline)'
  $join = Join @($g, $h) 'mixed-kinds.mp4'
  if ($isFixed) {
    Check (-not $join.ok) 'equal track counts do not make the join compatible: it is refused'
    Check ($join.log -match 'hold their sound differently') 'the refusal says what is wrong'
    Check (-not (Test-Path -LiteralPath $join.output)) 'the refused join leaves no output file'
    Check (-not (Test-Path -LiteralPath ($join.output + '.part.mp4'))) 'the refused join leaves no staged file'
  } else {
    Check $join.ok 'the old engine writes the join (repro)'
    Check (Test-Path -LiteralPath $join.output) 'the old join exists (repro)'
    Check (-not (Test-Path -LiteralPath (Sidecar $join.output))) 'with no labels, its mix plays beside its stems (repro)'
  }

  # ---- 5. imports joined with imports ----------------------------------------------------------
  Write-Host 'case 5: imports joined with imports'
  $i1 = CopyFixture 'Imported Pair.mp4' 'i1.mp4'
  $i2 = CopyFixture 'Imported Pair.mp4' 'i2.mp4'
  $join = Join @($i1, $i2) 'imports.mp4'
  Check $join.ok "two imports join ($(Verdict $join.log))"
  Check (-not (Test-Path -LiteralPath (Sidecar $join.output))) 'an import join carries no labels'
  Check (-not (Info $join.output).audio_buffrx) 'the import join still reads as an import'

  $s1 = CopyFixture 'Six Tracks.mp4' 's1.mp4'
  $s2 = CopyFixture 'Six Tracks.mp4' 's2.mp4'
  $six = Join @($s1, $s2) 'six-imports.mp4'
  Check $six.ok "two six-track imports join ($(Verdict $six.log))"
  Check ((Info $six.output).audio.Count -eq 6) 'all six independent streams are kept'
  Check (-not (Test-Path -LiteralPath (Sidecar $six.output))) 'a six-track import join carries no labels either'

  # ---- 6. an edit made before the join, reopened after it --------------------------------------
  Write-Host 'case 6: a level edit before the join, reopened after it'
  $j1 = CopyFixture 'Desktop Record.mp4' 'j1.mp4'
  $j2 = CopyFixture 'Long Desktop Name.mp4' 'j2.mp4'
  $join = Join @($j1, $j2) 'edited.mp4' @(1, 0.5)
  Check $join.ok "the edited join is made ($(Verdict $join.log))"
  $info = Info $join.output
  if ($isFixed) {
    Check (Test-Path -LiteralPath (Sidecar $join.output)) 'the edited join carries its labels'
    Check ($info.audio_aggregate -and $info.audio_complete) 'the edited join reopens as a complete aggregate'
  } else {
    Check (-not (Test-Path -LiteralPath (Sidecar $join.output))) 'the old engine drops the labels of the edited join (repro)'
  }
  if ($join.ok) {
    $p = RawPeaks $join.output
    $mix = Top $p.tracks[0]
    $stem = Top $p.tracks[1]
    $ratio = $mix / [Math]::Max($stem, 0.000001)
    Check ($ratio -gt 0.75 -and $ratio -lt 1.33) ("the joined mix follows the edited stem once ({0:N2}x)" -f $ratio)
    Check ($mix -lt 0.75 * $base) 'the half-level edit reached the joined mix'
    if ($isFixed) {
      $again = ExportOne $join.output 'edited-again.mp4' @(1, 0.25)
      Check $again.ok 'the joined file can be edited and exported again'
      if ($again.ok) {
        $q = RawPeaks $again.output
        $mix2 = Top $q.tracks[0]
        $stem2 = Top $q.tracks[1]
        $ratio2 = $mix2 / [Math]::Max($stem2, 0.000001)
        Check ($ratio2 -gt 0.75 -and $ratio2 -lt 1.33) ("the reopened mix still follows its stem once ({0:N2}x)" -f $ratio2)
        Check ($mix2 -lt 0.45 * $base -and $mix2 -gt 0.10 * $base) 'the second level edit landed on the reopened mix'
      }
    } else {
      $again = ExportOne $join.output 'edited-again.mp4' @(1, 0.25)
      Check $again.ok 'the old engine exports the reopened file (repro)'
      if ($again.ok) {
        $q = RawPeaks $again.output
        $mix2 = Top $q.tracks[0]
        $stem2 = Top $q.tracks[1]
        $ratio2 = $mix2 / [Math]::Max($stem2, 0.000001)
        Check ($ratio2 -gt 2.5) ("the old reopened file levels independent streams, not a mix ({0:N2}x) (repro)" -f $ratio2)
      }
    }
  }

  # ---- 7. the raw join child (`--media-join`, what a Timeline save runs) -------------------------
  Write-Host 'case 7: the raw join child carries the same rule'
  $r1 = CopyFixture 'Desktop Record.mp4' 'r1.mp4'
  $r2 = CopyFixture 'Long Desktop Name.mp4' 'r2.mp4'
  $join = RawJoin @($r1, $r2) 'raw-same-lanes.mp4'
  $rawVerdict = if ($join.reply) { [string]$join.reply.error } else { 'no result' }
  Check $join.ok "the raw join is made ($rawVerdict)"
  if ($join.ok) {
    $info = Info $join.output
    if ($isFixed) {
      Check ($join.written -and $join.written.ok) 'the raw join writes its result beside the clip'
      Check (Test-Path -LiteralPath (Sidecar $join.output)) 'the raw join carries its track labels'
      Check ($info.audio_buffrx -and $info.audio_aggregate -and $info.audio_complete) 'the raw join reads back as a bound, complete aggregate'
      Check ($info.audio.Count -eq 2 -and $info.audio[1].role -eq 'desktop' -and @($info.audio[1].sources) -contains 'desktop') 'the raw join keeps the common desktop lane'
    } else {
      Check (-not (Test-Path -LiteralPath (Sidecar $join.output))) 'the old raw join drops the labels (repro)'
      Check (-not $info.audio_buffrx) 'the old raw joined file reads as an import (repro)'
    }
    $rawPeaks = RawPeaks $join.output
    $rawDouble = ((Top $rawPeaks.tracks[0]) + (Top $rawPeaks.tracks[1])) / [Math]::Max((Top $rawPeaks.tracks[1]), 0.000001)
    Check ($rawPeaks.tracks.Count -eq 2 -and $rawDouble -ge 1.5) ("the raw join holds the mix and its stem, never two copies of one lane ({0:N2}x one lane)" -f $rawDouble)
  }

  $r3 = CopyFixture 'Desktop Record.mp4' 'r3.mp4'
  $r4 = CopyFixture 'Imported Pair.mp4' 'r4.mp4'
  $refused = RawJoin @($r3, $r4) 'raw-mixed-kinds.mp4'
  if ($isFixed) {
    Check (-not $refused.ok) 'the raw join of a mix and an import is refused'
    Check ($refused.reply -and $refused.reply.error -match 'hold their sound differently') 'the raw refusal says what is wrong'
    Check ($refused.written -and -not $refused.written.ok -and $refused.written.error -match 'hold their sound differently') 'the raw refusal is written as a failed result for the engine to read'
    Check (-not (Test-Path -LiteralPath $refused.output)) 'the refused raw join leaves no output'
  } else {
    Check $refused.ok 'the old raw join writes the copy anyway (repro)'
    Check (-not (Test-Path -LiteralPath (Sidecar $refused.output))) 'and drops the labels, so its mix plays beside its stems (repro)'
  }

  # A refusal must not take a file that was already at the output name: it is not this join's to delete.
  if ($isFixed) {
    $sentinel = Join-Path $work 'raw-unrelated.mp4'
    Set-Content -LiteralPath $sentinel -Value 'not this join''s file' -NoNewline
    $sentinelHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $sentinel).Hash
    $blocked = RawJoin @($r3, $r4) 'raw-unrelated.mp4'
    Check (-not $blocked.ok) 'a raw join refused at a taken output name is still refused'
    Check ((Test-Path -LiteralPath $sentinel) -and (Get-FileHash -Algorithm SHA256 -LiteralPath $sentinel).Hash -eq $sentinelHash) 'and the file already at that name keeps its bytes'
  }

  $r5 = CopyFixture 'Imported Pair.mp4' 'r5.mp4'
  $r6 = CopyFixture 'Imported Pair.mp4' 'r6.mp4'
  $imports = RawJoin @($r5, $r6) 'raw-imports.mp4'
  $importVerdict = if ($imports.reply) { [string]$imports.reply.error } else { 'no result' }
  Check $imports.ok "two imports join raw ($importVerdict)"
  if ($imports.ok) {
    Check (-not (Test-Path -LiteralPath (Sidecar $imports.output))) 'a raw import join carries no labels'
    Check (-not (Info $imports.output).audio_buffrx) 'a raw import join still reads as an import'
  }

  Write-Host ''
  if ($failures -eq 0) { Write-Host "audio join e2e: all checks passed ($Expect)" }
  else { Write-Host "audio join e2e: $failures check(s) failed ($Expect)"; exit 1 }
}
finally {
  Remove-Item Env:\BUFFRX_DATA_ROOT -ErrorAction SilentlyContinue
  Remove-Item Env:\BUFFRX_PIPE -ErrorAction SilentlyContinue
  # The fixtures are shared evidence: say loudly if a run touched them.
  foreach ($file in Get-ChildItem -LiteralPath $Fixtures -File -Filter '*.mp4*') {
    if ($fixtureHashes.ContainsKey($file.FullName)) {
      $now = (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash
      if ($now -ne $fixtureHashes[$file.FullName]) { Write-Host "audio join e2e: FIXTURE CHANGED $($file.FullName)" }
    }
  }
  if ($KeepData) { Write-Host "kept: $dataRoot" }
  else { Remove-Item -Recurse -Force $dataRoot -ErrorAction SilentlyContinue }
}
