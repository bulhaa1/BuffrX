# Timeline with a real game: launched, recorded, alt-tabbed, quit, and the aftermath checked.
#
#   powershell -File tools\timeline-game-session.ps1                      # 3 min of game, then the checks
#   powershell -File tools\timeline-game-session.ps1 -Minutes 1 -NoLaunch  # game already open
#
# What it proves, in order: Timeline writes while the game is detected; a manual recording survives the
# game closing; the game closing leaves the buffer on and Timeline at paused_no_game; the footage the
# session wrote is all on disk (index against files); a clip cut from it plays.
# Exit 0 = every check passed, 1 = a check failed, 2 = the game never appeared.
param(
  [int]$Minutes = 3,
  [string]$GameLike = 'KovaaK',
  [string]$GameExe = 'D:\KovaaKs.v3.9.10\KovaaKs.v3.9.10\FPSAimTrainer.exe',
  [string]$GameProcess = 'FPSAimTrainer-Win64-Shipping',
  [string]$EngineExe = 'buffrx-engine.exe',
  [string]$Ffprobe = 'ffprobe',
  [switch]$NoLaunch,
  [switch]$KeepData
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'probe-lib.ps1')
$stamp = Get-Date -Format 'HHmmss'
$logPath = Join-Path $env:TEMP "buffrx-timeline-game-$stamp.log"
function Say([string]$text) { $text | Tee-Object -FilePath $logPath -Append | Out-Host }
$g_checks = 0; $g_failed = 0
function Check([bool]$ok, [string]$name) {
  $script:g_checks++
  if ($ok) { Say "  ok   $name" } else { $script:g_failed++; Say "  FAIL $name" }
}

$game = $null
$engine = $null
$failure = ''
try {
  if (-not $NoLaunch) {
    if (-not (Test-Path $GameExe)) { Say "game not found: $GameExe"; exit 2 }
    Say "launching $GameExe"
    Start-Process $GameExe | Out-Null
  }
  $waited = 0
  while ($waited -lt 120) {
    $game = Get-Process $GameProcess -EA 0 | Select-Object -First 1
    if ($game) { break }
    Start-Sleep -Seconds 2; $waited += 2
  }
  if (-not $game) { Say "the game did not start within 120 s"; exit 2 }
  Say "game pid=$($game.Id) after $waited s; letting it settle 20 s (menus compile shaders)"
  Start-Sleep -Seconds 20

  $cfg = @{
    config_version = 7; start_buffering = $true; auto_game_capture = $true; timeline_minutes = 30
    capture_fps = 60; buffer_seconds = 30; codec = 'h264'; container = 'mp4'
    audio_tracks = 1; quality = 'standard'; notifications = $false; notification_sound = $false
    min_free_gb = 1; codec_auto_picked = $true; mic_enabled = $false
  } | ConvertTo-Json -Compress
  $engine = Start-ProbeEngine $cfg $EngineExe
  $null = Wait-Buffering $engine
  Say "buffer on; waiting for Timeline to detect the game"

  $states = New-Object System.Collections.Generic.List[string]
  $deadline = (Get-Date).AddSeconds(90)
  $writing = $false
  while ((Get-Date) -lt $deadline) {
    $s = (Ask-Engine $engine 'status.get' | ConvertFrom-Json).data
    $state = [string]$s.timeline.state
    if ($states.Count -eq 0 -or $states[$states.Count - 1] -ne $state) { $states.Add($state); Say "  state: $state (capture=$($s.capture.mode) target=$($s.capture.target))" }
    if ($state -eq 'writing') { $writing = $true; break }
    Start-Sleep -Seconds 3
  }
  Check $writing "Timeline writes while the game is detected"

  $null = Ask-Engine $engine 'recording.start'
  Say "manual recording started; $Minutes min of game play"
  $left = $Minutes * 60
  while ($left -gt 0) {
    Start-Sleep -Seconds 15; $left -= 15
    $s = (Ask-Engine $engine 'status.get' | ConvertFrom-Json).data
    Say ("  t-{0,3}s state={1} capture={2} fps={3} dropped={4} recording={5}" -f $left, $s.timeline.state, $s.capture.mode, $s.stats.capture_fps, $s.stats.dropped, $s.recording)
  }

  Say "alt-tabbing away from the game"
  # This runs from a hidden console, so there is no window of its own to bring forward, and a game in
  # the foreground refuses a plain AppActivate. Minimizing the game's own window puts a real other
  # window in front instead, which is what an alt-tab does as far as capture is concerned.
  if (-not ('BuffrXProbeWin' -as [type])) {
    Add-Type -Namespace BuffrX -Name ProbeWin -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
'@
  }
  $w = @(Get-Process $GameProcess -EA 0) | Select-Object -First 1
  $tookAway = $false
  if ($w -and $w.MainWindowHandle -ne 0) {
    $tookAway = [BuffrX.ProbeWin]::ShowWindow($w.MainWindowHandle, 6)   # SW_MINIMIZE
    Start-Sleep -Seconds 5
  }
  $s = (Ask-Engine $engine 'status.get' | ConvertFrom-Json).data
  Say "  after alt-tab: state=$($s.timeline.state) capture=$($s.capture.mode) target=$($s.capture.target) away=$tookAway"
  if ($tookAway) {
    Check ($s.timeline.state -eq 'writing' -or $s.capture.mode -eq 'game' -or $s.capture.mode -eq 'display') "capture continues after an alt-tab"
  } else {
    Say "  UNPROVEN: the game window could not be minimized, so the alt-tab row was not tested"
  }
  if ($w -and $w.MainWindowHandle -ne 0) {
    $null = [BuffrX.ProbeWin]::ShowWindow($w.MainWindowHandle, 9)        # SW_RESTORE
    Start-Sleep -Seconds 2
    $null = [BuffrX.ProbeWin]::SetForegroundWindow($w.MainWindowHandle)
    Start-Sleep -Seconds 3
  }

  Say "closing the game"
  $closed = $false
  foreach ($p in @(Get-Process $GameProcess -EA 0)) {
    if ($p.HasExited) { $closed = $true; continue }
    try { if ($p.CloseMainWindow()) { $closed = $p.WaitForExit(20000) } } catch { $closed = $p.HasExited }
  }
  if (-not $closed) {
    Get-Process $GameProcess -EA 0 | Stop-Process -Force -EA 0
    Start-Sleep -Seconds 3
  }
  Check (@(Get-Process $GameProcess -EA 0).Count -eq 0) "the game is gone"

  Start-Sleep -Seconds 8
  $s = (Ask-Engine $engine 'status.get' | ConvertFrom-Json).data
  Say "  after the game: state=$($s.timeline.state) buffering=$($s.buffering) recording=$($s.recording) capture=$($s.capture.mode)"
  Check ($s.timeline.state -eq 'paused_no_game') "Timeline says paused_no_game once the game is gone"
  Check ([bool]$s.buffering) "the buffer is still on"
  Check ([bool]$s.recording) "the manual recording is still running"

  $recordings = @(Get-ChildItem (Join-Path $engine.DataRoot 'clips\Recordings') -Recurse -File -Filter '*.mp4' -EA 0)
  Say ("  recordings: " + $(if ($recordings.Count) { ($recordings | ForEach-Object { "$($_.Name) $($_.Length)" }) -join '; ' } else { 'none' }))
  Check ($recordings.Count -eq 1) "one recording file, not cut in two"

  $t = (Ask-Engine $engine 'timeline.get' | ConvertFrom-Json).data
  Say "  session=$($t.session) chunks=$(@($t.chunks).Count) bytes=$($t.bytes) span=$($t.available_start_ms)..$($t.available_end_ms)"
  Check (@($t.chunks).Count -ge 2) "the session has published chunks"
  $onDisk = 0; $missing = @()
  foreach ($c in @($t.chunks)) {
    $p = [string]$c.file
    if (-not [System.IO.Path]::IsPathRooted($p)) { $p = Join-Path $env:USERPROFILE $p }
    if ((Test-Path $p) -and (Get-Item $p).Length -eq [int64]$c.bytes) { $onDisk++ } else { $missing += "$($c.file)=$($c.bytes)" }
  }
  Check ($onDisk -eq @($t.chunks).Count) "every indexed chunk is on disk with the size the index says$(if ($missing.Count) { ' (missing: ' + ($missing -join ', ') + ')' })"

  $end = [int]$t.available_end_ms
  $start = [Math]::Max(0, $end - 20000)
  $saved = Ask-Engine $engine 'timeline.save' (@{ session = $t.session; start_ms = $start; end_ms = $end } | ConvertTo-Json -Compress) | ConvertFrom-Json
  Say "  timeline.save -> ok=$($saved.ok) file=$($saved.data.file) bytes=$($saved.data.bytes) error=$($saved.error)"
  Check ([bool]$saved.ok) "a 20 s clip cut from the session saves"
  if ($saved.ok) {
    $clip = [string]$saved.data.file
    if (-not [System.IO.Path]::IsPathRooted($clip)) { $clip = Join-Path $engine.DataRoot $clip }
    if (Test-Path $clip) {
      $j = (& $Ffprobe -v error -show_entries 'format=duration:stream=codec_type' -of json $clip 2>$null | Out-String) | ConvertFrom-Json
      $dur = [double]$j.format.duration
      $hasVideo = @($j.streams | Where-Object { $_.codec_type -eq 'video' }).Count -gt 0
      Say "  clip: dur=$([math]::Round($dur,2)) s video=$hasVideo"
      Check ($hasVideo -and $dur -ge 18 -and $dur -le 23) "the saved clip is about 20 s of video"
    } else { Check $false "the saved clip is where the reply said ($clip)" }
  }

  $null = Ask-Engine $engine 'recording.stop'
  Start-Sleep -Seconds 3
  $recordings = @(Get-ChildItem (Join-Path $engine.DataRoot 'clips\Recordings') -Recurse -File -Filter '*.mp4' -EA 0)
  Say ("  recordings after stop: " + (($recordings | ForEach-Object { "$($_.Name) $($_.Length)" }) -join '; '))
  Check ($recordings.Count -eq 1) "the recording is still one file after it stops"
} catch {
  $failure = $_.Exception.Message
  Say "ERROR: $failure"
  Say ("  at: " + (($_.ScriptStackTrace -split "`n" | Select-Object -First 4) -join ' | '))
} finally {
  if ($engine) { Stop-ProbeEngine $engine -KeepData:$KeepData }
  if ($game -and -not $NoLaunch) {
    $left = Get-Process $GameProcess -EA 0
    if ($left) { $left | ForEach-Object { $null = $_.CloseMainWindow() }; Start-Sleep -Seconds 5; Get-Process $GameProcess -EA 0 | Stop-Process -Force -EA 0 }
  }
  Say ("timeline game session: {0} checks, {1} failed{2}" -f $g_checks, $g_failed, $(if ($failure) { " (error: $failure)" } else { '' }))
}
if ($g_failed -gt 0 -or $failure) { exit 1 }
exit 0
