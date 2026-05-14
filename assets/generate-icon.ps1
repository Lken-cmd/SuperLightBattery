param(
    [string]$OutputPath = (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) "SuperLightBattery.ico")
)

$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Drawing

# Vertical battery silhouette in a 64x64 design grid (matches the tray icon
# geometry in src/superlightbattery.c so file-explorer and tray icons look
# like the same product).
function New-BatteryBitmap {
    param([int]$Size)

    $bmp = New-Object System.Drawing.Bitmap $Size, $Size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    try {
        $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
        $g.Clear([System.Drawing.Color]::Transparent)

        $s = $Size / 64.0

        function MkPath($x1, $y1, $x2, $y2, $r) {
            $rect = New-Object System.Drawing.RectangleF ([single]($x1 * $s)), ([single]($y1 * $s)), ([single](($x2 - $x1) * $s)), ([single](($y2 - $y1) * $s))
            $rad = [single]($r * $s)
            $d = $rad * 2
            $p = New-Object System.Drawing.Drawing2D.GraphicsPath
            if ($d -ge $rect.Width -or $d -ge $rect.Height) {
                $p.AddEllipse($rect)
                return $p
            }
            $p.AddArc($rect.X, $rect.Y, $d, $d, 180, 90)
            $p.AddArc($rect.X + $rect.Width - $d, $rect.Y, $d, $d, 270, 90)
            $p.AddArc($rect.X + $rect.Width - $d, $rect.Y + $rect.Height - $d, $d, $d, 0, 90)
            $p.AddArc($rect.X, $rect.Y + $rect.Height - $d, $d, $d, 90, 90)
            $p.CloseFigure()
            return $p
        }

        $shellColor   = [System.Drawing.Color]::FromArgb(255, 22, 27, 34)
        $surfaceColor = [System.Drawing.Color]::FromArgb(255, 245, 247, 250)
        $fillColor    = [System.Drawing.Color]::FromArgb(255, 32, 201, 117)
        $shadowColor  = [System.Drawing.Color]::FromArgb(70, 0, 0, 0)

        $shadowBrush  = New-Object System.Drawing.SolidBrush $shadowColor
        $shellBrush   = New-Object System.Drawing.SolidBrush $shellColor
        $surfaceBrush = New-Object System.Drawing.SolidBrush $surfaceColor
        $fillBrush    = New-Object System.Drawing.SolidBrush $fillColor

        $p = MkPath 10 8 58 63 10; $g.FillPath($shadowBrush, $p); $p.Dispose()
        $p = MkPath 22 0 42 10 4;  $g.FillPath($shellBrush,  $p); $p.Dispose()
        $p = MkPath 8 5 56 63 10;  $g.FillPath($shellBrush,  $p); $p.Dispose()
        $p = MkPath 15 12 49 57 7; $g.FillPath($surfaceBrush, $p); $p.Dispose()

        # Default static fill ~75% (bottom-up).
        $p = MkPath 19 25 45 53 5; $g.FillPath($fillBrush,   $p); $p.Dispose()

        $shadowBrush.Dispose()
        $shellBrush.Dispose()
        $surfaceBrush.Dispose()
        $fillBrush.Dispose()
    }
    finally {
        $g.Dispose()
    }

    return $bmp
}

function Get-PngBytes {
    param([System.Drawing.Bitmap]$Bitmap)
    $ms = New-Object System.IO.MemoryStream
    try {
        $Bitmap.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        return $ms.ToArray()
    }
    finally {
        $ms.Dispose()
    }
}

$sizes = @(16, 20, 24, 32, 40, 48, 64, 128, 256)
$entries = @()
foreach ($size in $sizes) {
    $bmp = New-BatteryBitmap -Size $size
    try {
        $png = Get-PngBytes $bmp
        $entries += [pscustomobject]@{ Size = $size; Data = $png }
    }
    finally {
        $bmp.Dispose()
    }
}

$headerSize = 6
$entrySize = 16
$dataOffset = $headerSize + ($entries.Count * $entrySize)

$ms = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter $ms
try {
    $bw.Write([uint16]0)
    $bw.Write([uint16]1)
    $bw.Write([uint16]$entries.Count)

    $cursor = $dataOffset
    foreach ($e in $entries) {
        $w = [byte]($(if ($e.Size -ge 256) { 0 } else { $e.Size }))
        $bw.Write([byte]$w)
        $bw.Write([byte]$w)
        $bw.Write([byte]0)
        $bw.Write([byte]0)
        $bw.Write([uint16]1)
        $bw.Write([uint16]32)
        $bw.Write([uint32]$e.Data.Length)
        $bw.Write([uint32]$cursor)
        $cursor += $e.Data.Length
    }

    foreach ($e in $entries) {
        $bw.Write($e.Data)
    }

    $bw.Flush()
    [System.IO.File]::WriteAllBytes($OutputPath, $ms.ToArray())
}
finally {
    $bw.Dispose()
    $ms.Dispose()
}

Write-Host "Wrote $OutputPath ($($entries.Count) sizes)"
