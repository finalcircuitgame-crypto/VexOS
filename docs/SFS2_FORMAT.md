# SFS2 disk format (foundation)

SFS2 is the next native VexOS filesystem format. This document describes the
metadata contract introduced by `src/include/sfs2.h` and its validation helpers
in `src/kernel/core/sfs2.c`.

**Status:** metadata definitions, non-destructive read-only mount, namespace validation, path lookup, stat, directory enumeration, and ranged file reads are implemented as a standalone backend. The kernel probes SFS2 before legacy formats and exposes its read-only files and persistent directories through SimpleFS/VFS. VFS path normalization uses the active filesystem's path limit. Create, replace, and unlink remain unavailable on SFS2. No formatter, writable backend, journal, or migration tool is provided. Existing SFS1 volumes remain the supported
writable native format. Normal boot must never format or migrate a volume.

## Sector layout

All metadata uses 512-byte sectors. The first sector is the SFS2 superblock:

| Region | Location |
|---|---|
| Primary superblock | LBA 0 |
| Free-space bitmap | `bitmap_lba .. bitmap_lba + bitmap_sectors` |
| Fixed inode table | `inode_table_lba .. inode_table_lba + inode_table_sectors` |
| Data area | `data_lba .. volume_sectors` |

The superblock includes a format version, declared volume size, monotonically
increasing generation, metadata region bounds, inode capacity, root inode ID,
and an FNV-1a metadata checksum. The validator rejects overlapping/out-of-order
regions, out-of-device volume sizes, undersized metadata regions, and invalid
checksums before a mount implementation may trust the layout.

The bitmap has one bit per data-area sector (bit zero describes LBA
`data_lba`). A set bit means allocated. The current helper finds a contiguous
free run; allocation and persistence are deliberately not performed by this
metadata-only layer.

## Inodes

The fixed-size inode record stores:

- A unique nonzero inode ID and parent directory ID.
- File or directory type.
- A component name of up to 47 bytes (no slash, dot, or dot-dot components).
- File size and a contiguous data extent.
- A generation and per-record checksum.

Inode 1 is the root directory: it has no parent or name and owns no data
extent. Other inodes require a parent ID. Directory inodes have no data extent,
which makes empty directories representable independently of their children.

This initial layout uses one contiguous extent per file. The bitmap can reclaim
freed sectors and choose a new run, but a fragmented volume may lack a large
enough contiguous run despite having sufficient total free space. Extent chains
or an extent tree are a future format revision, not an implicit assumption.

## Safety and compatibility

- SFS1 remains unchanged and must continue to mount.
- SFS2 detection must be explicit and read-only until the complete backend is
  implemented and covered by disk-image tests.
- Never auto-format an unknown, damaged, or merely unsupported volume.
- Migration must copy SFS1 contents to a separate SFS2 volume/image and verify
  the copy before switching boot configuration; it must not rewrite the source
  volume in place.
- A later writable backend needs ordered metadata commits, crash recovery,
  allocator rollback on failed I/O, directory-parent validation, duplicate
  name checks, and extent/bitmap consistency checks.
- No format or migration command is included in this foundation change.

## Implemented helpers

- Version, geometry, metadata-region, and checksum validation for superblocks.
- Inode type, root-inode, component-name, extent-boundary, and checksum
  validation.
- Bounds-checked bitmap-bit lookup.
- Search for a contiguous free bitmap run.
- Read-only mount API using caller-supplied sector I/O callbacks; it never writes or formats.
- Full inode-table loading with table-padding handling, root/parent/type checks, unique sibling names, and rejection of orphaned/cyclic directory trees.
- Absolute or relative path resolution, stat, directory enumeration, and ranged reads across sector boundaries.
- CI-only metadata checks (`sfs2_ci_metadata_self_test`) and an in-memory disk-image test (`sfs2_ci_mount_read_test`) covering mount, empty directories, lookup, file reads, and damaged metadata rejection.

The next milestone is a host-side formatter and a journaled writable backend with create/replace/unlink and bitmap updates. A separate migration tool must copy and verify SFS1 data into a new SFS2 image. SFS1 migration follows only after crash-recovery and image-level tests exist.
