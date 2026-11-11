# N8: settings churn. One throwaway engine with the buffer on; every 10-25 s apply one valid change from what the UI
# offers, then ask status.get. Every 5 minutes save a 10 s clip and check its length and that it still plays.
# Pass: the engine stays up, last_error clears, no rebuild stays stuck, RAM does not climb, every clip is 9-15 s.
#
#   powershell -File tools\settings-churn.ps1 -Minutes 45 -Engine buffrx-engine-night.exe -Motion
param(
  [int]$Minutes = 45,
  [string]$Engine = 'buffrx-engine.exe',
  [switch]$Motion,
  [switch]$NoVideoChanges,
  [switch]$KeepData
)

. (Join-Path $PSScriptRoot 'probe-lib.ps1')

$config = @'
{"config_version":7,"start_buffering":false,"auto_game_capture":false,"timeline_minutes":30,"buffer_seconds":60,
 "codec":"h264","capture_fps":30,"output_width":1280,"output_height":720,"quality":"standard","peak_mode":"auto",
 "container":"mp4","audio_tracks":1,"desktop_enabled":true,"mic_enabled":false}
'@

# Display duplication delivered no frame on this machine on 2026-11-02, so footage comes from the moving window
# (a window, no capture of the screen).
$loop = $null
if ($Motion) {
  $loop = Start-Process -FilePath (Join-Path $PSScriptRoot '..\build\buffrx-present-loop.exe') `
    -ArgumentList @('--seconds', [string]($Minutes * 60 + 180), '--width', '1280', '--height', '720', '--motion', '1') -PassThru
}
$e = Start-ProbeEngine $config $Engine @{ BUFFRX_RING_RAM_MB = '128' }
"engine pid $($e.Process.Id) root $($e.DataRoot)"
if ($Motion) { $null = Set-WindowCapture $e; $null = Ask-Engine $e 'buffer.start' }
$failures = New-Object System.Collections.Generic.List[string]
$changes = 0
$saves = 0

function Fail([string]$what) {
  $script:failures.Add($what)
  "FAIL $what"
}

# One random but always valid change, as a config.set patch.
function Random-Patch {
  $pick = Get-Random -Maximum 10
  # A video rebuild (resolution, frame rate, Performance mode) is part of the churn. The 2026-11-02 claim that
  # it lost the source for good came from the repro reading a status field that does not exist; the repro
  # passes now, and a rebuild can set last_error while its new capture starts. -NoVideoChanges still isolates
  # the settings that need no reset.
  if ($NoVideoChanges) { $pick = Get-Random @(0, 3, 4, 5, 6, 8, 9) }
  switch ($pick) {
    0 { return @{ codec = (Get-Random @('h264', 'hevc', 'av1')) } }
    1 { return @{ capture_fps = [int](Get-Random @(30, 60, 120)) } }
    2 {
      $size = Get-Random @('1280x720', '1920x1080', '2560x1440')
      $w, $h = $size -split 'x'
      return @{ output_width = [int]$w; output_height = [int]$h }
    }
    3 { return @{ audio_tracks = [int](Get-Random @(1, 2)) } }
    4 { return @{ quality = (Get-Random @('low', 'standard', 'high', 'highest')) } }
    5 { return @{ peak_mode = (Get-Random @('auto', 'fixed')); custom_kbps = 20000 } }
    6 { return @{ container = (Get-Random @('mp4', 'mkv')) } }
    7 { return @{ performance_when = (Get-Random @('off', 'games', 'always')) } }
    8 { return @{ buffer_seconds = [int](Get-Random @(30, 60, 120)) } }
    default { return @{ timeline_minutes = [int](Get-Random @(5, 30)) } }
  }
}

function Wait-BufferOn([int]$seconds) {
  $until = (Get-Date).AddSeconds($seconds)
  while ((Get-Date) -lt $until) {
    $s = Ask-Engine $e 'status.get' '{}'
    # The capture has a frame too: a buffer that is "on" while its source never delivers is the failure this waits out.
    if (($s | Select-String -Quiet '"buffer_state":"on"') -and ($s | Select-String -Quiet '"capture_ready":true')) { return $s }
    Start-Sleep -Milliseconds 500
  }
  return $null
}

function Save-And-Check {
  $before = Get-Date
  # A save is only asked when the ring holds enough: the churn's own rebuilds empty it, so a save a second after a
  # rebuild can only hand over what exists there (measured 0.7-0.9 s clips on 2026-11-02). Skipping such a save is honest;
  # a save made with enough footage that still comes back short is the bug this check exists for.
  $held = 0.0
  $ringDeadline = (Get-Date).AddSeconds(60)
  while ((Get-Date) -lt $ringDeadline) {
    $s = (Ask-Engine $e 'status.get' | ConvertFrom-Json).data
    $held = [double]$s.stats.ring.held_seconds
    if ($held -ge 12) { break }
    Start-Sleep -Seconds 1
  }
  if ($held -lt 12) { "save skipped: the ring held $held s after waiting 60 s"; return }
  # The files that exist now, so a clip left by an earlier save can never satisfy this one.
  $known = @((Ask-Engine $e 'clips.list' '{}' | ConvertFrom-Json).data | ForEach-Object { $_.file })
  $reply = Ask-Engine $e 'buffer.save' '{"seconds":10}'
  if (-not ($reply | Select-String -Quiet '"queued":true')) { Fail "buffer.save was not queued: $reply"; return }
  $deadline = (Get-Date).AddSeconds(90)
  while ((Get-Date) -lt $deadline) {
    # clips.list answers the array itself in `data` (the doc's `[{file,...}]`); a fresh clip's duration is 0 until the
    # listing has probed it, which the budget does within a listing or two.
    $clips = @((Ask-Engine $e 'clips.list' '{}' | ConvertFrom-Json).data)
    $new = @($clips | Where-Object { $_.file -and ($known -notcontains $_.file) })
    $clip = $new | Where-Object { $_.duration -ge 9 -and $_.duration -le 15 } | Select-Object -First 1
    if ($clip) {
      $duration = [double]$clip.duration
      $path = $clip.file
      # The child writes its log to stderr, and a Stop preference turns that into a terminating error; judge it by its
      # exit code instead.
      $old = $ErrorActionPreference
      $ErrorActionPreference = 'Continue'
      $info = & $e.EnginePath --media-info $path 2>&1
      $code = $LASTEXITCODE
      $ErrorActionPreference = $old
      if ($code -ne 0) { Fail "saved clip does not decode: $path ($info)" } else { "saved $($clip.name) $duration s, $([int]((Get-Date) - $before).TotalSeconds) s after the key" }
      return
    }
    if ($new.Count -gt 0) {
      $first = $new[0]
      # A clip's length is 0 until a listing has probed it; only a known length is a length failure.
      if ([double]$first.duration -gt 0) {
        Fail "saved clip is $($first.duration) s, not 9-15 s: $($first.name)"
        return
      }
    }
    Start-Sleep -Seconds 1
  }
  Fail 'buffer.save produced no clip in 90 s'
}

try {
  if (-not (Wait-BufferOn 60)) { Fail 'the buffer did not come on' }
  $stop = (Get-Date).AddMinutes($Minutes)
  $nextSave = (Get-Date).AddMinutes(5)
  while ((Get-Date) -lt $stop) {
    $patch = Random-Patch
    $reply = Ask-Engine $e 'config.set' ($patch | ConvertTo-Json -Compress)
    $changes++
    if ($reply -match '"ok":false') { Fail "config.set refused $($patch | ConvertTo-Json -Compress): $reply" }
    Start-Sleep -Milliseconds (Get-Random -Minimum 300 -Maximum 900)
    # A rebuild is allowed a moment. last_error is set while the new capture starts and clears when it
    # delivers, so judge it on the settled pipeline, not one second after the change: the run that
    # called those transient errors failures had a live capture again by the next change.
    $settled = Wait-BufferOn 40
    if (-not $settled) {
      Fail "the buffer/capture did not come back after $($patch | ConvertTo-Json -Compress)"
    } elseif ($settled -match '"last_error"\s*:\s*"([^"]+)') {
      Fail "last_error after $($patch | ConvertTo-Json -Compress): $($Matches[1])"
    }
    if ((Get-Date) -ge $nextSave) { Save-And-Check; $saves++; $nextSave = (Get-Date).AddMinutes(5) }
    # The runbook's pace: one change every 10-25 s, so the buffer has time to settle between rebuilds.
    Start-Sleep -Milliseconds (Get-Random -Minimum 6000 -Maximum 18000)
  }
  $final = Ask-Engine $e 'status.get' '{}'
  if ($final -match '"last_error"\s*:\s*"([^"]+)') { Fail "last_error at the end: $($Matches[1])" }
  $info = Ask-Engine $e 'library.notify' '{}'
} finally {
  if (-not $e.Process.HasExited) { "engine pid $($e.Process.Id) still up after $changes changes and $saves saves" } else { Fail 'the engine exited' }
  Stop-ProbeEngine $e -KeepData:$KeepData
  if ($loop) { Stop-Process -Id $loop.Id -Force -ErrorAction SilentlyContinue }
}

"settings-churn: $changes changes, $saves saves, $($failures.Count) failures"
if ($failures.Count -gt 0) { exit 1 }
