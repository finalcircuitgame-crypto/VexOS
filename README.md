# VexOS

VexOS is a small educational x86_64 operating system project that boots via an EFI bootloader and runs a minimal kernel with a desktop environment. It includes basic kernel subsystems (PMM, VMM, heap, tasking), PCI/xHCI support, persistent storage, and a fullscreen setup wizard.

## Key features
- EFI bootloader (src/boot) that loads kernel.elf and passes a BootInfo struct
- Kernel core (src/kernel/core): framebuffer console, PMM, VMM, heap, PCI enumeration and drivers, desktop UI, and setup wizard
- Persistent SimpleFS storage with SFS1 compatibility and journaled SFS2 nested-file operations
- Boot diagnostics: serial output is buffered in a 32 KiB RAM ring and flushed to `/boot.log` through the writable SFS1/SFS2 VFS backend; logging never formats a disk
- Networking foundation: generic network-device registration plus a polling Intel E1000 driver with bounded RX/TX descriptor rings (QEMU 82540EM target)

## Development roadmap

See [`docs/ROADMAP.md`](docs/ROADMAP.md) for the full plan. The VFS foundation supports hierarchical paths, open-node lifetime tracking, and create/unlink operations on SFS1. Normal storage mounting is non-destructive: unknown or damaged volumes are rejected rather than automatically formatted.

SFS2 has checksummed metadata, VFS path/stat/directory/ranged-read support, and an optional 32-record redo journal. Journal-enabled images support transactional nested-file create/replace/unlink, automatically creating missing parent directories, with files up to 16 MiB. Journal replay runs before metadata validation, and CI injects failures at every replacement-transaction write boundary. Legacy SFS2 images without a journal remain read-only. The offline host utility in [`tools/sfs2_image.py`](tools/sfs2_image.py) formats journal-enabled images, verifies metadata, and migrates supported SFS1 images into separate SFS2 images. It refuses to overwrite existing output files unless `--force` is explicitly supplied.

**Limitations:** each file uses one contiguous extent and is limited to 16 MiB. Transaction concurrency hardening and broad physical-device qualification remain outstanding. SFS1 remains supported. See [`docs/SFS2_FORMAT.md`](docs/SFS2_FORMAT.md) and [`docs/VFS_DESIGN.md`](docs/VFS_DESIGN.md).

## Prerequisites
- GNU toolchain: gcc, ld, objcopy, make
- mtools (mformat, mmd, mcopy) for writing the EFI image
- qemu-system-x86_64 and OVMF (UEFI firmware) for running the VM
- python3 for host-side image utilities and tests

## Build

The kernel uses **stb_truetype** to parse and rasterize the original TTF faces
at runtime. No Pillow package, generated bitmap atlas, or host-side font
preprocessing step is required.

Provide the twelve static regular/bold TTF files listed in
[`assets/fonts/README.md`](assets/fonts/README.md). The Makefile searches
`assets/fonts/` recursively and also checks `/usr/share/fonts/` (useful for
Ubuntu/WSL and CI). On Ubuntu/WSL, the matching distro fonts can be installed
with:

```bash
sudo apt install fonts-inter fonts-lato fonts-dejavu-core fonts-noto-core
```

Then build normally:

```bash
make
```

The normal build produces `dist/BOOTX64.EFI`, `dist/kernel.elf`, the bootable FAT image `dist/tiny64.img`, and the separate persistent data image `dist/storage.img`.

## Prepare a bootable USB drive (Windows)

After building VexOS, launch the Windows flasher from the repository root:

```powershell
py .\tiny64_usb_flasher.py
```

The flasher provides two operations:

- **Quick Flash (no admin)** copies the UEFI bootloader, `kernel.elf`, and the generated `startup.nsh` when present to an existing FAT32 USB volume. It does not format the drive and preserves unrelated files.
- **Full Flash (admin)** creates a GPT partition table and FAT32 `VEXOS` volume, then copies the boot files. This erases every partition and file on the selected physical USB disk. Only this operation requires running elevated; verify the model and capacity, and complete both confirmation prompts.

The utility only lists physical USB disks for whole-disk flashing, rechecks the disk identity immediately before repartitioning, refuses Windows boot/system disks, and verifies copied files with SHA-256. It does not install VexOS to internal drives. The separate `dist/storage.img` is not written by this boot-media utility; persistent storage over USB mass storage is not part of this USB boot workflow yet.

Boot-log persistence uses the kernel's 32 KiB RAM ring and the writable SFS1/SFS2 `/boot.log` VFS backend. If storage is not available, diagnostics remain buffered in RAM; logging does not format or partition media. VexOS's first networking milestone is currently limited to raw Ethernet frames through the QEMU Intel E1000 device. ARP, IPv4, ICMP, DHCP, DNS, UDP, TCP, sockets, and physical laptop NIC qualification are not implemented yet.

A successful flash creates this boot layout:

```text
USB:/
├── EFI/
│   └── BOOT/
│       └── BOOTX64.EFI
├── kernel.elf
└── startup.nsh
```

DoomGeneric is kept in `src/doom` as a pinned upstream submodule. It is a separate user-mode ELF with a VexOS platform backend. To build the app and prepare storage with your own IWAD:

```sh
make doom
make doom-storage WAD=/path/to/DOOM.WAD
```

The packer validates the IWAD and stores it as `doom.wad` alongside `doom.elf`. The WAD stays outside the repository. The CI playtest uses Freedoom as its redistributable test IWAD.

## Run (development)

    ./run.sh

The script builds and launches QEMU with OVMF. It may embed `new.png` into the SimpleFS storage image if that file exists.

## Current status and limitations

- GitHub Actions builds the normal kernel and CI-instrumented kernel, then boots VexOS in QEMU and exercises desktop input and applications.
- The first-boot screen is a non-destructive welcome guide. It does not partition, format, or install to any disk.
- AHCI support targets SATA controllers. NVMe support is not implemented, and AHCI has not been qualified on every physical laptop.
- The current boot image is a development image, not a Windows replacement or dual-boot installer. Do not use it to repartition or overwrite an internal drive.
- A successful QEMU test does not guarantee compatibility with every laptop's firmware, graphics hardware, storage controller, keyboard, or touchpad.

## Notes & warnings

Large binary artifacts (dist/tiny64.img, dist/storage.img) exceed GitHub recommended sizes; consider using git-lfs or excluding build artifacts from the repository.

## Contributing

Please open issues or PRs. Keep changes small and document new build dependencies.

## License

No license file detected. Add a LICENSE if you want to make the project open-source.
