#ifndef VEXOS_ETHERNET_H
#define VEXOS_ETHERNET_H

#include "netdev.h"

#include <stddef.h>
#include <stdint.h>

#define ETHERNET_HEADER_SIZE       14u
#define ETHERNET_MAX_HANDLERS      16u
#define ETHERNET_TYPE_IPV4         0x0800u
#define ETHERNET_TYPE_ARP          0x0806u
#define ETHERNET_TYPE_IPV6         0x86DDu
#define ETHERNET_TYPE_VLAN         0x8100u
#define ETHERNET_TYPE_PROVIDER_VLAN 0x88A8u
#define ETHERNET_TYPE_QINQ         0x9100u

typedef enum {
    ETHERNET_DISPATCH_INVALID = 0,
    ETHERNET_DISPATCHED,
    ETHERNET_DISPATCH_NO_HANDLER,
    ETHERNET_DISPATCH_UNSUPPORTED
} ethernet_dispatch_result_t;

/*
 * The MAC addresses and payload point directly into the NIC's receive buffer.
 * They are borrowed and valid only until the callback returns. A protocol
 * handler must copy any bytes it needs to retain; it must never free the data.
 */
typedef void (*ethernet_protocol_handler_fn)(
    net_device_t *device,
    const uint8_t destination[6],
    const uint8_t source[6],
    const uint8_t *payload,
    size_t payload_length);

typedef struct {
    uint64_t rx_frames;
    uint64_t rx_dispatched;
    uint64_t rx_invalid;
    uint64_t rx_unsupported;
    uint64_t rx_no_handler;
} ethernet_stats_t;

/* Initialize the bounded EtherType registry and clear dispatcher statistics. */
void ethernet_init(void);

/* Register one synchronous handler for an Ethernet II EtherType. */
int ethernet_register_handler(uint16_t ether_type,
                              ethernet_protocol_handler_fn handler);

/* Parse and dispatch one untagged Ethernet II frame. */
ethernet_dispatch_result_t ethernet_dispatch_frame(
    net_device_t *device, const uint8_t *frame, size_t length);

/* Adapter matching netdev_receive_fn; install with netdev_set_receive_handler. */
void ethernet_receive(net_device_t *device, const uint8_t *frame,
                      size_t length);

void ethernet_get_stats(ethernet_stats_t *out_stats);

#endif
