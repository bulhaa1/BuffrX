# Regression fixtures are temporary Git repositories, never the contributor's branch.
$ErrorActionPreference = 'Stop'
$checker = Join-Path $PSScriptRoot 'check-whitespace.ps1'
$shell = (Get-Process -Id $PID).Path
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('buffrx-whitespace-' + [Guid]::NewGuid().ToString('N'))
$oldActions = $env:GITHUB_ACTIONS
$oldEventName = $env:GITHUB_EVENT_NAME
$oldEventPath = $env:GITHUB_EVENT_PATH
New-Item -ItemType Directory -Path $fixture | Out-Null
$eventFile = Join-Path $fixture 'event.json'
$checks = 0

function Git-Fixture([string[]]$GitArguments) {
  $result = & git @GitArguments
  if ($LASTEXITCODE -ne 0) { throw 'Git fixture setup failed.' }
  return $result
}

function Assert-Check([string]$Name, [bool]$Pass, [string]$Event = '', [string]$Base = '', [string]$ExplicitBase = '') {
  $env:GITHUB_ACTIONS = if ($Event) { 'true' } else { $null }
  $env:GITHUB_EVENT_NAME = $Event
  $env:GITHUB_EVENT_PATH = $eventFile
  $payload = if ($Event -eq 'pull_request') { @{pull_request = @{base = @{sha = $Base}}} } else { @{before = $Base} }
  [IO.File]::WriteAllText($eventFile, ($payload | ConvertTo-Json -Depth 5))
  $arguments = @('-NoProfile', '-File', $checker)
  if ($ExplicitBase) { $arguments += @('-BaseRef', $ExplicitBase) }
  $preference = $ErrorActionPreference
  try {
    $ErrorActionPreference = 'Continue'
    $output = & $shell @arguments 2>&1
    $exitCode = $LASTEXITCODE
  } finally { $ErrorActionPreference = $preference }
  if (($exitCode -eq 0) -ne $Pass) { throw "Unexpected result for ${Name}: exit $exitCode`n$($output -join "`n")" }
  $script:checks++
  Write-Host "PASS: $Name"
}

Push-Location $fixture
try {
  Git-Fixture -GitArguments @('init', '--quiet') | Out-Null
  Git-Fixture -GitArguments @('config', 'user.name', 'Whitespace fixture') | Out-Null
  Git-Fixture -GitArguments @('config', 'user.email', 'fixture@users.noreply.github.com') | Out-Null
  Git-Fixture -GitArguments @('config', 'commit.gpgsign', 'false') | Out-Null
  Git-Fixture -GitArguments @('config', 'core.autocrlf', 'false') | Out-Null
  [IO.File]::WriteAllText((Join-Path $fixture 'sample.txt'), "clean`n")
  Git-Fixture -GitArguments @('add', '--', 'sample.txt') | Out-Null
  Git-Fixture -GitArguments @('commit', '--quiet', '-m', 'Clean fixture') | Out-Null
  $base = Git-Fixture -GitArguments @('rev-parse', 'HEAD')
  Assert-Check -Name 'clean local checkout' -Pass $true
  [IO.File]::WriteAllText((Join-Path $fixture 'sample.txt'), "bad  `n")
  Assert-Check -Name 'unstaged trailing whitespace' -Pass $false
  Git-Fixture -GitArguments @('add', '--', 'sample.txt') | Out-Null
  Assert-Check -Name 'staged trailing whitespace' -Pass $false
  Git-Fixture -GitArguments @('commit', '--quiet', '-m', 'Whitespace regression fixture') | Out-Null
  Assert-Check -Name 'committed PR whitespace in clean checkout' -Pass $false -Event 'pull_request' -Base $base
  Assert-Check -Name 'committed push whitespace in clean checkout' -Pass $false -Event 'push' -Base $base
  Assert-Check -Name 'explicit comparison base' -Pass $false -ExplicitBase $base
  Assert-Check -Name 'unavailable base fails closed' -Pass $false -Event 'push' -Base ('f' * 40)
  [IO.File]::WriteAllText((Join-Path $fixture 'sample.txt'), "clean again`n")
  Git-Fixture -GitArguments @('add', '--', 'sample.txt') | Out-Null
  Git-Fixture -GitArguments @('commit', '--quiet', '-m', 'Clean result fixture') | Out-Null
  Assert-Check -Name 'clean committed PR range' -Pass $true -Event 'pull_request' -Base $base
  Git-Fixture -GitArguments @('checkout', '--quiet', '--detach', $base) | Out-Null
  Assert-Check -Name 'initial push root commit' -Pass $true -Event 'push' -Base ('0' * 40)
  Write-Host "Whitespace regression checks passed: $checks"
} finally {
  Pop-Location
  $env:GITHUB_ACTIONS = $oldActions
  $env:GITHUB_EVENT_NAME = $oldEventName
  $env:GITHUB_EVENT_PATH = $oldEventPath
  $resolved = [IO.Path]::GetFullPath($fixture)
  $tempPrefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
  if (-not $resolved.StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture cleanup escaped TEMP.' }
  Remove-Item -LiteralPath $resolved -Recurse -Force
}
