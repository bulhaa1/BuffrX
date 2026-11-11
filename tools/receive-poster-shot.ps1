# A picture of the receive sheet with the sender's poster in it: two engines, a real share, and the
# engine's own ui.capture of the window that the arriving clip opens.
#
# Usage: powershell -File tools/receive-poster-shot.ps1 -Clip <file> [-Kbps 100000] [-Out shot.png]

param(
  [Parameter(Mandatory = $true)][string]$Clip,
  [int]$Kbps = 100000,
  [string]$Out = ''
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$engineExe = Join-Path $root 'runtime\bin\64bit\buffrx-engine.exe'
if (-not (Test-Path $Clip)) { throw "no clip at $Clip" }
if (-not $Out) { $Out = Join-Path $env:TEMP 'receive-poster.png' }

$stamp = [DateTime]::UtcNow.Ticks
$rootA = Join-Path $env:TEMP "buffrx-shot-a-$stamp"
$rootB = Join-Path $env:TEMP "buffrx-shot-b-$stamp"
$pipeA = "buffrx-shot-a-$stamp"
$pipeB = "buffrx-shot-b-$stamp"

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
  $seed = @{ clips_dir = (Join-Path $path 'clips'); screenshots_dir = (Join-Path $path 'clips\Screenshots'); auto_game_capture = $false; start_buffering = $false; timeline_minutes = 0; share_upload_kbps = $Kbps } | ConvertTo-Json
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
  for ($i = 0; $i -lt 60; $i++) { Start-Sleep -Milliseconds 500; if (([Eng]::Ask($pipe, '{"id":1,"cmd":"hello"}', 3000)) -match '"protocol"') { return $true } }
  return $false
}

New-Root $rootA; New-Root $rootB
$engineA = $null; $engineB = $null
try {
  $shared = Join-Path $rootA 'clips\shared.mp4'
  Copy-Item $Clip $shared

  $engineB = Start-Engine $rootB $pipeB 'b'
  if (-not (Wait-Hello $pipeB)) { throw 'engine B did not start' }
  # Open the window first: the receive sheet is drawn over it, and ui.capture "current" needs a window.
  [Eng]::Ask($pipeB, '{"id":2,"cmd":"ui.capture","args":{"view":"main","file":"' + ($Out -replace '\\','\\\\') + '"}}', 20000) | Out-Null
  Start-Sleep -Seconds 2

  $engineA = Start-Engine $rootA $pipeA 'a'
  if (-not (Wait-Hello $pipeA)) { throw 'engine A did not start' }
  $share = [Eng]::Ask($pipeA, ('{"id":3,"cmd":"link.share","args":{"file":"' + ($shared -replace '\\','\\') + '","minutes":60}}'), 60000)
  $url = ''
  if ($share -match '"url":"((?:[^"\\]|\\.)*)"') { $url = $Matches[1] -replace '\\\\', '\' }
  if (-not $url) { throw "no link: $share" }
  $openUrl = 'buffrx://l/' + ($url -split '#', 2)[1]

  $opened = [Eng]::Ask($pipeB, ('{"id":4,"cmd":"link.open","args":{"url":"' + $openUrl + '"}}'), 30000)
  $receiveId = ''
  if ($opened -match '"id":"([^"]+)"') { $receiveId = $Matches[1] }

  # The poster is in the header, so it is there before the video; answer the size question, then shoot.
  $posterAt = $null
  for ($i = 0; $i -lt 120; $i++) {
    Start-Sleep -Milliseconds 500
    $report = [Eng]::Ask($pipeB, '{"id":5,"cmd":"link.list"}', 10000)
    if ($receiveId -and $report -match '"state":"asking"') {
      [Eng]::Ask($pipeB, ('{"id":6,"cmd":"link.answer","args":{"id":"' + $receiveId + '","accept":true}}'), 10000) | Out-Null
    }
    if ($report -match '"thumbnail":"') { $posterAt = $i; break }
  }
  if ($null -eq $posterAt) { throw 'the poster never arrived' }
  "poster present after $([math]::Round($posterAt * 0.5, 1)) s"
  Start-Sleep -Milliseconds 800
  $shot = [Eng]::Ask($pipeB, '{"id":7,"cmd":"ui.capture","args":{"view":"current","file":"' + ($Out -replace '\\','\\\\') + '"}}', 20000)
  "capture: $shot"
  if (Test-Path $Out) { "written: $Out ($([math]::Round((Get-Item $Out).Length/1KB))KB)" } else { "no file" }
}
finally {
  foreach ($p in @($engineA, $engineB)) { if ($p -and -not $p.HasExited) { try { $p.Kill() } catch { } } }
}
