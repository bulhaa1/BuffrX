# A lock for the whole machine, so two agents can work in one checkout without spoiling each other: a measurement needs a quiet machine, a build needs the
# shared `build\` folder to itself, a big engine run (a ring overflow, a soak row) spoils both. Whoever is about to do one of those takes the lock and gives it back
# when done; everybody else waits. Quiet work (reading, editing files, short tool calls) never needs it.
#
#   . (Join-Path $PSScriptRoot 'machine-lock.ps1')
#   Invoke-WithMachine -Owner 'night:N1:B4 60 fps' -MaxMinutes 10 { powershell -File tools\buffer-cost-probe.ps1 ... }   # waits, runs, always releases
#   Enter-Machine -Owner 'dedupe:build' -MaxMinutes 15      # $true when you hold it; $false after -WaitMinutes (default 60)
#   Exit-Machine -Owner 'dedupe:build'
#   Show-Machine                                             # who holds it, since when, until when
#
# The lock is a file, work\locks\machine.lock (work\ is not tracked), made with an atomic create. It carries an expiry: a holder that dies (or forgets) frees the
# machine when its MaxMinutes passes, so keep MaxMinutes honest (a build 15, a measurement row 10, a 10 minute ring run 20) and never hold it across a long idle wait.
# Hold it around ONE job, not around a whole session. Jobs that do not care about CPU and disk contention (a soak that only watches memory and handles, a 30 minute
# Timeline session) do not take it.
$script:MachineLockFile = Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..')).Path 'work\locks\machine.lock'

function Show-Machine {
  if (-not (Test-Path $script:MachineLockFile)) { return $null }
  try { Get-Content $script:MachineLockFile -Raw | ConvertFrom-Json } catch { $null }
}

function Enter-Machine([string]$Owner, [int]$MaxMinutes = 15, [int]$WaitMinutes = 60) {
  New-Item -ItemType Directory -Force (Split-Path $script:MachineLockFile) | Out-Null
  $deadline = (Get-Date).AddMinutes($WaitMinutes)
  while ($true) {
    $now = [DateTimeOffset]::UtcNow
    $body = [pscustomobject]@{ owner = $Owner; since = $now.ToString('o'); expires = $now.AddMinutes($MaxMinutes).ToString('o'); pid = $PID } | ConvertTo-Json -Compress
    try {
      $stream = [IO.File]::Open($script:MachineLockFile, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
      try { $bytes = [Text.Encoding]::UTF8.GetBytes($body); $stream.Write($bytes, 0, $bytes.Length) } finally { $stream.Dispose() }
      return $true
    } catch [IO.IOException] {
      # Held by somebody: free it if its time is up (a holder that died), otherwise wait.
      $held = Show-Machine
      if ($held -and ([DateTimeOffset]::Parse($held.expires) -lt [DateTimeOffset]::UtcNow)) {
        Remove-Item $script:MachineLockFile -Force -ErrorAction SilentlyContinue
        continue
      }
    }
    if ((Get-Date) -gt $deadline) { return $false }
    Start-Sleep -Seconds 3
  }
}

function Exit-Machine([string]$Owner) {
  $held = Show-Machine
  if (-not $held -or $held.owner -eq $Owner) { Remove-Item $script:MachineLockFile -Force -ErrorAction SilentlyContinue }
}

function Invoke-WithMachine([string]$Owner, [int]$MaxMinutes, [scriptblock]$Work, [int]$WaitMinutes = 60) {
  if (-not (Enter-Machine -Owner $Owner -MaxMinutes $MaxMinutes -WaitMinutes $WaitMinutes)) { throw "the machine lock was not free within $WaitMinutes minutes: $((Show-Machine).owner)" }
  try { & $Work } finally { Exit-Machine -Owner $Owner }
}
