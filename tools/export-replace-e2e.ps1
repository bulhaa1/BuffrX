param([Parameter(Mandatory = $true)][string]$SourceClip, [string]$EngineExe = '')

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$dir = Join-Path $env:TEMP ("buffrx-export-test-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $dir | Out-Null
$env:BUFFRX_DATA_ROOT = $dir
$engine = if ($EngineExe) { $EngineExe } else { Join-Path $root 'runtime\bin\64bit\buffrx-engine.exe' }
$clip = Join-Path $dir 'clip.mp4'
$jobPath = Join-Path $dir 'job.json'
try {
  @{ clips_dir = $dir; screenshots_dir = (Join-Path $dir 'shots') } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $dir 'config.json')
  foreach ($start in @(0, 0.3)) {
    Copy-Item -LiteralPath $SourceClip -Destination $clip -Force
    $job = @{ input = $clip; output = $clip; start = $start; end = 2; replace_original = $true } | ConvertTo-Json
    [System.IO.File]::WriteAllText($jobPath, $job)
    & $engine --media-export $jobPath
    if ($LASTEXITCODE -ne 0) { throw "Replace failed at $start seconds" }
    & $engine --media-info $clip
    if ($LASTEXITCODE -ne 0) { throw "Replacement could not be read at $start seconds" }
    if (Get-ChildItem -LiteralPath $dir -Filter 'clip.mp4.old-*' -File) { throw 'The original was left beside the replacement instead of being recycled' }
  }
  Write-Host 'export replace e2e: all checks passed'
} finally {
  Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
}
