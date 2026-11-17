#ifndef VEXOS_E1000_H
#define VEXOS_E1000_H

#include <stdint.h>

/* QEMU's Intel 82540EM-compatible E1000 controller. */
#define E1000_VENDOR_INTEL    0x8086u
#define E1000_DEVICE_82540EM  0x100Eu

/* The caller enables PCI memory decoding and bus mastering before this call. */
int e1000_init(uint64_t mmio_base);

#endif
