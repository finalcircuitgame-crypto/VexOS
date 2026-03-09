#ifndef ATA_PIO_H
#define ATA_PIO_H

#include <stdint.h>

typedef struct {
  uint16_t io_base;
  uint16_t ctrl_base;
  uint8_t is_slave;
  uint8_t present;
} ata_pio_device_t;

// Probes for an ATA PIO device on the primary channel. Prefers the slave drive
// (so we can keep the master drive for the EFI boot image).
int ata_pio_init(void);
const ata_pio_device_t *ata_pio_get_storage_device(void);

int ata_pio_read28(const ata_pio_device_t *dev, uint32_t lba, uint8_t count,
                   void *out_buf);
int ata_pio_write28(const ata_pio_device_t *dev, uint32_t lba, uint8_t count,
                    const void *in_buf);

#endif
