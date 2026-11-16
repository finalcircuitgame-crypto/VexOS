# SFS2 disk format (foundation)

SFS2 is the next native VexOS filesystem format. Its on-disk metadata contract is
defined in `src/include/sfs2.h` and validated by `src/kernel/core/sfs2.c`.

**Kernel status:** SFS2 mounts read/write only when the image has the optional
redo-journal extension and the storage backend supplies durable sector writes.
Legacy SFS2 images with no journal remain read-only. The current transactional
writer supports nested regular files, with missing parent directories created
transactionally, and a 16 MiB per-file limit. It supports create, replace, and
unlink. A committed redo journal is replayed before inode metadata is trusted.
SFS1 remains supported as the legacy writable format.

**Host-tool status:** `tools/sfs2_image.py` provides an offline formatter,
metadata verifier, and SFS1-to-SFS2 image migration command. These tools operate
on regular image files only; they do not write directly to a block device.
The migration utility supports the current SFS1 root-level file layout and
copies into a separate output image. It verifies the generated metadata and
compares copied file bytes before reporting success.

## Sector layout

All metadata uses 512-byte sectors. The first sector is the SFS2 superblock:

| Region | Location |
|---|---|
| Primary superblock | LBA 0 |
| Optional redo journal | `journal_lba .. journal_lba + journal_sectors` |
| Free-space bitmap | `bitmap_lba .. bitmap_lba + bitmap_sectors` |
| Fixed inode table | `inode_table_lba .. inode_table_lba + inode_table_sectors` |
| Data area | `data_lba .. volume_sectors` |

The superblock includes a format version, declared volume size, generation,
metadata region bounds, optional journal geometry, inode capacity, root inode ID,
and an FNV-1a checksum.
The validator rejects overlapping/out-of-order regions, out-of-device volume
sizes, undersized metadata regions, and invalid checksums.

The optional journal occupies a fixed region before the bitmap. The first
journal sector is a checksummed commit header; up to 32 following sectors
hold redo images for home sectors. A transaction writes journal payloads,
commits the header, applies home sectors, then clears the header. On mount, a
committed transaction is replayed before inode-table validation. Images with
zero journal geometry remain compatible as read-only SFS2 volumes.

The bitmap has one bit per data-area sector (bit zero describes LBA
`data_lba`). A set bit means allocated. Journaled nested-file operations update
the bitmap and inode record in one redo transaction. Each file is limited to
32768 sectors (16 MiB), using a 32-record journal so replacement can atomically
update both old and new contiguous extents, inode metadata, and the superblock.

## Inodes

The fixed-size inode record stores:

- A unique nonzero inode ID and parent directory ID.
- File or directory type.
- A component name of up to 47 bytes (no slash, dot, or dot-dot components).
- File size and a contiguous data extent.
- A generation and per-record checksum.

Inode 1 is the root directory: it has no parent or name and owns no data
extent. Other inodes require a parent ID. Directory inodes have no data extent,
so empty directories can be represented independently of their children.

This initial layout uses one contiguous extent per file. A fragmented volume
may lack a large enough contiguous run despite having sufficient total free
space. Extent chains or an extent tree would require a future format revision.

## Host image utility

The tool is intentionally offline and refuses to overwrite an existing output
unless `--force` is supplied. It never opens a raw block device.

Format an image from regular files (only each source file's basename is stored):

```sh
python3 tools/sfs2_image.py format dist/sfs2.img --sectors 131072 file1.txt file2.bin
```

Verify an image's superblock, metadata geometry, inode checksums, root inode,
names, file extents, bitmap allocations, and extent overlap:

```sh
python3 tools/sfs2_image.py verify dist/sfs2.img
```

Migrate a supported SFS1 image to a separate SFS2 image:

```sh
python3 tools/sfs2_image.py migrate-sfs1 dist/storage.img dist/storage-sfs2.img
```

The source image is never modified. The destination must be a different file.
Use `--force` only when intentionally replacing an existing destination.
Migration currently supports root-level SFS1 files; it rejects unsupported
names, malformed tables, out-of-range extents, and truncated source payloads.

Run the host regression tests with:

```sh
python3 -m unittest discover -s tools -p 'test_sfs2_image.py' -v
```

## Safety and compatibility

- SFS1 remains unchanged and remains the kernel's writable native format.
- Legacy SFS2 images without journal geometry mount read-only; journal-enabled images mount with the transactional nested-file writer when durable sector writes are available. No normal boot path formats or migrates media.
- Never auto-format an unknown, damaged, or unsupported volume.
- Host formatting and migration are explicit offline operations against image
  files, not automatic install or boot operations.
- Do not switch boot configuration to an SFS2 image until the image has been
  verified and the kernel has passed the relevant QEMU disk-image tests.
- Journaled writes commit new file data before metadata, and use redo replay for
  metadata recovery. Failed transactions require remount/replay before further
  access. The current transaction scratch buffer is not yet concurrency-safe.

## Implemented kernel helpers

- Version, geometry, metadata-region, and checksum validation for superblocks.
- Inode type, root-inode, component-name, extent-boundary, and checksum checks.
- Bounds-checked bitmap-bit lookup and contiguous free-run search helpers.
- Mount using caller-supplied sector I/O callbacks, including committed redo
  journal replay before metadata validation.
- Journaled nested-file create/replace/unlink, automatic transactional parent
  directory creation, contiguous allocation, bitmap persistence, inode updates,
  and superblock generation updates. Files up to 16 MiB are supported.
- Full inode-table loading with table-padding handling, root/parent/type checks,
  unique sibling names, and rejection of orphaned/cyclic directory trees.
- Absolute or relative path resolution, stat, directory enumeration, and
  ranged reads across sector boundaries.
- CI-only metadata checks and in-memory disk-image tests covering mount, empty
  directories, lookup, ranged reads, damaged metadata rejection, interrupted
  journal replay, nested directory creation/listing, over-2-MiB file writes and
  readback, and failure injection at every write boundary of a replacement
  transaction.

## Remaining kernel milestones

1. Add explicit concurrency protection around the transaction scratch state.
2. Qualify write ordering and flush behavior across physical AHCI/ATA devices.
3. Consider extent trees or extent chains to reduce contiguous-allocation
   failures for heavily fragmented volumes.
4. Keep old no-journal SFS2 images read-only and preserve SFS1 compatibility.
