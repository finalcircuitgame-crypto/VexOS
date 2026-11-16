# VexOS Roadmap: From Kernel to General-Purpose OS

This roadmap tracks the work required to turn VexOS from a bootable kernel and desktop prototype into a maintainable, general-purpose operating system. Status labels describe verified implementation, not aspirations.

## 1. Process isolation

**Status: Core implemented; hardening remains**

Implemented:
- Process control blocks, monotonic PIDs, parent tracking, lifecycle states, exit codes, and process names.
- Per-process address spaces and private user mappings.
- User-mode context, CR3 switching, TSS.RSP0 updates, and deferred stack cleanup.
- Parent/child wait, exit, zombie reaping, and cleanup of descriptors and mmap bookkeeping.
- Concurrent ring-3 stress coverage with four workers using the same virtual mapping address but independent private memory.

Remaining:
- Deliberate invalid-pointer and permission-violation tests.
- User-process fault containment and reliable reporting of fault exit reasons.
- Broader resource-leak and repeated create/exit stress tests.

## 2. Multitasking

**Status: Core implemented; application-level validation remains**

Implemented:
- Timer-driven preemptive scheduling and context switching.
- Runnable, blocked, and zombie task handling, including an idle task.
- Atomic wait/block/wakeup paths against timer interrupts.
- Concurrent execution of four ring-3 workers and parent wait/reap verification.

Remaining:
- Launch several independently ELF-loaded applications at once.
- Validate fairness, starvation resistance, and long-running scheduler stability.
- Add process signals/termination policy only when the basic application lifecycle is stable.

## 3. Virtual File System (VFS)

**Status: Initial VFS layer implemented; filesystem-tree functionality remains**

The existing SimpleFS persistent storage remains the first backend. VFS v1 now provides a root mount, backend operation table, path normalization for root-level files, stat, read, ranged read, write, and file listing adapters. User-mode open/read syscalls now resolve files through VFS. The existing SimpleFS disk format is unchanged.

Milestones:
Completed in VFS v1:
- Audited the current SimpleFS interface and confirmed that SFS1 is a flat root-file store; the existing FAT32 path is a limited read-only root-file scanner.
- Added a private backend-operations table and mounted the current SimpleFS API at `/`.
- Added path normalization, root stat, read, ranged read, write, and flat-file listing adapters.
- Added per-open VFS file handles with independent offsets, seek, close, and descriptor cleanup on process exit.
- Routed user-mode open/read/seek/close syscalls through VFS; each process retains its own descriptor table.
- Added CI checks for existing-file lookup, absolute/relative paths, traversal/nested-path rejection, write/read, ranged reads, and independent opens.

Remaining milestones:
1. Add explicit vnode/open-file objects and reference/lifetime rules.
2. Implement directory nodes and hierarchical paths; `/docs/readme.txt` is not supported by the current flat backend.
3. Move desktop and shell file consumers from direct SimpleFS calls to VFS.
4. Expand process descriptors to reference VFS open-file objects, retaining independent offsets and safe cleanup.
5. Add create/unlink semantics and comprehensive error/concurrency tests.
6. Add FAT32 as a separate backend after the VFS contract stabilizes.

Acceptance criteria:
- Existing persistent files remain readable and writable without changing their on-disk format.
- Hierarchical paths such as `/docs/readme.txt` resolve consistently (future milestone; not supported by VFS v1).
- Multiple processes can open the same file with independent offsets.
- File descriptors are reclaimed on exit and cannot be used by unrelated processes.
- CI exercises create, read, write, list, seek, close, and error cases.

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
- File manager backed by VFS.
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

Complete the VFS inventory and introduce the VFS abstraction over the existing SimpleFS backend without changing the on-disk format. Do not start FAT32 until existing SimpleFS callers are routed through VFS and regression tests pass.
