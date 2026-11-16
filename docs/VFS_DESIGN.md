# VexOS VFS Integration Design

## Goal

Provide a filesystem-independent kernel interface while preserving existing SimpleFS SFS1 volumes. VFS is a compatibility and namespace layer, not yet a replacement on-disk filesystem.

## Current implementation

`src/include/vfs.h` and `src/kernel/core/vfs.c` define a single root mount over a private SimpleFS backend operations table. Desktop file viewing, wallpaper loading, ring-3 ELF loading, the shell `fs` listing, File Manager, Notepad, and ring-3 open/read/seek/close syscalls use VFS rather than calling SimpleFS directly.

### Path and directory model

- Absolute and root-relative paths are normalized to a relative backend name.
- Repeated separators and `.` components are collapsed. `..` is rejected rather than resolved.
- SFS1 has a 32-byte name field, so the full stored relative path is limited to 31 bytes, including `/` separators.
- A directory is synthesized when one or more file names have that directory as a component prefix. Directory iteration returns immediate children and deduplicates repeated directory prefixes.
- Empty directories and persistent directory metadata do not exist. Removing the final file under a synthesized directory makes that directory disappear.
- Directory iteration uses the backend file-table index. It is not a snapshot; concurrent file-table mutation can change what a long-running iterator observes.

### Open-node lifetime

- VFS has a fixed cache of 64 node records for simultaneously open/reserved unique paths.
- Each open file handle holds a reference to a shared node; independent opens have independent offsets.
- The path reservation state prevents new opens while a path is being replaced/unlinked, and reference counts prevent replacement/unlink while any handle remains open.
- Handles and directory iterators must be zero-initialized before their open function is called.
- A too-small `vfs_dir_read` output buffer returns `-1` without consuming the current entry, so the caller can retry with a larger buffer.

### Locking

- SimpleFS owns mutable global tables and storage allocator state, so VFS serializes backend operations with a spinlock.
- Node records and mount initialization use separate spinlocks. Lock acquisition saves the caller's interrupt-enable state and disables local interrupts while held, preventing the single-core PIT scheduler from preempting a task that owns a VFS lock. This is not an SMP lock implementation.
- Namespace mutations (create, replace/write, and unlink) hold a namespace lock across parent/type validation, node reservation, and backend commit. This prevents concurrent mutations from creating both a file and a child beneath that file.
- VFS calls are task-context-only; do not call them from interrupt handlers.
- The current kernel is single-core. SMP, interrupt/preemption lock ordering, and lock-holder starvation require a separate audit before multicore support.

## Storage backends and limits

### SimpleFS SFS1

- Slash-separated paths are stored in the existing SFS1 name field; no SFS1 metadata structure or version changed.
- `vfs_create` creates an empty file. `vfs_write` creates or replaces a file. `vfs_unlink` clears a file-table entry and allows the slot to be reused.
- SFS1 allocation is append-only. Unlink and replacement do not reclaim old data sectors because SFS1 has no free-space map. Repeated writes/deletes can exhaust the data area even when the table has free entries.
- Mount validates every live table entry before exposing it: bounded relative path syntax, no duplicate or file/directory-conflicting names, no overlapping data extents, file-count consistency, and extents within the committed allocation and device-addressable bounds. ATA PIO currently enforces its 28-bit LBA address ceiling; it does not yet expose exact IDENTIFY capacity.
- Metadata writes are not journaled or crash-atomic. A power loss during table/superblock updates can leave inconsistent metadata.

### FAT32

- FAT32 support remains limited to the root directory, short 8.3 names, and read-only operations.
- `simplefs_read_file_at` now follows FAT cluster chains for ranged reads and bounds cluster traversal using the BPB volume geometry.
- The new FAT32 ranged-read path has compile coverage through the kernel build but still needs a dedicated FAT32 image fixture to validate fragmented files, end-of-chain handling, and offsets across cluster boundaries.
- FAT32 subdirectories, long filenames, and writes are not implemented.

## System-call boundary

Ring-3 processes currently have task-local file descriptor tables. Open handles use VFS; reads validate the user buffer and advance the handle offset; seek and close operate on the same handle. Process cleanup closes remaining handles.

Ring-3 has task-local file and directory descriptors. In addition to open/read/seek/close, it exposes create, whole-file replacement write, unlink, stat, opendir, and readdir. The write path copies a bounded user buffer into kernel-owned memory before invoking VFS. Detailed register and return conventions are in [USER_SYSCALLS.md](USER_SYSCALLS.md). Per-user filesystem permissions are not implemented; ring-3 tasks share the volume namespace.

## Known safety issue

`simplefs_init()` only mounts recognized FAT32 or validated SFS1 volumes. It refuses to format unrecognized media and refuses destructive recovery when an SFS1 file table cannot be read. There is no in-kernel format operation; a future installer must make formatting a separate, explicitly confirmed action. A failed mount leaves the volume unavailable rather than overwriting it.

## Regression coverage

The CI QEMU test harness now checks:
- Existing SFS1 file stat/read and path normalization.
- In-memory malformed-table fixtures covering traversal names, unterminated names, file/directory conflicts, overlapping extents, and extents beyond the committed allocation boundary.
- Nested path write/read and implicit directory stat.
- Root and child directory iteration, including duplicate synthetic-directory suppression.
- Retry after a directory read with an undersized output buffer.
- Multiple opens with independent node references and offsets.
- Rejection of write/unlink while a file is open, and successful unlink after the final close.
- Empty-file create, parent-is-file rejection, and root-file creation rejection.

The latest CI run must be checked against the exact PR head before this work is considered validated. A dedicated FAT32 fixture is still outstanding.

## Remaining work

1. Complete the SFS2 writable backend. The kernel probes SFS2, validates its inode tree and data bitmap, and exposes read-only path/stat/list/ranged-read operations through VFS. Offline host formatting, metadata verification, and separate-image SFS1 migration tooling now exist in `tools/sfs2_image.py`; they do not make the kernel writable. Remaining work is a journaled create/replace/unlink backend, persistent bitmap updates, recovery-before-mount, storage flush/error propagation, and QEMU fault-injection tests. See [SFS2_FORMAT.md](SFS2_FORMAT.md).
2. Add a FAT32 image fixture covering fragmented files, multi-cluster reads, invalid cluster chains, and end-of-chain handling; later design subdirectory and long-name support.
3. Add safe ring-3 create/write/unlink/directory syscalls with user-buffer validation and per-process policy.
4. Add tests for cache exhaustion, concurrent operations, interrupted I/O, and repeated create/delete cycles.
5. Add a separate installer-only format operation that requires explicit user confirmation; normal mount code must remain non-destructive.
