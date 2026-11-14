# Run from the checkout being checked. CI compares committed changes; local use checks edits.
# -BaseRef <commit-or-ref> compares a local branch with its merge base.
param([string]$BaseRef = '')

$ErrorActionPreference = 'Stop'
$separator = '...'
$initialPush = $false
if (-not $BaseRef -and $env:GITHUB_ACTIONS -eq 'true') {
  if (-not $env:GITHUB_EVENT_PATH -or -not (Test-Path -LiteralPath $env:GITHUB_EVENT_PATH)) {
    throw 'GitHub event data is missing; cannot determine the committed whitespace range.'
  }
  $event = Get-Content -LiteralPath $env:GITHUB_EVENT_PATH -Raw | ConvertFrom-Json
  switch ($env:GITHUB_EVENT_NAME) {
    'pull_request' { $BaseRef = $event.pull_request.base.sha }
    'push' {
      $BaseRef = $event.before
      $separator = '..'
      $initialPush = $BaseRef -match '^0{40}$'
    }
    default { throw "Unsupported whitespace-check event: $env:GITHUB_EVENT_NAME" }
  }
  if (-not $BaseRef) { throw 'GitHub event contains no comparison base.' }
}

function Invoke-Whitespace([string[]]$GitArguments) {
  & git -c core.safecrlf=false @GitArguments
  if ($LASTEXITCODE -ne 0) { throw 'Whitespace check failed; inspect the Git errors above.' }
}

if ($initialPush) {
  Invoke-Whitespace -GitArguments @('diff-tree', '--root', '--check', '-r', 'HEAD')
} elseif ($BaseRef) {
  $resolvedBase = & git rev-parse --verify --end-of-options "${BaseRef}^{commit}"
  if ($LASTEXITCODE -ne 0) { throw 'Comparison base is unavailable. Fetch the base/history before checking whitespace.' }
  Invoke-Whitespace -GitArguments @('diff', '--check', "${resolvedBase}${separator}HEAD")
} else {
  Invoke-Whitespace -GitArguments @('diff', '--check')
  Invoke-Whitespace -GitArguments @('diff', '--cached', '--check')
}
Write-Host 'Whitespace checks passed.'
