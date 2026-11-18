#ifndef VEXOS_ALX_H
#define VEXOS_ALX_H

#include <stddef.h>
#include <stdint.h>

/* Qualcomm Atheros AR8171/AR8175 (ALX) PCIe Ethernet controller. */
int alx_init(uint8_t bus, uint8_t slot, uint8_t function);
int alx_is_ready(void);
int alx_link_up(void);
const uint8_t *alx_mac_address(void);
int alx_send(const void *frame, uint16_t length);
int alx_receive(void *frame, uint16_t capacity, uint16_t *length);
void alx_poll(void);

#endif
