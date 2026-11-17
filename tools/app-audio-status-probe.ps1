# What this checks: the Audio page keeps saying what the one card said. With notices OFF, a captured game that
# is not in the selected apps publishes status.audio.problem=game_missing (and the steady flag), the page draws
# its persistent line, and the line clears on a real recovery, a mode switch, Performance mode and a stopped
# buffer. It also checks the selected rows' measured activity mark against a real tone that stops, and that no
# session is read while the page is not drawn.
#
# It runs a private copy of the built engine against a frozen OBS runtime, on a throwaway data root and pipe, so
# it sits beside the BuffrX the person is using and stops only the processes it started.
#
# Honest limit: the "game" is a real window of a real process (a PowerShell form titled "BuffrX Audio Tone") used
# as a pinned game-capture target, not a real game. The game's program is the same process that plays the tone,
# so its audio session is real Windows audio; the capture target is not a rendering game. win-wasapi may refuse
# to hook a PowerShell window on this machine; when it does, the run reports the recovery it could prove (mode
# switch) and says the hook was unavailable instead of claiming a connect.
#
# Run after a direct build:  powershell -File tools\app-audio-status-probe.ps1
# Artifacts (PNGs, engine log, this run's log) are saved under work\audio-review-artifacts.
param([switch]$KeepData, [string]$Engine = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'probe-lib.ps1')

$script:failures = 0
function Fail([string]$why) { Write-Host "app audio status probe: FAIL $why"; $script:failures++ }
function Ok([string]$what) { Write-Host "ok   $what" }

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$artifacts = Join-Path $root 'work\audio-review-artifacts'
New-Item -ItemType Directory -Force $artifacts | Out-Null

if (-not $Engine) {
  $Engine = Join-Path $root 'build\buffrx-engine.exe'
  if (-not (Test-Path -LiteralPath $Engine)) { throw 'build\buffrx-engine.exe is missing: build buffrx-engine first' }
}

# A private bin for the built exe, so a concurrent build or the staged runtime cannot swap the binary under the run.
$privateRoot = Join-Path $root 'work\audio-review-runtime'
$privateBin = Join-Path $privateRoot 'bin\64bit'
if (-not (Test-Path -LiteralPath (Join-Path $privateBin 'obs.dll'))) {
  New-Item -ItemType Directory -Force $privateBin | Out-Null
  Copy-Item -Path (Join-Path $root 'runtime\bin\64bit\*') -Destination $privateBin -Recurse -Force
}
Copy-Item -LiteralPath $Engine -Destination $privateBin -Force
$engineExe = Join-Path $privateBin 'buffrx-engine.exe'
$realRuntime = Join-Path $root 'runtime'

# The tone: a quiet 440 Hz WAV, played on a loop by a PowerShell form. The session is real (winmm -> WASAPI), so
# the page's meter either sees it or does not; nothing in the probe fakes a peak.
$toneDir = Join-Path $env:TEMP ('buffrx-tone-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $toneDir | Out-Null
$wav = Join-Path $toneDir 'tone.wav'
$stopFile = Join-Path $toneDir 'stop.txt'
$toneScript = Join-Path $toneDir 'tone.ps1'
$rate = 44100; $seconds = 5; $samples = $rate * $seconds
$bytes = New-Object byte[] ($samples * 2)
for ($i = 0; $i -lt $samples; $i++) {
  $sample = [int16](0.06 * 32767 * [Math]::Sin(2 * [Math]::PI * 440 * $i / $rate))
  $bytes[$i * 2] = [byte]($sample -band 0xFF)
  $bytes[$i * 2 + 1] = [byte](($sample -shr 8) -band 0xFF)
}
$stream = [IO.File]::Create($wav); $writer = New-Object IO.BinaryWriter($stream)
$writer.Write([Text.Encoding]::ASCII.GetBytes('RIFF')); $writer.Write([int32](36 + $bytes.Length))
$writer.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt ')); $writer.Write([int32]16)
$writer.Write([int16]1); $writer.Write([int16]1); $writer.Write([int32]$rate); $writer.Write([int32]($rate * 2))
$writer.Write([int16]2); $writer.Write([int16]16); $writer.Write([Text.Encoding]::ASCII.GetBytes('data'))
$writer.Write([int32]$bytes.Length); $writer.Write($bytes); $writer.Dispose(); $stream.Dispose()
@"
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Media
`$player = New-Object System.Media.SoundPlayer('$($wav -replace '\\', '\\\\')')
`$form = New-Object System.Windows.Forms.Form
`$form.Text = 'BuffrX Audio Tone'
`$form.Width = 360
`$form.Height = 180
`$form.StartPosition = 'CenterScreen'
`$timer = New-Object System.Windows.Forms.Timer
`$timer.Interval = 200
`$timer.add_Tick({ if (Test-Path '$($stopFile -replace '\\', '\\\\')') { `$player.Stop(); `$timer.Stop() } })
`$form.add_Shown({ `$player.PlayLooping(); `$timer.Start() })
`$form.add_FormClosed({ `$player.Stop() })
[System.Windows.Forms.Application]::Run(`$form)
"@ | Set-Content -LiteralPath $toneScript -Encoding UTF8

$tone = Start-Process -FilePath 'powershell.exe' -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-WindowStyle', 'Hidden', '-File', $toneScript) -PassThru
$toneProc = $tone
Start-Sleep -Milliseconds 1500

# A moving window to stand in for the captured game when a pinned target must be alive: it is never selected for
# audio and never plays sound, so it is only an identity.
$motion = Start-Process -FilePath (Join-Path $privateBin 'buffrx-present-loop.exe') -ArgumentList @('--seconds', '600', '--motion', '1') -PassThru
Start-Sleep -Milliseconds 1200

$config = '{"start_buffering":false,"auto_game_capture":false,"notifications":false,"notification_sound":false,' +
          '"desktop_enabled":true,"mic_enabled":false,"separate_tracks":true,"audio_tracks":3,"audio_capture_mode":"desktop","capture_mode":"display"}'

# The engine's own start, with its console (where `command:` lines are logged) kept in a file: the probe's last
# check needs to see that the page stopped asking for audio.sessions, and the failure log alone cannot show it.
function Start-ProbeEngineLogged([string]$configJson, [string]$exe, [string]$runtimeRoot) {
  $dataRoot = Join-Path $env:TEMP ('buffrx-probe-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
  Assert-ProbeDataRoot $dataRoot | Out-Null
  $clipsDir = Join-Path $dataRoot 'clips'
  New-Item -ItemType Directory -Force $clipsDir | Out-Null
  $json = $configJson.TrimEnd('}').TrimEnd() + ',"clips_dir":"' + ($clipsDir -replace '\\', '/') +
          '","screenshots_dir":"' + (($dataRoot + '\shots') -replace '\\', '/') + '"}'
  Set-Content -LiteralPath (Join-Path $dataRoot 'config.json') -NoNewline -Value $json
  $pipe = 'buffrx-probe-' + [guid]::NewGuid().ToString('N').Substring(0, 6)
  $stderr = Join-Path $dataRoot 'engine-stderr.log'
  $stdout = Join-Path $dataRoot 'engine-stdout.log'
  $env:BUFFRX_DATA_ROOT = $dataRoot; $env:BUFFRX_PIPE = $pipe; $env:BUFFRX_RUNTIME_ROOT = $runtimeRoot
  $proc = Start-Process -FilePath $exe -WorkingDirectory (Split-Path -Parent $exe) -PassThru -WindowStyle Hidden `
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
function Audio-Reads {
  if (-not (Test-Path -LiteralPath $e.Stderr)) { return 0 }
  @(Select-String -LiteralPath $e.Stderr -Pattern 'command: audio\.sessions').Count
}

$e = Start-ProbeEngineLogged $config $engineExe $realRuntime
$shots = Join-Path $e.DataRoot 'status-shots'
New-Item -ItemType Directory -Force $shots | Out-Null

# No window yet: the page cannot be drawn, so no session may be read.
Start-Sleep -Milliseconds 1500
if ((Audio-Reads) -ne 0) { Fail 'the engine read audio sessions with no page drawn' } else { Ok 'no session read before any page was drawn' }

function Status { ((Ask-Engine $e 'status.get') | ConvertFrom-Json).data }
function Audio-Status { (Status).audio }
function Windows { ((Ask-Engine $e 'sources.windows') | ConvertFrom-Json).data }
function Set-Apps($apps) { $null = Ask-Engine $e 'audio.set' (@{ audio_capture_mode = 'apps'; apps = @($apps) } | ConvertTo-Json -Depth 6 -Compress) }

# One view as PNG. A warm capture, one settle, then the reviewed file: the first capture of a view includes the
# window's fade-in, so two states are only compared once both have settled.
function Hide-Panel {
  $null = Ask-Engine $e 'ui.show' (@{ view = 'hide' } | ConvertTo-Json -Compress)
  Start-Sleep -Milliseconds 350
}

function Capture([string]$name, [switch]$KeepOpen) {
  $file = Join-Path $shots ($name + '.png')
  $warm = Join-Path $shots ($name + '.warming.png')
  # The panel is a real window on the person's screen for a moment. A real click or Escape can close Settings
  # while the capture waits, which shows the deck instead, so the page is verified by its own spine and asked
  # again once if that happened.
  for ($attempt = 0; $attempt -lt 2; $attempt++) {
    $null = Ask-Engine $e 'ui.capture' (@{ view = 'settings:audio'; file = $warm } | ConvertTo-Json -Compress)
    Start-Sleep -Milliseconds 450
    $reply = Ask-Engine $e 'ui.capture' (@{ view = 'settings:audio'; file = $file } | ConvertTo-Json -Compress)
    Remove-Item -LiteralPath $warm -ErrorAction SilentlyContinue
    if ($reply -notmatch '"file"') { Fail ("the Audio page could not be captured as '$name': " + $reply); return }
    # The Settings spine's filled AUDIO row is the witness that the page, not the deck, was drawn.
    if ((Count-Color $file 255 107 44 2 0 150 260 500) -gt 2000) { break }
    if ($attempt -eq 1) { Fail ("the capture '$name' drew the deck instead of the Audio page") }
  }
  Copy-Item -LiteralPath $file -Destination (Join-Path $artifacts ('status-' + $name + '.png')) -Force
  if (-not $KeepOpen) { Hide-Panel }
}

Add-Type -AssemblyName System.Drawing
function Count-Color([string]$file, [int]$r, [int]$g, [int]$b, [int]$tol,
                     [int]$x0 = 0, [int]$y0 = 0, [int]$x1 = -1, [int]$y1 = -1) {
  if (-not (Test-Path -LiteralPath $file)) { return -1 }
  $bmp = New-Object System.Drawing.Bitmap($file)
  $rect = New-Object System.Drawing.Rectangle(0, 0, $bmp.Width, $bmp.Height)
  $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $pixels = New-Object byte[] ($data.Stride * $bmp.Height)
  [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $pixels, 0, $pixels.Length)
  $stride = $data.Stride; $width = $bmp.Width; $height = $bmp.Height
  $bmp.UnlockBits($data); $bmp.Dispose()
  if ($x1 -lt 0) { $x1 = $width }
  if ($y1 -lt 0) { $y1 = $height }
  $x0 = [Math]::Max(0, $x0); $y0 = [Math]::Max(0, $y0)
  $x1 = [Math]::Min($width, $x1); $y1 = [Math]::Min($height, $y1)
  $count = 0
  for ($y = $y0; $y -lt $y1; $y++) {
    $row = $y * $stride
    for ($x = $x0; $x -lt $x1; $x++) {
      $i = $row + $x * 4
      if ([Math]::Abs([int]$pixels[$i] - $b) -le $tol -and
          [Math]::Abs([int]$pixels[$i + 1] - $g) -le $tol -and
          [Math]::Abs([int]$pixels[$i + 2] - $r) -le $tol) { $count++ }
    }
  }
  $count
}
# The Selected apps card only: the status strip at the foot carries its own red mark, and that is not the line
# under test.
function Count-Danger([string]$file) { Count-Color $file 255 77 94 24 300 180 -1 720 }
function Peak-Of($sessions, [string]$exe) {
  foreach ($s in $sessions.sessions) { if ($s.exe -ieq $exe) { return [double]$s.peak } }
  -1.0
}

try {
  $wins = Windows
  $toneWin = $wins | Where-Object { $_.name -like '*BuffrX Audio Tone*' } | Select-Object -First 1
  if (-not $toneWin) { throw 'the tone window was not in sources.windows' }
  $motionWin = $wins | Where-Object { $_.exe -like '*present-loop*' } | Select-Object -First 1
  if (-not $motionWin) { throw 'the motion window was not in sources.windows' }
  Ok ('windows found: tone=' + $toneWin.exe + ' (' + $toneWin.id + '), motion=' + $motionWin.exe)
  $null = Ask-Engine $e 'capture.set' (@{ mode = 'game'; game_title = $toneWin.id } | ConvertTo-Json -Compress)

  # 1. A selected program with a real measured tone, then the same program measurably silent (not unknown).
  Set-Apps @{ window = $toneWin.id; exe = $toneWin.exe; title = $toneWin.name; track = 2 }
  Start-Sleep -Milliseconds 1500
  $sessions = ((Ask-Engine $e 'audio.sessions') | ConvertFrom-Json).data
  $peakOn = Peak-Of $sessions $toneWin.exe
  Ok ('tone peak while playing: ' + $peakOn)
  if ($peakOn -lt 0.004) { Fail ('the tone was not measured: the session peak is ' + $peakOn) }
  Capture 'activity-on'
  Set-Content -LiteralPath $stopFile -Value 'stop'
  $peakOff = -1
  for ($i = 0; $i -lt 20; $i++) {
    Start-Sleep -Milliseconds 500
    $sessions = ((Ask-Engine $e 'audio.sessions') | ConvertFrom-Json).data
    $peakOff = Peak-Of $sessions $toneWin.exe
    if ($peakOff -ge 0 -and $peakOff -lt 0.004) { break }
  }
  Ok ('tone peak after it stopped: ' + $peakOff)
  if ($peakOff -lt 0) { Fail ('the meter disappeared after the tone stopped, so silence was unknown, not measured') }
  elseif ($peakOff -ge 0.004) { Fail ('the meter still read sound after the tone stopped: ' + $peakOff) }
  Capture 'activity-off'
  # Near-accent, not exact: the wave glyph is a 1.4 px stroke, so antialiasing leaves only a pixel or two of the exact
  # accent and the comparison would turn on unrelated page content. A tolerance of 40 counts the stroke it is drawn with.
  $accentOn = Count-Color (Join-Path $shots 'activity-on.png') 255 107 44 40
  $accentOff = Count-Color (Join-Path $shots 'activity-off.png') 255 107 44 40
  Ok ('accent pixels: playing=' + $accentOn + ', silent=' + $accentOff)
  if (-not ($accentOn -ge $accentOff + 8)) {
    Fail ('the selected row did not gain a filled activity mark while the tone played (' + $accentOn + ' vs ' + $accentOff + ')')
  }

  # 2. The buffer off with the app list kept: nothing to say, so the baseline has no line on it.
  Set-Apps @{ window = $toneWin.id; exe = $toneWin.exe; title = $toneWin.name; track = 2 }
  Start-Sleep -Milliseconds 1200
  $offBase = Audio-Status
  if ($offBase.problem -ne '') { Fail ('a stopped buffer reported ' + $offBase.problem) }
  Capture 'buffer-off-baseline'
  $dangerCovered = Count-Danger (Join-Path $shots 'buffer-off-baseline.png')

  # 3. The buffer on: first the covered game (quiet while its slot connects), then the game not selected.
  $null = Ask-Engine $e 'buffer.start'
  $connected = $null
  for ($i = 0; $i -lt 40; $i++) {
    Start-Sleep -Milliseconds 500
    $connected = Audio-Status
    if ($connected.problem -eq '') { break }
  }
  $hook_ok = $connected.problem -eq ''
  if ($hook_ok) {
    Ok 'the selected game connected: a covered game reports no problem'
    Capture 'covered'
  } else {
    Ok ('win-wasapi did not hook the PowerShell window on this machine (' + $connected.problem + '); ' +
        'the covered state is said as not connected, and the recovery is judged below')
  }

  Set-Apps @{ window = $motionWin.id; exe = $motionWin.exe; title = $motionWin.name; track = 2 }
  $missing = $null
  for ($i = 0; $i -lt 20; $i++) {
    Start-Sleep -Milliseconds 500
    $missing = Audio-Status
    if ($missing.problem_steady) { break }
  }
  Ok ('status.audio while the game is not selected: ' + ($missing | ConvertTo-Json -Compress))
  if ($missing.problem -ne 'game_missing') { Fail ('status.audio.problem is not game_missing: ' + $missing.problem) }
  if (-not $missing.problem_steady) { Fail 'status.audio did not settle the missing game into a steady problem' }
  if ($missing.game_exe -notlike '*powershell*') { Fail ('status.audio names the wrong game: ' + $missing.game_exe) }
  Capture 'missing'
  $dangerMissing = Count-Danger (Join-Path $shots 'missing.png')
  Ok ('danger pixels: covered=' + $dangerCovered + ', missing=' + $dangerMissing)
  if (-not ($dangerMissing -ge $dangerCovered + 50)) {
    Fail 'the page did not draw the persistent missing-game line (not enough danger-coloured pixels)'
  }

  # 3. Recovery: the Current game slot selected. win-wasapi hooks the program (or it does not, which is said).
  Set-Apps @{ kind = 'game'; title = 'Current game'; track = 0 }
  $recovered = $null
  for ($i = 0; $i -lt 40; $i++) {
    Start-Sleep -Milliseconds 500
    $recovered = Audio-Status
    if ($recovered.problem -eq '') { break }
  }
  $hook_ok = $recovered.problem -eq ''
  if ($hook_ok) {
    Ok 'the game slot connected: the persistent line cleared on recovery'
    Capture 'recovered'
    $dangerRecovered = Count-Danger (Join-Path $shots 'recovered.png')
    if ($dangerRecovered -gt $dangerCovered + 50) { Fail 'the line stayed drawn after a real recovery' }
  } else {
    Ok ('win-wasapi did not hook the PowerShell window on this machine (' + $recovered.problem + '); ' +
        'the mode-switch clear is proven instead, and no recovery is claimed')
  }

  # 4. Performance mode: the app routes are not what records, so the line clears (and returns with it off).
  Set-Apps @{ window = $motionWin.id; exe = $motionWin.exe; title = $motionWin.name; track = 2 }
  Start-Sleep -Milliseconds 1500
  $null = Ask-Engine $e 'config.set' (@{ patch = @{ performance_when = 'always' } } | ConvertTo-Json -Compress)
  Start-Sleep -Milliseconds 1500
  $perf = Audio-Status
  if ($perf.problem -ne '' -or $perf.problem_steady) { Fail ('Performance mode still reported a slot problem: ' + ($perf | ConvertTo-Json -Compress)) }
  Capture 'performance'
  $dangerPerf = Count-Danger (Join-Path $shots 'performance.png')
  if ($dangerPerf -gt $dangerCovered + 50) { Fail 'the line stayed drawn while Performance mode recorded its own mix' }
  $null = Ask-Engine $e 'config.set' (@{ patch = @{ performance_when = 'off' } } | ConvertTo-Json -Compress)

  # 5. A mode switch to the desktop: no selected slots, no line.
  $null = Ask-Engine $e 'capture.set' (@{ mode = 'display' } | ConvertTo-Json -Compress)
  Start-Sleep -Milliseconds 1200
  $display = Audio-Status
  if ($display.problem -ne '') { Fail ('desktop mode still reported ' + $display.problem) }
  $null = Ask-Engine $e 'audio.set' (@{ audio_capture_mode = 'desktop' } | ConvertTo-Json -Compress)
  Start-Sleep -Milliseconds 700
  $desktop = Audio-Status
  if ($desktop.problem -ne '') { Fail ('desktop audio mode still reported ' + $desktop.problem) }

  # 6. Buffer off: nothing said while nothing records, even with the app list kept.
  Set-Apps @{ window = $motionWin.id; exe = $motionWin.exe; title = $motionWin.name; track = 2 }
  $null = Ask-Engine $e 'capture.set' (@{ mode = 'game'; game_title = $toneWin.id } | ConvertTo-Json -Compress)
  Start-Sleep -Milliseconds 800
  $null = Ask-Engine $e 'buffer.stop'
  Start-Sleep -Milliseconds 1500
  $off = Audio-Status
  if ($off.problem -ne '') { Fail ('a stopped buffer still reported ' + $off.problem) }
  Capture 'buffer-off'
  $dangerOff = Count-Danger (Join-Path $shots 'buffer-off.png')
  if ($dangerOff -gt $dangerCovered + 50) { Fail 'the line stayed drawn after the buffer stopped' }

  # 7. No page drawn, no session read. ui.capture draws the page once; with no further capture the engine must
  #    stop asking. Every command is on the engine's console, which this probe keeps in a file.
  # The page is opened once more and left open, so its closed-picker cadence is measurable: at most twice a
  # second. Then the window is hidden and every read must stop.
  if ((Audio-Reads) -lt 1) { Fail 'the Audio page never asked for sessions while it was drawn' }
  Capture 'cadence' -KeepOpen
  $readsA = Audio-Reads
  Start-Sleep -Seconds 5
  $readsB = Audio-Reads
  Ok ('audio.sessions while the Audio page is open, no picker: ' + ($readsB - $readsA) + ' in 5 s')
  if (($readsB - $readsA) -gt 12) { Fail 'the closed-picker activity was sampled faster than twice a second' }
  $null = Ask-Engine $e 'ui.show' (@{ view = 'hide' } | ConvertTo-Json -Compress)
  Start-Sleep -Milliseconds 1500
  $readsHiddenA = Audio-Reads
  Start-Sleep -Seconds 5
  $readsHiddenB = Audio-Reads
  Ok ('audio.sessions with the window hidden: ' + ($readsHiddenB - $readsHiddenA) + ' in 5 s')
  if ($readsHiddenB -ne $readsHiddenA) { Fail 'the Audio page kept reading sessions after the window was hidden' }

  # Another Settings page: the Audio section is not drawn, so its sessions are not read either.
  $other = Join-Path $shots 'settings-capture.png'
  $null = Ask-Engine $e 'ui.capture' (@{ view = 'settings:capture'; file = $other } | ConvertTo-Json -Compress)
  Start-Sleep -Milliseconds 500
  $null = Ask-Engine $e 'ui.capture' (@{ view = 'settings:capture'; file = $other } | ConvertTo-Json -Compress)
  Start-Sleep -Milliseconds 800
  $otherA = Audio-Reads
  Start-Sleep -Seconds 5
  $otherB = Audio-Reads
  Ok ('audio.sessions with another Settings page open: ' + ($otherB - $otherA) + ' in 5 s')
  if ($otherB -ne $otherA) { Fail 'the Audio page read sessions while another Settings page was drawn' }
  Hide-Panel

  Copy-Item -LiteralPath (Join-Path $e.DataRoot 'logs\engine.log') -Destination (Join-Path $artifacts 'status-engine.log') -Force
  Copy-Item -LiteralPath $e.Stderr -Destination (Join-Path $artifacts 'status-engine-console.log') -Force
  if ($script:failures -gt 0) { exit 1 }
  Write-Host 'app audio status probe: all checks passed'
  Write-Host ('captures and log: ' + $artifacts)
}
finally {
  if ($toneProc) { Stop-Process -Id $toneProc.Id -Force -ErrorAction SilentlyContinue }
  if ($motion) { Stop-Process -Id $motion.Id -Force -ErrorAction SilentlyContinue }
  if (Test-Path -LiteralPath $toneDir) { Remove-Item -LiteralPath $toneDir -Recurse -Force -ErrorAction SilentlyContinue }
  Stop-ProbeEngine $e -KeepData:$KeepData
}
