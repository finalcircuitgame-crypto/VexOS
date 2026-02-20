#!/bin/bash

set -e

if ! make all; then
    echo "Build failed; not launching QEMU."
    exit 1
fi

OVMF_CODE=/usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS_TEMPLATE=/usr/share/OVMF/OVMF_VARS_4M.fd

if [ ! -f "$OVMF_CODE" ]; then
    echo "OVMF CODE firmware not found at $OVMF_CODE"
    exit 1
fi

# Use a writable local copy of VARS (NVRAM)
mkdir -p build
if [ ! -f build/OVMF_VARS_4M.fd ]; then
    cp "$OVMF_VARS_TEMPLATE" build/OVMF_VARS_4M.fd
fi

qemu-system-x86_64 \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file=build/OVMF_VARS_4M.fd \
    -drive format=raw,file=dist/tiny64.img \
    -device qemu-xhci \
    -device usb-kbd \
    -device usb-mouse \
    -nic none \
    -serial stdio \
    -boot order=c,menu=off
