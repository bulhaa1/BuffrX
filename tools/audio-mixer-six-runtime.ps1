# Six-stream editor mixer verification against a frozen private runtime.
#
#   powershell -File tools/audio-mixer-six-runtime.ps1 -Phase six|mix
#
# Nothing here touches a running BuffrX. The engine and the editor are this script's own PIDs, on their
# own BUFFRX_PIPE and BUFFRX_DATA_ROOT, started from a frozen copy of the runtime, so no live DLL is
# written. Phase six builds a disposable 6-stream aggregate (mix + microphone + four applications) from
# the silent Six Tracks fixture through the engine's own export, which binds the sidecar to the output's
# real bytes; the fixture itself is never altered. Both files it creates are removed at the end.
param(
  [Parameter(Mandatory = $true)][ValidateSet('six', 'mix')][string]$Phase,
  [string]$FrozenRoot = '',
  [string]$DataRoot = '',
  [int]$Port = 9349,
  [switch]$KeepWindow,
  [switch]$KeepMedia
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $FrozenRoot) { $FrozenRoot = Join-Path $root 'work\frozen-audio-runtime' }
if (-not $DataRoot) { $DataRoot = Join-Path $root 'work\frozen-audio-data' }
$bin = Join-Path $FrozenRoot 'bin\64bit'
$engineExe = Join-Path $bin 'buffrx-engine.exe'
$editorExe = Join-Path $bin 'buffrx-ui.exe'
$artifacts = Join-Path $root 'work\audio-review-artifacts'
$clips = Join-Path $DataRoot 'clips'
$recordings = Join-Path $clips 'Recordings'
$sixTracks = Join-Path $recordings 'Six Tracks.mp4'
$aggregate = Join-Path $recordings 'Six Aggregate.mp4'
$fixture = if ($Phase -eq 'six') { $aggregate } else { Join-Path $recordings 'Incomplete Mix.mp4' }
$work = Join-Path $root 'work\audio-six-probe'
$pipe = "buffrx-audio-six-$PID"
New-Item -ItemType Directory -Force -Path $artifacts, $work | Out-Null

if (-not (Test-Path $engineExe) -or -not (Test-Path $editorExe)) { throw "the frozen runtime is missing: $FrozenRoot" }
if (-not (Test-Path (Join-Path $recordings 'Six Tracks.mp4'))) { throw "the Six Tracks fixture is missing: $sixTracks" }

# Every process this script starts logs and reads under the disposable root, never the user's profile.
$env:BUFFRX_DATA_ROOT = $DataRoot
$env:BUFFRX_PIPE = $pipe
$env:BUFFRX_RUNTIME_ROOT = $FrozenRoot

# The engine's own media identity (src/engine/audio_layout.cpp): FNV-1a 64 over the first and last
# 16 KiB, with the offset basis the engine uses (1469598103934665603), the size and the FILETIME ticks.
if (-not ('BxMediaIdentity' -as [type])) {
  Add-Type -TypeDefinition @'
using System; using System.IO;
public static class BxMediaIdentity {
  public static ulong Fnv(string path, long offset, int count) {
    ulong h = 1469598103934665603UL;
    using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite)) {
      fs.Seek(offset, SeekOrigin.Begin);
      var buf = new byte[count]; int read = 0;
      while (read < count) { int n = fs.Read(buf, read, count - read); if (n <= 0) break; read += n; }
      for (int i = 0; i < read; i++) { unchecked { h ^= buf[i]; h *= 1099511628211UL; } }
      return h;
    }
  }
  public static string Hex(ulong v) { return v.ToString("x16"); }
}
'@
}

function Measure-Identity([string]$file) {
  $item = Get-Item -LiteralPath $file
  $size = [int64]$item.Length
  $headLen = [int][Math]::Min($size, [int64]16384)
  $tailLen = if ($size -gt 16384) { [int][Math]::Min($size - 16384, [int64]16384) } else { 0 }
  $head = [BxMediaIdentity]::Fnv($file, 0, $headLen)
  $tail = if ($tailLen -gt 0) { [BxMediaIdentity]::Fnv($file, $size - $tailLen, $tailLen) } else { [uint64]0 }
  [pscustomobject]@{
    size = $size
    mtime = [int64]$item.LastWriteTimeUtc.ToFileTimeUtc()
    head = [BxMediaIdentity]::Hex($head)
    tail = [BxMediaIdentity]::Hex($tail)
  }
}

function Write-BoundSidecar([string]$file, [object[]]$tracks) {
  $id = Measure-Identity $file
  $doc = [ordered]@{
    version = 2
    generator = 'BuffrX'
    media = [ordered]@{ size = $id.size; mtime = $id.mtime; head = $id.head; tail = $id.tail }
    tracks = @($tracks | ForEach-Object {
      [ordered]@{ index = $_.index; role = $_.role; title = $_.title; sources = @($_.sources) }
    })
  }
  [IO.File]::WriteAllText("$file.buffrx-audio.json", ($doc | ConvertTo-Json -Depth 8), (New-Object System.Text.UTF8Encoding($false)))
  $id
}

function Invoke-Engine([string[]]$arguments) {
  # A native engine writes its log to stderr; with Stop in force PowerShell would treat that as a failure.
  $previous = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try { & $engineExe @arguments 2>&1 | ForEach-Object { "$_" } } finally { $ErrorActionPreference = $previous }
}

function Read-MediaInfo([string]$file) {
  $raw = Invoke-Engine @('--media-info', $file)
  $line = @($raw | Where-Object { $_.Trim().StartsWith('{') } | Select-Object -Last 1)
  if (-not $line) { throw "no JSON from --media-info $file" }
  $line | ConvertFrom-Json
}

# The identity writer is proven against a sidecar the product itself wrote before it is used at all.
$known = Join-Path $recordings 'Desktop Record.mp4'
if (Test-Path $known) {
  $expected = Get-Content -Raw "$known.buffrx-audio.json" | ConvertFrom-Json
  $mine = Measure-Identity $known
  $same = $mine.size -eq $expected.media.size -and $mine.mtime -eq $expected.media.mtime -and $mine.head -eq $expected.media.head -and $mine.tail -eq $expected.media.tail
  "identity writer matches the product's own sidecar: $same"
  if (-not $same) { throw 'the identity writer does not match the engine contract' }
}

$labels = @(
  [pscustomobject]@{ index = 1; role = 'mix'; title = 'BuffrX Mix'; sources = @('microphone', 'app:Discord.exe', 'app:chrome.exe', 'app:Spotify.exe', 'app:game') },
  [pscustomobject]@{ index = 2; role = 'microphone'; title = 'BuffrX Microphone'; sources = @('microphone') },
  [pscustomobject]@{ index = 3; role = 'application'; title = 'Discord'; sources = @('app:Discord.exe') },
  [pscustomobject]@{ index = 4; role = 'application'; title = 'Google Chrome'; sources = @('app:chrome.exe') },
  [pscustomobject]@{ index = 5; role = 'application'; title = 'Desktop Secondary Display Capture With A Very Long Name'; sources = @('app:Spotify.exe') },
  [pscustomobject]@{ index = 6; role = 'application'; title = 'Game'; sources = @('app:game') }
)

function Build-Aggregate {
  $source = Join-Path $work 'six-source.mp4'
  Copy-Item -LiteralPath $sixTracks -Destination $source -Force
  $id = Write-BoundSidecar $source $labels
  "source sidecar bound: size=$($id.size) mtime=$($id.mtime) head=$($id.head) tail=$($id.tail)"
  # A gain change makes the export rebuild track 1 from the stems (audio_mix_plan recompute) instead of
  # copying it, and the export writes its own sidecar bound to the output's final bytes.
  $job = Join-Path $work 'six-export.json'
  @{ input = $source; output = $aggregate; volumes = @(1, 1, 1, 1, 1, 0.5) } | ConvertTo-Json -Depth 4 |
    Set-Content -LiteralPath $job -Encoding UTF8
  $export = Invoke-Engine @('--media-export', $job)
  $result = @($export | Where-Object { $_ -match '^EXPORT_' } | Select-Object -Last 1)
  "engine export: $result"
  if ($result -notmatch '^EXPORT_OK') { throw "the export failed: $result" }
  if (-not (Test-Path "$aggregate.buffrx-audio.json")) { throw 'the export wrote no sidecar beside the aggregate' }
}

if ($Phase -eq 'six') {
  if (Test-Path $aggregate) { Remove-Item -Force $aggregate -ErrorAction SilentlyContinue }
  if (Test-Path "$aggregate.buffrx-audio.json") { Remove-Item -Force "$aggregate.buffrx-audio.json" -ErrorAction SilentlyContinue }
  Build-Aggregate
  $info = Read-MediaInfo $aggregate
  "engine sees the aggregate: streams=$($info.audio.Count) buffrx=$($info.audio_buffrx) aggregate=$($info.audio_aggregate) complete=$($info.audio_complete)"
  "roles=$((@($info.audio) | ForEach-Object { $_.role }) -join ',')"
  if ($info.audio.Count -ne 6 -or -not $info.audio_buffrx -or -not $info.audio_aggregate -or -not $info.audio_complete) {
    throw 'the engine does not accept the built aggregate as a bound, complete six-stream layout'
  }
  # Negative control: the same bytes at another name (same stream count, a different mtime) must be an
  # import, or the identity check is not being applied at all.
  $negative = Join-Path $work 'identity-negative.mp4'
  Copy-Item -LiteralPath $aggregate -Destination $negative -Force
  Copy-Item -LiteralPath "$aggregate.buffrx-audio.json" -Destination "$negative.buffrx-audio.json" -Force
  (Get-Item $negative).LastWriteTimeUtc = (Get-Item $negative).LastWriteTimeUtc.AddSeconds(2)
  $negInfo = Read-MediaInfo $negative
  "identity negative control (same streams, different bytes/time): buffrx=$($negInfo.audio_buffrx)"
  if ($negInfo.audio_buffrx) { throw 'a sidecar bound to other bytes was accepted: identity is not enforced' }
  Remove-Item -Force $negative, "$negative.buffrx-audio.json" -ErrorAction SilentlyContinue
}

if (-not (Test-Path $fixture)) { throw "the fixture is missing: $fixture" }

$env:BUFFRX_WEBVIEW_DEBUG_PORT = "$Port"
$env:BUFFRX_NO_PREVIEW = '1'

$engine = $null
$editor = $null
try {
  $engine = Start-Process -FilePath $engineExe -PassThru -WindowStyle Hidden -WorkingDirectory $bin `
    -RedirectStandardOutput (Join-Path $work 'probe-out.txt') -RedirectStandardError (Join-Path $work 'probe-err.txt')
  $hello = $null
  for ($i = 0; $i -lt 40 -and -not $hello; $i++) {
    Start-Sleep -Milliseconds 250
    try { $hello = & powershell -NoProfile -File (Join-Path $root 'tools\engine-ask.ps1') -Command 'hello' -Arguments '{}' -TimeoutMs 2000 2>$null } catch { $hello = $null }
  }
  if (-not ($hello -match '"protocol"')) { throw 'the frozen engine did not answer on its pipe' }

  $editor = Start-Process -FilePath $editorExe -PassThru -WorkingDirectory $bin -ArgumentList @('--editor', ('"' + $fixture + '"'))
  "engine PID $($engine.Id), editor PID $($editor.Id), fixture $fixture"

  $node = @(
    (Join-Path $root 'tools\audio-mixer-six-probe.mjs'),
    "--phase=$Phase",
    "--port=$Port",
    "--editor-pid=$($editor.Id)",
    "--data=$DataRoot",
    "--clips=$clips",
    "--artifacts=$artifacts"
  )
  & node $node
  $code = $LASTEXITCODE
  if ($editor) { $editor.WaitForExit(8000) | Out-Null }
  Start-Sleep -Milliseconds 1500
  $leftMedia = @(Get-CimInstance Win32_Process -Filter "Name='buffrx-engine.exe'" -ErrorAction SilentlyContinue | Where-Object { $_.CommandLine -match '--media-audio' -and $_.CommandLine -match [regex]::Escape($DataRoot) })
  if ($leftMedia.Count -gt 0) {
    Write-Host ("  FAIL a media child process outlived the editor: " + (($leftMedia | Select-Object -ExpandProperty ProcessId) -join ', '))
    if ($code -eq 0) { $code = 1 }
  } else {
    Write-Host '  ok   no media child process left after the editor'
  }
  $leftEditor = @(Get-CimInstance Win32_Process -Filter "Name='buffrx-ui.exe'" -ErrorAction SilentlyContinue | Where-Object { $editor -and $_.ProcessId -eq $editor.Id })
  if ($leftEditor.Count -gt 0) {
    Write-Host ("  FAIL the editor process outlived its window: " + (($leftEditor | Select-Object -ExpandProperty ProcessId) -join ', '))
    if ($code -eq 0) { $code = 1 }
  } else {
    Write-Host '  ok   the editor process exited with its window'
  }
  "probe exit $code"
  exit $code
}
finally {
  if ($editor -and -not $KeepWindow) { Stop-Process -Id $editor.Id -Force -ErrorAction SilentlyContinue }
  if ($engine) { Stop-Process -Id $engine.Id -Force -ErrorAction SilentlyContinue }
  Start-Sleep -Milliseconds 500
  if ($Phase -eq 'six' -and -not $KeepMedia) {
    Remove-Item -Force $aggregate -ErrorAction SilentlyContinue
    Remove-Item -Force "$aggregate.buffrx-audio.json" -ErrorAction SilentlyContinue
    Remove-Item -Force (Join-Path $work 'six-source.mp4'), (Join-Path $work 'six-source.mp4.buffrx-audio.json') -ErrorAction SilentlyContinue
  }
  Remove-Item Env:\BUFFRX_DATA_ROOT -ErrorAction SilentlyContinue
  Remove-Item Env:\BUFFRX_PIPE -ErrorAction SilentlyContinue
  Remove-Item Env:\BUFFRX_RUNTIME_ROOT -ErrorAction SilentlyContinue
  Remove-Item Env:\BUFFRX_WEBVIEW_DEBUG_PORT -ErrorAction SilentlyContinue
  Remove-Item Env:\BUFFRX_NO_PREVIEW -ErrorAction SilentlyContinue
}
