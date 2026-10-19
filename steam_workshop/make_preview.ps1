Add-Type -AssemblyName System.Drawing
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$outPath = Join-Path $root 'preview.png'

$bmp = New-Object System.Drawing.Bitmap(640, 360)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit

# Dark Soviet-red gradient background
$rect = New-Object System.Drawing.Rectangle(0, 0, 640, 360)
$brushBg = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
    $rect,
    [System.Drawing.Color]::FromArgb(255, 115, 18, 18),
    [System.Drawing.Color]::FromArgb(255, 25, 25, 32),
    45.0
)
$g.FillRectangle($brushBg, $rect)

# Gold border
$penGold = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 235, 190, 60), 4)
$g.DrawRectangle($penGold, 12, 12, 616, 336)

# Center divider line representing Two States mode
$penDash = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(100, 235, 190, 60), 2)
$penDash.DashStyle = [System.Drawing.Drawing2D.DashStyle]::Dash
$g.DrawLine($penDash, 320, 20, 320, 340)

$fontTitle = New-Object System.Drawing.Font("Segoe UI", 32, [System.Drawing.FontStyle]::Bold)
$fontSub   = New-Object System.Drawing.Font("Segoe UI", 16, [System.Drawing.FontStyle]::Bold)
$fontBody  = New-Object System.Drawing.Font("Segoe UI", 13, [System.Drawing.FontStyle]::Bold)

$brushGold  = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 245, 205, 75))
$brushWhite = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 245, 245, 245))
$brushGrey  = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 215, 215, 220))

$sf = New-Object System.Drawing.StringFormat
$sf.Alignment = [System.Drawing.StringAlignment]::Center

$g.DrawString("REPUBLIC MP", $fontTitle, $brushGold, [System.Drawing.PointF]::new(320, 36), $sf)
$g.DrawString("WORKERS & RESOURCES: CO-OP MULTIPLAYER", $fontSub, $brushWhite, [System.Drawing.PointF]::new(320, 96), $sf)

$g.DrawString("* Mode 1: One State (Shared Treasury & Construction)", $fontBody, $brushGrey, [System.Drawing.PointF]::new(320, 150), $sf)
$g.DrawString("* Mode 2: Two States (West SSR & East SSR on 1 Map)", $fontBody, $brushGrey, [System.Drawing.PointF]::new(320, 182), $sf)
$g.DrawString("* 1-Click Steam P2P Join (No VPN / Port Forwarding)", $fontBody, $brushGrey, [System.Drawing.PointF]::new(320, 214), $sf)
$g.DrawString("* Full Lockstep Sync + 30x30 km Giant Map Expansion", $fontBody, $brushGrey, [System.Drawing.PointF]::new(320, 246), $sf)
$g.DrawString("* GitHub Releases & Steam Workshop Auto-Updater", $fontBody, $brushGold, [System.Drawing.PointF]::new(320, 290), $sf)

$bmp.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose()
$bmp.Dispose()
Write-Host "[OK] Generated $outPath"
