# What this checks: the editor's own audio levels reach a real exported file, and the raw waveform
# answer keeps a quiet track quiet instead of rounding it to silence.
#
#   powershell -File tools/audio-level-probe.ps1 -Source "<a 2-track recording>"
#
# The source is copied without its sidecar, so it is an import: its two tracks are independent and a
# volume reaches each one as it is. Three exports are made from the same bytes:
#   [1, 0.1]   two lanes about ten to one in the raw answer, each near full scale in the ordinary one
#   [1, 0.005] one lane a few thousandths of full scale, which two decimals would have rounded to zero
#   [1, 0]     one lane flat, the other loud
# Nothing here writes to a library: every file lives under a disposable data root.
param(
  [Parameter(Mandatory = $true)][string]$Source,
  [string]$Engine = 'buffrx-audio-review.exe',
  [switch]$KeepData
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$engineExe = Join-Path $root ('runtime\bin\64bit\' + $Engine)
if (-not (Test-Path $Source)) { throw "no source recording: $Source" }
if (-not (Test-Path $engineExe)) { throw "no engine: $engineExe" }

$dataRoot = Join-Path $env:TEMP ("buffrx-audio-level-" + [Guid]::NewGuid().ToString('N').Substring(0, 8))
$work = Join-Path $dataRoot 'clips\Edits'
New-Item -ItemType Directory -Force -Path $work | Out-Null
$env:BUFFRX_DATA_ROOT = $dataRoot
$env:BUFFRX_PIPE = "buffrx-audio-level-$PID"

function Fail([string]$why) {
  Write-Host "audio levels: FAIL $why"
  exit 1
}
function Pass([string]$what) { Write-Host "  ok   $what" }

# A native run with both streams captured: the engine logs to stderr, and a redirected native command's
# stderr reaches PowerShell as an error record that would stop this script.
function RunEngine([string[]]$arguments) {
  $tag = [Guid]::NewGuid().ToString('N').Substring(0, 8)
  $outFile = Join-Path $env:TEMP "buffrx-level-$tag.out.txt"
  $errFile = Join-Path $env:TEMP "buffrx-level-$tag.err.txt"
  $quoted = @($arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } })
  $process = Start-Process -FilePath $engineExe -ArgumentList $quoted -PassThru -Wait -NoNewWindow `
    -RedirectStandardOutput $outFile -RedirectStandardError $errFile
  $text = ((Get-Content -Raw $outFile -ErrorAction SilentlyContinue) + "`n" +
           (Get-Content -Raw $errFile -ErrorAction SilentlyContinue))
  Remove-Item -Force $outFile, $errFile -ErrorAction SilentlyContinue
  return $text
}

function Export([double[]]$volumes, [string]$name) {
  $output = Join-Path $work $name
  $spec = Join-Path $dataRoot ("job-$name.json")
  @{ input = (Join-Path $work 'source.mp4'); output = $output; volumes = $volumes; quality = 0; fail_if_exists = $true } |
    ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $spec -Encoding utf8
  $log = RunEngine @('--media-export', $spec)
  if ($log -notmatch 'EXPORT_OK') { Fail "the export $name failed: $log" }
  return $output
}

function Peaks([string]$file, [bool]$raw) {
  $arguments = @('--media-peaks', $file, '120')
  if ($raw) { $arguments += 'raw' }
  $text = RunEngine $arguments
  $line = ($text -split "`r?`n" | Where-Object { $_.TrimStart().StartsWith('{') } | Select-Object -Last 1)
  if (-not $line) { Fail "no waveform for $file" }
  return ($line | ConvertFrom-Json)
}

function Top($lane) { ($lane | Measure-Object -Maximum).Maximum }
function Mean($lane) { ($lane | Measure-Object -Average).Average }

try {
  # The same bytes without the layout beside them: an import, where each track is levelled on its own.
  Copy-Item $Source (Join-Path $work 'source.mp4')

  # --- a ten-to-one edit --------------------------------------------------
  $ten = Export @(1, 0.1) 'ten.mp4'
  $raw = Peaks $ten $true
  $plain = Peaks $ten $false
  if ($raw.raw -ne $true -or $plain.raw -ne $false) { Fail '--media-peaks did not say which scale it used' }
  if ($raw.tracks.Count -ne 2) { Fail "the ten-to-one export drew $($raw.tracks.Count) lanes, not 2" }
  $loud = Top $raw.tracks[0]
  $quiet = Top $raw.tracks[1]
  if ($loud -lt 0.2) { Fail "the loud lane is not loud in the raw answer ($loud)" }
  $ratio = $loud / [Math]::Max($quiet, 0.000001)
  if ($ratio -lt 5 -or $ratio -gt 15) { Fail "raw lanes are $ratio to one, not about ten" }
  Pass ("raw lanes keep the edit's ten-to-one level (ratio {0:N2})" -f $ratio)
  foreach ($lane in $plain.tracks) {
    if ((Mean $lane) -le 0.01) { continue }
    if ((Top $lane) -lt 0.9) { Fail "the ordinary answer stopped normalising a lane to its own maximum (top $(Top $lane))" }
  }
  Pass 'the ordinary answer still normalises each lane, as older callers expect'

  # --- a very quiet edit --------------------------------------------------
  # 0.005 of a 0.49 peak is about 0.0025: two decimals write every sample of it as 0.00.
  $faint = Export @(1, 0.005) 'faint.mp4'
  $faintRaw = Peaks $faint $true
  $faintQuiet = Top $faintRaw.tracks[1]
  if ($faintQuiet -le 0) { Fail 'a lane at 0.005 volume reads as pure silence in the raw answer' }
  if ($faintQuiet -ge 0.005) { Fail "the quiet lane is not quiet ($faintQuiet): the volume may not have reached it" }
  if ((Mean $faintRaw.tracks[1]) -le 0) { Fail 'the quiet lane has no non-zero sample, so it would draw flat' }
  Pass ("a 0.005 lane survives the raw answer (top {0:N5})" -f $faintQuiet)

  # --- a muted track ------------------------------------------------------
  $muted = Export @(1, 0) 'muted.mp4'
  $mutedRaw = Peaks $muted $true
  if ((Top $mutedRaw.tracks[0]) -lt 0.2) { Fail 'muting the second track silenced the first' }
  if ((Top $mutedRaw.tracks[1]) -gt 0.001) { Fail "the muted track still carries sound ($(Top $mutedRaw.tracks[1]))" }
  Pass 'a mute is silence on its own lane and leaves the other lane alone'

  Write-Host 'audio levels: all checks passed'
}
finally {
  Remove-Item Env:\BUFFRX_DATA_ROOT -ErrorAction SilentlyContinue
  Remove-Item Env:\BUFFRX_PIPE -ErrorAction SilentlyContinue
  if ($KeepData) { Write-Host "kept: $dataRoot" }
  else { Remove-Item -Recurse -Force $dataRoot -ErrorAction SilentlyContinue }
}
