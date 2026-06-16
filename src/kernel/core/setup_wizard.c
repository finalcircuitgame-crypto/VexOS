#include <stdint.h>
#include <stddef.h>
#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include "serial.h"

// Forward declarations (reused helpers from xhci)
extern void xhci_print_u32_dec(uint32_t v);
extern void xhci_print_hex32(uint32_t v);
extern void PrintString(const char *str, uint32_t color);

// Simple UI helpers
static void wizard_print_fb(const char *msg, uint32_t color) {
    serial_print("[WIZARD] ");
    serial_print(msg);
    serial_print("\n");
    PrintString("[WIZARD] ", color);
    PrintString(msg, color);
    PrintString("\n", color);
}

static void wizard_print_hex(uint32_t v) {
    serial_print("[WIZARD] 0x");
    xhci_print_hex32(v);
    serial_print("\n");
}

static void wizard_pause(void) {
    wizard_print_fb("Press any key to continue...", 0xFFFFFF);
    // Simple spin-wait; in a real kernel you'd wait for a keyboard interrupt
    for (volatile uint32_t i = 0; i < 50000000; ++i) { }
}

// Fake disk driver (RAM disk)
#define FAKE_DISK_SECTORS 2048       // 1 MiB (512-byte sectors)
#define SECTOR_SIZE 512

static uint8_t *fake_disk_storage = NULL;
static uint32_t fake_disk_sectors = FAKE_DISK_SECTORS;

static int fake_disk_init(void) {
    if (fake_disk_storage) {
        wizard_print_fb("Fake disk already initialized.", 0x00FF00);
        return 1;
    }

    // Allocate contiguous pages for the fake disk
    size_t bytes = fake_disk_sectors * SECTOR_SIZE;
    size_t pages = (bytes + 4095) / 4096;

    fake_disk_storage = (uint8_t *)PMM_AllocatePage();
    if (!fake_disk_storage) {
        wizard_print_fb("Failed to allocate first page for fake disk.", 0xFF0000);
        return 0;
    }

    // Map additional pages if needed (for simplicity, just 1 page for now)
    // In a real implementation you'd allocate and map pages in a loop

    // Zero out the disk
    for (size_t i = 0; i < 4096; ++i) {
        fake_disk_storage[i] = 0;
    }

    wizard_print_fb("Fake disk initialized (RAM disk, 1 MiB).", 0x00FFFF);
    return 1;
}

static int fake_disk_read_sector(uint32_t lba, void *buf) {
    if (!fake_disk_storage || lba >= fake_disk_sectors)
        return 0;
    uint8_t *src = fake_disk_storage + lba * SECTOR_SIZE;
    uint8_t *dst = (uint8_t *)buf;
    for (uint32_t i = 0; i < SECTOR_SIZE; ++i) {
        dst[i] = src[i];
    }
    return 1;
}

static int fake_disk_write_sector(uint32_t lba, const void *buf) {
    if (!fake_disk_storage || lba >= fake_disk_sectors)
        return 0;
    const uint8_t *src = (const uint8_t *)buf;
    uint8_t *dst = fake_disk_storage + lba * SECTOR_SIZE;
    for (uint32_t i = 0; i < SECTOR_SIZE; ++i) {
        dst[i] = src[i];
    }
    return 1;
}

// Simple MBR partitioning (single partition)
#pragma pack(push, 1)
typedef struct {
    uint8_t boot_flag;
    uint8_t start_chs[3];
    uint8_t partition_type;
    uint8_t end_chs[3];
    uint32_t start_lba;
    uint32_t total_sectors;
} mbr_partition_t;

typedef struct {
    uint8_t bootstrap[446];
    mbr_partition_t partitions[4];
    uint8_t signature[2]; // 0x55 0xAA
} mbr_t;
#pragma pack(pop)

static int write_mbr(uint32_t partition_start_lba, uint32_t partition_sectors) {
    mbr_t mbr = {0};

    // Single partition (type 0x0B = FAT32 CHS/LBA, but we'll use simple FAT12/16)
    mbr.partitions[0].boot_flag = 0x00; // non-bootable
    mbr.partitions[0].partition_type = 0x0E; // FAT16 LBA
    mbr.partitions[0].start_lba = partition_start_lba;
    mbr.partitions[0].total_sectors = partition_sectors;

    // MBR signature
    mbr.signature[0] = 0x55;
    mbr.signature[1] = 0xAA;

    // Write MBR to LBA 0
    return fake_disk_write_sector(0, &mbr);
}

// Simple FAT12/16 formatting (minimal)
#pragma pack(push, 1)
typedef struct {
    uint8_t jmp_boot[3];
    uint8_t oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t num_fats;
    uint16_t root_entries;
    uint16_t total_sectors_16;
    uint8_t media_type;
    uint16_t fat_size_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint8_t drive_number;
    uint8_t reserved1;
    uint8_t boot_signature;
    uint32_t volume_id;
    uint8_t volume_label[11];
    uint8_t fs_type[8];
} fat_bpb_t;
#pragma pack(pop)

static int format_fat16(uint32_t partition_lba, uint32_t partition_sectors) {
    fat_bpb_t bpb = {0};

    // Simple FAT16 BPB
    bpb.bytes_per_sector = SECTOR_SIZE;
    bpb.sectors_per_cluster = 4;
    bpb.reserved_sectors = 1;
    bpb.num_fats = 2;
    bpb.root_entries = 512;
    bpb.total_sectors_16 = (uint16_t)partition_sectors;
    bpb.media_type = 0xF8;
    bpb.fat_size_16 = (partition_sectors - bpb.reserved_sectors - (bpb.root_entries * 32 + SECTOR_SIZE - 1) / SECTOR_SIZE) / (bpb.num_fats + 1);
    bpb.sectors_per_track = 63;
    bpb.num_heads = 255;
    bpb.drive_number = 0x80;
    bpb.boot_signature = 0x29;
    bpb.volume_id = 0x12345678;
    const char *label = "FAKEDISK   ";
    const char *fs = "FAT16   ";
    for (int i = 0; i < 11; ++i) bpb.volume_label[i] = label[i];
    for (int i = 0; i < 8; ++i) bpb.fs_type[i] = fs[i];

    // Write BPB to first sector of partition
    if (!fake_disk_write_sector(partition_lba, &bpb)) {
        wizard_print_fb("Failed to write FAT BPB.", 0xFF0000);
        return 0;
    }

    // Initialize FATs (simplified: just zero them out)
    uint8_t fat_sector[SECTOR_SIZE];
    for (uint32_t i = 0; i < SECTOR_SIZE; ++i) fat_sector[i] = 0;
    // First FAT entry: media type + 0xFFF8 (end-of-chain)
    fat_sector[0] = bpb.media_type;
    fat_sector[1] = 0xFF;
    fat_sector[2] = 0xFF;

    uint32_t fat_start = partition_lba + bpb.reserved_sectors;
    for (uint32_t i = 0; i < bpb.num_fats; ++i) {
        if (!fake_disk_write_sector(fat_start + i, fat_sector)) {
            wizard_print_fb("Failed to write FAT.", 0xFF0000);
            return 0;
        }
    }

    wizard_print_fb("FAT16 formatted partition.", 0x00FF00);
    return 1;
}

// Wizard steps
static void wizard_step_detect(void) {
    wizard_print_fb("=== Step 1: Detect Fake Disk ===", 0x00FFFF);
    if (fake_disk_storage) {
        wizard_print_fb("Fake disk already present.", 0x00FF00);
    } else {
        wizard_print_fb("Initializing fake RAM disk...", 0xFFFFFF);
        if (!fake_disk_init()) {
            wizard_print_fb("ERROR: Failed to initialize fake disk.", 0xFF0000);
            return;
        }
    }
    wizard_pause();
}

static void wizard_step_partition(void) {
    wizard_print_fb("=== Step 2: Partition Disk ===", 0x00FFFF);
    // Simple: one partition starting at LBA 1, rest of disk
    uint32_t partition_start = 1;
    uint32_t partition_sectors = fake_disk_sectors - partition_start;
    wizard_print_fb("Writing MBR with single partition...", 0xFFFFFF);
    if (!write_mbr(partition_start, partition_sectors)) {
        wizard_print_fb("ERROR: Failed to write MBR.", 0xFF0000);
        return;
    }
    wizard_print_fb("Partition created.", 0x00FF00);
    wizard_pause();
}

static void wizard_step_format(void) {
    wizard_print_fb("=== Step 3: Format Partition (FAT16) ===", 0x00FFFF);
    uint32_t partition_lba = 1;
    uint32_t partition_sectors = fake_disk_sectors - 1;
    if (!format_fat16(partition_lba, partition_sectors)) {
        wizard_print_fb("ERROR: Failed to format partition.", 0xFF0000);
        return;
    }
    wizard_print_fb("Partition formatted.", 0x00FF00);
    wizard_pause();
}

static void wizard_step_verify(void) {
    wizard_print_fb("=== Step 4: Verify ===", 0x00FFFF);
    // Read back MBR and BPB and print a few fields
    mbr_t mbr;
    if (!fake_disk_read_sector(0, &mbr)) {
        wizard_print_fb("ERROR: Failed to read MBR.", 0xFF0000);
        return;
    }
    wizard_print_fb("MBR signature: ", 0xFFFFFF);
    wizard_print_hex((mbr.signature[0] << 8) | mbr.signature[1]);

    fat_bpb_t bpb;
    if (!fake_disk_read_sector(1, &bpb)) {
        wizard_print_fb("ERROR: Failed to read BPB.", 0xFF0000);
        return;
    }
    wizard_print_fb("FAT BPB: bytes_per_sector=", 0xFFFFFF);
    xhci_print_u32_dec(bpb.bytes_per_sector);
    wizard_print_fb(" sectors_per_cluster=", 0xFFFFFF);
    xhci_print_u32_dec(bpb.sectors_per_cluster);
    wizard_print_fb(" total_sectors=", 0xFFFFFF);
    xhci_print_u32_dec(bpb.total_sectors_16);
    wizard_print_fb(" fs_type=", 0xFFFFFF);
    for (int i = 0; i < 8; ++i) {
        char c[2] = {bpb.fs_type[i], 0};
        serial_print(c);
    }
    serial_print("\n");
    wizard_pause();
}

// Main entry point
void setup_wizard_run(void) {
    wizard_print_fb("=== VexOS Setup Wizard ===", 0x00FFFF);
    wizard_print_fb("This wizard will create and partition a fake disk (RAM disk).", 0xFFFFFF);
    wizard_pause();

    wizard_step_detect();
    wizard_step_partition();
    wizard_step_format();
    wizard_step_verify();

    wizard_print_fb("=== Setup Wizard Complete ===", 0x00FFFF);
    wizard_print_fb("Fake disk is now partitioned and formatted (FAT16).", 0x00FF00);
    wizard_print_fb("You can now mount and use it as a storage device.", 0xFFFFFF);
    wizard_pause();
}
