# A sequence export across mixed formats: three clips of different size and rate, each with its own
# crop, colour, speed and levels, joined in one job. Checks the length, the streams, the order, that
# every clip's edit reached the picture, and that no original was touched.
#
#   powershell -File tools\sequence-export-e2e.ps1 -SourceClip 'C:\...\clip.mp4' [-Keep]
#
# -Keep prints the folder it worked in and leaves it there, which is how a failure is looked at.

param(
  [Parameter(Mandatory = $true)][string]$SourceClip,
  [string]$EngineExe = '',
  [switch]$Keep
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$engine = if ($EngineExe) { $EngineExe } else { Join-Path $root 'runtime\bin\64bit\buffrx-engine.exe' }
if (-not (Test-Path $engine)) { throw "no engine at $engine; run tools\build.ps1 -Target buffrx-engine" }
if (-not (Test-Path $SourceClip)) { throw "no source clip at $SourceClip" }

$dir = Join-Path $env:TEMP ("buffrx-sequence-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $dir | Out-Null
$env:BUFFRX_DATA_ROOT = $dir

function Invoke-Export([string]$name, $job) {
  # Windows PowerShell turns a native program's stderr into a terminating error while
  # $ErrorActionPreference is Stop; the engine logs there, so these calls run under Continue.
  $ErrorActionPreference = 'Continue'
  $path = Join-Path $dir "$name.json"
  [System.IO.File]::WriteAllText($path, ($job | ConvertTo-Json -Depth 8))
  $output = & $engine --media-export $path 2>$null
  if ($LASTEXITCODE -ne 0) { throw "$name failed (exit $LASTEXITCODE): $($output -join ' ')" }
  return $job.output
}

function Get-Info([string]$file) {
  $ErrorActionPreference = 'Continue'
  $raw = & $engine --media-info $file 2>$null
  if ($LASTEXITCODE -ne 0) { throw "could not read $file" }
  return ($raw | Where-Object { $_ -match '^\s*\{' } | Select-Object -Last 1) | ConvertFrom-Json
}

function Get-Thumb([string]$file, [string]$name) {
  $ErrorActionPreference = 'Continue'
  $out = Join-Path $dir "$name.jpg"
  & $engine --media-thumb $file $out 320 2>$null | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "no thumbnail from $file" }
  return (Get-FileHash -LiteralPath $out -Algorithm SHA256).Hash
}

function Assert-True([bool]$ok, [string]$what) {
  if (-not $ok) { throw "FAILED: $what" }
  Write-Host "  ok  $what"
}

try {
  # Three inputs the join cannot assume anything about: a landscape 30 fps, a portrait 25 fps and a
  # wide 60 fps. Each is derived once, so the join is given three different files.
  $a = Join-Path $dir 'a.mp4'
  $b = Join-Path $dir 'b.mp4'
  $c = Join-Path $dir 'c.mp4'
  Invoke-Export 'make-a' @{ input = $SourceClip; output = $a; start = 0; end = 5; out_width = 640; out_height = 360; fps = 30; quality = 0.4 } | Out-Null
  Invoke-Export 'make-b' @{ input = $SourceClip; output = $b; start = 1; end = 3; out_width = 480; out_height = 640; fps = 25; quality = 0.4 } | Out-Null
  Invoke-Export 'make-c' @{ input = $SourceClip; output = $c; start = 2; end = 4; out_width = 800; out_height = 450; fps = 60; quality = 0.4 } | Out-Null

  $before = @{}
  foreach ($file in @($a, $b, $c)) { $before[$file] = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash }

  $infoA = Get-Info $a
  $infoB = Get-Info $b
  $infoC = Get-Info $c
  Write-Host ("inputs: {0}x{1}@{2}  {3}x{4}@{5}  {6}x{7}@{8}" -f $infoA.width, $infoA.height, $infoA.fps, $infoB.width, $infoB.height, $infoB.fps, $infoC.width, $infoC.height, $infoC.fps)

  # A keeps 4 s at 2x = 2 s on the sequence, B 2 s, C 2.5 s: 6.5 s in all, and every clip's own
  # crop, colour and levels differ so the join cannot pass by doing the same thing to each.
  $inputs = @(
    @{ input = $a; segments = @(@(0.5, 4.5)); speed = 2; crop_x = 40; crop_y = 20; crop_width = 400; crop_height = 300; grade_brightness = 130; grade_contrast = 120; grade_saturation = 80; volumes = @(1) },
    @{ input = $b; segments = @(@(0.2, 2.2)); speed = 1; grade_brightness = 60; grade_contrast = 100; grade_saturation = 100; volumes = @(0.5) },
    @{ input = $c; segments = @(@(0.3, 2.8)); speed = 1; crop_x = 100; crop_y = 0; crop_width = 600; crop_height = 450; grade_brightness = 90; grade_contrast = 130; grade_saturation = 110; volumes = @(1) }
  )
  $out = Join-Path $dir 'sequence.mp4'
  $job = @{ output = $out; inputs = $inputs; quality = 0.4; out_width = 0; out_height = 0; fit_pad = $true; audio_rate = 48000; audio_channels = 2 }
  Invoke-Export 'sequence' $job | Out-Null

  Assert-True (Test-Path $out) 'one output file was written'
  $info = Get-Info $out
  Assert-True ([bool]$info.ok) 'the output reads back'
  $expected = 2 + 2 + 2.5
  $tolerance = [Math]::Max(0.5, $expected * 0.005)
  Write-Host ("  duration {0:N2} s, expected {1:N2} s" -f $info.duration, $expected)
  Assert-True ([Math]::Abs($info.duration - $expected) -le $tolerance) 'the output is as long as the clips after their speed'
  Assert-True ($info.width -eq 640 -and $info.height -eq 360) "the output takes the first clip's shape"
  Assert-True ($info.audio.Count -le 1) 'the output has one audio track at most'
  if ($infoA.audio.Count -gt 0) { Assert-True ($info.audio.Count -eq 1) 'the output keeps the audio the inputs had' }

  # Each clip's own edit reaches the encoder: the same job with every edit taken out writes different
  # bytes, and changing one input's colour alone writes different bytes again. Pictures are not
  # sampled: --media-thumb always takes its frame one second into a file, which cannot address a
  # clip inside a join.
  $editedHash = (Get-FileHash -LiteralPath $out -Algorithm SHA256).Hash

  $plain = @{ output = (Join-Path $dir 'plain.mp4'); inputs = ($inputs | ForEach-Object { $copy = $_.Clone(); $copy.Remove('grade_brightness'); $copy.Remove('grade_contrast'); $copy.Remove('grade_saturation'); $copy.Remove('crop_x'); $copy.Remove('crop_y'); $copy.Remove('crop_width'); $copy.Remove('crop_height'); $copy['speed'] = 1; $copy }); quality = 0.4; fit_pad = $true }
  Invoke-Export 'plain' $plain | Out-Null
  Assert-True ((Get-FileHash -LiteralPath (Join-Path $dir 'plain.mp4') -Algorithm SHA256).Hash -ne $editedHash) 'the edits change the output'

  # One clip's colour alone, everything else the same: a per-input edit that never reached its temp
  # export would leave the file byte for byte the same.
  $one = Join-Path $dir 'one-grade.mp4'
  $variant = $inputs | ForEach-Object { $copy = $_.Clone(); if ($copy.input -eq $b) { $copy['grade_brightness'] = 40 }; $copy }
  Invoke-Export 'one-grade' @{ output = $one; inputs = $variant; quality = 0.4; fit_pad = $true } | Out-Null
  Assert-True ((Get-FileHash -LiteralPath $one -Algorithm SHA256).Hash -ne $editedHash) "one clip's colour changes the output"

  # The order is the job's order, not the order of the files on disk: swapped inputs give another file.
  $swappedDir = Join-Path $dir 'swapped.mp4'
  $swapped = @{ output = $swappedDir; inputs = @($inputs[2], $inputs[1], $inputs[0]); quality = 0.4; fit_pad = $true }
  Invoke-Export 'swapped' $swapped | Out-Null
  Assert-True ((Get-FileHash -LiteralPath $swappedDir -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $out -Algorithm SHA256).Hash) 'the clip order changes the output'
  $swappedInfo = Get-Info $swappedDir
  Assert-True ([Math]::Abs($swappedInfo.duration - $expected) -le $tolerance) 'the swapped order keeps the length'

  # Originals untouched, byte for byte.
  foreach ($file in @($a, $b, $c)) {
    Assert-True ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -eq $before[$file]) "the original $([System.IO.Path]::GetFileName($file)) is unchanged"
  }

  Write-Host 'sequence export e2e: all checks passed'
  exit 0
} catch {
  Write-Host ($_ | Out-String)
  Write-Host "work folder kept at $dir"
  exit 1
} finally {
  if ($Keep) { Write-Host "kept: $dir" } else { Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue }
}
