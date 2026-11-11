# End-to-end proof of the Timeline engine on real footage, without a game:
# a copied clip becomes a fabricated session chunk, then timeline.get, timeline.thumbs and
# timeline.save are driven over the protocol and their files are checked on disk.
#
# Usage: powershell -File tools/timeline-e2e.ps1 [-SourceClip <file>] [-EngineExe <file>] [-KeepData]
# Exit code 0 = every check passed, 1 = a check failed, 2 = the harness could not start.

param(
  [string]$SourceClip = '',
  [string]$EngineExe = '',
  [switch]$KeepData,
  [int]$Markers = 0
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$engineExe = if ($EngineExe) { $EngineExe } else { Join-Path $root 'runtime\bin\64bit\buffrx-engine.exe' }
if (-not (Test-Path $engineExe)) { throw "engine not built: $engineExe" }

$dataRoot = Join-Path $env:TEMP 'buffrx-timeline-e2e'
. (Join-Path $PSScriptRoot 'probe-guard.ps1')
Assert-ProbeDataRoot $dataRoot | Out-Null
if (Test-Path $dataRoot) { Remove-Item $dataRoot -Recurse -Force }
New-Item -ItemType Directory -Force -Path $dataRoot | Out-Null
$clipsDir = Join-Path $dataRoot 'clips'
New-Item -ItemType Directory -Force -Path $clipsDir | Out-Null
$env:BUFFRX_DATA_ROOT = $dataRoot
$env:BUFFRX_PIPE = 'buffrx-timeline-e2e'

# A short-lived engine of this run's own, so the person's BuffrX is never touched.
$seed = @{ clips_dir = $clipsDir; screenshots_dir = (Join-Path $clipsDir 'Screenshots'); auto_game_capture = $false; timeline_minutes = 30 } | ConvertTo-Json
[System.IO.File]::WriteAllText((Join-Path $dataRoot 'config.json'), $seed, (New-Object System.Text.UTF8Encoding($false)))

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
    public static string Ask(string payload, int ms) {
        var name = Environment.GetEnvironmentVariable("BUFFRX_PIPE");
        try {
            using (var p = new NamedPipeClientStream(".", name, PipeDirection.InOut, PipeOptions.Asynchronous)) {
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
  Write-Host ("  {0,-46} {1} {2}" -f $name, $status, $detail) -ForegroundColor $color
  if (-not $ok) { $script:failures++ }
}

$engine = Start-Process -FilePath $engineExe -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $dataRoot 'engine-out.txt') -RedirectStandardError (Join-Path $dataRoot 'engine-err.txt')
try {
  $ready = $false
  for ($i = 0; $i -lt 30 -and -not $ready; $i++) {
    Start-Sleep -Milliseconds 500
    $hello = [Eng]::Ask('{"id":1,"cmd":"hello"}', 3000)
    if ($hello -match '"protocol"') { $ready = $true }
  }
  Check 'engine answers on its own pipe' $ready
  if (-not $ready) { exit 2 }

  # Pick a real clip long enough for a range, copy it into the library, and let the engine measure it.
  if (-not $SourceClip) {
    $videos = Join-Path ([Environment]::GetFolderPath('MyVideos')) 'BuffrX'
    $candidates = Get-ChildItem $videos -File -Recurse -ErrorAction SilentlyContinue |
      Where-Object { $_.Length -gt 1MB -and $_.Extension -in '.mp4', '.mkv' } |
      Sort-Object Length -Descending
    if ($candidates) { $SourceClip = $candidates[0].FullName }
  }
  Check 'a source clip exists' (($SourceClip -ne '') -and (Test-Path $SourceClip)) $SourceClip
  if (-not $SourceClip -or -not (Test-Path $SourceClip)) { exit 2 }
  $probe = Join-Path $clipsDir 'probe.mp4'
  Copy-Item $SourceClip $probe
  $list = [Eng]::Ask('{"id":2,"cmd":"clips.list"}', 60000)
  $seconds = 0.0
  # The fields of a library entry have no fixed order: read the duration out of the object for probe.mp4.
  foreach ($m in [regex]::Matches($list, '\{[^{}]*"file":"[^"]*probe\.mp4"[^{}]*\}')) {
    if ($m.Value -match '"duration":([0-9.]+)') { $seconds = [double]$Matches[1] }
  }
  Check 'the engine measures the source clip' ($seconds -gt 1.0) ("{0:N2} s" -f $seconds)
  if ($seconds -le 1.0) { exit 2 }

  $sessionId = 'BuffrX 2026-01-01 00-00-00'
  $sessionDir = Join-Path $clipsDir "sessions\$sessionId"
  $mediaDir = Join-Path $sessionDir 'media'
  New-Item -ItemType Directory -Force -Path $mediaDir | Out-Null
  $chunk = Join-Path $mediaDir 'chunk-0.mp4'
  Move-Item $probe $chunk
  $bytes = (Get-Item $chunk).Length
  $nowMs = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
  $nowS = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
  $startedMs = 1000000
  $durationMs = [int]($seconds * 1000)
  $chunkEntry = @{ file = $chunk; bytes = $bytes; start_ms = $startedMs; end_ms = ($startedMs + $durationMs); wall_start_ms = ($nowMs - 60000); wall_end_ms = ($nowMs - 60000 + $durationMs); epoch = 0; published = $true }
  $entry = @{ id = $sessionId; kind = 'timeline'; app_id = ''; profile = ''; started_ms = $startedMs; started_at = $nowS; ended_ms = ($startedMs + $durationMs); closed = $true; chunks = @($chunkEntry); markers = @() }
  [System.IO.File]::WriteAllText((Join-Path $sessionDir 'index.json'), ($entry | ConvertTo-Json -Depth 6), (New-Object System.Text.UTF8Encoding($false)))

  $get = [Eng]::Ask("{`"id`":3,`"cmd`":`"timeline.get`",`"args`":{`"session`":`"$sessionId`"}}", 15000)
  Check 'timeline.get lists the fabricated session' ($get -match '"ok":true' -and $get -match [regex]::Escape($sessionId)) ($get.Substring(0, [Math]::Min(300, $get.Length)))
  Check 'the span ends at the chunk end' ($get -match ('"available_end_ms":' + $durationMs)) "expected $durationMs"

  # A chunk is one piece of a session: a rename would leave the index pointing at a file that is gone.
  $chunkJson = $chunk.Replace('\', '\\')
  $rename = [Eng]::Ask("{`"id`":29,`"cmd`":`"clips.rename`",`"args`":{`"file`":`"$chunkJson`",`"name`":`"hijacked`"}}", 15000)
  Check 'renaming a Timeline chunk is refused' ($rename -match '"ok":false') $rename
  Check 'the refused rename leaves the chunk in place' (Test-Path -LiteralPath $chunk)

  $mark = [Eng]::Ask("{`"id`":30,`"cmd`":`"markers.add`",`"args`":{`"session`":`"$sessionId`",`"at_ms`":500,`"kind`":`"marker`",`"label`":`"test`"}}", 15000)
  Check 'a marker can be added to a closed timeline' ($mark -match '"ok":true')
  $remove = [Eng]::Ask("{`"id`":31,`"cmd`":`"markers.remove`",`"args`":{`"session`":`"$sessionId`",`"at_ms`":500,`"kind`":`"marker`",`"label`":`"test`"}}", 15000)
  Check 'a marker can be removed' ($remove -match '"removed":true')
  $afterRemove = [Eng]::Ask("{`"id`":32,`"cmd`":`"session.get`",`"args`":{`"id`":`"$sessionId`"}}", 15000)
  Check 'the marker removal persists without removing footage' ($afterRemove -match '"marker_count":0' -and $afterRemove -match '"chunk_count":1')

  # Thumbnails: real JPEGs decoded from the real chunk.
  $mid = [int]($durationMs / 2)
  $times = "[0,$mid,$([int]($durationMs - 500))]"
  $thumbs = [Eng]::Ask("{`"id`":4,`"cmd`":`"timeline.thumbs`",`"args`":{`"session`":`"$sessionId`",`"times_ms`":$times}}", 60000)
  $thumbFiles = @()
  foreach ($m in [regex]::Matches($thumbs, '"file":"((?:[^"\\]|\\.)*)"')) {
    $thumbFiles += ($m.Groups[1].Value -replace '\\\\', '\')
  }
  Check 'timeline.thumbs answers ok' ($thumbs -match '"ok":true') $thumbs.Substring(0, [Math]::Min(240, $thumbs.Length))
  Check 'thumbnails were written' ($thumbFiles.Count -ge 1) "$($thumbFiles.Count) files"
  $jpeg = $true
  foreach ($f in $thumbFiles) {
    if (-not (Test-Path $f)) { $jpeg = $false; continue }
    $head = [System.IO.File]::ReadAllBytes($f)[0..2]
    if (-not ($head[0] -eq 0xFF -and $head[1] -eq 0xD8)) { $jpeg = $false }
  }
  Check 'every thumbnail is a JPEG on disk' ($jpeg -and $thumbFiles.Count -ge 1)
  Check 'thumbnails stay inside the session' (($thumbFiles | Where-Object { $_ -notlike "$sessionDir*" }).Count -eq 0)
  $again = [Eng]::Ask("{`"id`":5,`"cmd`":`"timeline.thumbs`",`"args`":{`"session`":`"$sessionId`",`"times_ms`":$times}}", 20000)
  Check 'a repeated strip is served from the cache' ($again -match '"ok":true' -and $again -match '\.jpg')

  # Save a real range: a stream copy of the chunk, keyframe aligned.
  $saveEnd = [Math]::Min(5000, $durationMs - 500)
  $saved = [Eng]::Ask("{`"id`":6,`"cmd`":`"timeline.save`",`"args`":{`"session`":`"$sessionId`",`"start_ms`":0,`"end_ms`":$saveEnd}}", 90000)
  $savedFile = ''
  if ($saved -match '"file":"((?:[^"\\]|\\.)*)"') { $savedFile = $Matches[1] -replace '\\\\', '\' }
  Check 'timeline.save writes a clip' ($saved -match '"ok":true' -and $savedFile -ne '' -and (Test-Path $savedFile)) $saved.Substring(0, [Math]::Min(300, $saved.Length))
  if ($savedFile -and (Test-Path $savedFile)) {
    Check 'the saved clip is not empty' ((Get-Item $savedFile).Length -gt 1024) ("{0} bytes" -f (Get-Item $savedFile).Length)
    $list2 = [Eng]::Ask('{"id":7,"cmd":"clips.list"}', 60000)
    $leaf = Split-Path $savedFile -Leaf
    Check 'the saved clip enters the library' ($list2 -match [regex]::Escape($leaf))
  }

  # A session with hundreds of markers: the index is rewritten on every add, so a cap or a slow
  # read would show here. The session is closed, so no writer is involved.
  if ($Markers -gt 0) {
    $beforeBulk = [Eng]::Ask("{`"id`":39,`"cmd`":`"session.get`",`"args`":{`"id`":`"$sessionId`"}}", 30000)
    $markerBase = if ($beforeBulk -match '"marker_count":(\d+)') { [int]$Matches[1] } else { 0 }
    $expectedMarkers = $markerBase + $Markers
    $addClock = [Diagnostics.Stopwatch]::StartNew()
    $added = 0
    for ($i = 0; $i -lt $Markers; $i++) {
      $at = 500 + $i * 5
      $r = [Eng]::Ask("{`"id`":40,`"cmd`":`"markers.add`",`"args`":{`"session`":`"$sessionId`",`"at_ms`":$at,`"kind`":`"marker`",`"label`":`"bulk $i`"}}", 15000)
      if ($r -match '"ok":true') { $added++ }
    }
    $addClock.Stop()
    Check "bulk add of $Markers markers" ($added -eq $Markers) ("{0} added in {1:N1} s" -f $added, $addClock.Elapsed.TotalSeconds)
    $readClock = [Diagnostics.Stopwatch]::StartNew()
    $bulk = [Eng]::Ask("{`"id`":41,`"cmd`":`"session.get`",`"args`":{`"id`":`"$sessionId`"}}", 30000)
    $readClock.Stop()
    Check 'every marker is in the session reply' ($bulk -match ('"marker_count":' + $expectedMarkers)) ("{0:N1} s, {1:N0} KB" -f $readClock.Elapsed.TotalSeconds, ($bulk.Length / 1KB))
    # Markers live in their own file beside the chunk index (session_markers_path), and only a session
    # written before that file existed keeps them in the index: read the sidecar, fall back to the index.
    $markerFile = Join-Path $sessionDir 'markers.json'
    $onDisk = if (Test-Path $markerFile) {
      Get-Content $markerFile -Raw | ConvertFrom-Json
    } else {
      Get-Content (Join-Path $sessionDir 'index.json') -Raw | ConvertFrom-Json
    }
    Check 'the marker file holds every marker' (@($onDisk.markers).Count -eq $expectedMarkers) ("$(@($onDisk.markers).Count) markers")

    # The engine comes back with no writer: the markers must be the disk's, not memory's.
    Stop-Process -Id $engine.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 800
    $engine = Start-Process -FilePath $engineExe -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $dataRoot 'engine2-out.txt') -RedirectStandardError (Join-Path $dataRoot 'engine2-err.txt')
    $ready = $false
    for ($i = 0; $i -lt 30 -and -not $ready; $i++) {
      Start-Sleep -Milliseconds 500
      $hello = [Eng]::Ask('{"id":42,"cmd":"hello"}', 3000)
      if ($hello -match '"protocol"') { $ready = $true }
    }
    Check 'the engine restarts on the same store' $ready
    $afterRestart = [Eng]::Ask("{`"id`":43,`"cmd`":`"session.get`",`"args`":{`"id`":`"$sessionId`"}}", 30000)
    Check 'every marker survives a restart' ($afterRestart -match ('"marker_count":' + $expectedMarkers))

    $removeClock = [Diagnostics.Stopwatch]::StartNew()
    $removed = 0
    for ($i = 0; $i -lt $Markers; $i++) {
      $at = 500 + $i * 5
      $r = [Eng]::Ask("{`"id`":44,`"cmd`":`"markers.remove`",`"args`":{`"session`":`"$sessionId`",`"at_ms`":$at,`"kind`":`"marker`",`"label`":`"bulk $i`"}}", 15000)
      if ($r -match '"removed":true') { $removed++ }
    }
    $removeClock.Stop()
    Check "bulk remove of $Markers markers" ($removed -eq $Markers) ("{0} removed in {1:N1} s" -f $removed, $removeClock.Elapsed.TotalSeconds)
    $empty = [Eng]::Ask("{`"id`":45,`"cmd`":`"session.get`",`"args`":{`"id`":`"$sessionId`"}}", 30000)
    Check 'no marker is left behind' ($empty -match ('"marker_count":' + $markerBase))
  }

  # A chunk that vanished must stop being footage, not break the listing.
  Remove-Item $chunk -Force
  $gone = [Eng]::Ask("{`"id`":8,`"cmd`":`"timeline.get`",`"args`":{`"session`":`"$sessionId`"}}", 15000)
  Check 'a missing chunk is not named as footage' ($gone -match '"ok":true' -and $gone -notmatch 'chunk-0\.mp4')
  Check 'the span of a session with no footage is empty' ($gone -match '"available_end_ms":0')
  $goneThumbs = [Eng]::Ask("{`"id`":9,`"cmd`":`"timeline.thumbs`",`"args`":{`"session`":`"$sessionId`",`"times_ms`":[0,1000]}}", 15000)
  Check 'thumbs refuse an empty session without failing' ($goneThumbs -match '"ok":true' -and $goneThumbs -notmatch '\.jpg')
}
finally {
  Stop-Process -Id $engine.Id -Force -ErrorAction SilentlyContinue
  Start-Sleep -Milliseconds 500
  if (-not $KeepData) { Remove-Item $dataRoot -Recurse -Force -ErrorAction SilentlyContinue }
}

Write-Host ""
if ($failures -eq 0) { Write-Host 'timeline e2e: all checks passed' -ForegroundColor Green; exit 0 }
Write-Host ("timeline e2e: {0} check(s) failed" -f $failures) -ForegroundColor Red
exit 1
