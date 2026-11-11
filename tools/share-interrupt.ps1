# Interruption proof for a BuffrX link on this PC: a transfer survives its sender's helper
# dying (the retry path), a cancel stops it, and a sender that disappears leaves no clip.
# Two data roots and pipes keep the run away from the engine the person is using; originals
# are only read. Exit 0 = every check passed, 1 = a check failed, 2 = the harness could not run.

param(
  [string]$SourceClip = '',
  [switch]$KeepData,
  [string]$Engine = 'buffrx-engine.exe'
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$engineExe = Join-Path $root ('runtime\bin\64bit\' + $Engine)
$helperName = 'buffrx-share.exe'
if (-not (Test-Path $engineExe)) { throw "engine not built: $engineExe" }

if (-not $SourceClip) {
  $videos = Join-Path ([Environment]::GetFolderPath('MyVideos')) 'BuffrX'
  $SourceClip = (Get-ChildItem $videos -File -Recurse -ErrorAction SilentlyContinue |
    Where-Object { $_.Length -gt 60MB -and $_.Length -lt 250MB -and $_.Extension -eq '.mp4' } |
    Sort-Object Length | Select-Object -First 1).FullName
}
if (-not $SourceClip -or -not (Test-Path $SourceClip)) { Write-Host 'interrupt e2e: no 60-250 MB clip found' -ForegroundColor Red; exit 2 }

$stamp = [DateTime]::UtcNow.Ticks
$rootA = Join-Path $env:TEMP "buffrx-share-int-a-$stamp"
$rootB = Join-Path $env:TEMP "buffrx-share-int-b-$stamp"
$pipeA = "buffrx-share-int-a-$stamp"
$pipeB = "buffrx-share-int-b-$stamp"

# $uploadKbps caps the sender: on one PC an uncapped 66 MB clip arrives in under a second, before any of the
# interruptions below can land in the middle of it (the default became Unlimited, and every check here failed).
function New-Root([string]$path, [int]$uploadKbps = 0) {
  New-Item -ItemType Directory -Force -Path (Join-Path $path 'clips') | Out-Null
  $seed = @{ clips_dir = (Join-Path $path 'clips'); screenshots_dir = (Join-Path $path 'clips\Screenshots'); auto_game_capture = $false; start_buffering = $false; timeline_minutes = 0; share_upload_kbps = $uploadKbps } | ConvertTo-Json
  [System.IO.File]::WriteAllText((Join-Path $path 'config.json'), $seed, (New-Object System.Text.UTF8Encoding($false)))
}
New-Root $rootA 16000
New-Root $rootB

$code = @'
using System; using System.IO; using System.IO.Pipes; using System.Text; using System.Threading.Tasks;
public static class IntEng {
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
  Write-Host ("  {0,-56} {1} {2}" -f $name, $status, $detail) -ForegroundColor $color
  if (-not $ok) { $script:failures++ }
}
function Wait-Hello([string]$pipe) {
  for ($i = 0; $i -lt 40; $i++) {
    Start-Sleep -Milliseconds 500
    if (([IntEng]::Ask($pipe, '{"id":1,"cmd":"hello"}', 3000)) -match '"protocol"') { return $true }
  }
  return $false
}
function Start-Engine([string]$dataRoot, [string]$pipe, [string]$tag) {
  $env:BUFFRX_DATA_ROOT = $dataRoot
  $env:BUFFRX_PIPE = $pipe
  Start-Process -FilePath $engineExe -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput (Join-Path $dataRoot "out-$tag.txt") `
    -RedirectStandardError (Join-Path $dataRoot "err-$tag.txt")
}
function Receive-Id([string]$reply) { if ($reply -match '"id":"([^"]+)"') { return $Matches[1] } return '' }
function Receive-State([string]$pipe, [string]$id) {
  $report = [IntEng]::Ask($pipe, '{"id":90,"cmd":"link.list"}', 10000)
  foreach ($m in [regex]::Matches($report, '\{[^{}]*"id":"' + [regex]::Escape($id) + '"[^{}]*\}')) { return $m.Value }
  return $report
}
function Wait-State([string]$pipe, [string]$id, [string[]]$want, [int]$seconds) {
  $last = ''
  for ($i = 0; $i -lt $seconds; $i++) {
    Start-Sleep -Seconds 1
    $last = Receive-State $pipe $id
    foreach ($w in $want) { if ($last -match ('"state":"' + $w + '"')) { return @{ State = $w; Detail = $last } } }
  }
  return @{ State = ''; Detail = $last }
}
function New-Link([string]$file) {
  $share = [IntEng]::Ask($pipeA, ('{"id":2,"cmd":"link.share","args":{"file":"' + ($file -replace '\\','\\') + '","minutes":30}}'), 45000)
  if (-not ($share -match '"url":"([^"]+)"')) { return '' }
  $url = $Matches[1]
  # The engine hands out the short page link (https://.../s/<id>); resolve its one redirect so the
  # app-to-app form the receiver opens carries the payload.
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
  return $url
}
function Start-Receive([string]$url) {
  $opened = [IntEng]::Ask($pipeB, ('{"id":3,"cmd":"link.open","args":{"url":"' + $url + '"}}'), 20000)
  return Receive-Id $opened
}
function Kill-SenderHelper($engineProcess) {
  $victims = Get-CimInstance Win32_Process -Filter "Name = '$helperName'" -ErrorAction SilentlyContinue |
    Where-Object { $_.ParentProcessId -eq $engineProcess.Id }
  foreach ($v in $victims) { Stop-Process -Id $v.ProcessId -Force -ErrorAction SilentlyContinue }
  return @($victims).Count
}
function Received-Copies {
  return @(Get-ChildItem (Join-Path $rootB 'clips') -File -Recurse -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^big( \(\d+\))?\.mp4$' })
}

$shared = Join-Path $rootA 'clips\big.mp4'
Copy-Item $SourceClip $shared
$originalBytes = (Get-Item $shared).Length
$leaf = 'big.mp4'

$engineA = $null
$engineB = $null
try {
  $engineA = Start-Engine $rootA $pipeA 'a'
  if (-not (Wait-Hello $pipeA)) { Write-Host 'interrupt e2e: engine A did not answer' -ForegroundColor Red; exit 2 }
  $null = [IntEng]::Ask($pipeA, '{"id":4,"cmd":"clips.list"}', 60000)

  $engineB = Start-Engine $rootB $pipeB 'b'
  if (-not (Wait-Hello $pipeB)) { Write-Host 'interrupt e2e: engine B did not answer' -ForegroundColor Red; exit 2 }

  # 1. The sender's helper dies mid-transfer; the retry has to finish the clip exactly once.
  $url = New-Link $shared
  Check 'a link is made for the big clip' ($url -match '^(buffrx://l/|https://[^#]+#)') $url
  $id1 = Start-Receive $url
  $started = Wait-State $pipeB $id1 @('receiving') 40
  Check 'the transfer reaches receiving' ($started.State -eq 'receiving') $started.Detail
  if ($started.State -eq 'receiving') {
    $killed = Kill-SenderHelper $engineA
    Check 'the sender helper was killed during the transfer' ($killed -ge 1) "$killed helper(s)"
    $end = Wait-State $pipeB $id1 @('done', 'failed') 240
    Check 'the transfer recovers after its helper died' ($end.State -eq 'done') $end.Detail
    if ($end.State -eq 'done') {
      $file = ''
      if ($end.Detail -match '"file":"((?:[^"\\]|\\.)*)"') { $file = $Matches[1] -replace '\\\\', '\' }
      Check 'the recovered file is the exact size' ($file -and (Get-Item $file).Length -eq $originalBytes) "$file"
      $copies = @(Received-Copies)
      Check 'the recovered clip landed once, not twice' ($copies.Count -eq 1) "$($copies.Count) file(s)"
    }
    $null = [IntEng]::Ask($pipeB, ('{"id":92,"cmd":"link.cancel","args":{"id":"' + $id1 + '"}}'), 10000)
  }

  # 1b. The receiver's own helper dies mid-transfer; the retry resumes the part file instead of starting again.
  $urlR = New-Link $shared
  $idR = Start-Receive $urlR
  $startedR = Wait-State $pipeB $idR @('receiving') 40
  if ($startedR.State -eq 'receiving') {
    Start-Sleep -Seconds 4
    $killedR = Kill-SenderHelper $engineB
    Check 'the receiver helper was killed during the transfer' ($killedR -ge 1) "$killedR helper(s)"
    $endR = Wait-State $pipeB $idR @('done', 'failed') 240
    Check 'the transfer finishes after the receiver helper died' ($endR.State -eq 'done') $endR.Detail
    $logB = Get-Content (Join-Path $rootB 'logs\engine.log') -Raw -ErrorAction SilentlyContinue
    Check 'the retry resumed instead of starting again' ($logB -match 'resuming at byte [1-9]') ''
    if ($endR.Detail -match '"file":"((?:[^"\\]|\\.)*)"') {
      $fileR = $Matches[1] -replace '\\\\', '\'
      Check 'the resumed file is identical to the original' ((Get-FileHash $fileR).Hash -eq (Get-FileHash $shared).Hash) $fileR
    }
    $null = [IntEng]::Ask($pipeB, ('{"id":94,"cmd":"link.cancel","args":{"id":"' + $idR + '"}}'), 10000)
  } else { Check 'the resume transfer reaches receiving' $false $startedR.Detail }
  # 2. A cancel mid-transfer stops the bytes and leaves no clip.
  $before = @(Received-Copies).Count
  $url2 = New-Link $shared
  $id2 = Start-Receive $url2
  $null = Wait-State $pipeB $id2 @('receiving') 40
  $cancel = [IntEng]::Ask($pipeB, ('{"id":93,"cmd":"link.cancel","args":{"id":"' + $id2 + '"}}'), 10000)
  Check 'a transfer can be cancelled' ($cancel -match '"ok":true') $cancel
  Start-Sleep -Seconds 3
  $after = Receive-State $pipeB $id2
  Check 'a cancelled transfer does not finish' ($after -match '"state":"cancelled"' -or $after -notmatch '"state":"done"') $after
  Check 'a cancelled transfer leaves no new clip' (@(Received-Copies).Count -eq $before) "$(@(Received-Copies).Count) file(s)"

  # 3. The sender engine disappears mid-transfer; the receive fails and files nothing.
  $url3 = New-Link $shared
  $id3 = Start-Receive $url3
  $null = Wait-State $pipeB $id3 @('receiving') 40
  Stop-Process -Id $engineA.Id -Force -ErrorAction SilentlyContinue
  $engineA = $null
  $end3 = Wait-State $pipeB $id3 @('done', 'failed') 300
  Check 'a sender that disappears ends in failed' ($end3.State -eq 'failed') $end3.Detail
  Check 'a failed receive files no new clip' (@(Received-Copies).Count -eq $before) "$(@(Received-Copies).Count) file(s)"
}
finally {
  if ($engineA) { Stop-Process -Id $engineA.Id -Force -ErrorAction SilentlyContinue }
  if ($engineB) { Stop-Process -Id $engineB.Id -Force -ErrorAction SilentlyContinue }
  Get-CimInstance Win32_Process -Filter "Name = '$helperName'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -match [regex]::Escape($rootA) -or $_.CommandLine -match [regex]::Escape($rootB) } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
  Start-Sleep -Milliseconds 700
  if (-not $KeepData) {
    Remove-Item $rootA -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item $rootB -Recurse -Force -ErrorAction SilentlyContinue
  }
}

Write-Host ''
if ($failures -eq 0) { Write-Host 'share interrupt e2e: all checks passed' -ForegroundColor Green; exit 0 }
Write-Host ("share interrupt e2e: {0} check(s) failed" -f $failures) -ForegroundColor Red
exit 1
