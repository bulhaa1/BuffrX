# End-to-end proof of a BuffrX link between two engines on this PC: engine A shares a clip it
# holds in its library, engine B opens the link and files the received clip. Two data roots and
# two pipe names keep the run away from the copy the person is using; nothing leaves this PC
# unless iroh's relay is the only route, which the run records as `via`.
#
# Usage: powershell -File tools/share-e2e.ps1 [-SourceClip <file>] [-KeepData] [-HideIp] [-NoLookup]
#   -HideIp   both engines relay only (BUFFRX_SHARE_RELAY_ONLY): proves the relay path, as on a network that blocks UDP
#   -NoLookup engine B's helper does no DNS lookup of peers: proves the relay address in the link is enough
#   -DeadProxy engine A's helper is told to use a proxy nothing listens on: sharing must still work
# Exit code 0 = every check passed, 1 = a check failed, 2 = the harness could not start.

param(
  [string]$SourceClip = '',
  [switch]$KeepData,
  [switch]$HideIp,
  [switch]$NoLookup,
  [switch]$DeadProxy,
  [string]$Engine = 'buffrx-engine.exe'
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$engineExe = Join-Path $root ('runtime\bin\64bit\' + $Engine)
$helperExe = Join-Path $root 'runtime\bin\64bit\buffrx-share.exe'
if (-not (Test-Path $helperExe)) { throw "sharing helper not built: $helperExe" }

$stamp = [DateTime]::UtcNow.Ticks
$rootA = Join-Path $env:TEMP "buffrx-share-e2e-a-$stamp"
$rootB = Join-Path $env:TEMP "buffrx-share-e2e-b-$stamp"
$pipeA = "buffrx-share-e2e-a-$stamp"
$pipeB = "buffrx-share-e2e-b-$stamp"

function New-Root([string]$path) {
  New-Item -ItemType Directory -Force -Path (Join-Path $path 'clips') | Out-Null
  $seed = @{ clips_dir = (Join-Path $path 'clips'); screenshots_dir = (Join-Path $path 'clips\Screenshots'); auto_game_capture = $false; start_buffering = $false; timeline_minutes = 0 } | ConvertTo-Json
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
  Write-Host ("  {0,-52} {1} {2}" -f $name, $status, $detail) -ForegroundColor $color
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
function Start-Engine([string]$dataRoot, [string]$pipe, [string]$tag, [bool]$noLookup = $false) {
  $env:BUFFRX_DATA_ROOT = $dataRoot
  $env:BUFFRX_PIPE = $pipe
  $env:BUFFRX_SHARE_NO_LOOKUP = if ($noLookup) { '1' } else { '' }
  # The helper's relay-only probe: the path a network that blocks UDP takes.
  $env:BUFFRX_SHARE_RELAY_ONLY = if ($HideIp) { '1' } else { '' }
  $env:HTTPS_PROXY = if ($DeadProxy -and $tag -eq 'a') { 'http://127.0.0.1:9' } else { '' }
  return Start-Process -FilePath $engineExe -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput (Join-Path $dataRoot "engine-$tag-out.txt") `
    -RedirectStandardError (Join-Path $dataRoot "engine-$tag-err.txt")
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

$shared = Join-Path $rootA 'clips\shared.mp4'
Copy-Item $SourceClip $shared
$originalBytes = (Get-Item $shared).Length

$engineA = $null
$engineB = $null
try {
  $engineA = Start-Engine $rootA $pipeA 'a'
  Check 'engine A answers on its own pipe' (Wait-Hello $pipeA)
  if (-not (Wait-Hello $pipeA)) { exit 2 }

  $list = [Eng]::Ask($pipeA, '{"id":2,"cmd":"clips.list"}', 30000)
  Check 'engine A sees the clip in its library' ($list -match 'shared\.mp4')

  $share = [Eng]::Ask($pipeA, ('{"id":3,"cmd":"link.share","args":{"file":"' + ($shared -replace '\\','\\') + '","minutes":10}}'), 45000)
  $url = ''
  if ($share -match '"url":"((?:[^"\\]|\\.)*)"') { $url = $Matches[1] -replace '\\\\', '\' }
  # A link as it leaves the engine is the static web page (https://.../l/#payload) so it stays clickable
  # in a chat; the app's own buffrx:// form is still accepted for an older profile's share_link_page. A
  # short link is resolved the way a browser resolves it - one redirect, then the page - so the checks
  # below run on what the app actually sees.
  Check 'link.share makes a link when the clip is in the library' ($share -match '"ok":true' -and ($url -match '^buffrx://' -or $url -match '^https://' -or $url -match '^http://(127\.0\.0\.1|localhost)')) $share.Substring(0, [Math]::Min(240, $share.Length))
  if ($url -notmatch '^buffrx://' -and $url -notmatch '^https://' -and $url -notmatch '^http://(127\.0\.0\.1|localhost)') { exit 2 }

  if ($url -match '/s/') {    $short = $url
    $status = 0; $location = ''
    $req = [System.Net.HttpWebRequest]::Create([Uri]$short)
    $req.AllowAutoRedirect = $false
    $req.Timeout = 15000
    $resp = $null
    try { $resp = $req.GetResponse() } catch [System.Net.WebException] { $resp = $_.Exception.Response }
    if ($resp) {
      $status = [int]$resp.StatusCode
      $location = [string]$resp.Headers['Location']
      $resp.Close()
    }
    Check 'a short link is short' ($short.Length -le 60) ("{0} chars: {1}" -f $short.Length, $short)
    Check 'a short link redirects to the link page' ($status -eq 302 -and $location -match '/l/#') ("$status $location")
    if ($location.StartsWith('/')) {
      $origin = ([Uri]$short).GetLeftPart([UriPartial]::Authority)
      $location = $origin + $location
    }
    $url = $location
  }
  if ($url -match '#(.+)$') { $payload = $Matches[1] } else { $payload = $url -replace '^buffrx://l/?', '' }
  $payload = $payload.Replace('-', '+').Replace('_', '/')
  while ($payload.Length % 4) { $payload += '=' }
  # When the short-link service is configured but did not answer, the share still succeeds with the
  # long link and the reply says why: silence would leave a 126-character link unexplained.
  if ($env:BUFFRX_SHORTEN_URL -and $url -notmatch '/s/') {
    Check 'a failed short link says why it fell back' ($share -match '"note":"[^"]+"') ($share.Substring(0, [Math]::Min(200, $share.Length)))
  }

  $decoded = [Text.Encoding]::ASCII.GetString([Convert]::FromBase64String($payload))
  # Version 2 writes a relay on an iroh domain as "~" + code + subdomain; an older link writes the
  # whole https:// address. Either one means the receiver needs no DNS lookup.
  Check 'the link carries its relay address (no DNS needed)' ($decoded -match '~[12]' -or $decoded -match 'https://') ("{0} chars" -f $url.Length)

  # What the page hands the app: the browser resolves the redirect, the page reads the fragment and
  # opens buffrx://. The engine never sees the web address.
  $openUrl = 'buffrx://l/' + ($url -split '#', 2)[1]
  $self = [Eng]::Ask($pipeA, ('{"id":4,"cmd":"link.open","args":{"url":"' + $openUrl + '"}}'), 15000)
  Check 'opening its own link is refused as own' ($self -match '"own":true')

  $engineB = Start-Engine $rootB $pipeB 'b' ([bool]$NoLookup)
  if (-not (Wait-Hello $pipeB)) { Check 'engine B answers on its own pipe' $false; exit 2 }
  Check 'engine B answers on its own pipe' $true

  $opened = [Eng]::Ask($pipeB, ('{"id":5,"cmd":"link.open","args":{"url":"' + $openUrl + '"}}'), 20000)
  Check 'engine B accepts the link' ($opened -match '"ok":true') $opened.Substring(0, [Math]::Min(200, $opened.Length))
  $receiveId = ''
  if ($opened -match '"id":"([^"]+)"') { $receiveId = $Matches[1] }

  $state = ''; $detail = ''; $received = ''
  for ($i = 0; $i -lt 90; $i++) {
    Start-Sleep -Seconds 1
    $report = [Eng]::Ask($pipeB, '{"id":6,"cmd":"link.list"}', 10000)
    if ($report -match '"receives":\[(.*?)\](?:,"|\})') { $detail = $Matches[1] }
    if ($detail -match '"state":"(done|failed)"') { $state = $Matches[1] }
    if ($detail -match '"file":"((?:[^"\\]|\\.)*)"') { $received = $Matches[1] -replace '\\\\', '\' }
    if ($state) { break }
  }
  Check 'the clip arrives (done, not failed)' ($state -eq 'done') ($detail.Substring(0, [Math]::Min(320, $detail.Length)))
  $via = if ($detail -match '"via":"([^"]+)"') { $Matches[1] } else { '?' }
  if ($HideIp) { Check 'with relay only it came through the relay' ($via -eq 'relay') $via } else { Write-Host "  arrived via $via" }
  Start-Sleep -Seconds 2
  $later = [Eng]::Ask($pipeB, '{"id":61,"cmd":"link.list"}', 10000)
  Check 'a finished receive stays listed until dismissed' ($later -match '"state":"done"')
  Check 'the received file exists' (($received -ne '') -and (Test-Path $received)) $received
  if ($received -and (Test-Path $received)) {
    $got = (Get-Item $received).Length
    Check 'the received file is the announced size' ($got -eq $originalBytes) "$got of $originalBytes bytes"
    $sharedRoot = [System.IO.Path]::GetFullPath((Join-Path $rootB 'clips'))
    Check 'the file landed inside engine B clips' ($received.StartsWith($sharedRoot, [StringComparison]::OrdinalIgnoreCase)) $received
    $libB = [Eng]::Ask($pipeB, '{"id":7,"cmd":"clips.list"}', 30000)
    $leaf = Split-Path $received -Leaf
    Check 'the received clip enters engine B library' ($libB -match [regex]::Escape($leaf))
  }

  if ($receiveId) {
    $dismiss = [Eng]::Ask($pipeB, ('{"id":62,"cmd":"link.cancel","args":{"id":"' + $receiveId + '"}}'), 10000)
    Check 'a finished receive can be dismissed' ($dismiss -match '"ok":true')
  }
  $stop = [Eng]::Ask($pipeA, ('{"id":8,"cmd":"link.stop","args":{"id":"' + ($(if ($share -match '"id":"([^"]+)"') { $Matches[1] } else { '' })) + '"}}'), 10000)
  Check 'the link stops' ($stop -match '"ok":true')
  $after = [Eng]::Ask($pipeB, ('{"id":9,"cmd":"link.open","args":{"url":"' + $url + '"}}'), 15000)
  $refused = ($after -match '"ok":false')
  if (-not $refused) {
    # The engine accepted the URL; the stopped helper has to refuse it, which arrives as a failed receive.
    for ($i = 0; $i -lt 70; $i++) {
      Start-Sleep -Seconds 1
      $r = [Eng]::Ask($pipeB, '{"id":91,"cmd":"link.list"}', 10000)
      if ($r -match '"state":"failed"') { $refused = $true; break }
    }
  }
  Check 'a stopped link no longer delivers' $refused ($after.Substring(0, [Math]::Min(200, $after.Length)))
}
finally {
  if ($engineA) { Stop-Process -Id $engineA.Id -Force -ErrorAction SilentlyContinue }
  if ($engineB) { Stop-Process -Id $engineB.Id -Force -ErrorAction SilentlyContinue }
  # Opening its own link shows the clip in a preview window: close it with the run.
  Get-CimInstance Win32_Process -Filter "Name = 'buffrx-ui.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -match [regex]::Escape($rootA) -or $_.CommandLine -match [regex]::Escape($rootB) } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
  Start-Sleep -Milliseconds 700
  if (-not $KeepData) {
    Remove-Item $rootA -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item $rootB -Recurse -Force -ErrorAction SilentlyContinue
  }
}

Write-Host ''
if ($failures -eq 0) { Write-Host 'share e2e: all checks passed' -ForegroundColor Green; exit 0 }
Write-Host ("share e2e: {0} check(s) failed" -f $failures) -ForegroundColor Red
exit 1
