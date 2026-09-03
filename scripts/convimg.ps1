# convimg.ps1 - converteste o imagine (PNG/JPG/BMP) in formatul raw al
# framebuffer-ului MyOS: 1024x768, 32bpp, little-endian B,G,R,X per pixel
# (identic cu u32 0x00RRGGBB al kernelului).
#
# Utilizare: powershell -File scripts\convimg.ps1 sursa.png fs\iesire.raw

param(
    [Parameter(Mandatory=$true)][string]$Src,
    [Parameter(Mandatory=$true)][string]$Dst
)

Add-Type -AssemblyName System.Drawing

$img = New-Object System.Drawing.Bitmap($Src)
$bmp = New-Object System.Drawing.Bitmap(1024, 768, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$g.DrawImage($img, 0, 0, 1024, 768)
$g.Dispose()
$img.Dispose()

$rect = New-Object System.Drawing.Rectangle(0, 0, 1024, 768)
$data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                      [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$bytes = New-Object byte[] (1024 * 768 * 4)
[System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
$bmp.UnlockBits($data)
$bmp.Dispose()

[IO.File]::WriteAllBytes($Dst, $bytes)
Write-Output "scris $Dst ($($bytes.Length) bytes)"
