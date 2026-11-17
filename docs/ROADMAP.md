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
1. Complete the core SFS2 writable milestone. The versioned metadata format, read-only compatibility mount, journal-enabled formatter/verifier, separate-image SFS1 migration, journal replay, journaled nested-file create/replace/unlink, automatic parent-directory creation, 16 MiB file transactions, and write-boundary fault injection are implemented. Remaining work is transaction concurrency protection, broader physical-device qualification, and future extent-tree support. Keep SFS1 volumes mountable throughout.
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

**Status: Partial — network-device layer, E1000 driver, Ethernet II receive dispatch, and ARP resolution implemented**

Implemented on `feature/network-foundation`:
- Generic network-device registration and bounded interface table.
- A transmit/receive callback contract with frame-length validation and per-device counters.
- Polling-only Intel 82540EM-compatible E1000 driver for QEMU (`8086:100E`).
- PCI BAR discovery, MMIO mapping, controller reset, MAC address discovery, RX/TX descriptor rings, and packet-buffer setup.
- Kernel-loop polling with bounded receive work; interrupts remain disabled until general network IRQ routing is available.
- Bounded Ethernet II EtherType registry and receive dispatch for registered protocols.
- Ethernet header length and MTU validation, big-endian EtherType parsing, and drop accounting for malformed, unsupported, or unhandled frames.
- Explicit borrowed-buffer contract: protocol handlers execute synchronously and must copy any packet data they need to retain.
- Host-side tests for Ethernet II bounds, handler registration, EtherType dispatch, unsupported 802.3/VLAN frames, and receive-drop accounting.
- ARP Ethernet/IPv4 packet parsing with strict hardware/protocol/length/opcode checks and Ethernet-source consistency validation.
- A bounded 32-entry per-interface IPv4-to-MAC cache with 60-second tick-based expiry, same-MAC refresh, and conflict rejection rather than replacing a live mapping.
- Solicited ARP reply validation and request handling; requests are answered only for a local source address currently represented by a pending IPv4 send.
- A fixed eight-packet pending IPv4 queue. Cache misses trigger an ARP request, matching replies flush queued packets, and unresolved packets expire after five seconds.
- An IPv4 send handoff API that validates the basic IPv4 header/total length and constructs Ethernet II frames after resolution. This is a handoff for a future IP layer, not a complete IPv4 implementation.
- Host-side ARP tests for malformed packets, cache learning/conflicts/expiry, known-neighbor sending, request/reply resolution, and pending-queue capacity/timeout.
- QEMU CI booted the E1000 device successfully and completed descriptor-ring initialization checks.

Next milestones:
1. IPv4 validation, local interface addressing, ICMP echo, and an explicit fragmentation policy.
2. UDP transport and temporary development-only NetLog over UDP (disabled in official builds).
3. DHCP and DNS clients.
4. TCP with sequence tracking, acknowledgements, retransmission, flow control, and connection lifecycle.
5. User-mode socket API and loopback/QEMU integration tests.
6. Add further device drivers based on physical hardware inventory; do not infer laptop NIC compatibility from QEMU tests.

Current limitations:
- ARP resolution is implemented, but there is no IPv4 stack or configured interface address yet; ICMP, DHCP, DNS, UDP, TCP, sockets, and Internet access remain unavailable.
- The Ethernet dispatcher currently accepts untagged Ethernet II frames only; IEEE 802.3 length-format frames and VLAN/QinQ encapsulation are rejected.
- Protocol handlers are synchronous and receive borrowed pointers into the NIC RX buffer. Retained packets require a future explicit buffer-copy or ownership-transfer API.
- Initial E1000 DMA storage relies on VexOS's current low identity-mapped kernel image and coherent x86 DMA assumptions.
- Driver operation is polled in task context; no E1000 interrupts, MSI/MSI-X, APIC routing, or SMP support.
- QEMU success is not physical hardware qualification.

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

The core SFS2 writable milestone is implemented: the metadata format, read-only compatibility mount, host formatter/verifier, offline SFS1 migration, redo-journal replay, journaled nested-file create/replace/unlink, 16 MiB transactions, and fault-injection tests at each journal write boundary are in place. Next address transaction concurrency protection and physical-device qualification. Keep SFS1 mountable throughout. Before any installer or arbitrary physical-disk support, require explicit user confirmation for destructive formatting.
