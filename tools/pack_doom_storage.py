#!/usr/bin/env python3
"""Pack DoomGeneric and a user-provided Doom WAD into VexOS SimpleFS."""
import argparse
import math
import os
import struct
import sys

SECTOR = 512
MAGIC = 0x31465353
VERSION = 1
TABLE_LBA = 1
TABLE_SECTORS = 8
DATA_LBA = TABLE_LBA + TABLE_SECTORS
MAX_FILES = 64
NAME_MAX = 32
ENTRY_SIZE = 1 + NAME_MAX + 4 + 4 + 4

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", default="dist/storage.img")
    parser.add_argument("--elf", default="dist/doom.elf")
    parser.add_argument("--wad", required=True, help="Path to your legally obtained doom1.wad")
    args = parser.parse_args()

    sources = [("doom.elf", args.elf), ("doom1.wad", args.wad)]
    for name, path in sources:
        if not os.path.isfile(path):
            parser.error(f"{name} source does not exist: {path}")
        if os.path.getsize(path) == 0:
            parser.error(f"{name} source is empty: {path}")

    if not os.path.isfile(args.image):
        parser.error(f"storage image does not exist: {args.image}")

    with open(args.image, "r+b") as image:
        image_size = os.path.getsize(args.image)
        if image_size < (DATA_LBA + 1) * SECTOR:
            parser.error("storage image is too small")

        image.seek(0)
        raw = image.read(SECTOR)
        fields = struct.unpack("<8I", raw[:32])
        magic, version, table_lba, table_sectors, data_lba, next_lba, count, _ = fields

        if magic != MAGIC or version != VERSION or table_lba != TABLE_LBA or not 1 <= table_sectors <= 64:
            table_lba, table_sectors, data_lba = TABLE_LBA, TABLE_SECTORS, DATA_LBA
            next_lba, count = data_lba, 0
            image.seek(0)
            image.write(struct.pack("<8I", MAGIC, VERSION, table_lba, table_sectors,
                                     data_lba, next_lba, count, 0) + bytes(SECTOR - 32))
            image.seek(table_lba * SECTOR)
            image.write(bytes(table_sectors * SECTOR))

        table_bytes = table_sectors * SECTOR
        if MAX_FILES * ENTRY_SIZE > table_bytes:
            parser.error("SimpleFS table is too small for its file limit")
        image.seek(table_lba * SECTOR)
        table = bytearray(image.read(table_bytes))

        entries = []
        for i in range(MAX_FILES):
            off = i * ENTRY_SIZE
            used = table[off]
            name = bytes(table[off+1:off+1+NAME_MAX]).split(b"\\0", 1)[0].decode("ascii", "ignore")
            start, size, _reserved = struct.unpack_from("<III", table, off+1+NAME_MAX)
            entries.append((used, name, start, size))

        for name, path in sources:
            data = open(path, "rb").read()
            existing = next((i for i, e in enumerate(entries) if e[0] and e[1].lower() == name.lower()), None)
            slot = existing if existing is not None else next((i for i, e in enumerate(entries) if not e[0]), None)
            if slot is None:
                parser.error("SimpleFS file table is full")
            sectors = math.ceil(len(data) / SECTOR)
            start = next_lba
            end = (start + sectors) * SECTOR
            if end > image_size:
                parser.error(f"not enough storage space for {name} ({len(data)} bytes needed)")
            image.seek(start * SECTOR)
            image.write(data + bytes(sectors * SECTOR - len(data)))
            name_bytes = name.encode("ascii") + bytes(NAME_MAX - len(name))
            off = slot * ENTRY_SIZE
            entry = struct.pack("<B32sIII", 1, name_bytes, start, len(data), 0)
            table[off:off+ENTRY_SIZE] = entry
            if existing is None:
                count += 1
            entries[slot] = (1, name, start, len(data))
            next_lba += sectors
            print(f"Packed {name}: {len(data):,} bytes at LBA {start}")

        image.seek(table_lba * SECTOR)
        image.write(table)
        image.seek(0)
        image.write(struct.pack("<8I", MAGIC, VERSION, table_lba, table_sectors,
                                data_lba, next_lba, count, 0) + bytes(SECTOR - 32))
        image.flush()
    print(f"Updated {args.image}")

if __name__ == "__main__":
    main()
