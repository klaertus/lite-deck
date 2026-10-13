# Pre-renders a scrolling marquee for the strip: one 800x112 JPEG per 2 px of scroll, rotated 180deg.
# Also renders key labels used to show the current test phase.
Add-Type -AssemblyName System.Drawing
$out = Join-Path $PSScriptRoot "img\frames"
New-Item -ItemType Directory -Force $out | Out-Null
Remove-Item (Join-Path $out "*.jpg") -ErrorAction SilentlyContinue

$codec = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq "image/jpeg" }
$params = New-Object System.Drawing.Imaging.EncoderParameters 1
$params.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter ([System.Drawing.Imaging.Encoder]::Quality), 80L

$text = "Shake It - Metro Station   *   Snipping Tool: screenshot saved   *   "
$font = New-Object System.Drawing.Font "Arial", 34, ([System.Drawing.FontStyle]::Bold)
$probe = New-Object System.Drawing.Bitmap 1, 1
$period = [int][System.Drawing.Graphics]::FromImage($probe).MeasureString($text, $font).Width

$i = 0
for ($off = 0; $off -lt $period; $off += 2) {
    $bmp = New-Object System.Drawing.Bitmap 800, 112
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.TextRenderingHint = "AntiAliasGridFit"
    $g.Clear([System.Drawing.Color]::Black)
    for ($x = -$off; $x -lt 800; $x += $period) { $g.DrawString($text, $font, [System.Drawing.Brushes]::White, $x, 30) }
    $g.Dispose()
    $bmp.RotateFlip([System.Drawing.RotateFlipType]::Rotate180FlipNone)
    $bmp.Save((Join-Path $out ("f{0:D4}.jpg" -f $i)), $codec, $params)
    $bmp.Dispose()
    $i++
}

function Label($text, $name) {
    $bmp = New-Object System.Drawing.Bitmap 112, 112
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::DarkSlateBlue)
    $f = New-Object System.Drawing.Font "Arial", 24, ([System.Drawing.FontStyle]::Bold)
    $fmt = New-Object System.Drawing.StringFormat
    $fmt.Alignment = "Center"; $fmt.LineAlignment = "Center"
    $g.DrawString($text, $f, [System.Drawing.Brushes]::White, (New-Object System.Drawing.RectangleF 0, 0, 112, 112), $fmt)
    $g.Dispose()
    $bmp.RotateFlip([System.Drawing.RotateFlipType]::Rotate180FlipNone)
    $bmp.Save((Join-Path $out $name), $codec, $params)
    $bmp.Dispose()
}
foreach ($p in "10", "20", "30", "MAX") { Label "$p`nfps" "label_$p.jpg" }

$files = Get-ChildItem $out -Filter "f*.jpg"
"{0} frames, period {1} px, avg {2:N0} bytes, max {3:N0} bytes" -f $files.Count, $period, ($files | Measure-Object Length -Average).Average, ($files | Measure-Object Length -Maximum).Maximum
