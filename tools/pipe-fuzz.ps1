# N9: the protocol fuzz. Random known and unknown commands with random arguments, frames whose length prefix lies, many
# parallel clients, and clients that vanish mid-reply - against one throwaway engine. The engine must stay up and answer
# `hello` within a second afterwards. Nothing here deletes or stops anything: the never-send list is app.quit,
# engine.restart and every *.delete/*.erase/*.prune command.
#
#   powershell -File tools\pipe-fuzz.ps1 -Minutes 8 -Engine buffrx-engine-night.exe
param(
  [double]$Minutes = 8,
  [string]$Engine = 'buffrx-engine.exe',
  [switch]$KeepData,
  [int[]]$Steps = @(1, 2, 3, 4, 5)   # which of the five kinds of hostility run each round (to find which one stalls an engine)
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'probe-lib.ps1')
Add-Type -AssemblyName System.Core

$cfg = '{"config_version":7,"start_buffering":false,"auto_game_capture":false,"capture_fps":30,"output_width":1280,"output_height":720}'
$e = Start-ProbeEngine $cfg $Engine @{} 0
"engine $($e.Process.Id) root $($e.DataRoot) pipe $($e.Pipe)"
$failures = New-Object System.Collections.Generic.List[string]
$sent = 0

$known = @('hello', 'status.get', 'config.get', 'config.set', 'sources.displays', 'sources.windows', 'sources.games',
  'capture.set', 'clips.list', 'clips.thumbnail', 'screenshots.list', 'hotkeys.get', 'devices.list', 'audio.get',
  'profiles.list', 'profiles.apply', 'sessions.list', 'sessions.get', 'markers.list', 'markers.add', 'game.state',
  'game.exclusions', 'timeline.get', 'timeline.thumbs', 'timeline.loudness', 'encoder.properties', 'ui.frame_stats',
  'ui.show', 'ui.input', 'storage.usage', 'library.notify', 'share.prepare', 'link.list', 'app.open_folder',
  'clips.rename', 'markers.remove')
$never = @('app.quit', 'engine.restart')

function New-RawClient([string]$name) {
  $c = New-Object System.IO.Pipes.NamedPipeClientStream('.', $name, [System.IO.Pipes.PipeDirection]::InOut)
  $c.Connect(2000)
  return $c
}

# Every blocking call on the pipe has a deadline: the run of 2026-11-02 hung for two hours in a bare Write/Read, so the harness could not say whether
# the engine had stopped answering. A call that outlasts its deadline throws, and the round records it as a failure with its name.
function Write-Timed($client, [byte[]]$bytes, [string]$what, [int]$Ms = 8000) {
  $task = $client.WriteAsync($bytes, 0, $bytes.Length)
  if (-not $task.Wait($Ms)) { throw "write of $what ($($bytes.Length) bytes) did not finish in $Ms ms" }
}

function Read-Timed($client, [int]$count, [string]$what, [int]$Ms = 3000) {
  $buffer = New-Object byte[] $count
  $task = $client.ReadAsync($buffer, 0, $count)
  if (-not $task.Wait($Ms)) { throw "read of $what did not return in $Ms ms" }
  return $task.Result
}

function Send-Bytes($client, [byte[]]$frame) {
  Write-Timed $client $frame 'frame'
}

function Send-Frame($client, [string]$text) {
  $payload = [Text.Encoding]::UTF8.GetBytes($text)
  $head = [BitConverter]::GetBytes([uint32]$payload.Length)
  Write-Timed $client $head 'frame header'
  Write-Timed $client $payload 'frame payload'
}

function Send-Lying([string]$pipe, [uint32]$claimed, [int]$actual, [switch]$Close) {
  try {
    $c = New-RawClient $pipe
    $head = [BitConverter]::GetBytes($claimed)
    Write-Timed $c $head 'a lying header'
    if ($actual -gt 0) {
      $body = New-Object byte[] $actual
      for ($i = 0; $i -lt $actual; $i++) { $body[$i] = 65 }
      Write-Timed $c $body 'a lying body'
    }
    if ($Close) { $c.Dispose() } else { Start-Sleep -Milliseconds 100; $c.Dispose() }
  } catch { if ("$_" -match 'did not finish') { $script:failures.Add("lying frame ($claimed claimed, $actual sent): $_") } }
}

function Random-Text([int]$n) {
  $chars = 'abcdefghijklmnopqrstuvwxyz0123456789{}[]",:+-*/<>|!@#$%^&*()_= \t\r\n' + [char]0x00e9 + [char]0x4e2d + [char]0x0001
  $b = New-Object Text.StringBuilder
  for ($i = 0; $i -lt $n; $i++) { [void]$b.Append($chars[(Get-Random -Maximum $chars.Length)]) }
  return $b.ToString()
}

function Random-Args {
  switch (Get-Random -Maximum 8) {
    0 { return '{}' }
    1 { return 'null' }
    2 { return '[]' }
    3 { return '{"seconds":"' + (Random-Text 20) + '"}' }
    4 { return '{"view":' + (Get-Random -Minimum -100000 -Maximum 100000) + '}' }
    5 { return ('[' * 400) + (']' * 400) }
    6 { return '{"file":"' + (Random-Text 300) + '","seconds":1e309}' }
    default { return ('{"x":' + (Random-Text 60) + '}') }
  }
}

function Random-Command {
  $roll = Get-Random -Maximum 10
  if ($roll -lt 7) { $cmd = $known[(Get-Random -Maximum $known.Count)] }
  else { $cmd = Random-Text (Get-Random -Minimum 1 -Maximum 30) }
  if ($never -contains $cmd) { $cmd = 'status.get' }
  return '{"id":' + (Get-Random -Maximum 100000) + ',"cmd":"' + ($cmd -replace '\\', '\\\\' -replace '"', '\"') + '","args":' + (Random-Args) + '}'
}

$stopAt = (Get-Date).AddMinutes($Minutes)
$round = 0
try {
  while ((Get-Date) -lt $stopAt) {
    $round++
    # 1. A handful of well-framed but hostile commands over one connection.
    if ($Steps -contains 1) { try {
      $c = New-RawClient $e.Pipe
      for ($i = 0; $i -lt 20; $i++) { Send-Frame $c (Random-Command); $sent++ }
      $c.Dispose()
    } catch { $failures.Add("round $round framed: $($_.Exception.Message)") } }

    # 2. One 1 MB and one 4 MB argument, well framed.
    if ($Steps -contains 2) { foreach ($size in 1MB, 4MB) {
      # The payload is made before the client connects: building 4 MB of per-character random text in PowerShell takes minutes, and a client that
      # sits connected and silent that long is not reading the engine's events, so the server rightly drops it (the first version of this step
      # blamed the engine). The frame's size is what the parser meets here, so a repeated block is enough.
      $big = 'a' * $size
      $text = '{"id":' + $round + ',"cmd":"config.set","args":{"filename_format":"' + ($big -replace '"', '') + '"}}'
      try {
        $c = New-RawClient $e.Pipe
        Send-Frame $c $text
        $sent++
        $c.Dispose()
      } catch { $failures.Add("round $round big $size : $($_.Exception.Message)") }
      Start-Sleep -Milliseconds 200
    } }

    # 3. Lying length prefixes, including one over kMaxFrameBytes (32 MiB).
    if ($Steps -contains 3) {
    Send-Lying $e.Pipe ([uint32]100) 10 -Close
    Send-Lying $e.Pipe ([uint32]4294967295) 100
    Send-Lying $e.Pipe ([uint32](40 * 1024 * 1024)) 0
    Send-Lying $e.Pipe ([uint32]0) 0
    $sent += 4
    }

    # 4. Three clients at once (the server has four slots): some vanish right after a write, the rest read a reply and leave mid-reply. A client that
    # cannot connect, or fails, is always disposed: a leaked connection holds a slot until the process ends.
    $clients = @()
    if ($Steps -contains 4) { try {
      for ($i = 0; $i -lt 3; $i++) {
        $c = New-RawClient $e.Pipe
        Send-Frame $c '{"id":1,"cmd":"hello","args":{}}'
        if ($i % 2 -eq 0) { $c.Dispose() } else { $clients += $c }
        $sent++
      }
      foreach ($c in $clients) {
        try { [void](Read-Timed $c 4 'a hello reply') } catch { $failures.Add("round $round parallel: $($_.Exception.Message)") }
        $c.Dispose()
      }
    } catch { $failures.Add("round $round parallel: $($_.Exception.Message)") }
      foreach ($c in $clients) { try { $c.Dispose() } catch { } } }

    # 5. A frame that claims a legal length and then trickles too little before the process goes.
    if ($Steps -contains 5) { Send-Lying $e.Pipe ([uint32]1000) 5 -Close }

    if ($round % 10 -eq 0) {
      if ($e.Process.HasExited) { $failures.Add("engine exited mid-round $round"); break }
      "round ${round}: $sent frames, alive"
    }
  }

  if ($e.Process.HasExited) { $failures.Add('engine exited before the final check') }
  else {
    # Liveness: a fresh client must reach hello within one second.
    $watch = [Diagnostics.Stopwatch]::StartNew()
    try {
      $p = New-Object PanelPipe($e.Pipe, 1000)
      try {
        $reply = $p.Ask('hello', '{}')
        $ms = $watch.ElapsedMilliseconds
        if (-not $reply) { $failures.Add('hello returned nothing after the fuzz') }
        elseif ($ms -gt 1000) { $failures.Add("hello took $ms ms after the fuzz") }
        else { "hello after the fuzz: $ms ms" }
      } finally { $p.Dispose() }
    } catch {
      # A second attempt with a longer budget, so one busy moment is not called a hang.
      try {
        $p = New-Object PanelPipe($e.Pipe, 3000)
        try { $reply = $p.Ask('hello', '{}'); if ($reply) { "hello after the fuzz: $($watch.ElapsedMilliseconds) ms (second attempt)" } else { $failures.Add('hello empty') } }
        finally { $p.Dispose() }
      } catch { $failures.Add("hello unreachable after the fuzz: $($_.Exception.Message)") }
    }
  }

  $log = Join-Path $e.DataRoot 'engine.log'
  if (Test-Path $log) {
    $hits = @(Get-Content $log | Select-String -Pattern 'crash|fatal|access violation|unhandled' -CaseSensitive:$false)
    if ($hits.Count -gt 0) { $failures.Add("engine.log has $($hits.Count) crash-like line(s), first: $($hits[0])") }
  }
} finally {
  if ($e -and $e.Process -and -not $e.Process.HasExited) {
    try { Stop-ProbeEngine $e -KeepData:$KeepData } catch { }
  }
}
"pipe-fuzz: $round rounds, $sent frames, $($failures.Count) failure(s)"
foreach ($f in $failures) { "FAIL: $f" }
if ($failures.Count -gt 0) { exit 1 }
