<#
.SYNOPSIS
Run the same software checks as pull-request CI. No area means all software checks.
.EXAMPLE
powershell -File tools/verify.ps1 -Area Native
#>
param([ValidateSet('Docs', 'Editor', 'Native', 'Build', 'All')][string]$Area = 'All')

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

function Invoke-Check([string]$Name, [scriptblock]$Action) {
  Write-Host "== $Name"
  $global:LASTEXITCODE = 0
  & $Action
  if ($LASTEXITCODE -ne 0) { throw "$Name failed (exit $LASTEXITCODE)" }
}

Push-Location $root
try {
  if ($Area -eq 'Docs' -or $Area -eq 'All') {
    # These are the public onboarding documents, not ignored internal notes.
    $documents = @('README.md', 'CONTRIBUTING.md', 'DEVELOPMENT.md', 'DEVELOPMENT_LINUX.md', 'CONTRIBUTOR_AGENT.md', 'docs/protocol.md')
    foreach ($document in $documents) {
      if (-not (Test-Path -LiteralPath $document)) { throw "Missing document: $document" }
      $content = Get-Content -LiteralPath $document -Raw
      foreach ($link in [regex]::Matches($content, '\]\(([^\s)]+)\)')) {
        $target = $link.Groups[1].Value
        if ($target -match '^(https?://|mailto:|#)') { continue }
        $target = [Uri]::UnescapeDataString(($target -split '#')[0])
        if (-not (Test-Path -LiteralPath (Join-Path (Split-Path -Parent (Join-Path $root $document)) $target))) {
          throw "Broken local link in ${document}: $target"
        }
      }
    }
    Invoke-Check 'whitespace' { git -c core.safecrlf=false diff --check }
    Write-Host 'Public document links passed.'
  }

  if ($Area -eq 'Editor' -or $Area -eq 'All') {
    if (-not (Get-Command node -ErrorAction SilentlyContinue)) { throw 'Install Node.js 24, then run: npm ci --prefix ui' }
    if (-not (Test-Path 'ui/node_modules')) { throw 'Run: npm ci --prefix ui' }
    Invoke-Check 'editor typecheck' { npm --prefix ui run typecheck }
    Invoke-Check 'editor tests' { npm --prefix ui test }
    Invoke-Check 'editor build' { npm --prefix ui run build }
  }

  if ($Area -eq 'Native' -or $Area -eq 'All') {
    if ($env:OS -ne 'Windows_NT') { throw 'Native checks require Windows. Use -Area Editor or -Area Docs here.' }
    $manifest = Join-Path $root 'work/toolchain-minimal.json'
    if (-not (Test-Path -LiteralPath $manifest)) { throw 'Run: powershell -File tools/bootstrap-toolchain.ps1 -Minimal' }
    $tools = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
    foreach ($tool in @($tools.cmake, $tools.clangxx, $tools.ninja)) {
      if (-not $tool -or -not (Test-Path -LiteralPath $tool)) { throw 'Toolchain incomplete. Run: powershell -File tools/bootstrap-toolchain.ps1 -Minimal' }
    }
    $build = Join-Path $root 'build-contrib'
    Invoke-Check 'configure native logic tests' {
      & $tools.cmake -S $root -B $build -G Ninja '-DBUFFRX_TESTS_ONLY=ON' '-DCMAKE_BUILD_TYPE=Release' `
        '-DCMAKE_SYSTEM_NAME=Windows' "-DCMAKE_MAKE_PROGRAM=$($tools.ninja)" "-DCMAKE_CXX_COMPILER=$($tools.clangxx)"
    }
    Invoke-Check 'build native logic tests' {
      & $tools.cmake --build $build --parallel 2 --target buffrx-tests
    }
    $previousData = $env:BUFFRX_DATA_ROOT
    $previousSource = $env:BUFFRX_SOURCE_DIR
    $testRoot = Join-Path ([IO.Path]::GetTempPath()) ('buffrx-contrib-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $testRoot | Out-Null
    try {
      $env:BUFFRX_DATA_ROOT = $testRoot
      $env:BUFFRX_SOURCE_DIR = $root
      Invoke-Check 'native logic tests' { & (Join-Path (Split-Path $tools.cmake) 'ctest.exe') --test-dir $build --output-on-failure }
    } finally {
      $env:BUFFRX_DATA_ROOT = $previousData
      $env:BUFFRX_SOURCE_DIR = $previousSource
      # Only remove the unique temporary directory this invocation created.
      $resolvedTest = [IO.Path]::GetFullPath($testRoot)
      $tempPrefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
      if (-not $resolvedTest.StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Test cleanup path escaped TEMP.' }
      Remove-Item -LiteralPath $resolvedTest -Recurse -Force
    }
  }
  if ($Area -eq 'Build' -or $Area -eq 'All') {
    if ($env:OS -ne 'Windows_NT') { throw 'Full compilation requires Windows.' }
    Invoke-Check 'editor page for embedding' { npm --prefix ui run build }
    # Run in a child so build.ps1's Rust/PATH settings stay out of this shell.
    Invoke-Check 'full app compilation' { & powershell -NoProfile -File (Join-Path $root 'tools/build.ps1') -NoStage }
  }
  Write-Host "PASS: $Area"
} finally {
  Pop-Location
}
