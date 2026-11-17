#ifndef VEXOS_UDP_H
#define VEXOS_UDP_H

#include "ipv4.h"

#include <stddef.h>
#include <stdint.h>

#define UDP_MAX_PORT_HANDLERS 16u
#define UDP_HEADER_SIZE 8u

typedef struct {
    uint64_t rx_datagrams;
    uint64_t rx_delivered;
    uint64_t rx_malformed;
    uint64_t rx_bad_checksum;
    uint64_t rx_no_port;
    uint64_t tx_datagrams;
    uint64_t tx_dropped;
} udp_stats_t;

/* UDP handlers run synchronously and borrow the IPv4 receive buffer. */
typedef void (*udp_receive_fn)(net_device_t *device,
                               const ipv4_packet_info_t *info,
                               uint16_t source_port,
                               uint16_t destination_port,
                               const uint8_t *payload,
                               size_t payload_length);

/* Register UDP protocol 17 with IPv4 and reset the bounded port table. */
int udp_init(void);

/* Bind one non-zero destination port. Duplicate ports are rejected. */
int udp_bind(uint16_t port, udp_receive_fn handler);
int udp_unbind(uint16_t port);

/* Send one UDP datagram using the configured interface address as source. */
int udp_send(net_device_t *device, uint32_t destination,
             uint16_t source_port, uint16_t destination_port,
             const void *payload, size_t payload_length);

void udp_get_stats(udp_stats_t *out_stats);

#endif
