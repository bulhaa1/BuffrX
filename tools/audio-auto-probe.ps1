# Hands-free proof of automatic app audio (audio_auto.h), on a throwaway engine and real Windows audio
# sessions. It never talks to the engine the person is using: BUFFRX_DATA_ROOT skips single-instance
# arbitration and BUFFRX_PIPE names a pipe of its own.
#
#   powershell -File tools/audio-auto-probe.ps1 -Engine build-auto\buffrx-engine.exe -Fixture build-auto\buffrx-audio-fixture.exe
#
# The fixture is copied to distinct executable names, so each copy is a distinct program to Windows. Three
# copies take the isolated lanes, a fourth takes Other audio, one leaves mid-recording and a fifth arrives,
# and the saved recording's sidecar must name both occupants of the lane it took without splitting the file.
param(
  [string]$Engine = 'runtime\bin\64bit\buffrx-engine.exe',
  [string]$Fixture = 'build-auto\buffrx-audio-fixture.exe',
  [string]$RuntimeRoot = 'runtime',
  [int]$TimeoutSeconds = 30,
  [switch]$Keep
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$engine = (Resolve-Path (Join-Path $repo $Engine)).Path
$fixture = (Resolve-Path (Join-Path $repo $Fixture)).Path
$runtime = (Resolve-Path (Join-Path $repo $RuntimeRoot)).Path
$root = Join-Path $env:TEMP ("buffrx-auto-probe-" + $PID)
$bin = Join-Path $root 'bins'
$media = Join-Path $root 'media'
$pipe = "buffrx-auto-probe-$PID"
New-Item -ItemType Directory -Force $bin, $media | Out-Null

$env:BUFFRX_DATA_ROOT = $root
$env:BUFFRX_PIPE = $pipe
$env:BUFFRX_RUNTIME_ROOT = $runtime
$env:PATH = (Join-Path $runtime 'bin\64bit') + ';' + $env:PATH

$failures = 0
function Check([bool]$ok, [string]$what) {
  if ($ok) { Write-Host "  ok   $what" }
  else { Write-Host "  FAIL $what"; $script:failures++ }
}

function Ask([string]$command, [string]$arguments = '{}') {
  $reply = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repo 'tools\engine-ask.ps1') $command $arguments
  return ($reply | ConvertFrom-Json)
}

$fixtures = @()
$engineProcess = $null
try {
  # Distinct binary names: the executable is the identity the policy groups by.
  $names = @('ProbeA.exe', 'ProbeB.exe', 'ProbeC.exe', 'ProbeD.exe', 'ProbeE.exe', 'ProbeF.exe')
  foreach ($name in $names) { Copy-Item $fixture (Join-Path $bin $name) -Force }

  $startFixture = {
    param($name)
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = (Join-Path $bin $name)
    $info.WorkingDirectory = $bin
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $process = [System.Diagnostics.Process]::Start($info)
    Start-Sleep -Milliseconds 700
    return $process
  }

  $engineInfo = New-Object System.Diagnostics.ProcessStartInfo
  $engineInfo.FileName = $engine
  $engineInfo.WorkingDirectory = Split-Path $engine
  $engineInfo.UseShellExecute = $false
  $engineInfo.CreateNoWindow = $true
  $engineProcess = [System.Diagnostics.Process]::Start($engineInfo)

  $ready = $false
  for ($i = 0; $i -lt $TimeoutSeconds * 4; $i++) {
    Start-Sleep -Milliseconds 250
    try {
      $status = Ask 'status.get'
      if ($status.ok) { $ready = $true; break }
    } catch { }
  }
  if (-not $ready) { throw 'the isolated engine did not answer its pipe' }

  # Keep every file inside the throwaway root. The JSON rides in a file: quotes in a command-line
  # argument do not survive Windows PowerShell.
  $pathPatch = @{ clips_dir = $media; screenshots_dir = (Join-Path $media 'Screenshots'); recordings_dir = $media } | ConvertTo-Json -Compress
  $patchFile = Join-Path $root 'patch.json'
  Set-Content -LiteralPath $patchFile -Value ('{"patch":' + $pathPatch + '}') -Encoding ascii
  $patched = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repo 'tools\engine-ask.ps1') 'config.set' -ArgumentsFile $patchFile | ConvertFrom-Json
  Check ([bool]$patched.ok) 'the throwaway engine keeps its files in the probe folder'

  # The probe's own programs ask for the isolated lanes, so another app already making sound on this
  # machine cannot take the lane the probe is watching. D is pinned too and takes A's freed lane. Every
  # program audible on this machine before the probe starts is excluded, so the lanes are free.
  $before = (Ask 'status.get').data
  $prefs = @{}
  foreach ($lane in @($before.audio.auto.lanes)) {
    foreach ($app in @($lane.apps)) {
      if ($app.exe -and $app.exe -notlike 'Probe*') { $prefs[$app.exe.ToLowerInvariant()] = @{ excluded = $true } }
    }
  }
  foreach ($name in @('probea.exe', 'probeb.exe', 'probec.exe', 'probed.exe', 'probee.exe')) {
    $prefs[$name] = @{ pinned = $true }
  }
  $prefsJson = $prefs | ConvertTo-Json -Compress
  $prefsFile = Join-Path $root 'prefs.json'
  Set-Content -LiteralPath $prefsFile -Value ('{"audio_app_prefs":' + $prefsJson + '}') -Encoding ascii
  $prefsReply = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repo 'tools\engine-ask.ps1') 'audio.set' -ArgumentsFile $prefsFile | ConvertFrom-Json
  Check ([bool]$prefsReply.ok) 'the probe programs are pinned to isolated lanes'

  $A = & $startFixture 'ProbeA.exe'; $fixtures += $A
  $B = & $startFixture 'ProbeB.exe'; $fixtures += $B
  $C = & $startFixture 'ProbeC.exe'; $fixtures += $C
  $D = & $startFixture 'ProbeD.exe'; $fixtures += $D
  Start-Sleep -Seconds 1

  $status = (Ask 'status.get').data
  $auto = $status.audio.auto
  Check ([bool]$auto.on) 'the first-run configuration is automatic capture'
  # Start the manual recording before the lane changes: the file must stay one file across them.
  $recording = Ask 'recording.start'
  Check ([bool]$recording.ok) 'a manual recording starts'
  Start-Sleep -Seconds 1
  $status = (Ask 'status.get').data
  Check ([bool]$status.audio.auto.watching) 'the session watcher runs while a recording wants sound'
  Start-Sleep -Seconds 2

  $A.StandardInput.WriteLine('quit'); $A.WaitForExit(3000) | Out-Null
  $fixtures = $fixtures | Where-Object { $_ -ne $A }
  Start-Sleep -Seconds 2
  $E = & $startFixture 'ProbeE.exe'; $fixtures += $E
  Start-Sleep -Seconds 2

  $status = (Ask 'status.get').data
  $lanes = @($status.audio.auto.lanes)
  Check ($lanes.Count -ge 3) 'the audible programs are on their own lanes'
  Check (($lanes | Where-Object { $_.other }).Count -le 1) 'at most one Other audio lane exists'
  $stopped = @($lanes | ForEach-Object { @($_.apps) } | Where-Object { $_.name -like '*ProbeA*' })
  Check ($stopped.Count -eq 0) 'a program that exited leaves the live lanes'

  $stop = Ask 'recording.stop'
  Check ([bool]$stop.ok) 'the manual recording stops'
  # The sidecar is written when the writer has let go of the file, a moment after the stop reply.
  for ($i = 0; $i -lt 40; $i++) {
    $files = @(Get-ChildItem -LiteralPath $media -Recurse -File -ErrorAction SilentlyContinue |
               Where-Object { $_.Name -notlike '*.buffrx-audio.json' })
    if ($files.Count -ge 1 -and (Test-Path -LiteralPath ($files[0].FullName + '.buffrx-audio.json'))) { break }
    Start-Sleep -Milliseconds 250
  }
  Check ($files.Count -eq 1) 'arrivals and departures leave the manual recording one file'
  if ($files.Count -ge 1) {
    $clip = $files[0].FullName
    $sidecar = $clip + '.buffrx-audio.json'
    Check (Test-Path -LiteralPath $sidecar) 'the recording has an audio sidecar'
    if (Test-Path -LiteralPath $sidecar) {
      $layout = Get-Content -LiteralPath $sidecar -Raw | ConvertFrom-Json
      Check (@($layout.tracks).Count -eq 6) 'the recording has six audio tracks'
      $tracks = @($layout.tracks)
      $mix = $tracks | Where-Object { $_.index -eq 1 }
      $lane = $tracks | Where-Object { $_.index -ge 3 -and $_.index -le 5 -and (@($_.sources) -contains 'auto:bus:3') }
      $occupied = $tracks | Where-Object { $_.index -ge 3 -and $_.index -le 5 -and $_.title -match 'ProbeA' }
      Check ($mix -and (@($mix[0].sources) -match '^auto:bus:')) 'the mix names the automatic buses'
      Check ($occupied -and $occupied[0].title -match 'ProbeA' -and $occupied[0].title -match 'ProbeD') `
        'the lane names both programs that occupied it (A + D)'
      Check ($lane -and (@($lane[0].sources) -contains 'auto:bus:3')) 'a lane keeps its stable bus id'
      $other = $tracks | Where-Object { $_.index -eq 6 }
      Check ($other -and $other[0].title -eq 'BuffrX Other audio') 'track 6 is Other audio'

      # The editor's own reading of the file and an export at changed lane levels: the complete layout
      # must let the export rebuild the mix, and muting every lane must make the measured RMS silence.
      $nativePreference = $ErrorActionPreference
      $ErrorActionPreference = 'Continue'
      Push-Location (Split-Path $engine)
      $info = (& $engine --media-info $clip | Select-Object -Last 1 | ConvertFrom-Json)
      Pop-Location
      Check ([bool]$info.audio_complete -and $info.audio_buffrx -and $info.audio.Count -eq 6) `
        'media-info reads a complete six-track automatic layout'

      $exported = Join-Path $media 'auto-export.mp4'
      $job = @{
        input = $clip; output = $exported; start = 0; end = [math]::Max(1, [math]::Floor([double]$info.duration));
        volumes = @(0, 0, 0, 0, 0, 0); quality = 70
      } | ConvertTo-Json -Compress
      $jobFile = Join-Path $root 'export.json'
      Set-Content -LiteralPath $jobFile -Value $job -Encoding ascii
      Push-Location (Split-Path $engine)
      $exportReply = (& $engine --media-export $jobFile 2>&1 | Select-Object -Last 1)
      Pop-Location
      Check ($exportReply -match '^EXPORT_OK') 'the export accepts the automatic layout with changed lane levels'
      if (Test-Path -LiteralPath $exported) {
        Push-Location (Split-Path $engine)
        $exportInfo = (& $engine --media-info $exported | Select-Object -Last 1 | ConvertFrom-Json)
        $loud = (& $engine --media-loudness $exported | Select-Object -Last 1 | ConvertFrom-Json)
        Pop-Location
        Check ([bool]$exportInfo.audio_complete) 'the exported file keeps a complete layout'
        Check (($loud.levels | Measure-Object -Maximum).Maximum -le 1) `
          'every lane at zero measured silence in the exported mix'
      }
      $ErrorActionPreference = $nativePreference
    }
  }

  # A ring that is already running follows an arrival without a rebuild: its counters keep climbing and
  # a clip saved after it carries the same complete automatic layout.
  $started = Ask 'buffer.start'
  Check ([bool]$started.ok) 'the buffer starts with automatic audio'
  Start-Sleep -Seconds 2
  $ringBefore = (Ask 'status.get').data.stats.ring
  $F = & $startFixture 'ProbeF.exe'; $fixtures += $F
  Start-Sleep -Seconds 2
  $during = (Ask 'status.get').data
  Check ([bool]$during.buffering -and -not [bool]$during.rebuilding -and
         [int64]$during.stats.ring.received_bytes -gt [int64]$ringBefore.received_bytes) `
    'a program arriving leaves the running ring untouched and its counters climbing'
  $saveFile = Join-Path $root 'save.json'
  Set-Content -LiteralPath $saveFile -Value '{"seconds":5}' -Encoding ascii
  $saved = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repo 'tools\engine-ask.ps1') 'buffer.save' -ArgumentsFile $saveFile | ConvertFrom-Json
  Check ([bool]$saved.ok) 'a clip saves from the ring while automatic lanes are live'
  for ($i = 0; $i -lt 40; $i++) {
    $clips = @(Get-ChildItem -LiteralPath $media -Recurse -File -ErrorAction SilentlyContinue |
               Where-Object { $_.Name -notlike '*.buffrx-audio.json' -and $_.Name -notlike 'auto-export*' })
    if ($clips.Count -ge 2) { break }
    Start-Sleep -Milliseconds 250
  }
  $ringClip = $clips | Where-Object { $_.Name -notlike 'auto-export*' } |
              Sort-Object LastWriteTime -Descending | Select-Object -First 1
  for ($i = 0; $i -lt 40; $i++) {
    $ringClip = $clips | Where-Object { $_.Name -notlike 'auto-export*' -and
                                        (Test-Path -LiteralPath ($_.FullName + '.buffrx-audio.json')) } |
                Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($ringClip) { break }
    Start-Sleep -Milliseconds 250
  }
  Check ($ringClip -and (Test-Path -LiteralPath ($ringClip.FullName + '.buffrx-audio.json'))) `
    'the saved clip carries its automatic layout'

  # Stop everything: the watcher must not keep a thread or a device open with nothing capturing.
  Ask 'buffer.stop' | Out-Null
  Ask 'recording.stop' | Out-Null
  Start-Sleep -Seconds 2
  $status = (Ask 'status.get').data
  Check (-not [bool]$status.audio.auto.watching) 'the watcher stops when nothing wants sound'
} finally {
  foreach ($process in $fixtures) {
    try { $process.StandardInput.WriteLine('quit'); $process.WaitForExit(2000) | Out-Null } catch { }
    try { if (-not $process.HasExited) { $process.Kill() } } catch { }
  }
  if ($engineProcess) {
    try { if (-not $engineProcess.HasExited) { $engineProcess.Kill(); $engineProcess.WaitForExit(5000) | Out-Null } } catch { }
  }
  if (-not $Keep) { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}

if ($failures -gt 0) {
  Write-Host "audio auto probe: $failures check(s) failed"
  exit 1
}
Write-Host 'audio auto probe: all checks passed'
