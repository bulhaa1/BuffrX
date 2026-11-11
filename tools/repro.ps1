# Repeats one command against a running engine until the engine is gone.
# Usage: powershell -File tools\repro.ps1 -Command 'clips.list' -Times 40
param(
  [string]$Command = 'clips.list',
  [int]$Times = 40,
  [string]$DataRoot = "$env:TEMP\buffrx-repro"
)
$ErrorActionPreference = 'Continue'
. (Join-Path $PSScriptRoot 'probe-guard.ps1')
# This run gets its own data root and pipe name, so no engine already running is touched: it is never
# killed here, and the only process the script stops at the end is the one it started itself.
Assert-ProbeDataRoot $DataRoot | Out-Null
Remove-Item $DataRoot -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path "$DataRoot\clips" -Force | Out-Null

# A real clip to work on: a small one from the user's library.
$clip = Get-ChildItem "$env:USERPROFILE\Videos\BuffrX" -Recurse -Filter *.mp4 -ErrorAction SilentlyContinue |
  Where-Object { $_.Length -gt 2MB -and $_.Length -lt 200MB } | Sort-Object Length | Select-Object -First 1
if ($clip) {
  Copy-Item $clip.FullName "$DataRoot\clips\Replay test.mp4" -Force
  "clip: $($clip.Name) ($([math]::Round($clip.Length/1MB)) MB)"
} else {
  "no clip found - the commands will run against an empty library"
}

# The library is this run's own folder, not the user's.
$config = @{
  clips_dir = "$DataRoot\clips"
  screenshots_dir = "$DataRoot\clips\Screenshots"
  auto_game_capture = $false
} | ConvertTo-Json
[System.IO.File]::WriteAllText("$DataRoot\config.json", $config, (New-Object System.Text.UTF8Encoding($false)))
$env:BUFFRX_PIPE = "buffrx-repro"

$env:BUFFRX_DATA_ROOT = $DataRoot
$engine = Start-Process runtime\bin\64bit\buffrx-engine.exe -PassThru -WindowStyle Hidden `
  -RedirectStandardOutput "$DataRoot\out.txt" -RedirectStandardError "$DataRoot\err.txt"
Start-Sleep -Seconds 12
if (-not (Get-Process -Id $engine.Id -ErrorAction SilentlyContinue)) {
  "the engine did not even start"
  Get-Content "$DataRoot\logs\engine.log" -Tail 5 -ErrorAction SilentlyContinue | ForEach-Object { "  [$_]" }
  exit 1
}
"the engine is up"
for ($i = 1; $i -le $Times; $i++) {
  $reply = powershell -File tools\engine-ask.ps1 $Command '{}' 2>&1 | Out-String
  $alive = [bool](Get-Process -Id $engine.Id -ErrorAction SilentlyContinue)
  $short = $reply.Trim()
  if ($short.Length -gt 90) { $short = $short.Substring(0, 90) }
  "run $i alive=$alive : $short"
  if (-not $alive) { break }
}
"--- the engine's log:"
Get-Content "$DataRoot\logs\engine.log" -Tail 8 -ErrorAction SilentlyContinue | ForEach-Object { "  [$_]" }
"--- the stderr's tail:"
Get-Content "$DataRoot\err.txt" -Tail 6 -ErrorAction SilentlyContinue | ForEach-Object { "  $_" }
Stop-Process -Id $engine.Id -Force -ErrorAction SilentlyContinue
