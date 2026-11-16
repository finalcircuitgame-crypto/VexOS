# VexOS VFS Integration Design

## Goal

Introduce a filesystem-independent kernel interface while preserving the current SimpleFS disk image and its contents. This is an integration layer, not a replacement filesystem format.

## Required repository audit before implementation

Identify and document:
- SimpleFS on-disk structures, magic/version fields, allocation model, and image builder.
- Storage block read/write interface and its locking/interrupt assumptions.
- Existing file and directory APIs, including error conventions.
- All direct SimpleFS call sites in the shell, desktop, applications, boot/setup flow, and tests.
- Whether current file handles are global, task-local, or already represented by process file descriptors.

Do not infer these details or change the disk format during the initial VFS work.

## Proposed object model

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

1. Audit and document SimpleFS and all call sites.
2. Add VFS types and a root mount backed by SimpleFS.
3. Implement lookup, open, read, write, close, and stat.
4. Route one small consumer (preferably a kernel test or shell command) through VFS.
5. Route remaining consumers and remove direct backend dependencies.
6. Integrate process-owned descriptors and test cleanup.
7. Add directory iteration, create, unlink, and error-path coverage.
8. Only then add another backend such as FAT32.

## Required regression tests

- Existing files from a pre-VFS storage image still open and read correctly.
- Create/write/close/reopen/read returns the written bytes.
- Independent opens maintain independent offsets.
- Directory iteration terminates and handles empty directories.
- Missing paths, invalid flags, read-only filesystems, and invalid descriptors return stable errors.
- Process exit releases all open descriptors.
- Repeated operations do not leak vnode/open-file references.

## Non-goals for the first VFS pull request

- No on-disk format migration.
- No FAT32 implementation.
- No symlinks, permissions model, journaling, or caching redesign.
- No broad rewrite of the storage driver.
