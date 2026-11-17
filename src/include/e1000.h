#ifndef VEXOS_E1000_H
#define VEXOS_E1000_H

#include <stdint.h>

/* First supported controller: QEMU's Intel 82540EM-compatible 8086:100E. */
#define E1000_VENDOR_INTEL 0x8086u
#define E1000_DEVICE_82540EM 0x100Eu

int e1000_init(uint8_t bus, uint8_t slot, uint8_t function,
               uint16_t vendor_id, uint16_t device_id);

#endif
