# VexOS Roadmap: From Kernel to General-Purpose OS

This roadmap tracks the work required to turn VexOS from a bootable kernel and desktop prototype into a maintainable, general-purpose operating system. Status labels describe verified implementation, not aspirations.

## 1. Process isolation

**Status: Finished — core process-isolation milestone implemented and CI-validated**

Implemented:
- Process control blocks, monotonic PIDs, parent tracking, lifecycle states, exit codes, and process names.
- Per-process address spaces and private user mappings.
- User-mode context, CR3 switching, TSS.RSP0 updates, and deferred stack cleanup.
- Parent/child wait, exit, zombie reaping, and cleanup of descriptors and mmap bookkeeping.
- Concurrent ring-3 stress coverage with four workers using the same virtual mapping address but independent private memory.

Post-completion hardening:
- Add deliberate invalid-pointer and permission-violation tests.
- Improve user-process fault containment and reporting of fault exit reasons.
- Expand resource-leak and repeated create/exit stress tests.

## 2. Multitasking

**Status: Finished — core preemptive multitasking milestone implemented and CI-validated**

Implemented:
- Timer-driven preemptive scheduling and context switching.
- Runnable, blocked, and zombie task handling, including an idle task.
- Atomic wait/block/wakeup paths against timer interrupts.
- Concurrent execution of four ring-3 workers and parent wait/reap verification.

Post-completion hardening and expansion:
- Validate simultaneous execution of several independently ELF-loaded applications.
- Expand fairness, starvation-resistance, and long-running scheduler-stability tests.
- Consider process signals and additional termination policies as the application lifecycle grows.

## 3. Virtual File System (VFS)

**Status: Hierarchical VFS and node lifetimes implemented over SFS1; storage-format limits remain**

The existing SimpleFS SFS1 volume remains the first backend. VFS now exposes normalized hierarchical paths, implicit directories, directory iteration, stat, read, ranged read, write, create, unlink, and per-open file handles. Slash-separated names are stored in SFS1's existing 32-byte name field, so the SFS1 structure and version remain unchanged. The full stored relative path is limited to 31 bytes. Directories are synthesized while they contain at least one descendant file; empty directories are not persisted.

The desktop viewer, wallpaper loader, ring-3 ELF loader, shell `fs` command, File Manager, Notepad, and user-mode read/open/seek/close syscalls use VFS rather than calling SimpleFS directly.

Implemented:
- Added a private backend-operation table and root mount over the existing SimpleFS backend.
- Added path normalization with repeated-separator and `.` cleanup, while rejecting `..` traversal.
- Added file/directory stat, ranged reads, immediate-child directory iteration, and synthetic directory lookup.
- Added a bounded cache of 64 unique VFS node paths with reference counts; each open handle owns a node reference and an independent cursor.
- Blocked replacement and unlink while any handle references the target file.
- Added create-empty and unlink APIs. SFS1 unlink releases the file-table entry for reuse, but cannot reclaim old data sectors because SFS1 has no free-space map.
- Added FAT32 ranged reads by following the file's cluster chain. FAT32 remains root-only, short-name, and read-only.
- Updated File Manager for folder navigation and the shell `fs` command for root directory listing.
- Added CI coverage for nested paths, implicit directories, directory enumeration, independent opens, create/unlink, and open-node lifetime rules.
- Added ring-3 create, whole-file replacement write, unlink, stat, directory-open, and directory-read syscalls with bounded kernel copies and user-buffer validation.
- Preserved the existing SFS1 disk layout and existing file names.

Remaining:
1. Design a versioned SFS2 or compatible metadata layer with a free-space map, persisted directory records (including empty directories), longer paths, and crash-safe metadata updates. Keep existing SFS1 volumes mountable and define a recovery/migration path before changing formats.
2. Add FAT32 subdirectory traversal and long-filename handling; retain explicit read-only behavior until writes are designed and tested.
3. Stress-test concurrent open/read/create/unlink, cache exhaustion, interrupted storage I/O, and process-exit cleanup.
4. Add a separate installer-only format operation that requires explicit user confirmation; normal mount code must remain non-destructive.

Acceptance criteria:
- Existing SFS1 files remain readable and writable without changing their disk structure.
- Hierarchical paths resolve within the 31-byte SFS1 name limit; empty directories are not stored.
- Independent opens retain independent offsets, and open handles prevent replacement or unlink.
- Directory iteration returns immediate children and handles duplicate implicit directories.
- CI covers create, read, write, list, seek, close, unlink, and error cases.

## 4. Hardware compatibility

**Status: Partial**

Existing work includes UEFI boot, PCI enumeration, xHCI support, and an AHCI SATA target. NVMe is not implemented, and AHCI has not been qualified on the target physical laptop.

Milestones:
- Improve ACPI table discovery and MADT parsing.
- Establish robust APIC interrupt routing and timer calibration.
- Complete and test AHCI/SATA support; implement NVMe.
- Improve USB keyboard/mouse coverage and device hot-plug behavior.
- Add hardware inventory and driver diagnostics.
- Test on documented physical machines; QEMU success alone is not hardware qualification.

## 5. Networking

**Status: Not started / foundational work required**

Milestones:
- Network-device abstraction and an Ethernet driver (begin with a QEMU-supported NIC).
- Ethernet framing and ARP.
- IPv4, ICMP, and packet-buffer ownership.
- UDP and TCP with timeout/retransmission handling.
- DHCP and DNS client.
- Socket API for user applications.
- Loopback tests and QEMU network integration tests.

## 6. Userland and system services

**Status: Partial — ring-3 execution exists; a complete userland does not**

Milestones:
- A stable executable loader that creates a separate process.
- `spawn`/`exec`, argument and environment passing, and exit-status propagation.
- A documented system-call ABI and user-space runtime.
- Minimal libc/POSIX-inspired compatibility layer where practical.
- Init/system process and supervised background services.
- Native command-line utilities and a package/application installation format.
- Application permissions and resource limits.

## 7. Desktop, installation, and system maintenance

**Status: Prototype desktop exists; install/update path is not production-ready**

Milestones:
- Keep the desktop and window/compositor responsibilities separated from kernel policy.
- Login/session startup and user configuration.
- Complete the VFS-backed File Manager and Notepad experience, including directory navigation and safe file operations.
- Audio, graphics acceleration, power management, and laptop input support.
- Non-destructive installer with explicit disk selection and confirmation.
- Bootloader installation, recovery environment, and safe system updates.
- Documented backup/recovery and hardware compatibility process.

## Engineering rules

- Keep `main` bootable; develop substantial work on feature branches and merge through reviewed pull requests.
- Every subsystem milestone needs deterministic tests and CI evidence.
- Do not claim physical hardware support based only on QEMU.
- Do not partition, format, or overwrite user storage without an explicit, separately reviewed installer workflow.
- Preserve existing disk formats until a migration and recovery strategy exists.

## Immediate next objective

Design a versioned storage format with a free-space map, durable directory metadata, and crash-safe metadata updates while keeping existing SFS1 volumes mountable. Before any installer or arbitrary physical-disk support, remove SimpleFS's implicit format-on-unrecognized-disk behavior and require explicit user confirmation for destructive formatting.
