# Draws the BuffrX mark (the chrome mark on a dark rounded tile, as brand/buffrx-icon.svg) and packs a multi-size .ico.
# Run: powershell -File tools/make-icon.ps1
# The icons are drawn, not exported from anything: no asset pipeline, no binary in the repo.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$out = Join-Path $PSScriptRoot '..\src-tauri\icons\icon.ico'
# 20, 24 and 40 are the tray and taskbar sizes at 125%, 150% and 250% scaling; without them Windows stretched a neighbour.
$sizes = 256, 64, 48, 40, 32, 24, 20, 16

# The mark's outline, the same path as brand/buffrx-mark.svg (absolute M L C Z only).
$markPath = 'M3.2 93L42.2 143.4L230 143.4C254.3 148.8 262 168.5 240 184.4C233.3 189.3 225.7 190.8 218 192.8L118.3 192.8L152.2 235.4L234 235.4C243.6 237.4 251.8 238.8 260 244.6C265.3 248.3 266.8 254.1 267.6 260C269.4 274.1 259.7 282.4 247 285.3L96 285.3L95.5 168L42.8 201.5L42.8 336.4L257 336.4C300.7 328.5 320.5 313.6 320.5 266C320.5 250 318.2 235.3 308 222.6C301.7 214.8 292 211.9 284.6 206C289.1 196.6 298.7 189.6 302.3 179C309.6 157.2 304.8 115.4 281 103.7C270.7 98.6 260.6 96.2 250 93L3.2 93Z'

function New-MarkPath {
  $path = New-Object System.Drawing.Drawing2D.GraphicsPath
  $tokens = [regex]::Matches($markPath, '[MLCZ]|-?[0-9.]+') | ForEach-Object { $_.Value }
  $i = 0; $pen = $null; $current = $null
  while ($i -lt $tokens.Count) {
    $command = $tokens[$i++]
    if ($command -eq 'M') { $current = @([single]$tokens[$i], [single]$tokens[$i + 1]); $i += 2; $path.StartFigure() }
    elseif ($command -eq 'L') {
      $next = @([single]$tokens[$i], [single]$tokens[$i + 1]); $i += 2
      $path.AddLine($current[0], $current[1], $next[0], $next[1]); $current = $next
    } elseif ($command -eq 'C') {
      $v = @(); for ($k = 0; $k -lt 6; $k++) { $v += [single]$tokens[$i + $k] }; $i += 6
      $path.AddBezier($current[0], $current[1], $v[0], $v[1], $v[2], $v[3], $v[4], $v[5]); $current = @($v[4], $v[5])
    } elseif ($command -eq 'Z') { $path.CloseFigure() }
  }
  return $path
}

# A dark rounded tile with the chrome mark on it (brand/buffrx-icon.svg). Drawn eight times larger and scaled down with a
# high-quality filter, so a 16 or 20 pixel icon in the tray is smooth instead of jagged; the small sizes also give the mark more
# of the tile, because a mark a third of a 16 pixel tile is a smudge (the user's tray icon, 2026-10-31).
function Draw-Mark([int]$size) {
  $big = $size * 8
  $bitmap = New-Object System.Drawing.Bitmap($big, $big)
  $g = [System.Drawing.Graphics]::FromImage($bitmap)
  $g.SmoothingMode = 'AntiAlias'
  $g.Clear([System.Drawing.Color]::Transparent)
  $radius = $big * 0.14 * 2
  $tile = New-Object System.Drawing.Drawing2D.GraphicsPath
  $tile.AddArc(0, 0, $radius, $radius, 180, 90)
  $tile.AddArc($big - $radius, 0, $radius, $radius, 270, 90)
  $tile.AddArc($big - $radius, $big - $radius, $radius, $radius, 0, 90)
  $tile.AddArc(0, $big - $radius, $radius, $radius, 90, 90)
  $tile.CloseFigure()
  $g.FillPath((New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 11, 12, 13))), $tile)
  $mark = New-MarkPath
  # the mark's box (x 3.2-320.5, y 93-336.4) is 317.3 wide; its share of the tile grows as the icon shrinks
  $share = if ($size -le 24) { 0.84 } elseif ($size -le 48) { 0.74 } else { 0.62 }
  $scale = $big * $share / 317.3
  $matrix = New-Object System.Drawing.Drawing2D.Matrix
  $matrix.Translate($big / 2, $big / 2)
  $matrix.Scale($scale, $scale)
  $matrix.Translate(-161.85, -214.7)
  $mark.Transform($matrix)
  $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
    (New-Object System.Drawing.PointF([single]($big / 2 - 159 * $scale), 0)),
    (New-Object System.Drawing.PointF([single]($big / 2 + 159 * $scale), 0)),
    [System.Drawing.Color]::FromArgb(255, 252, 252, 252), [System.Drawing.Color]::FromArgb(255, 195, 195, 195))
  $g.FillPath($brush, $mark)
  $g.Dispose()
  $small = New-Object System.Drawing.Bitmap($size, $size)
  $g2 = [System.Drawing.Graphics]::FromImage($small)
  $g2.InterpolationMode = 'HighQualityBicubic'
  $g2.PixelOffsetMode = 'HighQuality'
  $g2.SmoothingMode = 'HighQuality'
  $g2.Clear([System.Drawing.Color]::Transparent)
  $g2.DrawImage($bitmap, 0, 0, $size, $size)
  $g2.Dispose()
  $bitmap.Dispose()
  $stream = New-Object System.IO.MemoryStream
  $small.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
  $small.Dispose()
  # the comma keeps PowerShell from unrolling the byte array into the pipeline
  return ,$stream.ToArray()
}

$images = @{}
foreach ($size in $sizes) { $images[$size] = Draw-Mark $size }

# ICO container: 6-byte header, one 16-byte directory entry per image, then the PNG payloads.
$stream = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter($stream)
$writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
foreach ($size in $sizes) {
  $byte = if ($size -ge 256) { 0 } else { $size }
  $writer.Write([byte]$byte); $writer.Write([byte]$byte); $writer.Write([byte]0); $writer.Write([byte]0)
  $writer.Write([uint16]1); $writer.Write([uint16]32)
  $writer.Write([uint32]$images[$size].Length); $writer.Write([uint32]$offset)
  $offset += $images[$size].Length
}
foreach ($size in $sizes) { $writer.Write($images[$size]) }
$writer.Flush()
[System.IO.File]::WriteAllBytes($out, $stream.ToArray())
$writer.Dispose()
"icon written: $((Get-Item $out).Length) bytes"
