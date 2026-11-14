# VexOS 2.0 engineering roadmap

This document defines the engineering work required to move VexOS from a bootable desktop prototype toward a reliable, maintainable operating system. It is intentionally incremental: each phase should land as a reviewable change with a working build and regression coverage. The kernel and boot path should not be rewritten just for the sake of rewriting them.

## Current baseline

VexOS already has a UEFI boot path, x86-64 kernel, physical and virtual memory managers, heap, PIT-driven task switching, PCI enumeration, xHCI USB HID input, a framebuffer desktop, and ATA-backed SimpleFS/FAT32 read support. The QEMU workflow exercises boot, memory primitives, task switching, storage, USB keyboard input, USB tablet movement, desktop clicks, and several applications.

The current limitations are architectural rather than cosmetic:

- `desktop.c` combines rendering primitives, window management, application state, input handling, and app-specific drawing.
- The renderer has one fixed-size bitmap font and no explicit surface abstraction or well-defined pixel-format contract.
- Window state and app lifetime are mostly represented by global variables and application-specific flags.
- USB host and HID paths need allocation-failure handling, descriptor validation, device recovery, and more hardware-independent tests.
- Storage code combines SimpleFS and FAT32 logic directly with the ATA PIO driver instead of using a general block-device interface.
- The image viewer supports PNG and baseline JPEG, but not progressive JPEG. Unsupported formats must be reported accurately.
- CI has good boot-level coverage, but must keep growing toward feature-level tests and screenshot review.

## Phase 0 — make the code testable

Status: core regression suites are implemented and the current PR head has passed the QEMU interaction workflow; keep open while the wallpaper refresh and final review are validated.

- Extract low-level framebuffer operations into `gfx.c` and expose them through `gfx.h`.
- Keep desktop code responsible for layout and window content, not raw pixel loops.
- Validate every drawing surface and clip rectangles without integer wraparound.
- Add kernel-side regression checks for rendering, pointer/keyboard queue behavior, window-manager visibility/focus/z-order/geometry, and baseline/progressive-marker/truncated JPEG behavior.
- Keep the existing QEMU boot, storage, keyboard, mouse, shell, and screenshot tests green.

Acceptance: normal and CI kernel builds pass; all existing QEMU markers and the new renderer/input self-tests pass.

## Phase 1 — kernel and device stability

Status: xHCI controller initialization now checks DCBAA, scratchpad array/buffer, command-ring, event-ring, and ERST allocations; HID configuration/interrupt buffers and report lengths are also checked. Repeated-device stress, controller timeouts, disconnect/reconnect recovery, and block-device work remain.

- Audit every page allocation in xHCI, AHCI, ATA, and the kernel core; failed allocations must fail cleanly rather than dereference null. Initial xHCI controller/ring/scratchpad allocation paths now fail with diagnostics and release the pages allocated earlier in initialization.
- Validate USB configuration and HID report descriptors before using offsets, lengths, report IDs, or endpoint parameters. Initial work now guards xHCI page allocations, validates endpoint parameters, bounds parsed HID fields to report packets, and discards truncated reports.
- Make transfer submission, completion, timeout, and re-arm paths explicit; log recoverable device errors without flooding the serial console.
- Add deterministic tests for HID report parsing, pointer scaling, keyboard transitions, queue overflow, and device disconnect/reconnect behavior.
- Separate storage operations from controller details with a small block-device interface. Keep ATA PIO as the first backend and add AHCI only after the interface is tested.
- Expand paging, heap, PMM, task, and interrupt tests, including failure paths.

Acceptance: input devices remain responsive through repeated idle/move/click/key cycles; invalid descriptors and allocation failures do not hang or crash the desktop; storage tests pass through the block-device API.

## Phase 2 — graphics foundation

Status: reusable clipped framebuffer primitives and a procedural planet/orbit wallpaper are in the PR; typed surfaces, a larger font interface, image transforms, and dirty-region rendering remain.

- Define the framebuffer contract explicitly: pixel format, bytes per pixel, row pitch, dimensions, and ownership.
- Introduce reusable surfaces and safe fill, blend, blit, glyph, and text operations.
- Add a real font interface so the desktop is not tied to a single embedded 8x8 bitmap font.
- Add clipping, alpha, scaling, and image conversion tests, including awkward dimensions and clipped edges.
- Introduce dirty-region tracking only after correctness tests exist; full-frame composition remains the fallback.

Acceptance: renderer tests pass independently of desktop layout; the same renderer works on the boot framebuffer, desktop backbuffer, and app-owned surfaces.

## Phase 3 — desktop and window manager

Status: window visibility, focus, z-order, display-bound geometry clamping, and bounded resize calculations are in the standalone window manager; desktop windows now expose a bottom-right resize grip using one shared drag path. The QEMU workflow is being extended to drag Terminal's grip. Event routing, minimize/maximize/restore controls, and full app lifecycle are still coupled to desktop.c.

- Separate the window manager from application implementations.
- Give each window explicit bounds, state, z-order, focus, and event handling. Window records now share one rectangle type; drag movement is clamped and resizing respects minimum and display maximum sizes.
- Route pointer and keyboard events through one event queue rather than mixing polling logic with app state changes.
- Implement consistent title bars, minimize/maximize/close behavior, resizing, focus, keyboard shortcuts, and pointer capture.
- Make layout adapt to framebuffer size instead of assuming one desktop geometry.
- Keep drawing declarative at the app level and use the shared graphics API for rendering.

Acceptance: overlapping windows retain correct z-order and focus; drag movement and live resize are covered by repeatable QEMU tablet tests. Minimize, maximize, restore, and close lifecycle tests remain outstanding.

## Phase 4 — applications and storage experience

- Make the file manager support selection, navigation, metadata, and clear error states.
- Make the text editor handle file open/save, dirty state, and bounded text editing.
- Keep PNG and baseline JPEG previews reliable; baseline JPEG now has a real decoder fixture in CI. Progressive JPEG remains unsupported until a decoder and fixtures are integrated.
- Make the terminal use a real command registry and report command errors consistently.
- Add a settings app for display, input, and storage information.
- Use the same app lifecycle and window API for every application.

Acceptance: applications can open, close, regain focus, and report missing files or unsupported formats without crashing the desktop.

## Phase 5 — verification and release quality

- Maintain deterministic QEMU scenarios for first boot, storage, USB keyboard, USB tablet, window interaction, and application workflows.
- Add valid and invalid file fixtures for text, PNG, baseline JPEG, progressive JPEG, and malformed images.
- Capture screenshots at multiple resolutions and compare them against reviewed baselines where stable.
- Add stress tests for repeated window open/close, file reads, allocation/free cycles, and long-running pointer input.
- Preserve serial logs, QEMU logs, and screenshots as CI artifacts on failure.
- Track boot time, frame time, memory use, and input-to-pointer latency to detect regressions.

Acceptance: a release candidate passes the complete QEMU matrix, has no known crash or input-loss regressions, and documents unsupported hardware and image formats.

## Engineering rules

1. Do not merge an architectural phase without a normal build and its relevant regression tests passing.
2. Do not claim hardware support based only on QEMU success; keep a hardware compatibility matrix.
3. Do not hide unsupported functionality behind success-looking UI. Surface useful error messages and serial diagnostics.
4. Keep changes small enough to review and revert. Preserve the existing UEFI boot path unless a tested change requires otherwise.
5. Treat screenshots as product quality checks, not as substitutes for functional tests.
