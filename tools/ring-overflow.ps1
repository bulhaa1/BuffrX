# The replay ring's disk part, proved on a throwaway engine (own data root and pipe): buffer for a while, watch what RAM holds and what the file
# holds, save a clip the length asked for, and check the numbers against each other.
#
#   powershell -File tools\ring-overflow.ps1 -Seconds 600 -RunSeconds 700 -PeakKbps 40000 -Engine buffrx-engine-ring.exe
#   powershell -File tools\ring-overflow.ps1 -Seconds 30 -RunSeconds 70 -RamMb 32 -Engine buffrx-engine-ring.exe   # a forced overflow in a minute
#   powershell -File tools\ring-overflow.ps1 -Seconds 20 -NoSpill                                                   # today's behaviour, for the comparison
#
# What it checks: the clip is the length asked for (and ffprobe-clean); RAM never passed its limit; the engine wrote to disk exactly what the
# ring says it spilled (within one 8 MB block) and what spilled is what left RAM; the ring file existed while the buffer ran and is gone after it
# stops; nothing grew in the clips folder but the clip. Run it when the machine is quiet: the picture is kept changing by tools\screen-noise.ps1.
param(
  [int]$Seconds = 60,              # the buffer length asked for
  [int]$RunSeconds = 0,            # how long to buffer before saving (default: the length plus 30 s, so the ring is full and has been full)
  [int]$RamMb = 0,                 # BUFFRX_RING_RAM_MB: a smaller RAM limit, to overflow in seconds (0 = the real limit)
  [int]$PeakKbps = 0,              # a fixed peak rate (0 = the quality preset's own)
  [string]$Codec = 'h264',
  [string]$Container = 'mp4',
  [int]$Tracks = 1,
  [int]$Fps = 60,
  [string]$Engine = 'buffrx-engine.exe',
  [string]$Ffprobe = 'ffprobe',
  [switch]$NoSpill,                # BUFFRX_NO_SPILL=1
  [switch]$Motion,                 # a window of moving rectangles (buffrx-present-loop --motion 1): footage at game-like rates, not the noise square's 1 Mbps
  [switch]$NoNoise
)
. (Join-Path $PSScriptRoot 'probe-lib.ps1')
if ($RunSeconds -le 0) { $RunSeconds = $Seconds + 30 }
$noise = if ($NoNoise) { $null } else { Start-Process powershell -ArgumentList '-NoProfile', '-File', (Join-Path $PSScriptRoot 'screen-noise.ps1') -PassThru -WindowStyle Hidden }
$loop = $null
if ($Motion) {
  $exe = Join-Path $script:ProbeRoot 'build\buffrx-present-loop.exe'
  $loop = Start-Process $exe -ArgumentList '--seconds', ($RunSeconds + 120), '--width', '1280', '--height', '720', '--motion', '1' -PassThru
}
$e = $null
$fail = @()
function Check([string]$name, [bool]$ok, [string]$detail = '') { if (-not $ok) { $script:fail += $name }; ('  {0}  {1}  {2}' -f $(if ($ok) { 'ok  ' } else { 'FAIL' }), $name, $detail) }
function Flag($json, $name) { [regex]::Match($json, '"' + $name + '":\s*(true|false)').Groups[1].Value -eq 'true' }
try {
  $envs = @{}
  if ($RamMb -gt 0) { $envs['BUFFRX_RING_RAM_MB'] = [string]$RamMb }
  if ($NoSpill) { $envs['BUFFRX_NO_SPILL'] = '1' }
  $peak = if ($PeakKbps -gt 0) { ',"quality":"highest","peak_mode":"fixed","custom_kbps":' + $PeakKbps } else { '' }
  $cfg = '{"config_version":7,"start_buffering":false,"auto_game_capture":false,"timeline_minutes":0,"capture_fps":' + $Fps + ',"buffer_seconds":' + $Seconds + ',"codec":"' + $Codec + '","container":"' + $Container + '","audio_tracks":' + $Tracks + $peak + '}'
  $e = Start-ProbeEngine $cfg $Engine $envs
  Wait-Asleep $e
  if ($Motion) { $captured = Set-WindowCapture $e; "capturing window: $captured" }
  $clips = Join-Path $e.DataRoot 'clips'
  $ringDir = Join-Path $clips '.buffrx'
  $w0 = Write-Bytes $e
  $null = Ask-Engine $e 'buffer.start'; Wait-Buffering $e
  $started = Get-Date
  $maxRam = 0; $sawFile = 0; $fileMb = 0; $last = ''; $quiet = $true; $stalledSeen = $false
  $samples = @()
  while (((Get-Date) - $started).TotalSeconds -lt $RunSeconds) {
    Start-Sleep -Seconds 5
    $s = Ask-Engine $e 'status.get'; $last = $s
    $ram = Num $s 'ram_bytes'; if ($ram -gt $maxRam) { $maxRam = $ram }
    if (Flag $s 'stalled') { $stalledSeen = $true }
    $f = Get-ChildItem $ringDir -Filter 'replay-*.ring' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($f) { $sawFile++; $fileMb = [math]::Round($f.Length / 1MB, 0) }
    if (-not (Test-Quiet 5)) { $quiet = $false }
    $samples += [pscustomobject]@{ 's' = [int]((Get-Date) - $started).TotalSeconds; 'RAM MB' = [math]::Round($ram / 1MB, 0); 'Disk MB' = [math]::Round((Num $s 'disk_bytes') / 1MB, 0)
      'Spilled MB' = [math]::Round((Num $s 'spilled_bytes') / 1MB, 0); 'Received MB' = [math]::Round((Num $s 'received_bytes') / 1MB, 0); 'Mbps' = [math]::Round((Num $s 'avg_kbps') / 1000, 1)
      Disk = [regex]::Match($s, '"disk":"([^"]*)"').Groups[1].Value; Stalled = (Flag $s 'stalled') }
  }
  $samples | Select-Object -Last 12 | Format-Table -AutoSize | Out-String -Width 200
  $received = Num $last 'received_bytes'; $spilled = Num $last 'spilled_bytes'; $ramNow = Num $last 'ram_bytes'; $diskNow = Num $last 'disk_bytes'
  $before = Write-Bytes $e
  $clipsBefore = @(Get-ChildItem $clips -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.FullName -notlike '*\.buffrx\*' }).Count
  $null = Ask-Engine $e 'buffer.save' ('{"seconds":' + $Seconds + '}')
  $saveWatch = [Diagnostics.Stopwatch]::StartNew()
  $file = $null
  for ($i = 0; $i -lt 240 -and -not $file; $i++) {
    Start-Sleep -Milliseconds 500
    $f = Get-ChildItem $clips -Recurse -File -Include "*.$Container" -ErrorAction SilentlyContinue | Where-Object { $_.FullName -notlike '*\.buffrx\*' } | Sort-Object LastWriteTime | Select-Object -Last 1
    if ($f -and $f.Length -gt 0) { $size = $f.Length; Start-Sleep -Milliseconds 1500; $f.Refresh(); if ($f.Length -eq $size) { $file = $f.FullName } }
  }
  $saveSeconds = [math]::Round($saveWatch.Elapsed.TotalSeconds, 1)
  $afterSave = Write-Bytes $e
  "the clip was stable $saveSeconds s after the save request"
  "ring after the run: received $([math]::Round($received/1MB,0)) MB, spilled $([math]::Round($spilled/1MB,0)) MB, RAM $([math]::Round($ramNow/1MB,0)) MB (most $([math]::Round($maxRam/1MB,0)) MB), file holds $([math]::Round($diskNow/1MB,0)) MB, file size on disk $fileMb MB"
  "engine wrote $([math]::Round(($before - $w0)/1MB,1)) MB to disk while buffering, $([math]::Round(($afterSave - $before)/1MB,1)) MB during the save"
  $wrote = $before - $w0
  $limitBytes = if ($RamMb -gt 0) { $RamMb * 1MB } else { 2048MB }
  $spillOn = -not $NoSpill
  if ($file) {
    $j = (& $Ffprobe -v error -show_entries 'format=duration:stream=codec_type,codec_name' -of json $file 2>$null | Out-String) | ConvertFrom-Json
    $duration = [double]$j.format.duration
    Check 'a clip was saved' $true $file
    Check 'the clip is the length asked for' (($duration -ge ($Seconds - 0.5)) -and ($duration -le ($Seconds + 5))) ("{0:N1} s for {1} s" -f $duration, $Seconds)
    $before = $ErrorActionPreference; $ErrorActionPreference = 'Continue'   # ffprobe logs to stderr; that is not an error here
    $packets = (& $Ffprobe -v error -select_streams v:0 -count_packets -show_entries stream=nb_read_packets -of csv=p=0 $file 2>&1 | Out-String).Trim()
    $ErrorActionPreference = $before
    Check 'ffprobe reads every video packet with no error' (($packets -match '^\d+$') -and ([int]$packets -ge 0.9 * $Fps * $duration)) ("{0} packets for {1:N1} s at {2} fps" -f $packets, $duration, $Fps)
  } else { Check 'a clip was saved' $false 'no clip appeared' }
  $overflowed = $spilled -gt 0
  if ($spillOn -and $RamMb -gt 0 -or $overflowed) {
    Check 'RAM never passed its limit' ($maxRam -le $limitBytes) ("most {0:N0} MB, limit {1:N0} MB" -f ($maxRam / 1MB), ($limitBytes / 1MB))
    Check 'what spilled is what left RAM' ([math]::Abs($spilled - ($received - $ramNow)) -le ([math]::Max(0.03 * $received, 16MB))) ("spilled {0:N0} MB, received - RAM {1:N0} MB" -f ($spilled / 1MB), (($received - $ramNow) / 1MB))
    Check 'the engine wrote what the ring spilled and no more (within one block)' (($wrote -ge $spilled) -and (($wrote - $spilled) -le (8MB + 0.02 * $spilled + 4MB))) ("wrote {0:N0} MB, spilled {1:N0} MB" -f ($wrote / 1MB), ($spilled / 1MB))
    Check 'the ring file existed while the buffer ran' ($sawFile -gt 0) "$sawFile samples, $fileMb MB"
  } else {
    Check 'the footage fit in RAM: nothing spilled' ($spilled -eq 0) ("spilled {0:N0} MB, received {1:N0} MB" -f ($spilled / 1MB), ($received / 1MB))
    Check 'and the engine wrote almost nothing while buffering' ($wrote -lt 16MB) ("wrote {0:N1} MB" -f ($wrote / 1MB))
    Check 'and no ring file was made' ($sawFile -eq 0)
  }
  $null = Ask-Engine $e 'buffer.stop'
  $gone = $false
  for ($i = 0; $i -lt 40 -and -not $gone; $i++) { Start-Sleep -Milliseconds 500; $gone = -not (Get-ChildItem $ringDir -Filter 'replay-*.ring' -ErrorAction SilentlyContinue) }
  Check 'the ring file is gone once the buffer stops' $gone
  Check 'the machine was quiet for the whole run' $quiet '(a run during which the person used the machine is not publishable)'
} finally {
  Stop-ProbeEngine $e
  if ($loop) { Stop-Process -Id $loop.Id -Force -ErrorAction SilentlyContinue }
  if ($noise) { Stop-Process -Id $noise.Id -Force -ErrorAction SilentlyContinue }
}
if ($fail.Count) { "ring overflow: FAILED - " + ($fail -join '; '); exit 1 }
'ring overflow: all checks passed'
