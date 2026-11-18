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
VEXOS_VERSION ?= 2.0.0-dev
VEXOS_BUILD_ID := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)

CFLAGS_KERNEL = -ffreestanding -mno-red-zone -mcmodel=large -fno-pie -I$(SRCDIR)/include
CFLAGS_KERNEL += -MMD -MP

# Remote development logging is enabled in dev builds and omitted from releases.
ifneq ($(VEXOS_RELEASE),1)
CFLAGS_KERNEL += -DVEXOS_NETLOG
endif
CFLAGS_KERNEL += -DVEXOS_VERSION='"$(VEXOS_VERSION)"' -DVEXOS_BUILD_ID='"$(VEXOS_BUILD_ID)"'

# Refresh the embedded Git build ID on every make invocation.
.PHONY: FORCE_VEXOS_BUILD_ID
FORCE_VEXOS_BUILD_ID:
$(OBJDIR)/core/main.o: FORCE_VEXOS_BUILD_ID

# Optimize CPU-side desktop/rendering and memory paths. Keep hardware drivers
# at their existing optimization level until their MMIO/DMA paths are audited.
$(OBJDIR)/core/desktop.o $(OBJDIR)/core/png.o $(OBJDIR)/core/jpeg.o \
$(OBJDIR)/core/picojpeg.o $(OBJDIR)/core/simplefs.o \
$(OBJDIR)/mem/heap.o $(OBJDIR)/mem/pmm.o: CFLAGS_KERNEL += -O2

# Keep window rendering and compositor hot paths optimized.
$(OBJDIR)/desktop/%.o: CFLAGS_KERNEL += -O2
$(OBJDIR)/desktop/graphics/font_engine.o: CFLAGS_KERNEL += -O2

# CI-only boot/input instrumentation. Normal builds keep the real first-boot wizard.
ifeq ($(VEXOS_CI),1)
CFLAGS_KERNEL += -DVEXOS_CI_SKIP_WIZARD -DVEXOS_CI_INPUT_TEST
endif

# Only the dedicated DOOM playtest build runs the scripted desktop demo.
ifeq ($(VEXOS_CI_DOOM_DEMO),1)
CFLAGS_KERNEL += -DVEXOS_CI_DOOM_DEMO
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
ifeq ($(VEXOS_RELEASE),1)
KERNEL_SRCS := $(filter-out $(KERNELDIR)/net/netlog.c,$(KERNEL_SRCS))
endif
KERNEL_ASMS := $(shell find $(KERNELDIR) -name "*.s")

# Map to object files in OBJDIR, maintaining directory structure
KERNEL_OBJS := $(patsubst $(KERNELDIR)/%.c, $(OBJDIR)/%.o, $(KERNEL_SRCS)) \
              $(patsubst $(KERNELDIR)/%.s, $(OBJDIR)/%_asm.o, $(KERNEL_ASMS))
KERNEL_DEPS := $(KERNEL_OBJS:.o=.d)
-include $(KERNEL_DEPS)

# The Makefile controls compiler flags and dependency generation. Rebuild the
# existing objects once so every kernel translation unit gains a .d file.
$(KERNEL_OBJS): Makefile

FONT_OBJ = $(OBJDIR)/font.o

# Embed the original TrueType faces. Prefer project assets; CI may use the
# equivalent distro-provided font files under /usr/share/fonts.
FONT_SEARCH_ROOTS ?= assets/fonts /usr/share/fonts
font_find = $(shell find $(FONT_SEARCH_ROOTS) \( -type f -o -type l \) \( $(foreach name,$(1),-iname '$(name)' -o ) -false \) -print -quit 2>/dev/null)

FONT_SOURCE_INTER_REGULAR := $(call font_find,Inter-Regular.ttf Inter-Regular.otf Inter_18pt-Regular.ttf)
FONT_SOURCE_INTER_BOLD := $(call font_find,Inter-Bold.ttf Inter-Bold.otf Inter_18pt-Bold.ttf)
FONT_SOURCE_LATO_REGULAR := $(call font_find,Lato-Regular.ttf Lato-Regular.otf)
FONT_SOURCE_LATO_BOLD := $(call font_find,Lato-Bold.ttf Lato-Bold.otf)
FONT_SOURCE_DEJAVU_SANS_REGULAR := $(call font_find,DejaVuSans.ttf)
FONT_SOURCE_DEJAVU_SANS_BOLD := $(call font_find,DejaVuSans-Bold.ttf)
FONT_SOURCE_DEJAVU_MONO_REGULAR := $(call font_find,DejaVuSansMono.ttf)
FONT_SOURCE_DEJAVU_MONO_BOLD := $(call font_find,DejaVuSansMono-Bold.ttf)
FONT_SOURCE_NOTO_SANS_REGULAR := $(call font_find,NotoSans-Regular.ttf)
FONT_SOURCE_NOTO_SANS_BOLD := $(call font_find,NotoSans-Bold.ttf)
FONT_SOURCE_NOTO_SERIF_REGULAR := $(call font_find,NotoSerif-Regular.ttf)
FONT_SOURCE_NOTO_SERIF_BOLD := $(call font_find,NotoSerif-Bold.ttf)

FONT_VARIANTS = inter_regular inter_bold lato_regular lato_bold dejavu_sans_regular dejavu_sans_bold dejavu_mono_regular dejavu_mono_bold noto_sans_regular noto_sans_bold noto_serif_regular noto_serif_bold
FONT_OBJS = $(addprefix $(OBJDIR)/font_,$(addsuffix .o,$(FONT_VARIANTS)))

DESKTOP_ICON_SOURCES = $(addprefix assets/icons/,file-manager.png terminal.png settings.png notepad.png image-viewer.png doom.png)
DESKTOP_ICON_BIN = $(OBJDIR)/desktop_icons.bin
DESKTOP_ICON_OBJ = $(OBJDIR)/desktop_icons.o

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

# Convert the desktop PNGs to tightly packed RGBA and embed them in the kernel.
$(DESKTOP_ICON_BIN): tools/prepare_desktop_icons.py $(DESKTOP_ICON_SOURCES)
	@mkdir -p $(dir $@)
	python3 $< --output $@ $(DESKTOP_ICON_SOURCES)

$(DESKTOP_ICON_OBJ): $(DESKTOP_ICON_BIN)
	cd $(OBJDIR) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386 --rename-section .data=.rodata,alloc,load,readonly,data,contents desktop_icons.bin desktop_icons.o

# Each TTF is embedded as a read-only binary object. The stb_truetype
# runtime rasterizer consumes these bytes directly; no Pillow/atlas step exists.

ifneq ($(strip $(FONT_SOURCE_INTER_REGULAR)),)
$(OBJDIR)/font_inter_regular.o: $(FONT_SOURCE_INTER_REGULAR)
	@mkdir -p $(OBJDIR)
	@echo "[FONT] Embedding Inter Regular: $<"
	cp "$<" "$(OBJDIR)/font_inter_regular.ttf"
	cd $(OBJDIR) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386 --rename-section .data=.font_assets,alloc,load,readonly,data,contents font_inter_regular.ttf font_inter_regular.o
else
$(OBJDIR)/font_inter_regular.o:
	@echo "Missing required font face: Inter Regular" >&2
	@echo "Place Inter-Regular.ttf or Inter-Regular.otf under assets/fonts/." >&2
	@false
endif

ifneq ($(strip $(FONT_SOURCE_INTER_BOLD)),)
$(OBJDIR)/font_inter_bold.o: $(FONT_SOURCE_INTER_BOLD)
	@mkdir -p $(OBJDIR)
	@echo "[FONT] Embedding Inter Bold: $<"
	cp "$<" "$(OBJDIR)/font_inter_bold.ttf"
	cd $(OBJDIR) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386 --rename-section .data=.font_assets,alloc,load,readonly,data,contents font_inter_bold.ttf font_inter_bold.o
else
$(OBJDIR)/font_inter_bold.o:
	@echo "Missing required font face: Inter Bold" >&2
	@echo "Place Inter-Bold.ttf or Inter-Bold.otf under assets/fonts/." >&2
	@false
endif

ifneq ($(strip $(FONT_SOURCE_LATO_REGULAR)),)
$(OBJDIR)/font_lato_regular.o: $(FONT_SOURCE_LATO_REGULAR)
	@mkdir -p $(OBJDIR)
	@echo "[FONT] Embedding Lato Regular: $<"
	cp "$<" "$(OBJDIR)/font_lato_regular.ttf"
	cd $(OBJDIR) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386 --rename-section .data=.font_assets,alloc,load,readonly,data,contents font_lato_regular.ttf font_lato_regular.o
else
$(OBJDIR)/font_lato_regular.o:
	@echo "Missing required font face: Lato Regular" >&2
	@echo "Place Lato-Regular.ttf or Lato-Regular.otf under assets/fonts/." >&2
	@false
endif

ifneq ($(strip $(FONT_SOURCE_LATO_BOLD)),)
$(OBJDIR)/font_lato_bold.o: $(FONT_SOURCE_LATO_BOLD)
	@mkdir -p $(OBJDIR)
	@echo "[FONT] Embedding Lato Bold: $<"
	cp "$<" "$(OBJDIR)/font_lato_bold.ttf"
	cd $(OBJDIR) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386 --rename-section .data=.font_assets,alloc,load,readonly,data,contents font_lato_bold.ttf font_lato_bold.o
else
$(OBJDIR)/font_lato_bold.o:
	@echo "Missing required font face: Lato Bold" >&2
	@echo "Place Lato-Bold.ttf or Lato-Bold.otf under assets/fonts/." >&2
	@false
endif

ifneq ($(strip $(FONT_SOURCE_DEJAVU_SANS_REGULAR)),)
$(OBJDIR)/font_dejavu_sans_regular.o: $(FONT_SOURCE_DEJAVU_SANS_REGULAR)
	@mkdir -p $(OBJDIR)
	@echo "[FONT] Embedding DejaVu Sans Regular: $<"
	cp "$<" "$(OBJDIR)/font_dejavu_sans_regular.ttf"
	cd $(OBJDIR) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386 --rename-section .data=.font_assets,alloc,load,readonly,data,contents font_dejavu_sans_regular.ttf font_dejavu_sans_regular.o
else
$(OBJDIR)/font_dejavu_sans_regular.o:
	@echo "Missing required font face: DejaVu Sans Regular" >&2
	@echo "Place DejaVuSans.ttf under assets/fonts/." >&2
	@false
endif

ifneq ($(strip $(FONT_SOURCE_DEJAVU_SANS_BOLD)),)
$(OBJDIR)/font_dejavu_sans_bold.o: $(FONT_SOURCE_DEJAVU_SANS_BOLD)
	@mkdir -p $(OBJDIR)
	@echo "[FONT] Embedding DejaVu Sans Bold: $<"
	cp "$<" "$(OBJDIR)/font_dejavu_sans_bold.ttf"
	cd $(OBJDIR) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386 --rename-section .data=.font_assets,alloc,load,readonly,data,contents font_dejavu_sans_bold.ttf font_dejavu_sans_bold.o
else
$(OBJDIR)/font_dejavu_sans_bold.o:
	@echo "Missing required font face: DejaVu Sans Bold" >&2
	@echo "Place DejaVuSans-Bold.ttf under assets/fonts/." >&2
	@false
endif

ifneq ($(strip $(FONT_SOURCE_DEJAVU_MONO_REGULAR)),)
$(OBJDIR)/font_dejavu_mono_regular.o: $(FONT_SOURCE_DEJAVU_MONO_REGULAR)
	@mkdir -p $(OBJDIR)
	@echo "[FONT] Embedding DejaVu Sans Mono Regular: $<"
	cp "$<" "$(OBJDIR)/font_dejavu_mono_regular.ttf"
	cd $(OBJDIR) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386 --rename-section .data=.font_assets,alloc,load,readonly,data,contents font_dejavu_mono_regular.ttf font_dejavu_mono_regular.o
else
$(OBJDIR)/font_dejavu_mono_regular.o:
	@echo "Missing required font face: DejaVu Sans Mono Regular" >&2
	@echo "Place DejaVuSansMono.ttf under assets/fonts/." >&2
	@false
endif

ifneq ($(strip $(FONT_SOURCE_DEJAVU_MONO_BOLD)),)
$(OBJDIR)/font_dejavu_mono_bold.o: $(FONT_SOURCE_DEJAVU_MONO_BOLD)
	@mkdir -p $(OBJDIR)
	@echo "[FONT] Embedding DejaVu Sans Mono Bold: $<"
	cp "$<" "$(OBJDIR)/font_dejavu_mono_bold.ttf"
	cd $(OBJDIR) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386 --rename-section .data=.font_assets,alloc,load,readonly,data,contents font_dejavu_mono_bold.ttf font_dejavu_mono_bold.o
else
$(OBJDIR)/font_dejavu_mono_bold.o:
	@echo "Missing required font face: DejaVu Sans Mono Bold" >&2
	@echo "Place DejaVuSansMono-Bold.ttf under assets/fonts/." >&2
	@false
endif


ifneq ($(strip $(FONT_SOURCE_NOTO_SANS_REGULAR)),)
$(OBJDIR)/font_noto_sans_regular.o: $(FONT_SOURCE_NOTO_SANS_REGULAR)
	@mkdir -p $(OBJDIR)
	@echo "[FONT] Embedding Noto Sans Regular: $<"
	cp "$<" "$(OBJDIR)/font_noto_sans_regular.ttf"
	cd $(OBJDIR) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386 --rename-section .data=.font_assets,alloc,load,readonly,data,contents font_noto_sans_regular.ttf font_noto_sans_regular.o
else
$(OBJDIR)/font_noto_sans_regular.o:
	@echo "Missing required font face: Noto Sans Regular" >&2
	@echo "Install fonts-noto-core or place NotoSans-Regular.ttf under assets/fonts/." >&2
	@false
endif

ifneq ($(strip $(FONT_SOURCE_NOTO_SANS_BOLD)),)
$(OBJDIR)/font_noto_sans_bold.o: $(FONT_SOURCE_NOTO_SANS_BOLD)
	@mkdir -p $(OBJDIR)
	@echo "[FONT] Embedding Noto Sans Bold: $<"
	cp "$<" "$(OBJDIR)/font_noto_sans_bold.ttf"
	cd $(OBJDIR) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386 --rename-section .data=.font_assets,alloc,load,readonly,data,contents font_noto_sans_bold.ttf font_noto_sans_bold.o
else
$(OBJDIR)/font_noto_sans_bold.o:
	@echo "Missing required font face: Noto Sans Bold" >&2
	@echo "Install fonts-noto-core or place NotoSans-Bold.ttf under assets/fonts/." >&2
	@false
endif

ifneq ($(strip $(FONT_SOURCE_NOTO_SERIF_REGULAR)),)
$(OBJDIR)/font_noto_serif_regular.o: $(FONT_SOURCE_NOTO_SERIF_REGULAR)
	@mkdir -p $(OBJDIR)
	@echo "[FONT] Embedding Noto Serif Regular: $<"
	cp "$<" "$(OBJDIR)/font_noto_serif_regular.ttf"
	cd $(OBJDIR) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386 --rename-section .data=.font_assets,alloc,load,readonly,data,contents font_noto_serif_regular.ttf font_noto_serif_regular.o
else
$(OBJDIR)/font_noto_serif_regular.o:
	@echo "Missing required font face: Noto Serif Regular" >&2
	@echo "Install fonts-noto-core or place NotoSerif-Regular.ttf under assets/fonts/." >&2
	@false
endif

ifneq ($(strip $(FONT_SOURCE_NOTO_SERIF_BOLD)),)
$(OBJDIR)/font_noto_serif_bold.o: $(FONT_SOURCE_NOTO_SERIF_BOLD)
	@mkdir -p $(OBJDIR)
	@echo "[FONT] Embedding Noto Serif Bold: $<"
	cp "$<" "$(OBJDIR)/font_noto_serif_bold.ttf"
	cd $(OBJDIR) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386 --rename-section .data=.font_assets,alloc,load,readonly,data,contents font_noto_serif_bold.ttf font_noto_serif_bold.o
else
$(OBJDIR)/font_noto_serif_bold.o:
	@echo "Missing required font face: Noto Serif Bold" >&2
	@echo "Install fonts-noto-core or place NotoSerif-Bold.ttf under assets/fonts/." >&2
	@false
endif

# Build Kernel
$(KERNEL_ELF): $(KERNEL_OBJS) $(FONT_OBJ) $(FONT_OBJS) $(DESKTOP_ICON_OBJ)
	$(LD) $(LDFLAGS_KERNEL) $(KERNEL_OBJS) $(FONT_OBJ) $(FONT_OBJS) $(DESKTOP_ICON_OBJ) -o $@

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
DOOMCORE_NAMES = dummy am_map doomdef doomstat dstrings d_event d_items d_iwad d_loop d_main d_mode d_net f_finale f_wipe g_game hu_lib hu_stuff info i_cdmus i_endoom i_joystick i_scale i_sound i_system i_timer memio m_argv m_bbox m_cheat m_config m_controls m_fixed m_menu m_misc m_random p_ceilng p_doors p_enemy p_floor p_inter p_lights p_map p_maputl p_mobj p_plats p_pspr p_saveg p_setup p_sight p_spec p_switch p_telept p_tick p_user r_bsp r_data r_draw r_main r_plane r_segs r_sky r_things sha1 sounds statdump st_lib st_stuff s_sound tables v_video wi_stuff w_checksum w_file w_main z_zone w_file_stdc i_input i_video doomgeneric
DOOM_OBJS = $(addprefix $(DOOMOBJDIR)/,$(addsuffix .o,$(DOOMCORE_NAMES))) \
            $(DOOMOBJDIR)/w_wad.o \
            $(DOOMOBJDIR)/doomgeneric_vexos.o $(DOOMOBJDIR)/libc.o $(DOOMOBJDIR)/doom_start.o $(DOOMOBJDIR)/doom_entry.o

# Makefile changes alter Doom compile flags; rebuild stale objects (e.g. 640x400 -> 320x200).
$(DOOM_OBJS): Makefile
# Doom runs at 128 GiB, leaving the 256 GiB user heap/mmap region free, outside the kernel's low identity map.
DOOM_CFLAGS = -ffreestanding -fno-builtin -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -mno-red-zone -mcmodel=large -O2 -DNORMALUNIX -D_DEFAULT_SOURCE -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -I$(DOOMDIR) -I$(SRCDIR)/usermode/libc
ifeq ($(VEXOS_CI),1)
DOOM_CFLAGS += -DVEXOS_CI_INPUT_TEST
endif
DOOM_LDFLAGS = -nostdlib -static -no-pie -Wl,-T,$(SRCDIR)/usermode/doom/linker.ld -Wl,--build-id=none

.PHONY: doom
doom: $(DOOMDIST)

$(DOOMDIST): $(DOOM_OBJS) $(SRCDIR)/usermode/doom/linker.ld
	@mkdir -p $(DISTDIR)
	$(CC) $(DOOM_LDFLAGS) $(DOOM_OBJS) -lgcc -o $@
	@readelf -h $@ | grep -q 'EXEC'


# Patch upstream DoomGeneric input in a generated build copy. Do not modify the submodule.
$(DOOMOBJDIR)/i_input.o: $(DOOMDIR)/i_input.c tools/patch_doom_input.py Makefile
	@mkdir -p $(DOOMOBJDIR)
	python3 tools/patch_doom_input.py --source $< --output $(DOOMOBJDIR)/i_input_vexos.c
	$(CC) $(DOOM_CFLAGS) -c $(DOOMOBJDIR)/i_input_vexos.c -o $@

# Reconcile live USB arrow state immediately before Doom builds movement commands.
$(DOOMOBJDIR)/g_game.o: $(DOOMDIR)/g_game.c tools/patch_doom_game_input.py Makefile
	@mkdir -p $(DOOMOBJDIR)
	python3 tools/patch_doom_game_input.py --source $< --output $(DOOMOBJDIR)/g_game_vexos.c
	$(CC) $(DOOM_CFLAGS) -c $(DOOMOBJDIR)/g_game_vexos.c -o $@

$(DOOMOBJDIR)/%.o: $(DOOMDIR)/%.c
	@mkdir -p $(DOOMOBJDIR)
	$(CC) $(DOOM_CFLAGS) -c $< -o $@

# Instrument the WAD directory loader without modifying the upstream submodule.
# FORCE ensures a stale pre-instrumentation object can never be linked silently.
.PHONY: FORCE_DOOM_WAD_DIAG
FORCE_DOOM_WAD_DIAG:
$(DOOMOBJDIR)/w_wad.o: FORCE_DOOM_WAD_DIAG $(DOOMDIR)/w_wad.c tools/instrument_doom_wad.py
	@mkdir -p $(DOOMOBJDIR)
	python3 tools/instrument_doom_wad.py --source $(DOOMDIR)/w_wad.c --output $(DOOMOBJDIR)/w_wad_instrumented.c
	@grep -q 'VEXOS_WAD_DIAG_ASSIGN' $(DOOMOBJDIR)/w_wad_instrumented.c || (echo "[WADDBG] missing ASSIGN instrumentation" >&2; exit 1)
	@grep -q 'VEXOS_WAD_DIAG_AFTER_FREE' $(DOOMOBJDIR)/w_wad_instrumented.c || (echo "[WADDBG] missing AFTER_FREE instrumentation" >&2; exit 1)
	@grep -q 'VEXOS_WAD_DIAG_READ' $(DOOMOBJDIR)/w_wad_instrumented.c || (echo "[WADDBG] missing READ instrumentation" >&2; exit 1)
	@echo "[WADDBG] compiling instrumented w_wad.c"
	$(CC) $(DOOM_CFLAGS) -c $(DOOMOBJDIR)/w_wad_instrumented.c -o $@

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
# Usage: make doom-storage WAD=/path/to/DOOM.WAD (or DOOM1.WAD / another IWAD)
.PHONY: doom-storage
doom-storage: doom $(STORAGE_IMG)
	@test -n "$(WAD)" || (echo "Usage: make doom-storage WAD=/path/to/DOOM.WAD" >&2; exit 2)
	python3 tools/pack_doom_storage.py --image $(STORAGE_IMG) --elf $(DOOMDIST) --wad "$(WAD)"
