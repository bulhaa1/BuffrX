# N7 chaos A: kill the engine while it buffers, restart it on the same data root, and require that what was written
# survives: every session's index lists every file in its media folder, no ring file is left behind, the config is
# untouched and the library still lists. The ring is forced small so it wraps, Timeline writes, and a moving window
# gives the capture footage.
#   powershell -File tools\chaos-restart.ps1 -Cycles 40 -Engine buffrx-engine-night.exe
param(
  [int]$Cycles = 40,
  [int]$BufferSeconds = 20,
  [string]$Engine = 'buffrx-engine.exe',
  [switch]$KeepData
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'probe-lib.ps1')

$root = Join-Path $env:TEMP ('buffrx-chaos-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force -Path $root | Out-Null
$config = '{"config_version":7,"start_buffering":false,"auto_game_capture":false,"timeline_minutes":30,"buffer_seconds":10,"codec":"h264","codec_auto_picked":true,"capture_fps":30,"output_width":1280,"output_height":720}'
$motion = Start-Process -FilePath (Join-Path $PSScriptRoot '..\build\buffrx-present-loop.exe') `
  -ArgumentList @('--seconds', [string]($Cycles * ($BufferSeconds + 12) + 180), '--width', '1280', '--height', '720', '--motion', '1') -PassThru
"chaos-restart: root $root, motion PID $($motion.Id)"
$failures = New-Object System.Collections.Generic.List[string]
$script:cycle = 0
$script:sessions_checked = 0
$script:chunks_checked = 0
$script:no_session_reported = $false
$configBytes = $null

function Fail([string]$what) {
  $script:failures.Add("cycle $script:cycle`: $what")
  "  FAIL $what"
}

# Everything the last kill should not have damaged: the index of every session names every file in its media folder,
# no ring file is still on disk, and the files themselves are not empty.
function Measure-Root([string]$dataRoot) {
  $problems = New-Object System.Collections.Generic.List[string]
  $sessions = 0
  $files_seen = 0
  $sessionsDir = Join-Path $dataRoot 'clips\sessions'
  if (-not (Test-Path -LiteralPath $sessionsDir)) {
    # A pass over no sessions is no evidence at all: say so once, not once per cycle.
    $problems.Add("no Timeline session was written under $sessionsDir")
  } else {
    $dirs = @(Get-ChildItem -LiteralPath $sessionsDir -Directory)
    $sessions = $dirs.Count
    foreach ($dir in $dirs) {
      $index = Join-Path $dir.FullName 'index.json'
      if (-not (Test-Path -LiteralPath $index)) { $problems.Add("session '$($dir.Name)' has no index.json"); continue }
      try { $json = Get-Content -LiteralPath $index -Raw | ConvertFrom-Json } catch { $problems.Add("session '$($dir.Name)': index.json is not valid JSON"); continue }
      $listed = @{}
      foreach ($chunk in @($json.chunks)) { if ($chunk -and $chunk.file) { $listed[(Split-Path ([string]$chunk.file) -Leaf)] = $true } }
      $media = Join-Path $dir.FullName 'media'
      if (Test-Path -LiteralPath $media) {
        foreach ($file in Get-ChildItem -LiteralPath $media -File -Filter *.mp4) {
          $files_seen += 1
          if (-not $listed.ContainsKey($file.Name)) { $problems.Add("session '$($dir.Name)': $($file.Name) is not in the index") }
          elseif ($file.Length -le 0) { $problems.Add("session '$($dir.Name)': $($file.Name) is empty") }
        }
      }
    }
  }
  $rings = @(Get-ChildItem -LiteralPath (Join-Path $dataRoot 'clips') -Filter 'replay-*.ring' -Recurse -ErrorAction SilentlyContinue) +
           @(Get-ChildItem -LiteralPath $dataRoot -Filter 'replay-*.ring' -Recurse -ErrorAction SilentlyContinue)
  if ($rings.Count -gt 0) { $problems.Add("a ring file was left behind: $($rings[0].FullName)") }
  [pscustomobject]@{ Problems = $problems; Sessions = $sessions; Files = $files_seen }
}

# A kill leaves its tail chunk on disk before the index learns of it, and the next engine files it when it first reads
# the session. The check therefore asks for that read and waits for it instead of calling the gap a failure: the point
# of the check is that no footage is lost, not that recovery is instant.
function Check-Root([string]$dataRoot, $engine = $null) {
  $state = $null
  for ($attempt = 0; $attempt -lt 20; ++$attempt) {
    $state = Measure-Root $dataRoot
    if ($state.Problems.Count -eq 0) { break }
    if ($engine) { $null = Ask-Engine $engine 'timeline.get' }
    Start-Sleep -Milliseconds 500
  }
  $script:sessions_checked += $state.Sessions
  $script:chunks_checked += $state.Files
  foreach ($problem in $state.Problems) {
    if ($problem -like 'no Timeline session*') {
      if ($script:no_session_reported) { continue }
      $script:no_session_reported = $true
    }
    Fail $problem
  }
}

try {
  for ($cycle = 1; $cycle -le $Cycles; ++$cycle) {
    $script:cycle = $cycle
    $e = $null
    try { $e = Start-ProbeEngine $config $Engine @{ BUFFRX_RING_RAM_MB = '128'; BUFFRX_TEST_TIMELINE_GAME = '1' } 0 $root }
    catch { Fail "the engine did not answer within 20 s: $($_.Exception.Message)"; break }
    "cycle $cycle`: engine PID $($e.Process.Id)"
    if ($cycle -eq 1) {
      $configBytes = [Convert]::ToBase64String([IO.File]::ReadAllBytes((Join-Path $root 'config.json')))
    } else {
      Check-Root $root $e
      $now = [Convert]::ToBase64String([IO.File]::ReadAllBytes((Join-Path $root 'config.json')))
      if ($now -ne $configBytes) { Fail 'config.json changed across a kill and restart' }
      $library = Ask-Engine $e 'clips.list'
      if ($library -notmatch '"ok":true') { Fail "clips.list did not answer: $library" }
    }
    # Display duplication delivers nothing on this machine tonight, so the footage comes from the moving window
    # (allowed: a window, no capture of the screen).
    $null = Set-WindowCapture $e
    $null = Ask-Engine $e 'buffer.start'
    if (-not (Wait-Buffering $e)) { Fail 'the buffer never started' }
    Start-Sleep -Seconds $BufferSeconds
    Stop-Process -Id $e.Process.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 800
  }
  # The last kill needs its own restart to recover and be checked. Check before a new buffer starts: the new live
  # session has no index until its first chunk closes, and checking it here would fail on footage that is still
  # being written, not on footage a kill left behind.
  if ($failures.Count -eq 0 -and $script:cycle -ge 2) {
    $e = Start-ProbeEngine $config $Engine @{ BUFFRX_RING_RAM_MB = '128'; BUFFRX_TEST_TIMELINE_GAME = '1' } 0 $root
    "final restart: engine PID $($e.Process.Id)"
    Check-Root $root $e
    $null = Set-WindowCapture $e
    $null = Ask-Engine $e 'buffer.start'
    if (-not (Wait-Buffering $e)) { Fail 'the buffer never started after the last kill' }
    Stop-Process -Id $e.Process.Id -Force -ErrorAction SilentlyContinue
  }
} finally {
  Stop-Process -Id $motion.Id -Force -ErrorAction SilentlyContinue
  if (-not $KeepData) { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue } else { "kept $root" }
}
"chaos-restart: $Cycles cycles, $($script:sessions_checked) sessions and $($script:chunks_checked) files checked, $($failures.Count) failure(s)"
foreach ($line in $failures) { "  $line" }
if ($failures.Count -gt 0) { exit 1 }
