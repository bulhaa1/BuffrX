# Private check of the native panel's present recovery on its own engine: a deferred Present is retried without input, a failing window
# is held and re-arms on its bound, a device that keeps failing is rebuilt a bounded number of times, and a hidden or minimized panel
# makes no attempts. Not a product tool: it never touches the engine the person uses.
#
#   powershell -NoProfile -File tools/ui-idle-probe.ps1                                  # every scenario, about 4 minutes
#   powershell -NoProfile -File tools/ui-idle-probe.ps1 -Scenario busy,occluded          # some of them
#   powershell -NoProfile -File tools/ui-idle-probe.ps1 -Engine build\buffrx-engine.exe -RuntimeBin runtime\bin\64bit
#
# Ownership: every scenario starts its own buffrx-engine.exe on a GUID data root and a GUID pipe, with BUFFRX_UI_TEST_PRESENT and
# BUFFRX_UI_TEST_TARGET set for that child only. Only the process this script started is stopped, by its id. Nothing is killed by name,
# no config of the person's is written, and the scratch data is removed unless -KeepData is given.
#
# Evidence: the engine's own log (test-only NOTE lines with monotonic microseconds: each injected Present or target return, each
# deliberate input, plus the WARN/ERROR lines of the device rebuilds) and ui.frame_stats deltas. Window state (visible, minimized,
# hung) is sampled through user32 before any pipe request, so an idle engine is observed as it is. The probe exits 1 on any missed
# check. Run it only when the person at the machine is away: the panel opens on the desktop.
#
# Each scenario injects one fault mode (see src/engine/ui/ui_present_math.h). The GPU is never reset or lost by the probe; the
# frame, retry and rebuild paths are the real ones.
param(
  [string]$Engine = '',
  [string]$RuntimeBin = '',
  [ValidateSet('all', 'busy-once', 'busy', 'occluded', 'lost-once', 'lost', 'target-once', 'target-persistent', 'hide-minimize')]
  [string[]]$Scenario = @('all'),
  [int]$PassiveSeconds = 5,
  [string]$OutDir = '',
  [switch]$KeepData
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Engine) { $Engine = Join-Path $root 'build\buffrx-engine.exe' }
if (-not $RuntimeBin) { $RuntimeBin = Join-Path $root 'runtime\bin\64bit' }
$Engine = (Resolve-Path -LiteralPath $Engine).Path
$RuntimeBin = (Resolve-Path -LiteralPath $RuntimeBin).Path
if (-not (Test-Path -LiteralPath (Join-Path $RuntimeBin 'obs.dll'))) { throw "no obs.dll beside the engine's runtime: $RuntimeBin" }
$RuntimeRoot = Split-Path -Parent (Split-Path -Parent $RuntimeBin)
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
if (-not $OutDir) { $OutDir = Join-Path $root ('work\ui-idle-probe\' + $stamp) }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;

public static class UiIdleAsk {
    static void Write(Stream s, string payload) {
        var body = Encoding.UTF8.GetBytes(payload);
        s.Write(BitConverter.GetBytes((uint)body.Length), 0, 4);
        s.Write(body, 0, body.Length);
        s.Flush();
    }
    static string Read(Stream s) {
        while (true) {
            var head = new byte[4];
            int n = 0;
            while (n < 4) { int r = s.Read(head, n, 4 - n); if (r <= 0) throw new IOException("the engine closed the pipe"); n += r; }
            uint length = BitConverter.ToUInt32(head, 0);
            var body = new byte[length];
            n = 0;
            while (n < length) { int r = s.Read(body, n, (int)length - n); if (r <= 0) throw new IOException("the engine closed the pipe"); n += r; }
            var text = Encoding.UTF8.GetString(body);
            if (text.Contains("\"event\"")) continue;  // events arrive between replies
            return text;
        }
    }
    public static string Ask(string pipeName, string payload, int ms) {
        using (var pipe = new NamedPipeClientStream(".", pipeName, PipeDirection.InOut, PipeOptions.Asynchronous)) {
            pipe.Connect(5000);
            Write(pipe, payload);
            var reply = Task.Run(() => Read(pipe));
            if (!reply.Wait(ms)) return "{\"ok\":false,\"error\":\"no reply in time\"}";
            return reply.Result;
        }
    }
}

public static class UiIdleWin {
    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback, IntPtr lParam);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool IsHungAppWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr hWnd, int command);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr hWnd, StringBuilder name, int count);
    public static IntPtr[] WindowsOf(uint processId) {
        var found = new List<IntPtr>();
        EnumWindows((hWnd, lParam) => {
            uint owner;
            GetWindowThreadProcessId(hWnd, out owner);
            var name = new StringBuilder(128);
            GetClassName(hWnd, name, name.Capacity);
            if (owner == processId && name.ToString() == "BuffrXPanel") found.Add(hWnd);
            return true;
        }, IntPtr.Zero);
        return found.ToArray();
    }
}
'@

$script:results = New-Object System.Collections.ArrayList
$script:failures = 0
$script:engineHashes = @{}

function Expect([string]$name, [bool]$ok, [string]$detail = '') {
  $tag = 'FAIL'
  if ($ok) { $tag = 'PASS' } else { $script:failures++ }
  [void]$script:results.Add([pscustomobject]@{ Check = $name; Result = $tag; Detail = $detail })
  Write-Host ('  {0} {1} {2}' -f $tag, $name, $detail)
}

function Wait-Until([scriptblock]$condition, [double]$seconds) {
  $deadline = (Get-Date).AddSeconds($seconds)
  do {
    if (& $condition) { return $true }
    Start-Sleep -Milliseconds 100
  } while ((Get-Date) -lt $deadline)
  return $false
}

function Get-PanelWindows([int]$processId) {
  $rows = @()
  foreach ($handle in [UiIdleWin]::WindowsOf([uint32]$processId)) {
    $rows += [pscustomobject]@{
      Hwnd = $handle
      Visible = [UiIdleWin]::IsWindowVisible($handle)
      Iconic = [UiIdleWin]::IsIconic($handle)
      Hung = [UiIdleWin]::IsHungAppWindow($handle)
    }
  }
  return $rows
}

# The first panel window and its passive state, without any pipe request.
function Wait-Panel($run) {
  $deadline = (Get-Date).AddSeconds(25)
  while ((Get-Date) -lt $deadline) {
    if ($run.Proc.HasExited) { return $false }
    $visible = @(Get-PanelWindows $run.Proc.Id | Where-Object { $_.Visible })
    if ($visible.Count -gt 0) { return $true }
    Start-Sleep -Milliseconds 200
  }
  return $false
}

function Ask($run, [string]$command, $payloadArgs) {
  $payload = (@{ id = 1; cmd = $command; args = $payloadArgs } | ConvertTo-Json -Compress -Depth 6)
  $last = $null
  for ($attempt = 0; $attempt -lt 10; $attempt++) {
    try {
      $text = [UiIdleAsk]::Ask($run.Pipe, $payload, 20000)
      return ($text | ConvertFrom-Json)
    } catch {
      $last = $_.Exception.Message
      Start-Sleep -Milliseconds 300
    }
  }
  throw "no pipe answer for $command : $last"
}

function Get-Stats($run) {
  $reply = Ask $run 'ui.frame_stats' @{ reset = $false }
  if (-not $reply.ok) { throw "ui.frame_stats failed: $($reply.error)" }
  return $reply.data
}

function Read-Log($run) {
  if (-not (Test-Path -LiteralPath $run.Log)) { return @() }
  try {
    $stream = [System.IO.File]::Open($run.Log, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    try {
      $reader = New-Object System.IO.StreamReader($stream)
      return @($reader.ReadToEnd() -split "`r?`n")
    } finally {
      $stream.Dispose()
    }
  } catch {
    return @()
  }
}

function Get-Attempts($lines) {
  $rows = @()
  foreach ($line in $lines) {
    if ($line -match 'test fault: present attempt (\d+) returns 0x([0-9a-fA-F]+) at (\d+) us') {
      $rows += [pscustomobject]@{ Index = [int]$matches[1]; HResult = [Convert]::ToUInt32($matches[2], 16); Us = [int64]$matches[3] }
    }
  }
  return $rows
}

function Get-Inputs($lines) {
  $rows = @()
  foreach ($line in $lines) {
    if ($line -match 'test fault: deliberate input at (\d+) us') { $rows += [int64]$matches[1] }
  }
  return $rows
}

function Count-Lines($lines, [string]$pattern) { return @($lines | Where-Object { $_ -match $pattern }).Count }

# Device rebuilds of either kind: a lost present, or a target that could not be made after its attempts.
$RebuildPattern = 'its device will be rebuilt \(\d+ of 3\)|rebuilding its graphics device \(\d+ of 3\)'
$RefusedPattern = 'present failed again|could not draw after 3 device rebuilds'

function Start-Run([string]$present, [string]$target) {
  $guid = [guid]::NewGuid().ToString('N')
  $dataRoot = Join-Path $env:TEMP ('buffrx-ui-idle-' + $guid)
  $clips = Join-Path $dataRoot 'clips'
  New-Item -ItemType Directory -Force -Path $clips | Out-Null
  $config = @{ start_buffering = $false; auto_game_capture = $false; clips_dir = $clips } | ConvertTo-Json
  [System.IO.File]::WriteAllText((Join-Path $dataRoot 'config.json'), $config, (New-Object System.Text.UTF8Encoding($false)))
  # The panel's layout: a check made just now (so the daily update check makes no request) and no first-run welcome.
  $layout = @{ welcomed = $true; update = @{ checked_ms = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() } } | ConvertTo-Json -Depth 4
  [System.IO.File]::WriteAllText((Join-Path $dataRoot 'ui.json'), $layout, (New-Object System.Text.UTF8Encoding($false)))
  $names =@('BUFFRX_DATA_ROOT', 'BUFFRX_PIPE', 'BUFFRX_RUNTIME_ROOT', 'BUFFRX_UI_TEST_PRESENT', 'BUFFRX_UI_TEST_TARGET', 'BUFFRX_TEST_NO_DIALOGS', 'PATH')
  $saved = @{}
  foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
  $proc = $null
  try {
    [Environment]::SetEnvironmentVariable('BUFFRX_DATA_ROOT', $dataRoot, 'Process')
    [Environment]::SetEnvironmentVariable('BUFFRX_PIPE', ('buffrx-ui-idle-' + $guid), 'Process')
    [Environment]::SetEnvironmentVariable('BUFFRX_RUNTIME_ROOT', $RuntimeRoot, 'Process')
    [Environment]::SetEnvironmentVariable('BUFFRX_UI_TEST_PRESENT', $present, 'Process')
    [Environment]::SetEnvironmentVariable('BUFFRX_UI_TEST_TARGET', $target, 'Process')
    [Environment]::SetEnvironmentVariable('BUFFRX_TEST_NO_DIALOGS', '1', 'Process')
    [Environment]::SetEnvironmentVariable('PATH', $RuntimeBin + ';' + $saved['PATH'], 'Process')
    $proc = Start-Process -FilePath $Engine -WorkingDirectory $RuntimeBin -PassThru -WindowStyle Hidden
  } finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
  }
  $run = [pscustomobject]@{
    Proc = $proc
    DataRoot = $dataRoot
    Pipe = 'buffrx-ui-idle-' + $guid
    Log = Join-Path $dataRoot 'logs\engine.log'
    Present = $present
    Target = $target
  }
  try {
    # The pipe starts before UiHost; an earlier ui.show event would have no interface to receive it.
    $ready = Wait-Until {
      $stats = Ask $run 'ui.frame_stats' @{ reset = $false }
      $stats.ok -and [int64]$stats.data.passes -gt 0
    } 25
    if (-not $ready) { throw 'the native interface did not start' }
    # --show opens the compact panel. These checks target the main panel and its default ui.input destination.
    $reply = Ask $run 'ui.show' @{ view = 'main' }
    if (-not $reply.ok) { throw "ui.show failed: $($reply.error)" }
    return $run
  } catch {
    Stop-Run $run
    throw
  }
}

function Stop-Run($run) {
  if (-not $run) { return }
  if ($run.Proc -and -not $run.Proc.HasExited) {
    Stop-Process -Id $run.Proc.Id -Force -ErrorAction SilentlyContinue
    [void]$run.Proc.WaitForExit(5000)
  }
  if (-not $KeepData -and (Test-Path -LiteralPath $run.DataRoot)) {
    Remove-Item -LiteralPath $run.DataRoot -Recurse -Force -ErrorAction SilentlyContinue
  }
}

# Passive state while the panel is open and nothing is asked of it: the answer the person would see on an idle machine.
function Expect-PassiveIdle($run, [string]$label) {
  $before = @(Get-PanelWindows $run.Proc.Id)
  Expect "$label panel window appears without a pipe request" ($before.Count -gt 0) ("windows=" + $before.Count)
  Start-Sleep -Seconds $PassiveSeconds
  $after = @(Get-PanelWindows $run.Proc.Id)
  $hung = @($after | Where-Object { $_.Hung }).Count
  Expect "$label not hung after $PassiveSeconds s without input (passive)" ($hung -eq 0) ("hung=$hung visible=" + @($after | Where-Object { $_.Visible }).Count)
}

function Expect-Engine($run, [string]$label) {
  Expect "$label engine still running" (-not $run.Proc.HasExited) ("pid=" + $run.Proc.Id)
}

# A fault that never stops (busy, occluded): ordinary causes keep arriving (pointer moves every 20 ms, with the open fade at start),
# and one deliberate input arrives during the slow re-arms. Attempts are read from the engine's own lines.
function Invoke-Sustained([string]$label, [string]$present, [double]$runSeconds, [double]$inputAtSeconds, [double]$firstGapMinUs, [bool]$occluded) {
  Write-Host "[$label] $present for $runSeconds s, deliberate input at $inputAtSeconds s"
  $run = Start-Run $present $null
  try {
    Expect "$label panel window appears" (Wait-Panel $run)
    Expect-PassiveIdle $run $label
    Expect "$label first injected Present is seen" (Wait-Until { @(Get-Attempts (Read-Log $run)).Count -ge 1 } 15)
    $start = Get-Date
    $sentInput = $false
    $index = 0
    while (((Get-Date) - $start).TotalSeconds -lt $runSeconds) {
      $elapsed = ((Get-Date) - $start).TotalSeconds
      if (-not $sentInput -and $elapsed -ge $inputAtSeconds) {
        $null = Ask $run 'ui.input' @{ wheel = 0 }
        $sentInput = $true
      }
      $null = Ask $run 'ui.input' @{ move = @((200 + ($index % 5)), 200) }
      $index++
      Start-Sleep -Milliseconds 20
    }
    $lines = Read-Log $run
    $attempts = @(Get-Attempts $lines)
    $inputs = @(Get-Inputs $lines)
    Expect "$label one deliberate input was applied" ($inputs.Count -eq 1) ("inputs=" + $inputs.Count)
    $inputUs = if ($inputs.Count -ge 1) { $inputs[0] } else { [int64]::MaxValue }
    # Before the deliberate input the episode runs its own course: the burst of fast attempts, then the slow re-arms.
    $pre = @($attempts | Where-Object { $_.Us -lt $inputUs })
    $post = @($attempts | Where-Object { $_.Us -ge $inputUs })
    $fast = @($pre | Where-Object { $_.Index -le 13 })
    $slow = @($pre | Where-Object { $_.Index -ge 14 })
    $gaps = @()
    for ($i = 1; $i -lt $attempts.Count; $i++) { $gaps += ($attempts[$i].Us - $attempts[$i - 1].Us) }
    $fastGaps = @()
    for ($i = 1; $i -lt $fast.Count; $i++) { $fastGaps += ($fast[$i].Us - $fast[$i - 1].Us) }
    $slowGaps = @()
    for ($i = 1; $i -lt $slow.Count; $i++) { $slowGaps += ($slow[$i].Us - $slow[$i - 1].Us) }
    $firstGap = if ($pre.Count -ge 2) { $pre[1].Us - $pre[0].Us } else { 0 }
    $minFast = if ($fastGaps.Count) { ($fastGaps | Measure-Object -Minimum).Minimum } else { 0 }
    $minSlow = if ($slowGaps.Count) { ($slowGaps | Measure-Object -Minimum).Minimum } else { 0 }
    $inside = @($gaps | Where-Object { $_ -lt 40000 }).Count
    Expect "$label fast attempts reach the bound (13 before the input)" ($fast.Count -ge 13) ("attempts=" + $attempts.Count + " fast=" + $fast.Count)
    Expect "$label no attempt closer than 40 ms to the last one, with moves every ~20 ms (ordinary causes held)" ($inside -eq 0) ("closer_than_40ms=$inside")
    Expect "$label first gap honours the host backoff" ($firstGap -ge $firstGapMinUs) ("first_gap_ms=" + [math]::Round($firstGap / 1000.0, 1))
    Expect "$label fast gaps honour the backoff floor" ($minFast -ge $firstGapMinUs - 5000) ("min_fast_gap_ms=" + [math]::Round($minFast / 1000.0, 1))
    if ($occluded) {
      $nonDecreasing = $true
      for ($i = 1; $i -lt $fastGaps.Count; $i++) { if ($fastGaps[$i] -lt $fastGaps[$i - 1] - 5000) { $nonDecreasing = $false } }
      Expect "$label occluded gaps back off, never shrink" $nonDecreasing
    }
    Expect "$label slow re-arms are one second apart" (($slowGaps.Count -lt 2) -or ($minSlow -ge 900000)) ("slow=" + $slow.Count + " min_slow_gap_ms=" + [math]::Round($minSlow / 1000.0, 1))
    if ($inputs.Count -eq 1) {
      $promptMs = if ($post.Count) { ($post[0].Us - $inputUs) / 1000.0 } else { -1 }
      Expect "$label deliberate input draws within 150 ms, not at the retry's deadline" ($post.Count -gt 0 -and $promptMs -le 150) ("first_attempt_after_ms=" + [math]::Round($promptMs, 1))
      $restartGap = if ($post.Count -ge 2) { $post[1].Us - $post[0].Us } else { 0 }
      Expect "$label the new episode honours the backoff again" ($post.Count -ge 2 -and $restartGap -ge $firstGapMinUs - 5000) ("restart_gap_ms=" + [math]::Round($restartGap / 1000.0, 1))
    }
    Expect-Engine $run $label
    $stats = Get-Stats $run
    Write-Host ("  info {0} frame stats: presented={1} skipped={2} passes={3}" -f $label, $stats.presented, $stats.skipped, $stats.passes)
  } finally {
    Stop-Run $run
  }
}

function Invoke-BusyOnce {
  Write-Host '[busy-once] one busy Present, then the retry must draw without input'
  $run = Start-Run 'busy-once' $null
  try {
    Expect 'busy-once panel window appears' (Wait-Panel $run)
    Expect-PassiveIdle $run 'busy-once'
    Expect 'busy-once injected Present is seen' (Wait-Until { @(Get-Attempts (Read-Log $run)).Count -ge 1 } 15)
    $attempts = @(Get-Attempts (Read-Log $run))
    Expect 'busy-once exactly one injected attempt, the first' (($attempts.Count -eq 1) -and ($attempts[0].Index -eq 1)) ("attempts=" + $attempts.Count)
    Start-Sleep -Seconds 2
    $first = Get-Stats $run
    Expect 'busy-once the retry shows the frame with no input' ($first.presented -ge 1) ("presented=" + $first.presented)
    Start-Sleep -Seconds 3
    $second = Get-Stats $run
    $quiet = $second.presented - $first.presented
    Expect 'busy-once the picture settles (no frames without input)' ($quiet -le 10) ("presents in 3 s quiet=$quiet")
    Expect 'busy-once no second injected attempt' (@(Get-Attempts (Read-Log $run)).Count -eq 1)
    Expect-Engine $run 'busy-once'
  } finally {
    Stop-Run $run
  }
}

function Invoke-LostOnce {
  Write-Host '[lost-once] one lost device on the first Present: rebuilt once, then the frame shows'
  $run = Start-Run 'lost-once' $null
  try {
    Expect 'lost-once panel window appears' (Wait-Panel $run)
    Expect-PassiveIdle $run 'lost-once'
    Expect 'lost-once the device is rebuilt once' (Wait-Until { (Count-Lines (Read-Log $run) $RebuildPattern) -ge 1 } 15)
    Start-Sleep -Seconds 3
    $lines = Read-Log $run
    Expect 'lost-once exactly one rebuild' ((Count-Lines $lines $RebuildPattern) -eq 1) ("rebuilds=" + (Count-Lines $lines $RebuildPattern))
    Expect 'lost-once the rebuilt device shows its frame with no input' ((Get-Stats $run).presented -ge 1)
    Expect-Engine $run 'lost-once'
  } finally {
    Stop-Run $run
  }
}

function Invoke-LostPersistent {
  Write-Host '[lost] every Present lost: a bounded number of rebuilds, then only a deliberate input'
  $run = Start-Run 'lost' $null
  try {
    Expect 'lost panel window appears' (Wait-Panel $run)
    Expect-PassiveIdle $run 'lost'
    Expect 'lost the rebuild budget is spent and logged' (Wait-Until { (Count-Lines (Read-Log $run) $RefusedPattern) -ge 1 } 30)
    $lines = Read-Log $run
    Expect 'lost exactly three rebuilds before the refusal' ((Count-Lines $lines $RebuildPattern) -eq 3) ("rebuilds=" + (Count-Lines $lines $RebuildPattern))
    Start-Sleep -Seconds 5
    $quiet = Read-Log $run
    Expect 'lost no rebuild loop without input (passive, 5 s)' ((Count-Lines $quiet $RebuildPattern) -eq 3) ("rebuilds=" + (Count-Lines $quiet $RebuildPattern))
    Expect 'lost no attempts without input (passive, 5 s)' (@(Get-Attempts $quiet).Count -eq @(Get-Attempts $lines).Count)
    $null = Ask $run 'ui.input' @{ wheel = 0 }
    Expect 'lost a deliberate input grants exactly one more rebuild' (Wait-Until { (Count-Lines (Read-Log $run) $RebuildPattern) -ge 4 } 15)
    Start-Sleep -Seconds 5
    $after = Read-Log $run
    Expect 'lost the grant is spent again: four rebuilds in total, then refused' ((Count-Lines $after $RebuildPattern) -eq 4) ("rebuilds=" + (Count-Lines $after $RebuildPattern))
    Expect-Engine $run 'lost'
  } finally {
    Stop-Run $run
  }
}

function Invoke-TargetOnce {
  Write-Host '[target-once] the first target cannot be made: the next attempt makes it'
  $run = Start-Run $null 'once'
  try {
    Expect 'target-once panel window appears' (Wait-Panel $run)
    Expect-PassiveIdle $run 'target-once'
    Expect 'target-once the refusal is seen' (Wait-Until { (Count-Lines (Read-Log $run) 'test fault: target call 1 refused') -ge 1 } 15)
    Expect 'target-once the panel recovers and shows a frame' (Wait-Until { (Get-Stats $run).presented -ge 1 } 10)
    Expect 'target-once no device rebuild' ((Count-Lines (Read-Log $run) $RebuildPattern) -eq 0)
    Expect-Engine $run 'target-once'
  } finally {
    Stop-Run $run
  }
}

function Invoke-TargetPersistent {
  Write-Host '[target-persistent] no target is ever made: three rebuilds, then one attempt a second, then input only'
  $run = Start-Run $null 'persistent'
  try {
    Expect 'target-persistent panel window appears' (Wait-Panel $run)
    Expect-PassiveIdle $run 'target-persistent'
    $gaveUp = Wait-Until { (Count-Lines (Read-Log $run) 'could not draw after 3 device rebuilds') -ge 1 } 150
    $lines = Read-Log $run
    Expect 'target-persistent the episode gives up after three rebuilds' $gaveUp ("rebuilds=" + (Count-Lines $lines 'rebuilding its graphics device \(\d+ of 3\)'))
    Expect 'target-persistent exactly three escalations' ((Count-Lines $lines 'rebuilding its graphics device \(\d+ of 3\)') -eq 3)
    $before = Count-Lines $lines 'test fault: target call'
    Start-Sleep -Seconds 4
    $later = Read-Log $run
    $calls = (Count-Lines $later 'test fault: target call') - $before
    Expect 'target-persistent after the bound: one attempt a second, not a loop' (($calls -ge 2) -and ($calls -le 6)) ("target calls in 4 s=$calls")
    Expect 'target-persistent no rebuild after the bound' ((Count-Lines $later 'rebuilding its graphics device \(\d+ of 3\)') -eq 3)
    Expect-Engine $run 'target-persistent'
  } finally {
    Stop-Run $run
  }
}

function Invoke-HideMinimize {
  Write-Host '[hide-minimize] hidden and minimized panels make no attempts; restore and show resume them'
  $run = Start-Run 'occluded' $null
  try {
    Expect 'hide-minimize panel window appears' (Wait-Panel $run)
    Expect-PassiveIdle $run 'hide-minimize'
    Expect 'hide-minimize attempts run before the hide' (Wait-Until { @(Get-Attempts (Read-Log $run)).Count -ge 3 } 15)
    $null = Ask $run 'ui.show' @{ view = 'hide' }
    Start-Sleep -Seconds 2
    $hiddenAt = @(Get-Attempts (Read-Log $run)).Count
    Start-Sleep -Seconds 4
    $hiddenLater = @(Get-Attempts (Read-Log $run)).Count
    Expect 'hide-minimize hidden: no attempts' ($hiddenLater -eq $hiddenAt) ("attempts $hiddenAt -> $hiddenLater")
    $null = Ask $run 'ui.show' @{ view = 'main' }
    Expect 'hide-minimize shown again: attempts resume' (Wait-Until { @(Get-Attempts (Read-Log $run)).Count -gt $hiddenLater } 10)
    Start-Sleep -Seconds 1
    $panel = @(Get-PanelWindows $run.Proc.Id | Where-Object { $_.Visible } | Select-Object -First 1)
    if ($panel.Count -eq 0) {
      Expect 'hide-minimize a visible panel window to minimize' $false
      return
    }
    $null = [UiIdleWin]::ShowWindowAsync($panel[0].Hwnd, 6)  # SW_MINIMIZE
    Start-Sleep -Seconds 2
    $minimizedAt = @(Get-Attempts (Read-Log $run)).Count
    Start-Sleep -Seconds 4
    $minimizedLater = @(Get-Attempts (Read-Log $run)).Count
    Expect 'hide-minimize minimized: no attempts' ($minimizedLater -eq $minimizedAt) ("attempts $minimizedAt -> $minimizedLater")
    $null = [UiIdleWin]::ShowWindowAsync($panel[0].Hwnd, 9)  # SW_RESTORE
    Expect 'hide-minimize restored: attempts resume' (Wait-Until { @(Get-Attempts (Read-Log $run)).Count -gt $minimizedLater } 10)
    Expect-Engine $run 'hide-minimize'
  } finally {
    Stop-Run $run
  }
}

# ---------------------------------------------------------------- run
$chosen = if ($Scenario -contains 'all') {
  @('busy-once', 'busy', 'occluded', 'lost-once', 'lost', 'target-once', 'target-persistent', 'hide-minimize')
} else {
  $Scenario
}

Write-Host "engine:        $Engine ($((Get-Item -LiteralPath $Engine).LastWriteTime))"
Write-Host "runtime bin:   $RuntimeBin"
$script:engineHashes['engine'] = (Get-FileHash -LiteralPath $Engine -Algorithm SHA256).Hash
$script:engineHashes['obs.dll'] = (Get-FileHash -LiteralPath (Join-Path $RuntimeBin 'obs.dll') -Algorithm SHA256).Hash
Write-Host "engine sha256: $($script:engineHashes['engine'])"
Write-Host "scenarios:     $($chosen -join ', ')"

$started = Get-Date
try {
  foreach ($name in $chosen) {
    switch ($name) {
      'busy-once' { Invoke-BusyOnce }
      'busy' { Invoke-Sustained 'busy' 'busy' 18 8 55000 $false }
      'occluded' { Invoke-Sustained 'occluded' 'occluded' 18 15 240000 $true }
      'lost-once' { Invoke-LostOnce }
      'lost' { Invoke-LostPersistent }
      'target-once' { Invoke-TargetOnce }
      'target-persistent' { Invoke-TargetPersistent }
      'hide-minimize' { Invoke-HideMinimize }
    }
  }
} finally {
  $summary = [pscustomobject]@{
    started = $started.ToString('s')
    seconds = [math]::Round(((Get-Date) - $started).TotalSeconds, 1)
    engine = $Engine
    engine_sha256 = $script:engineHashes['engine']
    obs_dll_sha256 = $script:engineHashes['obs.dll']
    scenarios = $chosen
    failures = $script:failures
    checks = $script:results
  }
  $summary | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $OutDir 'results.json') -Encoding UTF8
  Write-Host ''
  Write-Host ("{0} checks, {1} failed; results in {2}" -f $script:results.Count, $script:failures, (Join-Path $OutDir 'results.json'))
}
if ($script:failures -gt 0) { exit 1 }
exit 0
