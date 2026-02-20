@echo off
echo Starting Tiny64 in QEMU...
echo Ensure QEMU is installed and added to your PATH.
echo You also need an OVMF.fd (UEFI Firmware) file.
echo.

set OVMF_PATH="C:\Program Files\qemu\share\ovmf\OVMF.fd"
if not exist %OVMF_PATH% (
    set OVMF_PATH="C:\Program Files\qemu\OVMF.fd"
)

qemu-system-x86_64 ^
    -drive format=raw,file=dist/tiny64.img ^
    -bios %OVMF_PATH% ^
    -device qemu-xhci ^
    -serial stdio
