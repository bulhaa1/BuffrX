# Publishes site-main/ to Cloudflare Pages.
#
# The zip this takes first is exactly what gets uploaded, so it is the copy to look at when a publish
# turns out bad; the previous version is undone from Cloudflare's deployment list, which keeps every
# upload. Both the zip and the deployments list are why the site folder needs no repository.
# The publish goes to a preview branch first and is checked there by tools/verify-site.ps1; only a preview
# that passes becomes production, so a bad folder costs a preview deployment instead of the site.
# The one-time table and its index are in site-main/schema.sql, applied once per database.
#   powershell -File tools/deploy-site.ps1
#   powershell -File tools/deploy-site.ps1 -SkipSnapshot
param(
  [switch]$SkipSnapshot
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$site = Join-Path $root 'site-main'

# A wrong or missing file publishes a site that looks fine and has no link service, and wrangler exits 0.
# Everything the deploy depends on is named here, so a rename is caught before it is public.
$required = @(
  'index.html',
  '404.html',
  'l/index.html',
  'l/link.js',
  '_headers',
  'wrangler.jsonc',
  'functions/api/shorten.ts',
  'functions/api/revoke.ts',
  'functions/s/[code].ts'
)
foreach ($name in $required) {
  $path = Join-Path $site $name
  if (-not (Test-Path -LiteralPath $path)) { throw "site-main is missing $name" }
}

# Every text rule lives in one checker, so a publish and a preview obey the same ones, and this script
# cannot drift from the tool an operator runs by hand.
& (Join-Path $PSScriptRoot 'verify-site.ps1') -Root $site -LocalOnly
if ($LASTEXITCODE -ne 0) { throw 'the text check failed; nothing was uploaded' }

# The dev cache belongs to `wrangler pages dev`. It must not publish, so a file that will not go is fatal.
$cache = Join-Path $site '.wrangler'
if (Test-Path -LiteralPath $cache) {
  Remove-Item -LiteralPath $cache -Recurse -Force
  if (Test-Path -LiteralPath $cache) { throw "$cache is still there; it would publish the local dev state" }
}

if (-not $SkipSnapshot) {
  $snapshots = Join-Path $root 'site-snapshots'
  New-Item -ItemType Directory -Force $snapshots | Out-Null
  $zip = Join-Path $snapshots ((Get-Date).ToString('yyyy-MM-dd-HHmmss') + '.zip')
  Compress-Archive -Path (Join-Path $site '*') -DestinationPath $zip -CompressionLevel Optimal -Force
  Write-Host "snapshot: $zip"
  # Ten publishes is about a month of history at 31 MB each; older zips are what Cloudflare's deployment
  # list already covers.
  Get-ChildItem $snapshots -Filter '*.zip' | Sort-Object LastWriteTime -Descending | Select-Object -Skip 10 |
    Remove-Item -Force
}

# One publish, to whichever branch. Pinned: this runs with the Cloudflare credentials on this machine, so
# the wrangler version is a decision, not whatever npm answered that day.
function Invoke-PagesDeploy([string]$Branch) {
  Push-Location $site
  $previous = $ErrorActionPreference
  # wrangler writes progress to stderr; PowerShell turns that into a terminating error under Stop.
  $ErrorActionPreference = 'Continue'
  try {
    $out = npx --yes wrangler@4.160.0 pages deploy . --project-name buffrx --branch $Branch --commit-dirty=true 2>&1
  } finally {
    $ErrorActionPreference = $previous
    Pop-Location
  }
  if ($LASTEXITCODE -ne 0) { throw "deploy to $Branch failed with exit code $LASTEXITCODE" }
  return ($out | Out-String)
}

# The preview carries the same bytes as production on a URL nobody has bookmarked, so a publish that fails
# its checks costs a preview deployment instead of the site.
$previewOutput = Invoke-PagesDeploy 'preview'
$preview = if ($previewOutput -match 'https://[0-9a-f]+\.buffrx\.pages\.dev') { $Matches[0] } else { 'https://preview.buffrx.pages.dev' }
Write-Host "verifying preview: $preview"
& (Join-Path $PSScriptRoot 'verify-site.ps1') -Root $site -Url $preview -LinkProbe fatal
if ($LASTEXITCODE -ne 0) { throw 'the preview failed its checks; production was not touched' }

Invoke-PagesDeploy 'main' | Out-Null
Write-Host 'verifying production: https://buffrx.app'
& (Join-Path $PSScriptRoot 'verify-site.ps1') -Root $site -Url 'https://buffrx.app' -LinkProbe warn
if ($LASTEXITCODE -ne 0) { Write-Host 'warning: production failed its checks after the publish; see above, then roll back' }
