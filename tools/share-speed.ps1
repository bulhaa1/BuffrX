# How fast one BuffrX link carries a real clip: two engines on this PC, one shares, the other
# receives, and the run reports seconds, MB/s and Mbit/s. A receive over 300 MB is answered yes as
# soon as the engine asks, which is what a person would press.
#
# Usage: powershell -File tools/share-speed.ps1 -Clip <file> [-Kbps N] [-KeepData] [-Engine exe]
#   -Kbps    the sender's link upload cap; 0 keeps the engine's own default (20 Mbit/s)
# Exit code 0 = the clip arrived, 1 = it did not, 2 = the harness could not start.

param(
  [Parameter(Mandatory = $true)][string]$Clip,
  [string]$Engine = 'buffrx-engine.exe',
  [int]$Kbps = 0,
  [switch]$KeepData
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$engineExe = Join-Path $root ('runtime\bin\64bit\' + $Engine)
if (-not (Test-Path $Clip)) { throw "no clip at $Clip" }
if (-not (Test-Path $engineExe)) { throw "no engine at $engineExe" }

$stamp = [DateTime]::UtcNow.Ticks
$rootA = Join-Path $env:TEMP "buffrx-speed-a-$stamp"
$rootB = Join-Path $env:TEMP "buffrx-speed-b-$stamp"
$pipeA = "buffrx-speed-a-$stamp"
$pipeB = "buffrx-speed-b-$stamp"

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

function New-Root([string]$path) {
  New-Item -ItemType Directory -Force -Path (Join-Path $path 'clips') | Out-Null
  $seed = @{
    clips_dir = (Join-Path $path 'clips'); screenshots_dir = (Join-Path $path 'clips\Screenshots')
    auto_game_capture = $false; start_buffering = $false; timeline_minutes = 0
    share_upload_kbps = $Kbps
  } | ConvertTo-Json
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
  for ($i = 0; $i -lt 40; $i++) {
    Start-Sleep -Milliseconds 500
    if (([Eng]::Ask($pipe, '{"id":1,"cmd":"hello"}', 3000)) -match '"protocol"') { return $true }
  }
  return $false
}

$failures = 0
function Check([string]$name, [bool]$ok, [string]$detail = '') {
  $status = if ($ok) { 'PASS' } else { 'FAIL' }; $color = if ($ok) { 'Green' } else { 'Red' }
  Write-Host ("  {0,-52} {1} {2}" -f $name, $status, $detail) -ForegroundColor $color
  if (-not $ok) { $script:failures++ }
}

New-Root $rootA
New-Root $rootB
$engineA = $null; $engineB = $null
try {
  $bytes = (Get-Item $Clip).Length
  $shared = Join-Path $rootA 'clips\shared.mp4'
  Copy-Item $Clip $shared
  Write-Host ("  clip {0:N1} MB" -f ($bytes / 1MB))

  $engineA = Start-Engine $rootA $pipeA 'a'
  if (-not (Wait-Hello $pipeA)) { Check 'engine A starts' $false; exit 2 }

  $share = [Eng]::Ask($pipeA, ('{"id":3,"cmd":"link.share","args":{"file":"' + ($shared -replace '\\','\\') + '","minutes":60}}'), 60000)
  $url = ''
  if ($share -match '"url":"((?:[^"\\]|\\.)*)"') { $url = $Matches[1] -replace '\\\\', '\' }
  $id = ''
  if ($share -match '"id":"([^"]+)"') { $id = $Matches[1] }
  Check 'the link is made' ($url -ne '') ("{0} chars: {1}" -f $url.Length, $url.Substring(0, [Math]::Min(60, $url.Length)))
  if ($url -eq '') { exit 2 }
  if ($share -match '"note":"([^"]*)"') { Write-Host ("  note: " + $Matches[1]) -ForegroundColor Yellow }
  if ($share -match '"short":') { }
  Write-Host ("  short link: " + ($url -match '/s/')) -ForegroundColor Cyan

  $engineB = Start-Engine $rootB $pipeB 'b'
  if (-not (Wait-Hello $pipeB)) { Check 'engine B starts' $false; exit 2 }

  $started = Get-Date
  $opened = [Eng]::Ask($pipeB, ('{"id":5,"cmd":"link.open","args":{"url":"' + $url + '"}}'), 30000)
  Check 'engine B accepts the link' ($opened -match '"ok":true') $opened.Substring(0, [Math]::Min(160, $opened.Length))
  $receiveId = ''
  if ($opened -match '"id":"([^"]+)"') { $receiveId = $Matches[1] }

  $state = ''; $detail = ''; $answered = $false; $last = 0; $posterSeen = $false
  for ($i = 0; $i -lt 3600; $i++) {
    Start-Sleep -Seconds 1
    $report = [Eng]::Ask($pipeB, '{"id":6,"cmd":"link.list"}', 15000)
    if ($report -match '"receives":\[(.*?)\](?:,"|\})') { $detail = $Matches[1] }
    if (-not $answered -and $detail -match '"state":"asking"' -and $receiveId) {
      $answer = [Eng]::Ask($pipeB, ('{"id":7,"cmd":"link.answer","args":{"id":"' + $receiveId + '","accept":true}}'), 15000)
      $answered = $true
      Check 'the large clip asks first, and yes is accepted' ($answer -match '"ok":true') $answer.Substring(0, [Math]::Min(120, $answer.Length))
    }
    if ($detail -match '"state":"(done|failed)"') { $state = $Matches[1]; break }
    if ($detail -match '"bytes":(\d+)') { $last = [int64]$Matches[1] }
    if (-not $posterSeen -and $detail -match '"thumbnail":"') { $posterSeen = $true; Write-Host "  poster arrived while receiving" -ForegroundColor Cyan }
    if (($i % 10) -eq 0 -and $last -gt 0) {
      Write-Host ("    {0,6:N0} MB" -f ($last / 1MB)) -NoNewline
      Write-Host ""
    }
  }
  $seconds = ((Get-Date) - $started).TotalSeconds

  Check 'the clip arrives' ($state -eq 'done') ($detail.Substring(0, [Math]::Min(240, $detail.Length)))
  if ($state -eq 'done') {
    $mb = $bytes / 1MB
    $mbs = $mb / $seconds
    $mbits = ($bytes * 8) / $seconds / 1e6
    Write-Host ""
    Write-Host ("  RESULT  {0:N1} MB in {1:N1} s  =  {2:N2} MB/s  =  {3:N1} Mbit/s" -f $mb, $seconds, $mbs, $mbits) -ForegroundColor Cyan
  }

  if ($id) { [Eng]::Ask($pipeA, ('{"id":8,"cmd":"link.stop","args":{"id":"' + $id + '"}}'), 10000) | Out-Null }
}
finally {
  foreach ($p in @($engineA, $engineB)) { if ($p -and -not $p.HasExited) { try { $p.Kill() } catch { } } }
  if (-not $KeepData) {
    foreach ($r in @($rootA, $rootB)) { try { Remove-Item $r -Recurse -Force -ErrorAction SilentlyContinue } catch { } }
  }
}
exit ($(if ($failures -eq 0) { 0 } else { 1 }))
