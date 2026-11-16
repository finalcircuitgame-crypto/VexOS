# VexOS VFS Integration Design

## Goal

Introduce a filesystem-independent kernel interface while preserving the current SimpleFS disk image and its contents. This is an integration layer, not a replacement filesystem format.

## Repository audit findings

- SimpleFS SFS1 is a flat root-file store with a fixed file table; it does not currently expose directories or unlink.
- The storage layer chooses ATA PIO or AHCI, and `simplefs_init()` mounts once. If the disk is neither recognized FAT32 nor a valid SFS1 image, current SimpleFS behavior may format it; this behavior is pre-existing and must be reviewed separately before treating arbitrary disks as safe.
- The FAT32 implementation scans root short-name files and is read-only; the current ranged-read helper supports SFS1 only.
- User-mode descriptors are task-local and own VFS file handles with independent offsets; process cleanup closes outstanding handles.
- Desktop file viewing, wallpaper loading, ELF loading, shell listing, File Manager enumeration, and Notepad load/save now use VFS. The VFS backend is the intended boundary for SimpleFS access.

## VFS v1 implementation

`src/include/vfs.h` and `src/kernel/core/vfs.c` define the first adapter layer. It has a root mount with a backend operation table and supports stat, read, ranged read, write, count, and flat-file listing. It normalizes absolute and relative root-file paths, strips `.` components, and rejects `..` traversal and nested paths. It also provides per-open file handles with independent offsets, read, seek, and close; user-mode open/read/seek/close syscalls now use these handles, and process cleanup closes outstanding handles. Desktop file consumers use VFS for metadata, reads, writes, and listing; direct SimpleFS operations are contained behind the VFS backend except for storage-specific regression fixtures. This is intentionally a compatibility layer; it is not yet a hierarchical VFS with vnode caching or mount-point routing.

## Required repository audit before further implementation

Identify and document:
- SimpleFS on-disk structures, magic/version fields, allocation model, and image builder.
- Storage block read/write interface and its locking/interrupt assumptions.
- Existing file and directory APIs, including error conventions.
- Direct SimpleFS call sites in desktop consumers and applications; distinguish backend implementation and explicit storage regression fixtures from normal file consumers.
- Whether current file handles are global, task-local, or already represented by process file descriptors.

Do not infer these details or change the disk format during the initial VFS work.

## Target object model

- **Filesystem type:** registers a backend and its mount operation.
- **Mount:** binds a backend instance to a normalized mount path.
- **Vnode:** represents a file or directory independent of its backing filesystem.
- **Open file:** holds a vnode reference, access flags, and a per-open file offset.
- **Directory iterator:** holds iteration state without exposing backend-private structures.

Backend operations should cover lookup, create, unlink, read, write, stat, and directory iteration. Unsupported operations must return an explicit error rather than silently succeeding.

## Path resolution

- Require absolute paths at the VFS boundary, or normalize relative paths against an explicit current working directory.
- Collapse repeated separators and `.`; resolve `..` without allowing traversal above root.
- Select the longest matching mount-point prefix.
- Reject malformed paths and enforce a documented maximum path/component length.
- Keep mount lookup and vnode lifetime rules explicit.

## File descriptor integration

- A process owns its descriptor table.
- Each descriptor refers to an open-file object; duplicated descriptors may share an open-file object and offset, while independent opens have independent offsets.
- Validate descriptor ownership and user buffers at syscall boundaries.
- Close all descriptors during process cleanup, including abnormal exit.
- Define behavior for read/write on directories, closed descriptors, invalid flags, and end-of-file.

## Incremental implementation order

VFS v1 is already present. The next steps must preserve its compatibility boundary while replacing the flat-file-only adapter with hierarchical vnode and directory semantics.

1. Audit and document SimpleFS and all call sites.
2. Add VFS types and a root mount backed by SimpleFS.
3. Implement lookup, open, read, write, close, and stat.
4. Route one small consumer (preferably a kernel test or shell command) through VFS.
5. Route remaining consumers and remove direct backend dependencies.
6. Integrate process-owned descriptors and test cleanup.
7. Add directory iteration, create, unlink, and error-path coverage.
8. Only then add another backend such as FAT32.

## Required regression tests

- Existing files from a pre-VFS storage image still open and read correctly (covered by the current QEMU fixture path through the VFS adapter).
- Create/write/close/reopen/read returns the written bytes.
- Independent opens maintain independent offsets.
- Directory iteration terminates and handles empty directories (future milestone; not available in VFS v1).
- Missing paths, invalid flags, read-only filesystems, and invalid descriptors return stable errors.
- Process exit releases all open descriptors.
- Repeated operations do not leak vnode/open-file references.

## Current VFS v1 limitations

- Only the root mount exists; nested directory paths are rejected rather than silently flattened.
- The current open-file handle is a lightweight value object, not a reference-counted vnode/open-file cache.
- No create/unlink or directory creation operations are exposed through VFS.
- The SimpleFS backend preserves its current disk format and limitations.
- FAT32 ranged reads are not implemented by the current backend.

## Non-goals for the first VFS pull request

- No on-disk format migration.
- No FAT32 implementation.
- No symlinks, permissions model, journaling, or caching redesign.
- No broad rewrite of the storage driver.
