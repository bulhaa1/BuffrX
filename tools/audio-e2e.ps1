# What this checks: a recorded clip carries the sound the machine was playing. Audio reaches the encoder
# only through a canvas channel (obs-audio.c mixes `root_nodes`), so a source that is captured and metered
# but never put on a channel writes digital silence - which is what every clip did until this was found. An
# AAC frame of silence is a handful of bytes; real sound is hundreds, so tools/mp4-samples.js tells them
# apart without decoding anything.
param([switch]$KeepData, [string]$Engine = 'buffrx-engine.exe')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$engineExe = Join-Path $root ('runtime\bin\64bit\' + $Engine)
$askScript = Join-Path $root 'tools\engine-ask.ps1'
$samplesScript = Join-Path $root 'tools\mp4-samples.js'
$dataRoot = Join-Path $env:TEMP "buffrx-audio-e2e-$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
$pipe = "buffrx-audio-e2e-$PID"

function Ask([string]$cmd, [string]$json = '{}', [int]$timeout = 15000) {
  $out = & powershell -NoProfile -File $askScript -Command $cmd -Arguments $json -TimeoutMs $timeout 2>&1
  return ($out | Out-String).Trim()
}

function Fail([string]$why) {
  Write-Host "audio e2e: FAIL $why"
  exit 1
}

New-Item -ItemType Directory -Force -Path (Join-Path $dataRoot 'clips\Screenshots') | Out-Null
$config = @{ clips_dir = (Join-Path $dataRoot 'clips'); screenshots_dir = (Join-Path $dataRoot 'clips\Screenshots'); auto_game_capture = $false; start_buffering = $false; notifications = $false }
[IO.File]::WriteAllText((Join-Path $dataRoot 'config.json'), ($config | ConvertTo-Json -Depth 4), (New-Object System.Text.UTF8Encoding($false)))

# A 4 s 440 Hz stereo tone at 48 kHz. Played on loop while the clip records.
$wav = Join-Path $env:TEMP 'buffrx-audio-e2e-tone.wav'
if (-not (Test-Path $wav)) {
  $rate = 48000; $n = $rate * 4
  $ms = New-Object System.IO.MemoryStream
  $bw = New-Object System.IO.BinaryWriter($ms)
  $bw.Write([Text.Encoding]::ASCII.GetBytes('RIFF')); $bw.Write([int](36 + $n * 4))
  $bw.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt ')); $bw.Write([int]16); $bw.Write([int16]1)
  $bw.Write([int16]2); $bw.Write([int]$rate); $bw.Write([int]($rate * 4)); $bw.Write([int16]4); $bw.Write([int16]16)
  $bw.Write([Text.Encoding]::ASCII.GetBytes('data')); $bw.Write([int]($n * 4))
  for ($i = 0; $i -lt $n; $i++) {
    $v = [int16](12000 * [Math]::Sin(2 * [Math]::PI * 440 * $i / $rate))
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
  # Only the picture the desktop already has: the display capture delivers a frame whenever anything on
  # screen changes, and a tone has to have started before the recording or the AAC track is legitimately
  # the silence of a quiet room.
  $proc = Start-Process -FilePath $engineExe -PassThru -WindowStyle Hidden -WorkingDirectory (Split-Path $engineExe) `
    -RedirectStandardOutput (Join-Path $dataRoot 'out.txt') -RedirectStandardError (Join-Path $dataRoot 'err.txt')
  $hello = $null
  for ($i = 0; $i -lt 40 -and -not $hello; $i++) { Start-Sleep -Milliseconds 250; $hello = Ask 'hello' '{}' 3000 }
  if (-not ($hello -match '"protocol"')) { Fail 'the engine did not answer on its pipe' }

  Start-Sleep -Seconds 4
  Ask 'recording.start' '{}' | Out-Null
  Start-Sleep -Seconds 9
  Ask 'recording.stop' '{}' | Out-Null
  Start-Sleep -Seconds 3

  $clip = Get-ChildItem -Path (Join-Path $dataRoot 'clips') -Recurse -Filter *.mp4 | Sort-Object LastWriteTime | Select-Object -Last 1
  if (-not $clip) { Fail 'the recording wrote no clip' }
  $report = & node $samplesScript $clip.FullName | Out-String
  Write-Host $report.Trim()
  $audio = [regex]::Match($report, 'soun samples=(\d+) bytes=(\d+) avg=([\d.]+)')
  if (-not $audio.Success) { Fail 'the clip has no audio track at all' }
  $avg = [double]$audio.Groups[3].Value
  # Silence is 1-25 bytes an AAC frame; a tone is several hundred. 100 sits far from both.
  if ($avg -lt 100) { Fail "the audio track is silent (average $avg bytes per AAC frame)" }
  Write-Host "audio e2e: all checks passed (average $avg bytes per AAC frame)"
}
finally {
  if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
  Stop-Process -Id $player.Id -Force -ErrorAction SilentlyContinue
  Remove-Item Env:\BUFFRX_DATA_ROOT -ErrorAction SilentlyContinue
  Remove-Item Env:\BUFFRX_PIPE -ErrorAction SilentlyContinue
  if (-not $KeepData) { Start-Sleep -Milliseconds 500; Remove-Item -Recurse -Force $dataRoot -ErrorAction SilentlyContinue }
}
