#!/usr/bin/env python3
"""Regression tests for the offline SFS2 image tooling."""
from __future__ import annotations

import struct
import tempfile
import unittest
from pathlib import Path

import sfs2_image as image


class SFS2ImageTests(unittest.TestCase):
    def test_format_verify_and_payloads_including_empty_file(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "volume.img"
            image.build_image(path, 128, [
                ("hello.txt", b"hello vexos"),
                ("empty.dat", b""),
                ("boundary.bin", bytes(range(256)) * 3),
            ], 16)
            verified = image.verify_image(path)
            self.assertEqual(
                [entry[0] for entry in verified["files"]],
                ["hello.txt", "empty.dat", "boundary.bin"],
            )
            with path.open("rb") as disk:
                for name, start, length in verified["files"]:
                    disk.seek(start * image.SECTOR)
                    payload = disk.read(length)
                    expected = {
                        "hello.txt": b"hello vexos",
                        "empty.dat": b"",
                        "boundary.bin": bytes(range(256)) * 3,
                    }[name]
                    self.assertEqual(payload, expected)

    def test_formats_and_verifies_file_larger_than_2_mib(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "large.img"
            payload = bytes((i * 17) & 0xFF for i in range(3 * 1024 * 1024 + 37))
            image.build_image(path, 8192, [("large.bin", payload)], 16)
            verified = image.verify_image(path)
            self.assertEqual(verified["files"][0][2], len(payload))
            with path.open("rb") as disk:
                disk.seek(verified["files"][0][1] * image.SECTOR)
                self.assertEqual(disk.read(len(payload)), payload)

    def test_formatter_reserves_clean_redo_journal(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "journal.img"
            image.build_image(path, 128, [("file.txt", b"journal")], 16)
            fields = image.SB.unpack_from(path.read_bytes())
            journal_lba, journal_sectors = fields[14], fields[15]
            self.assertEqual(journal_lba, 1)
            self.assertEqual(journal_sectors, image.JOURNAL_SECTORS)
            self.assertLessEqual(journal_lba + journal_sectors, fields[6])
            image.verify_image(path)

            damaged = bytearray(path.read_bytes())
            damaged[journal_lba * image.SECTOR] = 0x7F
            path.write_bytes(damaged)
            with self.assertRaisesRegex(ValueError, "un-replayed journal"):
                image.verify_image(path)

    def test_refuses_to_overwrite_existing_image(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "existing.img"
            path.write_bytes(b"keep this")
            with self.assertRaisesRegex(ValueError, "refusing to overwrite"):
                image.build_image(path, 64, [], 8)
            self.assertEqual(path.read_bytes(), b"keep this")

    def test_sfs1_migration_preserves_source_and_payload(self):
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / "legacy.img"
            destination = Path(temp) / "new.img"
            sectors = 64
            raw = bytearray(sectors * image.SECTOR)
            image.SFS1_SB.pack_into(raw, 0, image.SFS1_MAGIC, image.SFS1_VERSION,
                                    1, 8, 9, 10, 1, 0)
            payload = b"legacy SFS1 payload"
            image.SFS1_ENTRY.pack_into(raw, image.SECTOR, 1,
                                       b"legacy.txt".ljust(32, b"\0"),
                                       9, len(payload), 0)
            raw[9 * image.SECTOR:9 * image.SECTOR + len(payload)] = payload
            source.write_bytes(raw)
            original = source.read_bytes()

            sectors_read, files = image.read_sfs1(source)
            self.assertEqual(sectors_read, sectors)
            self.assertEqual(files, [("legacy.txt", payload)])
            image.build_image(destination, sectors_read, files, 16)
            verified = image.verify_image(destination)
            self.assertEqual([x[0] for x in verified["files"]], ["legacy.txt"])
            self.assertEqual(source.read_bytes(), original)
            with destination.open("rb") as disk:
                _, start, length = verified["files"][0]
                disk.seek(start * image.SECTOR)
                self.assertEqual(disk.read(length), payload)

    def test_rejects_corrupt_superblock_and_inode_checksums(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "volume.img"
            image.build_image(path, 64, [("x", b"x")], 8)
            good = path.read_bytes()

            damaged = bytearray(good)
            damaged[68] ^= 0x80
            path.write_bytes(damaged)
            with self.assertRaisesRegex(ValueError, "checksum"):
                image.verify_image(path)

            path.write_bytes(good)
            fields = image.SB.unpack_from(good)
            inode_lba = fields[8]
            damaged = bytearray(good)
            damaged[inode_lba * image.SECTOR + 20] ^= 1
            path.write_bytes(damaged)
            with self.assertRaisesRegex(ValueError, "inode"):
                image.verify_image(path)


if __name__ == "__main__":
    unittest.main(verbosity=2)
