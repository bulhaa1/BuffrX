# End-to-end proof of the two features together: while a manual recording runs on the display
# capture, a fabricated Timeline session saves a range, the saved clip is shared to a second
# engine over a BuffrX link, and the recording keeps running across a chunk boundary.
#
# Usage: powershell -File tools/integration-e2e.ps1 [-SourceClip <file>] [-KeepData]
# Exit code 0 = every check passed, 1 = a check failed, 2 = the harness could not start.

param(
  [string]$SourceClip = '',
  [switch]$KeepData,
  [string]$Engine = 'buffrx-engine.exe'
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$engineExe = Join-Path $root ('runtime\bin\64bit\' + $Engine)
if (-not (Test-Path $engineExe)) { throw "engine not built: $engineExe" }

$stamp = [DateTime]::UtcNow.Ticks
$rootA = Join-Path $env:TEMP "buffrx-integr-a-$stamp"
$rootB = Join-Path $env:TEMP "buffrx-integr-b-$stamp"
$pipeA = "buffrx-integr-a-$stamp"
$pipeB = "buffrx-integr-b-$stamp"

function New-Root([string]$path) {
  New-Item -ItemType Directory -Force -Path (Join-Path $path 'clips') | Out-Null
  $seed = @{ clips_dir = (Join-Path $path 'clips'); screenshots_dir = (Join-Path $path 'clips\Screenshots'); auto_game_capture = $false; start_buffering = $false; timeline_minutes = 30; notifications = $false } | ConvertTo-Json
  [System.IO.File]::WriteAllText((Join-Path $path 'config.json'), $seed, (New-Object System.Text.UTF8Encoding($false)))
}
New-Root $rootA
New-Root $rootB

$code = @'
using System; using System.IO; using System.IO.Pipes; using System.Text; using System.Threading.Tasks;
public static class Eng {
    static void W(Stream s, string p) { var b = Encoding.UTF8.GetBytes(p); var h = BitConverter.GetBytes((uint)b.Length); s.Write(h,0,4); s.Write(b,0,b.Length); s.Flush(); }
    static string R(Stream s) {
        while (true) {
            var h = new byte[4]; int n=0; while(n<4){int r=s.Read(h,n,4-n); if(r<=0) throw new IOException("eof"); n+=r;}
            uint len= BitConverter.ToUInt32(h,0); var b=new byte[len]; n=0; while(n<len){int r=s.Read(b,n,(int)len-n); if(r<=0) throw new IOException("eof"); n+=r;}
            var text = Encoding.UTF8.GetString(b);
            if (text.Contains("\"event\"")) continue;
            return text;
        }
    }
    public static string Ask(string pipe, string payload, int ms) {
        try {
            using (var p = new NamedPipeClientStream(".", pipe, PipeDirection.InOut, PipeOptions.Asynchronous)) {
                p.Connect(5000); W(p, payload);
                var t = Task.Run(() => R(p));
                if (!t.Wait(ms)) return "TIMEOUT";
                return t.Result;
            }
        } catch (Exception ex) { return "CONNFAIL: " + ex.Message; }
    }
}
'@
Add-Type -TypeDefinition $code

$failures = 0
function Check([string]$name, [bool]$ok, [string]$detail = '') {
  $status = if ($ok) { 'PASS' } else { 'FAIL' }; $color = if ($ok) { 'Green' } else { 'Red' }
  Write-Host ("  {0,-54} {1} {2}" -f $name, $status, $detail) -ForegroundColor $color
  if (-not $ok) { $script:failures++ }
}
function Wait-Hello([string]$pipe) {
  for ($i = 0; $i -lt 40; $i++) {
    Start-Sleep -Milliseconds 500
    $hello = [Eng]::Ask($pipe, '{"id":1,"cmd":"hello"}', 3000)
    if ($hello -match '"protocol"') { return $true }
  }
  return $false
}
function Start-Engine([string]$dataRoot, [string]$pipe, [string]$tag) {
  $env:BUFFRX_DATA_ROOT = $dataRoot
  $env:BUFFRX_PIPE = $pipe
  return Start-Process -FilePath $engineExe -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput (Join-Path $dataRoot "engine-$tag-out.txt") `
    -RedirectStandardError (Join-Path $dataRoot "engine-$tag-err.txt")
}
function Wait-Buffer([string]$pipe) {
  for ($i = 0; $i -lt 40; $i++) {
    $s = [Eng]::Ask($pipe, '{"id":2,"cmd":"status.get"}', 8000)
    # A buffer that is on but has never had a frame is not a buffer: every check below records, and a
    # recording with no frame writes nothing.
    if ($s -match '"buffer_state":"on"' -and $s -match '"capture_ready":true') { return $true }
    Start-Sleep -Milliseconds 500
  }
  return $false
}

if (-not $SourceClip) {
  $videos = Join-Path ([Environment]::GetFolderPath('MyVideos')) 'BuffrX'
  $candidates = Get-ChildItem $videos -File -Recurse -ErrorAction SilentlyContinue |
    Where-Object { $_.Length -gt 512KB -and $_.Length -lt 20MB -and $_.Extension -in '.mp4', '.mkv' } |
    Sort-Object Length
  if ($candidates) { $SourceClip = $candidates[0].FullName }
}
Check 'a source clip exists' (($SourceClip -ne '') -and (Test-Path $SourceClip)) $SourceClip
if (-not $SourceClip -or -not (Test-Path $SourceClip)) { exit 2 }

$engineA = $null; $engineB = $null; $noise = $null
try {
  # The display capture only produces frames while the picture changes, and a small window can be covered
  # by whatever else is open: a fullscreen window that paints every frame is the reliable way to keep the
  # desktop changing. `screen-noise` stays as the fallback for a tree without the tool.
  $loopExe = Join-Path (Split-Path $engineExe) 'buffrx-present-loop.exe'
  if (Test-Path $loopExe) {
    $noise = Start-Process -FilePath $loopExe -ArgumentList '--fullscreen', '1', '--seconds', '600', '--motion', '1' -PassThru -WindowStyle Hidden
  } else {
    $noise = Start-Process -FilePath 'powershell' -ArgumentList '-NoProfile', '-File', (Join-Path $root 'tools\screen-noise.ps1') -PassThru -WindowStyle Hidden
  }

  $engineA = Start-Engine $rootA $pipeA 'a'
  $helloA = Wait-Hello $pipeA
  Check 'engine A answers on its own pipe' $helloA
  if (-not $helloA) { exit 2 }

  $probe = Join-Path $rootA 'clips\probe.mp4'
  Copy-Item $SourceClip $probe
  $list = [Eng]::Ask($pipeA, '{"id":3,"cmd":"clips.list"}', 60000)
  Check 'engine A measures the source clip' ($list -match 'probe\.mp4' -and $list -match '"duration":([0-9.]+)') ''
  $durationMs = 0
  foreach ($m in [regex]::Matches($list, '\{[^{}]*"file":"[^"]*probe\.mp4"[^{}]*\}')) {
    if ($m.Value -match '"duration":([0-9.]+)') { $durationMs = [int]([double]$Matches[1] * 1000) }
  }
  if ($durationMs -lt 3000) { $durationMs = 3000 }

  # A published session chunk, exactly the shape the writer leaves behind.
  $sessionId = 'integr-' + $stamp
  $sessionDir = Join-Path $rootA "clips\sessions\$sessionId\media"
  New-Item -ItemType Directory -Force -Path $sessionDir | Out-Null
  $chunk = Join-Path $sessionDir 'chunk-0.mp4'
  Copy-Item $probe $chunk
  $nowMs = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
  $nowS = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
  $chunkEntry = @{ file = $chunk; bytes = (Get-Item $chunk).Length; start_ms = 0; end_ms = $durationMs; wall_start_ms = ($nowMs - 60000); wall_end_ms = ($nowMs - 60000 + $durationMs); epoch = 0; published = $true }
  $entry = @{ id = $sessionId; kind = 'timeline'; app_id = ''; profile = ''; started_ms = 0; started_at = $nowS; ended_ms = $durationMs; closed = $true; chunks = @($chunkEntry); markers = @() }
  [System.IO.File]::WriteAllText((Join-Path (Split-Path $sessionDir -Parent) 'index.json'), ($entry | ConvertTo-Json -Depth 6), (New-Object System.Text.UTF8Encoding($false)))

  $start = [Eng]::Ask($pipeA, '{"id":4,"cmd":"buffer.start"}', 20000)
  Check 'the buffer starts on the display capture' (Wait-Buffer $pipeA) $start.Substring(0, [Math]::Min(160, $start.Length))
  $rec = [Eng]::Ask($pipeA, '{"id":5,"cmd":"recording.start"}', 15000)
  Start-Sleep -Seconds 2
  $status = [Eng]::Ask($pipeA, '{"id":6,"cmd":"status.get"}', 8000)
  Check 'a manual recording starts' (($rec -match '"ok":true') -and ($status -match '"recording":true')) ''

  $save = [Eng]::Ask($pipeA, ('{"id":7,"cmd":"timeline.save","args":{"session":"' + $sessionId + '","start_ms":500,"end_ms":' + [Math]::Min(5000, $durationMs) + '}}'), 60000)
  Check 'timeline.save saves a range while recording' ($save -match '"ok":true' -and $save -match '"file"') $save.Substring(0, [Math]::Min(220, $save.Length))
  $savedFile = ''
  if ($save -match '"file":"((?:[^"\\]|\\.)*)"') { $savedFile = $Matches[1] -replace '\\\\', '\' }
  Check 'the saved timeline clip exists and is not empty' (($savedFile -ne '') -and (Test-Path $savedFile) -and ((Get-Item $savedFile).Length -gt 0)) $savedFile

  $share = [Eng]::Ask($pipeA, ('{"id":8,"cmd":"link.share","args":{"file":"' + ($savedFile -replace '\\','\\') + '","minutes":10}}'), 45000)
  $url = ''
  if ($share -match '"url":"((?:[^"\\]|\\.)*)"') { $url = $Matches[1] -replace '\\\\', '\' }
  Check 'the new clip is shared while recording' ($share -match '"ok":true' -and $url -match '^buffrx://') $share.Substring(0, [Math]::Min(200, $share.Length))

  if ($url -match '^buffrx://') {
    $engineB = Start-Engine $rootB $pipeB 'b'
    Check 'engine B answers on its own pipe' (Wait-Hello $pipeB)
    $opened = [Eng]::Ask($pipeB, ('{"id":9,"cmd":"link.open","args":{"url":"' + $url + '"}}'), 20000)
    Check 'engine B accepts the link' ($opened -match '"ok":true') ''
    $state = ''; $received = ''
    for ($i = 0; $i -lt 90; $i++) {
      Start-Sleep -Seconds 1
      $report = [Eng]::Ask($pipeB, '{"id":10,"cmd":"link.list"}', 10000)
      if ($report -match '"state":"(done|failed)"') { $state = $Matches[1] }
      if ($report -match '"file":"((?:[^"\\]|\\.)*)"') { $received = $Matches[1] -replace '\\\\', '\' }
      if ($state) { break }
    }
    Check 'the new clip arrives on engine B' ($state -eq 'done' -and $received -ne '' -and (Test-Path $received)) $received
    if ($received -and (Test-Path $received)) {
      Check 'the received file is the saved size' ((Get-Item $received).Length -eq (Get-Item $savedFile).Length) ''
    }
  }

  # A manual recording must survive a chunk boundary (the chunks are 5 s).
  Start-Sleep -Seconds 7
  $during = [Eng]::Ask($pipeA, '{"id":11,"cmd":"status.get"}', 8000)
  Check 'the recording still runs after a chunk boundary' ($during -match '"recording":true') ''
  Check 'the buffer is still on after save and share' ($during -match '"buffer_state":"on"') ''

  $stop = [Eng]::Ask($pipeA, '{"id":12,"cmd":"recording.stop"}', 30000)
  Start-Sleep -Seconds 2
  $after = [Eng]::Ask($pipeA, '{"id":13,"cmd":"status.get"}', 8000)
  Check 'recording.stop ends the recording' (($stop -match '"ok":true') -and ($after -match '"recording":false')) ''
}
finally {
  if ($engineA) { Stop-Process -Id $engineA.Id -Force -ErrorAction SilentlyContinue }
  if ($engineB) { Stop-Process -Id $engineB.Id -Force -ErrorAction SilentlyContinue }
  if ($noise) { Stop-Process -Id $noise.Id -Force -ErrorAction SilentlyContinue }
  Start-Sleep -Milliseconds 700
  if (-not $KeepData) {
    Remove-Item $rootA -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item $rootB -Recurse -Force -ErrorAction SilentlyContinue
  }
}

Write-Host ''
if ($failures -eq 0) { Write-Host 'integration e2e: all checks passed' -ForegroundColor Green; exit 0 }
Write-Host ("integration e2e: {0} check(s) failed" -f $failures) -ForegroundColor Red
exit 1
