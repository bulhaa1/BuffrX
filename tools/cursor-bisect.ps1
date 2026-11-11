# Which part of BuffrX makes the pointer feel slow? Starts a throwaway BuffrX (its own data root and pipe, a copy of your settings with
# one thing changed) in turn, opens the compact panel, and asks you to move the pointer over the panel's header and around the desktop.
# Close the BuffrX you use first. Nothing here touches your own settings or library.
#   powershell -File tools\cursor-bisect.ps1
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$bin = Join-Path $root 'runtime\bin\64bit'
$user = Join-Path $env:LOCALAPPDATA 'BuffrX\config.json'
. (Join-Path $PSScriptRoot 'probe-lib.ps1')
$modes = @(
  @{ name = '1. nothing capturing (buffer off, Timeline off)'; set = @{ start_buffering = $false; timeline_minutes = 0 } },
  @{ name = '2. buffer only (Timeline off)';                   set = @{ start_buffering = $true;  timeline_minutes = 0 } },
  @{ name = '3. buffer and Timeline, as your settings';        set = @{} },
  @{ name = '4. as 3, but capture through Windows Graphics Capture'; set = @{ display_method = 'wgc' } },
  @{ name = '5. as 3, at 60 frames a second';                  set = @{ capture_fps = 60 } },
  @{ name = '6. as 3, without the pointer in the picture';     set = @{ capture_cursor = $false } }
)
if (Get-Process buffrx-engine -ErrorAction SilentlyContinue) { throw 'Close BuffrX first (tray, Quit), then run this again.' }
$base = if (Test-Path $user) { Get-Content $user -Raw | ConvertFrom-Json } else { [pscustomobject]@{} }
$results = @()
foreach ($mode in $modes) {
  $dir = Join-Path $env:TEMP ('buffrx-bisect-' + [guid]::NewGuid().ToString('N').Substring(0, 6))
  New-Item -ItemType Directory (Join-Path $dir 'clips') | Out-Null
  $config = $base | ConvertTo-Json -Depth 8 | ConvertFrom-Json
  $config | Add-Member -NotePropertyName clips_dir -NotePropertyValue (Join-Path $dir 'clips') -Force
  foreach ($key in $mode.set.Keys) { $config | Add-Member -NotePropertyName $key -NotePropertyValue $mode.set[$key] -Force }
  $config | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $dir 'config.json') -Encoding utf8
  $env:BUFFRX_PIPE = 'buffrx-bisect'; $env:BUFFRX_DATA_ROOT = $dir; $env:BUFFRX_RUNTIME_ROOT = (Join-Path $root 'runtime'); $env:PATH = "$bin;$env:PATH"
  $process = Start-Process (Join-Path $bin 'buffrx-engine.exe') -WorkingDirectory $bin -PassThru -WindowStyle Hidden
  try {
    Start-Sleep -Seconds 5
    $pipe = New-Object PanelPipe 'buffrx-bisect', 5000
    $null = $pipe.Ask('ui.show', '{"view":"compact"}')
    Write-Host ''
    Write-Host ("MODE " + $mode.name) -ForegroundColor Cyan
    Write-Host 'Move the pointer over the compact panel header and around the desktop for about 15 seconds.'
    Start-Sleep -Seconds 15
    $answer = Read-Host 'Did the pointer feel slow? (y = slow, n = fine)'
    $results += [pscustomobject]@{ Mode = $mode.name; Slow = $answer }
  } finally {
    Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
  }
}
Write-Host ''
$results | Format-Table -AutoSize
