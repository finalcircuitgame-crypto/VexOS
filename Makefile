# Tools
CC = gcc
LD = ld
OBJCOPY = objcopy

# Directories
SRCDIR = src
BOOTDIR = $(SRCDIR)/boot
KERNELDIR = $(SRCDIR)/kernel
DISTDIR = dist
OBJDIR = $(DISTDIR)/obj

# DoomGeneric is maintained as a submodule but is not part of the kernel build.
# A VexOS display/input backend and a separate user-mode ELF target are still required.

# Bootloader Flags
EFI_INC = /usr/include/efi
EFI_LIB = /usr/lib
CFLAGS_EFI = -fno-stack-protector -fpic -fshort-wchar -mno-red-zone -I$(EFI_INC) -I$(EFI_INC)/x86_64 -I$(SRCDIR)/include -DEFI_FUNCTION_WRAPPER -DGNU_EFI_USE_MS_ABI
LDFLAGS_EFI = -nostdlib -znocombreloc -T $(EFI_LIB)/elf_x86_64_efi.lds -shared -Bsymbolic -L$(EFI_LIB) $(EFI_LIB)/crt0-efi-x86_64.o

# Kernel Flags
CFLAGS_KERNEL = -ffreestanding -mno-red-zone -mcmodel=large -fno-pie -I$(SRCDIR)/include

# Optimize CPU-side desktop/rendering and memory paths. Keep hardware drivers
# at their existing optimization level until their MMIO/DMA paths are audited.
$(OBJDIR)/core/desktop.o $(OBJDIR)/core/png.o $(OBJDIR)/core/jpeg.o \
$(OBJDIR)/core/picojpeg.o $(OBJDIR)/core/simplefs.o \
$(OBJDIR)/mem/heap.o $(OBJDIR)/mem/pmm.o: CFLAGS_KERNEL += -O2

# CI-only boot/input instrumentation. Normal builds keep the real first-boot wizard.
ifeq ($(VEXOS_CI),1)
CFLAGS_KERNEL += -DVEXOS_CI_SKIP_WIZARD -DVEXOS_CI_INPUT_TEST
endif
LDFLAGS_KERNEL = -nostdlib -T $(KERNELDIR)/linker.ld -z max-page-size=0x1000

# Targets
BOOTLOADER_EFI = $(DISTDIR)/BOOTX64.EFI
KERNEL_ELF = $(DISTDIR)/kernel.elf
DISK_IMG = $(DISTDIR)/tiny64.img
STORAGE_IMG = $(DISTDIR)/storage.img

# Kernel Objects
# Recursively find all C and S files
KERNEL_SRCS := $(shell find $(KERNELDIR) -name "*.c")
KERNEL_ASMS := $(shell find $(KERNELDIR) -name "*.s")

# Map to object files in OBJDIR, maintaining directory structure
KERNEL_OBJS := $(patsubst $(KERNELDIR)/%.c, $(OBJDIR)/%.o, $(KERNEL_SRCS)) \
              $(patsubst $(KERNELDIR)/%.s, $(OBJDIR)/%_asm.o, $(KERNEL_ASMS))
FONT_OBJ = $(OBJDIR)/font.o

all: setup $(DISK_IMG) $(STORAGE_IMG)

setup:
	mkdir -p $(OBJDIR)

# Build Bootloader
$(BOOTLOADER_EFI): $(BOOTDIR)/main.c
	@mkdir -p $(dir $(OBJDIR)/boot_main.o)
	$(CC) $(CFLAGS_EFI) -c $< -o $(OBJDIR)/boot_main.o
	$(LD) $(LDFLAGS_EFI) $(OBJDIR)/boot_main.o -o $(OBJDIR)/boot_main.so -lgnuefi -lefi
	$(OBJCOPY) -j .text -j .sdata -j .data -j .dynamic -j .dynsym  -j .rel -j .rela -j .reloc --target=efi-app-x86_64 $(OBJDIR)/boot_main.so $@

# Embed Font
$(FONT_OBJ): CGA.F08
	@mkdir -p $(dir $@)
	$(OBJCOPY) -I binary -O elf64-x86-64 -B i386 $< $@

# Build Kernel
$(KERNEL_ELF): $(KERNEL_OBJS) $(FONT_OBJ)
	$(LD) $(LDFLAGS_KERNEL) $(KERNEL_OBJS) $(FONT_OBJ) -o $@

# Pattern rules for nested kernel directories
$(OBJDIR)/%.o: $(KERNELDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_KERNEL) -c $< -o $@

$(OBJDIR)/%_asm.o: $(KERNELDIR)/%.s
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_KERNEL) -c $< -o $@

# Create Disk Image
$(DISK_IMG): $(BOOTLOADER_EFI) $(KERNEL_ELF)
	dd if=/dev/zero of=$(DISK_IMG) bs=1M count=64
	mformat -i $(DISK_IMG) -F ::
	mmd -i $(DISK_IMG) ::/EFI
	mmd -i $(DISK_IMG) ::/EFI/BOOT
	mcopy -o -i $(DISK_IMG) $(BOOTLOADER_EFI) ::/EFI/BOOT/BOOTX64.EFI
	mcopy -o -i $(DISK_IMG) $(KERNEL_ELF) ::/kernel.elf
	@mkdir -p build
	@printf "fs0:\n\\EFI\\BOOT\\BOOTX64.EFI\n" > build/startup.nsh
	mcopy -o -i $(DISK_IMG) build/startup.nsh ::/startup.nsh

# Create separate persistent storage disk (raw) used by SimpleFS.
# This avoids overwriting the EFI/FAT boot image.
$(STORAGE_IMG):
	dd if=/dev/zero of=$(STORAGE_IMG) bs=1M count=64


# Build DoomGeneric as a freestanding ring-3 application. The WAD is supplied
# separately by the user and is never bundled into this repository.
DOOMDIR = $(SRCDIR)/doom/doomgeneric
DOOMOBJDIR = $(OBJDIR)/doom
DOOMDIST = $(DISTDIR)/doom.elf
DOOMCORE_NAMES = dummy am_map doomdef doomstat dstrings d_event d_items d_iwad d_loop d_main d_mode d_net f_finale f_wipe g_game hu_lib hu_stuff info i_cdmus i_endoom i_joystick i_scale i_sound i_system i_timer memio m_argv m_bbox m_cheat m_config m_controls m_fixed m_menu m_misc m_random p_ceilng p_doors p_enemy p_floor p_inter p_lights p_map p_maputl p_mobj p_plats p_pspr p_saveg p_setup p_sight p_spec p_switch p_telept p_tick p_user r_bsp r_data r_draw r_main r_plane r_segs r_sky r_things sha1 sounds statdump st_lib st_stuff s_sound tables v_video wi_stuff w_checksum w_file w_main w_wad z_zone w_file_stdc i_input i_video doomgeneric
DOOM_OBJS = $(addprefix $(DOOMOBJDIR)/,$(addsuffix .o,$(DOOMCORE_NAMES))) \
            $(DOOMOBJDIR)/doomgeneric_vexos.o $(DOOMOBJDIR)/libc.o $(DOOMOBJDIR)/doom_start.o $(DOOMOBJDIR)/doom_entry.o
DOOM_CFLAGS = -ffreestanding -fno-builtin -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -mno-red-zone -mcmodel=small -O2 -DNORMALUNIX -D_DEFAULT_SOURCE -DDOOMGENERIC_RESX=640 -DDOOMGENERIC_RESY=400 -I$(DOOMDIR) -I$(SRCDIR)/usermode/libc
DOOM_LDFLAGS = -nostdlib -static -no-pie -Wl,-T,$(SRCDIR)/usermode/doom/linker.ld -Wl,--build-id=none

.PHONY: doom
doom: $(DOOMDIST)

$(DOOMDIST): $(DOOM_OBJS) $(SRCDIR)/usermode/doom/linker.ld
	@mkdir -p $(DISTDIR)
	$(CC) $(DOOM_LDFLAGS) $(DOOM_OBJS) -lgcc -o $@
	@readelf -h $@ | grep -q 'EXEC'

$(DOOMOBJDIR)/%.o: $(DOOMDIR)/%.c
	@mkdir -p $(DOOMOBJDIR)
	$(CC) $(DOOM_CFLAGS) -c $< -o $@

$(DOOMOBJDIR)/doomgeneric_vexos.o: $(SRCDIR)/usermode/doom/doomgeneric_vexos.c
	@mkdir -p $(DOOMOBJDIR)
	$(CC) $(DOOM_CFLAGS) -c $< -o $@

$(DOOMOBJDIR)/libc.o: $(SRCDIR)/usermode/libc/libc.c
	@mkdir -p $(DOOMOBJDIR)
	$(CC) $(DOOM_CFLAGS) -c $< -o $@

$(DOOMOBJDIR)/doom_start.o: $(SRCDIR)/usermode/doom/doom_start.s
	@mkdir -p $(DOOMOBJDIR)
	$(CC) $(DOOM_CFLAGS) -c $< -o $@

$(DOOMOBJDIR)/doom_entry.o: $(SRCDIR)/usermode/doom/doom_entry.c
	@mkdir -p $(DOOMOBJDIR)
	$(CC) $(DOOM_CFLAGS) -c $< -o $@

clean:
	rm -rf $(DISTDIR)/*
# Create a bootable SimpleFS payload containing DoomGeneric and the user's WAD.
# Usage: make doom-storage WAD=/path/to/freedoom1.wad
.PHONY: doom-storage
doom-storage: doom $(STORAGE_IMG)
	@test -n "$(WAD)" || (echo "Usage: make doom-storage WAD=/path/to/doom1.wad" >&2; exit 2)
	python3 tools/pack_doom_storage.py --image $(STORAGE_IMG) --elf $(DOOMDIST) --wad "$(WAD)"
