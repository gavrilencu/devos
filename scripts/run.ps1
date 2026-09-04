# Porneste MyOS in QEMU. Converteste automat imaginile de fundal,
# recompileaza, apoi ruleaza.
#
# Ca sa schimbi imaginile: inlocuieste pur si simplu fisierele
#   background1.png  (ecranul de incarcare / splash)
#   background.jpg   (fundalul desktopului)
# din radacina proiectului, apoi ruleaza din nou acest script.
#
# NOTA: doar caractere ASCII in acest fisier - PowerShell 5.1 citeste
# fisierul ca ANSI si diacriticele/em-dash-urile UTF-8 strica parsarea.
#
# Utilizare:  powershell -ExecutionPolicy Bypass -File scripts\run.ps1

$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent

function Convert-Bg($src, $dst) {
    $s = Join-Path $root $src
    if (Test-Path $s) {
        Write-Host "Convertesc $src -> $dst ..."
        & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "convimg.ps1") $s (Join-Path $root $dst)
    }
}

# splash: background1.png (sau .jpg); desktop: background.jpg (sau .png)
if (Test-Path (Join-Path $root "background1.png")) { Convert-Bg "background1.png" "fs\splash.raw" }
elseif (Test-Path (Join-Path $root "background1.jpg")) { Convert-Bg "background1.jpg" "fs\splash.raw" }

if (Test-Path (Join-Path $root "background.jpg")) { Convert-Bg "background.jpg" "fs\desk.raw" }
elseif (Test-Path (Join-Path $root "background.png")) { Convert-Bg "background.png" "fs\desk.raw" }

Write-Host "Compilez (wsl make) ..."
wsl make
if ($LASTEXITCODE -ne 0) { Write-Error "Compilarea a esuat." }

$qemu = "qemu-system-x86_64"
if (-not (Get-Command $qemu -ErrorAction SilentlyContinue)) {
    $qemu = "C:\Program Files\qemu\qemu-system-x86_64.exe"
}
$img = Join-Path $root "build\myos.img"
if (-not (Test-Path $img)) { Write-Error "Nu exista $img" }

Write-Host ""
Write-Host "  ================================================================"
Write-Host "   MOUSE: da un CLICK in fereastra QEMU ca sa fie 'prins' (grab)."
Write-Host "   Atunci dispare cursorul Windows si ramane doar cel din MyOS."
Write-Host "   Ca sa-l eliberezi inapoi in Windows: apasa Ctrl+Alt+G."
Write-Host "  ================================================================"
Write-Host ""
Write-Host "Pornesc MyOS ..."

# -vga std: VGA standard (VBE + interfata dispi Bochs: DevOS ruleaza Full HD
#   1920x1080, comutabil din Setari). -full-screen: fereastra cat tot monitorul,
#   ca sa se vada si taskbar-ul de jos (iesi cu Ctrl+Alt+F, elibereaza mouse-ul
#   cu Ctrl+Alt+G). -display gtk,zoom-to-fit: scaleaza imaginea la ecran.
# -netdev user + rtl8139: placa de retea (user-mode/SLIRP): MyOS primeste
#   10.0.2.15, gateway 10.0.2.2. Portul 2323 din Windows -> 23 (telnet) in MyOS.
$imgArg = 'file=' + $img + ',format=raw'
& $qemu -m 256M -vga std -drive $imgArg `
    -display gtk,zoom-to-fit=on -full-screen `
    -netdev user,id=net0,hostfwd=tcp::2323-:23 -device rtl8139,netdev=net0 `
    -serial stdio
