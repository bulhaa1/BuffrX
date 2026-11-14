# Hands-free proof of automatic app audio inside a real Timeline session: a live writer, three test
# programs on their own lanes, one of them leaving and another taking the lane it freed, a chunk
# published across that change, a clip cut out of the session with `timeline.save`, and the clip read
# back and exported.
#
#   powershell -File tools/audio-auto-timeline-probe.ps1 -Engine build-ui\buffrx-engine.exe -Fixture build-ui\buffrx-audio-fixture.exe
#
# The engine runs on its own data root and pipe (never the one the person uses), and the programs on the
# lanes are copies of tests/audio_fixture.cpp with distinct names, so they are distinct programs to
# Windows. Any other app making sound on this machine is excluded before the checks, so the person's own
# audio is never captured. The picture is kept moving by tools/screen-noise.ps1: a display capture only
# hands over a frame when the screen changes, and a still desktop would leave the session with no footage.
#
# Honest limits: the "programs" are silent-tone renderers, not real applications; the Timeline is told a
# game is running (BUFFRX_TEST_TIMELINE_GAME), because a game gate cannot be produced on demand here.
param(
  [string]$Engine = 'build-ui\buffrx-engine.exe',
  [string]$Fixture = 'build-ui\buffrx-audio-fixture.exe',
  [string]$PrivateRoot = 'work\auto-ui-runtime',
  [int]$TimeoutSeconds = 90,
  [switch]$Keep
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot 'probe-lib.ps1')

$enginePath = (Resolve-Path (Join-Path $repo $Engine)).Path
$fixturePath = (Resolve-Path (Join-Path $repo $Fixture)).Path
$bin = Join-Path $repo "$PrivateRoot\bin\64bit"
if (-not (Test-Path -LiteralPath (Join-Path $bin 'obs.dll'))) { throw "the private runtime is not staged: $bin" }
$engineExe = Join-Path $bin 'buffrx-engine.exe'
Copy-Item -LiteralPath $enginePath -Destination $engineExe -Force
$privateEngine = [pscustomobject]@{ Exe = $engineExe; Bin = $bin; RuntimeRoot = (Resolve-Path (Join-Path $repo 'runtime')).Path }
$dataRoot = Join-Path $env:TEMP ('buffrx-auto-timeline-' + $PID)
$shots = Join-Path $env:TEMP ('buffrx-auto-timeline-shots-' + $PID)
New-Item -ItemType Directory -Force $shots | Out-Null

$script:failures = 0
function Check([string]$what, [bool]$ok, [string]$detail = '') {
  $suffix = if ($detail) { '  ' + $detail } else { '' }
  if ($ok) { Write-Host ('  ok   ' + $what + $suffix) } else { Write-Host ('  FAIL ' + $what + $suffix) -ForegroundColor Red; $script:failures++ }
}
function Ask([string]$command, [string]$arguments = '{}') { Ask-Engine $e $command $arguments }
function AskJson([string]$command, [string]$arguments = '{}') { (Ask-Engine $e $command $arguments) | ConvertFrom-Json }
function Timeline { (AskJson 'timeline.get').data }
function Status { (AskJson 'status.get').data }
function MediaInfo([string]$file) { (& $privateEngine.Exe --media-info $file | Select-Object -Last 1 | ConvertFrom-Json) }
function Wait-Until([scriptblock]$condition, [int]$seconds = 60) {
  for ($i = 0; $i -lt $seconds * 2; $i++) { if (& $condition) { return $true }; Start-Sleep -Milliseconds 500 }
  return $false
}

$probePrefs = @{}
foreach ($process in Get-Process) {
  # Console fixtures can own a console host child; excluding that child would correctly block the tree.
  if ($process.ProcessName -in @('conhost', 'OpenConsole')) { continue }
  $probePrefs[($process.ProcessName + '.exe').ToLowerInvariant()] = @{ excluded = $true }
}
foreach ($name in @('probea.exe', 'probeb.exe', 'probec.exe', 'probed.exe')) {
  $probePrefs[$name] = @{ pinned = $true; excluded = $false }
}
$config = @{
  config_version = 14; codec_auto_picked = $true; start_buffering = $false; auto_game_capture = $false
  notifications = $false; notification_sound = $false; mic_enabled = $true; desktop_enabled = $true
  audio_capture_mode = 'auto'; timeline_minutes = 30; timeline_everywhere = $true
  capture_mode = 'display'; capture_fps = 60; codec = 'h264'; container = 'mp4'; quality = 'standard'
  audio_tracks = 6; separate_tracks = $true; buffer_seconds = 30; chunk_seconds = 5
  audio_app_prefs = $probePrefs
} | ConvertTo-Json -Compress

$fixtures = @()
$noise = $null
$e = $null
try {
  # The Timeline writes only beside a running game; the engine is told to see one for this run.
  $e = Start-ProbeEngine $config $privateEngine.Exe @{ BUFFRX_TEST_TIMELINE_GAME = '1'; BUFFRX_RUNTIME_ROOT = (Resolve-Path (Join-Path $repo 'runtime')).Path } 0 $dataRoot
  $noise = Start-Process -FilePath 'powershell' -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'screen-noise.ps1') -PassThru -WindowStyle Hidden
  $st = Status
  Check 'the first-run configuration is automatic capture' ([bool]$st.audio.auto.on)

  # Every other program making sound is excluded, so the lanes belong to the probe and no personal audio
  # is ever written into the session.
  for ($i = 0; $i -lt 8; $i++) {
    $intruders = @()
    foreach ($lane in @((Status).audio.auto.lanes)) { foreach ($app in @($lane.apps)) { if ($app.exe -and $app.exe -notlike 'Probe*') { $intruders += $app.exe } } }
    $intruders = @($intruders | Sort-Object -Unique)
    if ($intruders.Count -eq 0) { break }
    $prefs = @{}
    foreach ($exe in $intruders) { $prefs[$exe.ToLowerInvariant()] = @{ excluded = $true } }
    $null = Ask 'audio.set' (@{ audio_app_prefs = $prefs } | ConvertTo-Json -Depth 5 -Compress)
    Start-Sleep -Milliseconds 800
  }
  $prefs = @{}
  foreach ($name in @('probea.exe', 'probeb.exe', 'probec.exe', 'probed.exe')) { $prefs[$name] = @{ pinned = $true } }
  $null = Ask 'audio.set' (@{ audio_app_prefs = $prefs } | ConvertTo-Json -Depth 5 -Compress)

  foreach ($name in @('ProbeA.exe', 'ProbeB.exe', 'ProbeC.exe')) {
    $exe = Join-Path $bin $name
    Copy-Item -LiteralPath $fixturePath -Destination $exe -Force
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $exe; $info.WorkingDirectory = $bin; $info.UseShellExecute = $false
    $info.CreateNoWindow = $true; $info.RedirectStandardInput = $true; $info.RedirectStandardOutput = $true
    $fixtures += [System.Diagnostics.Process]::Start($info)
    Start-Sleep -Milliseconds 600
  }
  $null = Ask 'buffer.start'
  $started = Wait-Until {
    $names = @((Status).audio.auto.lanes | Where-Object { -not $_.other } | ForEach-Object { $_.exe.ToLowerInvariant() })
    [bool]((Timeline).enabled) -and (@($names | Sort-Object) -join ',') -eq 'probea.exe,probeb.exe,probec.exe'
  } 30
  $st = Status
  Check 'the buffer runs with three programs on their own lanes' `
    ($started -and [bool]$st.buffering -and @($st.audio.auto.lanes | Where-Object { -not $_.other }).Count -ge 3) `
    ('lanes: ' + (@($st.audio.auto.lanes | ForEach-Object { if ($_.other) { 'OTHER' } else { $_.exe } }) -join ','))

  $session = ''
  $firstEnd = 0
  $gotChunks = Wait-Until {
    $t = Timeline
    $script:session = $t.current_session
    $span = [double]$t.available_end_ms
    if ($t.current_session -and @($t.chunks).Count -ge 1 -and $span -ge 8000) { $script:firstEnd = $span; return $true }
    return $false
  } $TimeoutSeconds
  $session = $script:session
  Check 'the Timeline writer publishes footage of the live session' ($gotChunks -and $session) `
    ('session ' + $session + ', published to ' + [math]::Round($script:firstEnd) + ' ms')

  # A program leaves and another takes the lane it freed, while the writer keeps writing.
  $a = $fixtures[0]
  try { $a.StandardInput.WriteLine('quit'); $a.WaitForExit(3000) | Out-Null } catch { }
  $fixtures = @($fixtures | Where-Object { $_ -ne $a })
  $d = Join-Path $bin 'ProbeD.exe'
  Copy-Item -LiteralPath $fixturePath -Destination $d -Force
  $info = New-Object System.Diagnostics.ProcessStartInfo
  $info.FileName = $d; $info.WorkingDirectory = $bin; $info.UseShellExecute = $false
  $info.CreateNoWindow = $true; $info.RedirectStandardInput = $true; $info.RedirectStandardOutput = $true
  $fixtures += [System.Diagnostics.Process]::Start($info)
  $reused = Wait-Until {
    $st = Status
    $live = @($st.audio.auto.lanes | ForEach-Object { @($_.apps) | ForEach-Object { $_.exe } })
    return (($live -join ',') -match 'ProbeD') -and (($live -join ',') -notmatch 'ProbeA')
  } 30
  Check 'a departed program leaves the lanes and another takes its place' ($reused) `
    ('lanes: ' + ((@((Status).audio.auto.lanes | ForEach-Object { @($_.apps) | ForEach-Object { $_.exe } }) | Sort-Object -Unique) -join ','))

  # The chunks that carry the change are published before a clip is cut across them.
  $crossed = Wait-Until { [double](Timeline).available_end_ms -ge ($script:firstEnd + 8000) } $TimeoutSeconds
  $t = Timeline
  Check 'the session publishes the chunk that holds the change' `
    ($crossed -and [double]$t.available_end_ms -gt [double]$script:firstEnd) `
    ('published to ' + [math]::Round([double]$t.available_end_ms) + ' ms')

  # A clip out of the session, across the lane change.
  $rangeStart = [math]::Max(0, [double]$t.available_start_ms)
  $rangeEnd = [double]$t.available_end_ms
  $rangeSeconds = ($rangeEnd - $rangeStart) / 1000.0
  $range = @{ session = $session; start_ms = $rangeStart; end_ms = $rangeEnd } | ConvertTo-Json -Compress
  $saved = AskJson 'timeline.save' $range
  $clip = [string]$saved.data.file
  Check 'timeline.save cuts a clip across the lane change' `
    ([bool]$saved.ok -and $clip -and (Test-Path -LiteralPath $clip) -and [int64]$saved.data.bytes -gt 0) `
    $(if ($saved.ok) { $clip } else { [string]$saved.error })

  if ($clip -and (Test-Path -LiteralPath $clip)) {
    $info = MediaInfo $clip
    Check 'the clip reads back as a complete six-track automatic layout' `
      ([bool]$info.audio_complete -and [bool]$info.audio_buffrx -and @($info.audio).Count -eq 6) `
      ('audio_complete=' + $info.audio_complete + ' tracks=' + @($info.audio).Count)
    $titles = (@($info.audio) | ForEach-Object { [string]$_.title }) -join ' | '
    Check 'the reused lane names both programs that played in it' `
      ($titles -match 'ProbeA' -and $titles -match 'ProbeD') $titles
    $length = [math]::Max(1, [math]::Floor([double]$info.duration))
    Check 'the clip has the footage the range asked for' `
      ([double]$info.duration -gt 1 -and [double]$info.duration -ge ($rangeSeconds - 4)) `
      ('duration ' + [math]::Round([double]$info.duration, 2) + ' s')

    # The editor's export: one lane level per track, then the file read back as an editable layout.
    $exported = Join-Path $dataRoot ('export-' + [guid]::NewGuid().ToString('N').Substring(0, 6) + '.mp4')
    $job = @{
      input = $clip; output = $exported; start = 0; end = $length; quality = 70
      volumes = @(1, 1, 1, 0.6, 0.6, 0.6)
    } | ConvertTo-Json -Compress
    $jobFile = Join-Path $dataRoot 'export.json'
    Set-Content -LiteralPath $jobFile -Value $job -Encoding ascii
    Push-Location $bin
    $native = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $exportLines = @(& $privateEngine.Exe --media-export $jobFile 2>&1 | ForEach-Object { [string]$_ })
    $ErrorActionPreference = $native
    Pop-Location
    $reply = [string](@($exportLines | Where-Object { $_ -match '^EXPORT_(OK|FAIL)' }) | Select-Object -Last 1)
    Check 'the editor exports the session clip at changed lane levels' ($reply -match '^EXPORT_OK') ([string]$reply)
    if (Test-Path -LiteralPath $exported) {
      $exportInfo = MediaInfo $exported
      Check 'the export reopens as a complete, editable six-track layout' `
        ([bool]$exportInfo.audio_complete -and @($exportInfo.audio).Count -eq 6) `
        ('audio_complete=' + $exportInfo.audio_complete + ' tracks=' + @($exportInfo.audio).Count)
    } else {
      Check 'the export reopens as a complete, editable six-track layout' $false 'no file was written'
    }
  }
} finally {
  foreach ($process in $fixtures) {
    try { $process.StandardInput.WriteLine('quit'); $process.WaitForExit(1500) | Out-Null } catch { }
    try { if (-not $process.HasExited) { $process.Kill() } } catch { }
  }
  if ($noise) { Stop-Process -Id $noise.Id -Force -ErrorAction SilentlyContinue }
  if ($e) {
    Copy-Item -LiteralPath (Join-Path $e.DataRoot 'logs\engine.log') -Destination (Join-Path $shots 'engine.log') -Force -ErrorAction SilentlyContinue
    Stop-ProbeEngine $e -KeepData:$Keep
  }
  if (-not $Keep) { Remove-Item -LiteralPath $shots -Recurse -Force -ErrorAction SilentlyContinue }
}
if ($script:failures -gt 0) {
  Write-Host ('automatic audio Timeline probe: ' + $script:failures + ' check(s) failed')
  exit 1
}
Write-Host 'automatic audio Timeline probe: all checks passed'
