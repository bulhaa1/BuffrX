# Fails when a tracked file breaks the repository's publication rules (GITHUB-SAFETY.md):
# private agent files and internal documents, machine paths, personal email addresses, credentials.
# Scans everything git tracks, so a clean result means the whole published tree is clean.
# -Authors also requires every commit in -BaseRef..HEAD to carry a GitHub noreply address (owner use; CI skips it
# because a contributor's commits are theirs).
param([string]$Root = '.', [string]$BaseRef = '', [switch]$Authors)

$ErrorActionPreference = 'Stop'
$problems = [System.Collections.Generic.List[string]]::new()

$privatePaths = @('^AGENTS\.md$', '^CLAUDE\.md$', '^\.claude/', '^plans/', '^work/', '^runtime/', '^docs/(?!protocol\.md$)')
$contentRules = @(
  @{ Name = 'a Windows user-profile path'; Pattern = '(?i)\b[A-Z]:\\Users\\(?!Public\b|Default\b|<|\{|%|\*|username|user\b|name\b|you\b|\$)[^\\\s"''<>]+' },
  @{ Name = 'a Unix home path'; Pattern = '(?<![\w.:])/(?:Users|home)/(?!runner\b|user\b|username\b|you\b|name\b|<|\$|\{|\*)[a-z][\w.-]*/' },
  @{ Name = 'a personal email address'; Pattern = '(?i)\b[\w.%+-]+@(?:gmail|googlemail|outlook|hotmail|live|yahoo|icloud|proton(?:mail)?|pm)\.(?:com|me)\b' },
  @{ Name = 'a GitHub token'; Pattern = '\b(?:gh[pousr]_[A-Za-z0-9]{36,}|github_pat_[A-Za-z0-9_]{50,})\b' },
  @{ Name = 'an AWS access key'; Pattern = '\bAKIA[0-9A-Z]{16}\b' },
  @{ Name = 'a Slack token'; Pattern = '\bxox[baprs]-[A-Za-z0-9-]{10,}' },
  @{ Name = 'a private key'; Pattern = '-----BEGIN (?:RSA |EC |DSA |OPENSSH |PGP )?PRIVATE KEY-----' },
  @{ Name = 'a Discord webhook URL'; Pattern = 'discord(?:app)?\.com/api/webhooks/\d{15,}/[\w-]{30,}' }
)
# This script and its test spell the patterns out; binary and generated files cannot carry readable secrets.
$skip = '^(tools/check-privacy\.ps1|tools/test-check-privacy\.ps1)$|\.(png|jpg|jpeg|gif|ico|icns|woff2?|ttf|otf|exe|dll|lib|zip|7z|mp4|mkv|wav|res|svg)$|(^|/)(package-lock\.json|Cargo\.lock)$'

$files = @(& git -C $Root ls-files -z) -join '' -split "`0" | Where-Object { $_ }
if ($LASTEXITCODE -ne 0) { throw 'git ls-files failed' }
foreach ($file in $files) {
  foreach ($rule in $privatePaths) {
    if ($file -match $rule) { $problems.Add("${file}: private or internal file is tracked (stays local-only)"); break }
  }
  if ($file -match $skip) { continue }
  $full = Join-Path $Root $file
  if (-not (Test-Path -LiteralPath $full -PathType Leaf)) { continue }
  $text = [IO.File]::ReadAllText($full)
  if ($text.IndexOf([char]0) -ge 0) { continue }
  $lines = $text -split "`n"
  for ($n = 0; $n -lt $lines.Count; $n++) {
    foreach ($rule in $contentRules) {
      if ($lines[$n] -match $rule.Pattern) { $problems.Add("${file}:$($n + 1): $($rule.Name)") }
    }
  }
}

if ($Authors) {
  $range = if ($BaseRef) { "$BaseRef..HEAD" } else { '@{upstream}..HEAD' }
  foreach ($line in @(& git -C $Root log --format='%h %ae %ce' $range)) {
    $parts = $line -split ' '
    foreach ($email in $parts[1..2]) {
      if ($email -notmatch '@users\.noreply\.github\.com$') { $problems.Add("commit $($parts[0]): author or committer is not a GitHub noreply address") ; break }
    }
  }
}

if ($problems.Count -gt 0) {
  $problems | Select-Object -Unique | ForEach-Object { Write-Host "PRIVACY: $_" }
  throw "Privacy check failed: $($problems.Count) problem(s). See GITHUB-SAFETY.md."
}
Write-Host "Privacy check passed ($($files.Count) tracked files)."
