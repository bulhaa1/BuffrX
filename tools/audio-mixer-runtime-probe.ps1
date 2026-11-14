# Drives the real editor (Tauri shell + WebView2) against a frozen private runtime and disposable data,
# and runs the audio mixer probe in tools/audio-mixer-cdp-probe.mjs for one fixture.
#
#   powershell -File tools/audio-mixer-runtime-probe.ps1 -Phase agg|import|mix|long
#
# Nothing here touches a running BuffrX: the engine and the editor are this script's own PIDs, on their
# own BUFFRX_PIPE and BUFFRX_DATA_ROOT. The live runtime\bin\64bit DLLs are never written.
param(
  [Parameter(Mandatory = $true)][ValidateSet('agg', 'import', 'mix', 'long')][string]$Phase,
  [string]$FrozenRoot = '',
  [string]$DataRoot = '',
  [int]$Port = 9347,
  [switch]$KeepWindow
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
$pipe = "buffrx-audio-mix-$PID"
New-Item -ItemType Directory -Force -Path $artifacts | Out-Null

$fixtures = @{ agg = 'Desktop Record.mp4'; import = 'Imported Pair.mp4'; mix = 'Incomplete Mix.mp4'; long = 'Long Desktop Name.mp4' }
$clip = Join-Path $clips ('Recordings\' + $fixtures[$Phase])
if (-not (Test-Path $clip)) { throw "the fixture is missing: $clip (run the fixture step first)" }
if (-not (Test-Path $engineExe) -or -not (Test-Path $editorExe)) { throw "the frozen runtime is missing: $FrozenRoot" }

# A cache that cannot be written makes --media-audio fail on purpose: the preview must fall back to the
# mix and offer Try again, not play half the tracks in silence.
$blockCache = Join-Path $DataRoot 'cache'
if ($Phase -eq 'long') {
  if (Test-Path $blockCache) { Remove-Item -Recurse -Force $blockCache -ErrorAction SilentlyContinue }
  New-Item -ItemType File -Force -Path $blockCache | Out-Null
}

$env:BUFFRX_DATA_ROOT = $DataRoot
$env:BUFFRX_PIPE = $pipe
$env:BUFFRX_RUNTIME_ROOT = $FrozenRoot
$env:BUFFRX_WEBVIEW_DEBUG_PORT = "$Port"
$env:BUFFRX_NO_PREVIEW = '1'

$engine = $null
$editor = $null
try {
  $engine = Start-Process -FilePath $engineExe -PassThru -WindowStyle Hidden -WorkingDirectory $bin `
    -RedirectStandardOutput (Join-Path $DataRoot 'probe-out.txt') -RedirectStandardError (Join-Path $DataRoot 'probe-err.txt')
  $hello = $null
  for ($i = 0; $i -lt 40 -and -not $hello; $i++) {
    Start-Sleep -Milliseconds 250
    try { $hello = & powershell -NoProfile -File (Join-Path $root 'tools\engine-ask.ps1') -Command 'hello' -Arguments '{}' -TimeoutMs 2000 2>$null } catch { $hello = $null }
  }
  if (-not ($hello -match '"protocol"')) { throw 'the frozen engine did not answer on its pipe' }

  $editor = Start-Process -FilePath $editorExe -PassThru -WorkingDirectory $bin -ArgumentList @('--editor', ('"' + $clip + '"'))
  "engine PID $($engine.Id), editor PID $($editor.Id), clip $clip"

  $node = @(
    (Join-Path $root 'tools\audio-mixer-cdp-probe.mjs'),
    "--phase=$Phase",
    "--port=$Port",
    "--data=$DataRoot",
    "--engine=$engineExe",
    "--clips=$clips",
    "--artifacts=$artifacts"
  )
  & node $node
  $code = $LASTEXITCODE
  if ($editor) { $editor.WaitForExit(6000) | Out-Null }
  Start-Sleep -Milliseconds 1500
  $leftMedia = @(Get-CimInstance Win32_Process -Filter "Name='buffrx-engine.exe'" -ErrorAction SilentlyContinue | Where-Object { $_.CommandLine -match '--media-audio' -and $_.CommandLine -match [regex]::Escape($DataRoot) })
  if ($leftMedia.Count -gt 0) {
    Write-Host ("  FAIL a media child process outlived the editor: " + (($leftMedia | Select-Object -ExpandProperty ProcessId) -join ', '))
    if ($code -eq 0) { $code = 1 }
  } else {
    Write-Host '  ok   no media child process left after the editor'
  }
  $leftEditor = @(Get-CimInstance Win32_Process -Filter "Name='buffrx-ui.exe'" -ErrorAction SilentlyContinue | Where-Object { $editor -and $_.ProcessId -eq $editor.Id })
  if ($Phase -ne 'long' -and $leftEditor.Count -gt 0) {
    Write-Host ("  FAIL the editor process outlived its window: " + (($leftEditor | Select-Object -ExpandProperty ProcessId) -join ', '))
    if ($code -eq 0) { $code = 1 }
  }
  "probe exit $code"
  exit $code
}
finally {
  if ($Phase -eq 'long' -and (Test-Path $blockCache -PathType Leaf)) { Remove-Item -Force $blockCache -ErrorAction SilentlyContinue }
  if ($editor -and -not $KeepWindow) { Stop-Process -Id $editor.Id -Force -ErrorAction SilentlyContinue }
  if ($engine) { Stop-Process -Id $engine.Id -Force -ErrorAction SilentlyContinue }
  Remove-Item Env:\BUFFRX_DATA_ROOT -ErrorAction SilentlyContinue
  Remove-Item Env:\BUFFRX_PIPE -ErrorAction SilentlyContinue
  Remove-Item Env:\BUFFRX_RUNTIME_ROOT -ErrorAction SilentlyContinue
  Remove-Item Env:\BUFFRX_WEBVIEW_DEBUG_PORT -ErrorAction SilentlyContinue
  Remove-Item Env:\BUFFRX_NO_PREVIEW -ErrorAction SilentlyContinue
}
