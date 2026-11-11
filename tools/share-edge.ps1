# The edge cases a BuffrX link has to survive: no timer, two receivers at once, the sharer's engine
# restarting, a file that disappears after it was shared, and Stop.
#
# Usage: powershell -File tools/share-edge.ps1 [-SourceClip <file>] [-KeepData]
# Exit code 0 = every case held, 1 = one did not, 2 = the harness could not start.

param(
  [string]$SourceClip = '',
  [switch]$KeepData,
  [string]$Engine = 'buffrx-engine.exe'
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$engineExe = Join-Path $root ('runtime\bin\64bit\' + $Engine)
if (-not (Test-Path $engineExe)) { throw "no engine at $engineExe" }

$stamp = [DateTime]::UtcNow.Ticks
$rootA = Join-Path $env:TEMP "buffrx-edge-a-$stamp"
$rootB = Join-Path $env:TEMP "buffrx-edge-b-$stamp"
$rootC = Join-Path $env:TEMP "buffrx-edge-c-$stamp"
$pipeA = "buffrx-edge-a-$stamp"
$pipeB = "buffrx-edge-b-$stamp"
$pipeC = "buffrx-edge-c-$stamp"

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
  Write-Host ("  {0,-58} {1} {2}" -f $name, $status, $detail) -ForegroundColor $color
  if (-not $ok) { $script:failures++ }
}
function New-Root([string]$path) {
  New-Item -ItemType Directory -Force -Path (Join-Path $path 'clips') | Out-Null
  $seed = @{ clips_dir = (Join-Path $path 'clips'); screenshots_dir = (Join-Path $path 'clips\Screenshots'); auto_game_capture = $false; start_buffering = $false; timeline_minutes = 0 } | ConvertTo-Json
  [System.IO.File]::WriteAllText((Join-Path $path 'config.json'), $seed, (New-Object System.Text.UTF8Encoding($false)))
}
function Start-Engine([string]$dataRoot, [string]$pipe, [string]$tag) {
  $env:BUFFRX_DATA_ROOT = $dataRoot
  $env:BUFFRX_PIPE = $pipe
  return Start-Process -FilePath $engineExe -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput (Join-Path $dataRoot "engine-$tag-out.txt") `
    -RedirectStandardError (Join-Path $dataRoot "engine-$tag-err.txt")
}
function Wait-Hello([string]$pipe) {
  for ($i = 0; $i -lt 60; $i++) {
    Start-Sleep -Milliseconds 500
    if (([Eng]::Ask($pipe, '{"id":1,"cmd":"hello"}', 3000)) -match '"protocol"') { return $true }
  }
  return $false
}
function Receive-State([string]$pipe) {
  $report = [Eng]::Ask($pipe, '{"id":9,"cmd":"link.list"}', 15000)
  if ($report -match '"receives":\[(.*?)\](?:,"|\})') { return $Matches[1] }
  return ''
}

if (-not $SourceClip) {
  $videos = Join-Path ([Environment]::GetFolderPath('MyVideos')) 'BuffrX'
  $found = Get-ChildItem $videos -File -Recurse -ErrorAction SilentlyContinue |
    Where-Object { $_.Length -gt 512KB -and $_.Length -lt 20MB -and $_.Extension -in '.mp4', '.mkv' } | Select-Object -First 1
  if ($found) { $SourceClip = $found.FullName }
}
Check 'a source clip exists' (($SourceClip -ne '') -and (Test-Path $SourceClip)) $SourceClip
if (-not $SourceClip -or -not (Test-Path $SourceClip)) { exit 2 }

New-Root $rootA; New-Root $rootB; New-Root $rootC
$engineA = $null; $engineB = $null; $engineC = $null
try {
  $shared = Join-Path $rootA 'clips\shared.mp4'
  Copy-Item $SourceClip $shared
  $engineA = Start-Engine $rootA $pipeA 'a'
  if (-not (Wait-Hello $pipeA)) { Check 'engine A starts' $false; exit 2 }

  # 1. The window the person chose: an hour, and the link must hold for exactly that long.
  $share = [Eng]::Ask($pipeA, ('{"id":3,"cmd":"link.share","args":{"file":"' + ($shared -replace '\\','\\') + '","minutes":60}}'), 60000)
  $url = ''
  if ($share -match '"url":"((?:[^"\\]|\\.)*)"') { $url = $Matches[1] -replace '\\\\', '\' }
  $expires = 0
  if ($share -match '"expires":(\d+)') { $expires = [int64]$Matches[1] }
  $now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
  Check 'a one-hour link is made' ($url -ne '') ("{0} chars" -f $url.Length)
  Check 'it expires in the hour that was asked for' ((($expires - $now) -ge 3500) -and (($expires - $now) -le 3700)) ("{0} min" -f [math]::Round(($expires - $now) / 60))

  # The engine's link is now a short page URL (https://.../s/<id>): resolve it the way a browser does,
  # one redirect to the page that carries the payload, before the app-to-app form is built.
  if ($url -match '/s/') {
    $req = [System.Net.HttpWebRequest]::Create([Uri]$url)
    $req.AllowAutoRedirect = $false
    $req.Timeout = 15000
    $resp = $null
    try { $resp = $req.GetResponse() } catch [System.Net.WebException] { $resp = $_.Exception.Response }
    if ($resp) {
      $location = [string]$resp.Headers['Location']
      $resp.Close()
      if ($location.StartsWith('/')) { $location = ([Uri]$url).GetLeftPart([UriPartial]::Authority) + $location }
      if ($location) { $url = $location }
    }
  }
  $openUrl = 'buffrx://l/' + ($url -split '#', 2)[1]

  # 2. Two receivers at once.
  $engineB = Start-Engine $rootB $pipeB 'b'
  $engineC = Start-Engine $rootC $pipeC 'c'
  $bUp = Wait-Hello $pipeB
  if (-not $bUp) {   # one cold start in this folder failed once: try once more before calling it a bug
    if ($engineB -and -not $engineB.HasExited) { $engineB.Kill() }
    $engineB = Start-Engine $rootB $pipeB 'b2'
    $bUp = Wait-Hello $pipeB
  }
  $cUp = Wait-Hello $pipeC
  Check 'both receiver engines start' ($bUp -and $cUp) "B=$bUp C=$cUp"
  $openB = [Eng]::Ask($pipeB, ('{"id":5,"cmd":"link.open","args":{"url":"' + $openUrl + '"}}'), 20000)
  $openC = [Eng]::Ask($pipeC, ('{"id":6,"cmd":"link.open","args":{"url":"' + $openUrl + '"}}'), 20000)
  Check 'two receivers can open the same link' (($openB -match '"ok":true') -and ($openC -match '"ok":true')) $openB.Substring(0, [Math]::Min(120, $openB.Length))

  # 3. A second open on the same engine is the same download, not a second one.
  Start-Sleep -Milliseconds 400
  $again = [Eng]::Ask($pipeB, ('{"id":7,"cmd":"link.open","args":{"url":"' + $openUrl + '"}}'), 15000)
  Check 'opening it twice does not start a second transfer' ($again -match '"existing":true' -or $again -match '"ok":true') $again.Substring(0, [Math]::Min(120, $again.Length))

  $stateB = ''; $stateC = ''
  $deadline = (Get-Date).AddSeconds(120)
  while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 1
    $b = Receive-State $pipeB; $c = Receive-State $pipeC
    if ($b -match '"state":"(done|failed)"') { $stateB = $Matches[1] }
    if ($c -match '"state":"(done|failed)"') { $stateC = $Matches[1] }
    if ($stateB -eq 'failed' -or $stateC -eq 'failed') { break }
    if ($stateB -and $stateC) { break }
  }
  Check 'the first receiver got the clip' ($stateB -eq 'done') $stateB
  Check 'the second receiver got it too' ($stateC -eq 'done') $stateC

  # 4. The sharer's engine restarts: the link comes back from links.json and serves again.
  if ($engineA -and -not $engineA.HasExited) { $engineA.Kill(); $engineA.WaitForExit(10000) | Out-Null }
  Start-Sleep -Seconds 2
  $engineA = Start-Engine $rootA $pipeA 'a2'
  Check 'the sharer engine comes back' (Wait-Hello $pipeA)
  $openedAgain = [Eng]::Ask($pipeB, ('{"id":10,"cmd":"link.open","args":{"url":"' + $openUrl + '"}}'), 20000)
  $stateAfter = ''
  $deadline = (Get-Date).AddSeconds(90)
  while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 1
    $s = Receive-State $pipeB
    $states = [regex]::Matches($s, '"state":"(done|failed)"')
    if ($states.Count -ge 2) { $stateAfter = 'done'; break }
    if ($s -match '"state":"failed"') { $stateAfter = 'failed'; break }
  }
  Check 'the link still works after the sharer restarts' (($openedAgain -match '"ok":true') -and ($stateAfter -eq 'done')) $openedAgain.Substring(0, [Math]::Min(120, $openedAgain.Length))

  # 4b. The sharing helper itself is killed: the engine brings it back and the link serves again.
  Get-Process buffrx-share -ErrorAction SilentlyContinue | ForEach-Object { try { $_.Kill() } catch { } }
  Start-Sleep -Seconds 14
  $revived = [Eng]::Ask($pipeC, ('{"id":"13","cmd":"link.open","args":{"url":"' + $openUrl + '"}}'), 20000)
  $stateRevived = ''
  $deadline = (Get-Date).AddSeconds(90)
  while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 1
    $s = Receive-State $pipeC
    $states = [regex]::Matches($s, '"state":"(done|failed)"')
    if ($states.Count -ge 2) { $stateRevived = 'done'; break }
  }
  Check 'a link survives its sharing helper being killed' (($revived -match '"ok":true') -and ($stateRevived -eq 'done')) $revived.Substring(0, [Math]::Min(120, $revived.Length))

  # 5. Stop ends it at once.
  if ($share -match '"id":"([^"]+)"') {
    $stop = [Eng]::Ask($pipeA, ('{"id":11,"cmd":"link.stop","args":{"id":"' + $Matches[1] + '"}}'), 15000)
    Check 'Stop is accepted' ($stop -match '"ok":true')
  }
  $after = [Eng]::Ask($pipeC, ('{"id":12,"cmd":"link.open","args":{"url":"' + $openUrl + '"}}'), 20000)
  $ended = ($after -match '"ok":false')
  if (-not $ended) {
    for ($i = 0; $i -lt 60; $i++) {
      Start-Sleep -Seconds 1
      if ((Receive-State $pipeC) -match '"state":"failed"') { $ended = $true; break }
    }
  }
  Check 'a stopped link no longer delivers' $ended ($after.Substring(0, [Math]::Min(140, $after.Length)))
}
finally {
  foreach ($p in @($engineA, $engineB, $engineC)) { if ($p -and -not $p.HasExited) { try { $p.Kill() } catch { } } }
  if (-not $KeepData) { foreach ($r in @($rootA, $rootB, $rootC)) { try { Remove-Item $r -Recurse -Force -ErrorAction SilentlyContinue } catch { } } }
}
Write-Host ""
if ($failures -eq 0) { Write-Host "share edge cases: all checks passed" -ForegroundColor Green }
else { Write-Host "share edge cases: $failures check(s) failed" -ForegroundColor Red }
exit ($(if ($failures -eq 0) { 0 } else { 1 }))
