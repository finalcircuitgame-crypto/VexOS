#include <efi.h>
#include <efilib.h>
#include <elf.h>
#include "bootinfo.h"

EFI_FILE *LoadFile(EFI_FILE *Directory, CHAR16 *Path, EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_FILE *LoadedFile;
    EFI_LOADED_IMAGE_PROTOCOL *LoadedImage;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FileSystem;

    SystemTable->BootServices->HandleProtocol(ImageHandle, &gEfiLoadedImageProtocolGuid, (void**)&LoadedImage);
    SystemTable->BootServices->HandleProtocol(LoadedImage->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (void**)&FileSystem);

    if (Directory == NULL) {
        FileSystem->OpenVolume(FileSystem, &Directory);
    }

    EFI_STATUS Status = Directory->Open(Directory, &LoadedFile, Path, EFI_FILE_MODE_READ, EFI_FILE_READ_ONLY);
    if (EFI_ERROR(Status)) {
        return NULL;
    }
    return LoadedFile;
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    InitializeLib(ImageHandle, SystemTable);
    Print(L"Tiny64 Bootloader Initializing...\n");

    EFI_FILE *KernelFile = LoadFile(NULL, L"kernel.elf", ImageHandle, SystemTable);
    if (KernelFile == NULL) {
        Print(L"Error: Could not load kernel.elf\n");
        return EFI_NOT_FOUND;
    }

    // Get File Size
    EFI_FILE_INFO *FileInfo;
    UINTN FileInfoSize = 0;
    KernelFile->GetInfo(KernelFile, &gEfiFileInfoGuid, &FileInfoSize, NULL);
    SystemTable->BootServices->AllocatePool(EfiLoaderData, FileInfoSize, (void**)&FileInfo);
    KernelFile->GetInfo(KernelFile, &gEfiFileInfoGuid, &FileInfoSize, FileInfo);
    
    UINTN KernelSize = FileInfo->FileSize;
    void *KernelBuffer;
    SystemTable->BootServices->AllocatePool(EfiLoaderData, KernelSize, &KernelBuffer);
    KernelFile->Read(KernelFile, &KernelSize, KernelBuffer);
    KernelFile->Close(KernelFile);

    Elf64_Ehdr *Header = (Elf64_Ehdr*)KernelBuffer;
    if (Header->e_ident[0] != 0x7f || Header->e_ident[1] != 'E' || Header->e_ident[2] != 'L' || Header->e_ident[3] != 'F') {
        Print(L"Error: Invalid ELF Magic\n");
        return EFI_LOAD_ERROR;
    }

    Print(L"Kernel ELF Loaded. Parsing Segments...\n");

    Elf64_Phdr *Phdrs = (Elf64_Phdr*)((char*)KernelBuffer + Header->e_phoff);
    for (int i = 0; i < Header->e_phnum; i++) {
        Elf64_Phdr *Phdr = &Phdrs[i];
        if (Phdr->p_type == PT_LOAD) {
            int pages = (Phdr->p_memsz + 0x1000 - 1) / 0x1000;
            EFI_PHYSICAL_ADDRESS SegmentAddr = Phdr->p_vaddr;
            SystemTable->BootServices->AllocatePages(AllocateAddress, EfiLoaderData, pages, &SegmentAddr);
            
            // Zero out memory first (BSS)
            SetMem((void*)SegmentAddr, Phdr->p_memsz, 0);
            // Copy data
            CopyMem((void*)SegmentAddr, (char*)KernelBuffer + Phdr->p_offset, Phdr->p_filesz);
        }
    }

    Print(L"Kernel Loaded. Setting up Graphics...\n");

    EFI_GUID GopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;
    EFI_STATUS Status = SystemTable->BootServices->LocateProtocol(&GopGuid, NULL, (void**)&Gop);
    if (EFI_ERROR(Status)) {
        Print(L"Error: GOP not found\n");
        return Status;
    }

    BootInfo bootInfo;
    bootInfo.framebuffer = (uint32_t*)Gop->Mode->FrameBufferBase;
    bootInfo.width = Gop->Mode->Info->HorizontalResolution;
    bootInfo.height = Gop->Mode->Info->VerticalResolution;
    bootInfo.pitch = Gop->Mode->Info->PixelsPerScanLine;

    // Get Memory Map to find a free region for PMM
    UINTN MemoryMapSize = 0;
    EFI_MEMORY_DESCRIPTOR *MemoryMap = NULL;
    UINTN MapKey, DescriptorSize;
    UINT32 DescriptorVersion;

    SystemTable->BootServices->GetMemoryMap(&MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);
    MemoryMapSize += 2 * DescriptorSize;
    SystemTable->BootServices->AllocatePool(EfiLoaderData, MemoryMapSize, (void**)&MemoryMap);
    SystemTable->BootServices->GetMemoryMap(&MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);

    // Find the largest conventional memory region
    bootInfo.LargestFreeRegion.Base = 0;
    bootInfo.LargestFreeRegion.Size = 0;

    for (UINTN i = 0; i < MemoryMapSize / DescriptorSize; i++) {
        EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR*)((char*)MemoryMap + (i * DescriptorSize));
        if (desc->Type == EfiConventionalMemory) {
            if (desc->NumberOfPages * 4096 > bootInfo.LargestFreeRegion.Size) {
                bootInfo.LargestFreeRegion.Base = desc->PhysicalStart;
                bootInfo.LargestFreeRegion.Size = desc->NumberOfPages * 4096;
            }
        }
    }

    Print(L"FrameBuffer: 0x%lx (%dx%d)\n", bootInfo.framebuffer, bootInfo.width, bootInfo.height);

    SystemTable->BootServices->ExitBootServices(ImageHandle, MapKey);

    // Jump to Kernel
    void (*KernelEntry)(BootInfo*) = (void (*)(BootInfo*))Header->e_entry;
    KernelEntry(&bootInfo);

    return EFI_SUCCESS;
}