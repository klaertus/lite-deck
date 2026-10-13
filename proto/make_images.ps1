# Generates baseline JPEG test images for the N4 Pro probe (rotated 180deg as the device expects).
Add-Type -AssemblyName System.Drawing
$out = Join-Path $PSScriptRoot "img"
New-Item -ItemType Directory -Force $out | Out-Null

$codec = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq "image/jpeg" }
$params = New-Object System.Drawing.Imaging.EncoderParameters 1
$params.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter ([System.Drawing.Imaging.Encoder]::Quality), 85L

function Save($bmp, $name) {
    $bmp.RotateFlip([System.Drawing.RotateFlipType]::Rotate180FlipNone)
    $bmp.Save((Join-Path $out $name), $codec, $params)
    $bmp.Dispose()
}

function Label($w, $h, $text, $color, $name) {
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.TextRenderingHint = "AntiAliasGridFit"
    $g.Clear($color)
    $font = New-Object System.Drawing.Font "Arial", 26, ([System.Drawing.FontStyle]::Bold)
    $fmt = New-Object System.Drawing.StringFormat
    $fmt.Alignment = "Center"; $fmt.LineAlignment = "Center"
    $g.DrawString($text, $font, [System.Drawing.Brushes]::White, (New-Object System.Drawing.RectangleF 0, 0, $w, $h), $fmt)
    $g.Dispose()
    Save $bmp $name
}

# Keys: wire index -> label (top row 11..15, bottom row 6..10)
$colors = @("DarkRed", "DarkGreen", "DarkBlue", "DarkOrange", "Purple")
for ($i = 0; $i -lt 5; $i++) {
    Label 112 112 ("T{0}`n#{1}" -f ($i + 1), (11 + $i)) ([System.Drawing.Color]::FromName($colors[$i])) ("key{0}.jpg" -f (11 + $i))
    Label 112 112 ("B{0}`n#{1}" -f ($i + 1), (6 + $i)) ([System.Drawing.Color]::FromName($colors[$i])) ("key{0}.jpg" -f (6 + $i))
}
# Touch strip zones 1..4
for ($z = 1; $z -le 4; $z++) {
    Label 176 112 "ZONE $z" ([System.Drawing.Color]::FromName($colors[$z - 1])) "zone$z.jpg"
}

# Ruler image: gradient + tick every 50 px, labelled every 100 px
function Ruler($w, $h, $name, $title = "") {
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $rect = New-Object System.Drawing.Rectangle 0, 0, $w, $h
    $br = New-Object System.Drawing.Drawing2D.LinearGradientBrush $rect, ([System.Drawing.Color]::Navy), ([System.Drawing.Color]::DarkCyan), 0.0
    $g.FillRectangle($br, $rect)
    $font = New-Object System.Drawing.Font "Arial", 14, ([System.Drawing.FontStyle]::Bold)
    for ($x = 0; $x -lt $w; $x += 50) {
        $g.DrawLine([System.Drawing.Pens]::White, $x, 0, $x, $(if ($x % 100 -eq 0) { 30 } else { 15 }))
        if ($x % 100 -eq 0) { $g.DrawString("$x", $font, [System.Drawing.Brushes]::Yellow, $x + 2, 32) }
    }
    for ($y = 0; $y -lt $h; $y += 50) {
        $g.DrawLine([System.Drawing.Pens]::Orange, 0, $y, 20, $y)
        $g.DrawString("y$y", $font, [System.Drawing.Brushes]::Orange, 22, $y)
    }
    $g.DrawRectangle([System.Drawing.Pens]::Red, 0, 0, $w - 1, $h - 1)
    if ($title) {
        $big = New-Object System.Drawing.Font "Arial", 30, ([System.Drawing.FontStyle]::Bold)
        $g.DrawString($title, $big, [System.Drawing.Brushes]::White, 330, $h - 50)
    }
    $g.Dispose()
    Save $bmp $name
}
Ruler 800 112 "strip800.jpg"
Ruler 800 480 "face800x480.jpg"
Ruler 800 112 "stripA.jpg" "TEST A"
Ruler 800 112 "stripB.jpg" "TEST B"
Ruler 800 112 "stripC.jpg" "TEST C"
Ruler 800 480 "faceD.jpg" "TEST D"

Get-ChildItem $out | Select-Object Name, Length | Format-Table -AutoSize
