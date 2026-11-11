# Draws the installer's wizard art from the site's own logo files, so the setup wears the wordmark that
# buffrx.app shows instead of a lookalike. Run by tools/installer.ps1; output is tools/installer-art/*.png.
#
# The wordmark is site-main/buffrx-logo.svg (mark + letters + the custom X), the corner mark is
# brand/buffrx-mark.svg. Both are filled verbatim: the X keeps its chrome gradient across its own bounds.
# Colours are Clutch's (src/engine/themes.h): the ground it is drawn on.
#
# The parser knows the commands these two files use: M, L, H, V, C, Z.

param([string]$OutDir = '')

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing | Out-Null

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if ($OutDir -eq '') { $OutDir = Join-Path $PSScriptRoot 'installer-art' }
New-Item -ItemType Directory -Force $OutDir | Out-Null

$ground = [System.Drawing.ColorTranslator]::FromHtml('#0b0c0d')
$chromeFrom = [System.Drawing.ColorTranslator]::FromHtml('#fcfcfc')
$chromeTo = [System.Drawing.ColorTranslator]::FromHtml('#c3c3c3')

function Convert-PathToShape {
  param([string]$D)
  $tokens = @([regex]::Matches($D, '[MLCZHV]|-?\d+(?:\.\d+)?') | ForEach-Object { $_.Value })
  $shape = New-Object System.Drawing.Drawing2D.GraphicsPath
  $i = 0
  $cur = [System.Drawing.PointF]::new(0, 0)
  $start = [System.Drawing.PointF]::new(0, 0)
  while ($i -lt $tokens.Count) {
    $cmd = $tokens[$i]
    $i++
    if ($cmd -eq 'M') {
      $cur = [System.Drawing.PointF]::new([float]$tokens[$i], [float]$tokens[$i + 1]); $i += 2
      $start = $cur
      $shape.StartFigure()
    } elseif ($cmd -eq 'L') {
      $p = [System.Drawing.PointF]::new([float]$tokens[$i], [float]$tokens[$i + 1]); $i += 2
      $shape.AddLine($cur, $p); $cur = $p
    } elseif ($cmd -eq 'H') {
      $p = [System.Drawing.PointF]::new([float]$tokens[$i], $cur.Y); $i++
      $shape.AddLine($cur, $p); $cur = $p
    } elseif ($cmd -eq 'V') {
      $p = [System.Drawing.PointF]::new($cur.X, [float]$tokens[$i]); $i++
      $shape.AddLine($cur, $p); $cur = $p
    } elseif ($cmd -eq 'C') {
      $c1 = [System.Drawing.PointF]::new([float]$tokens[$i], [float]$tokens[$i + 1]); $i += 2
      $c2 = [System.Drawing.PointF]::new([float]$tokens[$i], [float]$tokens[$i + 1]); $i += 2
      $p = [System.Drawing.PointF]::new([float]$tokens[$i], [float]$tokens[$i + 1]); $i += 2
      $shape.AddBezier($cur, $c1, $c2, $p); $cur = $p
    } elseif ($cmd -eq 'Z') {
      $shape.CloseFigure(); $cur = $start
    }
  }
  return $shape
}

function Read-Svg {
  param([string]$File)
  $svg = Get-Content -LiteralPath $File -Raw
  $shapes = @()
  foreach ($m in [regex]::Matches($svg, '<path\b([^>]*)>')) {
    $attrs = $m.Groups[1].Value
    $d = [regex]::Match($attrs, '\bd="([^"]+)"').Groups[1].Value
    if ($d -eq '') { continue }
    $fill = [regex]::Match($attrs, 'fill="([^"]+)"').Groups[1].Value
    $shape = Convert-PathToShape $d
    if ($shape.PointCount -eq 0) { continue }
    $shapes += [pscustomobject]@{ Shape = $shape; Fill = $fill }
  }
  if ($shapes.Count -eq 0) { throw "no paths in $File" }
  return $shapes
}

# One artistic unit is 164 wide: the panel is 164x314, the corner mark 55, both scaled for DPI.
function Draw-Logo {
  param($g, $shapes, [float]$left, [float]$top, [float]$width)
  $bounds = $shapes[0].Shape.GetBounds()
  foreach ($s in $shapes) { $bounds = [System.Drawing.RectangleF]::Union($bounds, $s.Shape.GetBounds()) }
  $scale = $width / $bounds.Width
  foreach ($s in $shapes) {
    $matrix = [System.Drawing.Drawing2D.Matrix]::new($scale, 0, 0, $scale, ($left - $bounds.X * $scale), ($top - $bounds.Y * $scale))
    $shape = $s.Shape.Clone()
    $shape.Transform($matrix)
    if ($s.Fill -like 'url(*') {
      # the wordmark's X carries the chrome wash; the rest of the mark is flat white
      $b = $shape.GetBounds()
      $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush($b, $script:chromeFrom, $script:chromeTo, 0.0)
    } else {
      $brush = New-Object System.Drawing.SolidBrush($script:chromeFrom)
    }
    $g.FillPath($brush, $shape)
    $brush.Dispose()
    $shape.Dispose()
    $matrix.Dispose()
  }
  return ($bounds.Height * $scale)
}

function New-Panel {
  param([string]$File, [int]$Scale, [switch]$MarkOnly)
  $w = 164 * $Scale
  $h = 314 * $Scale
  if ($MarkOnly) { $w = 55 * $Scale; $h = 55 * $Scale }
  $bmp = New-Object System.Drawing.Bitmap($w, $h)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
  $g.Clear($script:ground)

  if ($MarkOnly) {
    $mw = 34 * $Scale
    $mh = 0
    # measured before drawing: the same shapes, so the mark can be centred
    $b = $script:markShapes[0].Shape.GetBounds()
    foreach ($s in $script:markShapes) { $b = [System.Drawing.RectangleF]::Union($b, $s.Shape.GetBounds()) }
    $mh = $b.Height * ($mw / $b.Width)
    Draw-Logo $g $script:markShapes ((55 * $Scale - $mw) / 2.0) ((55 * $Scale - $mh) / 2.0) $mw | Out-Null
  } else {
    # site-main/buffrx-logo.svg is 1542x418: the mark and the letters in one piece, as the site shows it
    $logoWidth = 138 * $Scale
    $logoHeight = $script:logoBounds.Height * ($logoWidth / $script:logoBounds.Width)
    Draw-Logo $g $script:logoShapes ((164 * $Scale - $logoWidth) / 2.0) ((314 * $Scale - $logoHeight) / 2.0) $logoWidth | Out-Null
  }
  $g.Dispose()
  $bmp.Save((Join-Path $OutDir $File), [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
  Write-Host ('  ' + $File)
}

$logoShapes = Read-Svg (Join-Path $root 'site-main\buffrx-logo.svg')
$markShapes = Read-Svg (Join-Path $root 'brand\buffrx-mark.svg')

$logoBounds = $logoShapes[0].Shape.GetBounds()
foreach ($s in $logoShapes) { $logoBounds = [System.Drawing.RectangleF]::Union($logoBounds, $s.Shape.GetBounds()) }

New-Panel 'wizard-164x314.png' 1
New-Panel 'wizard-328x628.png' 2
New-Panel 'wizard-492x942.png' 3
New-Panel 'wizard-small-55.png' 1 -MarkOnly
New-Panel 'wizard-small-110.png' 2 -MarkOnly
New-Panel 'wizard-small-165.png' 3 -MarkOnly
Write-Host ('installer art written to ' + $OutDir)
