# Trim bottom uniform margin and downscale screenshots (ASCII only: PS 5.1 reads UTF-8 as GBK).
Add-Type -AssemblyName System.Drawing

$dir = 'D:\wb(cn)\BMS\docs\manual\shots'
$maxLong = 2000

Get-ChildItem -Path $dir -Filter *.png | ForEach-Object {
    $p = $_.FullName
    $src = [System.Drawing.Bitmap]::FromFile($p)
    try {
        $w = $src.Width; $h = $src.Height
        # reference colour: bottom-left pixel (page background of every page is uniform)
        $ref = $src.GetPixel(2, $h - 2)
        $tol = 6
        # Scan the WHOLE row (step 8px): a fixed footer (buttons left, blank middle)
        # is content even though the centre and right edge are background.
        $lastContent = $h - 1
        for ($y = $h - 1; $y -ge 0; $y--) {
            $diff = $false
            for ($x = 1; $x -lt $w; $x += 8) {
                $c = $src.GetPixel($x, $y)
                if ([math]::Abs($c.R - $ref.R) -gt $tol -or
                    [math]::Abs($c.G - $ref.G) -gt $tol -or
                    [math]::Abs($c.B - $ref.B) -gt $tol) { $diff = $true; break }
            }
            if ($diff) { $lastContent = $y; break }
        }
        # Never trim a footer that sits at the very bottom: if content exists inside
        # the lowest 12% of the image, the blank band is layout spacing, not margin.
        $guard = [int]($h * 0.88)
        $footer = $false
        for ($y = $h - 1; $y -ge $guard; $y--) {
            for ($x = 1; $x -lt $w; $x += 8) {
                $c = $src.GetPixel($x, $y)
                if ([math]::Abs($c.R - $ref.R) -gt $tol -or
                    [math]::Abs($c.G - $ref.G) -gt $tol -or
                    [math]::Abs($c.B - $ref.B) -gt $tol) { $footer = $true; break }
            }
            if ($footer) { break }
        }
        if ($footer) { $lastContent = $h - 1 }
        # keep 16px breathing room below the last content row
        $newH = [math]::Min($h, $lastContent + 17)
        if ($newH -lt 200) { $newH = $h }

        $scale = 1.0
        $long = [math]::Max($w, $newH)
        if ($long -gt $maxLong) { $scale = $maxLong / $long }
        $ow = [int][math]::Round($w * $scale)
        $oh = [int][math]::Round($newH * $scale)

        $dst = New-Object System.Drawing.Bitmap($ow, $oh)
        $g = [System.Drawing.Graphics]::FromImage($dst)
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $g.DrawImage($src, (New-Object System.Drawing.Rectangle(0, 0, $ow, $oh)),
            (New-Object System.Drawing.Rectangle(0, 0, $w, $newH)),
            [System.Drawing.GraphicsUnit]::Pixel)
        $g.Dispose()

        $src.Dispose()
        $tmp = $p + '.tmp.png'
        $dst.Save($tmp, [System.Drawing.Imaging.ImageFormat]::Png)
        $dst.Dispose()
        Move-Item -Force $tmp $p
        Write-Output ("{0}: {1}x{2} -> {3}x{4}" -f $_.Name, $w, $h, $ow, $oh)
    } catch {
        try { $src.Dispose() } catch {}
        Write-Output ("{0}: SKIP {1}" -f $_.Name, $_.Exception.Message)
    }
}
