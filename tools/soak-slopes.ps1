# Least-squares slopes for a soak CSV (tools\soak-probe.ps1 writes it to <TEMP>\buffrx-soak-<stamp>.csv).
# Usage: powershell -File tools\soak-slopes.ps1 -Csv "$env:TEMP\buffrx-soak-20261102-073700.csv" -SkipMinutes 10
param(
  [Parameter(Mandatory = $true)][string]$Csv,
  [int]$SkipMinutes = 10,
  [int]$ExpectedHandleGrowth = 0
)

$rows = Import-Csv $Csv
if (-not $rows) { throw "no rows in $Csv" }
$rows = @($rows | Where-Object { [double]$_.minute -ge $SkipMinutes })
if ($rows.Count -lt 3) { throw "fewer than 3 rows after minute $SkipMinutes" }

function Get-Slope($values) {
  # Least squares against the sample index; the minute column is the sample's own clock.
  $n = $values.Count
  $sx = 0.0; $sy = 0.0; $sxx = 0.0; $sxy = 0.0
  for ($i = 0; $i -lt $n; $i++) {
    $x = [double]$i; $y = [double]$values[$i]
    $sx += $x; $sy += $y; $sxx += $x * $x; $sxy += $x * $y
  }
  $d = $n * $sxx - $sx * $sx
  if ($d -eq 0) { return 0.0 }
  return ($n * $sxy - $sx * $sy) / $d
}

function Growth-Percent($first, $last) {
  if ([double]$first -eq 0) { return 0.0 }
  return ([double]$last - [double]$first) / [double]$first * 100.0
}

$metrics = @(
  @{ Name = 'private_mb'; Growth = $true },
  @{ Name = 'working_set_mb'; Growth = $true },
  @{ Name = 'threads'; Growth = $true },
  @{ Name = 'handles'; Growth = $false },
  @{ Name = 'thread_handles'; Growth = $false },
  @{ Name = 'event_handles'; Growth = $false },
  @{ Name = 'gdi'; Growth = $false },
  @{ Name = 'user'; Growth = $false },
  @{ Name = 'video_lagged'; Growth = $false },
  @{ Name = 'ring_files'; Growth = $false },
  @{ Name = 'free_gb'; Growth = $false }
)

$samplesPerMinute = 0.0
$firstMinute = [double]$rows[0].minute
$lastMinute = [double]$rows[$rows.Count - 1].minute
$span = $lastMinute - $firstMinute
if ($span -gt 0) { $samplesPerMinute = ($rows.Count - 1) / $span }

"soak $Csv"
"rows {0} from minute {1:N1} to {2:N1} ({3:N1} samples/min)" -f $rows.Count, $firstMinute, $lastMinute, $samplesPerMinute
""
"column             first        last     slope/min   growth%   verdict"

$failures = 0
foreach ($m in $metrics) {
  $values = @($rows | ForEach-Object { [double]$_.($m.Name) })
  $first = $values[0]; $last = $values[$values.Count - 1]
  $slope = Get-Slope $values
  $growth = Growth-Percent $first $last
  $verdict = 'ok'
  if ($m.Name -eq 'free_gb') {
    if ($last -lt $first - 2.0) { $verdict = 'DISK LOST'; $failures++ }
  } elseif ($m.Name -eq 'ring_files') {
    if ($last -gt 1) { $verdict = 'MORE THAN ONE RING FILE'; $failures++ }
  } elseif ($m.Name -eq 'video_lagged') {
    if ($slope -gt 0) { $verdict = 'GROWING'; $failures++ }
  } elseif ($m.Name -eq 'handles' -or $m.Name -eq 'thread_handles' -or $m.Name -eq 'event_handles') {
    if ($last - $first -gt $ExpectedHandleGrowth) { $verdict = 'ABOVE THE EXPECTED {0}' -f $ExpectedHandleGrowth; $failures++ }
  } elseif ($m.Name -eq 'gdi' -or $m.Name -eq 'user') {
    if ($last - $first -gt 2) { $verdict = 'LEAK'; $failures++ }
  } elseif ($m.Growth) {
    # The runbook's pass rule: growth under 2% over the measured window.
    if ($growth -gt 2.0) { $verdict = 'ABOVE 2%'; $failures++ }
  }
  if ($slope -ne 0 -or $m.Name -eq 'free_gb') {
    "{0,-16} {1,10:N2} {2,10:N2} {3,10:N4} {4,9:N2}   {5}" -f $m.Name, $first, $last, ($slope * $samplesPerMinute), $growth, $verdict
  } else {
    "{0,-16} {1,10:N2} {2,10:N2} {3,10:N4} {4,9:N2}   {5}" -f $m.Name, $first, $last, 0.0, $growth, $verdict
  }
}

""
"asleep samples: {0} of {1}" -f (@($rows | Where-Object { $_.asleep -eq 'True' }).Count), $rows.Count
$lastAsleep = $rows[$rows.Count - 1].asleep
"verdict: {0}" -f $(if ($failures -eq 0) { 'pass' } else { "$failures column(s) failed" })
if ($failures -ne 0) { exit 1 }
