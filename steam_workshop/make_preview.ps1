$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$outPath = Join-Path $root 'preview.png'

if ((Test-Path $outPath) -and ((Get-Item $outPath).Length -gt 50000)) {
    Write-Host "[OK] Preserving existing stylized artwork at $outPath ($((Get-Item $outPath).Length) bytes)"
    exit 0
}

Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap(640, 360)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit

$rect = New-Object System.Drawing.Rectangle(0, 0, 640, 360)
$brushBg = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
    $rect,
    [System.Drawing.Color]::FromArgb(255, 115, 18, 18),
    [System.Drawing.Color]::FromArgb(255, 25, 25, 32),
    45.0
)
$g.FillRectangle($brushBg, $rect)

$penGold = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 235, 190, 60), 4)
$g.DrawRectangle($penGold, 12, 12, 616, 336)

$fontTitle = New-Object System.Drawing.Font("Segoe UI", 32, [System.Drawing.FontStyle]::Bold)
$fontSub   = New-Object System.Drawing.Font("Segoe UI", 16, [System.Drawing.FontStyle]::Bold)
$brushGold  = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 245, 205, 75))
$brushWhite = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 245, 245, 245))
$sf = New-Object System.Drawing.StringFormat
$sf.Alignment = [System.Drawing.StringAlignment]::Center

$g.DrawString("REPUBLIC MP", $fontTitle, $brushGold, [System.Drawing.PointF]::new(320, 120), $sf)
$g.DrawString("WORKERS & RESOURCES: CO-OP MULTIPLAYER", $fontSub, $brushWhite, [System.Drawing.PointF]::new(320, 190), $sf)

$bmp.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose()
$bmp.Dispose()
Write-Host "[OK] Generated fallback $outPath"
