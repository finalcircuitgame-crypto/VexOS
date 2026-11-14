# VexOS

VexOS is an experimental educational x86-64 operating system. It boots through UEFI, enters a freestanding kernel, and provides a framebuffer desktop with a small set of built-in applications. It is an evolving prototype, not a general-purpose replacement for Windows or Linux.

## Current capabilities

- UEFI bootloader and x86-64 kernel with physical/virtual memory management, heap allocation, interrupts, and task scheduling.
- PCI enumeration and an xHCI USB HID path for keyboard and absolute/relative pointer input.
- A framebuffer desktop with a responsive Start menu, Terminal, Files, Notepad, image Viewer, and Setup Wizard.
- Shared framebuffer drawing primitives in `src/kernel/core/gfx.c`, with clipped fills, alpha blending, rounded rectangles, bitmap text, and blitting.
- A small window manager for open/hidden state, focus, z-order, display-bound movement, and bounded mouse resizing.
- SimpleFS storage backed by a separate raw disk image.
- Text, PNG, and baseline JPEG file previews. Progressive JPEG is not supported yet.

The desktop and hardware drivers are still under active development. USB disconnect/reconnect recovery, a generalized block-device layer/AHCI backend, a larger font system, centralized event dispatch, and full minimize/maximize/restore behavior remain future work. See [VexOS 2.0 engineering roadmap](VEXOS_2_0_ROADMAP.md).

## Prerequisites

- GNU toolchain: gcc, ld, objcopy, make
- mtools (mformat, mmd, mcopy) for writing the EFI image
- qemu-system-x86_64 and OVMF (UEFI firmware) for running the VM
- python3 (optional) used by `run.sh` to embed `new.png`, `new.jpeg`, or `new.jpg` into `dist/storage.img`

## Build

Run:

    make

The normal build produces the UEFI boot image, kernel, and storage image.

## Run (development)

From WSL, in the repository root:

    bash run.sh

The launcher builds and boots VexOS in QEMU with OVMF. It embeds the first available asset named `new.png`, `new.jpeg`, or `new.jpg` into the SimpleFS image while preserving the file's name and detecting its format from its contents. QEMU is configured with the legacy IDE controller expected by the current ATA PIO storage driver.

## DoomGeneric

DoomGeneric is kept in `src/doom` as a pinned upstream submodule. It is **not** built by the normal kernel Makefile. Running it still requires its platform backend, a separate user-mode ELF build, and a compatible IWAD in SimpleFS storage. The desktop's `doom` / `ring3` command looks for `doom.elf` and reports when it is missing. Do not use the old `BUILD_DOOMGENERIC=1` option; it was a no-op placeholder, not a working port.

## CI and testing

GitHub Actions builds the bootloader, kernel, and disk images, then boots the CI kernel under QEMU. The test workflow covers kernel self-tests, storage operations, USB keyboard and tablet input, Start-menu selection, Terminal commands, Notepad text entry, Files opening, and window resizing. Screenshots and serial logs are uploaded as workflow artifacts.

CI success demonstrates the configured QEMU path; it does not establish compatibility with every physical machine or USB device.

## Notes

Large binary artifacts (for example, `dist/tiny64.img` and `dist/storage.img`) may exceed GitHub's recommended repository size. Consider Git LFS or excluding generated build artifacts from version control.

## Contributing

Keep changes small, reviewable, and backed by a normal build plus relevant regression tests. Document new dependencies and explicitly list unsupported hardware or file formats.

## License

No license file is currently present. Add a `LICENSE` before redistributing the project as open-source software.
