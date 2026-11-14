#!/bin/bash

set -e

if ! make all; then
    echo "Build failed; not launching QEMU."
    exit 1
fi

# Embed a local image into the SimpleFS storage image. The viewer detects
# image formats from file signatures, so the extension is preserved as supplied.
if command -v python3 >/dev/null 2>&1; then
    ASSET_SRC=""
    for candidate in new.jpeg new.jpg new.png; do
        if [ -f "$candidate" ]; then
            ASSET_SRC="$candidate"
            break
        fi
    done

    if [ -n "$ASSET_SRC" ]; then
        python3 - "$ASSET_SRC" <<'PY'
import os, struct, sys

SFS_MAGIC = 0x31465353  # 'SFS1'
SFS_VERSION = 1
SECTOR = 512
TABLE_LBA = 1
TABLE_SECTORS = 8
DATA_LBA = TABLE_LBA + TABLE_SECTORS
MAX_FILES = 64
NAME_MAX = 32
MAX_PREVIEW_BYTES = 8 * 1024 * 1024

img_path = os.path.join('dist', 'storage.img')
asset_src = sys.argv[1]
asset_name = os.path.basename(asset_src)

def ceil_div(a, b):
    return (a + b - 1) // b

def read_at(f, off, n):
    f.seek(off)
    return f.read(n)

def write_at(f, off, data):
    f.seek(off)
    f.write(data)

if not os.path.exists(img_path):
    print(f"run.sh: {img_path} not found", file=sys.stderr)
    sys.exit(1)

try:
    asset_name.encode('ascii')
except UnicodeEncodeError:
    print(f"run.sh: asset name must be ASCII: {asset_name!r}", file=sys.stderr)
    sys.exit(1)

if not asset_name or len(asset_name) >= NAME_MAX:
    print(f"run.sh: unsupported asset name: {asset_name!r}", file=sys.stderr)
    sys.exit(1)

asset_data = open(asset_src, 'rb').read()
asset_len = len(asset_data)
if asset_len == 0 or asset_len > MAX_PREVIEW_BYTES:
    print(f"run.sh: preview must be between 1 byte and {MAX_PREVIEW_BYTES} bytes", file=sys.stderr)
    sys.exit(1)

with open(img_path, 'r+b') as f:
    disk_sectors = os.path.getsize(img_path) // SECTOR
    sb_raw = read_at(f, 0, SECTOR)
    sb = sb_raw[:32]
    magic, ver, table_lba, table_secs, data_lba, next_free_lba, file_count, _ = struct.unpack('<8I', sb)

    if magic != SFS_MAGIC or ver != SFS_VERSION or table_secs == 0 or table_secs > 64:
        magic = SFS_MAGIC
        ver = SFS_VERSION
        table_lba = TABLE_LBA
        table_secs = TABLE_SECTORS
        data_lba = DATA_LBA
        next_free_lba = DATA_LBA
        file_count = 0
        sb2 = struct.pack('<8I', magic, ver, table_lba, table_secs, data_lba, next_free_lba, file_count, 0)
        write_at(f, 0, sb2 + b'\x00' * (SECTOR - len(sb2)))
        write_at(f, table_lba * SECTOR, b'\x00' * (table_secs * SECTOR))

    table = bytearray(read_at(f, table_lba * SECTOR, table_secs * SECTOR))
    entry_size = 1 + NAME_MAX + 4 + 4 + 4

    def get_entry(i):
        off = i * entry_size
        e = table[off:off+entry_size]
        used = e[0]
        name = e[1:1+NAME_MAX].split(b'\x00', 1)[0].decode('ascii', errors='ignore')
        start_lba, size_bytes, _ = struct.unpack('<III', e[1+NAME_MAX:1+NAME_MAX+12])
        return used, name, start_lba, size_bytes

    def set_entry(i, name, start_lba, size_bytes):
        off = i * entry_size
        name_b = name.encode('ascii')[:NAME_MAX-1]
        name_b = name_b + b'\x00' * (NAME_MAX - len(name_b))
        e = bytearray(entry_size)
        e[0] = 1
        e[1:1+NAME_MAX] = name_b
        e[1+NAME_MAX:1+NAME_MAX+12] = struct.pack('<III', int(start_lba), int(size_bytes), 0)
        table[off:off+entry_size] = e

    found_idx = None
    free_idx = None
    old_size = 0
    old_start_lba = 0
    for i in range(MAX_FILES):
        used, name, start_lba, size_bytes = get_entry(i)
        if used and name == asset_name:
            found_idx = i
            old_start_lba = start_lba
            old_size = size_bytes
            break
        if not used and free_idx is None:
            free_idx = i

    new_sectors = ceil_div(asset_len, SECTOR)
    if found_idx is None:
        if free_idx is None:
            print('run.sh: SimpleFS table full', file=sys.stderr)
            sys.exit(1)
        idx = free_idx
        start_lba = next_free_lba
        next_free_lba += new_sectors
        file_count += 1
    else:
        idx = found_idx
        old_sectors = ceil_div(old_size, SECTOR) if old_size else 0
        if new_sectors > old_sectors:
            start_lba = next_free_lba
            next_free_lba += new_sectors
        else:
            start_lba = old_start_lba

    if next_free_lba > disk_sectors:
        print('run.sh: image does not fit in dist/storage.img', file=sys.stderr)
        sys.exit(1)

    padded = asset_data + (b'\x00' * (new_sectors * SECTOR - asset_len))
    write_at(f, start_lba * SECTOR, padded)
    set_entry(idx, asset_name, start_lba, asset_len)
    write_at(f, table_lba * SECTOR, bytes(table))

    sb2 = struct.pack('<8I', SFS_MAGIC, SFS_VERSION, table_lba, table_secs, data_lba, next_free_lba, file_count, 0)
    write_at(f, 0, sb2 + b'\x00' * (SECTOR - len(sb2)))

print(f"run.sh: embedded {asset_name} ({asset_len} bytes) into dist/storage.img")
PY
    fi
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
    -machine pc \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file=build/OVMF_VARS_4M.fd \
    -drive if=ide,index=0,format=raw,file=dist/tiny64.img \
    -drive if=ide,index=1,format=raw,file=dist/storage.img \
    -m 1G \
    -device qemu-xhci \
    -device usb-kbd \
    -device usb-tablet \
    -nic none \
    -serial mon:stdio \
    -boot order=c,menu=off
