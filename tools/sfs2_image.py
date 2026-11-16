#!/usr/bin/env python3
"""Create, inspect, and migrate VexOS SFS2 disk images.

SFS2 v2 currently uses one contiguous extent per file. This tool is deliberately
offline: it never touches a block device, only regular image files.
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

SECTOR = 512
MAGIC = 0x32534653
VERSION = 2
NAME_MAX = 48
INODE_MAX = 256
SFS1_MAGIC = 0x31465353
SFS1_VERSION = 1
SB = struct.Struct("<IHHIQQQI QIIQI IQI428s")
INODE = struct.Struct("<IIBBH48sQQIII8s")
JOURNAL = struct.Struct("<IIIIQ8Q8I392s")
JOURNAL_SECTORS = 1 + 8
SFS1_SB = struct.Struct("<8I")
SFS1_ENTRY = struct.Struct("<B32sIII")


def checksum(data: bytes, offset: int) -> int:
    b = bytearray(data)
    b[offset:offset + 4] = b"\0" * 4
    h = 2166136261
    for value in b:
        h = ((h ^ value) * 16777619) & 0xFFFFFFFF
    return h


def ceildiv(n: int, d: int) -> int:
    return (n + d - 1) // d


def read_sfs1(image: Path):
    with image.open("rb") as f:
        raw = f.read(SECTOR)
        if len(raw) != SECTOR:
            raise ValueError("image is smaller than one sector")
        magic, version, table_lba, table_sectors, data_lba, next_free, count, _ = SFS1_SB.unpack_from(raw)
        if magic != SFS1_MAGIC or version != SFS1_VERSION:
            raise ValueError("source is not a supported SFS1 image")
        size = image.stat().st_size
        sectors = size // SECTOR
        if size % SECTOR or table_lba == 0 or table_sectors != 8 or count > 64:
            raise ValueError("invalid SFS1 geometry")
        if table_lba + table_sectors > sectors or data_lba < table_lba + table_sectors or next_free < data_lba or next_free > sectors:
            raise ValueError("SFS1 regions exceed image bounds")
        f.seek(table_lba * SECTOR)
        table = f.read(table_sectors * SECTOR)
        if len(table) != table_sectors * SECTOR:
            raise ValueError("truncated SFS1 table")
        entries = []
        idx = 0
        for sec in range(table_sectors):
            block = table[sec * SECTOR:(sec + 1) * SECTOR]
            for off in range(0, SECTOR - SFS1_ENTRY.size + 1, SFS1_ENTRY.size):
                if idx >= 64:
                    break
                used, name_raw, start, length, _reserved = SFS1_ENTRY.unpack_from(block, off)
                idx += 1
                if used not in (0, 1):
                    raise ValueError("invalid SFS1 entry used flag")
                if not used:
                    continue
                name = name_raw.split(b"\0", 1)[0]
                if not name or b"/" in name or name in (b".", b".."):
                    raise ValueError("unsupported or invalid SFS1 filename")
                try:
                    text_name = name.decode("utf-8")
                except UnicodeDecodeError as exc:
                    raise ValueError("SFS1 filename is not UTF-8") from exc
                if len(name) >= 32 or len(name) >= NAME_MAX:
                    raise ValueError("SFS1 filename is too long")
                data_sectors = max(1, ceildiv(length, SECTOR))
                if start < data_lba or start + data_sectors > next_free:
                    raise ValueError(f"SFS1 extent for {text_name!r} is out of bounds")
                f.seek(start * SECTOR)
                payload = f.read(length)
                if len(payload) != length:
                    raise ValueError(f"truncated SFS1 file: {text_name}")
                entries.append((text_name, payload))
        if len(entries) != count:
            raise ValueError("SFS1 file_count does not match its table")
        return sectors, entries


def build_image(destination: Path, volume_sectors: int, files: list[tuple[str, bytes]], inode_capacity: int, force: bool = False):
    if destination.exists() and not force:
        raise ValueError(f"refusing to overwrite existing image: {destination} (use --force)")
    if volume_sectors < 32 or volume_sectors > 0xFFFFFFFFFFFFFFFF:
        raise ValueError("volume must contain at least 32 sectors")
    if not 2 <= inode_capacity <= INODE_MAX:
        raise ValueError(f"inode capacity must be 2..{INODE_MAX}")
    if len(files) + 1 > inode_capacity:
        raise ValueError("not enough inode slots for files and root")
    seen = set()
    prepared = []
    for name, data in files:
        encoded = name.encode("utf-8")
        if not encoded or len(encoded) >= NAME_MAX or b"/" in encoded or name in (".", ".."):
            raise ValueError(f"invalid SFS2 filename: {name!r}")
        if name in seen:
            raise ValueError(f"duplicate filename: {name!r}")
        seen.add(name)
        prepared.append((encoded, data))

    journal_lba = 1
    journal_sectors = JOURNAL_SECTORS
    bitmap_lba = journal_lba + journal_sectors
    bitmap_sectors = ceildiv(volume_sectors, SECTOR * 8)
    inode_lba = bitmap_lba + bitmap_sectors
    inode_sectors = ceildiv(inode_capacity * INODE.size, SECTOR)
    data_lba = inode_lba + inode_sectors
    if data_lba >= volume_sectors:
        raise ValueError("volume is too small for requested metadata")

    inode_records = [None] * inode_capacity
    inode_records[0] = INODE.pack(1, 0, 2, 0, 0, b"".ljust(NAME_MAX, b"\0"), 0, 0, 0, 1, 0, bytes(8))
    cursor = data_lba
    bitmap = bytearray(bitmap_sectors * SECTOR)
    for i, (name, data) in enumerate(prepared, start=1):
        needed = ceildiv(len(data), SECTOR)
        if cursor + needed > volume_sectors:
            raise ValueError(f"not enough free sectors for {name.decode('utf-8')!r}")
        inode_records[i] = INODE.pack(i + 1, 1, 1, 0, 0, name.ljust(NAME_MAX, b"\0"),
                                      len(data), cursor if needed else 0, needed, 1, 0, bytes(8))
        for bit in range(cursor - data_lba, cursor + needed - data_lba):
            bitmap[bit >> 3] |= 1 << (bit & 7)
        cursor += needed

    inode_blob = bytearray(inode_sectors * SECTOR)
    for i, record in enumerate(inode_records):
        if record is None:
            continue
        off = i * INODE.size
        inode_blob[off:off + INODE.size] = record
    # Fill per-inode checksums after all fields have been serialized.
    for i in range(inode_capacity):
        off = i * INODE.size
        if inode_blob[off:off + INODE.size] == bytes(INODE.size):
            continue
        rec = bytearray(inode_blob[off:off + INODE.size])
        struct.pack_into("<I", rec, 84, checksum(rec, 84))
        inode_blob[off:off + INODE.size] = rec

    sb = bytearray(SB.pack(MAGIC, VERSION, SECTOR, SECTOR, volume_sectors, 1,
                           bitmap_lba, bitmap_sectors, inode_lba, inode_sectors,
                           inode_capacity, data_lba, 1, 0,
                           journal_lba, journal_sectors, bytes(428)))
    struct.pack_into("<I", sb, 68, checksum(sb, 68))

    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open("wb") as f:
        f.truncate(volume_sectors * SECTOR)
        f.seek(0); f.write(sb)
        f.seek(bitmap_lba * SECTOR); f.write(bitmap)
        f.seek(inode_lba * SECTOR); f.write(inode_blob)
        cursor = data_lba
        for _name, data in prepared:
            f.seek(cursor * SECTOR)
            f.write(data)
            padded = ceildiv(len(data), SECTOR) * SECTOR
            if padded > len(data):
                f.write(bytes(padded - len(data)))
            cursor += ceildiv(len(data), SECTOR)
    return {"sectors": volume_sectors, "files": len(files), "journal_lba": journal_lba,
            "journal_sectors": journal_sectors, "data_lba": data_lba,
            "used_sectors": cursor - data_lba, "inode_capacity": inode_capacity}


def verify_image(image: Path):
    size = image.stat().st_size
    if size % SECTOR:
        raise ValueError("image size is not sector aligned")
    with image.open("rb") as f:
        raw = f.read(SECTOR)
        if len(raw) != SECTOR:
            raise ValueError("image is empty")
        fields = SB.unpack(raw)
        (magic, version, header_size, sector_size, volume, _generation,
         bitmap_lba, bitmap_n, inode_lba, inode_n, cap, data_lba,
         root, sb_sum, journal_lba, journal_n, _reserved) = fields
        if (magic, version, header_size, sector_size) != (MAGIC, VERSION, SECTOR, SECTOR):
            raise ValueError("invalid SFS2 signature/version/header")
        if volume != size // SECTOR or root != 1 or sb_sum != checksum(raw, 68):
            raise ValueError("superblock checksum or geometry mismatch")
        if not 1 <= cap <= INODE_MAX or bitmap_n == 0 or inode_n == 0:
            raise ValueError("invalid SFS2 metadata capacity or region size")
        if (journal_lba < 1 or journal_n < JOURNAL_SECTORS or
                journal_lba + journal_n > bitmap_lba):
            raise ValueError("invalid SFS2 journal region")
        f.seek(journal_lba * SECTOR)
        journal_header = f.read(SECTOR)
        if len(journal_header) != SECTOR or journal_header != bytes(SECTOR):
            raise ValueError("SFS2 image contains an un-replayed journal")
        bitmap_end = bitmap_lba + bitmap_n
        inode_end = inode_lba + inode_n
        if (bitmap_end < bitmap_lba or inode_lba < bitmap_end or
                inode_end < inode_lba or data_lba < inode_end or data_lba >= volume):
            raise ValueError("invalid metadata region ordering")
        if bitmap_n * SECTOR * 8 < volume - data_lba:
            raise ValueError("allocation bitmap does not cover the data region")
        if inode_n * SECTOR < cap * INODE.size:
            raise ValueError("inode table is too small for its declared capacity")
        f.seek(bitmap_lba * SECTOR)
        bitmap = f.read(bitmap_n * SECTOR)
        f.seek(inode_lba * SECTOR)
        table = f.read(inode_n * SECTOR)
        if len(bitmap) != bitmap_n * SECTOR or len(table) != inode_n * SECTOR:
            raise ValueError("truncated metadata")

        live = []
        seen_ids = set()
        seen_names = set()
        extents = []
        root_count = 0
        for i in range(cap):
            rec = table[i * INODE.size:(i + 1) * INODE.size]
            (inode_id, parent, kind, _flags, _rsv, name_raw, length,
             start, sectors, _gen, ino_sum, _tail) = INODE.unpack(rec)
            if inode_id == 0:
                continue
            if ino_sum != checksum(rec, 84):
                raise ValueError(f"inode {inode_id} checksum mismatch")
            if inode_id in seen_ids:
                raise ValueError(f"duplicate inode ID: {inode_id}")
            seen_ids.add(inode_id)

            if inode_id == 1:
                root_count += 1
                if (kind != 2 or parent != 0 or
                        name_raw != bytes(NAME_MAX) or length or start or sectors):
                    raise ValueError("invalid root inode")
                continue

            if parent != 1 or kind != 1:
                raise ValueError("host verifier currently expects root-level regular files")
            name_bytes = name_raw.split(b"\0", 1)[0]
            if not name_bytes or len(name_bytes) >= NAME_MAX or b"/" in name_bytes or name_bytes in (b".", b".."):
                raise ValueError(f"invalid filename in inode {inode_id}")
            try:
                name = name_bytes.decode("utf-8")
            except UnicodeDecodeError as exc:
                raise ValueError(f"filename in inode {inode_id} is not UTF-8") from exc
            if name in seen_names:
                raise ValueError(f"duplicate filename: {name!r}")
            seen_names.add(name)

            need = ceildiv(length, SECTOR)
            if length == 0:
                if start != 0 or sectors != 0:
                    raise ValueError(f"invalid empty-file extent for inode {inode_id}")
            else:
                end_lba = start + sectors
                if (sectors < need or start < data_lba or end_lba < start or
                        end_lba > volume):
                    raise ValueError(f"invalid extent for inode {inode_id}")
                for old_start, old_end, old_id in extents:
                    if start < old_end and old_start < end_lba:
                        raise ValueError(f"overlapping extents for inodes {old_id} and {inode_id}")
                extents.append((start, end_lba, inode_id))
                for bit in range(start - data_lba, end_lba - data_lba):
                    if not (bitmap[bit >> 3] & (1 << (bit & 7))):
                        raise ValueError(f"bitmap misses allocation for inode {inode_id}")
            live.append((name, start, length))

        if root_count != 1:
            raise ValueError("image must contain exactly one root inode")
    return {"sectors": volume, "files": live, "data_lba": data_lba}

def cmd_format(args):
    files = []
    for source in args.files:
        p = Path(source)
        if not p.is_file():
            raise ValueError(f"not a regular file: {source}")
        files.append((p.name, p.read_bytes()))
    result = build_image(Path(args.output), args.sectors, files, args.inodes, args.force)
    print(f"Formatted {args.output}: {result}")


def cmd_migrate(args):
    source = Path(args.source).resolve()
    destination = Path(args.output).resolve()
    if source == destination:
        raise ValueError("migration source and destination must be different files")
    sectors, files = read_sfs1(source)
    result = build_image(destination, args.sectors or sectors, files, args.inodes, args.force)
    verified = verify_image(Path(args.output))
    verified_files = verified["files"]
    if [name for name, _start, _length in verified_files] != [name for name, _data in files]:
        raise ValueError("post-migration filename verification failed")
    with Path(args.output).open("rb") as f:
        for name, start, length in verified_files:
            f.seek(start * SECTOR)
            copied = f.read(length)
            original = next(payload for old_name, payload in files if old_name == name)
            if copied != original:
                raise ValueError(f"post-migration byte verification failed for {name!r}")
    print(f"Migrated SFS1 to separate SFS2 image: {result}; source left untouched")

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    fmt = sub.add_parser("format", help="format a new SFS2 image from regular files")
    fmt.add_argument("output")
    fmt.add_argument("--sectors", type=int, default=131072)
    fmt.add_argument("--inodes", type=int, default=INODE_MAX)
    fmt.add_argument("--force", action="store_true", help="replace an existing output image")
    fmt.add_argument("files", nargs="*")
    fmt.set_defaults(func=cmd_format)
    mig = sub.add_parser("migrate-sfs1", help="copy a legacy SFS1 image into a new SFS2 image")
    mig.add_argument("source")
    mig.add_argument("output")
    mig.add_argument("--sectors", type=int, default=0)
    mig.add_argument("--inodes", type=int, default=INODE_MAX)
    mig.add_argument("--force", action="store_true", help="replace an existing destination image")
    mig.set_defaults(func=cmd_migrate)
    ver = sub.add_parser("verify", help="verify SFS2 image metadata and allocation bitmap")
    ver.add_argument("image")
    ver.set_defaults(func=lambda a: print(verify_image(Path(a.image))))
    args = parser.parse_args()
    try:
        args.func(args)
    except (OSError, ValueError, struct.error) as exc:
        print(f"sfs2_image: error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
