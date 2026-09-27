# Draws the original XReceiver tile mark with System.Drawing.
# No font files are embedded. The mark is two bars (an X) over a screen rectangle.
# Run from the repository root: powershell -File tools\generate-tiles.ps1

Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $PSScriptRoot
$outDir = Join-Path $root "XReceiver\Assets"

function Save-Mark([int]$width, [int]$height, [string]$name) {
    $bmp = New-Object System.Drawing.Bitmap $width, $height
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::FromArgb(255, 18, 20, 26))
    $penW = [Math]::Max(2, [int]($height / 18))
    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 232, 236, 242)), $penW
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $m = [int]($height * 0.22)
    $side = [Math]::Min($width, $height) - (2 * $m)
    $x0 = [int](($width - $side) / 2)
    $y0 = [int](($height - $side) / 2)
    $g.DrawLine($pen, $x0, $y0, ($x0 + $side), ($y0 + $side))
    $g.DrawLine($pen, ($x0 + $side), $y0, $x0, ($y0 + $side))
    $rx = $x0 + [int]($side * 0.28)
    $ry = $y0 + [int]($side * 0.62)
    $rw = [int]($side * 0.44)
    $rh = [int]($side * 0.16)
    $g.DrawRectangle($pen, $rx, $ry, $rw, $rh)
    $path = Join-Path $outDir $name
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose()
    $pen.Dispose()
    $bmp.Dispose()
}

Save-Mark 50 50 "StoreLogo.png"
Save-Mark 88 88 "Square44x44Logo.scale-200.png"
Save-Mark 24 24 "Square44x44Logo.targetsize-24_altform-unplated.png"
Save-Mark 300 300 "Square150x150Logo.scale-200.png"
Save-Mark 48 48 "LockScreenLogo.scale-200.png"
Save-Mark 620 300 "Wide310x150Logo.scale-200.png"
Save-Mark 1240 600 "SplashScreen.scale-200.png"

Get-ChildItem (Join-Path $outDir "*.png") | ForEach-Object {
    $img = [System.Drawing.Image]::FromFile($_.FullName)
    "{0} {1}x{2}" -f $_.Name, $img.Width, $img.Height
    $img.Dispose()
}
