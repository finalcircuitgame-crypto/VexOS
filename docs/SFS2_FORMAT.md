# SFS2 disk format (foundation)

SFS2 is the next native VexOS filesystem format. Its on-disk metadata contract is
defined in `src/include/sfs2.h` and validated by `src/kernel/core/sfs2.c`.

**Kernel status:** SFS2 currently mounts read-only. The kernel exposes path
lookup, stat, directory enumeration, and ranged file reads through
SimpleFS/VFS. Kernel create, replace, unlink, bitmap persistence, journal
recovery, and writable mount support are **not implemented**. SFS1 remains the
writable native filesystem.

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
| Free-space bitmap | `bitmap_lba .. bitmap_lba + bitmap_sectors` |
| Fixed inode table | `inode_table_lba .. inode_table_lba + inode_table_sectors` |
| Data area | `data_lba .. volume_sectors` |

The superblock includes a format version, declared volume size, generation,
metadata region bounds, inode capacity, root inode ID, and an FNV-1a checksum.
The validator rejects overlapping/out-of-order regions, out-of-device volume
sizes, undersized metadata regions, and invalid checksums.

The bitmap has one bit per data-area sector (bit zero describes LBA
`data_lba`). A set bit means allocated. The kernel currently validates bitmap
bits for existing file extents; it does not yet persist allocations or frees.

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
- SFS2 kernel mount is read-only; no normal boot path formats or migrates media.
- Never auto-format an unknown, damaged, or unsupported volume.
- Host formatting and migration are explicit offline operations against image
  files, not automatic install or boot operations.
- Do not switch boot configuration to an SFS2 image until the image has been
  verified and the kernel has passed the relevant QEMU disk-image tests.
- A writable kernel backend still needs ordered metadata commits, journal
  replay, allocator rollback on failed I/O, duplicate-name checks, parent/type
  validation, and bitmap/extent consistency checks.

## Implemented kernel helpers

- Version, geometry, metadata-region, and checksum validation for superblocks.
- Inode type, root-inode, component-name, extent-boundary, and checksum checks.
- Bounds-checked bitmap-bit lookup and contiguous free-run search helpers.
- Read-only mount using caller-supplied sector I/O callbacks.
- Full inode-table loading with table-padding handling, root/parent/type checks,
  unique sibling names, and rejection of orphaned/cyclic directory trees.
- Absolute or relative path resolution, stat, directory enumeration, and
  ranged reads across sector boundaries.
- CI-only metadata checks and an in-memory disk-image test covering mount,
  empty directories, lookup, file reads, and damaged metadata rejection.

## Remaining kernel milestones

1. Define and reserve a journal region in a backward-compatible on-disk layout.
2. Implement journal replay before trusting mutable metadata.
3. Add write ordering and flush/error propagation to the storage abstraction.
4. Implement transactional create/replace/unlink and bitmap updates.
5. Add power-loss/fault-injection tests for every journal commit boundary.
6. Enable writable SFS2 mount only after recovery and QEMU tests pass.
