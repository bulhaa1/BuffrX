# Proves tools/check-privacy.ps1 fails on each kind of leak and passes on a clean tree.
$ErrorActionPreference = 'Stop'
$script = Join-Path $PSScriptRoot 'check-privacy.ps1'
$repo = Join-Path ([IO.Path]::GetTempPath()) ('privacy-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $repo | Out-Null
$user = 'C:' + '\Users\' + 'someone'
$token = 'gh' + 'p_' + ('a' * 36)
$cases = [ordered]@{
  'AGENTS.md'                 = 'private agent file'
  'docs/notes.md'             = 'internal doc'
  'src/a.cpp'                 = "const char* p = `"$user\clips`";"
  'src/b.cpp'                 = '// owner is jane.doe@gmail.com'
  'src/c.ts'                  = "const t = '$token';"
  'src/d.sh'                  = 'cd /home/jane/project'
}
try {
  git -C $repo init -q
  git -C $repo config user.email 'ci@users.noreply.github.com'
  git -C $repo config user.name ci
  New-Item -ItemType Directory -Path (Join-Path $repo 'docs'), (Join-Path $repo 'src') | Out-Null
  'clean `C:\Users\<name>\x` and fixture@example.com' | Set-Content (Join-Path $repo 'src/ok.md')
  'protocol' | Set-Content (Join-Path $repo 'docs/protocol.md')
  git -C $repo add -A; git -C $repo commit -q -m clean
  & $script -Root $repo | Out-Null
  Write-Output 'PASS clean tree passes'
  foreach ($name in $cases.Keys) {
    $path = Join-Path $repo $name
    $cases[$name] | Set-Content -LiteralPath $path
    git -C $repo add -A
    $failed = $false
    try { & $script -Root $repo 6>$null | Out-Null } catch { $failed = $true }
    if (-not $failed) { throw "check-privacy did not fail on $name" }
    Write-Output "PASS fails on $name"
    git -C $repo rm -q --cached -f -- $name
    Remove-Item -LiteralPath $path
  }
  git -C $repo -c user.email=jane@gmail.com commit -q --allow-empty --author='J <jane@gmail.com>' -m owner
  $failed = $false
  try { & $script -Root $repo -Authors -BaseRef HEAD~1 6>$null | Out-Null } catch { $failed = $true }
  if (-not $failed) { throw 'check-privacy -Authors did not fail on a personal commit address' }
  Write-Output 'PASS -Authors fails on a personal commit address'
} finally {
  $full = [IO.Path]::GetFullPath($repo)
  if ($full.StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()), [StringComparison]::OrdinalIgnoreCase)) {
    Remove-Item -LiteralPath $full -Recurse -Force -ErrorAction SilentlyContinue
  }
}
