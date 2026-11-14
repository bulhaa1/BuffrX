# What this checks: the native Settings > Audio page draws in every recording mode, and the mode and saved
# routes are still what was chosen after each switch. It starts an engine of its own on a throwaway data root
# and pipe (probe-lib.ps1), so it runs beside the BuffrX the person is using, and it stops only that process.
#
# Run after the UI is staged:  powershell -File tools\audio-settings-probe.ps1 -KeepData
# The four PNGs (desktop, apps, empty, performance) are left in <data root>\audio-settings for review.
param([switch]$KeepData, [string]$Engine = 'buffrx-engine.exe')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'probe-lib.ps1')

$script:failures = 0
function Fail([string]$why) {
  Write-Host "audio settings probe: FAIL $why"
  $script:failures++
}

$config = '{"start_buffering":false,"auto_game_capture":false,"notifications":false,"mic_enabled":true,' +
          '"desktop_enabled":true,"separate_tracks":true,"audio_tracks":3,"audio_capture_mode":"desktop"}'
$e = Start-ProbeEngine $config $Engine
try {
  $shots = Join-Path $e.DataRoot 'audio-settings'
  New-Item -ItemType Directory -Force $shots | Out-Null
  $hashes = [ordered]@{}

  # One capture of the page as it stands. The engine draws the view itself, so nothing depends on a pointer
  # or on the panel already being open. The first capture of a view arrives while the window fades in and its
  # content loads, so every state is captured the same way: a warm capture, one bounded settle, then the file
  # that is reviewed. A difference between two reviewed PNGs is then the state, not an animation.
  function Capture([string]$name) {
    $file = Join-Path $shots ($name + '.png')
    $warm = Join-Path $shots ($name + '.warming.png')
    $null = Ask-Engine $e 'ui.capture' (@{ view = 'settings:audio'; file = $warm } | ConvertTo-Json -Compress)
    Start-Sleep -Milliseconds 400
    $reply = Ask-Engine $e 'ui.capture' (@{ view = 'settings:audio'; file = $file } | ConvertTo-Json -Compress)
    Remove-Item -LiteralPath $warm -ErrorAction SilentlyContinue
    if ($reply -notmatch '"file"') { Fail ("the Audio page could not be captured in state '" + $name + "': " + $reply); return }
    if (-not (Test-Path -LiteralPath $file) -or (Get-Item -LiteralPath $file).Length -lt 2000) {
      Fail ("the capture for state '" + $name + "' is missing or empty"); return
    }
    $hashes[$name] = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
  }

  $routes = @(
    @{ window = 'test:Discord.exe'; exe = 'Discord.exe'; title = 'Discord'; track = 3 },
    @{ window = 'test:cs2.exe'; exe = 'cs2.exe'; title = 'Counter-Strike 2'; track = 4 }
  )
  function Set-Audio([hashtable]$audio) { Ask-Engine $e 'audio.set' ($audio | ConvertTo-Json -Depth 5 -Compress) }
  function Audio-Get { ((Ask-Engine $e 'audio.get') | ConvertFrom-Json).data }

  # 1. Desktop audio (the default): the output device and the microphone.
  Capture 'desktop'
  $audio = Audio-Get
  if ($audio.audio_capture_mode -ne 'desktop') { Fail ('the default mode is not desktop: ' + $audio.audio_capture_mode) }

  # 2. Selected apps with two routes, each on its own track.
  $null = Set-Audio @{ audio_capture_mode = 'apps'; apps = $routes }
  Capture 'apps'
  $audio = Audio-Get
  if ($audio.audio_capture_mode -ne 'apps' -or $audio.apps.Count -ne 2) {
    Fail ('two app routes did not stay two routes in apps mode: ' + (($audio | ConvertTo-Json -Depth 4 -Compress)))
  }

  # 3. Selected apps with nothing chosen: no fallback to the desktop, only the microphone.
  $null = Set-Audio @{ apps = @() }
  Capture 'empty'
  $audio = Audio-Get
  if ($audio.audio_capture_mode -ne 'apps' -or $audio.apps.Count -ne 0) {
    Fail ('an empty app list changed the mode or kept routes: ' + (($audio | ConvertTo-Json -Depth 4 -Compress)))
  }

  # 4. Performance mode: one mixed desktop track, the saved apps left alone. It is active from the stored
  #    choice alone, so the captured page is the Performance state; the stored choice must not move.
  $null = Set-Audio @{ apps = $routes }
  $null = Ask-Engine $e 'config.set' (@{ patch = @{ performance_when = 'always' } } | ConvertTo-Json -Depth 4 -Compress)
  $status = ((Ask-Engine $e 'status.get') | ConvertFrom-Json).data
  if (-not $status.performance.active) {
    Fail ('Performance mode did not become active, so the page captured next is not the Performance state')
  }
  Capture 'performance'
  $audio = Audio-Get
  if ($audio.audio_capture_mode -ne 'apps' -or $audio.apps.Count -ne 2) {
    Fail ('Performance mode rewrote the stored audio mode or routes: ' + (($audio | ConvertTo-Json -Depth 4 -Compress)))
  }
  $null = Ask-Engine $e 'config.set' (@{ patch = @{ performance_when = 'off' } } | ConvertTo-Json -Depth 4 -Compress)

  # The four states are four different pages: a capture identical to another would mean the page did not follow
  # the mode.
  $distinct = @($hashes.Values | Select-Object -Unique).Count
  if ($hashes.Count -ne 4 -or $distinct -ne 4) {
    Fail ('the Audio page drew the same picture in different modes (' + $distinct + ' of ' + $hashes.Count + ' distinct)')
  }

  if ($script:failures -gt 0) { exit 1 }
  Write-Host 'audio settings probe: all checks passed'
  Write-Host ('captures: ' + $shots)
}
finally {
  # Its own data root and pipe are removed unless the captures are asked for; only this process is stopped.
  Stop-ProbeEngine $e -KeepData:$KeepData
}
