# VexOS Process and User-Mode Isolation

## Implemented boundary

VexOS has a kernel-owned process table and a ring-3 execution path. A process record tracks a monotonic PID, parent PID, task ID, lifecycle state, exit code, user-mode flag, address space, and kernel stack ownership.

The current x86_64 implementation provides:

- A distinct page-table address space for each user process, with kernel mappings retained for syscall/interrupt entry.
- User mappings that are cleared before they are exposed to ring 3.
- Per-task kernel stacks and TSS.RSP0 updates when switching into a process.
- User-pointer validation at syscall boundaries; filesystem paths and write payloads are copied or validated before kernel use.
- Task-local syscall state indexed by scheduler task ID. File descriptors, directory descriptors, mmap cursor, and mmap accounting are not shared between processes.
- Process-exit cleanup that closes every file and directory handle owned by that task, clears mmap accounting, and destroys the address space after the task is off-CPU.
- Parent/child tracking, wait/exit status, zombie handling, orphan adoption, and deferred task/resource reaping.

The QEMU CI stress test starts four ring-3 workers. They intentionally use the same virtual mapping address while verifying that their physical memory contents remain private, then exit and are waited for/reaped. The kernel emits a dedicated CI pass marker for this test.

## Filesystem isolation semantics

VFS node references are global kernel objects, while user file descriptors are process-local handles. A process cannot use another process's descriptor number to access its open handle. Process exit closes its handles so an abandoned open reference cannot indefinitely block VFS replacement or unlink.

This is descriptor isolation, not a user-account permission model. All current ring-3 processes share the same mounted volume namespace and can access paths allowed by the syscall ABI. There are no per-user owners, groups, ACLs, capability grants, or sandboxed per-process mount namespaces. Do not describe the current system as providing multi-user filesystem security.

The descriptor table is task-local rather than inherited. There is no general fork/exec contract or descriptor inheritance policy yet; these must be designed explicitly if process creation grows beyond the current kernel-managed launch paths.

## Validation

- Process_CI_UserStressTest covers concurrent ring-3 address-space isolation, shared virtual-address independence, and process exit/wait/reap.
- syscall_process_cleanup_task releases the task's open file/directory handles and mmap bookkeeping.
- Process_ReapTask invokes that cleanup even if an unusual kernel exit bypasses the normal user-mode exit/fault path.
- docs/USER_SYSCALLS.md documents the syscall register ABI and user pointer constraints.

## Next hardening steps

1. Add adversarial user-pointer tests for every syscall argument shape, including overflow and cross-page ranges.
2. Add explicit tests proving one process cannot use another process's descriptor and that process exit releases open VFS references.
3. Define descriptor inheritance and close-on-exec semantics before implementing general fork/exec.
4. Add user identity and filesystem permission checks only alongside a documented security model; do not bolt ad-hoc path filtering onto the current shared namespace.
5. Improve user-fault isolation and diagnostics, and add repeated process creation/exit tests for address-space, stack, descriptor, and mmap leaks.
