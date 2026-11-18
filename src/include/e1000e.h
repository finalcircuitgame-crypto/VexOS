#ifndef E1000E_H
#define E1000E_H

#include <stddef.h>
#include <stdint.h>

/* Intel 82574L (QEMU e1000e) polled Ethernet interface. */
int e1000e_init(uint8_t bus, uint8_t slot, uint8_t function);
int e1000e_is_ready(void);
int e1000e_link_up(void);
const uint8_t *e1000e_mac_address(void);
int e1000e_send(const void *frame, uint16_t length);
int e1000e_receive(void *frame, uint16_t capacity, uint16_t *length);

/* CI-only smoke test: transmit a valid broadcast ARP request. */
int e1000e_ci_transmit_test(void);

#endif
