param(
    [Parameter(Mandatory = $true)]
    [string]$OutputDir
)

$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Drawing

$assets = @(
    @{ Name = "StoreLogo.png";          Width = 50;  Height = 50  },
    @{ Name = "Square150x150Logo.png";  Width = 150; Height = 150 },
    @{ Name = "Square44x44Logo.png";    Width = 44;  Height = 44  },
    @{ Name = "Wide310x150Logo.png";    Width = 310; Height = 150 },
    @{ Name = "SplashScreen.png";       Width = 620; Height = 300 }
)

foreach ($asset in $assets) {
    $path = Join-Path $OutputDir $asset.Name
    if (Test-Path $path) { continue }

    Write-Host "  Generating $($asset.Name) ($($asset.Width)x$($asset.Height))"

    $bmp = New-Object System.Drawing.Bitmap($asset.Width, $asset.Height)
    $g = [System.Drawing.Graphics]::FromImage($bmp)

    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.Point(0, 0)),
        (New-Object System.Drawing.Point($asset.Width, $asset.Height)),
        [System.Drawing.Color]::FromArgb(40, 40, 50),
        [System.Drawing.Color]::FromArgb(20, 20, 30)
    )
    $g.FillRectangle($brush, 0, 0, $asset.Width, $asset.Height)

    $fontSize = [Math]::Max(8, [Math]::Min($asset.Width, $asset.Height) * 0.35)
    $font = New-Object System.Drawing.Font("Consolas", $fontSize, [System.Drawing.FontStyle]::Bold)
    $textBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(180, 200, 200, 200))
    $format = New-Object System.Drawing.StringFormat
    $format.Alignment = [System.Drawing.StringAlignment]::Center
    $format.LineAlignment = [System.Drawing.StringAlignment]::Center
    $rect = New-Object System.Drawing.RectangleF(0, 0, $asset.Width, $asset.Height)
    $g.DrawString("MC", $font, $textBrush, $rect, $format)

    $g.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

Write-Host "  Assets generated"
