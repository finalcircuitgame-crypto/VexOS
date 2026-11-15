#ifndef AHCI_H
#define AHCI_H

#include <stdint.h>

/*
 * Minimal polling AHCI driver.
 *
 * The caller must initialize paging and PCI enumeration before ahci_init().
 * Sector I/O is synchronous and uses an internal DMA bounce buffer.
 */
int ahci_init(uint64_t abar);
int ahci_is_ready(void);
uint64_t ahci_get_sector_count(void);
int ahci_read_sectors(uint64_t lba, uint32_t count, void *buffer);
int ahci_write_sectors(uint64_t lba, uint32_t count, const void *buffer);

#endif
