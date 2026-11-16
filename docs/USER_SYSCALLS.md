# VexOS user-mode syscall ABI

VexOS user applications invoke the kernel with int $0x80. The syscall number is in RAX; arguments use RDI, RSI, and RDX. The return value is in RAX. For VFS stat and readdir calls, file size is returned in RDX as a secondary result.

All paths are copied into a bounded kernel buffer and normalized by VFS. The current SFS1 backend limits the complete relative path to 31 bytes. User pointers are range-checked before the kernel reads or writes them.

## VFS syscalls

| Number | Name | Arguments | Return |
|---:|---|---|---|
| 16 | vfs_create | RDI=path | 0 success, -1 failure |
| 17 | vfs_unlink | RDI=path | 0 success, -1 failure |
| 18 | vfs_write | RDI=path, RSI=data, RDX=length | 0 success, -1 failure |
| 19 | vfs_stat | RDI=path | RAX=1 file, 2 directory, -1 missing/error; RDX=size |
| 20 | vfs_opendir | RDI=path | File descriptor or -1 |
| 21 | vfs_readdir | RDI=directory fd, RSI=name buffer, RDX=capacity | RAX=1 file, 2 directory, 0 end, -1 error; RDX=size |

Existing file syscalls remain:

| Number | Name | Arguments | Return |
|---:|---|---|---|
| 3 | open | RDI=path | Read-only file descriptor or -1 |
| 4 | read | RDI=fd, RSI=buffer, RDX=count | Bytes read or -1 |
| 5 | close | RDI=fd | 0 or -1 |
| 6 | seek | RDI=fd, RSI=signed offset, RDX=whence | New offset or -1 |

File and directory descriptors share the same per-process table (16 slots, with 0–2 reserved). Use syscall 5 to close either kind. Process exit closes every remaining file and directory descriptor.

## Semantics and limits

- vfs_write replaces the complete file; it is not append and does not write through an open descriptor.
- Writes are limited to 1 MiB per syscall and copied into kernel-owned memory before reaching the filesystem backend.
- File open is read-only. A path cannot be replaced or unlinked while any process has it open.
- vfs_readdir returns immediate children only. The caller supplies a writable name buffer; insufficient capacity returns -1 without consuming the entry, allowing a retry.
- vfs_stat returns node type in RAX and size in RDX. Directory size is zero.
- The kernel validates user pointers and bounds path, write length, descriptor, and directory buffer capacity. The current kernel has no per-user filesystem permissions; ring-3 tasks share the volume namespace.
- SFS1 unlink frees a file-table slot but does not reclaim deleted data sectors. Empty directories are not persisted, and full paths remain limited to 31 bytes.
- FAT32 remains read-only, root-directory-only, and 8.3-name-only.

User applications can include src/usermode/libc/vexfs.h for freestanding C wrappers such as vexfs_create, vexfs_write_file, vexfs_stat, vexfs_open_dir, and vexfs_readdir.
