# Export the flat vector-style artwork into the PS5 launcher PNG format.
# Original icon and drawing recipe are preserved in artwork/ for reference.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$source = Join-Path $PSScriptRoot 'artwork/icon-vector-master.png'
if (-not (Test-Path -LiteralPath $source)) { throw "Missing icon master: $source" }
$image = [System.Drawing.Image]::FromFile($source)
$bitmap = New-Object System.Drawing.Bitmap 512,512,([System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
try {
    if ($image.Width -ne $image.Height) { throw 'The icon master must be square.' }
    $graphics.Clear([System.Drawing.Color]::Black)
    $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $graphics.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $graphics.DrawImage($image,[System.Drawing.Rectangle]::new(0,0,512,512),0,0,$image.Width,$image.Height,[System.Drawing.GraphicsUnit]::Pixel)
    $destination = Join-Path $PSScriptRoot 'sce_sys'
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    $bitmap.Save((Join-Path $destination 'icon0.png'),[System.Drawing.Imaging.ImageFormat]::Png)
    Write-Host "Exported 512x512 RGB launcher icon: $destination/icon0.png"
} finally {
    $graphics.Dispose(); $bitmap.Dispose(); $image.Dispose()
}
