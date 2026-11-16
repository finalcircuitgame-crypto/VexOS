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

**Status: In progress — design and integration are the next work**

The existing SimpleFS persistent storage must remain usable while filesystem access is moved behind a filesystem-independent VFS interface.

Milestones:
1. Inventory the existing SimpleFS format, storage driver, file APIs, and every caller.
2. Define VFS objects and operations: filesystem, mount, vnode/inode, directory iteration, open/read/write/close, create, unlink, and stat.
3. Implement path normalization and mount-point lookup, starting with a root mount backed by the existing SimpleFS.
4. Route kernel and desktop file access through VFS instead of calling SimpleFS directly.
5. Connect per-process file descriptors to VFS open-file objects with independent offsets and safe cleanup on process exit.
6. Add directory and file operation tests, error-path tests, and concurrent access tests.
7. Add a second filesystem backend only after the abstraction is stable (FAT32 is the first candidate).

Acceptance criteria:
- Existing persistent files remain readable and writable without changing their on-disk format.
- Paths such as `/docs/readme.txt` resolve consistently.
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
