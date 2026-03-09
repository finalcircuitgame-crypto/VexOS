#include "../../include/ata_pio.h"

// Minimal ATA PIO driver for QEMU IDE disks.
// Supports 28-bit LBA, 512-byte sectors.

static inline void outb(uint16_t port, uint8_t val) {
  __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
  uint8_t ret;
  __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}
static inline void outw(uint16_t port, uint16_t val) {
  __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint16_t inw(uint16_t port) {
  uint16_t ret;
  __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}

void serial_print(const char *str);

#define ATA_REG_DATA 0
#define ATA_REG_ERROR 1
#define ATA_REG_FEATURES 1
#define ATA_REG_SECCOUNT0 2
#define ATA_REG_LBA0 3
#define ATA_REG_LBA1 4
#define ATA_REG_LBA2 5
#define ATA_REG_HDDEVSEL 6
#define ATA_REG_COMMAND 7
#define ATA_REG_STATUS 7

#define ATA_REG_ALTSTATUS 0
#define ATA_REG_CONTROL 0

#define ATA_CMD_IDENTIFY 0xEC
#define ATA_CMD_READ_SECTORS 0x20
#define ATA_CMD_WRITE_SECTORS 0x30

#define ATA_SR_BSY 0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DRQ 0x08
#define ATA_SR_ERR 0x01

static ata_pio_device_t g_storage;

static void io_delay_400ns(uint16_t ctrl_base) {
  (void)inb((uint16_t)(ctrl_base + ATA_REG_ALTSTATUS));
  (void)inb((uint16_t)(ctrl_base + ATA_REG_ALTSTATUS));
  (void)inb((uint16_t)(ctrl_base + ATA_REG_ALTSTATUS));
  (void)inb((uint16_t)(ctrl_base + ATA_REG_ALTSTATUS));
}

static int ata_wait_not_bsy(uint16_t io_base, uint16_t ctrl_base,
                            uint32_t spins) {
  for (uint32_t i = 0; i < spins; i++) {
    uint8_t s = inb((uint16_t)(io_base + ATA_REG_STATUS));
    if (!(s & ATA_SR_BSY))
      return 1;
    (void)ctrl_base;
  }
  return 0;
}

static int ata_wait_drq(uint16_t io_base, uint16_t ctrl_base,
                        uint32_t spins) {
  for (uint32_t i = 0; i < spins; i++) {
    uint8_t s = inb((uint16_t)(io_base + ATA_REG_STATUS));
    if (s & ATA_SR_ERR)
      return 0;
    if (!(s & ATA_SR_BSY) && (s & ATA_SR_DRQ))
      return 1;
    (void)ctrl_base;
  }
  return 0;
}

static void ata_select_drive(uint16_t io_base, uint16_t ctrl_base,
                             uint8_t is_slave) {
  uint8_t sel = (uint8_t)(0xE0u | (is_slave ? 0x10u : 0u));
  outb((uint16_t)(io_base + ATA_REG_HDDEVSEL), sel);
  io_delay_400ns(ctrl_base);
}

static int ata_identify(uint16_t io_base, uint16_t ctrl_base, uint8_t is_slave,
                        uint16_t out_ident[256]) {
  ata_select_drive(io_base, ctrl_base, is_slave);

  outb((uint16_t)(io_base + ATA_REG_SECCOUNT0), 0);
  outb((uint16_t)(io_base + ATA_REG_LBA0), 0);
  outb((uint16_t)(io_base + ATA_REG_LBA1), 0);
  outb((uint16_t)(io_base + ATA_REG_LBA2), 0);
  outb((uint16_t)(io_base + ATA_REG_COMMAND), ATA_CMD_IDENTIFY);
  io_delay_400ns(ctrl_base);

  uint8_t status = inb((uint16_t)(io_base + ATA_REG_STATUS));
  if (status == 0 || status == 0xFF)
    return 0;

  if (!ata_wait_not_bsy(io_base, ctrl_base, 1000000))
    return 0;

  uint8_t lba1 = inb((uint16_t)(io_base + ATA_REG_LBA1));
  uint8_t lba2 = inb((uint16_t)(io_base + ATA_REG_LBA2));
  // If not 0, likely ATAPI.
  if (lba1 != 0 || lba2 != 0)
    return 0;

  if (!ata_wait_drq(io_base, ctrl_base, 1000000))
    return 0;

  for (int i = 0; i < 256; i++) {
    out_ident[i] = inw((uint16_t)(io_base + ATA_REG_DATA));
  }
  return 1;
}

int ata_pio_init(void) {
  // Primary channel legacy I/O ports.
  const uint16_t io_base = 0x1F0;
  const uint16_t ctrl_base = 0x3F6;

  uint16_t ident[256];

  // Prefer slave so the master can stay as the EFI boot disk.
  if (ata_identify(io_base, ctrl_base, 1, ident)) {
    g_storage.io_base = io_base;
    g_storage.ctrl_base = ctrl_base;
    g_storage.is_slave = 1;
    g_storage.present = 1;
    serial_print("[ATA] Found storage disk on primary SLAVE\n");
    return 1;
  }

  if (ata_identify(io_base, ctrl_base, 0, ident)) {
    g_storage.io_base = io_base;
    g_storage.ctrl_base = ctrl_base;
    g_storage.is_slave = 0;
    g_storage.present = 1;
    serial_print("[ATA] Found storage disk on primary MASTER\n");
    return 1;
  }

  g_storage.present = 0;
  serial_print("[ATA] No ATA PIO disk found\n");
  return 0;
}

const ata_pio_device_t *ata_pio_get_storage_device(void) {
  return g_storage.present ? &g_storage : (const ata_pio_device_t *)0;
}

int ata_pio_read28(const ata_pio_device_t *dev, uint32_t lba, uint8_t count,
                   void *out_buf) {
  if (!dev || !dev->present || count == 0)
    return 0;
  if (lba & 0xF0000000u)
    return 0;

  uint16_t io = dev->io_base;
  uint16_t ctrl = dev->ctrl_base;

  ata_select_drive(io, ctrl, dev->is_slave);
  outb((uint16_t)(io + ATA_REG_HDDEVSEL),
       (uint8_t)(0xE0u | (dev->is_slave ? 0x10u : 0u) | ((lba >> 24) & 0x0Fu)));
  io_delay_400ns(ctrl);

  outb((uint16_t)(io + ATA_REG_FEATURES), 0);
  outb((uint16_t)(io + ATA_REG_SECCOUNT0), count);
  outb((uint16_t)(io + ATA_REG_LBA0), (uint8_t)(lba & 0xFFu));
  outb((uint16_t)(io + ATA_REG_LBA1), (uint8_t)((lba >> 8) & 0xFFu));
  outb((uint16_t)(io + ATA_REG_LBA2), (uint8_t)((lba >> 16) & 0xFFu));
  outb((uint16_t)(io + ATA_REG_COMMAND), ATA_CMD_READ_SECTORS);

  uint16_t *dst = (uint16_t *)out_buf;
  for (uint32_t s = 0; s < count; s++) {
    if (!ata_wait_drq(io, ctrl, 1000000))
      return 0;
    for (int i = 0; i < 256; i++) {
      *dst++ = inw((uint16_t)(io + ATA_REG_DATA));
    }
    io_delay_400ns(ctrl);
  }
  return 1;
}

int ata_pio_write28(const ata_pio_device_t *dev, uint32_t lba, uint8_t count,
                    const void *in_buf) {
  if (!dev || !dev->present || count == 0)
    return 0;
  if (lba & 0xF0000000u)
    return 0;

  uint16_t io = dev->io_base;
  uint16_t ctrl = dev->ctrl_base;

  ata_select_drive(io, ctrl, dev->is_slave);
  outb((uint16_t)(io + ATA_REG_HDDEVSEL),
       (uint8_t)(0xE0u | (dev->is_slave ? 0x10u : 0u) | ((lba >> 24) & 0x0Fu)));
  io_delay_400ns(ctrl);

  outb((uint16_t)(io + ATA_REG_FEATURES), 0);
  outb((uint16_t)(io + ATA_REG_SECCOUNT0), count);
  outb((uint16_t)(io + ATA_REG_LBA0), (uint8_t)(lba & 0xFFu));
  outb((uint16_t)(io + ATA_REG_LBA1), (uint8_t)((lba >> 8) & 0xFFu));
  outb((uint16_t)(io + ATA_REG_LBA2), (uint8_t)((lba >> 16) & 0xFFu));
  outb((uint16_t)(io + ATA_REG_COMMAND), ATA_CMD_WRITE_SECTORS);

  const uint16_t *src = (const uint16_t *)in_buf;
  for (uint32_t s = 0; s < count; s++) {
    if (!ata_wait_drq(io, ctrl, 1000000))
      return 0;
    for (int i = 0; i < 256; i++) {
      outw((uint16_t)(io + ATA_REG_DATA), *src++);
    }
    io_delay_400ns(ctrl);
  }

  // Flush cache (optional for QEMU); ignore errors.
  outb((uint16_t)(io + ATA_REG_COMMAND), 0xE7);
  (void)ata_wait_not_bsy(io, ctrl, 1000000);

  return 1;
}
