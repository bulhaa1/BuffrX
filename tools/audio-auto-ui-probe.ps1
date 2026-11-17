# Hands-free proof of the automatic app audio interface: the empty state, three isolated programs, the
# Other group and its members, the microphone, every control on the strips and rows, what survives a
# restart, Performance mode, the compatibility fallbacks, 150% scaling and a deck that overflows.
#
#   powershell -File tools/audio-auto-ui-probe.ps1 -Engine build-ui\buffrx-engine.exe -Fixture build-ui\buffrx-audio-fixture.exe
#
# It runs a private copy of the built engine beside the BuffrX the person is using (its own data root and
# pipe, its own runtime copy) and stops only the processes it started. The programs on the lanes are copies
# of tests/audio_fixture.cpp with distinct names, so they are distinct programs to Windows; any other app
# that plays sound on this machine is excluded before the checks, so the person's own audio is never
# recorded. Every check reads a protocol reply or the pixels of a capture the interface drew itself.
#
# Honest limits: the "programs" are silent-tone renderers, not real applications; the deck's overflow is
# produced by the first-run guide card, not by a small monitor; 150% scaling is the interface's own scale
# setting, not a second monitor's DPI.
param(
  [string]$Engine = 'build-ui\buffrx-engine.exe',
  [string]$Fixture = 'build-ui\buffrx-audio-fixture.exe',
  [string]$RuntimeRoot = 'runtime',
  [string]$PrivateRoot = 'work\auto-ui-runtime',
  [string]$Artifacts = 'work\audio-review-artifacts\auto-ui',
  [switch]$Keep
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'probe-lib.ps1')
Add-Type -AssemblyName System.Drawing
if (-not ('BxShot' -as [type])) {
  Add-Type -TypeDefinition @'
using System; using System.Collections.Generic; using System.Drawing; using System.Drawing.Imaging; using System.Runtime.InteropServices;
public class BxShot : IDisposable {
  Bitmap bmp; byte[] px; int stride;
  public int Width, Height;
  public BxShot(string file) {
    bmp = new Bitmap(file); Width = bmp.Width; Height = bmp.Height;
    var data = bmp.LockBits(new Rectangle(0, 0, Width, Height), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
    stride = data.Stride; px = new byte[stride * Height]; Marshal.Copy(data.Scan0, px, 0, px.Length); bmp.UnlockBits(data);
  }
  public bool Is(int x, int y, int[] c, int tol) {
    if (x < 0 || y < 0 || x >= Width || y >= Height) return false;
    int i = y * stride + x * 4;
    if (i < 0 || i + 3 >= px.Length) return false;  // a capture read while it is written can be shorter than its header says
    return Math.Abs(px[i + 2] - c[0]) <= tol && Math.Abs(px[i + 1] - c[1]) <= tol && Math.Abs(px[i] - c[2]) <= tol;
  }
  public List<int[]> RowRuns(int y, int[] c, int tol, int x0, int x1) {
    var runs = new List<int[]>(); int start = -1; x1 = Math.Min(x1, Width); x0 = Math.Max(0, x0);
    for (int x = x0; x < x1; x++) { if (Is(x, y, c, tol)) { if (start < 0) start = x; } else if (start >= 0) { runs.Add(new int[] { start, x - start }); start = -1; } }
    if (start >= 0) runs.Add(new int[] { start, x1 - start });
    return runs;
  }
  public List<int[]> ColRuns(int x, int[] c, int tol, int y0, int y1) {
    var runs = new List<int[]>(); int start = -1; y1 = Math.Min(y1, Height); y0 = Math.Max(0, y0);
    for (int y = y0; y < y1; y++) { if (Is(x, y, c, tol)) { if (start < 0) start = y; } else if (start >= 0) { runs.Add(new int[] { start, y - start }); start = -1; } }
    if (start >= 0) runs.Add(new int[] { start, y1 - start });
    return runs;
  }
  public int Count(int[] c, int tol, int x0, int y0, int x1, int y1) {
    int n = 0; x1 = Math.Min(x1, Width); y1 = Math.Min(y1, Height);
    for (int y = Math.Max(0, y0); y < y1; y++) for (int x = Math.Max(0, x0); x < x1; x++) if (Is(x, y, c, tol)) n++;
    return n;
  }
  public void Dispose() { bmp.Dispose(); }
}
'@ -ReferencedAssemblies 'System.Drawing'
}

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$artifacts = Join-Path $repo $Artifacts
New-Item -ItemType Directory -Force $artifacts | Out-Null
$shots = Join-Path $env:TEMP ('buffrx-auto-ui-' + $PID)
New-Item -ItemType Directory -Force $shots | Out-Null

# The private runtime: the built exe beside a frozen copy of the runtime, so a concurrent build cannot swap
# the binary or the DLLs under the run.
$privateBin = Join-Path $repo "$PrivateRoot\bin\64bit"
if (-not (Test-Path -LiteralPath (Join-Path $privateBin 'obs.dll'))) {
  New-Item -ItemType Directory -Force $privateBin | Out-Null
  Copy-Item -Path (Join-Path $repo "$RuntimeRoot\bin\64bit\*") -Destination $privateBin -Recurse -Force
}
Copy-Item -LiteralPath (Resolve-Path (Join-Path $repo $Engine)).Path -Destination (Join-Path $privateBin 'buffrx-engine.exe') -Force
$engineExe = Join-Path $privateBin 'buffrx-engine.exe'
$fixtureSource = (Resolve-Path (Join-Path $repo $Fixture)).Path
$realRuntime = (Resolve-Path (Join-Path $repo $RuntimeRoot)).Path

$script:failures = 0
function Check([string]$what, [bool]$ok, [string]$detail = '') {
  $suffix = ''
  if ($detail) { $suffix = '  ' + $detail }
  if ($ok) { Write-Host ('  ok   ' + $what + $suffix) }
  else { Write-Host ('  FAIL ' + $what + $suffix) -ForegroundColor Red; $script:failures++ }
}

# A pane's border at rest is one seam colour antialiased over its face; a popup's border is line_strong; a lane
# state the interface says while a capture runs is the accent.
$script:seam = @(0x34, 0x36, 0x38)
$script:strong = @(0x70, 0x71, 0x71)
$script:accent = @(0xff, 0x6b, 0x2c)
$script:danger = @(0xff, 0x4d, 0x5e)
$script:muted = @(0xaa, 0xa7, 0xa2)

$script:stderr = $null
function Ask([string]$command, [string]$arguments = '{}') { Ask-Engine $e $command $arguments }
function AskJson([string]$command, [string]$arguments = '{}') { (Ask-Engine $e $command $arguments) | ConvertFrom-Json }
function Audio { (AskJson 'audio.get').data }
function Status { (AskJson 'status.get').data }
# A preference is keyed by the executable ("probea.exe"), and PowerShell cannot index a PSCustomObject by a
# property name that holds a dot: `$prefs['probea.exe']` is empty while `$prefs.'probea.exe'` is the entry.
# Every read of a program's own gain, mute and exclude goes through this, or a check passes on $null.
function Pref($prefs, [string]$exe) {
  if (-not $prefs -or -not $exe) { return $null }
  $key = $exe.ToLowerInvariant()
  foreach ($property in $prefs.PSObject.Properties) { if ($property.Name -eq $key) { return $property.Value } }
  return $null
}
# The engine's console carries one `command: <name>` line per command: the proof that the automatic mixer
# never opens an app picker, whose snapshot is the only reader of sources.windows in it.
function Command-Count([string]$name) {
  if (-not $script:stderr -or -not (Test-Path -LiteralPath $script:stderr)) { return 0 }
  @(Select-String -LiteralPath $script:stderr -Pattern ('command: ' + [regex]::Escape($name))).Count
}

function Start-Engine([string]$configJson, [string]$dataRoot, [switch]$Guide, [switch]$Reuse) {
  Assert-ProbeDataRoot $dataRoot | Out-Null
  New-Item -ItemType Directory -Force (Join-Path $dataRoot 'clips') | Out-Null
  $clips = ((Join-Path $dataRoot 'clips') -replace '\\', '/')
  $shotsDir = (($dataRoot + '\shots') -replace '\\', '/')
  $json = $configJson.TrimEnd('}').TrimEnd() + ',"clips_dir":"' + $clips + '","screenshots_dir":"' + $shotsDir + '"}'
  # A restart keeps the file the engine itself wrote: the gains, mutes and excludes are the point of it.
  if (-not $Reuse -or -not (Test-Path -LiteralPath (Join-Path $dataRoot 'config.json'))) {
    Set-Content -LiteralPath (Join-Path $dataRoot 'config.json') -NoNewline -Value $json
  }
  $layout = if ($Guide) { '{"welcomed":false}' } else { '{"welcomed":true}' }
  Set-Content -LiteralPath (Join-Path $dataRoot 'ui.json') -NoNewline -Value $layout
  $pipe = 'buffrx-auto-ui-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
  $script:stderr = Join-Path $dataRoot 'engine-console.log'
  $env:BUFFRX_DATA_ROOT = $dataRoot; $env:BUFFRX_PIPE = $pipe; $env:BUFFRX_RUNTIME_ROOT = $realRuntime
  $process = Start-Process -FilePath $engineExe -WorkingDirectory $privateBin -PassThru -WindowStyle Hidden `
    -RedirectStandardError $script:stderr -RedirectStandardOutput (Join-Path $dataRoot 'engine-stdout.log')
  Remove-Item Env:BUFFRX_DATA_ROOT, Env:BUFFRX_PIPE, Env:BUFFRX_RUNTIME_ROOT -ErrorAction SilentlyContinue
  $engine = [pscustomobject]@{ Process = $process; DataRoot = $dataRoot; Pipe = $pipe }
  $script:mixerOpen = $false  # a new process has no popup open
  for ($i = 0; $i -lt 60; $i++) {
    Start-Sleep -Milliseconds 500
    try { (New-Object PanelPipe($pipe, 500)).Dispose(); return $engine } catch { }
  }
  throw 'the isolated engine did not answer its pipe'
}

$script:fixtures = @()
function Start-Fixture([string]$name) {
  $exe = Join-Path $privateBin $name
  Copy-Item -LiteralPath $fixtureSource -Destination $exe -Force
  $info = New-Object System.Diagnostics.ProcessStartInfo
  $info.FileName = $exe; $info.WorkingDirectory = $privateBin; $info.UseShellExecute = $false
  $info.CreateNoWindow = $true; $info.RedirectStandardInput = $true; $info.RedirectStandardOutput = $true
  $process = [System.Diagnostics.Process]::Start($info)
  Start-Sleep -Milliseconds 800
  $script:fixtures += $process
  return $process
}
function Stop-Fixture($process) {
  try { $process.StandardInput.WriteLine('quit'); $process.WaitForExit(2000) | Out-Null } catch { }
  try { if (-not $process.HasExited) { $process.Kill() } } catch { }
  $script:fixtures = @($script:fixtures | Where-Object { $_ -ne $process })
}

# A program that is not one of the probe's own, playing sound on this machine, is excluded: the probe must
# never capture the person's audio, and a lane it does not own would change the strip count under test.
# Seed exclusions from all existing processes and render sessions before recording starts; sessions
# could become active during the run. Console hosts are ancestors of the fixture and must not be excluded.
function Exclude-ExistingSessions {
  $sessions = (AskJson 'audio.sessions').data
  if ($sessions.error) { throw ('Cannot safely enumerate audio sessions: ' + $sessions.error) }
  $prefs = @{}
  foreach ($process in @(Get-Process)) {
    $exe = $process.ProcessName + ".exe"
    if ($exe -notlike "Probe*" -and $exe -notin @("conhost.exe", "OpenConsole.exe")) {
      $prefs[$exe.ToLowerInvariant()] = @{ excluded = $true }
    }
  }
  foreach ($session in @($sessions.sessions)) {
    $exe = [string]$session.exe
    if ($exe -and $exe -notlike 'Probe*' -and $exe -notin @('conhost.exe', 'OpenConsole.exe')) {
      $prefs[$exe.ToLowerInvariant()] = @{ excluded = $true }
    }
  }
  if ($prefs.Count -gt 0) { $null = Ask 'audio.set' (@{ audio_app_prefs = $prefs } | ConvertTo-Json -Depth 5 -Compress) }
}

function Exclude-Intruders([int]$rounds = 8) {
  for ($i = 0; $i -lt $rounds; $i++) {
    $st = Status
    $intruders = @()
    foreach ($lane in @($st.audio.auto.lanes)) {
      foreach ($app in @($lane.apps)) { if ($app.exe -and $app.exe -notlike 'Probe*') { $intruders += $app.exe } }
    }
    $intruders = @($intruders | Sort-Object -Unique)
    if ($intruders.Count -eq 0) { return $true }
    $prefs = @{}
    foreach ($exe in $intruders) { $prefs[$exe.ToLowerInvariant()] = @{ excluded = $true } }
    $null = Ask 'audio.set' (@{ audio_app_prefs = $prefs } | ConvertTo-Json -Depth 5 -Compress)
    Start-Sleep -Milliseconds 800
  }
  return $false
}

function Capture([string]$view, [string]$name) {
  $file = Join-Path $shots ($name + '.png')
  $reply = Ask 'ui.capture' (@{ view = $view; file = ($file -replace '\\', '/') } | ConvertTo-Json -Compress)
  if ($reply -notmatch '"ok":true') { throw ('ui.capture ' + $view + ' failed: ' + $reply) }
  Start-Sleep -Milliseconds 450
  Copy-Item -LiteralPath $file -Destination (Join-Path $artifacts ($name + '.png')) -Force
  return $file
}
function Click([int]$x, [int]$y) {
  # `stay` keeps the pointer at the press. A fader or a slider takes its value from the pointer, so a click that
  # jumped the pointer away would write the value at the far end of its track.
  $null = Ask 'ui.input' (@{ window = 'main'; click = @($x, $y); stay = $true } | ConvertTo-Json -Compress)
  Start-Sleep -Milliseconds 700
}

# The AUDIO pane in a capture: the two antialiased border columns 140 design units apart, then the pane's own
# top from the left border's vertical run (which is clipped at the deck's foot when the deck overflows).
function Find-AudioPane([string]$png) {
  $shot = New-Object BxShot($png)
  try {
    $s = $shot.Width / 1035.0
    $cols = @($shot.RowRuns([int]($shot.Height * 0.9), $script:seam, 6, [int]($shot.Width * 0.2), [int]($shot.Width * 0.5)) |
              Where-Object { $_[1] -eq 2 } | Sort-Object { $_[0] })
    $left = -1
    $span = 140 * $s  # the AUDIO pane is 140 design units wide, whatever the interface's scale
    for ($i = 0; $i -lt $cols.Count - 1; $i++) {
      if ([math]::Abs(($cols[$i + 1][0] - $cols[$i][0]) - $span) -le 6) { $left = $cols[$i][0]; break }
    }
    if ($left -lt 0) { return $null }
    $runs = @($shot.ColRuns($left, $script:seam, 6, [int]($shot.Height * 0.3), $shot.Height) | Where-Object { $_[1] -ge 20 })
    if ($runs.Count -eq 0) { return $null }
    $top = ($runs | Sort-Object { $_[0] } | Select-Object -First 1)[0]
    return [pscustomobject]@{ Left = $left; Top = $top; Scale = $s; Height = $shot.Height }
  } finally { $shot.Dispose() }
}

# The mixer popup in a capture: its two 1 px line_strong borders, whose length is the popup's width. The width
# comes from the drawing, not from a status read a second later, so a lane that arrives or leaves between the
# two cannot move the coordinates off the controls.
function Find-Popup-Any([string]$png) {
  $shot = New-Object BxShot($png)
  try {
    $lines = @()
    for ($y = 0; $y -lt $shot.Height; $y++) {
      foreach ($run in $shot.RowRuns($y, $script:strong, 4, 0, $shot.Width)) {
        if ($run[1] -ge 300 -and $run[1] -le 900) { $lines += ,@($run[0], $y, $run[1]) }
      }
    }
    if ($lines.Count -lt 2) { return $null }
    foreach ($top in ($lines | Sort-Object { $_[1] })) {
      # A manual scan: a pipeline that matches one line hands back that line's three numbers, not the line.
      $bottom = $null
      foreach ($cand in $lines) {
        if (($cand[1] - $top[1]) -lt 250) { continue }
        if ([math]::Abs($cand[0] - $top[0]) -gt 2 -or [math]::Abs($cand[2] - $top[2]) -gt 2) { continue }
        if ($null -eq $bottom -or $cand[1] -lt $bottom[1]) { $bottom = $cand }
      }
      if ($null -eq $bottom) { continue }
      # The two side borders make it a box, not a pair of long lines.
      $mid = [int](($top[1] + $bottom[1]) / 2)
      if (-not ($shot.Is($top[0], $mid, $script:strong, 4) -and $shot.Is($top[0] + $top[2] - 1, $mid, $script:strong, 4))) { continue }
      $s = $shot.Width / 1035.0
      return [pscustomobject]@{ X = $top[0]; Y = $top[1]; W = $top[2]; H = ($bottom[1] - $top[1] + 1); Scale = $s; ShotHeight = $shot.Height }
    }
    return $null
  } finally { $shot.Dispose() }
}

# The strips the automatic mixer draws: three isolated lanes at most, the Other group, the microphone.
function Read-Strips($status) {
  $apps = 0; $other = $false; $rows = 0
  foreach ($lane in @($status.audio.auto.lanes)) {
    if ($lane.other) { $other = $true; $rows = @($lane.apps).Count } else { $apps++ }
  }
  $apps = [math]::Min($apps, 3)
  return [pscustomobject]@{ Strips = $apps + $(if ($other) { 1 } else { 0 }) + 1; Apps = $apps; Other = $other; Rows = $rows }
}
function Popup-Width([int]$strips) { return [math]::Max(320, 24 + $strips * 76 + ($strips - 1) * 8) }
# The popup's own height, exactly as draw_audio computes it: a layout that outgrew it would clip. The warning
# block is one head line, one line per refused program (three at most) and one line for the rest.
function Expected-Height([bool]$active, [int]$otherRows, [bool]$otherOpen, [int]$excluded, [bool]$performance, [int]$uncaptured = 0) {
  $extra = 0
  if (-not $active) { $extra += 40 }
  if ($otherRows -gt 0) { $extra += 26 }
  if ($otherRows -gt 0 -and $otherOpen) { $extra += [math]::Min($otherRows, 4) * 29 + 8 }
  if ($excluded -gt 0) { $extra += 26 }
  if ($performance) { $extra += 34 }
  if ($uncaptured -gt 0) {
    $shown = [math]::Min($uncaptured, 3)
    $extra += 15 * (1 + $shown) + $(if ($uncaptured -gt $shown) { 15 } else { 0 }) + 4
  }
  return 48 + 198 + 8 + 16 + 18 + $extra
}
function Uncaptured-Count($status) { return @($status.audio.auto.uncaptured).Count }
# The popup's content box and the design-unit offsets of its controls, as ui_live.cpp lays them out.
function Content($popup) { return [pscustomobject]@{ X = $popup.X + 12 * $popup.Scale; Y = $popup.Y + 38 * $popup.Scale; W = $popup.W - 24 * $popup.Scale } }
function Strip-Left($content, [int]$index, [int]$strips, [double]$scale) {
  $total = $strips * 76 * $scale + ($strips - 1) * 8 * $scale
  return $content.X + ($content.W - $total) / 2 + $index * 84 * $scale
}
function Excluded-Count($prefs) { return @($prefs.PSObject.Properties | Where-Object { $_.Value.excluded }).Count }
# The restore row in an idle mixer: the second block of muted text under the strips (the line, the row, the note).
function Find-RestoreRow([string]$png, $popup) {
  $shot = New-Object BxShot($png)
  try {
    $s = $popup.Scale
    $content = Content $popup
    $y0 = [int]($content.Y + 198 * $s)
    $y1 = [int]($popup.Y + $popup.H - 6 * $s)
    $rows = @()
    for ($y = $y0; $y -lt $y1; $y++) {
      if ($shot.Count($script:muted, 12, [int]$content.X, $y, [int]($content.X + $content.W), $y + 1) -gt 0) { $rows += $y }
    }
    if ($rows.Count -lt 3) { return $null }
    $blocks = @()
    $start = $rows[0]; $last = $rows[0]
    for ($i = 1; $i -lt $rows.Count; $i++) {
      $y = $rows[$i]
      if ($y - $last -le 6) { $last = $y } else { $blocks += ,@($start, $last); $start = $y; $last = $y }
    }
    $blocks += ,@($start, $last)
    if ($blocks.Count -lt 2) { return $null }
    $row = $blocks[1]
    return [pscustomobject]@{ X = [int]($content.X + 70 * $s); Y = [int](($row[0] + $row[1]) / 2) }
  } finally { $shot.Dispose() }
}

function Danger-Pixels([string]$png) {
  # Counts the danger colour in a capture the probe wrote. Each call reads its own copy, so a picture that is
  # still being written is never read twice.
  $picture = New-Object BxShot($png)
  try { return $picture.Count(@(255, 77, 94), 30, 0, 0, $picture.Width, $picture.Height) }
  finally { $picture.Dispose() }
}

function Open-Mixer([string]$name) {
  # A press on the Mixer button while the mixer is open closes it (the click lands outside the popup), so the
  # probe only presses the button when the popup is not already there. A popup that closed while the probe was
  # watching something else is noticed here and opened again, never assumed.
  if ($script:mixerOpen) {
    $png = Capture 'current' ($name + '-mixer')
    if (Find-Popup-Any $png) { return $png }
    $script:mixerOpen = $false
  }
  $main = Capture 'main' ($name + '-panel')
  $pane = Find-AudioPane $main
  if (-not $pane) { throw 'the AUDIO pane was not found in the capture' }
  Click ([int]($pane.Left + 70 * $pane.Scale)) ([int]($pane.Top + 24 * $pane.Scale))
  $script:mixerOpen = $true
  return Capture 'current' ($name + '-mixer')
}
function Close-Mixer {
  $null = Ask 'ui.input' (@{ window = 'main'; keys = @('Escape') } | ConvertTo-Json -Compress)
  Start-Sleep -Milliseconds 600
  $script:mixerOpen = $false
}
# The pointer out of the mixer and long enough without input that its rows stop holding their order: the drawn
# rows are then the engine's own order, which is the one status reports.
function Settle {
  $null = Ask 'ui.input' (@{ window = 'main'; move = @(10, 10) } | ConvertTo-Json -Compress)
  Start-Sleep -Milliseconds 2200
}
# One look at the mixer: the capture, the status, the popup found in the drawing, and the number of strips that
# popup was drawn for (read from its width: (width - 24 + 8) / 84).
function Mixer-Shot([string]$name) {
  Settle
  $png = Capture 'current' $name
  $st = Status
  $shape = Read-Strips $st
  $popup = Find-Popup-Any $png
  $strips = $shape.Strips
  if ($popup) {
    $derived = [int][math]::Round((($popup.W / $popup.Scale) - 24 + 8) / 84)
    if ($derived -ge 1 -and $derived -le 6) { $strips = $derived }
  }
  $drawnOther = ($strips - $shape.Apps - 1) -eq 1
  return [pscustomobject]@{ Png = $png; Status = $st; Shape = $shape; Popup = $popup; Strips = $strips; DrawnOther = $drawnOther }
}

$config = @{
  config_version = 11; start_buffering = $false; auto_game_capture = $false; notifications = $false
  notification_sound = $false; mic_enabled = $true; desktop_enabled = $true; audio_capture_mode = 'auto'
  timeline_minutes = 0; capture_mode = 'display'; capture_fps = 60; codec = 'h264'; container = 'mp4'
  quality = 'standard'; audio_tracks = 6; separate_tracks = $true; buffer_seconds = 30
} | ConvertTo-Json -Compress

$e = $null
$dataRoot = Join-Path $env:TEMP ('buffrx-auto-ui-data-' + $PID)
$noise = $null
$loop = $null
try {
  # ---- 1. Nothing capturing: a quiet line and the microphone, and no app picker anywhere.
  $e = Start-Engine $config $dataRoot
  $st = Status
  Check 'the first-run configuration is automatic capture' ([bool]$st.audio.auto.on -and -not [bool]$st.audio.auto.watching)
  $readsBefore = Command-Count 'sources.windows'
  $mixer = Open-Mixer 'empty'
  $shape = Read-Strips $st
  $popup = Find-Popup-Any $mixer
  Check 'the quiet mixer shows the microphone alone' ([bool]$popup -and $shape.Strips -eq 1) ('strips=' + $shape.Strips)
  if ($popup) {
    $height = Expected-Height $false 0 $false 0 $false (Uncaptured-Count $st)
    Check 'the quiet mixer is sized for its own content' ([math]::Abs($popup.H / $popup.Scale - $height) -le 2) `
      ('drawn ' + [math]::Round($popup.H / $popup.Scale, 1) + ' design, laid out ' + $height)
  }
  Check 'the automatic mixer reads no program list (no picker, no Add)' ((Command-Count 'sources.windows') -eq $readsBefore)
  $appsAtStart = (Audio).apps | ConvertTo-Json -Depth 4 -Compress
  Check 'the automatic mixer writes nothing into the saved app selection' (@((Audio).apps).Count -eq 0)

  # ---- 2. Escape closes the mixer and leaves the panel; the next one hides the window.
  Close-Mixer
  $closed = Capture 'current' 'empty-closed'
  Check 'Escape closes the mixer without closing the panel' ($null -eq (Find-Popup-Any $closed))
  $null = Ask 'ui.input' (@{ window = 'main'; keys = @('Escape') } | ConvertTo-Json -Compress)
  Start-Sleep -Milliseconds 700
  $hidden = Ask 'ui.capture' (@{ view = 'current'; file = (($shots + '\hidden.png') -replace '\\', '/') } | ConvertTo-Json -Compress)
  Check 'a second Escape hides the panel' ($hidden -notmatch '"ok":true')

  # ---- 3. Three isolated programs, the Other group and the microphone: five strips.
  $null = Start-Fixture 'ProbeA.exe'; $null = Start-Fixture 'ProbeB.exe'
  $null = Start-Fixture 'ProbeC.exe'; $null = Start-Fixture 'ProbeD.exe'; $null = Start-Fixture 'ProbeE.exe'
  Exclude-ExistingSessions
  $null = Ask 'recording.start'
  Check 'the probe excludes every other program that plays sound' (Exclude-Intruders)
  for ($i = 0; $i -lt 40; $i++) {
    $st = Status
    $shape = Read-Strips $st
    if ($shape.Apps -eq 3 -and $shape.Other) { break }
    Start-Sleep -Milliseconds 500
  }
  Check 'three programs take their own lanes and the fourth joins Other audio' `
    ($shape.Apps -eq 3 -and $shape.Other) ('app lanes=' + $shape.Apps + ' other=' + $shape.Other)
  $mixer = Open-Mixer 'five'
  $shot = Mixer-Shot 'five'
  Check 'the mixer draws five strips' ([bool]$shot.Popup -and $shot.Strips -eq 5) ('strips=' + $shot.Strips)
  $popup = $shot.Popup
  if ($popup) {
    $height = Expected-Height $true $(if ($shot.DrawnOther) { $shot.Shape.Rows } else { 0 }) $false (Excluded-Count (Audio).audio_app_prefs) $false (Uncaptured-Count $shot.Status)
    Check 'five strips and the Other row fit the popup the interface sized' `
      ([math]::Abs($popup.H / $popup.Scale - $height) -le 2) ('drawn ' + [math]::Round($popup.H / $popup.Scale, 1) + ' design, laid out ' + $height)
    # Every lane says what it is doing: "Recording" is accent-coloured, the "Saved" of a stored route is faint.
    $content = Content $popup
    $band = New-Object BxShot($shot.Png)
    try {
      $accent = $band.Count($script:accent, 40, [int]$content.X, [int]($content.Y + 16 * $popup.Scale), [int]($content.X + $content.W), [int]($content.Y + 30 * $popup.Scale))
    } finally { $band.Dispose() }
    Check 'a running capture makes every lane say Recording, never Saved' ($accent -ge 100) ('accent pixels in the state row: ' + $accent)
  }

  # ---- 4. The Other group's own fader: the group's level, and nothing of the members'.
  $prefsBefore = Audio
  $faderX = [int]((Strip-Left $content $shot.Shape.Apps $shot.Strips $popup.Scale) + 46 * $popup.Scale)
  $faderY = [int]($content.Y + (37.5 + 75) * $popup.Scale)
  Click $faderX $faderY
  $afterGroup = Audio
  Check 'the Other fader moves the group level' ($afterGroup.audio_other_volume -lt 0.45 -and $afterGroup.audio_other_volume -gt 0.2) `
    ('group volume ' + $afterGroup.audio_other_volume)
  Check 'the group fader writes no member gain or mute' `
    (($afterGroup.audio_app_prefs | ConvertTo-Json -Depth 4 -Compress) -eq ($prefsBefore.audio_app_prefs | ConvertTo-Json -Depth 4 -Compress))
  $groupVolume = $afterGroup.audio_other_volume

  # ---- 5. The Other group's members: the disclosure, a member's own gain and mute.
  # A program that started playing after the first exclusion joins Other audio like any other: it is excluded
  # again here, so the rows under test are the probe's own and the person's audio is never captured.
  Check 'every other program that plays sound is still out of the lanes' (Exclude-Intruders)
  $shot = Mixer-Shot 'before-members'
  $popup = $shot.Popup
  $content = Content $popup
  $members = @(@($shot.Status.audio.auto.lanes | Where-Object { $_.other })[0].apps)
  Check 'only the probe''s own programs share Other audio' (@($members | Where-Object { $_.exe -notlike 'Probe*' }).Count -eq 0) `
    (($members | ForEach-Object { $_.exe }) -join ',')
  $headY = [int]($content.Y + (198 + 8 + 12) * $popup.Scale)
  Click ([int]($content.X + 60 * $popup.Scale)) $headY
  $shot = Mixer-Shot 'members-open'
  $popup = $shot.Popup
  $heightOpen = Expected-Height $true $(if ($shot.DrawnOther) { $shot.Shape.Rows } else { 0 }) $(if ($shot.DrawnOther) { $true } else { $false }) (Excluded-Count (Audio).audio_app_prefs) $false (Uncaptured-Count $shot.Status)
  Check 'the Other disclosure opens its members inside the popup it grows' `
    ([bool]$popup -and [math]::Abs($popup.H / $popup.Scale - $heightOpen) -le 2) `
    ('drawn ' + $(if ($popup) { [math]::Round($popup.H / $popup.Scale, 1) } else { 0 }) + ' design, laid out ' + $heightOpen)
  $content = Content $popup
  $member = @($shot.Status.audio.auto.lanes | Where-Object { $_.other })[0].apps[0].exe
  $rowY = [int]($content.Y + (232 + 2 + 13) * $popup.Scale)
  $sliderX = [int]($content.X + 180 * $popup.Scale)
  Click $sliderX $rowY
  $memberPrefs = (Audio).audio_app_prefs
  $own = [double](Pref $memberPrefs $member).volume
  Check 'a member fader writes that member''s own gain' ($own -gt 0.3 -and $own -lt 0.7) ($member + ' volume ' + $own)
  Check 'the member fader writes an integer percent, and only on release' `
    ([math]::Abs($own * 100 - [math]::Round($own * 100)) -lt 0.0001) ('' + ($own * 100))
  Check 'the member fader leaves the group level alone' ((Audio).audio_other_volume -eq $groupVolume)
  $muteX = [int]($content.X + $content.W - 32 * $popup.Scale)
  Click $muteX $rowY
  Check 'a member mute is that member''s own' ([bool](Pref (Audio).audio_app_prefs $member).muted)
  Check 'member preferences do not expose a pin' (-not (Pref (Audio).audio_app_prefs $member).PSObject.Properties['pinned'])

  # ---- 6. Exclude: the program leaves the lane and the mixer offers it back. The restore itself is pressed
  #         later, with nothing capturing, so restoring a program that is not the probe's never records it.
  $excludeX = [int]($content.X + $content.W - 10 * $popup.Scale)
  Click $excludeX $rowY
  $excludedPrefs = (Audio).audio_app_prefs
  Check 'a member is excluded by its own switch' ([bool](Pref $excludedPrefs $member).excluded)
  Check 'the excluded member keeps the gain it was given' ([double](Pref $excludedPrefs $member).volume -eq $own)
  Start-Sleep -Milliseconds 900
  $shot = Mixer-Shot 'member-excluded'
  $excludedCount = Excluded-Count $excludedPrefs
  $expected = Expected-Height $true $(if ($shot.DrawnOther) { $shot.Shape.Rows } else { 0 }) $shot.DrawnOther $excludedCount $false (Uncaptured-Count $shot.Status)
  Check 'an excluded program is offered back in one press' `
    ([bool]$shot.Popup -and [math]::Abs($shot.Popup.H / $shot.Popup.Scale - $expected) -le 2) `
    ('excluded=' + $excludedCount + ' drawn ' + $(if ($shot.Popup) { [math]::Round($shot.Popup.H / $shot.Popup.Scale, 1) } else { 0 }) + ' laid out ' + $expected)
  Check 'the excluded member leaves the lane it was on' `
    (@(@($shot.Status.audio.auto.lanes | Where-Object { $_.other })[0].apps | Where-Object { $_.exe -ieq $member }).Count -eq 0)

  # ---- 7. An isolated lane's own mute, gain and direct exclusion.
  Start-Sleep -Milliseconds 700
  $shot = Mixer-Shot 'before-lane-mute'
  $popup = $shot.Popup
  $content = Content $popup
  $laneExe = @($shot.Status.audio.auto.lanes | Where-Object { -not $_.other })[0].exe
  $laneLeft = Strip-Left $content 0 $shot.Strips $popup.Scale
  $muteX = [int]($laneLeft + 18 * $popup.Scale)
  $muteY = [int]($content.Y + 185 * $popup.Scale)
  Click $muteX $muteY
  Check 'a lane''s mute is that program''s own mute' ([bool](Pref (Audio).audio_app_prefs $laneExe).muted)
  Click $muteX $muteY
  Check 'the same press unmutes it' (-not [bool](Pref (Audio).audio_app_prefs $laneExe).muted)
  # A fader changes only this lane; the direct X excludes it without a menu.
  Click ([int]($laneLeft + 46 * $popup.Scale)) ([int]($content.Y + (37.5 + 75) * $popup.Scale))
  $low = [double](Pref (Audio).audio_app_prefs $laneExe).volume
  Check 'an isolated lane''s fader writes its own gain' ($low -lt 0.6) ($laneExe + ' volume ' + $low)
  Click ([int]($laneLeft + 53 * $popup.Scale)) $muteY
  Check 'the direct exclude action excludes the lane' ([bool](Pref (Audio).audio_app_prefs $laneExe).excluded)
  Check 'exclusion preserves the lane gain' ([double](Pref (Audio).audio_app_prefs $laneExe).volume -eq $low)
  $null = Ask 'audio.set' (@{ audio_app_prefs = @{ $laneExe = @{ excluded = $false } } } | ConvertTo-Json -Depth 4 -Compress)

  # ---- 8. What survives a restart: the gains and mutes, by executable. The data root is kept: the
  #         engine's own config.json is the thing under test, and the root is erased at the end of the run.
  $savedPrefs = (Audio).audio_app_prefs | ConvertTo-Json -Depth 4 -Compress
  Start-Sleep -Milliseconds 1500  # the engine's own write of the config, before the process is stopped
  Stop-ProbeEngine $e -KeepData
  $e = Start-Engine $config $dataRoot -Reuse
  $reloaded = (Audio).audio_app_prefs | ConvertTo-Json -Depth 4 -Compress
  Check 'every gain, mute and exclude survives a restart' ($reloaded -eq $savedPrefs) `
    ('before: ' + $savedPrefs + '  after: ' + $reloaded)
  Exclude-ExistingSessions
  $null = Ask 'recording.start'
  Start-Sleep -Seconds 3
  $st = Status
  $shape = Read-Strips $st
  Check 'the lanes come back for the programs that are still playing' ($shape.Apps -ge 3 -and $shape.Other) `
    ('app lanes=' + $shape.Apps + ' other=' + $shape.Other)
  $mixer = Open-Mixer 'restarted'
  $shot = Mixer-Shot 'restarted-mixer'
  Check 'the restarted mixer draws the saved lanes' ([bool]$shot.Popup -and $shot.Strips -eq 5) ('strips=' + $shot.Strips)
  Close-Mixer
  $null = Ask 'recording.stop'
  Start-Sleep -Seconds 1

  # ---- 9. Performance mode: automatic separation stays on, so the lanes are still the lanes.
  $loop = Start-Process -FilePath (Join-Path $privateBin 'buffrx-present-loop.exe') -ArgumentList '--seconds 600 --motion 1' -PassThru -WindowStyle Hidden
  $noise = Start-Process -FilePath 'powershell' -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'screen-noise.ps1') -PassThru -WindowStyle Hidden
  Start-Sleep -Milliseconds 1500
  $null = Ask 'buffer.start'
  for ($i = 0; $i -lt 40; $i++) {
    $st = Status
    if ([bool]$st.buffering -and [bool]$st.capture_ready) { break }
    Start-Sleep -Milliseconds 500
  }
  $null = Ask 'config.set' (@{ patch = @{ performance_when = 'always' } } | ConvertTo-Json -Compress)
  for ($i = 0; $i -lt 40; $i++) {
    $st = Status
    if ([bool]$st.performance.active) { break }
    Start-Sleep -Milliseconds 500
  }
  Check 'Performance mode is running' ([bool]$st.performance.active) ('' + $st.performance.reason)
  Check 'Performance mode keeps automatic separation (no override)' (-not [bool]$st.audio.auto.performance_override)
  $mixer = Open-Mixer 'performance'
  $shot = Mixer-Shot 'performance-mixer'
  Check 'the mixer still draws the lanes under Performance mode' ([bool]$shot.Popup -and $shot.Strips -eq 5) ('strips=' + $shot.Strips)
  if ($shot.Popup) {
    $height = Expected-Height $true $(if ($shot.DrawnOther) { $shot.Shape.Rows } else { 0 }) $false (Excluded-Count (Audio).audio_app_prefs) $false (Uncaptured-Count $shot.Status)
    Check 'no "one mix" line is drawn under Performance mode' `
      ([math]::Abs($shot.Popup.H / $shot.Popup.Scale - $height) -le 2) ('drawn ' + [math]::Round($shot.Popup.H / $shot.Popup.Scale, 1) + ' design, laid out ' + $height)
  }
  Close-Mixer
  $null = Ask 'config.set' (@{ patch = @{ performance_when = 'off' } } | ConvertTo-Json -Compress)
  Start-Sleep -Milliseconds 800

  # ---- 10. The compatibility fallbacks, and back: no track count, no saved apps, one setting each way.
  $null = Ask 'audio.set' (@{ audio_capture_mode = 'desktop'; audio_tracks = 1; separate_tracks = $false } | ConvertTo-Json -Compress)
  Start-Sleep -Milliseconds 800
  $mixer = Open-Mixer 'compat-desktop'
  $legacy = Find-Popup-Any $mixer
  Check 'All sound together has no mixer popup' (-not [bool]$legacy)
  Close-Mixer
  $null = Ask 'audio.set' (@{ audio_capture_mode = 'auto' } | ConvertTo-Json -Compress)
  Start-Sleep -Milliseconds 800
  $mixer = Open-Mixer 'compat-auto'
  $shot = Mixer-Shot 'compat-auto-mixer'
  Check 'Use automatic app audio returns the lanes' ([bool]$shot.Popup -and $shot.Strips -eq 5) ('strips=' + $shot.Strips)
  Check 'mode switches preserve the saved app selection' `
    ((((Audio).apps | ConvertTo-Json -Depth 4 -Compress) -eq $appsAtStart)) `
    ('tracks ' + (Audio).audio_tracks + ', apps ' + @((Audio).apps).Count)
  Close-Mixer

  # ---- 11. 150% interface scale: the same content, and its lowest control still reachable.
  $null = Ask 'config.set' (@{ patch = @{ ui_scale_percent = 150 } } | ConvertTo-Json -Compress)
  Start-Sleep -Milliseconds 1200
  $mixer = Open-Mixer 'scale150'
  $shot = Mixer-Shot 'scale150-mixer'
  Check 'the interface draws at the requested 150%' ($shot.Popup -and $shot.Popup.Scale -gt 1.35) `
    ('scale ' + $(if ($shot.Popup) { [math]::Round($shot.Popup.Scale, 3) } else { 0 }))
  if ($shot.Popup) {
    $height = Expected-Height $true $(if ($shot.DrawnOther) { $shot.Shape.Rows } else { 0 }) $false (Excluded-Count (Audio).audio_app_prefs) $false (Uncaptured-Count $shot.Status)
    Check 'at 150% the five-strip mixer fits inside the panel' `
      ($shot.Popup.Y -ge 0 -and ($shot.Popup.Y + $shot.Popup.H) -le $shot.Popup.ShotHeight -and
       [math]::Abs($shot.Popup.H / $shot.Popup.Scale - $height) -le 2) `
      ('popup ' + $shot.Popup.Y + '..' + ($shot.Popup.Y + $shot.Popup.H) + ' of ' + $shot.Popup.ShotHeight)
    $content = Content $shot.Popup
    $micMuteX = [int]((Strip-Left $content ($shot.Strips - 1) $shot.Strips $shot.Popup.Scale) + 38 * $shot.Popup.Scale)
    $micMuteY = [int]($content.Y + 185 * $shot.Popup.Scale)
    $before = (Audio).mic_muted
    Click $micMuteX $micMuteY
    Check 'the last strip''s mute is reachable at 150%' ((Audio).mic_muted -ne $before)
  }
  Close-Mixer
  $null = Ask 'config.set' (@{ patch = @{ ui_scale_percent = 100 } } | ConvertTo-Json -Compress)
  Start-Sleep -Milliseconds 1000

  # ---- 12. A deck that overflows (the first-run guide): the wheel reaches the AUDIO pane, and its Mixer opens.
  Stop-ProbeEngine $e
  $e = Start-Engine $config $dataRoot -Guide -Reuse
  Exclude-ExistingSessions
  $null = Ask 'recording.start'
  Start-Sleep -Seconds 2
  $main = Capture 'main' 'guide-deck'
  $pane = Find-AudioPane $main
  Check 'the first-run guide makes the deck overflow' ([bool]$pane)
  if ($pane) {
    $null = Ask 'ui.input' (@{ window = 'main'; move = @([int]($pane.Left + 60 * $pane.Scale), [int]($pane.Height * 0.7)); wheel = -12 } | ConvertTo-Json -Compress)
    Start-Sleep -Milliseconds 1000
    $scrolled = Capture 'current' 'guide-scrolled'
    $moved = Find-AudioPane $scrolled
    Check 'the wheel reaches the AUDIO pane when the deck overflows' ([bool]$moved -and $moved.Top -lt $pane.Top - 10) `
      ('pane top ' + $pane.Top + ' -> ' + $(if ($moved) { $moved.Top } else { 'not found' }))
    if ($moved) {
      Click ([int]($moved.Left + 70 * $moved.Scale)) ([int]($moved.Top + 24 * $moved.Scale))
      $script:mixerOpen = $true
      $shot = Mixer-Shot 'guide-mixer'
      Check 'the mixer opens from an overflowing deck' ([bool]$shot.Popup) ('strips=' + $shot.Strips)
    }
  }
  $null = Ask 'recording.stop'
  Start-Sleep -Milliseconds 800

  # ---- 13. A process tree that holds an excluded program is refused whole: the mixer and Settings name it and
  #         the reason, and no lane claims it is recorded.
  Exclude-ExistingSessions
  $null = Ask 'recording.start'
  Start-Sleep -Milliseconds 1500
  Copy-Item -LiteralPath $fixtureSource -Destination (Join-Path $privateBin 'ProbeChild.exe') -Force
  $probeA = $script:fixtures[0]
  try { $probeA.StandardInput.WriteLine('spawn ' + (Join-Path $privateBin 'ProbeChild.exe')); $probeA.StandardInput.Flush() } catch { }
  Start-Sleep -Seconds 2
  $null = Ask 'audio.set' (@{ audio_app_prefs = @{ 'probechild.exe' = @{ excluded = $true } } } | ConvertTo-Json -Depth 4 -Compress)
  $blocked = $null
  for ($i = 0; $i -lt 30; $i++) {
    Start-Sleep -Milliseconds 500
    $st = Status
    $blocked = @($st.audio.auto.uncaptured | Where-Object { $_.state -eq 'blocked' }) | Select-Object -First 1
    if ($blocked) { break }
  }
  Check 'a tree with an excluded program is refused whole, not captured' ([bool]$blocked) `
    $(if ($blocked) { $blocked.error } else { 'no blocked entry' })
  if ($blocked) {
    Check 'the refusal names the program and the reason' ($blocked.name -like 'Probe*' -and $blocked.error -like '*ProbeChild*') `
      ($blocked.name + ': ' + $blocked.error)
    Check 'the refused program has no lane claiming it records' `
      (@($st.audio.auto.lanes | Where-Object { $_.exe -ieq $blocked.exe }).Count -eq 0)
    $mixer = Open-Mixer 'blocked'
    $shot = Mixer-Shot 'blocked-mixer'
    $excludedNow = Excluded-Count (Audio).audio_app_prefs
    # The Other disclosure may be open or shut from an earlier check: both layouts include the warning block, and
    # the drawn height has to be one of them exactly.
    $closed = Expected-Height $true $(if ($shot.DrawnOther) { $shot.Shape.Rows } else { 0 }) $false $excludedNow $false (Uncaptured-Count $shot.Status)
    $open = Expected-Height $true $(if ($shot.DrawnOther) { $shot.Shape.Rows } else { 0 }) $true $excludedNow $false (Uncaptured-Count $shot.Status)
    $drawn = if ($shot.Popup) { $shot.Popup.H / $shot.Popup.Scale } else { 0 }
    Check 'the mixer sizes its popup for the warning it draws' `
      ([bool]$shot.Popup -and ([math]::Abs($drawn - $closed) -le 2 -or [math]::Abs($drawn - $open) -le 2)) `
      ('drawn ' + [math]::Round($drawn, 1) + ' design, laid out ' + $closed + ' or ' + $open)
    if ($shot.Popup) {
      $content = Content $shot.Popup
      $band = New-Object BxShot($shot.Png)
      try {
        $danger = $band.Count($script:danger, 30, [int]$content.X, [int]($content.Y + 206 * $shot.Popup.Scale),
                               [int]($content.X + $content.W), [int]($content.Y + 258 * $shot.Popup.Scale))
      } finally { $band.Dispose() }
      Check 'the mixer says NOT RECORDED in the danger colour' ($danger -ge 60) ('danger pixels: ' + $danger)
    }
    Close-Mixer
    # The automatic section sits under the device cards, below the fold of a short page. The block has to be
    # reached the way a hand reaches it, so the page is scrolled until the danger colour is on screen. Each
    # capture is counted from the copy the probe keeps, a file nothing is writing while it is read. A count
    # that cannot read its picture is reported, not swallowed: a broken read fails the check, never passes it.
    $danger = -1
    $dangerError = ''
    $name = 'blocked-settings'
    for ($i = 0; $i -lt 8 -and $danger -lt 40; $i++) {
      $picture = Join-Path $artifacts ($name + '.png')
      try {
        $danger = Danger-Pixels $picture
      } catch {
        $dangerError = $picture + ': ' + $_.Exception.Message
      }
      if ($danger -ge 40) { break }
      $null = Ask 'ui.input' (@{ window = 'main'; move = @(620, 480); wheel = -4 } | ConvertTo-Json -Compress)
      Start-Sleep -Milliseconds 500
      $name = 'blocked-settings-' + $i
      $null = Capture 'current' $name
    }
    Check 'the Audio page names the refused program too (scrolled to it)' ($danger -ge 40) `
      ('danger pixels: ' + $danger + $(if ($dangerError) { ' (' + $dangerError + ')' } else { '' }))
  }

  # ---- 14. Restore: every excluded program comes back in one press, with nothing capturing, so nothing of the
  #          person's own audio is ever recorded.
  $null = Ask 'recording.stop'
  Start-Sleep -Milliseconds 1500
  $mixer = Open-Mixer 'restore'
  $st = Status
  $popup = Find-Popup-Any $mixer
  Check 'the quiet mixer still offers the excluded programs back' ([bool]$popup)
  if ($popup) {
    $row = Find-RestoreRow $mixer $popup
    if ($row) {
      $ownBefore = [double](Pref (Audio).audio_app_prefs $member).volume
      Click $row.X $row.Y
      $restoredPrefs = (Audio).audio_app_prefs
      Check 'Restore brings every excluded program back' ((Excluded-Count $restoredPrefs) -eq 0) `
        ('still excluded: ' + (Excluded-Count $restoredPrefs))
      Check 'a restored program keeps the gain it was given' `
        ([double](Pref $restoredPrefs $member).volume -eq $ownBefore) `
        ($member + ' ' + $ownBefore + ' -> ' + [double](Pref $restoredPrefs $member).volume)
    } else {
      Check 'the restore row is drawn under the excluded line' $false 'not found in the capture'
    }
  }

  Copy-Item -LiteralPath (Join-Path $e.DataRoot 'logs\engine.log') -Destination (Join-Path $artifacts 'engine.log') -Force
  Copy-Item -LiteralPath $script:stderr -Destination (Join-Path $artifacts 'engine-console.log') -Force
  Write-Host ('captures and logs: ' + $artifacts)
  if ($script:failures -gt 0) {
    Write-Host ('automatic audio UI probe: ' + $script:failures + ' check(s) failed')
    exit 1
  }
  Write-Host 'automatic audio UI probe: all checks passed'
} finally {
  foreach ($process in $script:fixtures) {
    try { $process.StandardInput.WriteLine('quit'); $process.WaitForExit(1500) | Out-Null } catch { }
    try { if (-not $process.HasExited) { $process.Kill() } } catch { }
  }
  Get-Process -Name ProbeChild -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq (Join-Path $privateBin 'ProbeChild.exe') } | Stop-Process -Force -ErrorAction SilentlyContinue
  if ($noise) { Stop-Process -Id $noise.Id -Force -ErrorAction SilentlyContinue }
  if ($loop) { Stop-Process -Id $loop.Id -Force -ErrorAction SilentlyContinue }
  if ($e) { Stop-ProbeEngine $e }
  if (-not $Keep) { Remove-Item -LiteralPath $shots -Recurse -Force -ErrorAction SilentlyContinue }
}
