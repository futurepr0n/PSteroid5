# Draw the 512x512 launcher icon from vector primitives (no game textures).
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$bitmap = New-Object System.Drawing.Bitmap 512,512,([System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$graphics.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
$white = New-Object System.Drawing.Pen ([System.Drawing.Color]::White),7
$white.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
$rock = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(205,214,224)),5
$rock.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
$star = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(120,140,160))
$shot = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255,69,0))
$text = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::White)
$font = New-Object System.Drawing.Font 'Arial',52,([System.Drawing.FontStyle]::Bold),([System.Drawing.GraphicsUnit]::Pixel)
$format = New-Object System.Drawing.StringFormat
$format.Alignment = [System.Drawing.StringAlignment]::Center
function Rock($cx,$cy,$r,$seed) {
    $points = @()
    for($i=0;$i -lt 8;$i++) {
        $a = $i*[Math]::PI/4; $k = 0.78 + (($i*37+$seed*11)%23)/100
        $points += [System.Drawing.PointF]::new($cx+[Math]::Cos($a)*$r*$k,$cy+[Math]::Sin($a)*$r*$k)
    }
    $graphics.DrawPolygon($rock,[System.Drawing.PointF[]]$points)
}
try {
    $graphics.Clear([System.Drawing.Color]::Black)
    for($i=0;$i -lt 60;$i++) { $graphics.FillEllipse($star,($i*137+31)%512,($i*83+19)%512,2,2) }
    Rock 370 130 78 1; Rock 120 300 52 2; Rock 410 320 34 3; Rock 150 110 24 4
    $graphics.DrawPolygon($white,[System.Drawing.PointF[]]@(
        [System.Drawing.PointF]::new(250,170),[System.Drawing.PointF]::new(208,300),[System.Drawing.PointF]::new(292,300)))
    $graphics.FillEllipse($shot,281,118,12,12); $graphics.FillEllipse($shot,306,84,10,10)
    $graphics.DrawString('CSSTEROIDS',$font,$text,[System.Drawing.RectangleF]::new(0,392,512,70),$format)
    $destination=Join-Path $PSScriptRoot 'sce_sys'
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    $bitmap.Save((Join-Path $destination 'icon0.png'),[System.Drawing.Imaging.ImageFormat]::Png)
} finally {
    $format.Dispose(); $font.Dispose(); $text.Dispose(); $shot.Dispose(); $star.Dispose()
    $rock.Dispose(); $white.Dispose(); $graphics.Dispose(); $bitmap.Dispose()
}
