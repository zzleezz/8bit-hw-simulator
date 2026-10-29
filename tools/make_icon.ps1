# make_icon.ps1 - the program's icon, from a screenshot of a board.
#
# Crops a square out of a screenshot (--screenshot), shrinks it to 256,
# 48, 32 and 16 pixels, and writes:
#   src/c64-hw-sim.ico  the icon in the .exe (src/icon.rc, build.bat)
#   src/icon_data.c     the same pixels for the window and the taskbar
#                       (sapp_desc.icon: sokol would show its own otherwise)
#
#   powershell -ExecutionPolicy Bypass -File tools\make_icon.ps1 shot.png [X Y SIZE]
#
# The icon in the repository: the left of the C64 board (CIAs, ROMs, CPU,
# PLA), from ./c64-hw-sim --machine c64 --frames 60 --screenshot shot.png,
# X 22, Y 253, SIZE 500.
param(
    [Parameter(Mandatory = $true)][string]$Shot,
    [int]$X = 22,
    [int]$Y = 253,
    [int]$Size = 500
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$src = Join-Path $root 'src'
$img = [System.Drawing.Bitmap]::FromFile((Resolve-Path $Shot))
$sizes = @(256, 48, 32, 16)
$pngs = @{}
$rgba = @{}
foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap($s, $s, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)
    $dst = New-Object System.Drawing.Rectangle(0, 0, $s, $s)
    $g.DrawImage($img, $dst, $X, $Y, $Size, $Size, [System.Drawing.GraphicsUnit]::Pixel)
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $pngs[$s] = $ms.ToArray()
    $bytes = New-Object byte[] ($s * $s * 4)
    $i = 0
    for ($py = 0; $py -lt $s; $py++) {
        for ($px = 0; $px -lt $s; $px++) {
            $c = $bmp.GetPixel($px, $py)
            $bytes[$i] = $c.R; $bytes[$i + 1] = $c.G; $bytes[$i + 2] = $c.B; $bytes[$i + 3] = $c.A
            $i += 4
        }
    }
    $rgba[$s] = $bytes
    $bmp.Dispose()
}
$img.Dispose()

# The .ico: a directory of PNG images (Windows Vista and later read them).
$ico = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter($ico)
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
foreach ($s in $sizes) {
    $d = if ($s -ge 256) { 0 } else { $s }
    $w.Write([byte]$d); $w.Write([byte]$d); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32)
    $w.Write([uint32]$pngs[$s].Length); $w.Write([uint32]$offset)
    $offset += $pngs[$s].Length
}
foreach ($s in $sizes) { $w.Write($pngs[$s]) }
$w.Flush()
[System.IO.File]::WriteAllBytes((Join-Path $src 'c64-hw-sim.ico'), $ico.ToArray())

# The C file: RGBA8 rows, the sizes sokol uses for the window (not 256).
$sb = New-Object System.Text.StringBuilder
[void]$sb.Append("/* icon_data.c - the program's icon for the window and the taskbar (the`n")
[void]$sb.Append(" * .exe's own is src/c64-hw-sim.ico). Made by tools/make_icon.ps1: do not`n")
[void]$sb.Append(" * edit. RGBA, 8 bits each, rows from the top. */`n")
[void]$sb.Append("#include `"icon_data.h`"`n")
$small = @(48, 32, 16)
foreach ($s in $small) {
    [void]$sb.Append("`nstatic const unsigned char icon$s[$($s * $s * 4)] = {`n")
    $b = $rgba[$s]
    for ($k = 0; $k -lt $b.Length; $k += 16) {
        $n = [Math]::Min(16, $b.Length - $k)
        $parts = for ($j = 0; $j -lt $n; $j++) { '0x{0:x2}' -f $b[$k + $j] }
        [void]$sb.Append('    ' + ($parts -join ',') + ",`n")
    }
    [void]$sb.Append("};`n")
}
[void]$sb.Append("`nconst icon_image_t icon_images[ICON_NIMAGES] = {`n")
foreach ($s in $small) { [void]$sb.Append("    { $s, $s, icon$s, sizeof(icon$s) },`n") }
[void]$sb.Append("};`n")
[System.IO.File]::WriteAllText((Join-Path $src 'icon_data.c'), $sb.ToString().Replace("`r`n", "`n"),
    (New-Object System.Text.UTF8Encoding($false)))
Write-Output "wrote src/c64-hw-sim.ico and src/icon_data.c"
