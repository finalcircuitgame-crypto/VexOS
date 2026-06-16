#!/bin/bash

set -e

if ! make all; then
    echo "Build failed; not launching QEMU."
    exit 1
fi

# Embed wallpaper asset into the SimpleFS storage image
if command -v python3 >/dev/null 2>&1; then
    if [ -f "new.png" ]; then
        python3 - <<'PY'
import os, struct, sys

SFS_MAGIC = 0x31465353  # 'SFS1'
SFS_VERSION = 1
SECTOR = 512
TABLE_LBA = 1
TABLE_SECTORS = 8
DATA_LBA = TABLE_LBA + TABLE_SECTORS
MAX_FILES = 64
NAME_MAX = 32

img_path = os.path.join('dist', 'storage.img')
png_src = 'new.png'
png_dst_name = 'new.png'

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

png_data = open(png_src, 'rb').read()
png_len = len(png_data)

with open(img_path, 'r+b') as f:
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

    # Load file table
    table = bytearray(read_at(f, table_lba * SECTOR, table_secs * SECTOR))

    entry_size = 1 + NAME_MAX + 4 + 4 + 4  # packed
    def get_entry(i):
        off = i * entry_size
        e = table[off:off+entry_size]
        used = e[0]
        name = e[1:1+NAME_MAX].split(b'\x00', 1)[0].decode('ascii', errors='ignore')
        start_lba, size_bytes, rsvd = struct.unpack('<III', e[1+NAME_MAX:1+NAME_MAX+12])
        return used, name, start_lba, size_bytes

    def set_entry(i, used, name, start_lba, size_bytes):
        off = i * entry_size
        name_b = name.encode('ascii', errors='ignore')[:NAME_MAX-1]
        name_b = name_b + b'\x00' * (NAME_MAX - len(name_b))
        e = bytearray(entry_size)
        e[0] = 1 if used else 0
        e[1:1+NAME_MAX] = name_b
        e[1+NAME_MAX:1+NAME_MAX+12] = struct.pack('<III', int(start_lba), int(size_bytes), 0)
        table[off:off+entry_size] = e

    found_idx = None
    free_idx = None
    for i in range(MAX_FILES):
        used, name, start_lba, size_bytes = get_entry(i)
        if used and name == png_dst_name:
            found_idx = i
            break
        if not used and free_idx is None:
            free_idx = i

    if found_idx is None:
        if free_idx is None:
            print('run.sh: SimpleFS table full', file=sys.stderr)
            sys.exit(1)
        idx = free_idx
        # Allocate new space
        sectors = ceil_div(png_len, SECTOR)
        start_lba = next_free_lba
        next_free_lba = next_free_lba + sectors
        file_count = file_count + 1
    else:
        idx = found_idx
        used, name, start_lba, size_bytes = get_entry(idx)
        # If the new png doesn't fit in-place, append a new copy.
        old_secs = ceil_div(size_bytes, SECTOR) if size_bytes else 0
        new_secs = ceil_div(png_len, SECTOR)
        if new_secs > old_secs:
            start_lba = next_free_lba
            next_free_lba = next_free_lba + new_secs

    # Write file data
    write_at(f, start_lba * SECTOR, png_data + (b'\x00' * (ceil_div(png_len, SECTOR) * SECTOR - png_len)))
    set_entry(idx, True, png_dst_name, start_lba, png_len)

    # Persist table
    write_at(f, table_lba * SECTOR, bytes(table))

    # Persist superblock
    sb2 = struct.pack('<8I', SFS_MAGIC, SFS_VERSION, table_lba, table_secs, data_lba, next_free_lba, file_count, 0)
    write_at(f, 0, sb2 + b'\x00' * (SECTOR - len(sb2)))

print('run.sh: embedded new.png into dist/storage.img')
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
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file=build/OVMF_VARS_4M.fd \
    -drive format=raw,file=dist/tiny64.img \
    -drive if=ide,index=1,format=raw,file=dist/storage.img \
    -m 1G \
    -device qemu-xhci \
    -device usb-kbd \
    -device usb-tablet \
    -nic none \
    -serial mon:stdio \
    -boot order=c,menu=off
