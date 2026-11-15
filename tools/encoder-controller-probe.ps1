# Plan 008, phase 3 and 4: does the controller move by itself, in both directions, without emptying the buffer, and does the machine remember?
#
#   powershell -File tools/encoder-controller-probe.ps1                 # scripted: the encoder is made to seem short, then healthy (about 9 minutes)
#   powershell -File tools/encoder-controller-probe.ps1 -Real -Fps 240   # a real heavy load instead (this GPU must fail to keep up with it)
#
# Scripted: BUFFRX_TEST_ENCODER_RATIO=@<file> makes the engine read the share of the pictures the encoder finished from a file, so the run does not
# depend on a load that happens to overwhelm the GPU. The script writes 1.0, then 0.80 (a down move after the grace window and 10 s), then 1.0 again
# (an up move after 3 minutes), then 0.80 and 1.0 held for 75 s (the level is remembered in capacity.json under a ctl| key), saves a clip of the whole
# buffer (decoded frame by frame with ffprobe), stops the engine without removing its data, starts another on the same data root and checks that it
# begins at the remembered level. Every 5 seconds the status is read: the footage held must never fall by more than a few seconds.
param(
  [switch]$Real,
  [string]$Codec = 'h264',   # ffprobe's AV1 decoder refuses NVENC's level 7.x at 1440p120; the engine's own --media-frames is used when ffprobe reads nothing
  [int]$Fps = 120,
  [string]$Mode = 'quality',
  [int]$RealMinutes = 4,
  [string]$Engine = 'buffrx-engine.exe',
  [int]$BufferSeconds = 40,
  [string]$Ffprobe = $env:BUFFRX_FFPROBE
)
. (Join-Path $PSScriptRoot 'probe-lib.ps1')
$noise = Start-Process powershell -ArgumentList '-NoProfile', '-File', (Join-Path $PSScriptRoot 'screen-noise.ps1') -PassThru -WindowStyle Hidden
$e = $null; $e2 = $null
$lines = New-Object System.Collections.Generic.List[string]
$failures = 0
$root = Join-Path $env:TEMP ('buffrx-ctl-probe-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
$ratioFile = Join-Path $root 'ratio.txt'
New-Item -ItemType Directory -Force $root | Out-Null
function Fail([string]$text) { $script:failures++; $script:lines.Add("FAIL $text") }
$state = @{ held = 0.0; maxDrop = 0.0; level = 0 }
function Sample($e, [datetime]$t0) {
  $s = Ask-Engine $e 'status.get'
  $level = [int](Num $s 'level'); $held = Num $s 'held_seconds'; $ratio = Num $s 'encoder_ratio'
  $label = [regex]::Match($s, '"label":\s*"([^"]*)"').Groups[1].Value
  if ($held -lt $state.held - 3) { $state.maxDrop = [math]::Max($state.maxDrop, $state.held - $held) }
  $state.held = $held
  if ($level -ne $state.level) { $lines.Add(('{0,5:N0}s  level {1} -> {2}  {3}  (epoch {4}, held {5:N1} s)' -f ((Get-Date) - $t0).TotalSeconds, $state.level, $level, $label, [int](Num $s 'epoch'), $held)); $state.level = $level }
  [pscustomobject]@{ Level = $level; Held = $held; Ratio = $ratio; Label = $label }
}
function Wait-Level($e, [datetime]$t0, [scriptblock]$until, [int]$seconds) {
  $deadline = (Get-Date).AddSeconds($seconds)
  while ((Get-Date) -lt $deadline) { Start-Sleep -Seconds 5; $r = Sample $e $t0; if (& $until $r) { return $true } }
  return $false
}
try {
  $envs = @{}
  if (-not $Real) { Set-Content -LiteralPath $ratioFile -Value '1.0' -NoNewline; $envs['BUFFRX_TEST_ENCODER_RATIO'] = '@' + $ratioFile }
  $cfg = '{"config_version":7,"codec_auto_picked":true,"start_buffering":false,"auto_game_capture":false,"timeline_minutes":0,"capture_fps":' + $Fps + ',"buffer_seconds":' + $BufferSeconds + ',"codec":"' + $Codec + '","encoder":"nvenc","encoder_mode":"' + $Mode + '","quality":"highest","container":"mp4","audio_tracks":1}'
  $e = Start-ProbeEngine $cfg $Engine $envs 0 $root
  Wait-Asleep $e
  $null = Ask-Engine $e 'buffer.start'; Wait-Buffering $e
  $t0 = Get-Date
  if ($Real) {
    $null = Wait-Level $e $t0 { param($r) $false } ($RealMinutes * 60)
  } else {
    # 1. healthy: nothing moves, grace included
    $null = Wait-Level $e $t0 { param($r) $r.Level -ne 0 } 45
    if ($state.level -ne 0) { Fail 'it moved while the encoder was healthy' }
    # 2. short: a down move
    Set-Content -LiteralPath $ratioFile -Value '0.80' -NoNewline; $tShort = Get-Date
    if (-not (Wait-Level $e $t0 { param($r) $r.Level -ge 1 } 90)) { Fail 'no move down within 90 s of 80% encoded' }
    else { $lines.Add(('INFO the first move came {0:N0} s after the encoder fell short (the rule is 10 s past the grace window)' -f ((Get-Date) - $tShort).TotalSeconds)) }
    # 3. healthy again: an up move after about three minutes
    Set-Content -LiteralPath $ratioFile -Value '1.0' -NoNewline; $tHealthy = Get-Date
    if (-not (Wait-Level $e $t0 { param($r) $r.Level -eq 0 } 330)) { Fail 'no move back up within 330 s of a healthy encoder' }
    else { $lines.Add(('INFO the move back up came {0:N0} s after the encoder was healthy again (the rule is 3 minutes)' -f ((Get-Date) - $tHealthy).TotalSeconds)) }
    # 4. short again, then healthy and held: the level is remembered
    Set-Content -LiteralPath $ratioFile -Value '0.80' -NoNewline
    if (-not (Wait-Level $e $t0 { param($r) $r.Level -ge 1 } 120)) { Fail 'no second move down' }
    Set-Content -LiteralPath $ratioFile -Value '0.995' -NoNewline
    $null = Wait-Level $e $t0 { param($r) $false } 80
  }
  $keyLevel = $state.level
  $capacity = Join-Path $e.DataRoot 'capacity.json'
  $capText = if (Test-Path $capacity) { Get-Content $capacity -Raw } else { '' }
  $key = [regex]::Match($capText, '"(ctl\|[^"]*)":\s*(\d+)')
  $lines.Add("INFO capacity.json: " + $(if ($key.Success) { "$($key.Groups[1].Value) = $($key.Groups[2].Value)" } else { '(no ctl| key)' }))
  if (-not $Real -and (-not $key.Success -or [int]$key.Groups[2].Value -ne $keyLevel)) { Fail "the level $keyLevel that held was not remembered" }
  $log = Join-Path $e.DataRoot 'logs\engine.log'
  $logText = Get-Content $log -Raw
  foreach ($l in @([regex]::Matches($logText, '[^\r\n]*(encoder controller|encoder swap:|the encoder swap was given up)[^\r\n]*') | ForEach-Object { $_.Value })) { $lines.Add("LOG $l") }
  $lines.Add("largest drop of held footage: $([math]::Round($state.maxDrop, 1)) s")
  if ($state.maxDrop -gt 5) { Fail 'the buffer lost footage during a move' }
  # the clip: the whole buffer, across the moves
  $clips = Join-Path $e.DataRoot 'clips'
  $null = Ask-Engine $e 'buffer.save' ('{"seconds":' + $BufferSeconds + '}')
  $file = $null
  for ($i = 0; $i -lt 120 -and -not $file; $i++) {
    Start-Sleep -Milliseconds 500
    $f = Get-ChildItem $clips -Recurse -File -Include '*.mp4' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -notlike '*\.buffrx\*' -and $_.Name -like 'Replay *' } | Sort-Object LastWriteTime | Select-Object -Last 1
    if ($f -and $f.Length -gt 0) { $size = $f.Length; Start-Sleep -Milliseconds 1500; $f.Refresh(); if ($f.Length -eq $size) { $file = $f.FullName } }
  }
  if (-not $file) { Fail 'no clip appeared' } else {
    $keep = Join-Path $env:TEMP 'encoder-controller-clip.mp4'; Copy-Item $file $keep -Force
    $pref = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    $out = (& $Ffprobe -v error -select_streams v:0 -show_entries frame=pts_time -of csv=p=0 $keep 2>&1 | Out-String)
    $ErrorActionPreference = $pref
    $times = @(); $bad = 0
    foreach ($l in ($out -split "`r?`n")) { if ($l -match '^[0-9.]+,?$') { $times += [double]($l.TrimEnd(',')) } elseif ($l.Trim()) { $bad++ } }
    $sorted = $times | Sort-Object
    $spacing = @{}
    for ($i = 1; $i -lt $sorted.Count; $i++) { $k = [math]::Round(($sorted[$i] - $sorted[$i - 1]) * 1000, 0); $spacing[$k] = 1 + [int]$spacing[$k] }
    $lines.Add("clip: $($times.Count) frames, $(if ($sorted.Count) { [math]::Round($sorted[-1], 1) } else { 0 }) s, decode errors $bad, spacing " + (($spacing.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 4 | ForEach-Object { "$($_.Key)ms x$($_.Value)" }) -join ' '))
    if ($times.Count -eq 0) {
      $pref = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
      $media = (& (Resolve-ProbeEngine $Engine).Exe --media-frames $keep 2>&1 | Where-Object { "$_" -like '{*' } | Select-Object -Last 1)
      $ErrorActionPreference = $pref
      $lines.Add("clip (the engine's own decoder, ffprobe read nothing): $media")
      if ($media -notmatch '"ok":true') { Fail 'the clip does not decode cleanly' }
    } elseif ($bad -gt 0) { Fail 'the clip does not decode cleanly' }
  }
  Copy-Item $log (Join-Path $env:TEMP 'encoder-controller.log') -Force -ErrorAction SilentlyContinue
  # 5. the memory: a new engine on the same data root starts at the level that held
  if (-not $Real -and $key.Success) {
    Stop-ProbeEngine $e -KeepData; $e = $null
    $e2 = Start-ProbeEngine $cfg $Engine $envs 0 $root
    Wait-Asleep $e2
    $null = Ask-Engine $e2 'buffer.start'; Wait-Buffering $e2
    $s2 = Ask-Engine $e2 'status.get'
    $startLevel = [int](Num $s2 'level')
    $lines.Add("INFO the second engine started at level $startLevel (remembered: $($key.Groups[2].Value))")
    if ($startLevel -ne [int]$key.Groups[2].Value) { Fail 'the second engine did not start at the remembered level' }
  }
} catch { Fail "line $($_.InvocationInfo.ScriptLineNumber): $($_.Exception.Message)" }
finally {
  if ($e) { Stop-ProbeEngine $e }
  if ($e2) { Stop-ProbeEngine $e2 }
  if ($noise) { Stop-Process -Id $noise.Id -Force -ErrorAction SilentlyContinue }
  if (Test-Path $root) { Assert-ProbeDataRoot $root | Out-Null; Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}
$lines | ForEach-Object { $_ }
"encoder controller probe: $failures failed"
exit $(if ($failures) { 1 } else { 0 })
