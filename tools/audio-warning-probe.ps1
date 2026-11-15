# What this checks, end to end on an isolated engine: a display capture over a running game, in Selected apps mode
# with nothing selected, publishes status.audio.problem=game_missing (the standing line) and makes the one card
# once after its grace; the card follows the notification setting; a real recovery and a mode switch clear it; a
# window capture of an unrelated program does not warn about the game it does not cover; and the stopped buffer
# says nothing.
#
# It runs a private copy of the built engine against a frozen OBS runtime, on a throwaway data root and pipe, so it
# sits beside the BuffrX the person is using and stops only the processes it started.
#
# The game is this product's own fullscreen test window (buffrx-present-loop --fullscreen --motion), which is the
# real foreground/fullscreen the detector judges. It is taken off the screen (minimized) as soon as the engine has
# detected it, so it is on the person's display for a few seconds only; nothing else is touched. Automatic game
# capture stays off, so the capture target remains the display - the case this warning had to learn to see.
#
# Run after a direct build:  powershell -File tools\audio-warning-probe.ps1 [-KeepData]
# Artifacts (engine log, console log, this run's notes) are saved under work\audio-warning-artifacts.
param([switch]$KeepData, [string]$Engine = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'probe-lib.ps1')

$script:failures = 0
function Fail([string]$why) { Write-Host "audio warning probe: FAIL $why"; $script:failures++ }
function Ok([string]$what) { Write-Host "ok   $what" }

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$artifacts = Join-Path $root 'work\audio-warning-artifacts'
New-Item -ItemType Directory -Force $artifacts | Out-Null

if (-not $Engine) {
  $Engine = Join-Path $root 'build\buffrx-engine.exe'
  if (-not (Test-Path -LiteralPath $Engine)) { throw 'build\buffrx-engine.exe is missing: build buffrx-engine first' }
}
$loop = Join-Path $root 'build\buffrx-present-loop.exe'
if (-not (Test-Path -LiteralPath $loop)) { throw 'build\buffrx-present-loop.exe is missing: build buffrx-present-loop first' }

# A private bin, so a concurrent build or the staged runtime cannot swap binaries under the run.
$privateRoot = Join-Path $root 'work\audio-warning-runtime'
$privateBin = Join-Path $privateRoot 'bin\64bit'
if (-not (Test-Path -LiteralPath (Join-Path $privateBin 'obs.dll'))) {
  New-Item -ItemType Directory -Force $privateBin | Out-Null
  Copy-Item -Path (Join-Path $root 'runtime\bin\64bit\*') -Destination $privateBin -Recurse -Force
}
Copy-Item -LiteralPath $Engine -Destination $privateBin -Force
Copy-Item -LiteralPath $loop -Destination $privateBin -Force
$engineExe = Join-Path $privateBin 'buffrx-engine.exe'
$loopExe = Join-Path $privateBin 'buffrx-present-loop.exe'

if (-not ('WarningWindows' -as [type])) {
  Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class WarningWindows {
  [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr hWnd, int command);
  public static bool Minimize(IntPtr hWnd) { return ShowWindowAsync(hWnd, 6); }
}
'@
}

# The engine's own start with stderr kept: `notice ...` lines are info and reach the console file, not engine.log.
function Start-ProbeEngineLogged([string]$configJson, [string]$runtimeRoot) {
  $dataRoot = Join-Path $env:TEMP ('buffrx-warn-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
  Assert-ProbeDataRoot $dataRoot | Out-Null
  $clipsDir = Join-Path $dataRoot 'clips'
  New-Item -ItemType Directory -Force $clipsDir | Out-Null
  $json = $configJson.TrimEnd('}').TrimEnd() + ',"clips_dir":"' + ($clipsDir -replace '\\', '/') +
          '","screenshots_dir":"' + (($dataRoot + '\shots') -replace '\\', '/') +
          '","recordings_dir":"' + (($dataRoot + '\recordings') -replace '\\', '/') + '"}'
  Set-Content -LiteralPath (Join-Path $dataRoot 'config.json') -NoNewline -Value $json
  $pipe = 'buffrx-warn-' + [guid]::NewGuid().ToString('N').Substring(0, 6)
  $stderr = Join-Path $dataRoot 'engine-stderr.log'
  $stdout = Join-Path $dataRoot 'engine-stdout.log'
  $env:BUFFRX_DATA_ROOT = $dataRoot; $env:BUFFRX_PIPE = $pipe; $env:BUFFRX_RUNTIME_ROOT = $runtimeRoot
  $proc = Start-Process -FilePath $engineExe -WorkingDirectory $privateBin -PassThru -WindowStyle Hidden `
    -RedirectStandardError $stderr -RedirectStandardOutput $stdout
  Remove-Item Env:BUFFRX_DATA_ROOT, Env:BUFFRX_PIPE, Env:BUFFRX_RUNTIME_ROOT -ErrorAction SilentlyContinue
  $up = $false
  for ($i = 0; $i -lt 40 -and -not $up; $i++) {
    Start-Sleep -Milliseconds 500
    try { (New-Object PanelPipe($pipe, 500)).Dispose(); $up = $true } catch { }
  }
  if (-not $up) { throw 'the engine did not answer on its pipe' }
  [pscustomobject]@{ Process = $proc; DataRoot = $dataRoot; Pipe = $pipe; Stderr = $stderr }
}

function Status { ((Ask-Engine $e 'status.get') | ConvertFrom-Json).data }
function Audio-Status { (Status).audio }
function Cards {
  if (-not (Test-Path -LiteralPath $e.Stderr)) { return 0 }
  @(Select-String -LiteralPath $e.Stderr -Pattern 'GAME AUDIO IS NOT RECORDED' -SimpleMatch).Count
}
function Set-Notifications([bool]$on) {
  $null = Ask-Engine $e 'config.set' (@{ patch = @{ notifications = $on } } | ConvertTo-Json -Compress)
}
function Set-AudioMode([string]$mode) {
  $null = Ask-Engine $e 'audio.set' (@{ audio_capture_mode = $mode } | ConvertTo-Json -Compress)
}
function Wait-Problem([string]$wanted, [int]$attempts = 40, [switch]$Steady) {
  $last = $null
  for ($i = 0; $i -lt $attempts; $i++) {
    Start-Sleep -Milliseconds 500
    $last = Audio-Status
    if ($last.problem -eq $wanted -and (-not $Steady -or $last.problem_steady)) { return $last }
  }
  return $last
}
function Wait-Cards([int]$count, [int]$attempts = 40) {
  for ($i = 0; $i -lt $attempts; $i++) {
    if ((Cards) -ge $count) { return $true }
    Start-Sleep -Milliseconds 500
  }
  return $false
}

$config = '{"start_buffering":false,"auto_game_capture":false,"notifications":false,"notification_sound":false,' +
          '"desktop_enabled":true,"mic_enabled":false,"separate_tracks":true,"audio_tracks":3,' +
          '"audio_capture_mode":"apps","audio_apps":[],"capture_mode":"display","timeline_minutes":30,' +
          '"timeline_everywhere":false,"performance_when":"off","pause_on_battery":false}'

$loopProc = $null
$formProc = $null
$formDir = $null
$e = $null
try {
  # The game: this product's own borderless fullscreen test window, with a moving picture so the display capture
  # has frames. It is minimized once the engine has detected it.
  $loopProc = Start-Process -FilePath $loopExe -ArgumentList @('--fullscreen', '1', '--motion', '1', '--vsync', '0', '--seconds', '600') -PassThru
  Start-Sleep -Milliseconds 2500
  $e = Start-ProbeEngineLogged $config (Join-Path $root 'runtime')
  Ok 'the isolated engine answers on its pipe'

  $off = Audio-Status
  if ($off.problem -ne '') { Fail ('a stopped buffer reported ' + $off.problem) } else { Ok 'no problem while nothing records' }

  $null = Ask-Engine $e 'buffer.start'
  for ($i = 0; $i -lt 40; $i++) {
    Start-Sleep -Milliseconds 500
    if ((Status).buffering) { break }
  }

  # The detector: automatic game capture is off and the capture target stays the display, so the Timeline gate is
  # what knows the game. The warning must name it without the Current game routing target existing at all.
  $detected = $null
  for ($i = 0; $i -lt 40; $i++) {
    $detected = Audio-Status
    if ($detected.game_exe -like '*present-loop*') { break }
    Start-Sleep -Milliseconds 500
  }
  if (-not ($detected.game_exe -like '*present-loop*')) {
    Fail ('the display capture never named the running game: ' + ($detected | ConvertTo-Json -Compress))
  } else {
    Ok ('the game the display covers is named with no game capture: ' + $detected.game_exe)
  }
  $state = Status
  if ($state.capture.mode -ne 'display') { Fail ('the capture mode was not the display: ' + $state.capture.mode) }
  if ([int]$detected.slots_used -ne 0) { Fail ('the probe started with selected apps: ' + $detected.slots_used) }

  # Off the person's screen, and still running: the warning judges a game that is playing, not one in front.
  if ($loopProc -and $loopProc.MainWindowHandle -ne [IntPtr]::Zero) {
    [WarningWindows]::Minimize($loopProc.MainWindowHandle) | Out-Null
    Ok 'the fullscreen test window is off the screen; the capture stays on the display'
  }

  # 1. Notices off: the page's standing line says it, and no card is made. That is the person's setting, not a bug.
  $missing = Wait-Problem 'game_missing' 40 -Steady
  Ok ('status.audio with notices off: ' + ($missing | ConvertTo-Json -Compress))
  if ($missing.problem -ne 'game_missing') { Fail ('status.audio.problem is not game_missing: ' + $missing.problem) }
  if (-not $missing.problem_steady) { Fail 'the missing game never settled into a steady problem' }
  if (-not $missing.recorded) { Fail 'the app mode did not report itself as the recorded audio' }
  if ((Cards) -ne 0) { Fail ('a card was made with notices off: ' + (Cards)) } else { Ok 'notices off: the line only' }

  # The page's own standing line, kept for review: the display capture's warning must read like the game capture's.
  $shot = Join-Path $artifacts 'audio-warning-display.png'
  $null = Ask-Engine $e 'ui.capture' (@{ view = 'settings:audio'; file = $shot } | ConvertTo-Json -Compress)
  Start-Sleep -Milliseconds 450
  $null = Ask-Engine $e 'ui.capture' (@{ view = 'settings:audio'; file = $shot } | ConvertTo-Json -Compress)
  Start-Sleep -Milliseconds 300
  if (-not (Test-Path -LiteralPath $shot)) { Fail 'the Audio page could not be captured' } else { Ok ('page captured: ' + $shot) }

  # 2. Notices on: a real recovery, then the problem again, earns exactly one card after its grace.
  Set-Notifications $true
  Set-AudioMode 'desktop'
  $recovered = Wait-Problem '' 20
  if ($recovered.problem -ne '') { Fail ('desktop audio mode still reported ' + $recovered.problem) } else { Ok 'the mode switch recovered the problem' }
  Set-AudioMode 'apps'
  $again = Wait-Problem 'game_missing' 40 -Steady
  if ($again.problem -ne 'game_missing') { Fail ('the app mode did not bring the problem back: ' + $again.problem) }
  if (-not (Wait-Cards 1 30)) { Fail 'no card was made after the grace with notices on' }
  $once = Cards
  Start-Sleep -Seconds 6
  $still = Cards
  Ok ('cards after the grace: ' + $once + ', six seconds later: ' + $still)
  if ($once -ne 1) { Fail ('the card was not made exactly once: ' + $once) }
  if ($still -ne $once) { Fail ('the card repeated while the problem stayed: ' + $still) }

  # 3. A window capture of another program: the game still runs and is detected, but it is not what is recorded, so
  #    nothing is said about its sound.
  $formDir = Join-Path $env:TEMP ('buffrx-warn-form-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
  New-Item -ItemType Directory -Force $formDir | Out-Null
  $formScript = Join-Path $formDir 'form.ps1'
  $stopPath = (Join-Path $formDir 'stop.txt') -replace '\\', '\\\\'
  @'
Add-Type -AssemblyName System.Windows.Forms
$form = New-Object System.Windows.Forms.Form
$form.Text = 'BuffrX Audio Warning Target'
$form.Width = 420
$form.Height = 220
$form.StartPosition = 'CenterScreen'
$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 500
$timer.add_Tick({ if (Test-Path '__STOP__') { $timer.Stop(); $form.Close() } })
$form.add_Shown({ $form.Activate(); $timer.Start() })
[System.Windows.Forms.Application]::Run($form)
'@.Replace('__STOP__', $stopPath) | Set-Content -LiteralPath $formScript -Encoding UTF8
  $formProc = Start-Process -FilePath 'powershell.exe' -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-WindowStyle', 'Hidden', '-File', $formScript) -PassThru
  $target = $null
  for ($i = 0; $i -lt 30 -and -not $target; $i++) {
    Start-Sleep -Milliseconds 500
    $rows = ((Ask-Engine $e 'sources.windows') | ConvertFrom-Json).data
    $target = $rows | Where-Object { $_.name -like '*Warning Target*' } | Select-Object -First 1
  }
  if (-not $target) { throw 'the window to capture was not in sources.windows' }
  $null = Ask-Engine $e 'capture.set' (@{ mode = 'window'; window_id = $target.id; window_name = $target.name } | ConvertTo-Json -Compress)
  $windowMode = Wait-Problem '' 20
  # The detector's own evidence, on the wire: Timeline names the game it still judges running. `status.game` is the
  # foreground line here, and it is empty because automatic capture is off - that is exactly this probe's setup.
  $timelineGame = ''
  for ($i = 0; $i -lt 30; $i++) {
    $timelineGame = (Status).timeline.game
    if ($timelineGame -like '*present-loop*') { break }
    Start-Sleep -Milliseconds 500
  }
  Ok ('window capture of another program: problem=' + $windowMode.problem + ', timeline game=' + $timelineGame)
  if ($windowMode.problem -ne '') { Fail ('a window capture of another program still warned about the game: ' + $windowMode.problem) }
  if (-not ($timelineGame -like '*present-loop*')) {
    Fail 'the detector had stopped knowing the game, so the window-mode check proved nothing'
  }
  if ((Cards) -ne $still) { Fail ('a card was made for a game the capture does not cover: ' + (Cards)) }

  # 4. Back to the display: the game is in the picture again, so the problem is back (the card keeps its once).
  $null = Ask-Engine $e 'capture.set' (@{ mode = 'display' } | ConvertTo-Json -Compress)
  $back = Wait-Problem 'game_missing' 20
  if ($back.problem -ne 'game_missing') { Fail ('the display capture did not warn again: ' + $back.problem) } else { Ok 'the display capture warns again over the game' }

  # 5. A stopped buffer: nothing records, so nothing is said.
  $null = Ask-Engine $e 'buffer.stop'
  $offAgain = Wait-Problem '' 20
  if ($offAgain.problem -ne '') { Fail ('a stopped buffer still reported ' + $offAgain.problem) } else { Ok 'nothing is said while nothing records' }

  Copy-Item -LiteralPath $e.Stderr -Destination (Join-Path $artifacts 'audio-warning-console.log') -Force
  if (Test-Path -LiteralPath (Join-Path $e.DataRoot 'logs\engine.log')) {
    Copy-Item -LiteralPath (Join-Path $e.DataRoot 'logs\engine.log') -Destination (Join-Path $artifacts 'audio-warning-engine.log') -Force
  }
  if ($script:failures -gt 0) { exit 1 }
  Write-Host 'audio warning probe: all checks passed'
  Write-Host ('logs: ' + $artifacts)
}
finally {
  if (Test-Path -LiteralPath $formDir) {
    Set-Content -LiteralPath (Join-Path $formDir 'stop.txt') -Value 'stop' -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 800
  }
  if ($formProc) { Stop-Process -Id $formProc.Id -Force -ErrorAction SilentlyContinue }
  if ($loopProc) { Stop-Process -Id $loopProc.Id -Force -ErrorAction SilentlyContinue }
  if ($formDir -and (Test-Path -LiteralPath $formDir)) { Remove-Item -LiteralPath $formDir -Recurse -Force -ErrorAction SilentlyContinue }
  Stop-ProbeEngine $e -KeepData:$KeepData
}
