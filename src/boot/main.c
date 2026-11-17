#include <efi.h>
#include <efilib.h>
#include <elf.h>
#include "bootinfo.h"
#include "uefi_bootlog.h"

#ifndef EFI_MEMORY_RUNTIME
#define EFI_MEMORY_RUNTIME 0x8000000000000000ull
#endif

#define ELF_MAX_LOAD_SEGMENTS 128u
#define ELF_PF_X 1u
#define ELF_PT_LOAD 1u

typedef struct {
    EFI_PHYSICAL_ADDRESS base;
    UINTN pages;
} LoadedSegment;

static EFI_STATUS LoadKernelFile(EFI_HANDLE image_handle,
                                 EFI_SYSTEM_TABLE *system_table,
                                 void **out_buffer, UINTN *out_size) {
    EFI_LOADED_IMAGE_PROTOCOL *loaded_image = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *filesystem = NULL;
    EFI_FILE *root = NULL;
    EFI_FILE *file = NULL;
    EFI_FILE_INFO *file_info = NULL;
    UINTN info_size = 0;
    void *buffer = NULL;
    EFI_STATUS status;

    if (!out_buffer || !out_size) return EFI_INVALID_PARAMETER;
    *out_buffer = NULL;
    *out_size = 0;

    status = system_table->BootServices->HandleProtocol(
        image_handle, &gEfiLoadedImageProtocolGuid, (void **)&loaded_image);
    if (EFI_ERROR(status)) return status;
    if (!loaded_image) return EFI_NOT_FOUND;

    status = system_table->BootServices->HandleProtocol(
        loaded_image->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid,
        (void **)&filesystem);
    if (EFI_ERROR(status)) return status;
    if (!filesystem) return EFI_UNSUPPORTED;

    status = filesystem->OpenVolume(filesystem, &root);
    if (EFI_ERROR(status)) return status;
    if (!root) return EFI_NOT_FOUND;

    status = root->Open(root, &file, L"kernel.elf",
                        EFI_FILE_MODE_READ, 0);
    root->Close(root);
    if (EFI_ERROR(status)) return status;
    if (!file) return EFI_NOT_FOUND;

    status = file->GetInfo(file, &gEfiFileInfoGuid, &info_size, NULL);
    if (status != EFI_BUFFER_TOO_SMALL || info_size == 0) {
        file->Close(file);
        return EFI_LOAD_ERROR;
    }

    status = system_table->BootServices->AllocatePool(
        EfiLoaderData, info_size, (void **)&file_info);
    if (EFI_ERROR(status)) {
        file->Close(file);
        return status;
    }

    status = file->GetInfo(file, &gEfiFileInfoGuid, &info_size, file_info);
    if (EFI_ERROR(status) || file_info->FileSize == 0 ||
        file_info->FileSize > (UINT64)(~(UINTN)0)) {
        system_table->BootServices->FreePool(file_info);
        file->Close(file);
        return EFI_LOAD_ERROR;
    }

    UINTN file_size = (UINTN)file_info->FileSize;
    status = system_table->BootServices->AllocatePool(
        EfiLoaderData, file_size, &buffer);
    system_table->BootServices->FreePool(file_info);
    if (EFI_ERROR(status)) {
        file->Close(file);
        return status;
    }

    UINTN bytes_read = file_size;
    status = file->Read(file, &bytes_read, buffer);
    file->Close(file);
    if (EFI_ERROR(status) || bytes_read != file_size) {
        system_table->BootServices->FreePool(buffer);
        return EFI_LOAD_ERROR;
    }

    *out_buffer = buffer;
    *out_size = file_size;
    return EFI_SUCCESS;
}


/*
 * Export the previous kernel's UEFI-variable checkpoint to the FAT boot
 * volume. This runs before ExitBootServices(), so UEFI file protocols are
 * still available even when the kernel's USB controller driver is broken.
 */
static EFI_STATUS ExportPreviousBootLog(EFI_HANDLE image_handle,
                                         EFI_SYSTEM_TABLE *system_table) {
    if (!system_table || !system_table->RuntimeServices ||
        !system_table->RuntimeServices->GetVariable ||
        !system_table->RuntimeServices->SetVariable)
        return EFI_UNSUPPORTED;

    UINT8 record[sizeof(VexOSBootLogHeader) +
                 VEXOS_BOOTLOG_MAX_PAYLOAD] __attribute__((aligned(8)));
    UINTN record_size = sizeof(record);
    UINT32 attributes = 0;
    EFI_STATUS status = system_table->RuntimeServices->GetVariable(
        (CHAR16 *)VEXOS_BOOTLOG_VARIABLE_NAME,
        (EFI_GUID *)&VEXOS_BOOTLOG_VENDOR_GUID,
        &attributes, &record_size, record);
    if (status == EFI_NOT_FOUND)
        return status;
    if (EFI_ERROR(status))
        return status;

    if (record_size < sizeof(VexOSBootLogHeader) ||
        record_size > sizeof(record))
        return EFI_LOAD_ERROR;

    VexOSBootLogHeader header;
    CopyMem(&header, record, sizeof(header));
    if (header.magic != VEXOS_BOOTLOG_MAGIC ||
        header.version != VEXOS_BOOTLOG_VERSION ||
        header.header_size != sizeof(VexOSBootLogHeader) ||
        header.payload_size > VEXOS_BOOTLOG_MAX_PAYLOAD ||
        record_size != sizeof(VexOSBootLogHeader) + header.payload_size ||
        vexos_bootlog_checksum(record + sizeof(VexOSBootLogHeader),
                               header.payload_size) != header.checksum) {
        Print(L"[VexOS] Previous UEFI boot log was invalid; leaving it untouched\n");
        return EFI_LOAD_ERROR;
    }

    EFI_LOADED_IMAGE_PROTOCOL *loaded_image = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *filesystem = NULL;
    EFI_FILE *root = NULL;
    EFI_FILE *file = NULL;
    EFI_FILE_INFO *file_info = NULL;
    UINTN info_size = 0;

    status = system_table->BootServices->HandleProtocol(
        image_handle, &gEfiLoadedImageProtocolGuid, (void **)&loaded_image);
    if (EFI_ERROR(status) || !loaded_image)
        return EFI_ERROR(status) ? status : EFI_NOT_FOUND;

    status = system_table->BootServices->HandleProtocol(
        loaded_image->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid,
        (void **)&filesystem);
    if (EFI_ERROR(status) || !filesystem)
        return EFI_ERROR(status) ? status : EFI_UNSUPPORTED;

    status = filesystem->OpenVolume(filesystem, &root);
    if (EFI_ERROR(status) || !root)
        return EFI_ERROR(status) ? status : EFI_NOT_FOUND;

    status = root->Open(root, &file, L"boot.log",
                        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
                            EFI_FILE_MODE_CREATE,
                        0);
    if (EFI_ERROR(status) || !file) {
        root->Close(root);
        return EFI_ERROR(status) ? status : EFI_NOT_FOUND;
    }

    status = file->SetPosition(file, 0);
    if (!EFI_ERROR(status)) {
        UINTN bytes_to_write = header.payload_size;
        status = file->Write(file, &bytes_to_write,
                             record + sizeof(VexOSBootLogHeader));
        if (!EFI_ERROR(status) && bytes_to_write != header.payload_size)
            status = EFI_DEVICE_ERROR;
    }

    if (!EFI_ERROR(status)) {
        status = file->GetInfo(file, &gEfiFileInfoGuid, &info_size, NULL);
        if (status == EFI_BUFFER_TOO_SMALL && info_size != 0) {
            status = system_table->BootServices->AllocatePool(
                EfiLoaderData, info_size, (void **)&file_info);
            if (!EFI_ERROR(status)) {
                status = file->GetInfo(file, &gEfiFileInfoGuid,
                                       &info_size, file_info);
                if (!EFI_ERROR(status)) {
                    file_info->FileSize = header.payload_size;
                    status = file->SetInfo(file, &gEfiFileInfoGuid,
                                           info_size, file_info);
                }
                system_table->BootServices->FreePool(file_info);
                file_info = NULL;
            }
        } else if (!EFI_ERROR(status)) {
            status = EFI_DEVICE_ERROR;
        }
    }

    file->Close(file);
    root->Close(root);
    if (EFI_ERROR(status))
        return status;

    Print(L"[VexOS] Exported previous boot checkpoint to boot.log (%u bytes)\n",
          header.payload_size);

    /* Clear only after the complete file write and truncate have succeeded. */
    status = system_table->RuntimeServices->SetVariable(
        (CHAR16 *)VEXOS_BOOTLOG_VARIABLE_NAME,
        (EFI_GUID *)&VEXOS_BOOTLOG_VENDOR_GUID,
        0, 0, NULL);
    if (EFI_ERROR(status))
        Print(L"[VexOS] Warning: exported log, but could not clear NVRAM copy (%r)\n",
              status);

    return EFI_SUCCESS;
}

static int CaptureRuntimeRegions(EFI_SYSTEM_TABLE *system_table,
                                 EFI_MEMORY_DESCRIPTOR *map,
                                 UINTN map_size, UINTN descriptor_size,
                                 BootInfo *boot_info) {
    if (!system_table || !system_table->RuntimeServices ||
        !system_table->RuntimeServices->SetVariable || !map || !boot_info ||
        descriptor_size < sizeof(EFI_MEMORY_DESCRIPTOR))
        return 0;

    UINT64 set_variable = (UINT64)(UINTN)
        system_table->RuntimeServices->SetVariable;
    UINT32 count = 0;
    int set_variable_is_runtime = 0;

    for (UINTN offset = 0; offset + descriptor_size <= map_size;
         offset += descriptor_size) {
        EFI_MEMORY_DESCRIPTOR *desc =
            (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)map + offset);
        if (!(desc->Attribute & EFI_MEMORY_RUNTIME) ||
            desc->NumberOfPages == 0)
            continue;

        if (desc->NumberOfPages > (~(UINT64)0) / EFI_PAGE_SIZE)
            return 0;
        UINT64 bytes = desc->NumberOfPages * EFI_PAGE_SIZE;
        if (desc->PhysicalStart > (~(UINT64)0) - bytes)
            return 0;
        UINT64 end = desc->PhysicalStart + bytes;

        if (count >= BOOTINFO_MAX_RUNTIME_REGIONS)
            return 0;

        boot_info->RuntimeRegions[count].Base = desc->PhysicalStart;
        boot_info->RuntimeRegions[count].Pages = desc->NumberOfPages;
        ++count;

        if (set_variable >= desc->PhysicalStart && set_variable < end)
            set_variable_is_runtime = 1;
    }

    if (!set_variable_is_runtime || count == 0) {
        boot_info->RuntimeRegionCount = 0;
        boot_info->RuntimeSetVariable = 0;
        return 0;
    }

    boot_info->RuntimeRegionCount = count;
    boot_info->RuntimeSetVariable = set_variable;
    return 1;
}

static void FreeLoadedSegments(EFI_BOOT_SERVICES *boot_services,
                               LoadedSegment *segments, UINTN count) {
    while (count > 0) {
        count--;
        boot_services->FreePages(segments[count].base, segments[count].pages);
    }
}

static EFI_STATUS LoadElfSegments(EFI_SYSTEM_TABLE *system_table,
                                  const void *kernel_buffer,
                                  UINTN kernel_size,
                                  UINT64 *entry_out) {
    const Elf64_Ehdr *header = (const Elf64_Ehdr *)kernel_buffer;
    const UINT8 *bytes = (const UINT8 *)kernel_buffer;
    LoadedSegment loaded[ELF_MAX_LOAD_SEGMENTS];
    UINTN loaded_count = 0;
    int entry_is_executable = 0;

    if (!header || kernel_size < sizeof(Elf64_Ehdr)) return EFI_LOAD_ERROR;
    if (header->e_ident[EI_MAG0] != ELFMAG0 ||
        header->e_ident[EI_MAG1] != ELFMAG1 ||
        header->e_ident[EI_MAG2] != ELFMAG2 ||
        header->e_ident[EI_MAG3] != ELFMAG3 ||
        header->e_ident[EI_CLASS] != ELFCLASS64 ||
        header->e_ident[EI_DATA] != ELFDATA2LSB ||
        header->e_ident[EI_VERSION] != EV_CURRENT ||
        header->e_type != ET_EXEC || header->e_machine != EM_X86_64 ||
        header->e_version != EV_CURRENT ||
        header->e_ehsize != sizeof(Elf64_Ehdr) ||
        header->e_phentsize != sizeof(Elf64_Phdr) ||
        header->e_phnum == 0 || header->e_phnum > ELF_MAX_LOAD_SEGMENTS) {
        return EFI_LOAD_ERROR;
    }

    if (header->e_phoff > kernel_size) return EFI_LOAD_ERROR;
    UINT64 ph_bytes = (UINT64)header->e_phnum * sizeof(Elf64_Phdr);
    if (ph_bytes > (UINT64)kernel_size - header->e_phoff)
        return EFI_LOAD_ERROR;

    const Elf64_Phdr *program_headers =
        (const Elf64_Phdr *)(bytes + header->e_phoff);

    for (UINTN i = 0; i < header->e_phnum; i++) {
        const Elf64_Phdr *ph = &program_headers[i];
        if (ph->p_type != ELF_PT_LOAD || ph->p_memsz == 0) continue;

        if (loaded_count >= ELF_MAX_LOAD_SEGMENTS ||
            ph->p_filesz > ph->p_memsz ||
            ph->p_offset > kernel_size ||
            ph->p_filesz > (UINT64)kernel_size - ph->p_offset ||
            ph->p_vaddr > (~(UINT64)0) - ph->p_memsz ||
            ph->p_vaddr == 0 ||
            (ph->p_vaddr & (EFI_PAGE_SIZE - 1u)) != 0 ||
            ph->p_memsz > (~(UINT64)0) - (EFI_PAGE_SIZE - 1u)) {
            FreeLoadedSegments(system_table->BootServices, loaded, loaded_count);
            return EFI_LOAD_ERROR;
        }

        if (ph->p_align > 1) {
            if ((ph->p_align & (ph->p_align - 1u)) != 0 ||
                (ph->p_vaddr & (ph->p_align - 1u)) !=
                (ph->p_offset & (ph->p_align - 1u))) {
                FreeLoadedSegments(system_table->BootServices, loaded, loaded_count);
                return EFI_LOAD_ERROR;
            }
        }

        UINT64 pages64 = (ph->p_memsz + EFI_PAGE_SIZE - 1u) / EFI_PAGE_SIZE;
        if (pages64 == 0 || pages64 > (UINT64)(~(UINTN)0)) {
            FreeLoadedSegments(system_table->BootServices, loaded, loaded_count);
            return EFI_LOAD_ERROR;
        }

        EFI_PHYSICAL_ADDRESS segment_address = ph->p_vaddr;
        UINTN pages = (UINTN)pages64;
        EFI_STATUS status = system_table->BootServices->AllocatePages(
            AllocateAddress, EfiLoaderData, pages, &segment_address);
        if (EFI_ERROR(status)) {
            FreeLoadedSegments(system_table->BootServices, loaded, loaded_count);
            return status;
        }

        loaded[loaded_count].base = segment_address;
        loaded[loaded_count].pages = pages;
        loaded_count++;

        SetMem((void *)(UINTN)segment_address, (UINTN)ph->p_memsz, 0);
        if (ph->p_filesz) {
            CopyMem((void *)(UINTN)segment_address,
                    (const void *)(bytes + ph->p_offset),
                    (UINTN)ph->p_filesz);
        }

        if ((ph->p_flags & ELF_PF_X) &&
            header->e_entry >= ph->p_vaddr &&
            header->e_entry < ph->p_vaddr + ph->p_memsz) {
            entry_is_executable = 1;
        }
    }

    if (loaded_count == 0 || !entry_is_executable) {
        FreeLoadedSegments(system_table->BootServices, loaded, loaded_count);
        return EFI_LOAD_ERROR;
    }

    *entry_out = header->e_entry;
    return EFI_SUCCESS;
}

static EFI_STATUS GetMemoryMapWithPool(EFI_SYSTEM_TABLE *system_table,
                                       EFI_MEMORY_DESCRIPTOR **map_out,
                                       UINTN *map_size_out,
                                       UINTN *map_key_out,
                                       UINTN *descriptor_size_out,
                                       UINT32 *descriptor_version_out) {
    UINTN required_size = 0;
    EFI_STATUS status = system_table->BootServices->GetMemoryMap(
        &required_size, NULL, map_key_out, descriptor_size_out,
        descriptor_version_out);
    if (status != EFI_BUFFER_TOO_SMALL || *descriptor_size_out == 0)
        return status;

    UINTN extra = (*descriptor_size_out) * 8u;
    if (required_size > (~(UINTN)0) - extra) return EFI_OUT_OF_RESOURCES;
    required_size += extra;

    EFI_MEMORY_DESCRIPTOR *map = NULL;
    status = system_table->BootServices->AllocatePool(
        EfiLoaderData, required_size, (void **)&map);
    if (EFI_ERROR(status)) return status;

    UINTN actual_size = required_size;
    status = system_table->BootServices->GetMemoryMap(
        &actual_size, map, map_key_out, descriptor_size_out,
        descriptor_version_out);
    if (EFI_ERROR(status)) {
        system_table->BootServices->FreePool(map);
        return status;
    }

    *map_out = map;
    *map_size_out = actual_size;
    return EFI_SUCCESS;
}

static int FindLargestConventionalRegion(EFI_MEMORY_DESCRIPTOR *map,
                                         UINTN map_size,
                                         UINTN descriptor_size,
                                         MemoryRegion *region) {
    if (!map || !region || descriptor_size < sizeof(EFI_MEMORY_DESCRIPTOR) ||
        descriptor_size == 0) return 0;

    region->Base = 0;
    region->Size = 0;
    for (UINTN offset = 0; offset + descriptor_size <= map_size;
         offset += descriptor_size) {
        EFI_MEMORY_DESCRIPTOR *desc =
            (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)map + offset);
        if (desc->Type != EfiConventionalMemory) continue;
        if (desc->NumberOfPages > (~(UINT64)0) / EFI_PAGE_SIZE) continue;

        UINT64 size = desc->NumberOfPages * EFI_PAGE_SIZE;
        if (size > region->Size) {
            region->Base = desc->PhysicalStart;
            region->Size = size;
        }
    }
    return region->Size >= EFI_PAGE_SIZE;
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    void *kernel_buffer = NULL;
    UINTN kernel_size = 0;
    UINT64 kernel_entry = 0;
    EFI_STATUS status;

    InitializeLib(ImageHandle, SystemTable);
    Print(L"VexOS 2.0.0-dev UEFI bootloader initializing...\n");

    EFI_STATUS previous_log_status =
        ExportPreviousBootLog(ImageHandle, SystemTable);
    if (previous_log_status != EFI_SUCCESS &&
        previous_log_status != EFI_NOT_FOUND &&
        previous_log_status != EFI_UNSUPPORTED) {
        Print(L"[VexOS] Previous boot log export unavailable (%r)\n",
              previous_log_status);
    }

    status = LoadKernelFile(ImageHandle, SystemTable,
                            &kernel_buffer, &kernel_size);
    if (EFI_ERROR(status)) {
        Print(L"VexOS: unable to read kernel.elf (status %r)\n", status);
        return status;
    }

    status = LoadElfSegments(SystemTable, kernel_buffer, kernel_size,
                             &kernel_entry);
    SystemTable->BootServices->FreePool(kernel_buffer);
    if (EFI_ERROR(status)) {
        Print(L"VexOS: invalid or unloadable kernel ELF (status %r)\n", status);
        return status;
    }

    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    status = SystemTable->BootServices->LocateProtocol(
        &gop_guid, NULL, (void **)&gop);
    if (EFI_ERROR(status) || !gop || !gop->Mode || !gop->Mode->Info) {
        Print(L"VexOS: GOP framebuffer unavailable\n");
        return EFI_UNSUPPORTED;
    }

    BootInfo boot_info;
    boot_info.framebuffer = (uint32_t *)(UINTN)gop->Mode->FrameBufferBase;
    boot_info.width = gop->Mode->Info->HorizontalResolution;
    boot_info.height = gop->Mode->Info->VerticalResolution;
    boot_info.pitch = gop->Mode->Info->PixelsPerScanLine;
    boot_info.LargestFreeRegion.Base = 0;
    boot_info.LargestFreeRegion.Size = 0;
    boot_info.RuntimeSetVariable = 0;
    boot_info.RuntimeRegionCount = 0;
    boot_info.RuntimeReserved = 0;

    // ExitBootServices requires a fresh map key. Retry a few times if the
    // firmware reports that the key changed between GetMemoryMap and exit.
    for (UINTN attempt = 0; attempt < 4; attempt++) {
        EFI_MEMORY_DESCRIPTOR *memory_map = NULL;
        UINTN memory_map_size = 0;
        UINTN map_key = 0;
        UINTN descriptor_size = 0;
        UINT32 descriptor_version = 0;

        status = GetMemoryMapWithPool(SystemTable, &memory_map,
                                      &memory_map_size, &map_key,
                                      &descriptor_size, &descriptor_version);
        if (EFI_ERROR(status)) {
            Print(L"VexOS: unable to obtain UEFI memory map (status %r)\n", status);
            return status;
        }

        if (!FindLargestConventionalRegion(memory_map, memory_map_size,
                                           descriptor_size,
                                           &boot_info.LargestFreeRegion)) {
            SystemTable->BootServices->FreePool(memory_map);
            Print(L"VexOS: no usable conventional memory region\n");
            return EFI_OUT_OF_RESOURCES;
        }

        /*
         * Preserve every EFI_MEMORY_RUNTIME range. The kernel replaces the
         * firmware page tables, so these pages must be identity-mapped before
         * it invokes Runtime Services such as SetVariable().
         */
        (void)CaptureRuntimeRegions(SystemTable, memory_map, memory_map_size,
                                    descriptor_size, &boot_info);

        status = SystemTable->BootServices->ExitBootServices(ImageHandle, map_key);
        if (!EFI_ERROR(status)) {
            void (*kernel_main)(BootInfo *) =
                (void (*)(BootInfo *))(UINTN)kernel_entry;
            kernel_main(&boot_info);
            return EFI_SUCCESS;
        }

        SystemTable->BootServices->FreePool(memory_map);
        if (status != EFI_INVALID_PARAMETER) return status;
    }

    Print(L"VexOS: ExitBootServices failed after retries\n");
    return EFI_LOAD_ERROR;
}
